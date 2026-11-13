#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "../configfile.h"
#include "../bench.h"

#ifdef TARGET_GX
// gfx_gx.c: converts N64 texture data straight into a native GX format (false = not handled)
bool gfx_gx_upload_texture_n64(uint8_t fmt, uint8_t siz, const uint8_t *addr, uint32_t size_bytes,
                               uint32_t line_size_bytes, const uint8_t *palette);
// gfx_gx.c: skybox drawn last (opt_sky_last): pinned to the far plane, depth tested
void gfx_gx_set_sky(bool on);
// gfx_gx.c: CPU-path fog in GX's fog unit (opt_hw_fog): N64 fog factor = f0 + f1 / w
void gfx_gx_set_cpu_fog(bool on, float f0, float f1, const uint8_t rgb[3]);
#endif

// Tagged G_NOOP around the held-back skybox (rendering_graph_node.c, opt_sky_last)
#define SKY_LAST_TAG 0x534B59
static bool sky_last_on;

#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_) * 0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_) * 0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_) * 0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2)

#define RATIO_X (gfx_current_dimensions.width / (2.0f * HALF_SCREEN_WIDTH))
#define RATIO_Y (gfx_current_dimensions.height / (2.0f * HALF_SCREEN_HEIGHT))

#define MAX_BUFFERED 256
#define MAX_LIGHTS 2
#define MAX_VERTICES 64

struct RGBA {
    uint8_t r, g, b, a;
};

struct XYWidthHeight {
    uint16_t x, y, width, height;
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t clip_rej;
#ifdef TARGET_GX
    // GPU vertex path (opt_gpu_tnl)
    uint8_t tnl_flags;   // TNL_HW: in the arena, TNL_SW: x..clip_rej valid, TNL_LIT: loaded lit
    uint8_t tnl_slot;    // GX matrix slot holding its modelview
    uint16_t tnl_idx;    // index in this frame's arrays
    uint32_t tnl_gen;    // slot generation when loaded
    uint32_t tnl_pver;   // projection version when loaded
    Vtx raw;             // for a late CPU transform if a triangle can't take the GPU path
#endif
};

struct TextureHashmapNode {
    struct TextureHashmapNode *next;

    const uint8_t *texture_addr;
    uint8_t fmt, siz;
    uint32_t size_bytes;
    uint32_t line_size_bytes;
    uint32_t texture_hash;
    uint32_t palette_hash;

    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
};
static struct {
    struct TextureHashmapNode *hashmap[1024];
    struct TextureHashmapNode pool[512];
    uint32_t pool_pos;
} gfx_texture_cache;

struct ColorCombiner {
    uint32_t cc_id;
    struct ShaderProgram *prg;
    uint8_t shader_input_mapping[2][4];
};

static struct ColorCombiner color_combiner_pool[64];
static uint8_t color_combiner_pool_size;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    float ps_light[MAX_LIGHTS][4];     // light coeffs / 127, padded for paired-single loads
    float ps_lookat[2][4];
    uint8_t current_num_lights; // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];

    uint8_t saved_opcode;
    uint8_t saved_tile;
    uint16_t saved_uls, saved_ult;
    int32_t saved_lrx, saved_lry, saved_ulx, saved_uly;
} rsp;

static struct RDP {
    const uint8_t *palette;
    struct {
        const uint8_t *addr;
        uint8_t siz;
        uint8_t tile_number;
    } texture_to_load;
    struct {
        const uint8_t *addr;
        uint32_t size_bytes;
    } loaded_texture[2];
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint32_t line_size_bytes;
    } texture_tile;
    bool textures_changed[2];

    uint32_t other_mode_l, other_mode_h;
    uint32_t combine_mode;

    struct RGBA env_color, prim_color, fog_color, fill_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void *z_buf_address;
    void *color_image_address;
} rdp;

static struct RenderingState {
    bool depth_test;
    bool depth_mask;
    bool decal_mode;
    bool alpha_blend;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram *shader_program;
    struct TextureHashmapNode *textures[2];
} rendering_state;

struct GfxDimensions gfx_current_dimensions;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (26 * 3)]; // 3 vertices in a triangle and 26 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI *gfx_wapi;
static struct GfxRenderingAPI *gfx_rapi;

#ifdef TARGET_GX
#include <malloc.h>
#include <ogc/cache.h>
#include "gfx_gx_tnl.h"
#define TNL_HW 1
#define TNL_SW 2
#define TNL_LIT 4
#define TNL_MAX_VTX 32768
#define TNL_MAX_BUF (MAX_BUFFERED * 3)
static float *tnl_pos;            // per frame: position xyz
static int16_t *tnl_tc;           //            texcoord st
static uint8_t *tnl_col;          //            colour rgba / normal xyz + alpha
static uint32_t tnl_n, tnl_stored; // vertices written / written back to memory for the GPU
static bool tnl_on;               // GPU path in use this frame
static uint32_t mv_version = 1, p_version = 1;
static uint32_t mv_checked, p_checked, p_sent;
static bool mv_affine, mv_uniform, p_ok;
static float mv_nscale;
static float p_gx[4][4];
static int tnl_slot;               // slot holding the current modelview (0 = none)
static uint32_t tnl_slot_mvv;
static uint32_t slot_gen[GX_TNL_MAX_SLOTS];
static int next_slot = 1;
static uint8_t tnl_buf_mtx[TNL_MAX_BUF];
static uint16_t tnl_buf_idx[TNL_MAX_BUF];
static int tnl_buf_n;
static struct GxTnlState tnl_st;   // state of the buffered GPU-path triangles
static uint32_t tnl_stat_hw, tnl_stat_sw, tnl_stat_late;
static void tnl_flush(void);
#endif

static uint32_t gfx_hash_bytes(const uint8_t *data, uint32_t size) {
    uint32_t hash = 2166136261U;

    if (data == NULL) {
        return hash;
    }

    uint32_t i = 0;
#ifdef TARGET_GX
    // A word at a time where aligned (FNV-1a over 32-bit words): 4x fewer multiplies. The hash
    // only has to be consistent for a given address, which it is
    if (((uintptr_t)data & 3) == 0) {
        const uint32_t *w = (const uint32_t *)data;
        for (; i + 4 <= size; i += 4) {
            hash ^= *w++;
            hash *= 16777619U;
        }
    }
#endif
    for (; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

#ifdef TARGET_GX
// opt_tex_hash: every texture switch used to hash the texture's bytes again, even when it was
// already cached. Texture data doesn't change while a frame's display list is processed, so a
// hash is remembered for the rest of the frame.
static uint32_t gfx_frame_no = 1;
static struct {
    const uint8_t *addr;
    uint32_t size, frame, hash;
} hash_memo[512];

static uint32_t gfx_hash_cached(const uint8_t *data, uint32_t size) {
    if (!configOptTexHash || data == NULL) {
        return gfx_hash_bytes(data, size);
    }
    uint32_t slot = ((uint32_t)(uintptr_t)data >> 3 ^ size) & 511;
    if (hash_memo[slot].addr == data && hash_memo[slot].size == size && hash_memo[slot].frame == gfx_frame_no) {
        return hash_memo[slot].hash;
    }
    hash_memo[slot].addr = data;
    hash_memo[slot].size = size;
    hash_memo[slot].frame = gfx_frame_no;
    hash_memo[slot].hash = gfx_hash_bytes(data, size);
    return hash_memo[slot].hash;
}
#else
#define gfx_hash_cached gfx_hash_bytes
#endif

#include <time.h>
static unsigned long get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void gfx_flush(void) {
#ifdef TARGET_GX
    if (tnl_buf_n > 0) {
        tnl_flush();
    }
#endif
    if (buf_vbo_len > 0) {
        int num = buf_vbo_num_tris;
        unsigned long t0 = get_time();
        bench_push(BENCH_SUBMIT);
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        bench_pop();
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
        unsigned long t1 = get_time();
        /*if (t1 - t0 > 1000) {
            printf("f: %d %d\n", num, (int)(t1 - t0));
        }*/
    }
}

static struct ShaderProgram *gfx_lookup_or_create_shader_program(uint32_t shader_id) {
    struct ShaderProgram *prg = gfx_rapi->lookup_shader(shader_id);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static void gfx_generate_cc(struct ColorCombiner *comb, uint32_t cc_id) {
    uint8_t c[2][4];
    uint32_t shader_id = (cc_id >> 24) << 24;
    uint8_t shader_input_mapping[2][4] = {{0}};
    for (int i = 0; i < 4; i++) {
        c[0][i] = (cc_id >> (i * 3)) & 7;
        c[1][i] = (cc_id >> (12 + i * 3)) & 7;
    }
    for (int i = 0; i < 2; i++) {
        if (c[i][0] == c[i][1] || c[i][2] == CC_0) {
            c[i][0] = c[i][1] = c[i][2] = 0;
        }
        uint8_t input_number[8] = {0};
        int next_input_number = SHADER_INPUT_1;
        for (int j = 0; j < 4; j++) {
            int val = 0;
            switch (c[i][j]) {
                case CC_0:
                    break;
                case CC_TEXEL0:
                    val = SHADER_TEXEL0;
                    break;
                case CC_TEXEL1:
                    val = SHADER_TEXEL1;
                    break;
                case CC_TEXEL0A:
                    val = SHADER_TEXEL0A;
                    break;
                case CC_PRIM:
                case CC_SHADE:
                case CC_ENV:
                case CC_LOD:
                    if (input_number[c[i][j]] == 0) {
                        shader_input_mapping[i][next_input_number - 1] = c[i][j];
                        input_number[c[i][j]] = next_input_number++;
                    }
                    val = input_number[c[i][j]];
                    break;
            }
            shader_id |= val << (i * 12 + j * 3);
        }
    }
    comb->cc_id = cc_id;
    comb->prg = gfx_lookup_or_create_shader_program(shader_id);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner *gfx_lookup_or_create_color_combiner(uint32_t cc_id) {
    static struct ColorCombiner *prev_combiner;
    if (prev_combiner != NULL && prev_combiner->cc_id == cc_id) {
        return prev_combiner;
    }

    for (size_t i = 0; i < color_combiner_pool_size; i++) {
        if (color_combiner_pool[i].cc_id == cc_id) {
            return prev_combiner = &color_combiner_pool[i];
        }
    }
    gfx_flush();
    struct ColorCombiner *comb = &color_combiner_pool[color_combiner_pool_size++];
    gfx_generate_cc(comb, cc_id);
    return prev_combiner = comb;
}

static bool gfx_texture_cache_lookup(int tile, struct TextureHashmapNode **n, const uint8_t *orig_addr,
                                     uint32_t fmt, uint32_t siz, uint32_t size_bytes,
                                     uint32_t line_size_bytes, uint32_t texture_hash,
                                     uint32_t palette_hash) {
    size_t hash = (uintptr_t)orig_addr;
    hash = (hash >> 5) & 0x3ff;
    struct TextureHashmapNode **node = &gfx_texture_cache.hashmap[hash];
    while (*node != NULL && *node - gfx_texture_cache.pool < (int)gfx_texture_cache.pool_pos) {
        if ((*node)->texture_addr == orig_addr && (*node)->fmt == fmt && (*node)->siz == siz
            && (*node)->size_bytes == size_bytes && (*node)->line_size_bytes == line_size_bytes
            && (*node)->texture_hash == texture_hash && (*node)->palette_hash == palette_hash) {
            gfx_rapi->select_texture(tile, (*node)->texture_id);
            *n = *node;
            return true;
        }
        node = &(*node)->next;
    }
    if (gfx_texture_cache.pool_pos == sizeof(gfx_texture_cache.pool) / sizeof(struct TextureHashmapNode)) {
        // Pool is full. We just invalidate everything and start over.
        gfx_texture_cache.pool_pos = 0;
        memset(gfx_texture_cache.hashmap, 0, sizeof(gfx_texture_cache.hashmap));
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rendering_state.textures[0] = NULL;
        rendering_state.textures[1] = NULL;
        node = &gfx_texture_cache.hashmap[hash];
        //puts("Clearing texture cache");
    }
    *node = &gfx_texture_cache.pool[gfx_texture_cache.pool_pos++];
    if ((*node)->texture_addr == NULL) {
        (*node)->texture_id = gfx_rapi->new_texture();
    }
    gfx_rapi->select_texture(tile, (*node)->texture_id);
    gfx_rapi->set_sampler_parameters(tile, false, 0, 0);
    (*node)->cms = 0;
    (*node)->cmt = 0;
    (*node)->linear_filter = false;
    (*node)->next = NULL;
    (*node)->texture_addr = orig_addr;
    (*node)->fmt = fmt;
    (*node)->siz = siz;
    (*node)->size_bytes = size_bytes;
    (*node)->line_size_bytes = line_size_bytes;
    (*node)->texture_hash = texture_hash;
    (*node)->palette_hash = palette_hash;
    *n = *node;
    return false;
}

static void import_texture_rgba16(int tile) {
    uint8_t rgba32_buf[8192];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint16_t col16 = (rdp.loaded_texture[tile].addr[2 * i] << 8) | rdp.loaded_texture[tile].addr[2 * i + 1];
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile) {
    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = (rdp.loaded_texture[tile].size_bytes / 2) / rdp.texture_tile.line_size_bytes;
    gfx_rapi->upload_texture(rdp.loaded_texture[tile].addr, width, height);
}

static void import_texture_ia4(int tile) {
    uint8_t rgba32_buf[32768];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part >> 1;
        uint8_t alpha = part & 1;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_3_8(r);
        rgba32_buf[4*i + 1] = SCALE_3_8(g);
        rgba32_buf[4*i + 2] = SCALE_3_8(b);
        rgba32_buf[4*i + 3] = alpha ? 255 : 0;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia8(int tile) {
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[i] >> 4;
        uint8_t alpha = rdp.loaded_texture[tile].addr[i] & 0xf;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = SCALE_4_8(alpha);
    }

    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ia16(int tile) {
    uint8_t rgba32_buf[8192];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes / 2; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[2 * i];
        uint8_t alpha = rdp.loaded_texture[tile].addr[2 * i + 1];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = alpha;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes / 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_i4(int tile) {
    uint8_t rgba32_buf[32768];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint8_t intensity = part;
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = SCALE_4_8(r);
        rgba32_buf[4*i + 1] = SCALE_4_8(g);
        rgba32_buf[4*i + 2] = SCALE_4_8(b);
        rgba32_buf[4*i + 3] = 255;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_i8(int tile) {
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t intensity = rdp.loaded_texture[tile].addr[i];
        uint8_t r = intensity;
        uint8_t g = intensity;
        uint8_t b = intensity;
        rgba32_buf[4*i + 0] = r;
        rgba32_buf[4*i + 1] = g;
        rgba32_buf[4*i + 2] = b;
        rgba32_buf[4*i + 3] = 255;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}


static void import_texture_ci4(int tile) {
    uint8_t rgba32_buf[32768];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes * 2; i++) {
        uint8_t byte = rdp.loaded_texture[tile].addr[i / 2];
        uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        uint16_t col16 = (rdp.palette[idx * 2] << 8) | rdp.palette[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes * 2;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_ci8(int tile) {
    uint8_t rgba32_buf[16384];

    for (uint32_t i = 0; i < rdp.loaded_texture[tile].size_bytes; i++) {
        uint8_t idx = rdp.loaded_texture[tile].addr[i];
        uint16_t col16 = (rdp.palette[idx * 2] << 8) | rdp.palette[idx * 2 + 1]; // Big endian load
        uint8_t a = col16 & 1;
        uint8_t r = col16 >> 11;
        uint8_t g = (col16 >> 6) & 0x1f;
        uint8_t b = (col16 >> 1) & 0x1f;
        rgba32_buf[4*i + 0] = SCALE_5_8(r);
        rgba32_buf[4*i + 1] = SCALE_5_8(g);
        rgba32_buf[4*i + 2] = SCALE_5_8(b);
        rgba32_buf[4*i + 3] = a ? 255 : 0;
    }

    uint32_t width = rdp.texture_tile.line_size_bytes;
    uint32_t height = rdp.loaded_texture[tile].size_bytes / rdp.texture_tile.line_size_bytes;

    gfx_rapi->upload_texture(rgba32_buf, width, height);
}

static void import_texture_impl(int tile);

static void import_texture(int tile) {
    bench_push(BENCH_TEXTURE);
    import_texture_impl(tile);
    bench_pop();
}

static void import_texture_impl(int tile) {
    uint8_t fmt = rdp.texture_tile.fmt;
    uint8_t siz = rdp.texture_tile.siz;
    uint32_t size_bytes = rdp.loaded_texture[tile].size_bytes;
    uint32_t line_size_bytes = rdp.texture_tile.line_size_bytes;
    uint32_t texture_hash = gfx_hash_cached(rdp.loaded_texture[tile].addr, size_bytes);
    uint32_t palette_hash = 0;

    if (fmt == G_IM_FMT_CI) {
        uint32_t palette_size = (siz == G_IM_SIZ_4b) ? 16 * 2 : 256 * 2;
        palette_hash = gfx_hash_cached(rdp.palette, palette_size);
    }

    if (gfx_texture_cache_lookup(tile, &rendering_state.textures[tile], rdp.loaded_texture[tile].addr,
                                 fmt, siz, size_bytes, line_size_bytes, texture_hash, palette_hash)) {
        return;
    }

#ifdef TARGET_GX
    // opt_native_tex: straight from N64 data to a native GX format, with no RGBA32 step
    if (configOptNativeTex && gfx_gx_upload_texture_n64(fmt, siz, rdp.loaded_texture[tile].addr, size_bytes,
                                                        line_size_bytes, rdp.palette)) {
        return;
    }
#endif
    int t0 = get_time();
    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile);
        } else {
            abort();
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile);
        } else {
            abort();
        }
    } else {
        abort();
    }
    int t1 = get_time();
    //printf("Time diff: %d\n", t1 - t0);
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t *light, float coeffs[3]) {
    float light_dir[3] = {
        light->dir[0] / 127.0f,
        light->dir[1] / 127.0f,
        light->dir[2] / 127.0f
    };
    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] +
                        a[i][1] * b[1][j] +
                        a[i][2] * b[2][j] +
                        a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t *addr) {
    float matrix[4][4];
#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

#ifdef TARGET_GX
    if (parameters & G_MTX_PROJECTION) {
        p_version++;
        if (tnl_on)
            gfx_flush(); // buffered triangles use the depth planes of the old projection
    } else {
        mv_version++;
    }
#endif
    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
#ifdef TARGET_GX
    mv_version++;
#endif
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x) {
    return x * (4.0f / 3.0f) / gfx_current_dimensions.aspect_ratio;
}

static void gfx_sp_vertex_impl(size_t n_vertices, size_t dest_index, const Vtx *vertices);

#ifdef TARGET_GX
// ---------------------------------------------------------------------------------------------
// GPU vertex path (opt_gpu_tnl)

// The projection in GX_PERSPECTIVE form (columns: x' = p00 ex + p02 ez, y' = p11 ey + p12 ez,
// z' = p22 ez + p23, w' = -ez), or false if the N64 matrix doesn't have that shape (orthographic
// HUD projections, anything unusual: those stay on the CPU path). Depth uses the same range as the
// CPU path, which feeds GX the N64 clip w (= s * -ez), so the two paths' depths agree.
static bool tnl_make_proj(void) {
    const float (*P)[4] = rsp.P_matrix;
    float mx = 0.0f;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (fabsf(P[i][j]) > mx) mx = fabsf(P[i][j]);
    const float eps = mx * 1e-6f;
    if (fabsf(P[1][0]) > eps || fabsf(P[3][0]) > eps || fabsf(P[0][1]) > eps || fabsf(P[3][1]) > eps
        || fabsf(P[0][2]) > eps || fabsf(P[1][2]) > eps
        || fabsf(P[0][3]) > eps || fabsf(P[1][3]) > eps || fabsf(P[3][3]) > eps)
        return false;
    const float s = -P[2][3];
    if (!(s > 0.0f))
        return false;
    // N64 near and far planes in clip-w units: z/w = -1 and +1, with z = A w + B
    const float A = -P[2][2] / s, B = P[3][2];
    const float wn = B / (-1.0f - A), wf = B / (1.0f - A);
    if (!(wn > 0.0f) || !(wf > wn))
        return false;
    gfx_gx_tnl_set_depth(wn, wf); // both paths use them from here on
    const float k = (4.0f / 3.0f) / gfx_current_dimensions.aspect_ratio;
    const float n = gfx_gx_tnl_near(), f = gfx_gx_tnl_far();
    memset(p_gx, 0, sizeof(p_gx));
    p_gx[0][0] = k * P[0][0] / s;
    p_gx[0][2] = k * P[2][0] / s;
    p_gx[1][1] = P[1][1] / s;
    p_gx[1][2] = P[2][1] / s;
    p_gx[2][2] = -n / (f - n);
    p_gx[2][3] = -(n * f) / ((f - n) * s);
    p_gx[3][2] = -1.0f;
    return true;
}

// Projection and depth planes for the current P (both paths' depth follows it)
static void tnl_update_proj(void) {
    if (p_checked != p_version) {
        p_ok = tnl_make_proj();
        p_checked = p_version;
    }
}

// Modelview checks: affine (GX position matrices are 3x4), and for lighting a uniform scale (the
// N64 normalises each light's direction in object space; with a uniform scale that equals
// dividing the normal matrix by the scale)
static void tnl_check_mv(void) {
    const float (*M)[4] = rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1];
    mv_affine = fabsf(M[0][3]) < 1e-6f && fabsf(M[1][3]) < 1e-6f && fabsf(M[2][3]) < 1e-6f
                && fabsf(M[3][3] - 1.0f) < 1e-5f;
    float l[3];
    for (int r = 0; r < 3; r++)
        l[r] = sqrtf(M[r][0] * M[r][0] + M[r][1] * M[r][1] + M[r][2] * M[r][2]);
    const float d01 = M[0][0] * M[1][0] + M[0][1] * M[1][1] + M[0][2] * M[1][2];
    const float d02 = M[0][0] * M[2][0] + M[0][1] * M[2][1] + M[0][2] * M[2][2];
    const float d12 = M[1][0] * M[2][0] + M[1][1] * M[2][1] + M[1][2] * M[2][2];
    const float lm = l[0];
    mv_uniform = lm > 1e-6f && fabsf(l[1] - lm) < lm * 0.01f && fabsf(l[2] - lm) < lm * 0.01f
                 && fabsf(d01) < lm * lm * 0.01f && fabsf(d02) < lm * lm * 0.01f && fabsf(d12) < lm * lm * 0.01f;
    // S8 normals carry 6 fraction bits (n / 64); the N64 uses n / 127
    mv_nscale = mv_uniform ? (64.0f / 127.0f) / lm : 0.0f;
}

// Combiner fields that need the CPU path (TEXEL1, LOD), from the raw combine mode
static bool tnl_cc_needs_cpu(uint32_t cc) {
    for (int i = 0; i < 8; i++) {
        uint32_t v = (cc >> (i * 3)) & 7;
        if (v == CC_TEXEL1 || v == CC_LOD)
            return true;
    }
    return false;
}

// Can this vertex batch go to the GPU path? (Triangles can still fall back individually.)
static bool tnl_batch_ok(void) {
    if (!tnl_on || sky_last_on || rsp.modelview_matrix_stack_size == 0)
        return false;
    tnl_update_proj(); // first: CPU-path batches use its depth planes too
    if (rsp.geometry_mode & G_TEXTURE_GEN)
        return false;
    if (!p_ok)
        return false;
    if (mv_checked != mv_version) {
        tnl_check_mv();
        mv_checked = mv_version;
    }
    if (!mv_affine)
        return false;
    if ((rsp.geometry_mode & G_LIGHTING) && !mv_uniform)
        return false;
    // predicted fallbacks: cheaper to transform on the CPU now than late
    if (tnl_cc_needs_cpu(rdp.combine_mode))
        return false;
    if ((rdp.other_mode_l >> 30) == G_BL_CLR_FOG && !(rsp.geometry_mode & G_FOG))
        return false;
    return true;
}

static void tnl_load_vertices(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    if (tnl_slot == 0 || tnl_slot_mvv != mv_version) {
        // Pending triangles may still use the slot about to be overwritten
        gfx_flush();
        const int slot = next_slot;
        next_slot = next_slot % (GX_TNL_MAX_SLOTS - 1) + 1; // 1..9
        // The normal matrix is loaded whatever the lighting mode: lighting can come on later
        // under the same modelview (0 when the scale isn't uniform; lit batches then don't come here)
        gfx_gx_tnl_load_mtx(slot, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], mv_nscale);
        slot_gen[slot]++;
        tnl_slot = slot;
        tnl_slot_mvv = mv_version;
    }
    const uint8_t lit = (rsp.geometry_mode & G_LIGHTING) ? TNL_LIT : 0;
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];
        const uint32_t k = tnl_n++;
        tnl_pos[3 * k + 0] = v->ob[0];
        tnl_pos[3 * k + 1] = v->ob[1];
        tnl_pos[3 * k + 2] = v->ob[2];
        tnl_tc[2 * k + 0] = v->tc[0];
        tnl_tc[2 * k + 1] = v->tc[1];
        memcpy(&tnl_col[4 * k], v->cn, 4); // colour, or normal xyz + alpha
        d->tnl_flags = TNL_HW | lit;
        d->tnl_slot = (uint8_t)tnl_slot;
        d->tnl_idx = (uint16_t)k;
        d->tnl_gen = slot_gen[tnl_slot];
        d->tnl_pver = p_version;
        d->raw = vertices[i];
    }
}

// A triangle on the GPU path that turned out to need the CPU path: transform its vertices now,
// with the current matrices (in display lists these are the ones they were loaded with)
static void tnl_ensure_sw(struct LoadedVertex *d) {
    if ((d->tnl_flags & (TNL_HW | TNL_SW)) != TNL_HW)
        return;
    const size_t idx = d - rsp.loaded_vertices;
    Vtx tmp = d->raw;
    const uint8_t flags = d->tnl_flags;
    gfx_sp_vertex_impl(1, idx, &tmp);
    d->tnl_flags = flags | TNL_SW;
    tnl_stat_late++;
}

static void tnl_flush(void) {
    const int n = tnl_buf_n;
    tnl_buf_n = 0;
    if (tnl_stored < tnl_n) {
        // The GPU reads the arrays from memory
        DCStoreRange(tnl_pos + 3 * tnl_stored, (tnl_n - tnl_stored) * 12);
        DCStoreRange(tnl_tc + 2 * tnl_stored, (tnl_n - tnl_stored) * 4);
        DCStoreRange(tnl_col + 4 * tnl_stored, (tnl_n - tnl_stored) * 4);
        tnl_stored = tnl_n;
    }
    if (p_sent != p_version) {
        gfx_gx_tnl_set_proj(p_gx);
        p_sent = p_version;
    }
    bench_push(BENCH_SUBMIT);
    gfx_gx_tnl_draw(&tnl_st, tnl_buf_mtx, tnl_buf_idx, n);
    bench_pop();
}

static void tnl_begin_frame(void) {
    tnl_on = configOptGpuTnl;
    if (tnl_on && tnl_pos == NULL) {
        tnl_pos = memalign(32, TNL_MAX_VTX * 12);
        tnl_tc = memalign(32, TNL_MAX_VTX * 4);
        tnl_col = memalign(32, TNL_MAX_VTX * 4);
        if (tnl_pos == NULL || tnl_tc == NULL || tnl_col == NULL) {
            configOptGpuTnl = false;
            tnl_on = false;
            return;
        }
    }
    tnl_n = tnl_stored = 0;
    tnl_buf_n = 0;
    tnl_slot = 0;
    p_checked = p_sent = 0; // aspect may have changed; resend the projection every frame
    for (int i = 0; i < MAX_VERTICES + 4; i++)
        rsp.loaded_vertices[i].tnl_flags = 0;
    if (tnl_on)
        gfx_gx_tnl_begin_frame(tnl_pos, tnl_tc, tnl_col);
    static uint32_t frames;
    if (++frames % 300 == 0) {
        dbglog("gpu tnl: last 300 frames %u gpu tris, %u cpu tris, %u late cpu transforms",
               (unsigned)tnl_stat_hw, (unsigned)tnl_stat_sw, (unsigned)tnl_stat_late);
        tnl_stat_hw = tnl_stat_sw = tnl_stat_late = 0;
    }
}
#endif

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
    bench_push(BENCH_VERTEX);
#ifdef TARGET_GX
    if (tnl_batch_ok() && tnl_n + n_vertices <= TNL_MAX_VTX && dest_index + n_vertices <= MAX_VERTICES) {
        tnl_load_vertices(n_vertices, dest_index, vertices);
        bench_pop();
        return;
    }
#endif
    gfx_sp_vertex_impl(n_vertices, dest_index, vertices);
#ifdef TARGET_GX
    for (size_t i = 0; i < n_vertices && dest_index + i < MAX_VERTICES + 4; i++)
        rsp.loaded_vertices[dest_index + i].tnl_flags = TNL_SW;
#endif
    bench_pop();
}

#ifdef TARGET_GX
// Paired-single helpers (opt_ps_vertex). The transform does two lanes per instruction. Positions
// are already floats in this port (F3DEX2E defines GBI_FLOATS); normals are s8, and the quantized
// load (psq_l through GQR7 = s8) turns them into floats in hardware, where the C version needs a
// trip through memory per component (Broadway has no integer-to-float instruction).
static inline void ps_set_gqrs(void) {
    __asm__ volatile("mtspr 918,%0\n\tmtspr 919,%1" : : "r"(0x00070007u), "r"(0x00060006u));
}

// out[0..3] = (x, y, z, 1) * M for a float position
static inline void ps_xform(const float *ob, const float *m, float *out) {
    __asm__ volatile(
        "psq_l 0,0(%0),0,0\n\t"     // (x, y)
        "psq_l 1,8(%0),1,0\n\t"     // (z, 1)
        "psq_l 2,0(%1),0,0\n\t"     // m00 m01
        "psq_l 3,8(%1),0,0\n\t"     // m02 m03
        "psq_l 4,16(%1),0,0\n\t"    // m10 m11
        "psq_l 5,24(%1),0,0\n\t"    // m12 m13
        "psq_l 6,32(%1),0,0\n\t"    // m20 m21
        "psq_l 7,40(%1),0,0\n\t"    // m22 m23
        "psq_l 8,48(%1),0,0\n\t"    // m30 m31
        "psq_l 9,56(%1),0,0\n\t"    // m32 m33
        "ps_madds0 10,2,0,8\n\t"    // (X, Y)  = row0 * x + row3
        "ps_madds0 11,3,0,9\n\t"    // (Z, W)
        "ps_madds1 10,4,0,10\n\t"   //        + row1 * y
        "ps_madds1 11,5,0,11\n\t"
        "ps_madds0 10,6,1,10\n\t"   //        + row2 * z
        "ps_madds0 11,7,1,11\n\t"
        "psq_st 10,0(%2),0,0\n\t"
        "psq_st 11,8(%2),0,0"
        :
        : "b"(ob), "b"(m), "b"(out)
        : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "memory");
}

// n . c for an s8 normal and c = (c0, c1, c2, 0)
static inline float ps_ndot(const int8_t *n, const float *c) {
    float r;
    __asm__ volatile(
        "psq_l 0,0(%1),0,7\n\t"     // (n0, n1)
        "psq_l 1,2(%1),1,7\n\t"     // (n2, 1)
        "psq_l 2,0(%2),0,0\n\t"     // (c0, c1)
        "psq_l 3,8(%2),0,0\n\t"     // (c2, 0)
        "ps_mul 4,0,2\n\t"
        "ps_madd 5,1,3,4\n\t"       // (n0c0 + n2c2, n1c1)
        "ps_sum0 %0,5,5,5"
        : "=f"(r)
        : "b"(n), "b"(c)
        : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "memory");
    return r;
}
#endif

static void gfx_sp_vertex_impl(size_t n_vertices, size_t dest_index, const Vtx *vertices) {
#ifdef TARGET_GX
    const bool ps = configOptPsVertex;
    if (ps) {
        ps_set_gqrs();
    }
    const float aspect_x = (4.0f / 3.0f) / gfx_current_dimensions.aspect_ratio;
#endif
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx_t *v = &vertices[i].v;
        const Vtx_tn *vn = &vertices[i].n;
        struct LoadedVertex *d = &rsp.loaded_vertices[dest_index];

        float x, y, z, w;
#ifdef TARGET_GX
        if (ps) {
            float o[4] __attribute__((aligned(8)));
            ps_xform(v->ob, &rsp.MP_matrix[0][0], o);
            x = o[0] * aspect_x;
            y = o[1];
            z = o[2];
            w = o[3];
        } else
#endif
        {
        x = v->ob[0] * rsp.MP_matrix[0][0] + v->ob[1] * rsp.MP_matrix[1][0] + v->ob[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        y = v->ob[0] * rsp.MP_matrix[0][1] + v->ob[1] * rsp.MP_matrix[1][1] + v->ob[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        z = v->ob[0] * rsp.MP_matrix[0][2] + v->ob[1] * rsp.MP_matrix[1][2] + v->ob[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        w = v->ob[0] * rsp.MP_matrix[0][3] + v->ob[1] * rsp.MP_matrix[1][3] + v->ob[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];

        x = gfx_adjust_x_for_aspect_ratio(x);
        }

        short U = v->tc[0] * rsp.texture_scaling_factor.s >> 16;
        short V = v->tc[1] * rsp.texture_scaling_factor.t >> 16;

        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                static const Light_t lookat_x = {{0, 0, 0}, 0, {0, 0, 0}, 0, {127, 0, 0}, 0};
                static const Light_t lookat_y = {{0, 0, 0}, 0, {0, 0, 0}, 0, {0, 127, 0}, 0};
                calculate_normal_dir(&lookat_x, rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&lookat_y, rsp.current_lookat_coeffs[1]);
                rsp.lights_changed = false;
#ifdef TARGET_GX
                for (int l = 0; l < rsp.current_num_lights - 1; l++) {
                    for (int k = 0; k < 3; k++)
                        rsp.ps_light[l][k] = rsp.current_lights_coeffs[l][k] / 127.0f;
                    rsp.ps_light[l][3] = 0.0f;
                }
                for (int l = 0; l < 2; l++) {
                    for (int k = 0; k < 3; k++)
                        rsp.ps_lookat[l][k] = rsp.current_lookat_coeffs[l][k] / 127.0f;
                    rsp.ps_lookat[l][3] = 0.0f;
                }
#endif
            }

            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];

            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
#ifdef TARGET_GX
                if (ps) {
                    intensity = ps_ndot(vn->n, rsp.ps_light[i]);
                } else
#endif
                {
                intensity += vn->n[0] * rsp.current_lights_coeffs[i][0];
                intensity += vn->n[1] * rsp.current_lights_coeffs[i][1];
                intensity += vn->n[2] * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                }
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
#ifdef TARGET_GX
                if (ps) {
                    // already divided by 127; the formula below divides again, so scale back
                    dotx = ps_ndot(vn->n, rsp.ps_lookat[0]) * 127.0f;
                    doty = ps_ndot(vn->n, rsp.ps_lookat[1]) * 127.0f;
                } else
#endif
                {
                dotx += vn->n[0] * rsp.current_lookat_coeffs[0][0];
                dotx += vn->n[1] * rsp.current_lookat_coeffs[0][1];
                dotx += vn->n[2] * rsp.current_lookat_coeffs[0][2];
                doty += vn->n[0] * rsp.current_lookat_coeffs[1][0];
                doty += vn->n[1] * rsp.current_lookat_coeffs[1][1];
                doty += vn->n[2] * rsp.current_lookat_coeffs[1][2];
                }

                U = (int32_t)((dotx / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
                V = (int32_t)((doty / 127.0f + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = v->cn[0];
            d->color.g = v->cn[1];
            d->color.b = v->cn[2];
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) d->clip_rej |= 1;
        if (x > w) d->clip_rej |= 2;
        if (y < -w) d->clip_rej |= 4;
        if (y > w) d->clip_rej |= 8;
        if (z < -w) d->clip_rej |= 16;
        if (z > w) d->clip_rej |= 32;

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }

            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = 32767.0f;
            }

            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            if (fog_z < 0) fog_z = 0;
            if (fog_z > 255) fog_z = 255;
            // Fog is disabled by making to blend completely transparently
            if (!configFog) fog_z = 0;
            d->color.a = fog_z; // Use alpha variable to store fog factor
        } else {
            d->color.a = v->cn[3];
        }
    }
}

// Trivial rejection and back-face culling on CPU-transformed vertices
static bool sw_tri_rejected(const struct LoadedVertex *v1, const struct LoadedVertex *v2,
                            const struct LoadedVertex *v3) {
    if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
        // The whole triangle lies outside the visible area
        return true;
    }

    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;

        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }

        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) return true;
                break;
            case G_CULL_BACK:
                if (cross >= 0) return true;
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return true;
        }
    }
    return false;
}

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx) {
    struct LoadedVertex *v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex *v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex *v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex *v_arr[3] = {v1, v2, v3};

#ifdef TARGET_GX
    // GPU path if all three vertices are in this frame's arrays under live matrix slots and the
    // current projection, loaded with the current lighting mode (the GPU clips and culls)
    const uint8_t lit_now = (rsp.geometry_mode & G_LIGHTING) ? TNL_LIT : 0;
    bool hw = tnl_on && (v1->tnl_flags & v2->tnl_flags & v3->tnl_flags & TNL_HW)
              && (v1->tnl_flags & TNL_LIT) == lit_now && (v2->tnl_flags & TNL_LIT) == lit_now
              && (v3->tnl_flags & TNL_LIT) == lit_now
              && v1->tnl_gen == slot_gen[v1->tnl_slot] && v2->tnl_gen == slot_gen[v2->tnl_slot]
              && v3->tnl_gen == slot_gen[v3->tnl_slot]
              && v1->tnl_pver == p_version && v2->tnl_pver == p_version && v3->tnl_pver == p_version;
    if (hw) {
        if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_BOTH)
            return;
    } else {
        tnl_ensure_sw(v1);
        tnl_ensure_sw(v2);
        tnl_ensure_sw(v3);
        if (sw_tri_rejected(v1, v2, v3))
            return;
    }
#else
    if (sw_tri_rejected(v1, v2, v3))
        return;
#endif

    // The held-back skybox only fills what the level left empty: depth tested, never written
    bool depth_test = sky_last_on || (rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER;
    if (depth_test != rendering_state.depth_test) {
        gfx_flush();
        gfx_rapi->set_depth_test(depth_test);
        rendering_state.depth_test = depth_test;
    }

    bool z_upd = !sky_last_on && (rdp.other_mode_l & Z_UPD) == Z_UPD;
    if (z_upd != rendering_state.depth_mask) {
        gfx_flush();
        gfx_rapi->set_depth_mask(z_upd);
        rendering_state.depth_mask = z_upd;
    }

    bool zmode_decal = !sky_last_on && (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;
    if (zmode_decal != rendering_state.decal_mode) {
        gfx_flush();
        gfx_rapi->set_zmode_decal(zmode_decal);
        rendering_state.decal_mode = zmode_decal;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    uint32_t cc_id = rdp.combine_mode;

    bool use_alpha = (rdp.other_mode_l & (G_BL_A_MEM << 18)) == 0;
    bool use_fog = (rdp.other_mode_l >> 30) == G_BL_CLR_FOG;
    bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    bool use_noise = (rdp.other_mode_l & G_AC_DITHER) == G_AC_DITHER;

    if (texture_edge) {
        use_alpha = true;
    }

    if (use_alpha) cc_id |= SHADER_OPT_ALPHA;
    if (use_fog) cc_id |= SHADER_OPT_FOG;
    if (texture_edge) cc_id |= SHADER_OPT_TEXTURE_EDGE;
    if (use_noise) cc_id |= SHADER_OPT_NOISE;

    if (!use_alpha) {
        cc_id &= ~0xfff000;
    }

    struct ColorCombiner *comb = gfx_lookup_or_create_color_combiner(cc_id);
    struct ShaderProgram *prg = comb->prg;
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha);
        rendering_state.alpha_blend = use_alpha;
    }
    uint8_t num_inputs;
    bool used_textures[2];
    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    for (int i = 0; i < 2; i++) {
        if (used_textures[i]) {
            if (rdp.textures_changed[i] || rendering_state.textures[i] == NULL) {
                gfx_flush();
                import_texture(i);
                rdp.textures_changed[i] = false;
            }
            bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
#ifdef TARGET_GX
            // Use Nearest Neighbour instead of Bilinear filtering
            // It looks *BAD*. Don't use it
            if (configForceNearest) {
                linear_filter = false;
            }
#endif
            if (linear_filter != rendering_state.textures[i]->linear_filter || rdp.texture_tile.cms != rendering_state.textures[i]->cms || rdp.texture_tile.cmt != rendering_state.textures[i]->cmt) {
                gfx_flush();
                gfx_rapi->set_sampler_parameters(i, linear_filter, rdp.texture_tile.cms, rdp.texture_tile.cmt);
                rendering_state.textures[i]->linear_filter = linear_filter;
                rendering_state.textures[i]->cms = rdp.texture_tile.cms;
                rendering_state.textures[i]->cmt = rdp.texture_tile.cmt;
            }
        }
    }

    bool use_texture = used_textures[0] || used_textures[1];
    uint32_t tex_width = (rdp.texture_tile.lrs - rdp.texture_tile.uls + 4) / 4;
    uint32_t tex_height = (rdp.texture_tile.lrt - rdp.texture_tile.ult + 4) / 4;

    bool z_is_from_0_to_1 = gfx_rapi->z_is_from_0_to_1();

#ifdef TARGET_GX
    if (hw) {
        bool lod = false;
        for (int j = 0; j < 4; j++)
            lod |= comb->shader_input_mapping[0][j] == CC_LOD || comb->shader_input_mapping[1][j] == CC_LOD;
        if (!used_textures[1] && !lod && !(use_fog && !(rsp.geometry_mode & G_FOG))) {
            struct GxTnlState st;
            memset(&st, 0, sizeof(st));
            st.prg = prg;
            memcpy(st.map, comb->shader_input_mapping, sizeof(st.map));
            memcpy(st.prim, &rdp.prim_color, 4);
            memcpy(st.env, &rdp.env_color, 4);
            st.textured = used_textures[0];
            if (st.textured) {
                // CPU path: u = ((tc * scale >> 16) - uls * 8) / 32 (+ 0.5 when filtered), / width
                const bool linear = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
                const float half = linear ? 0.5f : 0.0f;
                st.texmtx[0][0] = rsp.texture_scaling_factor.s / 65536.0f / 32.0f / tex_width;
                st.texmtx[0][3] = (-(float)rdp.texture_tile.uls / 4.0f + half) / tex_width;
                st.texmtx[1][1] = rsp.texture_scaling_factor.t / 65536.0f / 32.0f / tex_height;
                st.texmtx[1][3] = (-(float)rdp.texture_tile.ult / 4.0f + half) / tex_height;
            }
            st.lighting = lit_now != 0;
            if (st.lighting) {
                int nl = rsp.current_num_lights - 1;
                if (nl < 0) nl = 0;
                if (nl > 2) nl = 2;
                st.num_lights = (uint8_t)nl;
                for (int l = 0; l < nl; l++) {
                    float d[3] = { rsp.current_lights[l].dir[0], rsp.current_lights[l].dir[1],
                                   rsp.current_lights[l].dir[2] };
                    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (len > 0.0f) {
                        for (int k = 0; k < 3; k++)
                            st.light_dir[l][k] = d[k] / len;
                    }
                    memcpy(st.light_col[l], rsp.current_lights[l].col, 3);
                }
                memcpy(st.ambient, rsp.current_lights[nl].col, 3);
            }
            st.alpha_one = use_fog; // shade alpha is 100% for fog
            if (use_fog && configFog) {
                // N64 fog is linear in z/w, and so is GX's screen depth d (both linear in 1/w):
                // fog_z = A + B d, and GX's linear fog runs from d = -A/B to d = -A/B + 255/B
                const float n = gfx_gx_tnl_near(), f = gfx_gx_tnl_far();
                const float sc = -rsp.P_matrix[2][3];
                const float p22 = rsp.P_matrix[2][2], p32 = rsp.P_matrix[3][2];
                const float A = rsp.fog_mul * (-p22 / sc + p32 / n) + rsp.fog_offset;
                const float B = -rsp.fog_mul * p32 * (f - n) / (f * n);
                if (fabsf(B) > 1e-9f) {
                    st.fog = 1;
                    st.fog_start = -A / B;
                    st.fog_end = st.fog_start + 255.0f / B;
                    memcpy(st.fog_col, &rdp.fog_color, 3);
                }
            }
            switch (rsp.geometry_mode & G_CULL_BOTH) {
                case G_CULL_BACK: st.cull = 1; break;
                case G_CULL_FRONT: st.cull = 2; break;
                default: st.cull = 0; break;
            }
            st.decal = (rdp.other_mode_l & ZMODE_DEC) == ZMODE_DEC;

            if (buf_vbo_len > 0)
                gfx_flush(); // keep the draw order
            if (tnl_buf_n > 0 && memcmp(&st, &tnl_st, sizeof(st)) != 0)
                tnl_flush();
            tnl_st = st;
            for (int i = 0; i < 3; i++) {
                tnl_buf_mtx[tnl_buf_n] = (uint8_t)(v_arr[i]->tnl_slot * 3);
                tnl_buf_idx[tnl_buf_n++] = v_arr[i]->tnl_idx;
            }
            tnl_stat_hw++;
            if (tnl_buf_n >= TNL_MAX_BUF)
                tnl_flush();
            return;
        }
        // Not for the GPU path after all
        tnl_ensure_sw(v1);
        tnl_ensure_sw(v2);
        tnl_ensure_sw(v3);
        if (sw_tri_rejected(v1, v2, v3))
            return;
    }
    if (tnl_buf_n > 0)
        tnl_flush(); // keep the draw order
    tnl_stat_sw++;

#ifdef TARGET_GX
    if (configOptHwFog) {
        // N64 fog factor per vertex: z/w * mul + offset, with z/w = -p22/sc + p32/w under a
        // perspective projection, so it's f0 + f1/w (fog in GX's fog unit, see gfx_gx.c)
        static struct { bool on; float f0, f1; uint8_t col[3]; } sent;
        bool on = use_fog && configFog && (rsp.geometry_mode & G_FOG)
                  && rsp.P_matrix[2][3] != 0.0f && rsp.P_matrix[3][3] == 0.0f;
        float f0 = 0.0f, f1 = 0.0f;
        if (on) {
            const float sc = -rsp.P_matrix[2][3];
            f0 = rsp.fog_mul * (-rsp.P_matrix[2][2] / sc) + rsp.fog_offset;
            f1 = rsp.fog_mul * rsp.P_matrix[3][2];
        }
        if (sent.on != on || (on && (sent.f0 != f0 || sent.f1 != f1 || memcmp(sent.col, &rdp.fog_color, 3) != 0))) {
            gfx_flush();
            sent.on = on;
            sent.f0 = f0;
            sent.f1 = f1;
            memcpy(sent.col, &rdp.fog_color, 3);
            gfx_gx_set_cpu_fog(on, f0, f1, sent.col);
        }
    }
#endif

    // GX layout: x y z w, [u v], [fog: packed RGBA8 = fog rgb + factor], one packed RGBA8 per
    // combiner input. Colours stay bytes the whole way (they used to go byte -> float/255 ->
    // *255 -> byte, with a divide per channel), and the texture scale is a multiply.
    {
        const bool linear = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
        const float u_scale = use_texture ? 1.0f / (32.0f * tex_width) : 0.0f;
        const float v_scale = use_texture ? 1.0f / (32.0f * tex_height) : 0.0f;
        const float u_off = rdp.texture_tile.uls * 8 - (linear ? 16.0f : 0.0f); // +0.5 texel
        const float v_off = rdp.texture_tile.ult * 8 - (linear ? 16.0f : 0.0f);
        uint8_t lod = 0;
        bool lod_done = false;
        for (int i = 0; i < 3; i++) {
            float z = v_arr[i]->z, w = v_arr[i]->w;
            z = (z + w) * 0.5f;
            buf_vbo[buf_vbo_len++] = v_arr[i]->x;
            buf_vbo[buf_vbo_len++] = v_arr[i]->y;
            buf_vbo[buf_vbo_len++] = z;
            buf_vbo[buf_vbo_len++] = w;
            if (use_texture) {
                buf_vbo[buf_vbo_len++] = (v_arr[i]->u - u_off) * u_scale;
                buf_vbo[buf_vbo_len++] = (v_arr[i]->v - v_off) * v_scale;
            }
            if (use_fog) {
                uint32_t f = ((uint32_t)rdp.fog_color.r << 24) | ((uint32_t)rdp.fog_color.g << 16)
                           | ((uint32_t)rdp.fog_color.b << 8) | v_arr[i]->color.a;
                memcpy(&buf_vbo[buf_vbo_len++], &f, 4); // bits, never through the FPU
            }
            for (int j = 0; j < num_inputs; j++) {
                const struct RGBA *c;
                struct RGBA tmp;
                uint8_t r = 0, g = 0, b = 0, a = 255;
                for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                    switch (comb->shader_input_mapping[k][j]) {
                        case CC_PRIM: c = &rdp.prim_color; break;
                        case CC_SHADE: c = &v_arr[i]->color; break;
                        case CC_ENV: c = &rdp.env_color; break;
                        case CC_LOD:
                            if (!lod_done) {
                                float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                                if (distance_frac < 0.0f) distance_frac = 0.0f;
                                if (distance_frac > 1.0f) distance_frac = 1.0f;
                                lod = (uint8_t)(distance_frac * 255.0f);
                                lod_done = true;
                            }
                            tmp.r = tmp.g = tmp.b = tmp.a = lod;
                            c = &tmp;
                            break;
                        default:
                            tmp.r = tmp.g = tmp.b = tmp.a = 0;
                            c = &tmp;
                            break;
                    }
                    if (k == 0) {
                        r = c->r;
                        g = c->g;
                        b = c->b;
                    } else {
                        a = (use_fog && c == &v_arr[i]->color) ? 255 : c->a; // shade alpha is 100% for fog
                    }
                }
                uint32_t packed = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
                memcpy(&buf_vbo[buf_vbo_len++], &packed, 4);
            }
        }
        (void)z_is_from_0_to_1;
        if (++buf_vbo_num_tris == MAX_BUFFERED) {
            gfx_flush();
        }
        return;
    }
#endif

    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }
        buf_vbo[buf_vbo_len++] = v_arr[i]->x;
        buf_vbo[buf_vbo_len++] = v_arr[i]->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;

        if (use_texture) {
            float u = (v_arr[i]->u - rdp.texture_tile.uls * 8) / 32.0f;
            float v = (v_arr[i]->v - rdp.texture_tile.ult * 8) / 32.0f;
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                u += 0.5f;
                v += 0.5f;
            }
            buf_vbo[buf_vbo_len++] = u / tex_width;
            buf_vbo[buf_vbo_len++] = v / tex_height;
        }

        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = v_arr[i]->color.a / 255.0f; // fog factor (not alpha)
        }

        for (int j = 0; j < num_inputs; j++) {
            struct RGBA *color;
            struct RGBA tmp;
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                    case CC_PRIM:
                        color = &rdp.prim_color;
                        break;
                    case CC_SHADE:
                        color = &v_arr[i]->color;
                        break;
                    case CC_ENV:
                        color = &rdp.env_color;
                        break;
                    case CC_LOD:
                    {
                        float distance_frac = (v1->w - 3000.0f) / 3000.0f;
                        if (distance_frac < 0.0f) distance_frac = 0.0f;
                        if (distance_frac > 1.0f) distance_frac = 1.0f;
                        tmp.r = tmp.g = tmp.b = tmp.a = distance_frac * 255.0f;
                        color = &tmp;
                        break;
                    }
                    default:
                        memset(&tmp, 0, sizeof(tmp));
                        color = &tmp;
                        break;
                }
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = color->r / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->g / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->b / 255.0f;
                } else {
                    if (use_fog && color == &v_arr[i]->color) {
                        // Shade alpha is 100% for fog
                        buf_vbo[buf_vbo_len++] = 1.0f;
                    } else {
                        buf_vbo[buf_vbo_len++] = color->a / 255.0f;
                    }
                }
            }
        }
        /*struct RGBA *color = &v_arr[i]->color;
        buf_vbo[buf_vbo_len++] = color->r / 255.0f;
        buf_vbo[buf_vbo_len++] = color->g / 255.0f;
        buf_vbo[buf_vbo_len++] = color->b / 255.0f;
        buf_vbo[buf_vbo_len++] = color->a / 255.0f;*/
    }
    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static void gfx_calc_and_set_viewport(const Vp_t *viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = SCREEN_HEIGHT - ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    width *= RATIO_X;
    height *= RATIO_Y;
    x *= RATIO_X;
    y *= RATIO_Y;

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t *) data);
            break;
#if 0
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            memcpy(rsp.current_lookat + (index - G_MV_LOOKATY) / 2, data, sizeof(Light_t));
            //rsp.lights_changed = 1;
            break;
#endif
#ifdef F3DEX_GBI_2
        case G_MV_LIGHT: {
            int lightidx = offset / 24 - 2;
            if (lightidx >= 0 && lightidx <= MAX_LIGHTS) { // skip lookat
                // NOTE: reads out of bounds if it is an ambient light
                memcpy(rsp.current_lights + lightidx, data, sizeof(Light_t));
            }
            break;
        }
#else
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
#endif
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uint32_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
#ifdef F3DEX_GBI_2
            rsp.current_num_lights = data / 24 + 1; // add ambient light
#else
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
#endif
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f * RATIO_X;
    float y = (SCREEN_HEIGHT - lry / 4.0f) * RATIO_Y;
    float width = (lrx - ulx) / 4.0f * RATIO_X;
    float height = (lry - uly) / 4.0f * RATIO_Y;

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, const void* addr) {
    rdp.texture_to_load.addr = addr;
    rdp.texture_to_load.siz = size;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette, uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks, uint32_t shifts) {
    if (tile == G_TX_RENDERTILE) {
        SUPPORT_CHECK(palette == 0); // palette should set upper 4 bits of color index in 4b mode
        rdp.texture_tile.fmt = fmt;
        rdp.texture_tile.siz = siz;
        rdp.texture_tile.cms = cms;
        rdp.texture_tile.cmt = cmt;
        rdp.texture_tile.line_size_bytes = line * 8;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }

    if (tile == G_TX_LOADTILE) {
        rdp.texture_to_load.tile_number = tmem / 256;
    }
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    if (tile == G_TX_RENDERTILE) {
        rdp.texture_tile.uls = uls;
        rdp.texture_tile.ult = ult;
        rdp.texture_tile.lrs = lrs;
        rdp.texture_tile.lrt = lrt;
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t high_index) {
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    rdp.palette = rdp.texture_to_load.addr;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    if (tile == 1) return;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0; // Or -1? It's unused in SM64 anyway.
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }
    uint32_t size_bytes = (lrs + 1) << word_size_shift;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;
    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;

    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    if (tile == 1) return;
    SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    uint32_t word_size_shift;
    switch (rdp.texture_to_load.siz) {
        case G_IM_SIZ_4b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_8b:
            word_size_shift = 0;
            break;
        case G_IM_SIZ_16b:
            word_size_shift = 1;
            break;
        case G_IM_SIZ_32b:
            word_size_shift = 2;
            break;
    }

    uint32_t size_bytes = (((lrs >> G_TEXTURE_IMAGE_FRAC) + 1) * ((lrt >> G_TEXTURE_IMAGE_FRAC) + 1)) << word_size_shift;
    rdp.loaded_texture[rdp.texture_to_load.tile_number].size_bytes = size_bytes;

    assert(size_bytes <= 4096 && "bug: too big texture");
    rdp.loaded_texture[rdp.texture_to_load.tile_number].addr = rdp.texture_to_load.addr;
    rdp.texture_tile.uls = uls;
    rdp.texture_tile.ult = ult;
    rdp.texture_tile.lrs = lrs;
    rdp.texture_tile.lrt = lrt;

    rdp.textures_changed[rdp.texture_to_load.tile_number] = true;
}


static uint8_t color_comb_component(uint32_t v) {
    switch (v) {
        case G_CCMUX_TEXEL0:
            return CC_TEXEL0;
        case G_CCMUX_TEXEL1:
            return CC_TEXEL1;
        case G_CCMUX_PRIMITIVE:
            return CC_PRIM;
        case G_CCMUX_SHADE:
            return CC_SHADE;
        case G_CCMUX_ENVIRONMENT:
            return CC_ENV;
        case G_CCMUX_TEXEL0_ALPHA:
            return CC_TEXEL0A;
        case G_CCMUX_LOD_FRACTION:
            return CC_LOD;
        default:
            return CC_0;
    }
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return color_comb_component(a) |
           (color_comb_component(b) << 3) |
           (color_comb_component(c) << 6) |
           (color_comb_component(d) << 9);
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha) {
    rdp.combine_mode = rgb | (alpha << 12);
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;

    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = {0, 0, gfx_current_dimensions.width, gfx_current_dimensions.height};
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;

    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3);

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint32_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), color_comb(0, 0, 0, G_ACMUX_TEXEL0));

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5
    if (flip) {
        dsdx = -dsdx;
        dtdy = -dtdy;
    }
    int16_t width = !flip ? lrx - ulx : lry - uly;
    int16_t height = !flip ? lry - uly : lrx - ulx;
    float lrs = ((uls << 7) + dsdx * width) >> 7;
    float lrt = ((ult << 7) + dtdy * height) >> 7;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }

    uint32_t saved_combine_mode = rdp.combine_mode;
    gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), color_comb(0, 0, 0, G_ACMUX_SHADE));
    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void *z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
}

static inline void *seg_addr(uintptr_t w1) {
    return (void *) w1;
}

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

static void gfx_run_dl(Gfx* cmd) {
    int dummy = 0;
    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;

        switch (opcode) {
            case G_NOOP:
                if ((cmd->words.w0 & 0xFFFFFF) == SKY_LAST_TAG) {
                    gfx_flush();
                    sky_last_on = cmd->words.w1 != 0;
#ifdef TARGET_GX
                    gfx_gx_set_sky(sky_last_on);
#endif
                }
                break;
            // RSP commands:
            case G_MTX:
#ifdef F3DEX_GBI_2
                gfx_sp_matrix(C0(0, 8) ^ G_MTX_PUSH, (const int32_t *) seg_addr(cmd->words.w1));
#else
                gfx_sp_matrix(C0(16, 8), (const int32_t *) seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_POPMTX:
#ifdef F3DEX_GBI_2
                gfx_sp_pop_matrix(cmd->words.w1 / 64);
#else
                gfx_sp_pop_matrix(1);
#endif
                break;
            case G_MOVEMEM:
#ifdef F3DEX_GBI_2
                gfx_sp_movemem(C0(0, 8), C0(8, 8) * 8, seg_addr(cmd->words.w1));
#else
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
#endif
                break;
            case (uint8_t)G_MOVEWORD:
#ifdef F3DEX_GBI_2
                gfx_sp_moveword(C0(16, 8), C0(0, 16), cmd->words.w1);
#else
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_TEXTURE:
#ifdef F3DEX_GBI_2
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(1, 7));
#else
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
#endif
                break;
            case G_VTX:
#ifdef F3DEX_GBI_2
                gfx_sp_vertex(C0(12, 8), C0(1, 7) - C0(12, 8), seg_addr(cmd->words.w1));
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_vertex(C0(10, 6), C0(16, 8) / 2, seg_addr(cmd->words.w1));
#else
                gfx_sp_vertex((C0(0, 16)) / sizeof(Vtx), C0(16, 4), seg_addr(cmd->words.w1));
#endif
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    gfx_run_dl((Gfx *)seg_addr(cmd->words.w1));
                } else {
                    cmd = (Gfx *)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
#ifdef F3DEX_GBI_2
            case G_GEOMETRYMODE:
                gfx_sp_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
#else
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
#endif
            case (uint8_t)G_TRI1:
#ifdef F3DEX_GBI_2
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
#elif defined(F3DEX_GBI) || defined(F3DLP_GBI)
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
#else
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10);
#endif
                break;
#if defined(F3DEX_GBI) || defined(F3DLP_GBI)
            case (uint8_t)G_TRI2:
                gfx_sp_tri1(C0(16, 8) / 2, C0(8, 8) / 2, C0(0, 8) / 2);
                gfx_sp_tri1(C1(16, 8) / 2, C1(8, 8) / 2, C1(0, 8) / 2);
                break;
#endif
            case (uint8_t)G_SETOTHERMODE_L:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(31 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, cmd->words.w1);
#else
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
#endif
                break;
            case (uint8_t)G_SETOTHERMODE_H:
#ifdef F3DEX_GBI_2
                gfx_sp_set_other_mode(63 - C0(8, 8) - C0(0, 8), C0(0, 8) + 1, (uint64_t) cmd->words.w1 << 32);
#else
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t) cmd->words.w1 << 32);
#endif
                break;
#ifdef F3D_OLD
            case (uint8_t)G_RDPHALF_2:
#else
            case (uint8_t)G_RDPHALF_1:
#endif
                switch (rsp.saved_opcode) {
                    case G_TEXRECT:
                    case G_TEXRECTFLIP:
#ifdef F3DEX_GBI_2E
                        rsp.saved_ulx = (int32_t)(C0(0, 24) << 8) >> 8;
#endif
                        rsp.saved_uls = (uint16_t)C1(16, 16);
                        rsp.saved_ult = (uint16_t)C1(0, 16);
                        break;
#ifdef F3DEX_GBI_2E
                    case G_FILLRECT:
                    {
                        int32_t ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                        int32_t uly = (int32_t)(C1(0, 24) << 8) >> 8;
                        gfx_dp_fill_rectangle(ulx, uly, rsp.saved_lrx, rsp.saved_lry);
                        rsp.saved_opcode = G_NOOP;
                        break;
                    }
#endif
                }
                break;
#ifdef F3D_OLD
            case (uint8_t)G_RDPHALF_CONT:
#else
            case (uint8_t)G_RDPHALF_2:
#endif
                switch (rsp.saved_opcode) {
                    case G_TEXRECT:
                    case G_TEXRECTFLIP:
                    {
                        uint8_t tile = rsp.saved_tile;
                        int32_t ulx = rsp.saved_ulx, lrx = rsp.saved_lrx, lry = rsp.saved_lry;
                        uint16_t uls = rsp.saved_uls, ult = rsp.saved_ult;
#ifdef F3DEX_GBI_2E
                        int32_t uly = (int32_t)(C0(0, 24) << 8) >> 8;
#else
                        int32_t uly = rsp.saved_uly;
#endif
                        uint16_t dsdx = (uint16_t)C1(16, 16);
                        uint16_t dtdy = (uint16_t)C1(0, 16);
                        gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, rsp.saved_opcode == G_TEXRECTFLIP);
                        rsp.saved_opcode = G_NOOP;
                        break;
                    }
                }

            // RDP Commands:
            case G_SETTIMG:
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), seg_addr(cmd->words.w1));
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4), C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C1(14, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(
                    color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                    color_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)));
                    /*color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                    color_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));*/
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_TEXRECT:
            case G_TEXRECTFLIP:
            {
                rsp.saved_opcode = opcode;
#ifdef F3DEX_GBI_2E
                rsp.saved_lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                rsp.saved_lry = (int32_t)(C1(0, 24) << 8) >> 8;
                rsp.saved_tile = (int32_t)C1(24, 3);
#else
                rsp.saved_lrx = C0(12, 12);
                rsp.saved_lry = C0(0, 12);
                rsp.saved_tile = C1(24, 3);
                rsp.saved_ulx = C1(12, 12);
                rsp.saved_uly = C1(0, 12);
#endif
                break;
            }
            case G_FILLRECT:
#ifdef F3DEX_GBI_2E
            {
                rsp.saved_opcode = G_FILLRECT;
                rsp.saved_lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                rsp.saved_lry = (int32_t)(C1(0, 24) << 8) >> 8;
                break;
            }
#else
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
#endif
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

void gfx_get_dimensions(uint32_t *width, uint32_t *height) {
    gfx_wapi->get_dimensions(width, height);
}

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *game_name, bool start_in_fullscreen) {
    gfx_wapi = wapi;
    gfx_rapi = rapi;
    gfx_wapi->init(game_name, start_in_fullscreen);
    gfx_rapi->init();

    // Used in the 120 star TAS
    static uint32_t precomp_shaders[] = {
        0x01200200,
        0x00000045,
        0x00000200,
        0x01200a00,
        0x00000a00,
        0x01a00045,
        0x00000551,
        0x01045045,
        0x05a00a00,
        0x01200045,
        0x05045045,
        0x01045a00,
        0x01a00a00,
        0x0000038d,
        0x01081081,
        0x0120038d,
        0x03200045,
        0x03200a00,
        0x01a00a6f,
        0x01141045,
        0x07a00a00,
        0x05200200,
        0x03200200,
        0x09200200,
        0x0920038d,
        0x09200045
    };
    for (size_t i = 0; i < sizeof(precomp_shaders) / sizeof(uint32_t); i++) {
        gfx_lookup_or_create_shader_program(precomp_shaders[i]);
    }
}

struct GfxRenderingAPI *gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

void gfx_start_frame(void) {
#ifdef TARGET_GX
    gfx_frame_no++;
#endif
    gfx_wapi->handle_events();
    gfx_wapi->get_dimensions(&gfx_current_dimensions.width, &gfx_current_dimensions.height);
    if (gfx_current_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_dimensions.height = 1;
    }
    gfx_current_dimensions.aspect_ratio = (float)gfx_current_dimensions.width / (float)gfx_current_dimensions.height;
#ifdef TARGET_GX
    gfx_current_dimensions.aspect_ratio = configWidescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);
    {
        // Square-pixel widescreen frames that aren't 16:9 (1920x1020p) use their real shape
        extern float gfx_gx_wm_frame_aspect(void);
        float fa = gfx_gx_wm_frame_aspect();
        // An aspect set with video_aspect (or a custom mode's) applies whatever the widescreen
        // setting, so 5:4 or 16:10 come out right too
        if (fa > 0.0f && (configWidescreen || configVideoMode == VIDEO_MODE_CUSTOM
                          || configVideoAspect != VIDEO_ASPECT_AUTO))
            gfx_current_dimensions.aspect_ratio = fa;
    }
#endif
}

void gfx_run(Gfx *commands) {
    gfx_sp_reset();

    //puts("New frame");

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;

    double t0 = gfx_wapi->get_time();
    bench_push(BENCH_DLIST);
    gfx_rapi->start_frame();
    sky_last_on = false;
#ifdef TARGET_GX
    gfx_gx_set_sky(false);
    tnl_begin_frame();
#endif
    gfx_run_dl(commands);
    gfx_flush();
    bench_pop();
    double t1 = gfx_wapi->get_time();
    //printf("Process %f %f\n", t1, t1 - t0);
    gfx_rapi->end_frame();
    bench_push(BENCH_PRESENT);
    gfx_wapi->swap_buffers_begin();
    bench_pop();
}

void gfx_end_frame(void) {
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
}
