# vi_pll — write-and-measure probe for the Wii VI clock / PLL

**EXPERIMENTAL. This app writes to undocumented Hollywood clock registers.** It is the
write-capable successor to `vi_probe` (which is read-only). The goal is to find register
writes that push the VI pixel clock above the stock **54 MHz**, as the first step toward
720p/1080i-class pixel clocks (74.25 / 148.5 MHz).

All registers it touches reset on a console power cycle, and each experiment restores the
original values afterwards, so a bad value costs you a reboot, nothing more.

---

## RESULT (runs 1-2): the VI clock is fully reprogrammable

Confirmed on a retail Wii, 1280x240p anchor, clean up to 193 MHz with no lock loss, every
value fully recovered:

**F = 54 MHz x Nfb / V**
- **Nfb = `0x1C4` bits 8-15 (byte1) + 1** — feedback multiplier, baseline 7. Linear 7..25
  (54..193 MHz), step 54/7 = 7.714 MHz.
- **V = `0x1C8` bits 20-23** — output divider, baseline 7. Confirmed 7->8 = x7/8 (47.25 MHz).

Exact HD pixel clocks:

| Target | 54 x Nfb/V | `0x1C4` | `0x1C8` |
| --- | --- | --- | --- |
| 74.25 MHz (720p/1080i) | 54 x 11/8 | `0x00200AC0` | `0xD880700E` |
| 148.5 MHz              | 54 x 22/8 | `0x002015C0` | `0xD880700E` |
| 148.5 MHz (alt)        | 54 x 11/4 | `0x00200AC0` | `0xD840700E` |

Secondary / danger notes:
- `0x1C4` **byte2 (bits 16-23) HARD-HANGS** when incremented (0x20->0x21). Never written by
  the default sweep. Keep byte2 = `0x20`.
- `0x1C4` byte0 = output post-divider: `0xC0`=/1 (54), `0xC1`=/2 (27), `0xC2`=/3 (18);
  larger low-nibble values lost lock in the base config. byte0 bits 6-7 give a small fine
  trim (~2 MHz/bit). Not needed once Nfb and V hit the target exactly (leave byte0 = `0xC0`).
- `0x1A0` bit31 is an inert status mirror, not a usable control.

Clock generation is confirmed; a valid HD *video signal* still needs the VI horizontal/
vertical timing reprogrammed. Note HTR0's half-line width is 10-bit: 720p htotal 1650
(half 825) fits; 1080i htotal 2200 (half 1100) does NOT, so 1080i needs a different timing
approach (see the briefing's open VI-timing questions).

---

## What it does

1. **Snapshots** the candidate registers and logs their boot values.
2. Runs an experiment list and, for each one:
   - sets the fixed **1280×240p / 54 MHz** timing as a measurement anchor,
   - **write-ahead logs** the exact register write(s) and *commits the log file to the USB
     drive before poking* — so if a value hangs the console, the log shows which experiment
     was in flight,
   - pokes the register(s) with interrupts disabled,
   - reads back what actually stuck,
   - **measures the real VI clock** (field rate + line period, timed against the CPU
     timebase, so no display sync is needed),
   - **restores** the snapshot and re-measures to confirm recovery.
3. Logs everything to **`vi_pll.txt`** on the USB drive (and SD if present).

### The default run (no config file)

- `CONTROL` — rewrite `0x1C4` with its own value (proves writes land + measurement is stable).
- `CONTROL` — set `0x1A0` bit 31 (the documented 27/54 ÷2 select). If the clock **halves**,
  the write+measure path is proven to detect a real clock change; if nothing changes, that bit
  is a status mirror, not a live divider. Either way it is logged and reversed.
- `MAP 0x1C4` — increment one nibble at a time (`+1<<0`, `+1<<4`, … `+1<<28`) and measure.
- `MAP 0x1C8` — same.

The map tells you, per nibble, whether incrementing it **raises** the clock (→ feedback
multiplier), **lowers** it (→ divider), or does nothing (→ config/lock bits). That is the
information needed to then aim at 74.25 / 148.5 MHz — **without guessing the layout.**

### Iterating without rebuilding

Drop a `vi_pll.cfg` (see `vi_pll.cfg.sample`) in the USB root to replace the sweep with
explicit `TRY OFF=VAL …` lines. Edit it on your PC between runs. Only the snapshotted
offsets (`1A0 1A4 1A8 1AC 1C4 1C8`) are accepted.

---

## Build

Same toolchain as `vi_probe` / the SM64 port (devkitPPC + libogc + libfat; also libwiiuse/libbte
for the start button). From this directory:

```
make
```

Install `boot.dol` and `meta.xml` to `apps/vi_pll/` on the USB drive (or SD). The
`<ahb_access/>` tag in `meta.xml` asks the Homebrew Channel for Hollywood register access.

## Run

Launch from the Homebrew Channel. A prompt appears (press **A** to start, or it auto-starts in
10 s so it also works with no controller synced; **HOME**/**Start** exits). The display will
lose sync during the run — that is expected; the measurement does not need it. The app returns
to the Homebrew Channel when done.

Then copy `vi_pll.txt` off the USB drive and send it back for the next round.

---

## How to read the log

Each experiment block looks like:

```
### EXPERIMENT 7: MAP 0x1C4 += 1<<8  (probe nibble @ bit 8)
  WILL WRITE HW 1C4 = 002007C0 (mask FFFFFFFF)  before=002006C0
  >>> poking now <<<
  readback HW 1C4 = 002007C0
    [after-write] field=59.940 Hz
    [after-write] line=... hlw=858  counter~28000 kHz  EFF PIXEL CLOCK ~ 56000 kHz  (maxline ...)
    [after-write] ratio vs base = 1.03x   -> >>> CLOCK WENT UP <<<
    [after-restore] ... EFF PIXEL CLOCK ~ 54000 kHz
  --- end experiment 7 ---
```

- **`EFF PIXEL CLOCK`** is the headline number (kHz). Baseline is ~54000.
- **`ratio vs base`** and the verdict (`CLOCK WENT UP` / `DOWN` / `no change`) flag the nibble
  that moves the clock.
- **`readback`** differing from the written value means the PLL ignored/massaged some bits.
- **`line=NO LOCK`** means the beam counter stopped sweeping — the clock stalled or went
  unstable at that value (useful: marks an out-of-range field).
- **`field=NO RETRACE`** means the VI stopped generating fields but the line measurement may
  still read the raw counter.

The numbers follow the same convention as `vi_probe`: `counter` is `2*hlw/line_period`, and
`EFF PIXEL CLOCK = counter × 2`.

---

## Safety notes

- Writes are confined to `0x1A0, 0x1A4, 0x1A8, 0x1AC, 0x1C4, 0x1C8` (the VI-clock candidates
  identified by `vi_probe`). Nothing writes CPU/DSP/DDR PLLs.
- Every experiment restores the snapshot; every register also resets on power cycle.
- All measurement loops are bounded by the CPU timebase — a stopped VI is reported, never an
  infinite hang inside the app.
- The **write-ahead log** is the recovery tool: if the console freezes on a specific value,
  `vi_pll.txt` ends at `>>> poking now <<<` for that experiment, naming the exact write.

## Known open risk (from the briefing)

The PLL at `0x1C4/0x1C8` *may* feed blocks other than the VI. If a write visibly disturbs
audio, GX, or causes instability beyond the video output, note which experiment and power
cycle. Start from small perturbations (the default map does), not large jumps.
