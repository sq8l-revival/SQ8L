# SQ8L — 64-bit port

A faithful 64-bit port of **SQ8L 0.91b**, the free Ensoniq SQ-80 emulation by
**Siegfried Kullmann** (2006–2008), for modern macOS (Apple Silicon and Intel), Windows x64
and Linux (x86-64 and ARM64), as **VST2, VST3, Audio Unit (macOS), CLAP and LV2 (Linux)**.

SQ8L was a 32-bit Windows-only VST written in Delphi. Its source code was never released,
and the plugin could no longer run in today's 64-bit hosts. This project rebuilds it from the
original binary so that it can keep being played.

> **Unofficial project.** Not affiliated with Siegfried Kullmann, with Ensoniq or with its
> successors. See [Rights and credits](#rights-and-credits).

![The original editor (left, rendered from the original code) and the port (right)](docs/images/gui_original_vs_port.png)

## How faithful is it?

The port was not written "by ear": every part was translated from the original machine
code and verified against the original plugin running inside an x86 emulator.

- **Sound: bit-exact.** The complete engine produces exactly the same samples as the
  original: 1176 test renders (all 168 factory programs × 7 MIDI scenarios: chords,
  legato, controllers, aftertouch, sustain, voice stealing, random sequences), 120.8 million
  samples per channel, zero differences. Each DSP module (DOC 5503 oscillators, filter,
  LFOs, envelopes, amplifier, voice management) was also verified call by call on real
  captured states — tens of millions of calls in total.
- **Editor: pixel-exact.** The window uses the original artwork and the original drawing
  logic: across 162 editor states, 0 differing pixels (text excepted, see below). The page
  and parameter logic was checked against the original in 26,705 interactions.
- **Compatibility.** Same VST2 unique ID (`SQ8L`), same 512 programs and the same chunk
  format as the original: projects saved with the original plugin load with their sound.
  Library files (`.8XL`), bank files and SQ-80/ESQ-1 SysEx are read and written exactly
  like the original.

### Differences from the original

- Popup menus, message boxes, file pickers and the WRITE / MIDI-port dialogs use the
  operating system's native controls: on macOS they look like macOS. On Linux they are drawn
  inside the plug-in window (the file picker is the desktop's, or a simple built-in one).
- A few small additions to the editor: a left click on the program number opens the program
  list (the original needs a right or double click), and these items at the bottom of
  OPTIONS: *Polyphony* (see below), *Down arrow -> next program* (the original's hidden
  `swapProgUpDn` setting), *Ask before loading banks/libraries* (on by default, like the
  original), *Zoom* and *HD graphics* (see below).
- **Window size and HD graphics:** the editor can be enlarged from 100% to 300% with
  OPTIONS → *Zoom* (or the window's corner, where the host allows it); the original's pixels
  stay crisp at any size. OPTIONS → *HD graphics* draws the knobs, buttons, LEDs, both
  displays and the texts at the window's resolution instead; the panel behind them is still
  the original's picture, enlarged. Both settings are global (saved in `SQ8L.ini`).
- **Polyphony:** 8 voices like the SQ-80 by default, up to 32 with OPTIONS → *Polyphony*.
  With 8 the sound is bit-exact; with more, a performance changes only where the original
  would have stolen a voice. The output is not rescaled, so many voices sounding together
  are louder: lower the volume if needed. The setting is global (saved in `SQ8L.ini`) and
  changing it stops the notes that are playing.
- Text (status bar, program name) uses Liberation Sans, a free font metrically compatible
  with Arial, with anti-aliasing.
- SEND/REQ to a hardware SQ-80/ESQ-1 over MIDI ports are not connected yet (SysEx import and
  export through files work).
- Two original bugs could not be reproduced and were made safe: an integer division by zero
  in the envelope SHAPE code for a few rare level/velocity combinations (the original
  crashed), and an uninitialised byte read in the "AM bug" emulation when oscillator 2 is
  edited while a note plays (the port uses the intended value).
- As in the original, sample rates below 44.1 kHz are not supported.

## Installation

Download the archive for your system from the
[Releases](../../releases) page.

**macOS** (10.15 or later, universal): copy `SQ8L.component` to
`~/Library/Audio/Plug-Ins/Components`, `SQ8L.vst3` to `~/Library/Audio/Plug-Ins/VST3`,
`SQ8L.vst` to `~/Library/Audio/Plug-Ins/VST`, `SQ8L.clap` to `~/Library/Audio/Plug-Ins/CLAP`.
The plug-ins are not notarized by Apple, so macOS blocks them when they come from the
internet: remove the quarantine flag in Terminal, then restart your DAW.

```sh
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/SQ8L.component ~/Library/Audio/Plug-Ins/VST3/SQ8L.vst3 ~/Library/Audio/Plug-Ins/VST/SQ8L.vst ~/Library/Audio/Plug-Ins/CLAP/SQ8L.clap
```

**Windows** (10/11, 64-bit): copy `SQ8L.dll` (VST2) to your VST plug-in folder,
`SQ8L.vst3` to `C:\Program Files\Common Files\VST3`, `SQ8L.clap` to
`C:\Program Files\Common Files\CLAP`. If the old 32-bit `SQ8L.dll` is in the same folder,
keep a copy of it first: the files have the same name.

**Linux** (x86-64 or ARM64, X11 or XWayland): unpack `SQ8L-Linux-x64.tar.gz` (or `-arm64`)
and copy `SQ8L.so` (VST2) to `~/.vst`, `SQ8L.vst3` to `~/.vst3`, `SQ8L.clap` to `~/.clap`
and `SQ8L.lv2` to `~/.lv2`. Banks A and B and the options are kept in `~/.config/SQ8L`.

The user library (banks A and B) and the options are stored in
`~/Library/Application Support/SQ8L` (macOS), `%APPDATA%\SQ8L` (Windows) or `~/.config/SQ8L`
(Linux) as
`SQ8L_backup.dat` and `SQ8L.ini`, the same files the original kept in its folder. To use an
existing library, copy your old `SQ8L_backup.dat` there (or load it with *FILE → Load
library*).

The manual of the original (`readme.txt` in the SQ8L 0.91b archive) applies unchanged.

## Building from source

Requirements: CMake ≥ 3.22, Ninja, a C++17 compiler (Clang, GCC or MSVC), Git.

```sh
git clone --recursive <this repository>
cmake -S . -B build -G Ninja
cmake --build build
```

- macOS universal: add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15`.
- Linux: install the X11 and OpenGL development packages first (Debian/Ubuntu:
  `libx11-dev libxext-dev libxcursor-dev libxrandr-dev libgl-dev libdbus-1-dev`).
- Windows x64 from macOS/Linux with MinGW-w64:
  `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake` (Linux: use the `-posix` compilers).
- Plug-ins are written to `build/bin/`.

On Windows, build natively with Visual Studio 2022 instead (`cl.exe` or `clang-cl`, both
x64) through the presets in `CMakePresets.json`, which write to `build/<preset>/bin/`:

```sh
cmake --preset msvc        # or: clang-cl, msvc-debug, clang-cl-debug
cmake --build --preset msvc
```

In VS Code, pick the preset in the status bar and press F7; the CMake Tools extension
supplies the x64 developer environment. Run the presets from a Developer Command Prompt
otherwise. Building `third_party/core-math` with `cl.exe` needs `src/compat/core_math_msvc.hpp`,
which supplies the 128-bit integer type and the GCC builtins MSVC lacks.

Check that the engine still reproduces the original (no original files needed):

```sh
cmake --build build --target sq8l_render_check
./build/sq8l_render_check --regression tests/regression
```

This renders 243 MIDI cases and compares a hash of the output with the hash of the original
plugin's output.

## How it was made

- `oracle/` — a small Win32 emulator (Unicorn) that runs the **original** `SQ8L.dll` on any
  platform: it renders audio through the VST2 interface and even opens and drives the
  original editor (USER32/GDI emulated on a framebuffer). It is the reference for all tests.
- `re/scripts/` — analysis tools: Delphi class/RTTI recovery, form (DFM) decoding, Ghidra
  export per Delphi unit, data extraction.
- `tests/` — differential tests: real calls of the original routines are captured in the
  emulator and replayed on the C++ code, which must give identical results and state.
- `docs/` — [porting guide](docs/PORTING_GUIDE.md) (x87 floating point semantics, Delphi
  conventions, method), [GUI architecture](docs/GUI_ARCHITECTURE.md) and one document per
  module of the original (`docs/modules/`).
- `src/engine/` the synth engine, `src/gui/` the editor, `src/plugin/` the plug-in
  (DPF) and platform layers (macOS, Windows).

The emulator and the differential tests need the original plugin, which is **not**
included. Put `SQ8L.dll` (v0.91b, SHA-256
`ffbb70884a6cd2af9600d287deebb2ba8dc6ec2270150ceef04c23d0fc92ab0e`) and `SQ8L.ini` from
the original archive in `original/`, install `requirements.txt`, and run
`tools/prepare_reference.sh`.

## Rights and credits

**SQ8L** © 2006–2008 Siegfried Kullmann. Factory sounds in bank C by Ole Jeppesen and
Siegfried Kullmann; bank D contains the Ensoniq SQ-80 factory sounds. Thanks to Rainer
Buchty "for SQ80 resources & disassembled OS code", as in the original.

This port reproduces SQ8L's behaviour, artwork and data (including the SQ-80 wave ROM and
factory sounds that the original plugin contained) to preserve a freeware instrument that
could no longer run. It is free, non-commercial and published in good faith. The rights
to the original work belong to Siegfried Kullmann; Ensoniq, ESQ-1 and SQ-80 are
trademarks of their respective owners.

If you hold rights to any part of this work and object to its publication, please
[open an issue](../../issues) (you can ask there for a private contact) and it will be
addressed promptly.

Third-party components and their licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Licence of this repository: see [LICENSE.md](LICENSE.md).
