Chroma subsampling test
=======================

The Wii/GameCube external framebuffer and digital video bus are YUV 4:2:2: every
two horizontal pixels share one Cb/Cr (colour) sample, so fine colour detail is
lost even though luma (brightness) detail is not.

This tool shows patterns that make that loss visible, and lets you compare a
standard signal against a "brute force double" signal where every source pixel is
duplicated into two output pixels. In the doubled signal each original pixel
occupies a whole 4:2:2 pair, so its colour is preserved on the bus and an FPGA
(PixelFX, GCVideo, etc.) could pair the duplicates back into full 4:4:4.

Controls
  D-pad Left/Right : resolution mode
  D-pad Up/Down    : pattern
  HOME (Wii) / START (GC pad) : exit

Resolution modes
  240P STD : 640x240 internal, 640 wide on the bus
  480I STD : 640x480 internal, 640 wide on the bus
  480P STD : 640x480 internal, 640 wide on the bus (component/digital only)
  240P X2  : 640x240 internal, 1280 wide on the bus (each pixel doubled)
  480I X2  : 640x480 internal, 1280 wide on the bus (each pixel doubled)

  480P X2 (1440 wide) is NOT included: it needs about a 108 MHz pixel clock,
  i.e. a custom VI/PLL clock, which this tool does not touch.

Patterns (number shown as P0..P5)
  P0 MAGENTA/GREEN 1PX   : 1px stripes of opposite hue, similar luma. In 4:2:2
                           these smear toward grey; the doubled mode should keep
                           them crisp once an FPGA reconstructs.
  P1 RED/BLUE 1PX        : 1px red/blue stripes.
  P2 ISOLUMA RED/GRAY    : 1px stripes of red and an equal-luma grey. The two
                           differ only in colour, so 4:2:2 blurs them into flat
                           grey; the sharpest chroma-only test.
  P3 CHROMA VS LUMA WEDGE: left-to-right stripe widths 1..16px. Top half is a
                           red/cyan (chroma) wedge, bottom half white/black
                           (luma). Shows chroma detail dying before luma.
  P4 COLOR + GRAY REF    : 8 colour bars and a grey ramp. Sanity check for colour
                           and black/white levels.
  P5 SHARP CHROMA EDGE   : a hard red|cyan edge with thin marker lines, to show
                           chroma ringing around a colour transition.

What to look for
  In a standard mode, P0/P1/P2 collapse toward grey or muddy colour and the fine
  end of the P3 chroma wedge washes out while the luma wedge stays sharp. Switch
  the same pattern to the matching X2 mode and compare what your capture or FPGA
  shows. Your display may briefly lose sync when switching into or out of an
  X2 mode; that is expected.

The X2 modes reuse the 1280x240p horizontal timing (54 MHz pixel clock) proven
on a Wii Dual. 240p and 480i are the modes GCVideo handles most readily.
Built from source in this folder with devkitPPC + libogc.
