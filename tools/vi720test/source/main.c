// vi720test v2: the 720p raster LOCKED on hardware (74.25 MHz, ~45 kHz line). The picture
// came out a green/magenta mess -- the classic signature of a chroma/YUYV phase or sampling
// error. This build keeps the EXACT locked timing and steps through diagnostic patterns to
// localise the fault (Wii chroma vs GCVideo undersampling above 54 MHz):
//
//   MODE A  1280x240p @ 54 MHz, fine pattern     -- known-good control (should look perfect)
//   720p-1  GRAYSCALE (chroma neutral, U=V=128)  -- if clean -> luma path perfect, fault is
//                                                   chroma/phase or GCVideo; if still green/
//                                                   magenta -> chroma invented from nothing
//                                                   -> points at GCVideo >54 MHz.
//   720p-2  PRIMARIES (8 wide solid colour blocks)-- coarse chroma; tests colour fidelity at
//                                                   low horizontal chroma frequency.
//   720p-3  FINE (bars + grid + corners)         -- reproduces the original mess for compare.
//
// A small stack of white squares top-left marks the mode (1,2,3 squares = gray/primaries/fine).
// Timing is unchanged from the run that locked. PLL/VI restored at end; all resets on reboot.

#include <gccore.h>
#include <fat.h>
#include <stdbool.h>
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include <ogc/machine/processor.h>
#include <ogc/lwp_watchdog.h>

#define VI  ((volatile u16 *)0xCC002000)
#define HW(off)  (*(volatile u32 *)(0xCD800000 + (off)))

#define W 1280
#define H 720
#define WORDS_PER_ROW (W/2)

#define PLL_1C4_7425 0x00200AC0u
#define PLL_1C8_7425 0xD880700Eu
#define HLW_240 858
#define HLW_720 413

static u32 *xfb;
static volatile u32 retraces;
static void on_retrace(u32 c){ (void)c; retraces++; }

static void logf_(const char *fmt, ...)
{
    static const char *const paths[] = { "usb:/vi720.txt", "sd:/vi720.txt" };
    static bool first = true;
    char line[320]; va_list ap; va_start(ap,fmt); vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    for (int i=0;i<2;i++){ FILE *f=fopen(paths[i], first?"w":"a"); if(f){fputs(line,f);fputc('\n',f);fflush(f);fclose(f);} }
    first=false; printf("%s\n",line);
}
static void tb_delay_ms(u32 ms){ u64 to=gettime()+millisecs_to_ticks(ms); while(gettime()<to){} }
static u32 cur_line(void){ return VI[0x2C/2]&0x7FF; }

typedef struct { bool field_ok; u32 field_milli; bool line_ok; u32 line_ns; u32 hlw;
                 u32 counter_khz; u32 eff_khz; u32 maxline; } Meas;
static bool wait_cross(u32 t,u64 tk,u32*ml){ u64 to=gettime()+tk; u32 l;
    while((l=cur_line())>=t){ if(l>*ml)*ml=l; if(gettime()>to)return false; }
    while((l=cur_line())<t){ if(l>*ml)*ml=l; if(gettime()>to)return false; } return true; }
static Meas measure(void)
{
    Meas m; memset(&m,0,sizeof(m)); m.hlw=VI[0x06/2]&0x3FF;
    u32 r0=retraces; u64 fto=gettime()+millisecs_to_ticks(500);
    while(retraces==r0&&gettime()<fto){}
    if(retraces!=r0){ u32 r1=retraces; u64 t1=gettime(); u64 win=gettime()+millisecs_to_ticks(3000);
        while((retraces-r1)<120&&gettime()<win){} u64 t2=gettime(); u32 g=retraces-r1;
        if(g>=2){ u64 us=ticks_to_microsecs(t2-t1); if(us){m.field_ok=true;m.field_milli=(u32)((u64)g*1000000000ull/us);} } }
    u64 best=~0ull;
    for(int k=0;k<8;k++){ u32 ml=m.maxline; u64 sto=gettime()+millisecs_to_ticks(50); u32 l;
        while((l=cur_line())>10){ if(l>ml)ml=l; if(gettime()>sto)break; }
        bool a=wait_cross(40,millisecs_to_ticks(50),&ml); u64 ta=gettime();
        bool b=wait_cross(200,millisecs_to_ticks(50),&ml); u64 tb=gettime(); m.maxline=ml;
        if(a&&b&&tb>ta&&(tb-ta)<best)best=tb-ta; }
    if(best!=~0ull){ u64 ns=ticks_to_nanosecs(best); m.line_ns=(u32)(ns/160);
        if(m.line_ns){m.line_ok=true;m.counter_khz=(u32)(2ull*m.hlw*1000000ull/m.line_ns);m.eff_khz=m.counter_khz*2;} }
    return m;
}
static void log_meas(const char*tag,Meas*m){
    if(m->field_ok) logf_("    [%s] field=%u.%03u Hz",tag,m->field_milli/1000,m->field_milli%1000);
    else logf_("    [%s] field=NO RETRACE",tag);
    if(m->line_ok){ u32 lk=m->line_ns?(u32)(1000000000ull/m->line_ns):0;
        logf_("    [%s] line=%u ns (~%u.%02u kHz) hlw=%u eff~%u kHz maxline=%u (lines=%u)",
              tag,m->line_ns,lk/1000,(lk%1000)/10,m->hlw,m->eff_khz,m->maxline,(m->maxline+1)/2);
    } else logf_("    [%s] line=NO LOCK maxline=%u",tag,m->maxline);
}

// ---------------- patterns (YUYV) ----------------
static inline void putpair(u32 x,u32 y,u8 Y,u8 Cb,u8 Cr){
    if(x+1>=W||y>=H) return;
    xfb[y*WORDS_PER_ROW+(x>>1)] = ((u32)Y<<24)|((u32)Cb<<16)|((u32)Y<<8)|Cr; }
static void fill(u32 x0,u32 y0,u32 x1,u32 y1,u8 Y,u8 Cb,u8 Cr){
    if(x1>W)x1=W; if(y1>H)y1=H;
    for(u32 y=y0;y<y1;y++) for(u32 x=x0&~1u;x<x1;x+=2) putpair(x,y,Y,Cb,Cr); }
static void clear_black(void){ for(u32 i=0;i<(u32)(W*H/2);i++) xfb[i]=0x10801080; }

// mode id marker: n white squares down the top-left
static void mark(int n){ for(int i=0;i<n;i++) fill(8, 8+i*20, 24, 24+i*20, 235,128,128); }

static void draw_gray(void){            // MODE 1: chroma-neutral luma ramp + gray bars
    clear_black();
    for(u32 x=0;x<W;x+=2){ u8 Y=16+(u8)((219u*x)/ (W-1)); fill(x,0,x+2,H,Y,128,128); }
    // 8 flat gray steps across the top third for easy luma judging
    static const u8 step[8]={16,47,78,109,140,171,202,235};
    for(int i=0;i<8;i++) fill(i*(W/8),0,(i+1)*(W/8),H/3, step[i],128,128);
    for(u32 x=0;x<W;x+=128) fill(x,0,x+2,H,235,128,128);  // geometry grid (still neutral)
    fill(0,0,W,4,235,128,128); fill(0,H-4,W,H,235,128,128); fill(0,0,4,H,235,128,128); fill(W-4,0,W,H,235,128,128);
    mark(1);
}
static void draw_primaries(void){       // MODE 2: wide solid colour blocks (coarse chroma)
    static const u8 c[8][3]={ {235,128,128},{81,90,240},{51,240,110},{41,240,110},
                              {210,16,146},{170,166,16},{106,202,222},{16,128,128} };
    // white, red, green, blue, yellow, cyan, magenta, black  (note green/blue similar in YUV)
    for(int i=0;i<8;i++) fill(i*(W/8),0,(i+1)*(W/8),H,c[i][0],c[i][1],c[i][2]);
    fill(0,0,W,4,235,128,128); fill(0,H-4,W,H,235,128,128);
    mark(2);
}
static void draw_fine(void){            // MODE 3: original bars+grid+corners
    static const u8 bar[8][3]={ {200,128,128},{190,16,148},{170,166,16},{145,54,34},
                                {106,202,222},{81,90,240},{51,240,110},{16,128,128} };
    u32 bw=W/8;
    for(int i=0;i<8;i++) fill(i*bw,0,(i+1)*bw,H,bar[i][0],bar[i][1],bar[i][2]);
    for(u32 x=0;x<W;x+=80) fill(x,0,x+2,H,235,128,128);
    for(u32 y=0;y<H;y+=80) fill(0,y,W,y+2,235,128,128);
    fill(0,0,W,4,235,128,128); fill(0,H-4,W,H,235,128,128); fill(0,0,4,H,235,128,128); fill(W-4,0,W,H,235,128,128);
    fill(0,0,56,56,81,90,240); fill(W-56,0,W,56,51,240,110);
    fill(0,H-56,56,H,51,240,110); fill(W-56,H-56,W,H,235,128,128);
    mark(3);
}

// ---------------- VI ----------------
static const u32 CAND[]={0x1A0,0x1C4,0x1C8}; static u32 snap[3];
static void snapshot(void){ for(int i=0;i<3;i++) snap[i]=HW(CAND[i]); }
static void restore_pll(void){ u32 l; _CPU_ISR_Disable(l); for(int i=0;i<3;i++) HW(CAND[i])=snap[i]; _CPU_ISR_Restore(l); }
static u32 scale_field(u32 v,u32 n,u32 d){ return (v*n)/d; }

static GXRModeObj mode240={ VI_TVMODE_NTSC_DS,1280,240,240,(VI_MAX_WIDTH_NTSC-640)/2,(VI_MAX_HEIGHT_NTSC/2-480/2)/2,
    640,480,VI_XFBMODE_SF,GX_FALSE,GX_FALSE,
    {{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6}},{0,0,21,22,21,0,0} };

static void vi_present(GXRModeObj*m){ VIDEO_Configure(m); VIDEO_SetNextFramebuffer(xfb); VIDEO_SetBlack(false); VIDEO_Flush(); tb_delay_ms(60); }

static void apply_240p(void){ u32 l; _CPU_ISR_Disable(l);
    VI[0x6C/2]=0x0001; VI[0x04/2]=0x4769; VI[0x06/2]=0x035A; VI[0x08/2]=0x059A; VI[0x0A/2]=0x93FF;
    VI[0x4A/2]=0x0100; VI[0x48/2]=0x5050; VI[0x02/2]=0x0005; _CPU_ISR_Restore(l); tb_delay_ms(200); }

// EXACT timing that locked on hardware last run.
static void apply_720p(void){
    u32 htr0_hi=(scale_field(0x47,HLW_720,HLW_240)<<8)|scale_field(0x69,HLW_720,HLW_240);
    u32 h1=0x059A93FF; u32 hsy=h1&0x7F, hbe=(h1>>7)&0x3FF, hbs=(h1>>17)&0x3FF;
    hsy=scale_field(hsy,HLW_720,HLW_240); hbe=scale_field(hbe,HLW_720,HLW_240); hbs=scale_field(hbs,HLW_720,HLW_240);
    u32 htr1=(hbs<<17)|(hbe<<7)|hsy; u32 htr1_lo=htr1>>16, htr1_hi=htr1&0xFFFF;
    u32 vtr=(720u<<4)|0x0C;
    logf_("  720p timing: 0x1C4=%08X 0x1C8=%08X HLW=%u HTR0hi=%04X HTR1=%04X%04X VTR=%04X",
          PLL_1C4_7425,PLL_1C8_7425,HLW_720,(unsigned)htr0_hi,(unsigned)htr1_lo,(unsigned)htr1_hi,(unsigned)vtr);
    u32 l; _CPU_ISR_Disable(l);
    VI[0x6C/2]=0x0001; HW(0x1C4)=PLL_1C4_7425; HW(0x1C8)=PLL_1C8_7425;
    VI[0x04/2]=(u16)htr0_hi; VI[0x06/2]=(u16)HLW_720;
    VI[0x08/2]=(u16)htr1_lo; VI[0x0A/2]=(u16)htr1_hi;
    VI[0x02/2]=0x0005; VI[0x48/2]=0x5050; VI[0x4A/2]=0x0100;
    VI[0x00/2]=(u16)vtr; VI[0x0C/2]=0x0024; VI[0x0E/2]=0x0018; VI[0x10/2]=0x0024; VI[0x12/2]=0x0018;
    _CPU_ISR_Restore(l); tb_delay_ms(300);
}

static void hold(u32 ms){ u64 to=gettime()+millisecs_to_ticks(ms);
    while(gettime()<to){ WPAD_ScanPads(); PAD_ScanPads();
        if((WPAD_ButtonsDown(0)&WPAD_BUTTON_HOME)||(PAD_ButtonsDown(0)&PAD_BUTTON_START)) break; tb_delay_ms(30); } }

int main(void)
{
    VIDEO_Init(); WPAD_Init(); PAD_Init();
    GXRModeObj *pref=VIDEO_GetPreferredMode(NULL);
    xfb=(u32*)MEM_K0_TO_K1(memalign(32,W*H*2));
    clear_black();
    VIDEO_Configure(pref); VIDEO_SetNextFramebuffer(xfb); VIDEO_SetPostRetraceCallback(on_retrace);
    VIDEO_SetBlack(false); VIDEO_Flush(); VIDEO_WaitVSync();
    CON_Init(xfb,20,20,40,40,W*2);
    bool fat=fatInitDefault();
    logf_("vi720test v2 (colour diagnostics) build " __DATE__ " " __TIME__ "  fat=%d",fat);
    snapshot();

    // MODE A -- known-good control
    logf_(""); logf_("=== MODE A: 1280x240p @ 54 MHz FINE pattern (control, should look perfect) ===");
    draw_fine(); vi_present(&mode240); apply_240p();
    Meas a=measure(); log_meas("240p",&a);
    logf_("   hold ~15 s (HOME/Start = next)"); hold(15000);

    // 720p diagnostics at the locked timing
    vi_present(&mode240); apply_720p();

    logf_(""); logf_("=== 720p MODE 1: GRAYSCALE (chroma neutral). Decisive test. (1 square top-left) ===");
    logf_("   If this is clean grey -> Wii luma perfect, colour fault is chroma/phase or GCVideo.");
    logf_("   If this is ALSO green/magenta -> chroma invented from nothing -> GCVideo >54 MHz.");
    draw_gray(); Meas m1=measure(); log_meas("720p-gray",&m1);
    logf_("   hold ~20 s (HOME/Start = next)"); hold(20000);

    logf_(""); logf_("=== 720p MODE 2: PRIMARIES, 8 wide solid blocks (2 squares) ===");
    logf_("   order L->R: white red green blue yellow cyan magenta black.");
    draw_primaries(); hold(20000);

    logf_(""); logf_("=== 720p MODE 3: FINE bars+grid (3 squares) -- reproduces original mess ===");
    draw_fine(); hold(20000);

    restore_pll(); tb_delay_ms(200); vi_present(pref);
    logf_(""); logf_("restored 0x1C4=%08X 0x1C8=%08X",(unsigned)HW(0x1C4),(unsigned)HW(0x1C8));
    logf_("DONE"); return 0;
}
