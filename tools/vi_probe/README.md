# vi_probe (read-only)

Homebrew Channel app for the Wii that measures the real VI pixel clock and logs the VI and
Hollywood clock/PLL registers (`0x0D800190`-`0x1FC`) in 480i, 480p, 240p and 1280x240p.
It writes no clock or PLL registers.

Results go to `vi_probe.txt` in the root of the SD card and USB drive. Your display may lose
sync during the run, which takes about 20 seconds, and the app then returns to the Homebrew
Channel.

How it measures: the field rate and line period are timed against the CPU timebase, which runs
independently of the VI, so no working display is needed. The implied VI clock is
2 x hlw / line period.

Build with devkitPPC + libogc + libfat: `make`. Install `boot.dol` and `meta.xml` to
`apps/vi_probe/`. The `<ahb_access/>` tag in `meta.xml` asks the Homebrew Channel for access
to the Hollywood registers.
