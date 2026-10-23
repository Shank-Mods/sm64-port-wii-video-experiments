#ifndef GFX_GX_WM_H
#define GFX_GX_WM_H

#define Mtx __Mtx
#define Vtx __Vtx
#define guPerspectiveF GXguPerspectiveF
#define guPerspective GXguPerspective
#define guOrtho GXguOrtho
#define guOrthoF GXguOrthoF
#include <gccore.h>
#undef Mtx
#undef Vtx
#undef guPerspectiveF
#undef guPerspective
#undef guOrtho
#undef guOrthoF

#include "gfx_window_manager_api.h"

extern struct GfxWindowManagerAPI gfx_gx_wm_api;

GXRModeObj *gfx_gx_wm_get_rmode(void);

// Width of the XFB in 1280x240p mode; the EFB is rendered as tiles of HIRES_TILE_WIDTH
#define HIRES_XFB_WIDTH 1280
#define HIRES_TILE_WIDTH 640

// True when the 1280x240p mode is in use
bool gfx_gx_wm_hires(void);

// The framebuffer that the next swap will display
void *gfx_gx_wm_get_back_buffer(void);

// Field rate of the selected video mode in Hz.
int gfx_gx_wm_get_field_rate(void);

#endif
