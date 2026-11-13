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

// True when the frame is rendered in EFB tiles (1280x240p or 1280x720p)
bool gfx_gx_wm_hires(void);

// True when a 720p mode (1280x720p or 960x720p) is in use
bool gfx_gx_wm_hd(void);

// True when a high resolution frame is rendered and its top left shown as normal 480p/480i
// (or as 640x960i for the interlaced modes)
bool gfx_gx_wm_crop(void);
// Crop modes: lines of each output buffer that are shown
int gfx_gx_wm_crop_lines(void);

// Width of the whole rendered frame in pixels
int gfx_gx_wm_get_frame_width(void);

// Name of the 720p mode in use ("1280x720p" / "960x720p"), or NULL
const char *gfx_gx_wm_hd_name(void);

// Tile grid of the tiled modes: 2x1 tiles of 640x240 in 1280x240p, 2x2 of 640x360 in
// 1280x720p, 2x2 of 480x360 in 960x720p, 2x2 of 640x480 in the 1280x960 crop mode
void gfx_gx_wm_get_tiles(int *tiles_x, int *tiles_y, int *tile_w, int *tile_h);

// Height of the whole rendered frame in pixels (720 in 720p, otherwise the EFB height)
int gfx_gx_wm_get_frame_height(void);

// Puts the stock VI clock and the safe video mode back (no-op unless 720p is up)
void gfx_gx_wm_shutdown(void);

// Exact field rate of the selected video mode in Hz
double gfx_gx_wm_get_field_hz(void);

// The framebuffer that the next swap will display
void *gfx_gx_wm_get_back_buffer(void);

// Field rendering (interlaced modes with interlace_render field): each image is one field
bool gfx_gx_wm_field_mode(void);
// Render passes this image (2 when a frame's two fields are drawn from one image), the
// buffer each goes to, and its vertical offset in rendered lines (the half-line of interlace)
int gfx_gx_wm_pass_count(void);
void *gfx_gx_wm_get_pass_buffer(int k);
float gfx_gx_wm_pass_shift(int k);

// Display aspect ratio of the high resolution modes (0 in the console's own modes)
float gfx_gx_wm_frame_aspect(void);

// True when frames are presented without waiting for vsync (benchmark mode)
bool gfx_gx_wm_uncapped(void);

// Field rate of the selected video mode in Hz.
int gfx_gx_wm_get_field_rate(void);

#endif
