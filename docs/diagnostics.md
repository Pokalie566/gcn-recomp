# Diagnostics and measurement

How to drive a game without a controller, record and replay a run, measure the port, and
find out which guest code or which draw is responsible for something. Environment variables
are `GCN_*`; none of this is needed to play.

## State, settings and pausing

`GCN_SAVE_DIR=<dir>` keeps this machine's state for the game somewhere else than `saves/`:
memory card, settings.ini, shader cache, the prepared game. A scripted run
points it at an empty directory, so that it neither reads the player's settings nor touches
their card.

`GCN_PAUSE_AT=<frame>[:<ms>]` pauses at that presented frame for that long (1000 ms by
default), as Esc would, then resumes. A run paused that way must play out exactly as one
that is not: the guest stops at an interrupt poll, the virtual clock with it, and the time
paused is not caught up (`guest_pause` in runtime.h).

`GCN_QUIT_AT=<frame>` quits once that many frames have been presented: a scripted run then
ends on its own.

The interface -- launcher, pause menu, settings, saves -- can be checked without anyone at
the keyboard (runtime/ui/ui.h): `GCN_UI_INPUT=Down,Return,...` presses those keys in it, one
every 16 frames, as a player would; `shot:<name>` in the list writes the window to
`GCN_UI_SHOT/ui_<name>.png`, and `drop:<path>` drops a disc image on the launcher. With
`--no-launcher` and pausing at frame 300, `Down,Down,Return,Down,Right,Down,Return,Escape,Escape`
sets the internal scale to 3 and turns on 16:9, which settings.ini and the frames after the
pause must both show. A run that starts from the launcher and prepares the game from
scratch, as a player's first would, is `drop:<iso>,Down,Return,Return` with an empty
`GCN_SAVE_DIR`.

The launcher is skipped by `--no-launcher` and by any scripted run (`GCN_INPUT`, `--replay`,
`--headless`, `--prepare`), which then needs the disc to be there, and prepares the game first
if it must, reporting on stderr.

## The game module

The executable loads the recompiled game at startup (`runtime/game_module.h`): the
`GCN_GAME_MODULE=<file>` module if set, else the one prepared for the disc in
`saves/game/<dol sha1>-<inputs>/`, preparing it first if there is none (`runtime/prepare.h`,
with its tools' output in `prepare.log` beside it). `--prepare` prepares again and quits;
`GCN_KEEP_GEN=1` keeps the generated C there, in `gen/`, to read. `GCN_RESOURCES=<dir>`
points the preparation at an app's copy of the recompiler, tables, symbols and headers
(`recomp/`, `tables/`, `symbols.txt`, `include/`) instead of the source tree, and
`GCN_TOOLCHAIN=<dir>` at another toolchain than the build's.

## Input logging and replay

`--input-log=<dir>` records the controller state the guest polled, keyed by the poll's
sequence number, into `<dir>/inputs.txt`, with a snapshot of the memory card beside it.

`--replay=<dir>` plays a log back: the guest reads the logged controller instead of the live
one until the log runs out, then the live controller takes over. The run uses a scratch copy
of the logged card (`card_replay/` in the log directory), so the real card is never
touched and the replay sees the same saves the original did. A replay can itself be recorded
(`--input-log` again), so a route can be extended by replaying it and then playing on.

A replay is exact. Guest time is virtual (see "The clock" below): it is a function of what
the guest executes, not of the host's clock, so the Nth pad poll of the replay is the same
instant in the game as the Nth poll of the recording, every timed event (retrace, DVD
completion, decrementer) lands on the same instruction, and the RTC is started from the
recording's start time, which the log carries. A replay therefore reproduces the recording
bit for bit, including a crash at its end, unless the runtime itself has a nondeterministic
path -- which is then the bug to find. `--fast` runs a replay as fast as the host allows;
the game still sees 60 Hz.

The log format is text: a header (version, game, start time and its epoch), then one line
per change of a channel's state --
`poll frame chan connected buttons stick_x stick_y cstick_x cstick_y trig_l trig_r`, in
hex -- and, when the run ended in a way the runtime saw (a normal exit, a fault or an
interrupt), a final `# end <poll> <frame>`. A replay holds the last
state up to that end before handing over to the live pad; without the marker it hands over
at the last change, which can be well before where the run stopped, so a log that lacks
one (a run killed from outside) can be given one by hand. Lines can be edited or written
by hand; `frame` is for the reader.

## Scripted input

`GCN_MEMCARD=<dir>` runs on another memory card than the player's (a card folder, card.h),
made empty if it does not exist: a scripted run should start from an empty card, and must not write to the
real one. `GCN_RTC=<unix time>` starts the real-time clock at a fixed date
instead of now: a game that seeds randomness from it plays a scripted run differently every
time otherwise.


`GCN_INPUT="frame:BUTTON:duration,..."` presses buttons at given submitted-frame counts; `+`
joins buttons (`A+B`), `SL SR SU SD` push the control stick and `CL CR CU CD` the C stick,
all the way or by the percentage that follows (`CR30`). Presses are counted in
presented game frames, not wall-clock. `GCN_INPUT_LOG=1` prints each press as it fires. A
scripted press is recorded in the input log like any other, so a script is one way to make
a log.

## Frame dumps

`--dump-dir=DIR --dump-every=N` writes every Nth presented frame as a PNG, read back from
Aurora's framebuffer. `GCN_DUMP_RANGE=a-b` restricts that to a window of frames.
`--headless` runs the guest with no window and no GPU at all, the stream dropped.

With `--interpolate`, an in-between frame is dumped as `frame_NNNNN_i.png`, before frame
NNNNN; `GCN_INTERPOLATE=force` makes one for every frame of an unpaced run, and
`GCN_INTERPLOG=1` prints each frame's verdict. See graphics.md, "Aurora".

## The clock

Guest time is virtual. Every backward branch in recompiled code counts one
back-edge, and the time base is that count times a constant (`TICKS_PER_EDGE` in
`cpu.cpp`); events are due when the count reaches them, and the SDK's idle spin
(`idle.txt` in the game's tables) jumps the count to the next event instead of iterating.
The host is held to real time by sleeping when virtual time runs ahead; when the host
cannot keep up, the game slows down rather than catching up later. So the game's view of
time depends only on what it executed, which is what makes replays exact; the cost is that
a scene heavier than a frame's budget of back-edges looks to the game like a dropped frame,
at a threshold set by the constant rather than by the real console.

`GCN_TIMESCALE=N` paces virtual time at N× real time (0: unpaced, as `--fast`).

`GCN_FRAMETIME=1` prints a line per frame from the GX bridge: the guest's time for the
frame, its draws, vertex bytes and EFB copies, and how long it waited on the GPU side. The GPU
side adds, once a second, how many frames it showed (`[present] N frames/s`, the
in-between frames of 60 fps included).

## Finding a draw or a function

- The GX bridge (`gx/aurora_bridge.cpp`): `GCN_DRAW_SKIP=a-b` drops
  draws a to b of every frame, `a-b,c-d` several ranges, `!a-b,c-d` all but those (keep the
  frame's last draws too when the game redraws the frame from a copy at its end), `srtg` the
  draws with a colour texgen; `GCN_DRAW_LOG=<n>`
  prints draw n's state (gen mode, Z, blend, alpha test, TEV stages, texgens, channels, TEV
  registers and konst colours -- which share their BP addresses -- fog); `GCN_DRAW_LOG_FRAME=<frame>` prints, for that frame, its colour-texgen draws, its EFB
  copies (rect, format, destination) and every texture it loads (address, size, format), so
  a copy can be followed to the draws that sample it. Frame numbers are the bridge's
  (`g_frames_submitted`): one less than the dump's.
- `--sample` reports once a second which guest functions each guest thread is in, by
  walking the guest's own stack; names come from the game's `symbols.txt`.
- Build with `-DGCN_TRACE_CALLS=ON` to keep a real guest call stack, printed by the crash
  dump (`GCN_TRACE_DEPTH` sets how many frames).
- `--log-all` enables every log category, including the DSP mailbox and DVD reads;
  `--log=exi,dsp` enables just those (`cpu os hw dvd gx vi si exi dsp ai thr`).

## Panics and faults

A game's `OSPanic` should be listed in its `hle.txt`: the replacement prints the message and
a guest backtrace, and exits, instead of spinning in `PPCHalt` looking like a
hang. A host fault inside guest memory prints the guest address and every thread's
registers.

Everything on stderr also goes to `<save dir>/<game>.log` (`crash_log.h`); the run before's
is kept as `<game>.old.log`. A fault or a `fatal()` (a panic included) ends with a `CRASH:`
line there and a dialog for the player, shown by the executable started again with
`--crash-report=<text>` (the crashed process may not be able to draw). `GCN_CRASH_TEST=1`
faults on purpose once the menus are up (`=fatal`: a fatal error); `GCN_CRASH_DIALOG=print`
prints the dialog's text instead of showing it, `=0` shows nothing.
