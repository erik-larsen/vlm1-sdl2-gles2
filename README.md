# VLM-1 — SDL2/GLES2 port

A from-source C port of Jeff Minter's **Virtual Light Machine** (the
light synthesizer built into the Atari Jaguar CD, 1995), targeting one
shared code path for native (macOS via ANGLE, Linux, Windows) and the
browser (Emscripten/WebGL).

The port is built against the reverse-engineered original source in
[mwenge/vlm](https://github.com/mwenge/vlm) (the "llamasource"
project), which reconstructs the 68000 + Jaguar GPU assembly with the
original symbol names. The goal is full fidelity: the engine operates
on the original big-endian fx-object images, the original preset data
(byte-identical to the shipped `vlm.abs`), and reproduces the original
fixed-point math.

## Status

- [x] Cross-platform build (Mac native + Emscripten), makefiles adapted
      from [sgi-demos](https://github.com/mipmap/sgi-demos)
- [x] SDL2/GLES2 platform layer: CRY16 framebuffer presented via a
      textured quad, CRY→RGB using the Jaguar TOM hardware chroma tables
- [x] Bank data pipeline: all 10 preset banks extracted from `banks.s`
      (byte-identical to original binary), `ifxobj`/`gm` decompressor
      ported and validated against reference dumps
- [x] Parameter animation engine: 8-oscillator bank, wave-link
      modulation matrix (`omega.gas` BigLoop64), ADSR envelopes
      (`doadsr`), spectrum band envelopes + beat triggers (`psi.gas`)
- [x] All preset-used effect modules: alpha (starfield/trail/ring),
      beta (DVF feedback), gamma (Gouraud polygons), delta (plasma),
      epsilon+theta (particles), shu (spectrum dots), and the dbeast
      delay-line ghost system (sigma/tau are unused by presets)
- [x] Live audio input: SDL capture (mic/loopback; getUserMedia in
      the browser) → 256-point FFT → the DSP's magnitude format, with
      a synthetic test-signal fallback (`A` key toggles)
- [x] Object-processor display window: the NTSC-visible 384x240 crop
      of the render buffer, with the per-effect `skale` zoom (1x/2x/4x)
      that beta drives from the preset's Thang parameter
- [x] Boot splash: the Jaguar CD wait screen (bank 10, plasma sunburst
      + the pulsing red JAGUAR logo via delta mode 3 / clut_sha),
      auto-advancing into the VLM; all delta submodes now ported
- [x] Pad-inertia user controls (symadj): arrows steer the px/py
      oscillator waveforms with hardware acceleration/friction
- [x] The hidden effects editor (core): entry combos, menu tree with
      the hidden 8-wrap root, live Spectrum & Triggers editing, and
      parameter sliders with the original widget displays
- [ ] Editor: waveform attach, source-wave editor, keypad screens

All 9 banks render their preset effects reacting to live audio; `V`
toggles a diagnostic scope of the animation engine. First launch asks
for microphone permission (deny it and the test signal drives the
visuals instead). To visualize system audio on macOS, route it
through a loopback device (e.g. BlackHole) set as default input.

## Building

Prereqs: SDL2 (`brew install sdl2`), Emscripten for the browser
target, Python 3 for the data extractors, and a checkout of
[mwenge/vlm](https://github.com/mwenge/vlm) at `../vlm-ref` (only
needed to regenerate `src/*_data.c`).

```sh
make native    # -> bin-<os>-<arch>/vlm
make browser   # -> web/vlm.html (needs emcc on PATH)
make           # both
```

Run: `make run-native`, or serve `web/` and open `vlm.html`.

Controls (matching the Jaguar pad mapping): keypad/number keys select
the effect, `8`(*) + digit selects a bank, `Z`/`X`/`C` are the A/B/C
buttons (trigger the ADSR envelopes), `O` option, `P` pause, arrows =
d-pad, Esc quits.

## Layout

- `src/` — the port. `engine.c` (animation core), `banks.c` (preset
  decompressor), `waves.c` (oscillator tables), `cry.c` (CRY→RGB),
  `platform.c` (SDL2/GLES2 glue), `*_data.*` (generated).
- `scripts/` — data extractors + validators (`extract_banks.py`,
  `extract_waves.py`, `validate_banks.py`).
- `docs/` — porting specs reverse-engineered from the original source:
  [68k engine](docs/analysis-68k-engine.md),
  [GPU effect modules](docs/analysis-gpu-effects.md),
  [bank data format](docs/analysis-banks-data.md).
- `libs/libgles/` — ANGLE dylibs (vendored from sgi-demos) so the
  native build uses real GLES2 on macOS.

## License / provenance

The original VLM is © Jeff Minter / Llamasoft (published 1995 by Atari
for the Jaguar CD); the reconstructed assembly source is preserved in
mwenge/vlm for study. This repository contains a new C implementation
written against that reference, plus data tables extracted from it,
for preservation and study. CRY conversion tables are the Jaguar TOM
hardware values as documented in the Jaguar Technical Reference and
Virtual Jaguar (GPL).
