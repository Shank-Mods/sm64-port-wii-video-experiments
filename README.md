# Super Mario 64 Wii/Gamecube Port

This is a native port of Super Mario 64 to the Nintendo Gamecube/Nintendo Wii.

![Peach's Castle](docs/peachcastle.png)
![Bob-Omb Battlefield](docs/bob.png)

This repo does **not** include all assets necessary for compiling the game.
A prior copy of the game is required to extract the assets.
Do not ask for where to obtain a ROM. That's for you to figure out.

With the port to more advanced hardware, there also is a handful of option enhancements that can be enabled in-game.
Do note that some enhancements may result in slightly buggy or odd behaviour that wouldn't be noticed otherwise, but nothing game-breaking.
Enhancements can be accessed by pausing then pressing the Z/L equivalent for your controller.

You can expect full-speed on Wii with all the performance enhancements enabled at all times.
I didn't test enough on Gamecube to make the same claim, but it should be able to handle at least 60fps without issue.

![Enhancements Menu](docs/settings.png)

## 1280x720p output (Wii, `wii-720p` branch)

This branch renders the game **natively at 1280x720** and outputs a real **720p60** signal, for
digital video mods that accept HD pixel clocks from the Wii (e.g. a PixelFX RetroGEM). Analog
outputs and GCVideo-based boards (rated to 54 MHz) will not show it.

- **Video:** the VI pixel clock is reprogrammed to 148.5 MHz (PLL Nfb 22, V 8: the bus moves one
  74.25 MHz pixel every two clocks) and the VI gets a complete SMPTE 296M register image
  (1650 x 750 total, 1280 x 720 active, exactly 60.000 Hz). Timing, PLL law and register layout
  come from [Wii-VI-Experiments](https://github.com/Shank-Mods/Wii-VI-Experiments)
  (`tools/vimodetest`, `docs/VI_PLL.md`). libogc is left on the console's normal mode
  underneath; framebuffer flips are written straight to the VI from the retrace interrupt. On
  exit (HOME, power, reset) the stock clock and mode are put back before anything else.
- **Rendering:** the GPU's embedded framebuffer is at most 640x528, so each frame is recorded
  into display lists once and replayed as a **2x2 grid of 640x360 tiles**, each copied into its
  quadrant of the 1280x720 XFB (the 1280x240p tiling, extended to two rows). The CPU-side work
  (game logic, display list decode, software vertex transform) happens once per frame; the GPU
  draws the scene four times with about 3x the pixels of 480p, so expect GPU time to dominate.
- **960x720p** (`video_mode 960x720p`): vimodetest's 4:3 720p mode. Same 750-line frame and
  45.00 kHz line rate as 1280x720p (so also exactly 60.000 Hz), with 1200-pixel lines (960
  active, sync 30, back porch 160) at a 108 MHz VI clock (Nfb 14, V 7; 54 Mpx/s on the bus).
  Rendered as 2x2 tiles of 480x360, about 56% of the pixels of 1280x720.
- **1280x960 crop** (`video_mode 1280x960crop`): a benchmark of rendering at 1280x960 that
  needs no special video hardware. The frame is rendered at 1280x960 as 2x2 tiles of 640x480,
  and every tile is copied into a full 1280x960 XFB, so the CPU and GPU do all the work of that
  resolution. The VI stays on the console's normal 480p (or 480i) timing and clock; only its
  picture configuration changes, to fetch 640 pixels per line with a 1280-pixel stride, so the
  TV shows the **top left quarter** of the frame. The only part not representative of a real
  1280x960 output is the VI's own memory reads (those of 640x480).
- **Higher modes** (same direct-VI method; timing checked against the VI limits found by
  vilinetest: lines <= 2046 pixels, fields <= ~2047 half-lines, PRB >= 1, EQU >= 1):

  | `video_mode` | total | pixel rate (Nfb/V) | field rate | tiles | notes |
  |---|---|---|---|---|---|
  | `1280x960p` | 1800 x 1000 | 108 MHz (16/4) | 60.000 Hz | 2x2 of 640x480 | VESA 1280x960@60, 4:3 |
  | `1920x1020p` | 2026 x 1022 | 124.2 MHz (23/5) | 59.98 Hz | 3x2 of 640x510 | square pixels (1.88:1); only 2 lines of vertical blanking, the most active lines the VI's field counter allows |
  | `1920x1440i` | 2038 x 1501 | 91.8 MHz (17/5) | 60.02 Hz | 3x3 of 640x480 | 4:3 square pixels, 720 lines per field; rendered progressive, each field reads every other line |

  Their XFBs (2.5 / 3.9 / 5.5 MB each, three of them) don't fit in MEM1 next to the game, so
  some or all land in MEM2; the debug log records where. `crop_480p true` works with all of
  them.
- Widescreen AUTO resolves to 16:9 in 1280x720p and 1920x1020p (which uses its real 1.88:1
  shape) and to 4:3 in the other modes.
  Antialiasing is not used in the 720p modes.
- The app needs `<ahb_access/>` (in `meta.xml`) to reach the PLL registers.
- The startup log (`sm64_debug.txt` in the root of the SD card / USB drive) records every VI register and PLL
  value written.

**Check the mode first:** if 720p doesn't show, try the `1280x720p` entry in Wii-VI-Experiments'
`vimodetest` on the same setup. If its patterns look right, the problem is in the game; if not,
it's the VI timing or the video chain. Set `video_mode default` to go back to 480p.

### Options (`apps/sm64/sm64config.txt`)

| option | default | |
|---|---|---|
| `video_mode` | `720p` | `720p` (1280x720p), `960x720p`, `1280x960p`, `1920x1020p`, `1920x1440i`, `1280x960crop` (= `1280x960p` with `crop_480p`), or `default` for the console's own mode (480p/480i, or 240p / 1280x240p if those are enabled) |
| `crop_480p` | `false` | render at `video_mode`'s full size (1280x720 or 960x720) but leave the video signal on the console's normal 480p, showing the **top left 640x480** of the frame (the same method as `1280x960crop`). Measures the full-resolution render on any display |
| `bench_demo` | `true` | timedemo: plays the attract demos in a loop, input ignored, results logged. `false` = normal play with your controller |
| `bench_uncapped` | `true` | one game tick per rendered frame and no vsync wait, so game logic runs as fast as the hardware allows (fast motion, audio silent). `false` = normal 30 Hz logic presented at vsync, with the `60fps` option's interpolation. `render` = normal 30 Hz logic in real time (normal speed and audio) while frames are drawn as fast as possible, each interpolated to the moment it's drawn: the frame rate real gameplay could run at |
| `bench_overlay` | `true` | the frame-time overlay, in demo mode and in normal play |
| `bench_gpu_split` | `2` | 0 overlapped, 1 split, 2 alternate (in normal play: one 0.5 s window in four is split) |

`benchmark` (the `wii-benchmark` branch's name for `bench_demo`) is still read.

In capped mode the overlay's FPS is the presented rate; the **busy** line shows the time per
frame excluding the vsync wait, and the frame rate that alone would allow.

### N64 controllers (`n64_controller`, default `true`)

A real N64 controller works in any GameCube port through a passive 3-wire adapter: GameCube pin 2
(data) to the N64 data pin, pin 3/4 (ground) to ground, pin 6 (3.43 V) to the N64's 3.3 V pin.
**Never connect pin 1 (5 V).** The console's serial interface sends the N64 Joybus commands
itself. The controller's buttons and stick go to the game exactly as on an N64 (1:1 stick, no
remapping). Ports are probed once a second, so controllers can be plugged in and out while the
game runs; ports holding a GameCube controller are left to the normal GameCube controller code.
With several N64 controllers, the lowest-numbered port is player 1. Protocol proven with
`Shank-Mods/n64_pad_tester_wii`.

### Optimizations (`wii-optimize` branch)

All on by default; each can be switched off in `sm64config.txt` (`opt_x false`) to measure what it
is worth. Checked frame by frame in Dolphin against the unoptimized build (all video modes):
differences are sub-pixel rounding only.

| Option | What it does |
|---|---|
| `opt_ps_vertex` | CPU vertex transform and lighting with Broadway paired-single math; packed vertex buffer |
| `opt_native_tex` | N64 textures converted straight to GX formats (RGB5A3, IA4, IA8) instead of RGBA8: 2-4x less texture memory and conversion |
| `opt_tex_hash` | cheaper texture cache hashing, memoised per frame |
| `opt_state_cache` | skips redundant texture loads and projection/viewport/scissor reloads in tiled replay |
| `opt_tile_cull` | tiled modes: draws that miss a tile are skipped on that tile |
| `opt_mem2_gpu` | textures and the tiled display list in MEM2 (16 MB heap) |
| `opt_sky_last` | skybox drawn after the opaque level (before cut-outs and translucent layers), depth tested at the far plane, so the GPU only fills the sky where it shows instead of painting the whole screen first |
| `opt_hw_fog` | CPU-path fog done by GX's fog unit from screen depth, instead of an extra colour channel and TEV stage per fogged pixel |
| `opt_tri_bin` | tiled modes: a draw spanning several tiles is cut into runs of triangles by the tiles they touch, so each tile only processes its own geometry (order kept) |
| `jrb_mist` | `false` skips Jolly Roger Bay's mist layer (a visual change; default `true`) |
| `opt_gpu_tnl` | GPU transform, lighting, texgen and fog: N64 vertices go to GX by index with hardware matrices; the CPU path remains for draws GX can't express. Default `false`: slower on hardware in the tiled modes |

Also built in (no switch): `-O3 -ffast-math` for the renderer, `-O3 -funroll-loops` and hoisted
branches for the audio mixer.

## GameCube build (`wii-720p` branch)

`make TARGET_WII=0 TARGET_GAMECUBE=1` builds the same benchmark for the GameCube. The GameCube's
VI clock can't be raised, so every high resolution `video_mode` runs as its **480p crop**: the
frame is rendered at full size, and the top left 640x480 is shown on the console's normal
480p/480i. The default there is `video_mode 1920x1020p` (6 tiles of 640x510).

**How crop mode works (Wii and GameCube alike):** every tile is drawn and copied out to RAM.
The top left tile goes into the displayed XFB (640 wide, one tile tall); every other tile goes
into a single tile-sized scratch buffer. The GPU draws the same pixels and writes the same
number of bytes as it would into a full-size XFB, without the memory for one (a 1920x1020 XFB
is 3.9 MB, and the GameCube has 24 MB in all). The video signal is plain libogc 480p, so no VI
registers are touched. Results files record the console.

- Clock calibration uses the GameCube's stock clocks (162 MHz bus, 486 MHz CPU).
- Config, saves, `sm64_bench.txt` and (debug builds) `sm64_debug.txt` go to the root of the
  first SD card found: SD2SP2 (`sd:`) or an SD Gecko (`carda:`/`cardb:`).
- No disc-slot LED heartbeat (Wii only).



This branch turns the port into a repeatable **timedemo benchmark**, meant for comparing a stock
Wii against an overclocked one. With `benchmark true` (the default on this branch):

- The game runs **uncapped**: one game tick and one rendered frame per loop, as fast as the
  hardware allows, so the game plays back in fast motion. The video output stays at its normal
  rate (480p60 etc.). Frames go to three XFBs and only the newest finished frame is queued for
  the next retrace, so there is no tearing and the TV never sees a mode change.
- It plays the seven attract-mode demos (Bowser 1 arena, WF, CCM, BBH, JRB, HMC, PSS) in a loop with no
  input, which makes every run the same workload. Controllers are ignored except HOME (exit).
  The title screen waits 1 second instead of 27 between demos.
- Audio is still synthesized every tick (it is real CPU work) but not played.
- One **pass** is all seven demos. At the end of each pass the results are appended to
  `apps/sm64/sm64_bench.txt`: avg FPS, 1% low FPS, worst frame per demo, a per-task CPU
  breakdown and (on split passes) a GPU breakdown, ending in a `SCORE` line.

### Overlay

The corner overlay (refreshed twice a second) shows live FPS, frame time, how many frames per
second actually reach the TV, the measured bus clock and an estimated CPU clock, the CPU time
per task sorted by cost, the GPU time per task, and the scores of the last few passes.

CPU tasks: game logic (Mario, objects, collision, camera), scene graph (display list building),
audio synthesis, gfx_pc's display list decode and triangle setup, its software vertex transform,
texture decoding, GX command submission, waiting for the GPU, presenting, and the overlay itself.

GPU tasks: 3D world draws, 2D/orthographic draws (HUD, text, sky, transitions) and the EFB to
XFB copy. In 1280x240p mode the GPU time is reported only as a total.

### Overlapped and split passes

Normally the GPU executes commands while the CPU is still writing them, so its time can't be
separated from the CPU's. **Split** passes hold the GPU back with a command FIFO breakpoint until
the CPU has finished the frame, then let it run: CPU time and GPU time are then measured cleanly
(and the overlay shows the FPS each side alone would allow), at the cost of losing the overlap.
**Overlapped** passes run the normal pipeline and give the real-world FPS.

`bench_gpu_split` chooses: `0` overlapped only, `1` split only, `2` alternate (default: odd
passes overlapped, even passes split). Compare overlapped scores for "how much faster is it",
and split passes for "which side got faster".

### Clock calibration

The PowerPC timebase runs at a quarter of the bus clock, so an overclock speeds it up and would
make every timing wrong by the overclock ratio. The benchmark instead counts timebase ticks
between VI retrace interrupts, which come from the video clock that the overclock doesn't touch,
and converts all times with the measured rate. The overlay and results show the resulting bus
clock (243 MHz at stock) and an estimate of the CPU clock from a timed chain of dependent adds
(729 MHz at stock), so you can confirm the overclock is active. The field rate is assumed to be
59.94 Hz (50 Hz for PAL 50); 240p modes are actually ~0.2% slower, which shifts stock and
overclocked results equally.

### Options (`apps/sm64/sm64config.txt`)

See the option table in the 1280x720p section above (`benchmark` is now `bench_demo`, and
`bench_uncapped` controls the uncapped part separately).

The `60fps` interpolation option is ignored while benchmarking (one frame per tick). On this
branch `hires_1280x240p` defaults to `false`, so the benchmark runs in the console's normal
480p/480i mode; an existing config file keeps whatever it already says.

## 1280x240p mode (Wii, this fork)

This fork adds an experimental **1280x240p** output mode for the Wii: a 240p signal with 1280
pixels per line (4:3), for scalers and digital video mods that pass the full horizontal
resolution through. It is on by default on the `wii-1280x240p` branch.

- Toggle it in the pause menu under RESTART OPTIONS > 1280X240P, or set
  `hires_1280x240p true`/`false` in `apps/sm64/sm64config.txt`. It takes priority over 240P.
- WIDESCREEN AUTO resolves to 4:3 in this mode.
- Tested on a Wii with a GCVideo-based digital output (Wii Dual). The analog component output
  (AVE-RVL) blanks this mode.

How it works: the Wii's embedded framebuffer is only 640 pixels wide, so each frame is recorded
once into display lists and drawn twice, once per 640x240 half. Each half uses a viewport the
size of the framebuffer, with the projection rescaled so its slice of the frame fills it, and is
copied into its half of a 1280-wide XFB. The VI scans that out with a 54 MHz pixel clock.

If the game can't find an SD card, it now keeps its config and saves on the USB drive.

## Enhancements Menu Options
| Enhancement    | Options                 | Description                                                                                                                                                                                                             |
|----------------|-------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 60FPS          | On/Off                  | Enable interpolated 60FPS mode.                                                                                                                                                                                         |
| WIDESCREEN     | On/Off/Auto/Pillarboxed | Enable/Disable widescreen. Auto goes based on Wii's system settings. Pillarboxing is for 4:3 on 16:9 displays to not get stretching.                                                                                    |
| 240P           | On/Off                  | Output at 240p over 480i/p [Requires restart to apply].                                                                                                                                                                 |
| 1280X240P      | On/Off                  | (This fork) Output 1280x240p (4:3), rendered as two 640x240 halves [Requires restart to apply].                                                                                                                       |
| ANTIALIAS      | On/Off                  | Enable/Disable antialiasing [Only applies when 240p is on and requires restart to apply].                                                                                                                               |
| INVERT CAMERA  | On/Off                  | Flip the camera movement caused by right and left c-buttons [Set to off if using Puppycam].                                                                                                                             |
| RUMBLE         | On/Off                  | Enable/Disable the Shindou version's rumble on any region.                                                                                                                                                              |
| FOG            | On/Off                  | Enable/Disable drawing fog in the distance. Also increases draw-distance for some objects.                                                                                                                              |
| FORCE NEAREST  | On/Off                  | Force nearest-neighbour over bilinear filtering for textures. Looks awful, do not use.                                                                                                                                  |
| VI DEFLICKER   | On/Off                  | Enable/Disable the Wii's deflicker filter. Enable this on non-CRTs for a cleaner image.                                                                                                                                 |
| PUPPYCAM       | On/Off                  | Enable/Disable Puppycam (See here on more information on what this is: https://github.com/FazanaJ/puppycam. This also makes the camera fully analogue, bypassing the c-buttons entirely except on Wii remote + Nunchuk. |
| SENSITIVITY X  | Number                  | Higher makes the camera more sensitive and thus faster in the X axis.                                                                                                                                                   |
| SENSITIVITY Y  | Number                  | Higher makes the camera more sensitive and thus faster in the Y axis.                                                                                                                                                   |
| INVERT X       | On/Off                  | Invert the left and right camera inputs.                                                                                                                                                                                |
| INVERT Y       | On/Off                  | Invert the up and down camera inputs.                                                                                                                                                                                   |
| STOPPING SPEED | Number                  | Controls how quickly the camera decelerates after the button/stick isn't being held. Higher means longer deceleration time.                                                                                             |
| CENTERING      | Number                  | How aggressively should the camera try to centre behind Mario. Higher is more aggressive.                                                                                                                               |
| PANNING        | Number                  | How far should the camera pan when the stick is pressed. Higher means more panning.                                                                                                                                     |
 
Everything is saved automatically upon changing the value. There's no need to save and restart unless you toggle 240p or Antialiasing.

## Known Issues

- Peach painting doesn't fade into Bowser
- Credits have clipping issues
- Docker build may be broken.

## Controls

| N64 control   | Wii Remote + Nunchuk   | Classic Controller | GameCube Controller          |
|---------------|------------------------|--------------------|------------------------------|
| Control stick | Nunchuk stick          | Left stick         | Control stick                |
| A             | A                      | A                  | A                            |
| B             | B or 2                 | B                  | B                            |
| Start         | + or -                 | + or -             | Start                        |
| Z             | Nunchuk Z or 1         | L                  | Z                            |
| L             | Not mapped             | Not mapped         | L                            |
| R             | Nunchuk C              | R                  | R                            |
| C-Up          | Wii Remote D-Pad Up    | Right stick up     | D-Pad Up or C-Stick Up       |
| C-Down        | Wii Remote D-Pad Down  | Right stick down   | D-Pad Down or C-Stick Down   |
| C-Left        | Wii Remote D-Pad Left  | Right stick left   | D-Pad Left or C-Stick Left   |
| C-Right       | Wii Remote D-Pad Right | Right stick right  | D-Pad Right or C-Stick Right |
| D-Pad         | Not mapped             | Not mapped         | Not mapped                   |

## Building

Successful compilation will result in a `boot.dol` being created in `build/VERSION_GX/boot.dol` where `VERSION` is one of `us`, `eu`, `jp` or `sh`, and `GX` is either `wii` or `cube`.

For Wii, place the `boot.dol`, `meta.xml`, and `icon.png` from the above `build/VERSION_GX/boot.dol` in a directory called `/apps/sm64` on your SD card and run using the [Homebrew Channel](https://wiibrew.org/wiki/Homebrew_Channel).
For Gamecube, copy the `boot.dol` to your SD card and run using Swiss or your preferred Gamecube homebrew launcher.

**Supported Build Methods:**

- [Docker](#docker) // May currently be broken
- [Linux / WSL (Ubuntu >= 18.04)](#linux--wsl-ubuntu)
- [Windows (MSYS2)](#windows-msys2)

### Docker
The following assumes a basic understanding of [Docker](https://www.docker.com/); if you do not belong to the `docker` group, prefix those commands with `sudo`.

**Install Docker:**
Follow the instructions here: [Docker Install](https://docs.docker.com/engine/install/ubuntu/) to get docker installed

**Clone Repository:**
```sh
git clone https://github.com/mkst/sm64-port.git --branch wii

cd sm64-port
```

**Copy in baserom.XX.z64:**
```sh
cp /path/to/your/baserom.us.z64 ./ # change 'us' to 'eu', 'jp' or 'sh' as appropriate
```

**Build with pre-baked image:**
Change `VERSION=us` if applicable.
```sh
docker run --rm -v $(pwd):/sm64 markstreet/sm64:wii make VERSION=us --jobs 4 # Linux/macOS
docker run -rm -v C:\path\to\sm64-port:/sm64 markstreet/sm64:wii make Version=us --jobs 4 # Windows. Replace /path/to with your actual path.
```

### Linux / WSL (Ubuntu)

These exact instructions work on **Ubuntu >= 18.04**\
This can be done on any Linux distro, just substitute the packages and DevkitPro section for your distro's equivalent.

```sh
sudo apt-get update && \
    sudo apt-get install -y \
        binutils-mips-linux-gnu \
        bsdmainutils \
        build-essential \
        libaudiofile-dev \
        pkg-config \
        python3 \
        wget \
        zlib1g-dev
````
Follow **Debian and derivatives** section of the DevkitPro install instructions over at [DevkitPro](https://devkitpro.org/wiki/devkitPro_pacman)

```shell
sudo dkp-pacman -Syu wii-dev gamecube-dev --noconfirm

cd

git clone https://github.com/mkst/sm64-port.git --branch wii

cd sm64-port

# Go and copy the baserom to C:\temp (create the directory in Windows Explorer)
# If you are running Linux natively, just copy the baserom.us.z64 file to the current directory. No need to create anything.
cp /mnt/c/temp/baserom.us.z64 ./

sudo chmod 644 ./baserom.us.z64

export PATH="/opt/devkitpro/tools/bin/:~/sm64-port/tools:${PATH}"
export DEVKITPRO=/opt/devkitpro
export DEVKITPPC=/opt/devkitpro/devkitPPC

make -j$(nproc) 
```
If the ```make -j$(nproc)``` command fails, try the Docker build in the same directory you are already in. If the Docker build then fails, run:

```shell
sudo -i
make -j$(nproc)
```
This is super hacky, but should get you a working executable. If you ever wish to rebuild you should only need to rerun the make command.

### Windows (MSYS2)

WSL is the preferred route, but you can also use MSYS2 (MINGW64) to compile.

For each instruction copy and paste the contents into the **MINGW64** console.

**Get MSYS2:**

Navigate to https://www.msys2.org/ and download the installer.

**Install and Run MINGW64:**

```
Next, Next, Next, Finish (keep the box checked to "Run MSYS 64bit now").
```

**Add keyserver for package validation:**

```sh
pacman-key --recv BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
pacman-key --lsign BC26F752D25B92CE272E0F44F7FD5492264BB9D0
```

**Add DevKitPro keyring:**

```sh
pacman -U --noconfirm https://downloads.devkitpro.org/devkitpro-keyring.pkg.tar.xz
```

**Add DevKitPro package repositories:**

```sh
cat <<EOF >> /etc/pacman.conf
[dkp-libs]
Server = https://downloads.devkitpro.org/packages
[dkp-windows]
Server = https://downloads.devkitpro.org/packages/windows
EOF
```

**Update dependencies:**

```sh
pacman -Syu --noconfirm
```

MINGW64 may close itself when done, if it does, find `MSYS2 MinGW 64bit` in your Start Menu and open again.

**Install Dependencies:**

```sh
pacman -S wii-dev git make python3 mingw-w64-x86_64-gcc --noconfirm
```

**Setup Environment Variables:**

```sh
export PATH=$PATH:/opt/devkitpro/tools/bin && echo "OK!"
export DEVKITPRO=/opt/devkitpro && echo "OK!"
export DEVKITPPC=/opt/devkitpro/devkitPPC && echo "OK!"
```

**Clone Repository:**

```sh
git clone https://github.com/mkst/sm64-port.git --branch wii
```

**Navigate into freshly checked out repo:**

```sh
cd sm64-port && echo "OK!"
```

**Copy in baserom.XX.z64:**

This assumes that you have create the directory `c:\temp` via Windows Explorer and copied the Super Mario 64 `baserom.XX.z64` to it.
```sh
cp /c/temp/baserom.us.z64 ./ && echo "OK!" # change 'us' to 'eu', 'jp' or 'sh' as appropriate
```

**Compile:**

```sh
make VERSION=us -j$(nproc)  # change 'us' to 'eu', 'jp' or 'sh' as appropriate
```

## Project Structure

    sm64
    ├── actors: object behaviors, geo layout, and display lists
    ├── asm: handwritten assembly code, rom header
    │   └── non_matchings: asm for non-matching sections
    ├── assets: animation and demo data
    │   ├── anims: animation data
    │   └── demos: demo data
    ├── bin: C files for ordering display lists and textures
    ├── build: output directory
    ├── data: behavior scripts, misc. data
    ├── doxygen: documentation infrastructure
    ├── enhancements: example source modifications
    ├── include: header files
    ├── levels: level scripts, geo layout, and display lists
    ├── lib: SDK library code
    ├── rsp: audio and Fast3D RSP assembly code
    ├── sound: sequences, sound samples, and sound banks
    ├── src: C source code for game
    │   ├── audio: audio code
    │   ├── buffers: stacks, heaps, and task buffers
    │   ├── engine: script processing engines and utils
    │   ├── game: behaviors and rest of game source
    │   ├── goddard: Mario intro screen
    │   ├── menu: title screen and file, act, and debug level selection menus
    │   └── pc: port code, audio and video renderer
    ├── text: dialog, level names, act names
    ├── textures: skybox and generic texture data
    └── tools: build tools

## Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.
