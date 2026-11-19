Super Mario 64 - native Wii port (tester build)
================================================

A native Wii build of Super Mario 64 (USA), compiled from the sm64 decompilation
(the sm64-port PC port, with a GX renderer for the Wii).

No game data is included. Put your own Super Mario 64 (USA) ROM in apps/sm64/
as sm64.z64 (.v64 / .n64 byte orders work too). Every time it starts, the game
loads its textures, sounds, animations and models from that file. Without it
(or with another version of the game) it shows a message and goes back to the
Homebrew Channel after 30 seconds.

Install
-------
Copy the apps folder to the root of the SD card (or USB drive), put the ROM in
apps/sm64/ as sm64.z64, and start "Super Mario 64" from the Homebrew Channel.
Saves go to apps/sm64/sm64_save_file.bin, settings to apps/sm64/sm64config.txt.

Controls
--------
GameCube controller (A/Y = A, B/X = B, Start, Z, L, R; C-stick or D-pad =
C buttons), Classic Controller, Wii Remote + Nunchuk, or a real N64 controller
through a passive GameCube-port adapter. HOME exits to the Homebrew Channel;
the console's Reset button goes back to the Wii Menu.

Settings (apps/sm64/sm64config.txt, one "name value" per line)
--------------------------------------------------------------
video_mode      default (the console's own 480p/480i), 720p, 960x720p, 1280x960p ...
                (the HD modes need a digital video output, e.g. an HDMI mod)
widescreen      auto (follows the Wii's 16:9 setting), true, false, pillarbox
60fps           true: 60 fps interpolation; false: the original 30 fps
fog             false turns off distance fog
invert_camera   true / false
rumble          true / false
bench_demo      true runs the attract-mode demos as a benchmark (input ignored)
bench_overlay   true shows frame times on screen

Please report problems with what you did, what you saw and the video mode.
