# VLM-1 68000 Engine Analysis — Port Specification

Analysis of Jeff Minter's Virtual Light Machine (Atari Jaguar, 1994) from the
reconstructed sources at `/Users/larsens/Github/vlm-ref/src/vlm/`. This is the
primary spec for the SDL2/GLES2 C port.

Source inventory (all paths relative to `/Users/larsens/Github/vlm-ref/src/`):

| File | Role |
|---|---|
| `vlm/vlm.s` (8562 lines) | 68k main program: init, vsync ISR, object list, editor UI, bank loading |
| `vlm/vlm.inc` | All RAM addresses and fxobj field offsets (original symbol names) |
| `vlm.equ` | External entry points/addresses exported to the CD front-end |
| `vlm/ians.s` | DSP (Jerry) boot: loads Ian Bennett's FFT code into DSP RAM |
| `vlm/sines.s` | 8-bit signed sine table (257 entries used) + 16-bit table |
| `vlm/vidinit.s` | NTSC/PAL video timing, `_fsync`/`_fmode`/`_ein_buf` mailbox |
| `vlm/vlmgpu.s` | GPU module loader (`gpuload`/`gpurun`) + `gpumods` table + embedded module binaries |
| `vlm/gpu/omega.gas` | **The real main loop** (runs on GPU): per-frame parameter animation + module dispatch |
| `vlm/gpu/psi.gas` | Per-frame postamble: FFT→envelope/trigger processing + edit-mode draw server |
| `vlm/gpu/dbeast.gas` | Per-sub-effect preamble: delay-line (trail) recorder/playback driver |
| `vlm/gpu/alpha/beta/gamma/delta/epsilon/theta/shu/sigma/tau.gas` | The drawing modules (one per sub-effect type) |
| `vlm/banks.s` | Compressed data for all 9 banks x 9 effects x 6 sub-effects |
| `vlm/images.s`, `vlm/font.s` | `polyos` polygon shape data, logos, 8x8 font |

---

## 1. Overall runtime structure

### 1.1 Entry points

- `LaunchVLM` / `free` (`vlm.s:227`): "free-run" boot path (Jaguar attract
  screen). Sets `skid=5`, `imatrix=9` (bank 10, the splash bank), `freerun=1`,
  calls `everything`, returns to the CD front end.
- `audio` (`vlm.s:245`): the audio-reactive VLM proper. Sets `skid=4`,
  `imatrix=0` (bank 1 effect 4 is the power-on effect), falls into
  `goagain` (`vlm.s:263`) which clears `freerun` and loops on `everything`
  forever.

### 1.2 `everything` (`vlm.s:281`) — one-time world construction

1. `gogo` (`vlm.s:524`) → `startup` (`vlm.s:1907`) if `started==0`:
   - `VideoIni` (`vidinit.s:44`): NTSC 384-wide timing; sets `pal`, `n_vdb`,
     `n_vde`, `width2`, `height2`, HDB/HDE/VDE registers.
   - `InitList` (`vlm.s:5957`): builds the two Object Processor lists
     (`list1`→`dlist`, `list2`→`blist`, `ddlist` = OLP head with branch
     objects for vdb/vde).
   - Installs `Frame` as the level-0 autovector (`move.l #Frame,($100).w`,
     `vlm.s:1935`), `VI = n_vde|1` (`vlm.s:1938`), `intmask=1` → **only the
     video interrupt is enabled**. `VMODE = $6C1` (`vlm.s:1944`) = CRY16,
     PWIDTH=4, i.e. a ~384-pixel-wide 16-bit CRY display.
   - Clears both framebuffers `$100000` (`cscreen`) and `$148000`
     (`dscreen`); `draw_screen = dscreen` (`vlm.s:1947-1953`).
   - Builds the waveform tables `p_sines`, `p_rect`, `p_saw`, `p_ramp`,
     `p_square` from `sines` (`vlm.s:1956-2001`): each is 256+1 **unsigned
     bytes**; `p_sines[i] = sines[i]+$80` (biased), `p_rect` = |sine|*2,
     ramp/saw/square as named.
2. `iansdoit` (`ians.s:30`) — boots the DSP FFT (audio mode only).
3. Creates 6 pristine fxobjs in `refblock` and 6 in `fxspace` via `ifxobj`
   (`vlm.s:292-306`, `450-457`); `fx1/fxobj/fxedbase/i_fxobj` point into
   `fxspace`.
4. `zapdel` (`vlm.s:1261`) initialises the two delay lines; `jsr gm` unpacks
   the current bank into `matrix` and loads effect `skid` (see section 5).
5. Builds the `beasties` display-object backing list (main screen object,
   edit `board` overlay, Jaguar logo, Dave's CD-controls overlay, VLM logo)
   (`vlm.s:336-433`), clears `envvals`, sets `routine=symadj`, and jumps to
   `titlescr`.

### 1.3 `titlescr` (`vlm.s:1474`) — hand-off to the GPU

- Fills `dvf_buf` (the shared DVF parameter packet) from the first fxobj.
- Populates the GPU mailbox `in_buf` ($F03F60, GPU RAM):

  | offset | contents (`vlm.s:1511-1518`) |
  |---|---|
  | +$00 | `lol` — pointer table: `bmobj, skale, beasties, pobjs, mpobjs, mmasks, maxes, absdelta, zerstart, avbank, envvals, _fsync, freerun` (`vlm.s:8341`) |
  | +$04 | `oscvals` |
  | +$08 | `fx` (pointer to the 6-slot table of active fxobj pointers) |
  | +$0C | `results` |
  | +$10 | `pbinfo` (parameter metadata table, section 3.4) |
  | +$14 | `delayf` (delay-line settings block) |
  | +$18 | `polyos` (polygon shape table, `images.s:24`) |
  | +$1C | `cobuf` |

- `mods = gpumods`, then `gpurun` loads and starts `omega` on the GPU
  (`vlm.s:1520-1524`, `vlmgpu.s:88`). **The GPU never returns**; `omega`'s
  `RunFXObjModulesLoop` is the frame loop for the rest of the session.
- The 68k then sits in `editloop`/`eloop` (`vlm.s:1526-1543`): in
  audio-reactive mode (`freerun==0`) it waits for `_fsync` (set once per GPU
  frame by `psi.gas`), runs `yakedit` (editor/action processing,
  `vlm.s:1552`) and `davesvec` (the CD front-end service hook), clears
  `_fsync`, repeats. In free-run mode it returns to the caller.

### 1.4 The three concurrent contexts

| Context | Code | Cadence |
|---|---|---|
| 68k VI interrupt | `Frame` (`vlm.s:4244`) | every vertical interrupt (VI at line `n_vde|1`; `frames++` per VI) |
| 68k mainline | `eloop`/`yakedit` (`vlm.s:1533`) | once per **GPU** frame, gated by `_fsync` |
| GPU (Tom) | `RunFXObjModules` (`omega.gas:125`) | free-running; one iteration = one rendered frame |
| DSP (Jerry) | Ian's FFT (`ians.s`) | continuous, on CD audio I2S input |

### 1.5 Frame pacing and buffer swap (the `sync`/`screen_ready` handshake)

- GPU (`omega.gas:133-147`): sets `sync=1`, then busy-waits until `Frame`
  clears `sync` to 0; then renders the whole frame into `draw_screen`
  (= `dscreen`), and finally sets `screen_ready=1` (`omega.gas:566`).
- 68k `Frame` on each VI (`vlm.s:4284-4296`): if `screen_ready && sync`,
  swaps `cscreen <-> dscreen`, sets `draw_screen = old cscreen`, clears both
  flags. Then patches the data pointer of the first (main) object in
  `dlist` to `cscreen` (`stdb`, `vlm.s:4312-4322`).
- Consequence: rendering is **not** locked to 60 Hz. The effect runs as fast
  as the GPU can draw a frame, latching to the next VI. Heavy effects
  degrade gracefully. A port should reproduce this: simulation steps once
  per rendered frame, presentation at vsync.
- Additionally `Frame` copies `blist`→`dlist` ($41 longs, `vlm.s:4266-4271`)
  and rebuilds `blist` via `RunBeasties` every VI.

`Frame` also: handles the C-button strobe flash (`flash`, `scron`, BG
register, `vlm.s:4324-4351`), the VLM-logo timer (`vlmtim`), joypad reading
(`readpad`, `vlm.s:6556` — active-high bitmask documented at
`vlm.s:6529-6554`), keypad bank/effect selection (section 5), edit-mode
dispatch through `editvex`, and finally calls the two per-frame vectors
`_fx` and `routine` (normally `symadj`) (`vlm.s:4466-4477`). `CheckTimer`
(`vlm.s:4483`) tests INT1 bit 3 (timer) but the branch is empty — **the
timer interrupt is unused**; only VI matters.

### 1.6 `symadj` and the inertial controllers

`symadj` (`vlm.s:859`) runs in the ISR each VI:
- `doadsr` (`vlm.s:906`): steps the three ADSR envelopes (A/B/C buttons) —
  see section 3.5.
- Feeds pad direction bits through `pinertco` into `pixcon`/`piycon`, then
  derives `px = pixcon>>8 & $FFFF`, `py = piycon>>8 & $FFFF`
  (`vlm.s:876-896`) — these are the "User control X/Y" waveform sources.

`inertcon`/`pinertco` (`vlm.s:6729-6834`) implement an inertial slider.
Block layout (see `pixcon` initial data at `vlm.s:8432`):

| off | field | initial |
|---|---|---|
| +0 | value (24-bit used; value>>8 = 0..$FFFF output) | $800000 |
| +4 | velocity | 0 |
| +8 | acceleration per tick | $1600 |
| +12 | velocity cap reference | $3000 |
| +16 | max value | $FFFF00 |
| +20 | min value | $000000 |
| +24 | friction | $40000 |

`ixcon`/`iycon` (`vlm.s:8492`) are the same structure used by the editor for
parameter sliders (accel `UPDA2=$400`, cap $20000, friction $20000).

---

## 2. Core data structures

### 2.1 The fxobj (sub-effect parameter block)

- One **effect** = 6 sub-effect slots; `fx` is a 6-entry pointer table
  (entries point into `matrix`, or 0 for `<Empty>`).
- One **fxobj** = 1024 bytes. High-level layout (`ifxobj`, `vlm.s:1808-1901`;
  also `banks.s` header):

| bytes | contents |
|---|---|
| 0-255 | 64 longword parameters (table below) |
| 256-511 | 64 **wave-link words** (one long per parameter; init `$0000FFFF`) |
| 512-767 | 64 **base-value slots** (captured parameter value at link time; init 0) |
| 768-895 | 8 x 16-byte **oscillator bank** (copy of `oscbank`, `vlm.s:8423`) |
| 896-1003 | copy of the 108-byte `pixcon` block: `pixcon`(28) `piycon`(28) `adsra`(10) `adsrb`(10) `adsrc`(12) `py`(4) `px`(4) `delayf`(2) `delayp`(2) `dline`(4) `delayt`(2) `delayn`(2) |
| 1004-1023 | unused |

Per-effect persistence: `skidoo` copies fxobj0+896..1003 back over the live
`pixcon` block when an effect loads (`vlm.s:4631-4636`), and `dofass` copies
the live block into the fxobj before assigning an effect to a keypad slot
(`vlm.s:4918-4934`). So pad-inertia tuning, ADSR shapes and delay-line
settings are part of each stored effect.

### 2.2 Parameter table (fxobj bytes 0-255), field by field

Index = long index (byte offset /4). Offsets are the `vlm.inc` equates.
Defaults from `ifxobj` (`vlm.s:1816-1871`); names/semantics from `pbinfo`
(`vlm.s:7722-8175`) and the `ifxobj` comment block (`vlm.s:1742-1806`).
"max" is the `pbinfo` full-scale used by the modulation math (section 3.4);
type `$8x` = signed/centered; b/w = byte/word display type.

| # | off | symbol | default | editor max/type | meaning / format |
|---|---|---|---|---|---|
| 0 | $00 | — | 0 | — | scratch: omega stores `dscreen`/`draw_screen` here each pass (`omega.gas:219-223, 264-270`) |
| 1 | $04 | `dvf_ws_x` | 0 | $1800000 w | DVF window size X; used as `>>16` pixels |
| 2 | $08 | `dvf_ws_y` | $E00000 | $1800000 w | DVF window size Y |
| 3 | $0C | `vfb_xsca` | $1850000 | $3FFFFFF w | DVF scale X; `>>16` gives 8.8 zoom factor (1.0=$100; $185~1.52) |
| 4 | $10 | `vfb_ysca` | $1850000 | $3FFFFFF w | DVF scale Y |
| 5 | $14 | `vfb_angl` | 0 | $1FFFFFF +/-w | DVF rotate angle; `>>16` gives 8.8 angle in 256-step circle |
| 6 | $18 | — | $C00000 | $1800000 w | DVF centre of rotation X (16.16 px, screen 0..384) |
| 7 | $1C | — | $C00000 | $1800000 w | DVF centre of rotation Y |
| 8 | $20 | — | $780000 | $FFFFFF w | DVF delta intensity (feedback decay) |
| 9 | $24 | `dstoffx` | $C00000 | $1800000 w | destination position X, 16.16 px ($C00000=192.0=centre) |
| 10 | $28 | `dstoffy` | $C00000 | $1800000 w | destination position Y |
| 11 | $2C | `vfb_xpos` | $C00000 | $1800000 w | DVF window centre X |
| 12 | $30 | `vfb_ypos` | $C00000 | $1800000 w | DVF window centre Y |
| 13 | $34 | `dstoffz` | 0 | $7FFFFFF w | destination Z (16.16) |
| 14 | $38 | `vectorx`/`d_x` | 0 | $FFFFFF +/-w | vector X (16.16, signed-centered) |
| 15 | $3C | `dy` | $200000 | $1800000 +/-w | destination Y offset |
| 16 | $40 | `vectory`/`d_z` | 0 | $FFFFFF +/-w | vector Y |
| 17 | $44 | — | 0 | bitfield | symmetry planes/types; edited via `init_sym` |
| 18 | $48 | `rsym_ord` | 8 | $100 b | rotational symmetry order (integer count) |
| 19 | $4C | `rsym_ste` | $80000 | $FFFFFF w | rotational angle step (16.16 of 256-step circle) |
| 20 | $50 | `rsym_ist` | 0 | $FFFFF +/-w | rotational angle step delta |
| 21 | $54 | `_i1` | $8000 | $FFFF w | intensity 1 (0.16 fraction) |
| 22 | $58 | `_i2` | $8000 | $FFFF w | intensity 2 |
| 23 | $5C | `_i3` | $8000 | $FFFF w | intensity 3 |
| 24 | $60 | `_i4` | $8000 | $FFFF w | intensity 4 |
| 25 | $64 | `_i5` | $8000 | $FFFF w | intensity 5 |
| 26 | $68 | `_i6` | $8000 | $FFFF w | intensity 6 |
| 27 | $6C | `zamp` | $400 | $FFFF b | Z amplitude |
| 28 | $70 | `phase1` | 0 | $1FFFFFF +/-w | fixed-point phase 1 (`>>16 & $FF` = table index; see `gamma.gas` usage) |
| 29 | $74 | `phase2` | $400 | $1FFFFF +/-w | delta phase 1 |
| 30 | $78 | — | 0 | $1FFFFFF +/-w | rotational sym overall phase |
| 31 | $7C | `phase4` | $400000 | $1FFFFFF +/-w | fixed-point phase 2 |
| 32 | $80 | `i` | $140000 | $FFFF w | number of iterations (16.16). **Overloaded:** for DVF objects `initdvf` stores the `dvf_buf` pointer here (`vlm.s:818`) |
| 33 | $84 | `j` | $400 | $FFFF b | X amplitude |
| 34 | $88 | `k` | $400 | $FFFF b | Y amplitude |
| 35 | $8C | — | 0/1 | $1000000 w | number of other iterations; for DVF: 0 = clear feedback area, 1 = no clear (`makefb`/`makecfb`, `vlm.s:588-599`) |
| 36 | $90 | `col1` | $8000 | — | colour 1 |
| 37 | $94 | `deltaz` | 0 | $FFFFFF +/-w | delta Z |
| 38 | $98 | `thang` | 0 | $FFFFFF w | "choice of Thang": DVF pixel-mode variant (1 = scale2, 2 = scale4; `mdvf2`/`mdvf4`, `vlm.s:560-582`); low 3 bits select `polyos` shape in gamma (`gamma.gas` header block) |
| 39 | $9C | — | 0 | — | undefined |
| 40 | $A0 | `asym_fla` | 3 | — | symmetry flags actually used by GPU modules (byte $A1 bit0 = rot-sym on, byte $A3 = 8 mirror-plane bits; `sp1..sp9`/`syminv`, `vlm.s:1304-1346`) |
| 41 | $A4 | `sine_bas` | `p_sines` | — | waveform table pointer #1 (source function) |
| 42 | $A8 | `rxcen` | $C00000 | $1800000 w | rotational sym centre X (16.16 px) |
| 43 | $AC | `rycen` | $C00000 | $1800000 w | rotational sym centre Y |
| 44 | $B0 | `roscale` | $800 | $3FFF +/-b | rotational symmetry scale |
| 45 | $B4 | `roscal2` | 0 | $3FF +/-b | rotational scale delta X |
| 46 | $B8 | `cvx` | $1000 | $7FFFF +/-w | colour generator vector X |
| 47 | $BC | `cvy` | $20000 | $7FFFF +/-w | colour generator vector Y |
| 48 | $C0 | `roscalei` | 0 | $1000 +/-b | rotational scale delta Y |
| 49 | $C4 | `drxcen` | 0 | $FFFFF +/-w | rotational centre delta X |
| 50 | $C8 | `drycen` | 0 | $FFFFF +/-w | rotational centre delta Y |
| 51 | $CC | `phase5` | $21555 | $1FFFFF +/-w | delta phase 2 |
| 52 | $D0 | `wave_2` | `p_sines` | — | waveform table pointer #2 |
| 53 | $D4 | `radius` | $80000 | $FFFFFFF +/-w | radius (16.16) |
| 54 | $D8 | `cvx2` | $10000 | $1FFFFF +/-w | base colour generator vector X |
| 55 | $DC | `cvy2` | $10000 | $1FFFFF +/-w | base colour generator vector Y |
| 56 | $E0 | `colx` | $8F000 | $1FFFFF +/-w | base colour X (CRY colour-space coordinate) |
| 57 | $E4 | `coly` | $8F000 | $1FFFFF +/-w | base colour Y |
| 58 | $E8 | `plot_mod` | 0 | enum | destination plot routine index (pixel-plot variant tables inside alpha/epsilon at GPU RAM $F039B0/$F039C0) |
| 59 | $EC | `pixsize` | 0 | $7FFFFF w | maximum pixel size |
| 60 | $F0 | `height` | 0 | — | **overwritten every frame with `trig`** (trigger word) by omega (`omega.gas:273-278`) — this is how effects see audio triggers |
| 61 | $F4 | `_mtrig` | 0 | bitfield | trigger mask: which triggers the effect responds to (bits set by `bp1..bp8`, `vlm.s:1348-1378`) |
| 62 | $F8 | `info` | 0 | enum 1..14 | sub-effect type; index into `vars` (`vlm.s:8179`); 0 = empty slot |
| 63 | $FC | `gpu` | 0 | — | `(module<<16) | variant`: high word indexes `gpumods` (`vlmgpu.s:113`), low word is a sub-variant passed to the module via GPU RAM `$F03F5C` (`omega.gas:664-670`) |

`info` values → `vars` names (`vlm.s:8179-8263`): 1 poly object (gamma), 2
3D starfield (alpha), 3 ring of pixels (alpha v9), 4 DVF area (beta), 5
WaveSurf (alpha v2), 6 psychedelic bitmap (delta), 7 i-shaded bitmap (delta
v1), 10 colour plasma (delta v2/v4), 11 Jaguar logo sprite (delta v3), 12
particle object (epsilon), 13 particle motion (theta), 14 spectrum as
intensities (shu). Creation routines: `makepoly`, `makecfb`/`makefb`,
`makering`, `makestar`, `amkesurf`, `makemono`, `makeiri`, `mdvf2/4`,
`mplaz1/2`, `mjaglogo`, `mpo`, `ipm`, `itau`, `mopo`, `makeshun`
(`vlm.s:495-738`).

### 2.3 Wave-link word format (fxobj + 256 + 4*i)

Set by `wlink` (`vlm.s:844`): copies the parameter's current value into the
base slot (+512+4*i) and stores the link long at +256+4*i.

```
bit 31..24  envelope-select bits: env k attaches envvals[k]    (ant1..ant8, vlm.s:5107-5151)
bit 23..16  oscillator-select bits: osc k attaches oscillator k (lul1..lul8, vlm.s:5056-5102)
bit 15..0   amplitude, 0..$FFFF (edited via ixcon -> word at fxobj+$102+4*i; wset, vlm.s:5031-5037)
```

A parameter is animated **iff the high word is non-zero** (checked at
`omega.gas:400-404`; mirrored by the editor's `s16`/`lnkchk`,
`vlm.s:3835-3847`, `2875-2884`, which display the +512 base slot instead of
the live slot for linked parameters).

### 2.4 Oscillator bank entry (fxobj + 768 + 16*k, k = 0..7)

From `oscbank` (`vlm.s:8423`) and `omega.gas:307-382`:

| off | field |
|---|---|
| +0 | phase accumulator (table index = `(phase>>16) & $FF`, lerp fraction = `(phase>>8) & $FF`; one waveform cycle = $1000000) |
| +4 | delta (added per frame; editor step $1000, clamped to $1FFFFF — `stinc`/`zinc`, `vlm.s:5427-5439`) |
| +8 | waveform table pointer (rewritten from type on effect load, `skidoo yuz`, `vlm.s:4616-4625`) |
| +12 | waveform type (long; 68k editor writes the word at +14 — `swf1..swf9`, `vlm.s:5445-5494`) |

Types → tables (`wts`, `vlm.s:7666`; `sineslookup`, `vlm.s:7677`): 0 sine
(`p_sines`), 1 sawtooth (`p_saw`), 2 square (`p_square`), 3 ramp (raw phase,
no table), 4 rectified sine (`p_rect`), 5 noise (`rantab`, `vlm.s:7394`), 6
constant (delta forced 0), 7 user control Y (`py`), 8 user control X (`px`).

### 2.5 Global fx bookkeeping

- `fx` ($1AE536): pointer to the live 6-slot table (`fx1` = `fxspace`).
- `fxobj`, `fxedbase`, `i_fxobj`, `fxed`, `og`: current edit target slot.
- `refblock` ($1B4BF8): 6 pristine fxobjs (compression reference).
- `matrix` ($1D83F8): 9 effects x 6144 bytes = the decompressed current bank.
- `results` ($1E81F4): 64 words per frame — raw modulation scalar per
  animated parameter (written at `omega.gas:518`, displayed by the editor
  scope `domonito`, `vlm.s:2387`).
- `oscvals` ($1E8094): 8 words, current oscillator outputs (per fxobj pass).
- `envvals` ($1E80B4): 8 words: [0..4] spectrum band envelopes, [5] ADSR C,
  [6] ADSR B, [7] ADSR A (`doadsr` writes envvals+10/+12/+14,
  `vlm.s:912-930`).

---

## 3. The parameter animation system (heart of VLM)

All of this runs **on the GPU** in `omega.gas`, once per fxobj per frame,
*after* the fxobj's drawing module has run (`PostMainModuleRun` →
`UpdateOSCBank` → `OSCValsLoop` → `BigLoop64`).

### 3.1 Oscillator phase update — `UpdateOSCBank` (`omega.gas:307-330`)

For each of the 8 oscillators in the fxobj's oscbank:
`phase += delta` (unless type == 6 "constant", where delta is treated as 0).

### 3.2 Oscillator output — `OSCValsLoop` (`omega.gas:336-382`)

For each oscillator k, compute a 16-bit unsigned output into `oscvals[k]`:

- type >= 7 (user controls): output = `py`/`px` (located as
  `&delayf - 8 + 4*(type-7)`; `omega.gas:353-359`).
- type == 3 (ramp): output = `phase >> 8` (linear 0..$FFFF).
- otherwise, table lookup with linear interpolation:

```
idx  = (phase >> 16) & 255
frac = (phase >> 8) & 255
a = table[idx]; b = table[idx+1]           // unsigned bytes
oscvals[k] = (a << 8) + (b - a) * frac
```

Tables are biased unsigned, so $8000 ~ zero (`p_sines[i]=sine+$80`,
`startup`, `vlm.s:1961-1971`).

### 3.3 Modulation accumulation — `BigLoop64` inner loop (`omega.gas:400-461`)

For each of the 64 parameters whose link high word != 0:

```
acc   = 0
w     = $7FFF                    // running wave term
env   = $FFFE                    // default envelope multiplier (~1.0)
link  = fxobj[256+4i]            // 32-bit link word
sel   = link >> 16               // osc bits 0-7, env bits 8-15
for k = 0..7:
    if sel bit k:                // oscillator k attached
        w = (w * (oscvals[k] - $8000)) >> 16     // signed 0.16 multiply
        acc += w
    if sel bit (k+8):            // envelope k attached
        env = envvals[k]         // last one selected wins
```

Note the deliberate cascade: `w` is *not* reset between oscillators, so
attaching several waves ring-modulates them (each successive wave multiplies
the running product). `TOPNEN` = $8000 (`blitter.inc:43`) is the unbias
constant. (The `addq #2,r8` advancing the `oscvals` pointer lives in the
loop jump's delay slot at `omega.gas:466` — Jaguar RISC delay-slot
semantics; it executes every iteration.)

### 3.4 Amplitude/envelope scaling and write-back (`omega.gas:463-518`)

```
amp   = (link & $FFFF) >> 1                  // 0..$7FFF
scale = (env * amp) >> 16
acc   = (acc * scale) >> 15                  // signed
mod   = acc + $8000                          // signed -> unsigned 0..$FFFF
```

(Nominally the +$8000 is "if acc < 0", `omega.gas:472-473`, but the add sits
in the `jr` delay slot with the branch target immediately after it, so on
hardware it is unconditional — a plain bias. Port as `mod = (acc + 0x8000) &
0xFFFF`.)

Then the parameter is regenerated from its `pbinfo` metadata (40-byte
records at `pbinfo`/`editinginfo`, `vlm.s:7722`; record layout: 30 bytes
text, +30 flags byte, +31 partner-index byte, +32 edvex/editvex routine
index, +33 **type byte**, +34 word = parameter long-index, +36 long =
**max**):

```
t    = type byte; signed = (t & $80); t &= $7F     // 1=byte,2=word,3=16.16
M    = max; base = fxobj[512+4i]
if t == 2:  v = (M * mod) >> 16                    // linear 0..M
else:       v = fmult(M, mod)                      // 16.16 signed multiply (fmult macro, omega.gas:71)
v -= M/2                                           // omega path: always centred
v += base
fxobj[4i]  = v
results[i] = mod                                   // word
```

The editor's direct-write twin (`psi.gas routine18e`, `_fmode==2`,
`psi.gas:222-303`) is the same math with r21==0: no base add, and the
`-M/2` centring only when the signed flag is set. That is the entire
difference between "editing a parameter" and "animating a parameter".

**Summary: animated parameter = base + M*(mod/65536) - M/2**, i.e. the base
value plus/minus half the parameter's full-scale, driven by the product of
attached oscillators, gated by an attached envelope and the per-link
amplitude. This is the whole "wave plot" system.

### 3.5 ADSR envelopes (68k side, per VI)

`doadsr` (`vlm.s:906-1023`) runs a 5-state machine per channel (`adsrvex`:
`trigwait, attack, decay, sustain, release`, `vlm.s:943`), clocked by the
A/B/C buttons, output word 0..$FFFF into `envvals[7/6/5]`. Channel blocks
`adsra/adsrb/adsrc` (`vlm.s:8439`):

| off | field | note |
|---|---|---|
| +0 | attack rate | added as `rate>>2` per VI |
| +2 | decay rate | subtracted as `rate>>2` per VI |
| +4 | sustain level | e.g. $C000 |
| +6 | release rate | subtracted as `rate>>2` per VI |
| +8 | state word 0-4 | |

Edited in "Edit ADSR envelope shape" (`ahead2`/`adsred`, `vlm.s:3036`,
`5315`; +/-$80 per press).

### 3.6 Delay lines ("Delayline settings" — parameter echo / trails)

State: two channels, `delayf/delayp/dline/delayt/delayn` (channel 1,
`vlm.s:8447-8451`) and `zerstart/d2elayp/d2line/d2elayt/d2elayn` (channel 2,
`vlm.inc`). `zapdel` (`vlm.s:1261`): `dline = delaylin`,
`d2line = delaylin+768`; the arena at `delaylin` ($1C03F8) is two
interleaved rings of 64 x 768-byte records (stride $600); the `info` field
(offset 248) of every record is cleared to mark it invalid.

Per frame, per fxobj, `dbeast.gas` runs before the fxobj's module
(`omega.gas:174-180`):

1. `delayf < 0` → no delay; module runs once (`dbeast.gas:34-42`, r1=0).
2. If `delayf == slot#` (channel 2: `zerstart == slot#`; slot# =
   5-object_counter; `dbeast.gas:44-67`): blit-copy the first $300 bytes of
   the fxobj (parameters + link words + base values) into ring record
   `(delayp & $3F)` (`dbeast.gas:69-87`).
3. Compute playback: start = `delayp - delayt*delayn`, step = `delayt`,
   count = `delayn` (`dbeast.gas:89-104`), return r1=1.

Back in omega's `MainModuleRunLoop` (`omega.gas:199-259`): for each step,
point r3 at ring record `dline + (idx & $3F)*$600`, skip if its `info` is 0,
run the drawing module on that **historical parameter snapshot**, advance
`idx += delayt`. Afterwards `delayp++` (`omega.gas:255-259`). Finally
`PostMainModuleRun` draws the present-day fxobj once, storing `trig` into
`height` first (`omega.gas:264-293`). Result: `delayn` ghosts trailing the
live effect at `delayt`-frame spacing. UI: `edline`/`dladj`/`udedg`
(`vlm.s:3429-3487`, `5249-5310`).

---

## 4. Audio input path

### 4.1 DSP

`iansdoit` (`ians.s:30-54`) copies a prebuilt binary (`data_array`) to DSP
RAM at `storage = $F1B97C`, starts the DSP at `$F1B9AC`, `SMODE=$14` (I2S
from the CD DAC tap). The DSP performs the FFT ("FFT code by ib2", version
string `vlm.s:7463`) and continuously writes magnitude longs at **$F1B034**
(64+3 bins consumed). `fft_on` ($F1B020) and `VOLUME` ($F1BD60) are DSP-side
controls. The 68k never touches spectral data; the GPU reads DSP RAM
directly.

### 4.2 Peak-hold spectrum — `psi.gas` first block (`psi.gas:42-99`)

Once per frame (psi runs after all 6 fxobjs):

```
for band b = 0..63:
    s = average of 4 consecutive longs at $F1B034 + 4b      // overlapping windows
    peakflag[b] = (absdelta[b+1] > absdelta[b] && absdelta[b+1] > absdelta[b+2])
                  // stored at absdelta + 252 + 4b
    absdelta[b] = max(absdelta[b] - $3000, s)               // decay WID64=$3000/frame
```

`absdelta` ($1AE57C; 64 value longs + 64 flag longs) is the 64-band
peak-hold spectrum. It is what `shspec` draws in the trigger editor
(`vlm.s:2171`) and what drawing modules (e.g. shu) consume via `lol[7]`.

### 4.3 Band envelopes and triggers — `psi.gas routine65a` (`psi.gas:732-823`)

`avbank` (`vlm.s:8522`) defines 5 bands, 4 words each:
`{start_bin, end_bin, trig_min, release_min}` (defaults 1-6/$7000,
7-10/$6000, 11-24/$5000, 25-40/$4000, 41-63/$3000).

Per band b = 0..4:

```
peak = max(absdelta[start..end]);  peak = sat16(peak*2)     // 0..$FFFF
envvals[b] = max(envvals[b] - $200, peak)                   // decay UPDA1=$200/frame
if peak > trig_min:  trigger bit b set;  trig_min = peak    // threshold rides the peak (auto-gain)
else:                trig_min = max(trig_min - $300, 0)     // threshold decays
if release_min >= peak: trigger bit cleared
trig = (togglebits << 8) | triggerbits    // high byte: bit b flips on each trigger event
```

Written to `trig` (GPU RAM $F03F3C). `omega` copies `trig` into every
fxobj's `height` slot each frame; each fxobj's `_mtrig` selects which bits
it reacts to inside its drawing module. The five trigger lamps in the editor
read `trig` bits 0-4 (`zizz`, `vlm.s:2268-2280`).

Full audio→visual chain: **DSP FFT → absdelta (peak-hold, decay $3000) →
envvals[0..4] (band envelopes, decay $200) → (a) modulation multipliers via
link-word envelope bits, (b) `trig` bits → `height`/`_mtrig` gating in
effect modules** — plus `envvals[5..7]` from the button ADSRs and `px`/`py`
pad pseudo-waves.

Trigger editing: `speced2`/`speced3` (`vlm.s:4723`, `4696`) adjust band
start/end/position and `trig_min` (+/-$100).

---

## 5. Bank / effect selection

- `imatrix` (0..8) = bank, `skid` (1..9) = effect.
- Keypad handling in `Frame` (`vlm.s:4404-4450`): while `*` is held, digit
  presses shift through `star_on` (bank, then effect); on `*` release either
  `action=skidoo` (effect only) or `imatrix=bank-1; action=gm` (both).
  `dcode`/`codez` map pad bits to digits (`vlm.s:6501-6520`).
- `action` ($1AE524) is a **deferred one-shot vector**: set from the ISR,
  executed by `yakedit` in the mainline at frame cadence, then cleared
  (`vlm.s:1567-1573`). Keeps heavy work out of the interrupt.
- `gm` (`vlm.s:7017-7105`) unpacks a bank into `matrix`:
  1. fill `matrix` with $FF; copy `refblock` (6 pristine fxobjs) as effect-1
     base;
  2. locate bank data via `av1ref` → `av1` offset table
     (`vlm.s:8535-8551`; bank 10 = splash bank);
  3. apply the (skip,value) byte-pair delta stream across 6144 bytes;
  4. each subsequent effect uses the previous effect as its base layer
     (8 more passes);
  5. `skidoo` activates effect `skid`.
- `skidoo` (`vlm.s:4595-4639`): points the 6 `fx` slots at
  `matrix + (skid-1)*6144 + n*1024` (0 where `info`==0), fixes each
  oscillator's table pointer from its type via `sineslookup`, restores the
  108-byte `pixcon` block from fxobj0+896, `zapdel`, clears `og`.

### 5.1 Edit mode / UI state machine (skim)

- Modes: `vlm_mode` 0 = CD-controls, 1 = VLM active, 3 = editing
  (`vlm.inc:323-325`); `setedit`/`editquit` (`vlm.s:1287`, `3907`). In this
  reconstruction the Pause button enters edit mode (`vlm.s:4453-4462`);
  cycling the "Spectrum and Triggers" menu 8 times reveals `old_edit`
  (`seldn`, `vlm.s:5600-5615`).
- Per-VI edit dispatch: `editing` (1..16) indexes `editvex`
  (`vlm.s:4664-4686`): `selector, xy1, x_one, x_one, xy1, xy1, kpad, awf_x,
  awf_y, spdinc, adsred, dladj, padex, speced, speced2, speced3`.
- Control-overlay rendering per frame: `symed` indexes `edvex`
  (`vlm.s:2533`): `crot, snglx, sisnglx, sidbl, dvect, rrts, wsshow,
  shspec`, drawn into `draw_screen` via `sblitblockk` and GPU `_fmode` ops.
- Menu screens are pushed on `esp`/`e_stac` (`estack`/`eunstack`,
  `vlm.s:3607-3629`); option lists `option1..8` (`vlm.s:8277-8322`) are
  `(count<<16)`, footer string, then (text, routine) pairs; `elcon`
  (`vlm.s:1628`) builds parameter menus from the per-type `vars` lists and
  `pbinfo` records; `eparam`/`crunt`/`primexy`/`s16` (`vlm.s:3635-3894`)
  bind a parameter to the `ixcon`/`iycon` inertial sliders.
- Editor→GPU drawing uses the `_fsync`/`_fmode`/`_ein_buf` mailbox
  (`vidinit.s:32-37`): when `freerun==0`, psi sets `_fsync=1` each frame
  (`psi.gas:122-123`) then services `_fmode` commands until the 68k clears
  `_fsync` (`psi.gas:124-197`). Ops: 1 ADSR-shape plot, 2 **parameter
  write** (`routine18e`), 3 line draw (`X_1/Y_1/X_2/Y_2/LINCOL`), 5 waveform
  plot, 6/7/8 oscilloscope traces of `results` into `monbuf`. `freerun==2`
  halts the GPU (`psi.gas:210-221`).
- `print` (`vlm.s:3928`) renders control-coded text into the 1bpp `board`
  overlay (`/` newline, `@` clear, `~gX:Y:` goto, `~eX:Y:` embed menu,
  `~i+/-` inverse, `~cN:` timed clear) via blitter `charblit`
  (`vlm.s:6291`) and `myFont`.
- Dev-only: `compr`/`dostash`/`deltablo` compress `matrix` to a ROM emulator
  at $900000 (`vlm.s:3314-3424`, `6899-6963`); `rset`, `mset`, `kpass`
  (assign effect to keypad slot, `vlm.s:4861-4934`).

---

## 6. Display composition

### 6.1 Framebuffers

- Two 16-bit **CRY** screens: `$100000` and `$148000`, 384x384, pitch 768
  bytes ($300 — the constant loaded into r10 by every GPU module, e.g.
  `psi.gas:615`). `cscreen` = displayed, `dscreen` = `draw_screen` = being
  rendered; swapped in `Frame` (section 1.5).
- `skale` (0/1/2) selects render height 384/192/96; `everything` sets
  `skale=1` (`vlm.s:345`), so the GPU normally renders **384x192** and the
  Object Processor **line-doubles** it to 384x384 (`clip0`→`clip2`→
  `MakeScaledObject`, `vlm.s:5818-5829`, `6019`; OP scale words
  $14040/$18080 at `vlm.s:6115-6121`; `dbadj` handles the split-object row
  offset). `sblitblockk` shifts editor drawing coords by `skale`
  (`vlm.s:6343`).
- `board` ($1B00F8): 320x240 1bpp text/UI overlay (ObType 1), transparent.
- `VMODE=$6C1`: CRY16, PWIDTH=4, BGEN. PAL adds `palside=6`, `paltop=$1E`
  offsets (`vlm.s:2003-2010`).

### 6.2 Object list ("beasties")

- `beasties` ($1AE048): 12 x 64-byte backing records (`InitBeasties`,
  `vlm.s:5638`). Record layout (`vlm.s:5624-5636`): +0 X.l, +4 Y.l, +8 W.w,
  +10 H.w, +12 OBJ_MODE (index into `ModeVex`; -1 = off), +14 OBJ_TYPE
  (index into `ObTypes`), +16 data pointer, +20 postfixup index
  (`make_rmw`/`make_transparent`, `vlm.s:5741-5768`), +24/26/28 phrase
  width/height/depth copied from `ObTypes`.
- `ObTypes` (`vlm.s:7316`): {phrase-width, height, depth, CLUT index}:
  0 = 384x384 16bpp main screen, 1 = 320x240 1bpp board, 2 = 4x8 cursor,
  3/4 = 384x192 / 384x96 16bpp (scaled modes), 5 = logo 1bpp, 6 = 64x48 4bpp
  (Dave's CD overlay), 7 = 32x10 16bpp VLM logo.
- Every VI: `RunBeasties` (`vlm.s:5702`) walks the records, dispatching via
  `ModeVex` (`clip0/clip1/stoptop/clip2`, `vlm.s:5773`) to
  `MakeUnScaledObject`/`MakeScaledObject` (`nmulto`/`nmulto2`,
  `vlm.s:6019-6229`, which pack the OP bitmap-object phrases and handle
  negative-Y clipping via the `hango` data-pointer bias) into `blist`;
  `StopList` (`vlm.s:6009`) terminates. `Frame` copies `blist`→`dlist`;
  `OLP` points at `ddlist`'s branch-object header (`InitList`,
  `vlm.s:5957`).

### 6.3 GPU module system

- `gpumods` (`vlmgpu.s:113`): 11 modules, each blob prefixed with
  {load address.l, length.l} (`vlmgpu.s:142-190`). `omega` loads once at
  `$F03A78` and stays resident; all others load at `$F03000` (max $A78
  bytes) via blitter copy (`GPULoad`, `omega.gas:590`), with a last-loaded
  cache (`previously_loaded_module`, `omega.gas:658-710`).
- Per frame, per fxobj slot 0..5: run `dbeast` (delay preamble), then the
  module `gpu>>16` — N ghost passes plus one live pass — with the fxobj
  pointer in r3 and the variant word (`gpu & $FFFF`) at $F03F5C. After all
  slots: run `psi`, set `screen_ready`, loop (`omega.gas:125-571`).
- Module roles: `alpha` starfield/ring/pixel-plot family, `beta` DVF
  (feedback zoom/rotate, consumes `dvf_buf`), `gamma` polygon objects
  (`polyos` shapes + sine tables + symmetry), `delta` colour-plasma/bitmap
  family, `epsilon` particle drawing, `theta` particle motion integration,
  `shu` spectrum-as-intensities, `sigma`/`tau` unused.

### 6.4 68k blitter helpers

`blitblock`/`sblitblockk` solid fills, `cleol` board clear, `charblit` 8x8
glyphs, `CopyBlock`/`MergeBlock`, `blitcopy` (bulk copies for `gm`)
(`vlm.s:6329-6494`, `7127`).

---

## 7. Porting map (68k/GPU/DSP → C + SDL2/GLES2)

Must be faithful (defines the look and feel):

| Subsystem | Original | Port recommendation |
|---|---|---|
| fxobj layout | 1024-byte block, section 2 | Keep byte-identical (`uint8_t[1024]` + accessors on the `vlm.inc` offsets). Byte-identity is mandatory: `banks.s` is a binary diff against `ifxobj` output — port `ifxobj` exactly and `gm`'s (skip,value) decompressor runs unchanged. 68k longs are **big-endian**: store big-endian or byte-swap on load. |
| Parameter animation | `omega.gas` sections 3.1-3.4 | Port exactly in integer math, once per rendered frame: phase adds, 8-bit table lerp, ring-modulated wave cascade, `(env*amp)>>16`, `>>15`, `+0x8000` bias, `v = base + scale(M,mod) - M/2`. Shifts and saturations exactly as written — the character of the motion lives here. |
| Waveform tables | built in `startup` | Rebuild identically from `sines.s` (unsigned biased 256+1 bytes). |
| ADSR + inertial pads | `doadsr`, `inertcon` | Step at a fixed 60 Hz tick (they run in the VI ISR on hardware), not at render rate. |
| Delay lines | `dbeast` + `MainModuleRunLoop` | Ring of 64 x 768-byte parameter snapshots per channel; re-run the drawing module on historical snapshots. Cheap and essential for the trail looks. |
| Audio analysis | DSP FFT → `psi.gas` | Replace the DSP with any FFT producing 64+3 magnitude values at comparable scale, then port the psi chain **exactly**: 4-bin overlapped averages, peak-hold decay $3000/frame, envelope decay $200/frame, trigger auto-gain (threshold := peak on trigger, -$300/frame otherwise), toggle bits in trig's high byte. The decays are per rendered frame — run them at render cadence like the original. |
| Effect modules | alpha..shu (.gas) | Port each as a C function writing a 384x(384>>skale) **CRY16** buffer (pitch 768). Recommend software rendering + one GLES2 textured quad with a CRY→RGB conversion (a 65536-entry LUT texture or shader function). beta/DVF is per-pixel source resampling with intensity decay — shader-friendly, but software keeps exact feedback rounding/saturation, which is highly visible in DVF-heavy banks. |
| Frame pacing | `sync`/`screen_ready` | Simulate+draw one frame as fast as possible; present at vsync; swap only when a frame completed. Do **not** force one simulation step per display frame — slowdown under load is authentic. |
| Bank data | `banks.s`, `av1` | Embed the compressed stream verbatim; port `gm`/`skidoo` including the `sineslookup` fix-up and the 108-byte pixcon restore. |
| Trigger plumbing | `trig` → `height`, `_mtrig` | Keep: modules read fxobj `height` as the live trigger word masked by `_mtrig`. |

Hardware glue to replace outright:

| Original | Replacement |
|---|---|
| Object Processor lists (`beasties`, `blist`/`dlist`, `ObTypes`, `RunBeasties`, OP scaling) | Compose layers directly: main CRY screen (2x/4x vertical scale for `skale`), `board` 1bpp text overlay, logo sprites — one GLES2 quad each. Drop `nmulto*` phrase packing entirely. |
| Blitter fills/copies (`blitblock`, `blitcopy`, `charblit`, DVF inner blits) | memset/memcpy/loops in C. |
| GPU module loading (`gpuload`, the $F03000 overlay trick, `mods`) | Function pointers; keep `gpu = (module<<16)|variant` as the dispatch key. |
| `in_buf` mailbox, `_fsync`/`_fmode`/`_ein_buf` | Direct function calls; `_fmode` ops become editor drawing helpers. |
| `readpad`, keypad matrix, `codez` | SDL input mapped onto the same 32-bit `pad_now` bitmask (`vlm.s:6529-6554`) so all `Frame`-derived logic ports verbatim. |
| VI interrupt | A 60 Hz (NTSC) tick calling the ported `Frame` logic (input, ADSR, swap decision, `action`/`routine` dispatch). PAL fixups (`palside`/`paltop`) droppable. |
| ROMulator save (`dostash`, $900000), CD front-end (`davesvec`, `vlm_mode==0`) | Omit, or replace with file I/O for saving edited banks. |

Suggested C module boundaries mirroring the source: `vlm_state.c` (globals
from `vlm.inc`), `fxobj.c` (`ifxobj`, banks, `gm`/`skidoo`), `animate.c`
(omega, section 3), `audio.c` (FFT + psi, section 4), `frame.c` (Frame/ISR
logic, pads, ADSR, inertials), `editor.c` (menus, `print`, board),
`fx_*.c` (one per .gas module), `render_gl.c` (CRY LUT + layer compositing).

Fixed-point cheat sheet: screen coordinates 16.16 in a 384-unit space
(centre $C00000 = 192.0); intensities/colours 0.16; oscillator phase 24-bit
cycle (8 index + 8 frac + 8 subfrac); DVF scale/angle "8.8 in the high
word" (1.0 = $100 after `>>16`); envelope and oscillator outputs unsigned
16-bit biased at $8000; `results` modulation 0..$FFFF.
