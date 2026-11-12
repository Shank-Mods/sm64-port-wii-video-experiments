#ifndef GFX_GX_TNL_H
#define GFX_GX_TNL_H

// GPU vertex path (opt_gpu_tnl): gfx_pc.c hands N64 vertices to the GX transform unit instead of
// transforming, lighting and fogging them on the CPU. Vertices are copied once into per-frame
// arrays (position F32 xyz, texcoord S16 st, colour/normal 4 bytes) and drawn by index, each
// tagged with the GX matrix slot (1..9) holding the modelview it was loaded under.

#include <stdint.h>
#include <stdbool.h>

struct ShaderProgram;

struct GxTnlState {
    struct ShaderProgram *prg; // colour combiner program (gfx_gx.c)
    uint8_t map[2][4];         // CC_* source of each combiner input: [0] rgb, [1] alpha
    uint8_t prim[4], env[4];
    uint8_t textured;
    float texmtx[2][4];        // raw S16 texcoord -> normalised st
    uint8_t lighting;
    uint8_t num_lights;        // directional lights (ambient separate)
    float light_dir[2][3];     // unit vectors, eye space
    uint8_t light_col[2][3];
    uint8_t ambient[3];
    uint8_t alpha_one;         // shade alpha replaced by 255 (fogged draws)
    uint8_t fog;               // hardware fog on
    uint8_t fog_col[3];
    float fog_start, fog_end;  // GX_FOG_ORTHO_LIN range in screen depth
    uint8_t cull;              // 0 none, 1 back, 2 front (N64 sense)
    uint8_t decal;             // decal depth bias
};

#define GX_TNL_MAX_SLOTS 10 // GX position/normal matrix slots; slot 0 stays identity for the CPU path

void gfx_gx_tnl_begin_frame(void *pos, void *tc, void *col);
void gfx_gx_tnl_load_mtx(int slot, const float mv[4][4], float normal_scale);
void gfx_gx_tnl_set_proj(const float p[4][4]); // GX_PERSPECTIVE form, already includes depth range
void gfx_gx_tnl_draw(const struct GxTnlState *st, const uint8_t *mtx, const uint16_t *idx, int n);
// Depth planes (in N64 clip-w units) shared by both paths while the GPU path is on: the N64
// projection's own near and far, so depth and near/far clipping match the N64 on both paths
void gfx_gx_tnl_set_depth(float n, float f);
float gfx_gx_tnl_near(void);
float gfx_gx_tnl_far(void);

#endif
