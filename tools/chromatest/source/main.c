// chromatest: a chroma-subsampling torture test for the Wii/GameCube digital video output.
//
// The console's external framebuffer and its digital video bus are YUV 4:2:2, so every pair of
// horizontal pixels shares one Cb/Cr sample. This tool shows test patterns designed to make that
// subsampling obvious, and lets you switch between standard modes and "brute force double
// horizontal" modes where each source pixel is duplicated into a full 4:2:2 pair, so an FPGA on
// the output could reconstruct 4:4:4.
//
// Modes (D-pad left/right):
//   0  240P STD   640x240 internal, 640 wide on the bus
//   1  480I STD   640x480 internal, 640 wide on the bus
//   2  480P STD   640x480 internal, 640 wide on the bus (needs component/digital)
//   3  240P X2    640x240 internal, 1280 wide on the bus (each pixel doubled)
//   4  480I X2    640x480 internal, 1280 wide on the bus (each pixel doubled)
//
// 480P X2 (1440-wide 480p) would need a ~108 MHz pixel clock, i.e. reprogramming the VI/PLL
// clock. It is intentionally not included here.
//
// Patterns (D-pad up/down): see the names drawn top-left and the README.
//
// HOME (Wii remote) or START (GameCube pad) exits to the loader.

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogc/machine/processor.h>

#define VI ((volatile u16 *)0xCC002000)

#define TILE_W 640                 // EFB width per tile
#define MAX_XFB_W 1280
#define MAX_XFB_H 480

static void *xfb;                  // single buffer, large enough for any mode
static GXRModeObj rmode;           // current mode (a copy we can edit)
static u8 gpfifo[256 * 1024] __attribute__((aligned(32)));

static int mode_idx = 0;           // resolution mode
static int pat_idx = 0;            // pattern

#define NUM_MODES 5
#define NUM_PATS 6

// ------------------------------------------------------------------ video modes

// Horizontal timing for a 1280-wide active line at 54 MHz, from a register dump of the mode
// working on hardware. Same values the SM64 1280x240p mode uses.
static void poke_hires_horizontal(void)
{
    u32 level;
    _CPU_ISR_Disable(level);
    VI[0x6C / 2] = 0x0001; // VICLK = 54 MHz
    VI[0x04 / 2] = 0x4769; // HTR0 hcs/hce
    VI[0x06 / 2] = 0x035A; // HTR0 half-line width 858
    VI[0x08 / 2] = 0x059A; // HTR1
    VI[0x0A / 2] = 0x93FF;
    VI[0x4A / 2] = 0x0100; // horizontal scaler off
    VI[0x48 / 2] = 0x5050; // PICCONF: 1280 stride/fetch
    _CPU_ISR_Restore(level);
}

static bool have_component;

// Fills rmode for the current mode_idx and returns whether it is a doubled (X2) mode.
static bool build_mode(int idx)
{
    bool doubled = false;
    switch (idx) {
        case 0: memcpy(&rmode, &TVNtsc240Ds, sizeof(rmode)); break;
        case 1: memcpy(&rmode, &TVNtsc480IntDf, sizeof(rmode)); break;
        case 2: memcpy(&rmode, have_component ? &TVNtsc480Prog : &TVNtsc480IntDf, sizeof(rmode)); break;
        case 3: memcpy(&rmode, &TVNtsc240Ds, sizeof(rmode)); rmode.fbWidth = MAX_XFB_W; doubled = true; break;
        case 4: memcpy(&rmode, &TVNtsc480IntDf, sizeof(rmode)); rmode.fbWidth = MAX_XFB_W; doubled = true; break;
        default: memcpy(&rmode, &TVNtsc240Ds, sizeof(rmode)); break;
    }
    return doubled;
}

static bool cur_doubled;

static void apply_mode(int idx)
{
    cur_doubled = build_mode(idx);

    VIDEO_SetBlack(true);
    VIDEO_Configure(&rmode);
    if (cur_doubled)
        poke_hires_horizontal();

    // clear the whole buffer to black (YUV) so unused area is clean
    u32 *p = (u32 *)xfb;
    for (int i = 0; i < MAX_XFB_W * MAX_XFB_H / 2; i++)
        p[i] = 0x00800080;

    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    VIDEO_WaitVSync();
    if (cur_doubled)
        poke_hires_horizontal();
    VIDEO_SetBlack(false);
    VIDEO_Flush();
}

// ------------------------------------------------------------------ GX helpers

static void gx_init(void)
{
    GXColor bg = { 0, 0, 0, 0xff };
    GX_Init(gpfifo, sizeof(gpfifo));
    GX_SetCopyClear(bg, GX_MAX_Z24);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetNumChans(1);
    GX_SetNumTexGens(0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
}

// Set up EFB viewport/scissor/projection for a 640-wide tile of the given height.
static void gx_begin_tile(int efbHeight)
{
    Mtx44 ortho;
    Mtx mv;
    GX_SetViewport(0, 0, TILE_W, efbHeight, 0, 1);
    GX_SetScissor(0, 0, TILE_W, efbHeight);
    guOrtho(ortho, 0, efbHeight, 0, TILE_W, 0, 300);
    GX_LoadProjectionMtx(ortho, GX_ORTHOGRAPHIC);
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
}

static inline void bar(float x, float y, float w, float h, u8 r, u8 g, u8 b)
{
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(x, y);       GX_Color4u8(r, g, b, 0xff);
    GX_Position2f32(x + w, y);   GX_Color4u8(r, g, b, 0xff);
    GX_Position2f32(x + w, y + h); GX_Color4u8(r, g, b, 0xff);
    GX_Position2f32(x, y + h);   GX_Color4u8(r, g, b, 0xff);
    GX_End();
}

// ------------------------------------------------------------------ tiny font (digits + P I S T D X)

// 8x8, bits 0x80..0x01 left to right, 7 rows used.
static const u8 glyph_digit[10][8] = {
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0}, // 1
    {0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0}, // 2
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0}, // 3
    {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0}, // 4
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0}, // 5
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0}, // 6
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0}, // 7
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0}, // 8
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0}, // 9
};
// index: 0=P 1=I 2=S 3=T 4=D 5=X
static const u8 glyph_letter[6][8] = {
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0}, // P
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0}, // I
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0}, // S
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0}, // T
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0}, // D
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0}, // X
};

static void draw_glyph(const u8 g[8], float x, float y, float s)
{
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col))
                bar(x + col * s, y + row * s, s, s, 255, 255, 255);
}

// A tiny string language: '0'-'9' digits, 'P','I','S','T','D','X' letters, ' ' space.
static void draw_text(const char *str, float x, float y, float s)
{
    for (const char *c = str; *c; c++) {
        if (*c >= '0' && *c <= '9') draw_glyph(glyph_digit[*c - '0'], x, y, s);
        else switch (*c) {
            case 'P': draw_glyph(glyph_letter[0], x, y, s); break;
            case 'I': draw_glyph(glyph_letter[1], x, y, s); break;
            case 'S': draw_glyph(glyph_letter[2], x, y, s); break;
            case 'T': draw_glyph(glyph_letter[3], x, y, s); break;
            case 'D': draw_glyph(glyph_letter[4], x, y, s); break;
            case 'X': draw_glyph(glyph_letter[5], x, y, s); break;
        }
        x += 9 * s;
    }
}

static const char *mode_label[NUM_MODES] = { "240P STD", "480I STD", "480P STD", "240P X2", "480I X2" };

// ------------------------------------------------------------------ patterns
// Each pattern draws into one 640-wide tile. For doubled modes, physical output x = tileBase + ex
// and the logical source pixel is x/2, so a 1-pixel source feature becomes 2 physical pixels, i.e.
// a full 4:2:2 pair on the bus.

static inline int logical_x(int tileBase, int ex) { return cur_doubled ? (tileBase + ex) / 2 : ex; }

// 1px vertical stripes of two colors, chosen per pattern.
static void pat_stripes(int tileBase, int H, u8 ar, u8 ag, u8 ab, u8 br, u8 bg, u8 bb)
{
    for (int ex = 0; ex < TILE_W; ex++) {
        int lx = logical_x(tileBase, ex);
        if (lx & 1) bar(ex, 0, 1, H, ar, ag, ab);
        else        bar(ex, 0, 1, H, br, bg, bb);
    }
}

// Chroma vs luma resolution wedge: top half red/cyan (chroma), bottom half white/black (luma),
// each divided into zones left->right with stripe period 1,2,3,4,6,8 logical px.
static void pat_wedge(int tileBase, int H)
{
    static const int period[8] = { 1, 2, 3, 4, 6, 8, 12, 16 };
    int half = H / 2;
    for (int ex = 0; ex < TILE_W; ex++) {
        int lx = logical_x(tileBase, ex);
        int zone = (lx * 8) / 640; if (zone > 7) zone = 7;
        int on = (lx / period[zone]) & 1;
        // top: chroma pair (red / cyan), similar luma
        if (on) bar(ex, 0, 1, half, 200, 0, 0);
        else    bar(ex, 0, 1, half, 0, 160, 160);
        // bottom: luma pair (white / black)
        if (on) bar(ex, half, 1, H - half, 255, 255, 255);
        else    bar(ex, half, 1, H - half, 0, 0, 0);
    }
}

// Solid reference bars: 8 color bars on top, grayscale ramp on bottom. Sanity check for color.
static void pat_reference(int tileBase, int H)
{
    static const u8 cols[8][3] = {
        {255,255,255},{255,255,0},{0,255,255},{0,255,0},
        {255,0,255},{255,0,0},{0,0,255},{0,0,0}
    };
    int half = H / 2;
    for (int i = 0; i < 8; i++)
        bar(i * TILE_W / 8.0f, 0, TILE_W / 8.0f + 1, half, cols[i][0], cols[i][1], cols[i][2]);
    for (int ex = 0; ex < TILE_W; ex++) {
        int lx = logical_x(tileBase, ex);
        u8 v = lx * 255 / 639;
        bar(ex, half, 1, H - half, v, v, v);
    }
}

// Single sharp chroma edge in the middle (left red, right cyan) with a few 1px white and black
// lines straddling it, to show chroma ringing around a hard colour transition.
static void pat_edge(int tileBase, int H)
{
    for (int ex = 0; ex < TILE_W; ex++) {
        int lx = logical_x(tileBase, ex);
        if (lx < 320) bar(ex, 0, 1, H, 200, 0, 0);
        else          bar(ex, 0, 1, H, 0, 160, 160);
    }
    // thin marker lines near the centre
    for (int d = -8; d <= 8; d += 2) {
        int lx = 320 + d;
        // find a physical ex that maps to this logical lx in this tile
        for (int ex = 0; ex < TILE_W; ex++) {
            if (logical_x(tileBase, ex) == lx) {
                bar(ex, H / 4, cur_doubled ? 2 : 1, H / 2, (d & 1) ? 0 : 255, (d & 1) ? 0 : 255, (d & 1) ? 0 : 255);
                break;
            }
        }
    }
}

static const char *pat_name[NUM_PATS] __attribute__((unused)) = {
    "MAGENTA/GREEN 1PX", "RED/BLUE 1PX", "ISOLUMA RED/GRAY 1PX",
    "CHROMA VS LUMA WEDGE", "COLOR + GRAY REFERENCE", "SHARP CHROMA EDGE"
};

static void draw_pattern(int tileBase, int H)
{
    switch (pat_idx) {
        case 0: pat_stripes(tileBase, H, 0, 200, 0, 200, 0, 200); break;   // green / magenta
        case 1: pat_stripes(tileBase, H, 0, 0, 220, 220, 0, 0); break;     // blue / red
        case 2: pat_stripes(tileBase, H, 76, 76, 76, 220, 0, 0); break;    // equal-luma gray / red
        case 3: pat_wedge(tileBase, H); break;
        case 4: pat_reference(tileBase, H); break;
        case 5: pat_edge(tileBase, H); break;
    }
}

// Status readout, drawn on the first tile only, over a black backing box.
static void draw_status(void)
{
    char line2[4] = { 'P', (char)('0' + pat_idx), 0, 0 };
    bar(6, 6, 170, 42, 0, 0, 0);
    draw_text(mode_label[mode_idx], 10, 9, 2.0f);
    draw_text(line2, 10, 28, 2.0f);
}

// ------------------------------------------------------------------ main loop

static void render(void)
{
    int H = rmode.efbHeight;
    int tiles = cur_doubled ? 2 : 1;
    int xfbWidth = cur_doubled ? MAX_XFB_W : rmode.fbWidth;

    GX_SetDispCopyYScale((f32)rmode.xfbHeight / (f32)H);
    GX_SetDispCopyDst(xfbWidth, rmode.xfbHeight);
    GX_SetCopyFilter(rmode.aa, rmode.sample_pattern, GX_FALSE, rmode.vfilter);
    GX_SetFieldMode(rmode.field_rendering, (rmode.viHeight == 2 * rmode.xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

    for (int t = 0; t < tiles; t++) {
        int tileBase = t * TILE_W;
        gx_begin_tile(H);
        GX_SetCopyClear((GXColor){ 0, 0, 0, 0xff }, GX_MAX_Z24);
        draw_pattern(tileBase, H);
        if (t == 0)
            draw_status();
        GX_SetDispCopySrc(0, 0, TILE_W, H);
        GX_CopyDisp((u8 *)xfb + t * TILE_W * 2, GX_TRUE);
    }
    GX_DrawDone();

    VIDEO_SetNextFramebuffer(xfb);
    if (cur_doubled)
        poke_hires_horizontal();
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

int main(void)
{
    VIDEO_Init();
    WPAD_Init();
    PAD_Init();

    have_component = VIDEO_HaveComponentCable();

    xfb = MEM_K0_TO_K1(memalign(32, MAX_XFB_W * MAX_XFB_H * 2));

    apply_mode(mode_idx);
    gx_init();

    bool need_reapply = false;
    while (1) {
        WPAD_ScanPads();
        PAD_ScanPads();
        u32 wd = WPAD_ButtonsDown(0);
        u32 pd = PAD_ButtonsDown(0);

        if (wd & WPAD_BUTTON_HOME) break;
        if (pd & PAD_BUTTON_START) break;

        int old_mode = mode_idx, old_pat = pat_idx;

        if ((wd & WPAD_BUTTON_RIGHT) || (pd & PAD_BUTTON_RIGHT)) mode_idx = (mode_idx + 1) % NUM_MODES;
        if ((wd & WPAD_BUTTON_LEFT)  || (pd & PAD_BUTTON_LEFT))  mode_idx = (mode_idx + NUM_MODES - 1) % NUM_MODES;
        if ((wd & WPAD_BUTTON_UP)    || (pd & PAD_BUTTON_UP))    pat_idx = (pat_idx + 1) % NUM_PATS;
        if ((wd & WPAD_BUTTON_DOWN)  || (pd & PAD_BUTTON_DOWN))  pat_idx = (pat_idx + NUM_PATS - 1) % NUM_PATS;

        if (mode_idx != old_mode) need_reapply = true;
        (void)old_pat;

        if (need_reapply) {
            apply_mode(mode_idx);
            need_reapply = false;
        }
        render();
    }

    // restore a safe mode before returning to the loader
    GXRModeObj *safe = VIDEO_GetPreferredMode(NULL);
    VIDEO_Configure(safe);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    return 0;
}
