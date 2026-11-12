#ifdef TARGET_GX

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <ultra64.h>
#include "macros.h" // for UNUSED

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_gx_wm.h"
#include "gfx_screen_config.h"
#include "gfx_gx_tnl.h"

#include "../configfile.h"
#include "../bench.h"

#define TEXTURE_POOL_SIZE 4096
#ifdef __wii__
// Large enough to hold a whole frame while the GPU is held back for split timing
#define DEFAULT_FIFO_SIZE (512 * 1024)
#else
#define DEFAULT_FIFO_SIZE (256 * 1024)
#endif

// shaders
struct ShaderProgram {
    uint32_t shader_id;
    uint32_t program_id;
    uint8_t num_floats;
    struct CCFeatures cc_features;
};

static struct ShaderProgram shader_program_pool[32];
static uint8_t shader_program_pool_size;
static uint8_t current_shader;

// textures
static uint32_t texture_index;
static uint32_t texture_units[2];
static uint32_t current_texture;

static GXTexObj texture_pool[TEXTURE_POOL_SIZE];

struct TextureStorage {
    void *data;
};

static struct TextureStorage texture_storage[TEXTURE_POOL_SIZE];
static void *deferred_texture_frees[TEXTURE_POOL_SIZE];
static size_t deferred_texture_free_count;

static unsigned char gp_fifo[DEFAULT_FIFO_SIZE] __attribute__((aligned(32)));

#define GX_NEAR_PLANE 16.0f
#define GX_FAR_PLANE 24000.0f
#define GX_FAR_PLANE_EXTENDED 60000.0f // Far plane used when fog is turned off
#define GX_DECAL_BIAS 0.0001f
// A depth offset b covers about b * w^2 / near of distance; when the depth planes follow the N64
// projection (opt_gpu_tnl) the bias is scaled with the near plane, keeping the same tolerance
static float decal_bias = GX_DECAL_BIAS;
#define GX_TEXTURE_EDGE_ALPHA_THRESHOLD 76

// Tiled rendering (1280x240p and 1280x720p)
// The EFB is at most 640x528, so a 1280 pixel wide frame is rendered as a grid of tiles: 2x1
// tiles of 640x240 in 1280x240p, 2x2 tiles of 640x360 in 720p, each copied into its place in the
// XFB. The frame's
// GX commands are recorded into display list segments once, then replayed for each tile. Viewport
// and scissor are kept out of the recorded commands (each segment remembers them) so they can be
// adjusted per tile. The projection matrix is kept out too: for each tile the hardware viewport
// is exactly the EFB tile, and the projection is rescaled so that tile's slice of the frame
// fills it (horizontally, and in 720p vertically too). Geometry outside the tile is then clipped by the transform unit, so no screen
// coordinate ever lands far outside the EFB (the rasterizer mangles triangles that do).
// Frames record well under 100 KB on hardware; keep headroom without costing GameCube memory
#ifdef __wii__
#define HIRES_DL_SIZE (1024 * 1024)
#define HIRES_MAX_SEGS 8192 // room for per-draw segments (opt_tile_cull)
#else
#define HIRES_DL_SIZE (512 * 1024)
#define HIRES_MAX_SEGS 2048
#endif

struct HiresSeg {
    uint32_t offset;
    uint32_t size;
    int vp[4];
    f32 far_z;
    int sc[4];
    uint8_t proj;   // PROJ_* (and GPU-path projections from PROJ_HW on)
    bool cullable;  // geometry only (one draw); skipped on tiles its bounds miss
    int16_t bb[4];  // its bounds in frame pixels: x0, y0, x1, y1 (exclusive)
};

// Projection the current draws use: the CPU path's perspective or orthographic (2D) pass-through,
// or one of this frame's GPU-path projections (opt_gpu_tnl)
enum { PROJ_PERSP, PROJ_ORTHO, PROJ_SKY, PROJ_HW };
#define HW_PROJ_MAX 16
static uint8_t cur_proj;
static Mtx44 hw_proj[HW_PROJ_MAX];
static int hw_nproj;
static uint8_t sw_persp_proj = PROJ_PERSP; // CPU-path perspective in use (a table entry when depth is the N64's)
static int hw_proj_add(const Mtx44 m);

static bool hr_enabled;
static uint8_t *hr_crop_scratch; // crop modes: destination of every tile but the top left one
static int hr_tiles_x = 2, hr_tiles_y = 1;      // tile grid
static int hr_tile_w = HIRES_TILE_WIDTH, hr_tile_h = 240;
static uint8_t *hr_dl;
static struct HiresSeg hr_segs[HIRES_MAX_SEGS];
static int hr_nseg;
static uint32_t hr_pos;
static bool hr_recording;
static bool hr_seg_drawn;
static bool hr_overflow;

static int sc_x, sc_y, sc_w, sc_h; // current scissor, in GX_SetScissor terms

static Mtx44 gx_perspective_mtx; // 3D feeds -w as z, GX divides by real w
static Mtx44 gx_ortho_mtx;       // 2D/HUD: pass-through of already-NDC coords
static Mtx44 gx_sky_mtx;         // held-back skybox (opt_sky_last): pass-through x/y, depth just short of the far plane
static bool sky_on;

static bool gfx_gx_z_is_from_0_to_1(void)
{
    return true;
}

static void gfx_gx_vertex_array_set_attribs(UNUSED struct ShaderProgram *prg)
{
}

static bool shader_item_is_input(uint8_t item)
{
    return item >= SHADER_INPUT_1 && item <= SHADER_INPUT_4;
}

static bool shader_item_is_texture(uint8_t item)
{
    return item == SHADER_TEXEL0 || item == SHADER_TEXEL0A || item == SHADER_TEXEL1;
}

static int shader_item_input_index(uint8_t item)
{
    return shader_item_is_input(item) ? item - SHADER_INPUT_1 : -1;
}

static int formula_first_input(const uint8_t c[4])
{
    for (int i = 0; i < 4; i++)
    {
        if (shader_item_is_input(c[i]))
            return shader_item_input_index(c[i]);
    }
    return -1;
}

static int formula_num_inputs(const uint8_t c[4])
{
    bool used[4] = { false };
    int count = 0;

    for (int i = 0; i < 4; i++)
    {
        int input = shader_item_input_index(c[i]);
        if (input >= 0 && !used[input])
        {
            used[input] = true;
            count++;
        }
    }
    return count;
}

static uint8_t formula_first_texture(const uint8_t c[4])
{
    for (int i = 0; i < 4; i++)
    {
        if (shader_item_is_texture(c[i]))
            return c[i];
    }
    return SHADER_0;
}

static uint8_t gx_color_channel_for_input(int input)
{
    switch (input)
    {
        case 0:
            return GX_COLOR0A0;
        case 1:
            return GX_COLOR1A1;
        default:
            return GX_COLORNULL;
    }
}

static uint32_t gx_texmap_for_item(uint8_t item)
{
    return item == SHADER_TEXEL1 ? GX_TEXMAP1 : GX_TEXMAP0;
}

static uint8_t gx_color_source_for_item(uint8_t item, int ras_input, int prev_input)
{
    int input = shader_item_input_index(item);
    if (input >= 0)
    {
        if (input == prev_input)
            return GX_CC_CPREV;
        if (input == ras_input)
            return GX_CC_RASC;
        return GX_CC_ZERO;
    }

    switch (item)
    {
        case SHADER_TEXEL0:
        case SHADER_TEXEL1:
            return GX_CC_TEXC;
        case SHADER_TEXEL0A:
            return GX_CC_TEXA;
        default:
            return GX_CC_ZERO;
    }
}

static uint8_t gx_alpha_source_for_item(uint8_t item, int ras_input, int prev_input)
{
    int input = shader_item_input_index(item);
    if (input >= 0)
    {
        if (input == prev_input)
            return GX_CA_APREV;
        if (input == ras_input)
            return GX_CA_RASA;
        return GX_CA_ZERO;
    }

    switch (item)
    {
        case SHADER_TEXEL0:
        case SHADER_TEXEL0A:
        case SHADER_TEXEL1:
            return GX_CA_TEXA;
        default:
            return GX_CA_ZERO;
    }
}

static void gx_set_tev_order(uint8_t stage, uint8_t texture_item, int input)
{
    uint8_t texcoord = shader_item_is_texture(texture_item) ? GX_TEXCOORD0 : GX_TEXCOORDNULL;
    uint32_t texmap = shader_item_is_texture(texture_item) ? gx_texmap_for_item(texture_item) : GX_TEXMAP_NULL;

    GX_SetTevOrder(stage, texcoord, texmap, gx_color_channel_for_input(input));
}

static void gx_set_tev_op(uint8_t stage)
{
    GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

static void gx_set_color_formula(uint8_t stage, const uint8_t c[4],
                                 bool do_single, bool do_multiply, bool do_mix,
                                 int ras_input, int prev_input)
{
    if (do_single)
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO,
                         gx_color_source_for_item(c[3], ras_input, prev_input));
    }
    else if (do_multiply)
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, gx_color_source_for_item(c[0], ras_input, prev_input),
                         gx_color_source_for_item(c[2], ras_input, prev_input), GX_CC_ZERO);
    }
    else if (do_mix)
    {
        GX_SetTevColorIn(stage, gx_color_source_for_item(c[1], ras_input, prev_input),
                         gx_color_source_for_item(c[0], ras_input, prev_input),
                         gx_color_source_for_item(c[2], ras_input, prev_input), GX_CC_ZERO);
    }
    else
    {
        GX_SetTevColorIn(stage, GX_CC_ZERO, gx_color_source_for_item(c[0], ras_input, prev_input),
                         gx_color_source_for_item(c[2], ras_input, prev_input),
                         gx_color_source_for_item(c[3], ras_input, prev_input));
    }
    gx_set_tev_op(stage);
}

static void gx_set_alpha_formula(uint8_t stage, const uint8_t c[4],
                                 bool do_single, bool do_multiply, bool do_mix,
                                 int ras_input, int prev_input)
{
    if (do_single)
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO,
                         gx_alpha_source_for_item(c[3], ras_input, prev_input));
    }
    else if (do_multiply)
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, gx_alpha_source_for_item(c[0], ras_input, prev_input),
                         gx_alpha_source_for_item(c[2], ras_input, prev_input), GX_CA_ZERO);
    }
    else if (do_mix)
    {
        GX_SetTevAlphaIn(stage, gx_alpha_source_for_item(c[1], ras_input, prev_input),
                         gx_alpha_source_for_item(c[0], ras_input, prev_input),
                         gx_alpha_source_for_item(c[2], ras_input, prev_input), GX_CA_ZERO);
    }
    else
    {
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, gx_alpha_source_for_item(c[0], ras_input, prev_input),
                         gx_alpha_source_for_item(c[2], ras_input, prev_input),
                         gx_alpha_source_for_item(c[3], ras_input, prev_input));
    }
    gx_set_tev_op(stage);
}

// http://amnoid.de/gc/tev.html
// Sets up the colour-combiner TEV stages and returns how many stages it used.
static int update_tev_combiner(struct ShaderProgram *prg)
{
    const uint8_t *color = prg->cc_features.c[0];
    bool has_tex = prg->cc_features.used_textures[0] || prg->cc_features.used_textures[1];
    int num_chans = prg->cc_features.num_inputs > 2 ? 2 : prg->cc_features.num_inputs;

    GX_SetNumChans(num_chans);
    for (int i = 0; i < num_chans; i++)
    {
        GX_SetChanCtrl(GX_COLOR0A0 + i, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    }

    GX_SetNumTexGens(has_tex ? 1 : 0);
    if (has_tex && !hr_enabled)
    {
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    }

    if (formula_num_inputs(color) == 2 && prg->cc_features.do_mix[0]
        && shader_item_is_input(color[0]) && shader_item_is_input(color[1])
        && !shader_item_is_input(color[2]))
    {
        const uint8_t *alpha = prg->cc_features.color_alpha_same ? color : prg->cc_features.c[1];
        int prev_input = shader_item_input_index(color[0]);
        int ras_input = shader_item_input_index(color[1]);

        GX_SetNumTevStages(2);
        gx_set_tev_order(GX_TEVSTAGE0, SHADER_0, prev_input);
        gx_set_color_formula(GX_TEVSTAGE0, (uint8_t[4]){ SHADER_0, SHADER_0, SHADER_0, color[0] },
                             true, false, false, prev_input, -1);
        gx_set_alpha_formula(GX_TEVSTAGE0, (uint8_t[4]){ SHADER_0, SHADER_0, SHADER_0, color[0] },
                             true, false, false, prev_input, -1);

        gx_set_tev_order(GX_TEVSTAGE1, color[2], ras_input);
        gx_set_color_formula(GX_TEVSTAGE1, color, false, false, true, ras_input, prev_input);
        gx_set_alpha_formula(GX_TEVSTAGE1, alpha,
                             prg->cc_features.do_single[1],
                             prg->cc_features.do_multiply[1],
                             prg->cc_features.do_mix[1],
                             ras_input, prev_input);
        return 2;
    }

    if (formula_num_inputs(color) == 2 && prg->cc_features.do_multiply[0]
        && shader_item_is_input(color[0]) && shader_item_is_input(color[2]))
    {
        const uint8_t *alpha = prg->cc_features.color_alpha_same ? color : prg->cc_features.c[1];
        int prev_input = shader_item_input_index(color[0]);
        int ras_input = shader_item_input_index(color[2]);

        GX_SetNumTevStages(2);
        gx_set_tev_order(GX_TEVSTAGE0, SHADER_0, prev_input);
        gx_set_color_formula(GX_TEVSTAGE0, (uint8_t[4]){ SHADER_0, SHADER_0, SHADER_0, color[0] },
                             true, false, false, prev_input, -1);
        gx_set_alpha_formula(GX_TEVSTAGE0, (uint8_t[4]){ SHADER_0, SHADER_0, SHADER_0, color[0] },
                             true, false, false, prev_input, -1);

        gx_set_tev_order(GX_TEVSTAGE1, SHADER_0, ras_input);
        gx_set_color_formula(GX_TEVSTAGE1, color, false, true, false, ras_input, prev_input);
        gx_set_alpha_formula(GX_TEVSTAGE1, alpha,
                             prg->cc_features.do_single[1],
                             prg->cc_features.do_multiply[1],
                             prg->cc_features.do_mix[1],
                             ras_input, prev_input);
        return 2;
    }

    int ras_input = formula_first_input(color);
    uint8_t texture_item = formula_first_texture(color);

    if (ras_input < 0 && prg->cc_features.opt_alpha)
        ras_input = formula_first_input(prg->cc_features.c[1]);
    if (texture_item == SHADER_0 && prg->cc_features.opt_alpha)
        texture_item = formula_first_texture(prg->cc_features.c[1]);

    GX_SetNumTevStages(1);
    gx_set_tev_order(GX_TEVSTAGE0, texture_item, ras_input);
    gx_set_color_formula(GX_TEVSTAGE0, color,
                         prg->cc_features.do_single[0],
                         prg->cc_features.do_multiply[0],
                         prg->cc_features.do_mix[0],
                         ras_input, -1);
    gx_set_alpha_formula(GX_TEVSTAGE0, prg->cc_features.c[1],
                         prg->cc_features.do_single[1],
                         prg->cc_features.do_multiply[1],
                         prg->cc_features.do_mix[1],
                         ras_input, -1);
    return 1;
}

// Apply distance fog if the stage uses it
static void update_tev(struct ShaderProgram *prg)
{
    int num_chans = prg->cc_features.num_inputs > 2 ? 2 : prg->cc_features.num_inputs;
    bool fog = prg->cc_features.opt_fog && num_chans < 2;

    int stages = update_tev_combiner(prg);

    if (fog)
    {
        uint8_t fog_chan = num_chans; // next free colour channel

        GX_SetNumChans(num_chans + 1);
        GX_SetChanCtrl(GX_COLOR0A0 + fog_chan, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX,
                       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);

        uint8_t stage = GX_TEVSTAGE0 + stages;
        GX_SetNumTevStages(stages + 1);
        GX_SetTevOrder(stage, GX_TEXCOORDNULL, GX_TEXMAP_NULL,
                       gx_color_channel_for_input(fog_chan));
        // colour = lerp(prev, fog colour, fog factor); alpha passes through
        GX_SetTevColorIn(stage, GX_CC_CPREV, GX_CC_RASC, GX_CC_RASA, GX_CC_ZERO);
        GX_SetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
}

// Fixed vertex format used in 1280x240p mode, so the recorded display lists never need to
// change CP vertex descriptor registers: position, two colours and one texture coordinate.
static void gx_set_fixed_vtx_desc(void)
{
    GX_InvVtxCache();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    // Matrix index registers live in CP too; the texgen never changes, so set it once
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_Flush(); // push the CP register writes out now, outside any display list
}

static void update_vtx_desc(struct ShaderProgram *prg)
{
    bool has_tex = prg->cc_features.used_textures[0] || prg->cc_features.used_textures[1];

    if (hr_enabled)
        return;

    // clear description
    GX_InvVtxCache();
    GX_ClearVtxDesc();
    // we always have a position xyz
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    // rgba colours
    int num_chans = prg->cc_features.num_inputs > 2 ? 2 : prg->cc_features.num_inputs;
    for (int i = 0; i < num_chans; i++)
    {
        GX_SetVtxDesc(GX_VA_CLR0 + i, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0 + i, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    // extra colour channel carrying the fog colour (rgb) + fog factor (a)
    if (prg->cc_features.opt_fog && num_chans < 2)
    {
        GX_SetVtxDesc(GX_VA_CLR0 + num_chans, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0 + num_chans, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    // tex coords
    if (has_tex)
    {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }
}

static bool tnl_active;     // GPU-path vertex format / TEV / channels are what's set up
static void gx_tnl_leave(void);

static void gfx_gx_load_shader(struct ShaderProgram *new_prg)
{
    if (tnl_active)
        gx_tnl_leave();
    current_shader = new_prg->program_id;

    update_vtx_desc(new_prg);

    update_tev(new_prg);

    if (new_prg->cc_features.opt_texture_edge && new_prg->cc_features.opt_alpha)
    {
        // Reject alpha before Z otherwise invisible billboards hide distant shadows
        GX_SetAlphaCompare(GX_GREATER, GX_TEXTURE_EDGE_ALPHA_THRESHOLD, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_FALSE);
    }
    else
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
    }
}

static void gfx_gx_unload_shader(UNUSED struct ShaderProgram *old_prg)
{
}

static struct ShaderProgram *gfx_gx_create_and_load_new_shader(uint32_t shader_id)
{
    int id = shader_program_pool_size;

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];

    prg->program_id = id;

    prg->shader_id = shader_id;
    gfx_cc_get_features(shader_id, &prg->cc_features);

    prg->num_floats = 4;

    if (prg->cc_features.used_textures[0] || prg->cc_features.used_textures[1])
    {
        prg->num_floats += 2;
    }
    if (prg->cc_features.opt_fog)
    {
        prg->num_floats += 1; // packed RGBA8 (see gfx_pc.c gfx_sp_tri1)
    }
    prg->num_floats += prg->cc_features.num_inputs; // one packed RGBA8 per input

    gfx_gx_load_shader(prg);

    return prg;
}

static struct ShaderProgram *gfx_gx_lookup_shader(uint32_t shader_id)
{
    for (uint8_t i = 0; i < shader_program_pool_size; i++)
    {
        if (shader_program_pool[i].shader_id == shader_id)
        {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_gx_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2])
{
    *num_inputs = prg->cc_features.num_inputs;
    used_textures[0] = prg->cc_features.used_textures[0];
    used_textures[1] = prg->cc_features.used_textures[1];
}

static uint32_t gfx_gx_new_texture(void)
{
    if (texture_index == TEXTURE_POOL_SIZE)
        return 0; // out of textures

    return texture_index++;
}

// opt_state_cache: which texture object each map holds, so an unchanged one isn't reloaded on
// every draw. Invalidated whenever a texture object is (re)initialised, the frame starts (each
// tile replay starts from whatever the previous one left) and after the overlay borrows a map.
static int32_t loaded_tex[2] = { -1, -1 };

static void gx_invalidate_loaded_tex(void)
{
    loaded_tex[0] = loaded_tex[1] = -1;
}

static void gx_load_tex(int map, uint32_t texture_id)
{
    if (configOptStateCache && loaded_tex[map] == (int32_t)texture_id)
        return;
    GX_LoadTexObj(&texture_pool[texture_id], map == 0 ? GX_TEXMAP0 : GX_TEXMAP1);
    loaded_tex[map] = (int32_t)texture_id;
}

static void gfx_gx_select_texture(int tile, uint32_t texture_id)
{
    current_texture = texture_id;
    texture_units[tile] = texture_id;

    if (texture_storage[texture_id].data != NULL)
        gx_load_tex(tile, texture_id);
}

// from https://github.com/camthesaxman/neverball-wii/blob/master/share/wiigl.c
static uint32_t round_up(uint32_t number, uint32_t multiple)
{
    return ((number + multiple - 1) / multiple) * multiple;
}

// ---------------------------------------------------------------------------------------------
// Benchmark GPU timing
//
// Split frames hold the GPU back with a command processor FIFO breakpoint placed at the start of
// the frame, so it executes nothing while the CPU builds the frame. At the end of the frame the
// breakpoint is removed and the GPU runs the whole frame on its own: the time from release to
// GX_DrawDone returning is pure GPU time. Draw-sync tokens at each change between 3D and 2D
// draws (and before the XFB copy) are timestamped from their interrupt, splitting that time by
// task. Overlapped frames run normally (GPU consuming while the CPU writes), which is how the
// game really runs, but they can't separate GPU time.

#define BENCH_MAX_TOKENS 60
#define BENCH_TOKEN_BASE 0xB000
#define BENCH_SPILL_BYTES (DEFAULT_FIFO_SIZE * 3 / 4)

static vu32 *const pi_reg = (vu32 *)0xCC003000;
static vu16 *const cp_reg = (vu16 *)0xCC000000;

static bool bench_split;      // breakpoint set, GPU held back
static bool bench_released;   // split frame whose GPU work was released at the end
static bool bench_spilled;    // split frame released early because the FIFO filled up
static uint32_t bench_t_release;
static int bench_cur_cls = -1;
static int bench_ntok;
static uint8_t bench_tok_cls[BENCH_MAX_TOKENS + 1];
static volatile uint32_t bench_tok_tb[BENCH_MAX_TOKENS + 1];

static void bench_token_callback(u16 token)
{
    unsigned i = token - BENCH_TOKEN_BASE;
    if (i >= 1 && i <= BENCH_MAX_TOKENS)
        bench_tok_tb[i] = bench_tb();
}

static void bench_release_gpu(bool spilled)
{
    if (!bench_split)
        return;
    GX_Flush();
    bench_t_release = bench_tb();
    GX_DisableBreakPt();
    bench_split = false;
    bench_released = true;
    bench_spilled = spilled;
}

static void bench_frame_start_gpu(void)
{
    bench_split = false;
    bench_released = false;
    bench_spilled = false;
    bench_cur_cls = -1;
    bench_ntok = 0;
    if (!bench_split_this_frame())
        return;

    // The GPU is idle (the previous frame ended with GX_DrawDone), so its read pointer is at the
    // write pointer; a breakpoint there stops it before this frame's first command
    GX_Flush();
    u32 wp = pi_reg[5] & 0x1FFFFFE0;
    GX_EnableBreakPt((void *)MEM_PHYSICAL_TO_K0(wp));
    bench_split = true;
}

static uint32_t bench_fifo_bytes(void)
{
    return ((uint32_t)cp_reg[25] << 16) | cp_reg[24];
}

// Called before each draw: marks the end of the previous draw class with a token
static void bench_gpu_class(int cls)
{
    if (!bench_split)
        return;
    if (bench_fifo_bytes() > BENCH_SPILL_BYTES) {
        bench_release_gpu(true);
        return;
    }
    if (hr_enabled)
        return; // tokens would fire once per tile; 1280x240p reports only the GPU total
    if (bench_cur_cls < 0) {
        bench_cur_cls = cls;
        return;
    }
    if (cls == bench_cur_cls || bench_ntok >= BENCH_MAX_TOKENS)
        return;
    bench_ntok++;
    bench_tok_cls[bench_ntok] = (uint8_t)bench_cur_cls;
    bench_tok_tb[bench_ntok] = 0;
    GX_SetDrawSync(BENCH_TOKEN_BASE + bench_ntok);
    bench_cur_cls = cls;
}

static void bench_frame_end_gpu(uint32_t t_done)
{
    uint32_t task[BENCH_GPU_COUNT] = { 0 };
    bool valid = bench_released && !bench_spilled;

    if (valid) {
        uint32_t prev = bench_t_release;
        for (int i = 1; i <= bench_ntok; i++) {
            uint32_t t = bench_tok_tb[i];
            if (t == 0 || (int32_t)(t - prev) < 0)
                continue; // token not seen; its time is counted with the next one
            task[bench_tok_cls[i]] += t - prev;
            prev = t;
        }
        if (hr_enabled)
            task[BENCH_GPU_WORLD] += t_done - prev; // not split by task in 1280x240p
        else
            task[bench_cur_cls < 0 ? BENCH_GPU_COPY : bench_cur_cls] += t_done - prev;
    }
    bench_gpu_frame(valid, valid ? t_done - bench_t_release : 0, task, bench_spilled);
}

// Waits for the GPU outside the normal end of frame (rare texture memory pressure)
static void gx_wait_gpu(void)
{
    bench_release_gpu(true);
    GX_DrawDone();
}

// ---------------------------------------------------------------------------------------------
// GPU-only memory (opt_mem2_gpu, Wii): texture data, the tile display lists and the crop scratch
// buffer are only ever read or written by the GPU, so they go in a heap in MEM2. That leaves MEM1,
// which the CPU reaches with less latency, to the game's own data.
#ifdef __wii__
#include <ogc/lwp_heap.h>
static heap_cntrl gpu_heap;
static uint8_t *gpu_heap_lo, *gpu_heap_hi;
#define GPU_HEAP_SIZE (16 * 1024 * 1024)
#endif

static void gpu_heap_init(void)
{
#ifdef __wii__
    if (!configOptMem2Gpu || gpu_heap_lo != NULL)
        return;
    uint8_t *lo = (uint8_t *)(((uintptr_t)SYS_GetArena2Lo() + 31) & ~31);
    uint8_t *hi = (uint8_t *)SYS_GetArena2Hi();
    if (hi - lo < GPU_HEAP_SIZE + 4 * 1024 * 1024)
    {
        dbglog("gpu heap: not enough MEM2 (%p-%p), using the normal heap", lo, hi);
        return;
    }
    SYS_SetArena2Lo(lo + GPU_HEAP_SIZE);
    __lwp_heap_init(&gpu_heap, lo, GPU_HEAP_SIZE, 32);
    gpu_heap_lo = lo;
    gpu_heap_hi = lo + GPU_HEAP_SIZE;
    dbglog("gpu heap: %u KB in MEM2 at %p", (unsigned)(GPU_HEAP_SIZE / 1024), lo);
#endif
}

// 32-byte aligned; the heap's own pointer is kept just below the returned block
static void *gpu_alloc(size_t size)
{
#ifdef __wii__
    if (gpu_heap_lo != NULL)
    {
        uint8_t *raw = __lwp_heap_allocate(&gpu_heap, size + 64);
        if (raw != NULL)
        {
            uint8_t *p = (uint8_t *)(((uintptr_t)raw + 32 + 31) & ~31);
            ((void **)p)[-1] = raw;
            return p;
        }
    }
#endif
    return memalign(32, size);
}

static void gpu_free(void *p)
{
    if (p == NULL)
        return;
#ifdef __wii__
    if ((uint8_t *)p >= gpu_heap_lo && (uint8_t *)p < gpu_heap_hi)
    {
        __lwp_heap_free(&gpu_heap, ((void **)p)[-1]);
        return;
    }
#endif
    free(p);
}

// GX can still be reading old texture memory after commands enter the FIFO
static void gx_free_deferred_texture_data(void)
{
    for (size_t i = 0; i < deferred_texture_free_count; i++)
    {
        gpu_free(deferred_texture_frees[i]);
    }
    deferred_texture_free_count = 0;
}

static void gx_defer_texture_free(void *data)
{
    if (data == NULL)
        return;

    if (deferred_texture_free_count == TEXTURE_POOL_SIZE)
    {
        // Can't wait for the GPU while a frame is being recorded; leak instead (very rare)
        if (hr_recording)
            return;
        gx_wait_gpu();
        gx_free_deferred_texture_data();
    }

    deferred_texture_frees[deferred_texture_free_count++] = data;
}

// Memory for a texture, freeing textures the GPU has finished with if needed (NULL if none)
static void *gx_alloc_texture(size_t size)
{
    void *dest = gpu_alloc(size);
    if (dest == NULL)
    {
        if (hr_recording)
            return NULL;
        gx_wait_gpu();
        gx_free_deferred_texture_data();
        dest = gpu_alloc(size);
    }
    return dest;
}

// Point the current texture object at new data, keeping the sampler state gfx_pc configured
static void gx_install_texture(void *dest, size_t size, int width, int height, uint8_t gxfmt)
{
    DCFlushRange(dest, size);

    // GX_InitTexObj resets wrap/filter
    // Please preserve the sampler state gfx_pc configured just before this
    // It did hard work, okay.
    GXTexObj *obj = &texture_pool[current_texture];
    u8 wrap_s = GX_GetTexObjWrapS(obj);
    u8 wrap_t = GX_GetTexObjWrapT(obj);
    u8 min_filt, mag_filt;
    GX_GetTexObjFilterMode(obj, &min_filt, &mag_filt);

    gx_defer_texture_free(texture_storage[current_texture].data);
    texture_storage[current_texture].data = dest;

    GX_InitTexObj(obj, dest, width, height, gxfmt, wrap_s, wrap_t, GX_FALSE);
    GX_InitTexObjFilterMode(obj, min_filt, mag_filt);
    gx_invalidate_loaded_tex();
}

static void convert_to_rgb5a3(uint16_t *dest, const uint8_t *data, uint32_t width, uint32_t height,
                              uint32_t buffer_width, uint32_t buffer_height)
{
    for (uint32_t x = 0; x < buffer_width; x++)
    {
        uint32_t blockX = x / 4;
        uint32_t remX = x % 4;
        uint32_t srcX = MIN(x, width - 1);

        for (uint32_t y = 0; y < buffer_height; y++)
        {
            uint8_t r, g, b, a;
            uint16_t pixel;
            uint32_t srcY = MIN(y, height - 1);
            uint32_t srcIndex = 4 * (srcX + srcY * width);

            if (data[srcIndex + 3] == 255)
            {
                r = (data[srcIndex + 0] >> 3) & 31;
                g = (data[srcIndex + 1] >> 3) & 31;
                b = (data[srcIndex + 2] >> 3) & 31;
                pixel = (1 << 15) | (r << 10) | (g << 5) | b;
            }
            else
            {
                r = (data[srcIndex + 0] >> 4) & 15;
                g = (data[srcIndex + 1] >> 4) & 15;
                b = (data[srcIndex + 2] >> 4) & 15;
                a = (data[srcIndex + 3] >> 5) & 7;
                pixel = (a << 12) | (r << 8) | (g << 4) | b;
            }

            uint32_t blockY = y / 4;
            uint32_t remY = y % 4;
            uint32_t index = 16 * (blockX + blockY * buffer_width / 4) + (remY * 4 + remX);
            dest[index] = pixel;
        }
    }
}

static void gfx_gx_upload_texture(const uint8_t *rgba32_buf, int width, int height)
{
    uint32_t buffer_width = round_up(width, 4);
    uint32_t buffer_height = round_up(height, 4);
    size_t texture_size = (size_t)buffer_width * buffer_height * sizeof(uint16_t);

    uint16_t *dest = gx_alloc_texture(texture_size);
    if (dest == NULL)
        return;

    convert_to_rgb5a3(dest, rgba32_buf, width, height, buffer_width, buffer_height);
    gx_install_texture(dest, texture_size, width, height, GX_TF_RGB5A3);
}

// ---------------------------------------------------------------------------------------------
// opt_native_tex: N64 texture data converted in one pass into a GX format that holds it exactly
// (formerly N64 -> RGBA32 -> RGB5A3, which also cut intensity and alpha to 4-5 / 3 bits):
//   RGBA16, CI4, CI8 -> RGB5A3 (the same texels as before)
//   IA4 -> IA8, IA8 -> IA4, IA16 -> IA8, I4 -> IA4 (alpha opaque), I8 -> IA8 (alpha opaque)
// I4/I8 keep this port's opaque alpha, so they use the IA formats rather than GX's I formats
// (which would repeat the intensity as alpha). RGBA32 (rare) still takes the old path.

// GX tiling: 16-bit texels in 4x4 blocks, 8-bit texels in 8x4 blocks, each block 32 bytes
static inline uint32_t tex_index16(uint32_t x, uint32_t y, uint32_t bw)
{
    return 16 * ((x >> 2) + (y >> 2) * (bw >> 2)) + (y & 3) * 4 + (x & 3);
}

static inline uint32_t tex_index8(uint32_t x, uint32_t y, uint32_t bw)
{
    return 32 * ((x >> 3) + (y >> 2) * (bw >> 3)) + (y & 3) * 8 + (x & 7);
}

static inline uint16_t rgba16_to_rgb5a3(uint16_t col)
{
    uint32_t r = col >> 11, g = (col >> 6) & 0x1f, b = (col >> 1) & 0x1f;
    if (col & 1)
        return 0x8000 | (r << 10) | (g << 5) | b;
    // transparent: same as the old RGBA32 path (4-bit colour, alpha 0)
    r = ((r * 0xFF) / 0x1F) >> 4;
    g = ((g * 0xFF) / 0x1F) >> 4;
    b = ((b * 0xFF) / 0x1F) >> 4;
    return (uint16_t)((r << 8) | (g << 4) | b);
}

bool gfx_gx_upload_texture_n64(uint8_t fmt, uint8_t siz, const uint8_t *addr, uint32_t size_bytes,
                               uint32_t line_size_bytes, const uint8_t *palette)
{
    // N64 formats (gbi.h): RGBA 0, YUV 1, CI 2, IA 3, I 4; sizes 4b 0, 8b 1, 16b 2, 32b 3
    enum { F_RGBA = 0, F_CI = 2, F_IA = 3, F_I = 4 };
    enum { S_4 = 0, S_8 = 1, S_16 = 2 };
    if (line_size_bytes == 0 || addr == NULL)
        return false;
    uint32_t bits;
    uint8_t gxfmt;
    if (fmt == F_RGBA && siz == S_16) { bits = 16; gxfmt = GX_TF_RGB5A3; }
    else if (fmt == F_CI && (siz == S_4 || siz == S_8)) { bits = siz == S_4 ? 4 : 8; gxfmt = GX_TF_RGB5A3; }
    else if (fmt == F_IA && siz == S_4) { bits = 4; gxfmt = GX_TF_IA8; }
    else if (fmt == F_IA && siz == S_8) { bits = 8; gxfmt = GX_TF_IA4; }
    else if (fmt == F_IA && siz == S_16) { bits = 16; gxfmt = GX_TF_IA8; }
    else if (fmt == F_I && siz == S_4) { bits = 4; gxfmt = GX_TF_IA4; }
    else if (fmt == F_I && siz == S_8) { bits = 8; gxfmt = GX_TF_IA8; }
    else return false;
    if (fmt == F_CI && palette == NULL)
        return false;

    // Same dimensions as gfx_pc's import_texture_* functions
    const uint32_t width = line_size_bytes * 8 / bits;
    const uint32_t height = size_bytes / line_size_bytes;
    const uint32_t texels = size_bytes * 8 / bits;
    if (width == 0 || height == 0)
        return false;

    const bool wide = (gxfmt == GX_TF_IA4); // 8-bit GX texels
    const uint32_t bw = round_up(width, wide ? 8 : 4);
    const uint32_t bh = round_up(height, 4);
    const size_t bytes = (size_t)bw * bh * (wide ? 1 : 2);
    void *dest = gx_alloc_texture(bytes);
    if (dest == NULL)
        return true; // out of memory: draw untextured rather than decode twice
    uint16_t *d16 = dest;
    uint8_t *d8 = dest;

    uint16_t pal[256];
    if (fmt == F_CI)
    {
        const uint32_t n = siz == S_4 ? 16 : 256;
        for (uint32_t i = 0; i < n; i++)
            pal[i] = rgba16_to_rgb5a3((uint16_t)((palette[2 * i] << 8) | palette[2 * i + 1]));
    }

    for (uint32_t y = 0; y < bh; y++)
    {
        const uint32_t sy = y < height ? y : height - 1;
        for (uint32_t x = 0; x < bw; x++)
        {
            const uint32_t sx = x < width ? x : width - 1;
            const uint32_t i = sy * width + sx;
            uint32_t v = 0;
            if (i < texels)
            {
                if (bits == 4)
                    v = (addr[i >> 1] >> ((i & 1) ? 0 : 4)) & 0xF;
                else if (bits == 8)
                    v = addr[i];
                else
                    v = (addr[2 * i] << 8) | addr[2 * i + 1];
            }
            uint16_t out;
            if (fmt == F_RGBA)
                out = rgba16_to_rgb5a3((uint16_t)v);
            else if (fmt == F_CI)
                out = pal[v];
            else if (fmt == F_IA && siz == S_4)
                out = (uint16_t)(((v & 1) ? 0xFF00 : 0) | ((v >> 1) * 0x24)); // IA8: alpha, intensity
            else if (fmt == F_IA && siz == S_8)
                out = (uint16_t)(((v & 0xF) << 4) | (v >> 4));                // IA4: alpha nibble high
            else if (fmt == F_IA)
                out = (uint16_t)(((v & 0xFF) << 8) | (v >> 8));               // IA16 -> IA8
            else if (siz == S_4)
                out = (uint16_t)(0xF0 | v);                                    // I4 -> IA4, opaque
            else
                out = (uint16_t)(0xFF00 | v);                                  // I8 -> IA8, opaque
            if (wide)
                d8[tex_index8(x, y, bw)] = (uint8_t)out;
            else
                d16[tex_index16(x, y, bw)] = out;
        }
    }
    gx_install_texture(dest, bytes, width, height, gxfmt);
    return true;
}

static uint32_t gfx_cm_to_gx(uint32_t val)
{
    return (val & G_TX_CLAMP) ? GX_CLAMP : (val & G_TX_MIRROR) ? GX_MIRROR : GX_REPEAT;
}

static void gfx_gx_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt)
{
    GX_InitTexObjWrapMode(&texture_pool[texture_units[tile]], gfx_cm_to_gx(cms), gfx_cm_to_gx(cmt));
    GX_InitTexObjFilterMode(&texture_pool[texture_units[tile]], linear_filter ? GX_LINEAR : GX_NEAR, linear_filter ? GX_LINEAR : GX_NEAR);
    gx_invalidate_loaded_tex();
}

static bool depth_test_on;
static bool depth_mask_on;
static bool zmode_decal_on;

static int vp_x, vp_y, vp_w, vp_h;

static f32 applied_far_z = 1.0f;

static void set_z_mode()
{
    GX_SetZMode(depth_test_on, GX_LEQUAL, depth_mask_on);
}

static void gfx_gx_set_depth_test(bool depth_test)
{
    depth_test_on = depth_test;
    set_z_mode();
}

static void gfx_gx_set_depth_mask(bool z_upd)
{
    depth_mask_on = z_upd;
    set_z_mode();
}

static void hr_seg_capture(struct HiresSeg *seg)
{
    seg->vp[0] = vp_x;
    seg->vp[1] = vp_y;
    seg->vp[2] = vp_w;
    seg->vp[3] = vp_h;
    seg->far_z = applied_far_z;
    seg->sc[0] = sc_x;
    seg->sc[1] = sc_y;
    seg->sc[2] = sc_w;
    seg->sc[3] = sc_h;
    seg->proj = cur_proj;
}

static void hr_begin_seg(void)
{
    struct HiresSeg *seg = &hr_segs[hr_nseg];
    seg->offset = hr_pos;
    seg->size = 0;
    seg->cullable = false;
    hr_seg_capture(seg);
    hr_seg_drawn = false;
    GX_BeginDispList(hr_dl + hr_pos, HIRES_DL_SIZE - hr_pos);
    // One CP NOP, so a segment with no other commands doesn't end with size 0 (which is how
    // GX_EndDispList reports an overflow)
    *(volatile uint8_t *)0xCC008000 = 0x00;
}

static void hr_end_seg(void)
{
    u32 n = GX_EndDispList();
    if (n == 0)
    {
        hr_overflow = true;
        return;
    }
    hr_segs[hr_nseg].size = n;
    hr_pos += n;
    hr_nseg++;
}

// Viewport or scissor changed while recording
static void hr_state_changed(void)
{
    if (hr_overflow)
        return; // the list overflowed; the rest of this frame draws directly and is discarded
    if (!hr_seg_drawn || hr_overflow || hr_nseg >= HIRES_MAX_SEGS - 1
        || hr_pos + 64 * 1024 > HIRES_DL_SIZE)
    {
        // Nothing drawn under the old state yet (or out of room): just retarget this segment
        hr_seg_capture(&hr_segs[hr_nseg]);
        return;
    }
    hr_end_seg();
    if (hr_overflow)
        return;
    hr_begin_seg();
}

// Bounds of a batch in frame pixels (false = can't be bounded, or it covers every tile anyway)
static bool hr_draw_bounds(const float *v, int stride, size_t n, bool is_2d, int16_t bb[4])
{
    if (hr_tiles_x * hr_tiles_y < 2 || hr_nseg >= HIRES_MAX_SEGS - 4 || hr_pos + 64 * 1024 > HIRES_DL_SIZE)
        return false;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (size_t i = 0; i < n; i++, v += stride)
    {
        float x = v[0], y = v[1];
        if (!is_2d)
        {
            const float w = v[3];
            if (w < 0.001f)
                return false; // behind the eye: the clipped triangle can be anywhere
            const float iw = 1.0f / w;
            x *= iw;
            y *= iw;
        }
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    // NDC -> frame pixels through the current viewport (y down), with a 2 pixel margin
    float px0 = vp_x + (x0 + 1.0f) * 0.5f * vp_w - 2.0f;
    float px1 = vp_x + (x1 + 1.0f) * 0.5f * vp_w + 2.0f;
    float py0 = vp_y + (1.0f - y1) * 0.5f * vp_h - 2.0f;
    float py1 = vp_y + (1.0f - y0) * 0.5f * vp_h + 2.0f;
    // and inside the scissor
    if (px0 < sc_x) px0 = sc_x;
    if (py0 < sc_y) py0 = sc_y;
    if (px1 > sc_x + sc_w) px1 = sc_x + sc_w;
    if (py1 > sc_y + sc_h) py1 = sc_y + sc_h;
    const float fw = hr_tiles_x * hr_tile_w, fh = hr_tiles_y * hr_tile_h;
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > fw) px1 = fw;
    if (py1 > fh) py1 = fh;
    if (px1 <= px0 || py1 <= py0)
    {
        px0 = py0 = px1 = py1 = 0; // off screen: culled on every tile
    }
    bb[0] = (int16_t)px0;
    bb[1] = (int16_t)py0;
    bb[2] = (int16_t)(px1 + 0.999f);
    bb[3] = (int16_t)(py1 + 0.999f);
    // Worth a segment only if it misses at least one tile
    const int tx0 = bb[0] / hr_tile_w, tx1 = (bb[2] - 1) / hr_tile_w;
    const int ty0 = bb[1] / hr_tile_h, ty1 = (bb[3] - 1) / hr_tile_h;
    if (bb[2] <= bb[0])
        return true;
    return (tx1 - tx0 + 1) * (ty1 - ty0 + 1) < hr_tiles_x * hr_tiles_y;
}

// Close the segment holding the state commands before this draw and open one for the draw alone
static bool hr_split_before_draw(void)
{
    hr_end_seg();
    if (hr_overflow)
        return false;
    hr_begin_seg();
    hr_segs[hr_nseg].cullable = true;
    return true;
}

static void hr_split_after_draw(const int16_t bb[4])
{
    if (hr_overflow)
        return;
    memcpy(hr_segs[hr_nseg].bb, bb, sizeof(hr_segs[hr_nseg].bb));
    hr_end_seg();
    if (hr_overflow)
        return;
    hr_begin_seg();
}

static void hr_apply_scissor(int x, int y, int w, int h)
{
    // Direct (unrecorded) scissor while in a tiled mode: keep it inside the top left tile
    int x1 = MIN(x + w, hr_tile_w);
    int y1 = MIN(y + h, hr_tile_h);
    GX_SetScissor(x, y, x1 > x ? x1 - x : 0, y1 > y ? y1 - y : 0);
}

static void gx_issue_viewport(f32 far_z)
{
    applied_far_z = far_z;
    if (hr_recording)
    {
        hr_state_changed();
        return;
    }
    GX_SetViewport((f32)vp_x, (f32)vp_y, (f32)vp_w, (f32)vp_h, 0.0f, far_z); // near z, far z
}

static void gfx_gx_set_zmode_decal(bool zmode_decal)
{
    zmode_decal_on = zmode_decal;
}

// If for some reason you like stretched 4:3 this is why this isn't forced.
// You're wrong, but I'll give you this at least.
static bool gx_pillarbox_active(void)
{
    return configPillarbox && !configWidescreen;
}

static int gx_map_x(int x, int fbWidth)
{
    if (gx_pillarbox_active())
        return fbWidth / 8 + x * fbWidth * 3 / (DESIRED_SCREEN_WIDTH * 4);
    return x * fbWidth / DESIRED_SCREEN_WIDTH;
}

static int gx_map_w(int width, int fbWidth)
{
    if (gx_pillarbox_active())
        return width * fbWidth * 3 / (DESIRED_SCREEN_WIDTH * 4);
    return width * fbWidth / DESIRED_SCREEN_WIDTH;
}

// Height of the whole frame: 720 in 720p (rendered as two rows of tiles), else the EFB height
static int gx_frame_h(void)
{
    return gfx_gx_wm_get_frame_height();
}

static void gfx_gx_set_viewport(int x, int y, int width, int height)
{
    const int fh = gx_frame_h();
    const int fw = gfx_gx_wm_get_frame_width();
    vp_x = gx_map_x(x, fw);
    vp_w = gx_map_w(width, fw);
    vp_y = y * fh / DESIRED_SCREEN_HEIGHT;
    vp_h = height * fh / DESIRED_SCREEN_HEIGHT;
    gx_issue_viewport(1.0f);
}

static void gfx_gx_set_scissor(int x, int y, int width, int height)
{
    // OpenGL has a bottom left origin, GX has top right
    // This took me hours to find out
    const int fw = gfx_gx_wm_get_frame_width();
    int sx = gx_map_x(x, fw);
    int sw = gx_map_w(width, fw);
    const int fh = gx_frame_h();
    int sy = y * fh / DESIRED_SCREEN_HEIGHT;
    int sh = height * fh / DESIRED_SCREEN_HEIGHT;
    sc_x = sx;
    sc_y = fh - sy - sh;
    sc_w = sw;
    sc_h = sh;
    if (hr_recording)
        hr_state_changed();
    else if (hr_enabled)
        hr_apply_scissor(sc_x, sc_y, sc_w, sc_h);
    else
        GX_SetScissor(sc_x, sc_y, sc_w, sc_h);
}

static bool cur_use_alpha;

static void gfx_gx_set_use_alpha(bool use_alpha)
{
    cur_use_alpha = use_alpha;
    if (use_alpha)
    {
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR); // guess
    }
    else
    {
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR); // another guess
    }
}

static uint8_t float_to_u8(float x)
{
    return MAX(0, MIN(255, x * 255));
}

static void gfx_gx_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (tnl_active)
        gx_tnl_leave();
    bool hasAlpha = shader_program_pool[current_shader].cc_features.opt_alpha;
    bool hasFog = shader_program_pool[current_shader].cc_features.opt_fog;
    bool hasTex = shader_program_pool[current_shader].cc_features.used_textures[0]
                  || shader_program_pool[current_shader].cc_features.used_textures[1];

    uint8_t num_floats =  shader_program_pool[current_shader].num_floats;
    uint8_t num_inputs = shader_program_pool[current_shader].cc_features.num_inputs;

    // Make sure each map holds the fully-initialised texobj (skipped when it already does)
    if (shader_program_pool[current_shader].cc_features.used_textures[0]
        && texture_storage[texture_units[0]].data != NULL)
        gx_load_tex(0, texture_units[0]);
    if (shader_program_pool[current_shader].cc_features.used_textures[1]
        && texture_storage[texture_units[1]].data != NULL)
        gx_load_tex(1, texture_units[1]);

    // gfx_pc emits 2D rectangles with w == 1
    bool is_2d = (buf_vbo[3] == 1.0f);

    if (gBenchOn)
        bench_gpu_class(is_2d ? BENCH_GPU_2D : BENCH_GPU_WORLD);

    if (hr_recording)
    {
        // Projection is applied per tile at replay time
        const uint8_t want = is_2d ? (sky_on ? PROJ_SKY : PROJ_ORTHO) : sw_persp_proj;
        if (want != cur_proj)
        {
            cur_proj = want;
            hr_state_changed();
        }
    }
    else if (is_2d)
        GX_LoadProjectionMtx(sky_on ? gx_sky_mtx : gx_ortho_mtx, GX_ORTHOGRAPHIC);
    else
        GX_LoadProjectionMtx(gx_perspective_mtx, GX_PERSPECTIVE);

    const f32 want_far_z = (!is_2d && zmode_decal_on) ? (1.0f - decal_bias) : 1.0f;
    if (want_far_z != applied_far_z)
        gx_issue_viewport(want_far_z);

    if (hr_recording)
        hr_seg_drawn = true;

    // opt_tile_cull: a draw that misses some of the tiles gets a segment of its own, which the
    // replay skips on the tiles it misses
    int16_t bb[4];
    const bool split = hr_recording && configOptTileCull && !hr_overflow
                       && hr_draw_bounds(buf_vbo, num_floats, 3 * buf_vbo_num_tris, is_2d, bb)
                       && hr_split_before_draw();

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3 * buf_vbo_num_tris);
    {
        const bool emit_fog = hasFog && num_inputs < 2;
        const float *p = buf_vbo;
        for (size_t i = 0; i < 3 * buf_vbo_num_tris; i++, p += num_floats)
        {
            if (is_2d)
                GX_Position3f32(p[0], p[1], p[2]);
            else
                GX_Position3f32(p[0], p[1], -p[3]);

            int vo = 4;
            float s = 0.0f, t = 0.0f;
            if (hasTex)
            {
                s = p[4];
                t = p[5];
                vo = 6;
            }
            uint32_t fogc = 0;
            if (hasFog)
            {
                memcpy(&fogc, &p[vo], 4); // packed fog rgb + factor
                vo++;
            }
            if (hr_enabled)
            {
                // Fixed format: always two colours and a texcoord
                uint32_t col[2] = { 0x000000FF, 0x000000FF };
                int ci = 0;
                for (int j = 0; j < num_inputs && ci < 2; j++)
                    memcpy(&col[ci++], &p[vo + j], 4);
                if (emit_fog && ci < 2)
                    col[ci] = fogc;
                GX_Color1u32(col[0]);
                GX_Color1u32(col[1]);
                GX_TexCoord2f32(s, t);
                continue;
            }
            for (int j = 0; j < num_inputs && j < 2; j++)
            {
                uint32_t c;
                memcpy(&c, &p[vo + j], 4);
                GX_Color1u32(c);
            }
            if (emit_fog)
                GX_Color1u32(fogc);
            if (hasTex)
                GX_TexCoord2f32(s, t);
        }
    }
    GX_End();
    (void)hasAlpha;

    if (split)
        hr_split_after_draw(bb);
}

// ---------------------------------------------------------------------------------------------
// GPU vertex path (opt_gpu_tnl, see gfx_gx_tnl.h)

static struct GxTnlState tnl_cur;  // what's applied, valid while tnl_valid
static bool tnl_valid;
static Mtx44 tnl_proj;
static bool tnl_proj_loaded;      // non-tiled: tnl_proj is the loaded projection
static uint8_t tnl_hr_proj = PROJ_PERSP; // tiled: the GPU path's projection (CPU-path draws switch cur_proj away)

static float tnl_near = GX_NEAR_PLANE, tnl_far = GX_FAR_PLANE;
float gfx_gx_tnl_near(void) { return tnl_near; }
float gfx_gx_tnl_far(void) { return tnl_far; }

// Index of a projection in this frame's table (tiled modes), adding it if new
void gfx_gx_set_sky(bool on)
{
#ifdef SKYDBG
    static unsigned n;
    if (on && n++ % 300 == 0)
        dbglog("sky last: %u skyboxes drawn last", n);
#endif
    sky_on = on;
}

static int hw_proj_add(const Mtx44 m)
{
    for (int i = hw_nproj - 1; i >= 0; i--)
        if (memcmp(hw_proj[i], m, sizeof(Mtx44)) == 0)
            return i;
    if (hw_nproj == HW_PROJ_MAX)
    {
        static unsigned full;
        if (full++ % 300 == 0)
            dbglog("hw proj table full (%d)", HW_PROJ_MAX);
        return HW_PROJ_MAX - 1;
    }
    memcpy(hw_proj[hw_nproj], m, sizeof(Mtx44));
    return hw_nproj++;
}

void gfx_gx_tnl_set_depth(float n, float f)
{
    if (!configFog && f < GX_FAR_PLANE_EXTENDED)
        f = GX_FAR_PLANE_EXTENDED; // fog off keeps its longer draw distance
    tnl_near = n;
    tnl_far = f;
    decal_bias = GX_DECAL_BIAS * n / GX_NEAR_PLANE;
    if (decal_bias > 0.01f)
        decal_bias = 0.01f;
    // The CPU path feeds clip w as z: same depth function as the GPU path's projection
    gx_perspective_mtx[2][2] = -n / (f - n);
    gx_perspective_mtx[2][3] = -(n * f) / (f - n);
    if (hr_recording)
        sw_persp_proj = PROJ_HW + hw_proj_add(gx_perspective_mtx);
}

void gfx_gx_tnl_begin_frame(void *pos, void *tc, void *col)
{
    // Vertex arrays (CP registers; the CPU path never touches arrays). Called after the frame
    // started, so in the tiled modes this is recorded and replayed before every tile.
    GX_SetArray(GX_VA_POS, pos, 12);
    GX_SetArray(GX_VA_TEX0, tc, 4);
    GX_SetArray(GX_VA_CLR0, col, 4);
    GX_SetArray(GX_VA_NRM, col, 4);
    GX_InvVtxCache(); // the arrays were rewritten since the last frame
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_NRM, GX_NRM_XYZ, GX_S8, 0); // 6 fraction bits (fixed)
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_S16, 0);
    tnl_valid = false;
    tnl_proj_loaded = false;
}

// N64 modelview (row vectors, v * M) -> GX position matrix (column vectors) in a slot, and the
// same 3x3 scaled as the normal matrix
void gfx_gx_tnl_load_mtx(int slot, const float mv[4][4], float normal_scale)
{
    __Mtx m, n;
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
        {
            m[r][c] = mv[c][r];
            n[r][c] = mv[c][r] * normal_scale;
        }
        m[r][3] = mv[3][r];
        n[r][3] = 0.0f;
    }
    GX_LoadPosMtxImm(m, GX_PNMTX0 + 3 * slot);
    GX_LoadNrmMtxImm(n, GX_PNMTX0 + 3 * slot);
}

void gfx_gx_tnl_set_proj(const float p[4][4])
{
    if (hr_recording)
    {
        // The tiled replay rescales it per tile: keep it in this frame's table
        const int id = hw_proj_add(p);
        tnl_hr_proj = PROJ_HW + id;
        if (cur_proj != PROJ_HW + id)
        {
            cur_proj = PROJ_HW + id;
            hr_state_changed();
        }
        return;
    }
    memcpy(tnl_proj, p, sizeof(Mtx44));
    tnl_proj_loaded = false;
}

static uint8_t tnl_csrc(uint8_t item, const struct GxTnlState *st)
{
    if (item >= SHADER_INPUT_1 && item <= SHADER_INPUT_4)
    {
        switch (st->map[0][item - SHADER_INPUT_1])
        {
            case CC_SHADE: return GX_CC_RASC;
            case CC_PRIM: return GX_CC_C0;
            case CC_ENV: return GX_CC_C1;
            default: return GX_CC_ZERO;
        }
    }
    switch (item)
    {
        case SHADER_TEXEL0:
        case SHADER_TEXEL1: return GX_CC_TEXC;
        case SHADER_TEXEL0A: return GX_CC_TEXA;
        default: return GX_CC_ZERO;
    }
}

static uint8_t tnl_asrc(uint8_t item, const struct GxTnlState *st)
{
    if (item >= SHADER_INPUT_1 && item <= SHADER_INPUT_4)
    {
        switch (st->map[1][item - SHADER_INPUT_1])
        {
            case CC_SHADE: return GX_CA_RASA;
            case CC_PRIM: return GX_CA_A0;
            case CC_ENV: return GX_CA_A1;
            default: return GX_CA_ZERO;
        }
    }
    switch (item)
    {
        case SHADER_TEXEL0:
        case SHADER_TEXEL0A:
        case SHADER_TEXEL1: return GX_CA_TEXA;
        default: return GX_CA_ZERO;
    }
}

// The combiner in one TEV stage: every input is a register or the rasterised shade colour, so
// the two-stage arrangements the CPU path needs for two per-vertex inputs aren't needed. Same
// formula shapes as the CPU path (single / multiply / mix / a*c + d).
static void tnl_apply_tev(const struct GxTnlState *st)
{
    const struct CCFeatures *f = &st->prg->cc_features;
    const uint8_t *c = f->c[0], *a = f->c[1];
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, st->textured ? GX_TEXCOORD0 : GX_TEXCOORDNULL,
                   st->textured ? GX_TEXMAP0 : GX_TEXMAP_NULL, GX_COLOR0A0);
    if (f->do_single[0])
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, tnl_csrc(c[3], st));
    else if (f->do_multiply[0])
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, tnl_csrc(c[0], st), tnl_csrc(c[2], st), GX_CC_ZERO);
    else if (f->do_mix[0])
        GX_SetTevColorIn(GX_TEVSTAGE0, tnl_csrc(c[1], st), tnl_csrc(c[0], st), tnl_csrc(c[2], st), GX_CC_ZERO);
    else
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, tnl_csrc(c[0], st), tnl_csrc(c[2], st), tnl_csrc(c[3], st));
    if (f->do_single[1])
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, tnl_asrc(a[3], st));
    else if (f->do_multiply[1])
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, tnl_asrc(a[0], st), tnl_asrc(a[2], st), GX_CA_ZERO);
    else if (f->do_mix[1])
        GX_SetTevAlphaIn(GX_TEVSTAGE0, tnl_asrc(a[1], st), tnl_asrc(a[0], st), tnl_asrc(a[2], st), GX_CA_ZERO);
    else
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, tnl_asrc(a[0], st), tnl_asrc(a[2], st), tnl_asrc(a[3], st));
    gx_set_tev_op(GX_TEVSTAGE0);

    if (f->opt_texture_edge && f->opt_alpha)
    {
        GX_SetAlphaCompare(GX_GREATER, GX_TEXTURE_EDGE_ALPHA_THRESHOLD, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_FALSE);
    }
    else
    {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
    }
}

static void tnl_apply_lighting(const struct GxTnlState *st)
{
    uint8_t mask = 0;
    if (st->lighting)
    {
        for (int i = 0; i < st->num_lights && i < 2; i++)
        {
            GXLightObj lo;
            // Directional: a position very far along the direction, no attenuation
            GX_InitLightPos(&lo, st->light_dir[i][0] * 1e10f, st->light_dir[i][1] * 1e10f,
                            st->light_dir[i][2] * 1e10f);
            GX_InitLightColor(&lo, (GXColor){ st->light_col[i][0], st->light_col[i][1], st->light_col[i][2], 255 });
            GX_InitLightAttn(&lo, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
            GX_LoadLightObj(&lo, i == 0 ? GX_LIGHT0 : GX_LIGHT1);
            mask |= i == 0 ? GX_LIGHT0 : GX_LIGHT1;
        }
        GX_SetChanAmbColor(GX_COLOR0A0, (GXColor){ st->ambient[0], st->ambient[1], st->ambient[2], 0 });
    }
    GX_SetChanMatColor(GX_COLOR0A0, (GXColor){ 255, 255, 255, 255 });
    // N64 lighting: colour = ambient + sum of clamped N.L * light colour, clamped; material white
    GX_SetChanCtrl(GX_COLOR0, st->lighting ? GX_ENABLE : GX_DISABLE, GX_SRC_REG,
                   st->lighting ? GX_SRC_REG : GX_SRC_VTX, mask, GX_DF_CLAMP, GX_AF_NONE);
    GX_SetChanCtrl(GX_ALPHA0, GX_DISABLE, GX_SRC_REG, st->alpha_one ? GX_SRC_REG : GX_SRC_VTX,
                   GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
}

static void tnl_apply_texmtx(const struct GxTnlState *st)
{
    GX_SetNumTexGens(st->textured ? 1 : 0);
    if (!st->textured)
        return;
    __Mtx m;
    memset(m, 0, sizeof(m));
    memcpy(m[0], st->texmtx[0], sizeof(m[0]));
    memcpy(m[1], st->texmtx[1], sizeof(m[1]));
    GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX2x4);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);
}

static void tnl_apply_fog(const struct GxTnlState *st)
{
    GXColor c = { st->fog_col[0], st->fog_col[1], st->fog_col[2], 255 };
    if (st->fog)
        GX_SetFog(GX_FOG_ORTHO_LIN, st->fog_start, st->fog_end, 0.0f, 1.0f, c);
    else
        GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, c);
}

static uint8_t tnl_cull_mode(uint8_t n64)
{
    // N64 culls in a y-up screen space and GX in a y-down one, so the senses swap
    return n64 == 1 ? GX_CULL_FRONT : n64 == 2 ? GX_CULL_BACK : GX_CULL_NONE;
}

static void gx_tnl_enter(void)
{
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_PTNMTXIDX, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_NRM, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
    GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX16);
    GX_SetNumChans(1);
    tnl_active = true;
    tnl_valid = false;
    tnl_proj_loaded = false;
}

// Back to the CPU path's set-up
static void gx_tnl_leave(void)
{
    tnl_active = false;
    tnl_valid = false;
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, (GXColor){ 0, 0, 0, 255 });
    if (hr_enabled)
        gx_set_fixed_vtx_desc(); // vertex format and texgen the tiled replay expects
    if (shader_program_pool_size > 0)
        gfx_gx_load_shader(&shader_program_pool[current_shader]); // VCD (non-tiled), TEV, channels
}

void gfx_gx_tnl_draw(const struct GxTnlState *st, const uint8_t *mtx, const uint16_t *idx, int n)
{
    if (n <= 0)
        return;
    if (!tnl_active)
        gx_tnl_enter();

    const struct GxTnlState *o = tnl_valid ? &tnl_cur : NULL;
    if (o == NULL || o->prg != st->prg || memcmp(o->map, st->map, sizeof(st->map)) != 0
        || o->textured != st->textured)
        tnl_apply_tev(st);
    if (o == NULL || memcmp(o->prim, st->prim, 4) != 0)
        GX_SetTevColor(GX_TEVREG0, (GXColor){ st->prim[0], st->prim[1], st->prim[2], st->prim[3] });
    if (o == NULL || memcmp(o->env, st->env, 4) != 0)
        GX_SetTevColor(GX_TEVREG1, (GXColor){ st->env[0], st->env[1], st->env[2], st->env[3] });
    if (o == NULL || o->lighting != st->lighting || o->alpha_one != st->alpha_one
        || (st->lighting && (o->num_lights != st->num_lights || memcmp(o->light_dir, st->light_dir, sizeof(st->light_dir))
                             || memcmp(o->light_col, st->light_col, sizeof(st->light_col))
                             || memcmp(o->ambient, st->ambient, sizeof(st->ambient)))))
        tnl_apply_lighting(st);
    if (o == NULL || o->textured != st->textured
        || (st->textured && memcmp(o->texmtx, st->texmtx, sizeof(st->texmtx)) != 0))
        tnl_apply_texmtx(st);
    if (o == NULL || o->fog != st->fog
        || (st->fog && (o->fog_start != st->fog_start || o->fog_end != st->fog_end
                        || memcmp(o->fog_col, st->fog_col, 3) != 0)))
        tnl_apply_fog(st);
    if (o == NULL || o->cull != st->cull)
        GX_SetCullMode(tnl_cull_mode(st->cull));
    tnl_cur = *st;
    tnl_valid = true;

    if (st->textured && texture_storage[texture_units[0]].data != NULL)
        gx_load_tex(0, texture_units[0]);

    if (gBenchOn)
        bench_gpu_class(BENCH_GPU_WORLD);
    if (hr_recording && cur_proj != tnl_hr_proj)
    {
        // A CPU-path draw since the last GPU-path one switched to its own projection
        cur_proj = tnl_hr_proj;
        hr_state_changed();
    }
    if (!hr_recording && !tnl_proj_loaded)
    {
        GX_LoadProjectionMtx(tnl_proj, GX_PERSPECTIVE);
        tnl_proj_loaded = true;
    }
    const f32 want_far_z = st->decal ? (1.0f - decal_bias) : 1.0f;
    if (want_far_z != applied_far_z)
        gx_issue_viewport(want_far_z);
    if (hr_recording)
        hr_seg_drawn = true;

    GX_Begin(GX_TRIANGLES, GX_VTXFMT1, n);
    for (int i = 0; i < n; i++)
    {
        GX_MatrixIndex1x8(mtx[i]);
        GX_Position1x16(idx[i]);
        GX_Normal1x16(idx[i]);
        GX_Color1x16(idx[i]);
        GX_TexCoord1x16(idx[i]);
    }
    GX_End();
}

// ---------------------------------------------------------------------------------------------
// Benchmark overlay: the text is rendered on the CPU into one RGB5A3 texture twice a second and
// drawn every frame as a single quad, so it costs next to nothing per frame

#define OV_TEX_W BENCH_OV_W
#define OV_TEX_H BENCH_OV_H

static uint16_t *ov_tex;
static GXTexObj ov_texobj;

static void gx_bench_draw_overlay(void)
{
    if (!bench_overlay_enabled())
        return;
    bench_push(BENCH_OVERLAY);
    // The frame may have ended with GPU-path draws (opt_gpu_tnl): reset what they change
    if (tnl_active && !hr_enabled)
        gx_tnl_leave();
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, (GXColor){ 0, 0, 0, 255 });
    if (hr_enabled)
        gx_set_fixed_vtx_desc();

    if (ov_tex == NULL) {
        ov_tex = memalign(32, OV_TEX_W * OV_TEX_H * sizeof(uint16_t));
        if (ov_tex == NULL) {
            bench_pop();
            return;
        }
        memset(ov_tex, 0, OV_TEX_W * OV_TEX_H * sizeof(uint16_t));
        GX_InitTexObj(&ov_texobj, ov_tex, OV_TEX_W, OV_TEX_H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    }
    if (bench_overlay_update(ov_tex, OV_TEX_W, OV_TEX_H)) {
        DCFlushRange(ov_tex, OV_TEX_W * OV_TEX_H * sizeof(uint16_t));
        GX_InvalidateTexAll();
    }

    bench_gpu_class(BENCH_GPU_2D);

    GXRModeObj *rmode = gfx_gx_wm_get_rmode();
    // In the tiled modes the overlay sits inside the top left tile, whose origin is the frame's
    const f32 efb_w = hr_enabled ? hr_tile_w : rmode->fbWidth;
    const f32 efb_h = hr_enabled ? hr_tile_h : rmode->efbHeight;
    const bool short_efb = gx_frame_h() < 400;   // 240-line modes: squeeze rows to fit
    const f32 sx = (hr_enabled && gx_frame_h() < 400) ? 2.0f : 1.0f; // 1280x240p pixels are half as wide
    const f32 sy = short_efb ? 0.5f : 1.0f;
    GX_InitTexObjFilterMode(&ov_texobj, short_efb ? GX_LINEAR : GX_NEAR, short_efb ? GX_LINEAR : GX_NEAR);

    const f32 px0 = 24.0f;
    const f32 py0 = 16.0f * sy;
    const f32 px1 = px0 + OV_TEX_W * sx;
    const f32 py1 = py0 + OV_TEX_H * sy;
    const f32 x0 = -1.0f + 2.0f * px0 / efb_w, x1 = -1.0f + 2.0f * px1 / efb_w;
    const f32 y0 = 1.0f - 2.0f * py0 / efb_h, y1 = 1.0f - 2.0f * py1 / efb_h;

    GX_SetViewport(0.0f, 0.0f, efb_w, efb_h, 0.0f, 1.0f);
    GX_SetScissor(0, 0, (u32)efb_w, (u32)efb_h);
    GX_LoadProjectionMtx(gx_ortho_mtx, GX_ORTHOGRAPHIC);

    if (!hr_enabled) {
        GX_InvVtxCache();
        GX_ClearVtxDesc();
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_VTX, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_LoadTexObj(&ov_texobj, GX_TEXMAP0);
    gx_invalidate_loaded_tex();

    const f32 vx[4] = { x0, x1, x1, x0 }, vy[4] = { y0, y0, y1, y1 };
    const f32 vs[4] = { 0.0f, 1.0f, 1.0f, 0.0f }, vt[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    for (int i = 0; i < 4; i++) {
        GX_Position3f32(vx[i], vy[i], 0.0f);
        GX_Color4u8(255, 255, 255, 255);
        if (hr_enabled)
            GX_Color4u8(255, 255, 255, 255);
        GX_TexCoord2f32(vs[i], vt[i]);
    }
    GX_End();

    // Put back what gfx_pc expects to persist into the next frame (1280x240p replays re-record it)
    if (!hr_enabled) {
        GX_SetViewport((f32)vp_x, (f32)vp_y, (f32)vp_w, (f32)vp_h, 0.0f, applied_far_z);
        GX_SetScissor(sc_x, sc_y, sc_w, sc_h);
        if (shader_program_pool_size > 0)
            gfx_gx_load_shader(&shader_program_pool[current_shader]);
        gfx_gx_set_use_alpha(cur_use_alpha);
    }
    bench_pop();
}

static void gx_apply_copy_filter(void)
{
    GXRModeObj *rmode = gfx_gx_wm_get_rmode();
    // Not in 720p: the filter blends neighbouring lines, which would mark the seam between the
    // two rows of tiles (each copy only sees its own tile), and progressive 720p has no flicker
    const bool deflicker = configViDeflicker && !gfx_gx_wm_hd() && !gfx_gx_wm_crop();
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, deflicker ? GX_TRUE : GX_FALSE, rmode->vfilter);
}

// Configure the embedded framebuffer and the EFB->XFB copy
static void gx_setup_efb(void)
{
    GXRModeObj *rmode = gfx_gx_wm_get_rmode();

    GX_SetCopyClear((GXColor){ 0, 0, 0, 0xff }, GX_MAX_Z24);

    vp_x = 0;
    vp_y = 0;
    vp_w = gfx_gx_wm_get_frame_width();
    vp_h = gx_frame_h();
    gx_issue_viewport(1.0f); // plain (0, 1) range
    sc_x = 0;
    sc_y = 0;
    sc_w = gfx_gx_wm_get_frame_width();
    sc_h = gx_frame_h();
    if (hr_enabled)
    {
        // Each tile is copied 1:1 into its place in the XFB
        GX_SetDispCopyYScale(1.0f);
        hr_apply_scissor(sc_x, sc_y, sc_w, sc_h);
        GX_SetDispCopySrc(0, 0, hr_tile_w, hr_tile_h);
        GX_SetDispCopyDst(rmode->fbWidth, hr_tile_h);
    }
    else
    {
        GX_SetDispCopyYScale((f32)rmode->xfbHeight / (f32)rmode->efbHeight);
        GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
        GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
        GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    }
    gx_apply_copy_filter();
    GX_SetFieldMode(rmode->field_rendering, ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));

    if (rmode->aa)
        GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);
    else
        GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

    GX_SetDispCopyGamma(GX_GM_1_0);
}

static void gx_build_projection(void)
{
    const f32 n = GX_NEAR_PLANE;
    const f32 f = configFog ? GX_FAR_PLANE : GX_FAR_PLANE_EXTENDED;

    memset(gx_perspective_mtx, 0, sizeof(gx_perspective_mtx));
    gx_perspective_mtx[0][0] = 1.0f;
    gx_perspective_mtx[1][1] = 1.0f;
    gx_perspective_mtx[2][2] = -n / (f - n);       // near (w=n) -> NDC z -1
    gx_perspective_mtx[2][3] = -(n * f) / (f - n); // far  (w=f) -> NDC z  0
    gx_perspective_mtx[3][2] = -1.0f;
    gx_perspective_mtx[3][3] = 0.0f;

    memset(gx_ortho_mtx, 0, sizeof(gx_ortho_mtx));
    gx_ortho_mtx[0][0] = 1.0f;
    gx_ortho_mtx[1][1] = 1.0f;
    gx_ortho_mtx[2][2] = 1.0f;
    gx_ortho_mtx[2][3] = -1.0f; // z [0,1] -> [-1,0]
    gx_ortho_mtx[3][3] = 1.0f;
    // Depth 16 steps (of 2^24) short of the cleared value: passes LEQUAL only where nothing drew
    memcpy(gx_sky_mtx, gx_ortho_mtx, sizeof(gx_sky_mtx));
    gx_sky_mtx[2][2] = 0.0f;
    gx_sky_mtx[2][3] = -1.0f / (1 << 20);
}

static void gx_init()
{
    // initialise graphics
    GX_Init(gp_fifo, DEFAULT_FIFO_SIZE);
    gpu_heap_init();

    dbglog("gx_init: GX_Init done");
    if (gfx_gx_wm_hires())
    {
        hr_dl = gpu_alloc(HIRES_DL_SIZE);
        dbglog("gx_init: hires display list buffer %p", hr_dl);
        if (hr_dl != NULL)
        {
            // The CPU never touches the list; make sure no stale cache lines get written over it
            DCInvalidateRange(hr_dl, HIRES_DL_SIZE);
            hr_enabled = true;
            gfx_gx_wm_get_tiles(&hr_tiles_x, &hr_tiles_y, &hr_tile_w, &hr_tile_h);
            dbglog("gx_init: tiles %dx%d of %dx%d", hr_tiles_x, hr_tiles_y, hr_tile_w, hr_tile_h);
            if (gfx_gx_wm_crop()) {
                const u32 bytes = (u32)gfx_gx_wm_get_rmode()->fbWidth * hr_tile_h * 2;
                hr_crop_scratch = gpu_alloc(bytes);
                if (hr_crop_scratch != NULL)
                    DCInvalidateRange(hr_crop_scratch, bytes); // GPU-written only
                dbglog("gx_init: crop scratch %p (%u bytes)", hr_crop_scratch, (unsigned)bytes);
            }
            // Keep libogc's register shadows as recorded, rather than rolling them back at the
            // end of every display list segment
            GX_SetMisc(GX_MT_DL_SAVE_CTX, 0);
            gx_set_fixed_vtx_desc();
        }
    }

    gx_setup_efb();

    if (bench_wanted())
        GX_SetDrawSyncCallback(bench_token_callback);

    // other gx setup
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);

    __Mtx ident;
    guMtxIdentity(ident);
    GX_LoadPosMtxImm(ident, GX_PNMTX0);

    gx_build_projection();
}

static void gfx_gx_init(void)
{
    gx_init();
}

static void gfx_gx_on_resize(void)
{
}

static void gfx_gx_start_frame(void)
{
    GX_SetCopyClear((GXColor){ 0, 0, 0, 0xff }, GX_MAX_Z24);

    // Live apply fog config changes
    static bool prev_fog = true;
    static bool projection_initialized = false;
    if (!projection_initialized || configFog != prev_fog) {
        prev_fog = configFog;
        projection_initialized = true;
        gx_build_projection();
    }

    // Apply deflicker filter
    static bool prev_deflicker = true;
    static bool deflicker_initialized = false;
    if (!deflicker_initialized || configViDeflicker != prev_deflicker) {
        prev_deflicker = configViDeflicker;
        deflicker_initialized = true;
        gx_apply_copy_filter();
    }

    if (hr_enabled)
    {
        hr_nseg = 0;
        hr_pos = 0;
        hw_nproj = 0;
        sw_persp_proj = PROJ_PERSP;
        hr_overflow = false;
        hr_recording = true;
        hr_begin_seg();

        // Each tile replays this frame from the state the previous replay left behind, so the
        // frame has to start by setting everything gfx_pc expects to carry over between frames
        // (including undoing whatever GPU-path draws changed)
        tnl_active = false;
        tnl_valid = false;
        GX_SetCullMode(GX_CULL_NONE);
        GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.0f, 1.0f, (GXColor){ 0, 0, 0, 255 });
        gx_set_fixed_vtx_desc();
        if (shader_program_pool_size > 0)
            gfx_gx_load_shader(&shader_program_pool[current_shader]);
        gfx_gx_set_use_alpha(cur_use_alpha);
        GX_SetColorUpdate(GX_TRUE);
    }

    if (gBenchOn)
        bench_frame_start_gpu();

    GX_InvalidateTexAll();
    gx_invalidate_loaded_tex();
    set_z_mode();
}

// Projection for one segment on one tile: x_ndc' = a * x_ndc + b maps the part of the segment's
// viewport (in frame coordinates) that falls inside the tile onto a viewport the size of the
// tile. With more than one row of tiles y is remapped the same way (GX viewports run top down,
// so NDC y = +1 is the viewport's top row vp[1]); with one row the viewport keeps its own y.
static void hr_load_tile_projection(const struct HiresSeg *seg, int tile_x0, int tile_y0)
{
    Mtx44 m;
    const f32 tw = (f32)hr_tile_w, th = (f32)hr_tile_h;
    const f32 a = (f32)seg->vp[2] / tw;
    const f32 b = (2.0f * (f32)(seg->vp[0] - tile_x0) + (f32)seg->vp[2]) / tw - 1.0f;

    memcpy(m, seg->proj == PROJ_ORTHO ? gx_ortho_mtx : seg->proj == PROJ_SKY ? gx_sky_mtx
              : seg->proj == PROJ_PERSP ? gx_perspective_mtx : hw_proj[seg->proj - PROJ_HW], sizeof(m));
    for (int j = 0; j < 4; j++)
        m[0][j] = a * m[0][j] + b * m[3][j];
    if (hr_tiles_y > 1) {
        const f32 ay = (f32)seg->vp[3] / th;
        const f32 by = 1.0f - (2.0f * (f32)(seg->vp[1] - tile_y0) + (f32)seg->vp[3]) / th;
        for (int j = 0; j < 4; j++)
            m[1][j] = ay * m[1][j] + by * m[3][j];
    }
    GX_LoadProjectionMtx(m, (seg->proj == PROJ_ORTHO || seg->proj == PROJ_SKY) ? GX_ORTHOGRAPHIC : GX_PERSPECTIVE);
}

static void hr_replay(void)
{
    GXRModeObj *rmode = gfx_gx_wm_get_rmode();
    void *xfb = gfx_gx_wm_get_back_buffer();

    for (int ty = 0; ty < hr_tiles_y; ty++)
    for (int tx = 0; tx < hr_tiles_x; tx++)
    {
        const int tile_x0 = tx * hr_tile_w, tile_x1 = tile_x0 + hr_tile_w;
        const int tile_y0 = ty * hr_tile_h, tile_y1 = tile_y0 + hr_tile_h;

        const struct HiresSeg *prev = NULL;
        for (int i = 0; i < hr_nseg; i++)
        {
            const struct HiresSeg *seg = &hr_segs[i];
            if (seg->cullable && (seg->bb[2] <= tile_x0 || seg->bb[0] >= tile_x1
                                  || seg->bb[3] <= tile_y0 || seg->bb[1] >= tile_y1))
                continue; // this draw misses the tile
            // Projection, viewport and scissor only change between segments with different ones
            if (prev != NULL && configOptStateCache && prev->proj == seg->proj && prev->far_z == seg->far_z
                && memcmp(prev->vp, seg->vp, sizeof(seg->vp)) == 0 && memcmp(prev->sc, seg->sc, sizeof(seg->sc)) == 0)
            {
                GX_CallDispList(hr_dl + seg->offset, seg->size);
                continue;
            }
            prev = seg;
            int x0 = MAX(seg->sc[0], tile_x0);
            int x1 = MIN(seg->sc[0] + seg->sc[2], tile_x1);

            hr_load_tile_projection(seg, tile_x0, tile_y0);
            if (hr_tiles_y > 1) {
                int y0 = MAX(seg->sc[1], tile_y0);
                int y1 = MIN(seg->sc[1] + seg->sc[3], tile_y1);
                GX_SetViewport(0.0f, 0.0f, (f32)hr_tile_w, (f32)hr_tile_h, 0.0f, seg->far_z);
                GX_SetScissor(x0 - tile_x0, y1 > y0 ? y0 - tile_y0 : 0,
                              x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
            } else {
                GX_SetViewport(0.0f, (f32)seg->vp[1], (f32)hr_tile_w, (f32)seg->vp[3],
                               0.0f, seg->far_z);
                GX_SetScissor(x0 - tile_x0, seg->sc[1], x1 > x0 ? x1 - x0 : 0, seg->sc[3]);
            }
            GX_CallDispList(hr_dl + seg->offset, seg->size);
        }

        if (tx == 0 && ty == 0 && bench_overlay_enabled())
            gx_bench_draw_overlay();

        // The copy's clear only writes Z (and colour) when those updates are enabled, and
        // the frame may have ended with depth writes off
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
        if (gfx_gx_wm_crop()) {
            // Only the top left tile is shown; the rest are copied out all the same
            const bool shown = tx == 0 && ty == 0;
            GX_CopyDisp(shown || hr_crop_scratch == NULL ? xfb : hr_crop_scratch, GX_TRUE);
        } else {
            const uint32_t offset = ((uint32_t)tile_y0 * rmode->fbWidth + (uint32_t)tile_x0) * 2;
            GX_CopyDisp((uint8_t *)xfb + offset, GX_TRUE);
        }
    }

    GX_Flush();
}

static void gfx_gx_end_frame(void)
{
    static unsigned int ef;
    ef++;
    if (hr_enabled && (ef <= 3 || ef % 600 == 0))
        dbglog("end_frame %u: segs=%d bytes=%u overflow=%d", ef, hr_nseg, (unsigned)hr_pos, hr_overflow);
    bool uncapped = gfx_gx_wm_uncapped();

    const bool trace = ef <= 5;
    if (hr_recording)
    {
        if (!hr_overflow)
            hr_end_seg();
        hr_recording = false;
        if (trace)
            dbglog("end_frame %u: replaying %d segs (%u bytes) into %p", ef, hr_nseg, (unsigned)hr_pos,
                   gfx_gx_wm_get_back_buffer());
        if (!hr_overflow)
            hr_replay();
    }
    else if (!uncapped)
    {
        // Presenting at vsync: the window manager copies the frame when it swaps
        if (bench_overlay_enabled())
            gx_bench_draw_overlay();
    }
    else
    {
        // Copy now, so one wait covers the frame and its copy, and the window manager can queue
        // the buffer without waiting for vsync
        if (bench_overlay_enabled())
            gx_bench_draw_overlay();
        bench_gpu_class(BENCH_GPU_COPY);
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE); // the copy's clear only writes Z when Z updates are on
        GX_SetColorUpdate(GX_TRUE);
        GX_CopyDisp(gfx_gx_wm_get_back_buffer(), GX_TRUE);
        GX_Flush();
    }

    if (gBenchOn)
        bench_release_gpu(false);
    bench_push(BENCH_GPUWAIT);
    if (trace)
        dbglog("end_frame %u: waiting for GPU", ef);
    GX_DrawDone();
    if (trace)
        dbglog("end_frame %u: GPU done", ef);
    bench_pop();
    if (gBenchOn)
        bench_frame_end_gpu(bench_tb());
    gx_free_deferred_texture_data();

    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
}

static void gfx_gx_finish_render(void)
{
}

struct GfxRenderingAPI gfx_gx_api = {
    gfx_gx_z_is_from_0_to_1,
    gfx_gx_unload_shader,
    gfx_gx_load_shader,
    gfx_gx_create_and_load_new_shader,
    gfx_gx_lookup_shader,
    gfx_gx_shader_get_info,
    gfx_gx_new_texture,
    gfx_gx_select_texture,
    gfx_gx_upload_texture,
    gfx_gx_set_sampler_parameters,
    gfx_gx_set_depth_test,
    gfx_gx_set_depth_mask,
    gfx_gx_set_zmode_decal,
    gfx_gx_set_viewport,
    gfx_gx_set_scissor,
    gfx_gx_set_use_alpha,
    gfx_gx_draw_triangles,
    gfx_gx_init,
    gfx_gx_on_resize,
    gfx_gx_start_frame,
    gfx_gx_end_frame,
    gfx_gx_finish_render
};

#endif
