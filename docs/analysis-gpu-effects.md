# VLM-1 GPU Effect Modules — Reverse-Engineering Analysis

Source base: `/Users/larsens/Github/vlm-ref/src/vlm/` (all references below are relative to
that directory unless an absolute path is given). The GPU modules live in `gpu/*.gas`
(Tom RISC assembly, mostly machine-regenerated disassembly with original addresses in
`movei` immediates and `; #$offset` comments). Loader/dispatch is `vlmgpu.s`, 68k side is
`vlm.s`, shared constants in `vlm.inc`.

**Reading notes for the disassembly**

- Several `movei #CONST` immediates were symbolized by the disassembler with *blitter*
  constant names that are numerically identical but semantically wrong. Decode them as
  numbers: `ZOFFS3`=$C0 (192, half screen width), `ZOFFS6`=$180 (384, screen width),
  `TOPNEN`=$8000, `LFU_NSAD`=$400000 (i.e. 64<<16, a +90° sine-phase offset),
  `LFU_N_SXORD`=$C00000 (192<<16, the screen-centre bias of 16.16 coordinates),
  `WID4`=$1000, `WID64`=$3000, `PIXEL8`=$18, `XADDPIX`=$10000, `UPDA1`=$200, `UPDA2`=$400
  (`/Users/larsens/Github/vlm-ref/src/blitter.inc:28-182`).
- Tom RISC semantics that matter for bit-accuracy: `mult` = **unsigned** 16×16→32,
  `imult` = **signed** 16×16→32, `div` = unsigned 32/32 (all code wraps it in
  `abs`/`neg` sign fixups), `sharq` = arithmetic shift right, `shrq` = logical,
  `sat16` clamps signed 32-bit to 0..$FFFF, `moveta/movefa` move to/from the *alternate
  register bank* (used as 32 extra globals), and `load/store (rN+k)` scales the constant
  offset `k` by **4** (so `(r14+$15)` = byte offset $54).
- Trailing "instructions" at the end of alpha/epsilon/sigma/tau/shu
  (`add r7,r16` / `btst …` pairs, e.g. `gpu/alpha.gas:1027-1044`) are **data**: jump
  tables of 4 longword routine addresses indexed by `plot_mod`/`pixsize` (see §4.6/4.7).

---

## 1. The GPU module ABI

### 1.1 Bootstrap (68k side)

- `vlmgpu.s:41-83 gpuload` — blits a module blob into GPU local RAM. Each blob is
  prefixed by 8 bytes: load address, byte length (`vlmgpu.s:142-190`). The copy is a
  plain blitter LFU copy in phrase mode.
- `vlmgpu.s:88-94 gpurun` — `gpuload` + write load address to `G_PC`, `G_CTRL=$11` (go).
- The 68k only ever *runs* one module this way: **omega**, from `titlescr`
  (`vlm.s:1520-1524`). Omega is linked at **$F03A78** and stays resident; every other
  module is linked at **$F03000** and is ≤ $A78 bytes, so omega can blit them under
  itself without being overwritten (`vlmgpu.s:134-141`, `gpu/omega.gas:573-588`).
- Omega's own `GPULoad` (`gpu/omega.gas:590-649`) and `RunGPUModule`
  (`gpu/omega.gas:658-707`) load-and-jump sub-modules. `RunGPUModule` caches the last
  module index (`previously_loaded_module`, `gpu/omega.gas:709-710`) to skip redundant
  loads. Note `gpu/omega.gas:695-697`: `$8000` is added to the $F03000 destination
  before blitting — the blitter must write GPU RAM through an alias address; the GPU
  then *executes* at $F03000 (`loaded_module EQU $F03000`).

### 1.2 Module calling convention

On entry to every sub-module:

| Register / location | Meaning |
|---|---|
| `r3` | pointer to the **fxobj** parameter block (1024 bytes, in main DRAM — either the live object or a delay-line snapshot; see §2) |
| `r14` | `in_buf+$20` = $F03F80, base of a GPU-RAM scratch workspace addressed as `(r14+slot)` (slot × 4 bytes) |
| `(r14+$15)` | **return address** (`_post_gpu_routine`). A module "returns" with `load (r14+$15),r0 ; jump (r0)` — e.g. `gpu/alpha.gas:189-191`, `gpu/theta.gas:357-359` |
| `(r14+$17)` / `(r14+$18)` / `(r14+$19)` | live fxobj ptr / object counter (5..0) / results ptr, stashed by omega (`gpu/omega.gas:167-170`) |
| `$F03F5C` (`screen_ready+$c`) | **sub-mode** = low 16 bits of the fxobj `gpu` field, stored by `RunGPUModule` (`gpu/omega.gas:664-671`); modules dispatch on it (e.g. `gpu/alpha.gas:63-79`, `gpu/delta.gas:83-110`) |
| Alternate register bank | omega parks its loop state there around a module call (`moveta r24-r27`, `gpu/omega.gas:227-244`) |

GPU-RAM communication block (`vlm.inc:276-283`):

```
$F03F3C trig          beat-trigger bits (written by psi, read by omega)
$F03F40 dscreen       current draw buffer address     (swapped by 68k Frame)
$F03F44 cscreen       last-completed (displayed) buffer
$F03F48 mods          -> gpumods table (vlmgpu.s:113-123)
$F03F4C sync          GPU sets 1 when frame built; 68k clears at vblank
$F03F50 screen_ready  GPU sets 1 when frame complete
$F03F54 draw_screen   draw buffer address (again); draw_screen+4 = "advance
                      parameters" flag: 1 during delay-echo runs (dbeast,
                      gpu/dbeast.gas:99-101), 0 for the live run
                      (gpu/omega.gas:279-282)
$F03F60 in_buf        13-pointer table set up by titlescr (vlm.s:1511-1518):
        +$00 lol  +$04 oscvals  +$08 fx  +$0C results  +$10 pbinfo
        +$14 delayf  +$18 polyos  +$1C cobuf
$F03F80 (in_buf+$20)  r14 workspace, slots 0..$1F
```

`lol` (`vlm.s:8341-8342`) is a second pointer table: `bmobj, skale, beasties, pobjs,
mpobjs, mmasks, maxes, absdelta, zerstart, avbank, envvals, _fsync, freerun`. Modules
index into it (`in_buf[0] + n`) to reach 1-bit logo bitmaps (delta, tau), particle
buffers `pobjs` (theta, epsilon), mono objects `mpobjs` (sigma), voxel masks `mmasks`
(tau), and the DSP spectrum `absdelta` (shu).

### 1.3 Frame protocol (per-frame, never per-scanline)

The *main loop of the whole program* is `RunFXObjModules` in omega
(`gpu/omega.gas:125-571`). Per frame:

1. `sync=1`, spin until 68k `Frame` clears it at vblank (`gpu/omega.gas:133-146`,
   `vlm.s:4284-4296`).
2. For each of the 6 fxobjs (`fx` array of 6 pointers, `vlm.s:1514`,
   `gpu/omega.gas:150-171`):
   a. run **dbeast** (snapshot params into the delay line, compute echo count);
   b. if delay active, loop `delayn` times running the fxobj module against
      *archived* parameter blocks (`MainModuleRunLoop`, `gpu/omega.gas:199-250`;
      slot address = `dline + (idx&63)*$600`, module index from archived `gpu` field
      at offset $FC, `info` field at $F8 gates empty slots);
   c. run the module once against the **live** fxobj with `fxobj[0]=draw_screen`,
      `fxobj[$F0]=trig`, `draw_screen+4=0` (`PostMainModuleRun`,
      `gpu/omega.gas:264-293`);
   d. advance the 8-entry oscillator bank inside the fxobj (`UpdateOSCBank`,
      `gpu/omega.gas:307-330`), evaluate the 8 oscillators into `oscvals`
      (`gpu/omega.gas:336-382`), then run the 64-parameter **modulation matrix**
      (`BigLoop64`, `gpu/omega.gas:390-533`) which rewrites fxobj parameters from
      oscillators/envelopes and writes UI feedback words into `results`.
3. Run **psi** (spectrum envelopes, beat triggers, editor overlay commands).
4. `screen_ready=1`, loop (`gpu/omega.gas:566-571`). The 68k `Frame` ISR swaps
   `cscreen`/`dscreen`/`draw_screen` when both `screen_ready` and `sync` are set
   (`vlm.s:4283-4296`).

Everything renders a **whole frame per pass** into a 384-wide, 16bpp CRY buffer
(row stride $300 bytes; buffers at $100000 and $148000, `vlm.s:1946-1953`). The Object
Processor merely displays the finished buffer; there is no per-scanline GPU work.

### 1.4 Completion signalling

Sub-modules never touch `sync`/`screen_ready`; they only `jump (r14+$15)` back into
omega. Omega signals the 68k via `screen_ready`/`sync` as above. Error/stop paths write
`G_CTRL` bit0 = 0 and spin (`gpu/alpha.gas:372-378`, `gpu/psi.gas:214-221`).

---

## 2. The fxobj parameter block

Canonical layout documented at `vlm.s:133-198` (offsets also in `vlm.inc:210-289`).
The first 256 bytes are the effect parameters (all 16.16 fixed point unless noted);
key fields, by byte offset:

```
$00  screen ptr (written by omega each run)     $04 dvf_ws_x   $08 dvf_ws_y
$0C  vfb_xsca   $10 vfb_ysca   $14 vfb_angl     $18 dvf_crot_x $1C dvf_crot_y
$20  dvf_deltai $24 dstoffx    $28 dstoffy      $2C vfb_xpos   $30 vfb_ypos
$34  dstoffz    $38 vectorx/d_x $3C dy          $40 vectory/d_z
$44  (sym types) $48 rsym_ord  $4C rsym_ste     $50 rsym_ist
$54..$68 _i1.._i6 (intensities, $8000-centred)  $6C zamp
$70  phase1     $74 phase2     $78 (phase3 accumulator) $7C phase4
$80  i (count in high word)    $84 j            $88 k          $8C (count2)
$90  col1       $94 deltaz     $98 thang (selector, high word)
$A0  asym_fla   (mirror-symmetry bits 0-3, bit16 = rotational symmetry on)
$A4  sine_bas   (waveform table pointer: p_sines/p_saw/p_square/p_ramp/p_rect)
$A8  rxcen      $AC rycen      $B0 roscale      $B4 roscal2
$B8  cvx        $BC cvy        $C0 roscalei     $C4 drxcen     $C8 drycen
$CC  phase5     $D0 wave_2     $D4 radius       $D8 cvx2       $DC cvy2
$E0  colx       $E4 coly       (colour-space phase accumulators)
$E8  plot_mod   $EC pixsize    $F0 height (=trig for live run)  $F4 _mtrig
$F8  info (sub-effect type; 0 = slot unused)    $FC gpu (module# << 16 | submode)
```

Bytes 256-511: 64 × 4-byte modulation-matrix entries (source mask word + scale word,
initialised to $0000FFFF, `vlm.s:1873-1883`). Bytes 512-767: 64 base-value longs.
Bytes 768-895: private copy of `oscbank` — 8 × 16-byte oscillator records
`{accum.l, increment.l, table_ptr.l, type.l}` (`vlm.s:1885-1890`, `vlm.s:8416-8430`).
Bytes 896-1003: `pixcon`/`piycon` inertial-controller block (`vlm.s:1894-1898`).

Coordinates such as `dstoffx` are 16.16 with $C0.0000 (=192.0) meaning screen centre —
modules subtract $C00000 to centre them (e.g. `gpu/theta.gas:102-110`,
`gpu/epsilon.gas:227-229` where the disassembler shows the constant as
`LFU_N_SXORD`).

---

## 3. Screen format and pixel write conventions

- Pixel = 16-bit CRY word: high byte CC (4-bit X | 4-bit Y chroma-cube index),
  low byte Y intensity. Modules compose `pixel = (intensity16 >> 8) | (CC << 8)`.
- All point-plot writes are **plain overwrites** — no read-modify-write blending
  (`gpu/alpha.gas:627-646`). The only additive/saturating intensity arithmetic is done
  *inside the blitter* by beta's SRCSHADE mode (§5-beta) and psi's `sat16` envelope
  math.
- Clipping in the shared plot routines: `0 <= x,y < $17E` (382) against `r15=$17e`
  (`gpu/alpha.gas:53`, `gpu/alpha.gas:627-634`). Address = `screen + y*$300 + x*2`.

---

## 4. Shared code idioms (appear verbatim in most modules)

### 4.1 CRY colour fold ("colour generator")

Inputs `colx`/`coly` ($E0/$E4) are free-running phases. Two precisions exist:

**Narrow fold** (`gpu/alpha.gas:86-107`, `gpu/beta.gas:13-34`, gamma callers,
`gpu/shu.gas:158-180`):
```c
cx = (colx >> 16) & 0x1e;  cy = (coly >> 16) & 0x1e;   // 0..30, even
cx -= 15; if (cx < 0) cx = -cx;                        // triangle fold 0..15
cy -= 15; if (cy < 0) cy = -cy;
CC = (cx << 4) | cy;  r5 = CC << 8;                    // pixel high byte
```

**Wide fold** (mask $1FFFFF, `gpu/delta.gas:135-163`, `gpu/epsilon.gas:44-71`,
`gpu/sigma.gas:44-71`, `gpu/theta.gas:194-217`): same triangle fold at 21-bit width
(`v &= 0x1fffff; v -= 0x100000; abs`), then `>>16` to a nibble. Delta additionally
derives an *intensity* from the fold: `Y = (fold >> 13) + 64`
(`gpu/delta.gas:154-162`). This is the whole "colour cycling" system: a
triangle-folded walk of the 16×16 CRY chroma plane, phases stepped per-pixel by
`cvx/cvy` and per-row/frame/copy by `cvx2/cvy2`.

### 4.2 Waveform-table lookup with linear interpolation

The universal oscillator/sine sampler (e.g. `gpu/alpha.gas:440-457`,
`gpu/epsilon.gas:76-93`, `gpu/omega.gas:365-377`):
```c
idx  = (phase >> 16) & 0xff;
s0   = table[idx];  s1 = table[idx + 1];          // tables have a 257th entry
frac = (phase >> 8) & 0xff;
val  = ((s0 - 128) << 8) + (s1 - s0) * frac;      // signed variant (r29=128)
// omega oscvals variant omits the -128: val = (s0<<8) + (s1-s0)*frac (unsigned)
```
Cosine = same lookup with phase + $400000 (64 index steps = 90°; disassembled as
`movei #LFU_NSAD` — `gpu/epsilon.gas:75`, `gpu/sigma.gas:75`), or `table[idx+64]`
directly in gamma (`gpu/gamma.gas:29-37`).

Tables (`vlm.s:1955-2001`, base sine data `sines.s:20+`): `p_sines[i]=sine[i]+128`,
`p_rect[i]=2*|sine[i]|`, `p_saw[i]=|2i-255|`, `p_ramp[i]=i`, `p_square[i]= i>=128?255:0`
(256 bytes each + wrap byte; addresses `vlm.inc:179-183`).

### 4.3 `fmult` — signed 16.16 × 16.16 multiply

Macro at `gpu/omega.gas:71-99`, inlined everywhere (all the
`abs/addc/mult/shrq#16/…/btst #0,r27/neg` blocks, e.g. `gpu/alpha.gas:229-257`,
`gpu/beta.gas:277-303`, huge chains in epsilon/sigma/tau/gamma). Semantics:
`result = sign(a)*sign(b) * ((|a| * |b|) >> 16)` computed from four unsigned 16×16
partial products. In C, `(int32_t)(((int64_t)a * b) >> 16)` matches **except** that
sign-magnitude truncates toward zero while two's-complement `>>` truncates toward −∞.
For bit-accuracy port it as sign-magnitude:
`s = (a<0)^(b<0); r = (uint64_t)llabs(a)*llabs(b) >> 16; return s ? -(int64_t)r : r;`

### 4.4 Signed divide idiom

`move x,r29; abs x; div d, x; btst #$1f,r29; jr z; neg x` — unsigned divide with sign
restore, quotient truncated toward zero (`gpu/alpha.gas:992-1006`,
`gpu/gamma.gas:588-618`).

### 4.5 Rotational-symmetry engine

Present in alpha (`gpu/alpha.gas:386-525`), epsilon (`gpu/epsilon.gas:687-826`),
sigma (`gpu/sigma.gas:689-828`), shu (`gpu/shu.gas:217-357`), gamma (reduced form,
`gpu/gamma.gas:350-425`). Triggered when `asym_fla` ($A0) bit 16 is set. Pseudocode:

```c
// state cached in r14 slots: [$0]=count, [$1]/[$2]=phase state,
// [$4]=angle_accum, [$b]/[$c]=colour phases, [$d]/[$e]=centre, [$3]=sine table
count = rsym_ord($48); phase accum += phase step per copy (from $4C/$50/$78 family)
dx = x - (rxcen($A8)>>16); dy = y - (rycen($AC)>>16);
s = interp(table, angle);  c = interp(table, angle + 90°);   // both -128-centred
rx = (dx*c - dy*s) >> 15;  ry = (dx*s + dy*c) >> 15;         // imult, sharq #15
rx = (rx * roscale($B0)) >> 12;  ry = (ry * roscale) >> 12;  // sharq #12
plot(rx + rxcen>>16, ry + rycen>>16) with per-copy colour advanced by cvx/cvy;
roscale += roscal2($B4)+$1000 / roscalei($C0)+$1000;         // movei #WID4 adds,
rxcen += drxcen($C4); rycen += drycen($C8);                  // gpu/alpha.gas:529-553
```
Each copy re-derives CC from the per-copy colour phases `(r14+$b)/(r14+$c)` stepped by
`cvx/cvy` (`gpu/alpha.gas:489-522`).

### 4.6 Mirror-symmetry chain + plot dispatch

`gpu/alpha.gas:566-623` (identical in `gpu/epsilon.gas:867-923`,
`gpu/sigma.gas:869-925`, `gpu/shu.gas:399-467`). `r22 = asym_fla`, W=$17E. For point
(x,y): bit0 → plot (x,y); bit1 → plot (W−x, y); bit2 → plot (W−x, W−y);
bit3 → plot (x, W−y); bit4 (after `shrq #4`) → diagonal swap pass ((y, W−x) family),
re-running bits 0-3 with bit8 marking the second pass. The final plot routine pointer
sits in `(r14+$11)`, loaded at module entry from a per-module 4-entry table indexed by
`plot_mod` ($E8) (`gpu/alpha.gas:55-62` → table at module offset $9B0 =
`gpu/alpha.gas:1027-1036`; epsilon $9C0, sigma $9D8, shu `end_end`
`gpu/shu.gas:641-649`).

### 4.7 Plot routines (same code in each module)

- **1×1**: bounds-check, `storew (CC<<8)|(intensity>>8)` — `gpu/alpha.gas:627-646`.
- **2×2**: same + stores at x+1, y+1 — `gpu/alpha.gas:647-671`.
- **N×N via blitter** (pixsize ≥ 3): A1_BASE=screen, A1_FLAGS=$14420
  (XADDPIX|WID384|PIXEL16), A1_PIXEL=(x−N/2, y−N/2), A1_STEP=(−N,1),
  B_PATD=pixel replicated, B_COUNT=N×N, B_CMD=$10200 (PATDSEL|UPDA1) —
  `gpu/alpha.gas:906-983` (chooses 1×1/2×2 fast paths for pixsize 0/1).
- Starfield/ring modes scale intensity by pixsize first:
  `r1 = (intensity * (pixsize>>16)) & $7F0000` (`gpu/alpha.gas:1016-1022`).

### 4.8 Perspective divide

`gpu/alpha.gas:985-1014` (starfield flavour):
```c
X = x << 14;  Y = y << 14;  z &= 0x1ff;
intensity = (0x1ff - z) << 7;            // depth-shaded brightness
X /= (z << 2); Y /= (z << 2);            // signed-div idiom
screen_x = (X >> 5) + 0xC0;  screen_y = (Y >> 5) + 0xC0;   // arith shifts
```
Epsilon/sigma/tau flavour (`gpu/epsilon.gas:1053-1081`): `z >>= 14 (arith);
if (z < 1) reject; X = ((x<<14)/z) >> 7 (arith) + 0xC0`, intensity from object data.

---

## 5. Module-by-module analysis

### omega — orchestrator + parameter animation (`gpu/omega.gas`)

Scheduling covered in §1.3. Two pieces matter for fidelity beyond scheduling:

**Oscillator update** (`gpu/omega.gas:307-382`): for each of the 8 records at
fxobj+768: `accum += increment` (increment forced 0 if `type==6`,
`gpu/omega.gas:321-325`). Then produce `oscvals[0..7]` words:
- `type >= 7` → value read from `delayf-8 + (type-7)*4` = the pad-driven `px/py`
  cells (`gpu/omega.gas:350-359`, data `vlm.s:8445-8447`);
- `type == 3` → `value = accum >> 8` (raw ramp, `gpu/omega.gas:360-364`);
- else → §4.2 interpolation (unsigned variant) of `table_ptr` at phase `accum`
  (`gpu/omega.gas:365-377`).

**Modulation matrix** `BigLoop64` (`gpu/omega.gas:390-533`): for parameter i = 0..63
with matrix long m = `fxobj[256+i*4]` (skip if high word 0):
- 8-iteration inner loop (`gpu/omega.gas:435-459`): bit b of m's high byte selects
  `src = (int16)(oscvals[b] - 0x8000)`; contribution `(src * scale) >> 16` accumulated
  (scale starts $7FFF); bit (b+8) replaces the scale word from the envelope region
  (`oscvals+0x40`, i.e. `envvals`).
- Combine: `acc = (acc * ((depth * escale) >> 16)) >> 15; if (acc<0) acc += 0x8000`
  (`gpu/omega.gas:466-473`), depth = low word of m halved.
- Destination decode from the 40-byte `pbinfo` record for parameter i
  (`pbinfo + i*40 + $1e`, `gpu/omega.gas:408-419` + 477-515): byte = type | flag<<7,
  word = fxobj byte offset / 4, long = scale. Type 2 → `fmult(scale, acc)`, else
  `(acc * scale) >> 16` (unsigned `mult`); flag bit7 → subtract scale/2; add the base
  long at `fxobj[512+i*4]` when the "r21" base pointer is live. Result long stored at
  `fxobj[offset*4]`; the raw 16-bit value is written to `results[i]` for the editor
  (`gpu/omega.gas:516-518`).

Runs on the GPU but is pure integer math — it *is* the parameter animation for every
effect and must be ported with the modules.

### dbeast — per-object preamble / echo ("delay line") setup (`gpu/dbeast.gas`)

- Reads `delayf` via `in_buf+$14`; negative → return `r1=0`, no echoes
  (`gpu/dbeast.gas:34-42`).
- Echoes only apply to the fxobj whose index matches the configured word
  (`gpu/dbeast.gas:44-67`).
- Blitter-copies the **first $300 bytes of the live fxobj** into delay-line slot
  `dline + (delayp & 63) * $600` (`gpu/dbeast.gas:69-87`; local GPULoad 133-192;
  `delaylin` buffer, `vlm.inc:176`).
- Computes echo start index `delayp − delayt*delayn`; returns r25=`delayt` (step),
  r24=`delayn` (count), r27=`dline`, sets `draw_screen+4 = 1`
  (`gpu/dbeast.gas:89-104`). Omega then replays archived parameter blocks through the
  effect module — the visual "echo/trail" feature — and increments `delayp` once per
  frame (`gpu/omega.gas:255-259`). Because `draw_screen+4`=1 during echo runs, the
  *archived* blocks self-advance their phases each frame (see the `draw_screen+4`
  checks in alpha/epsilon/sigma), so echoes stay animated.

**Category**: infrastructure (parameter history ring buffer). memcpy + index math.

### psi — per-frame postamble (`gpu/psi.gas`)

1. **Spectrum envelope followers** (`gpu/psi.gas:42-99`): for band i = 0..63, read 4
   longs from **DSP RAM** starting `$F1B034 + i*16` (FFT magnitudes), average
   (`sum >> 2`); `cobuf[i] = max(cobuf[i] − 0x3000, avg)` (fast attack, linear decay);
   also writes a flag long at `cobuf + $FC + i*4`: 1 if
   `cobuf[i] >= cobuf[i+1] && cobuf[i+1] >= cobuf[i+2]` (spectral-peak detect).
2. **Beat triggers** (`routine65a`, `gpu/psi.gas:732-823`): for each of 4 configured
   band ranges: scan `cobuf[lo..hi]` for max, `v = sat16(2*max)`; per-band adaptive
   threshold word decays $200/frame and rises to v; sets/clears bits of `trig`
   ($F03F3C) with hysteresis kept in bits 8+ (xor toggles). Omega copies `trig` into
   `fxobj[$F0]` for the live run; gamma/delta gate on `height & _mtrig`.
3. **Editor mailbox** (`gpu/psi.gas:100-197`): sets the `_fsync` word (68k edit loop
   waits on it, `vlm.s:1533`) then executes command word: 1=ADSR envelope plot
   (`gpu/psi.gas:608-693`), 2=single-parameter modulation poke (same math as
   BigLoop64 tail, 222-303), 3=**blitter line draw** (A1_INC/A1_FINC DDA, PATDSEL,
   304-463), 5=oscilloscope trace of a wavetable (565-607), 6/7/8=waveform-table
   plots vertical/horizontal/XY (464-539). Drawing uses a 2×2 plot into `dscreen`
   (`gpu/psi.gas:694-731`) with fixed CRY colours ($8800/$8F00/$F000/$8000).
4. Stop-request handling (`gpu/psi.gas:210-221`).

**Category**: infrastructure + tiny scope/waveform plotters (editor only).

### alpha — "3D starfield" + point/trail/ring drawers (`gpu/alpha.gas`)

Sub-mode = low word of `gpu` field, from $F03F5C (`gpu/alpha.gas:63-79`):
`1` → jump straight to the symmetry/plot entry ($F03392); `2` → sine-trail mode
($F031C8); `9` → ring-of-pixels ($F03646); default `0` → starfield.

**Mode 0 — starfield** (`gpu/alpha.gas:80-191`):
```c
CC from colx/coly narrow fold; plot routine from plot_mod ($E8);
ox = dstoffx >> 12; oy = dstoffy >> 12; oz = dstoffz >> 12;   // sharq #12
count = i($80) >> 16;  pixscale = pixsize($EC)>>16 -> (r14+$1b)
lfsrA = 8; lfsrB = _i1($84); lfsrC = 0xF;                     // 16-bit LFSRs
per star (loop at $F0316C, gpu/alpha.gas:145-187):
    step each LFSR: v = (v & 1) ? (v >> 1) ^ 0x3500 : v >> 1;
    x = ((lfsrA + (ox>>4)) & 0x1ff) - 0xff;
    y = ((lfsrB + (oy>>4)) & 0x1ff) - 0xff;
    z =  (lfsrC + (oz>>4));                     // masked 0x1ff in the divide
    perspective + depth brightness (§4.8, $F03944) -> symmetry -> plot
```
LFSRs reseed identically each frame; motion comes from `dstoff*` advancing (modulation
matrix), so stars stream deterministically. Intensity scaled by pixsize before the
blitter plot (`gpu/alpha.gas:1016-1022`).

**Mode 2 — sine trail** (`gpu/alpha.gas:192-371`): draws `i>>16` points. X starts at
`dstoffx − fmult(vectorx, count<<16)/2` (`gpu/alpha.gas:223-260`) and advances by
`vectorx` per point (352-354); phase `phase1($70) + phase5($CC)` steps by `phase2($74)`
per point; Y offset = `(interp(sine_bas, phase) * k($88)) >> 8` added to `dstoffy`
(325-346); Z accumulates `dstoffz` vs `vectory` with a secondary count `$8C`
controlling segment restarts (359-364); colour phases advance by `cvx2/cvy2` ($D8/$DC)
per point (316-324). Produces the wave/ribbon-of-dots effects.

**Mode 9 — ring of pixels** (`gpu/alpha.gas:676-905`), matches the "Draw a ring of
pixels" sub-effects: setup `stepx=_i1>>12+1, stepy=_i2>>12+1` (alt bank), CC fold,
`count = i>>16`, `radius = $D4`, `angle = phase1($70)`, `astep = phase4($7C)`;
rejects `count<1 || radius>>16 < 4` (736-742); `0x1000000 / count` computed (746).
Per point (747-856): `dx = fmult(radius, interp(sine_bas, angle))`,
`angle += astep`, `dy = fmult(radius, interp(sine_bas, angle))` (i.e. the 90° offset
comes from stepping the phase between the two lookups), plot
`(dstoffx + (dy>>16), dstoffy + (dx>>16))` through the full symmetry chain at $F03392.
Live-run self-update (gated on `draw_screen+4` bit0, `gpu/alpha.gas:858-904`):
`radius += vectory>>3; colx += cvx2; coly += cvy2; pixsize += vectorx>>7`.

**Category**: starfield / parametric point plotters. Pure CPU loop.

### beta — Digital Video Feedback (DVF) area (`gpu/beta.gas`)

The signature Minter feedback zoom/rotate. One blitter pass; no per-pixel GPU loop.

1. CC colour from colx/coly narrow fold (`gpu/beta.gas:7-34`).
2. Window: `w = dvf_ws_x>>16, h = dvf_ws_y>>16` ($04/$08); position from
   `vfb_xpos/vfb_ypos>>16` ($2C/$30) − 192; clipped to 0..384; writes `thang>>16` into
   the `skale` word via `lol[1]` (display scale) (`gpu/beta.gas:35-112`).
3. Clears the four border rectangles around the window with the base colour using
   blitter PATDSEL fills (helper `gpu/beta.gas:572-624`, cmd $10200; callers 113-155).
4. Rotation/zoom vectors (`gpu/beta.gas:225-388`):
```c
sin = interp(sine_bas($A4), vfb_angl($14)) - 0x8000;      // 237-255
cos = interp(sine_bas, vfb_angl + ~90°) - 0x8000;         // 256-274 (idx+0x3f bias)
sx = (w * (vfb_xsca($0C)>>16)) << 8;  sy = (h * (vfb_ysca($10)>>16)) << 8;
Xx =  fmult(sx, cos);  Xy = fmult(sx, sin);   // source step per dest-X step
Yx = -fmult(sy, sin);  Yy = fmult(sy, cos);   // per dest-Y step (277-389)
xincX = Xx / w; xincY = Xy / w;                       // per-pixel A1 inc (390-448)
yincX = Yx / h - xincX; yincY = Yy / h - xincY;       // per-row A1 step
srcX0 = dvf_crot_x($18) + 0x8000 - Xx/2 - Yx/2;       // start corner around the
srcY0 = dvf_crot_y($1C) + 0x8000 - Xy/2 - Yy/2;       //  centre of rotation (449-474)
```
5. Blitter program (`gpu/beta.gas:156-508`): **A1 = source = `cscreen`** (previous
   completed frame, 207-209), flags $34420 = XADDINC|WID384|PIXEL16, A1_PIXEL/FPIXEL =
   src corner, A1_INC/FINC = per-pixel vector, A1_STEP/FSTEP = per-row vector;
   **A2 = dest = `fxobj[0]`** (current draw buffer, 191-194), flags $14420,
   A2_PIXEL = window origin, A2_STEP = (−w, 1); B_COUNT = w | h<<16.
   B_IINC = B_ZINC = B_PATD = B_I3.. = `(dvf_deltai($20) − 0x800000) & 0xFFFFFF`
   (160-185). Command `$41802F41` = SRCSHADE|LFU-copy|GOURZ|DSTA2|UPDA2|UPDA1|UPDA1F|
   CLIPA1|SRCEN — copy source pixels while **adding a signed delta to each pixel's CRY
   intensity byte with hardware saturation** (the feedback fade/bloom). If deltai == 0
   the command drops SRCSHADE/GOURZ (`$1800F41`, `gpu/beta.gas:165-171`).
6. Alternate path when `fxobj[$8C] != 0` (`gpu/beta.gas:196-201`, 530-570): straight
   (unrotated) window copy with only a vertical source offset
   `(fxobj[$1C] − 0xC00000) & 0xFFFF0000` — the vertical-displacement feedback
   variant.

**Category**: digital video feedback / spiral zoom-rotate. Reads the previous frame.
CPU port: per dest pixel, step a 16.16 (x,y) source accumulator (truncate to integer
pixel — the blitter address is integer+fraction, sampling is nearest/truncating);
`out.y = clamp(src.y + delta, 0, 255)` where delta derives from `dvf_deltai−0x800000`
(the exact SRCSHADE scaling of the 24-bit IINC value onto the 8-bit Y should be
verified against an emulator capture — see §7).

### gamma — polygon (Gouraud) object (`gpu/gamma.gas`)

- Trigger gate: if `_mtrig($F4) != 0 && (height($F0) & _mtrig) != _mtrig` → skip the
  whole object (`gpu/gamma.gas:6-18`). Beat-gated polygon flashes.
- Rotation basis (`gpu/gamma.gas:19-52`): `s = table($A4)[(phase1($70)>>16)&0xff]−128`,
  `c = table[idx+64]−128`; deform scales `sx = _i1($84)−0x8000, sy = _i2($88)−0x8000`
  stored `(r14+$1c/$1d)`.
- Object source: `polyos[(thang>>16) & 7]` via `in_buf+$18` (`gpu/gamma.gas:53-68`;
  data `images.s:24+`). Header: word n, word cx, word cy; then per-face records
  (colour word, vertex index words) with the vertex x,y word table at
  `base + n*16` (`gpu/gamma.gas:70-74`, 102-113).
- Vertex transform ×3 per face (blocks `gpu/gamma.gas:98-147, 153-203, 209-259`):
```c
vx = (vert.x - cx) >> 4;      vy = (vert.y - cy) >> 4;      // shrq #4
vx = (vx * sx) >> 11;         vy = (vy * sy) >> 11;         // imult, sharq #11
rx = (vx*c - vy*s) >> 7;      ry = (vx*s + vy*c) >> 7;      // imult matrix 129-139
X  = rx + dstoffx>>16;        Y  = ry + dstoffy>>16;
```
  Each vertex passes the (reduced) symmetry chain, then is *emitted* — writer at
  `gpu/gamma.gas:475-487` stores `{x.w, y.w, (facecol|code).w, pad}` — into a vertex
  buffer (base = `in_buf+$1C` = `cobuf` region; between the three per-face blocks the
  row pointer advances by $3000 = `WID64`, `gpu/gamma.gas:148-152, 204-208`).
- Triangle assembly (`gpu/gamma.gas:268-321`): reads 3 emitted vertices spaced
  $2FFA+6 = $3000 apart (same column, 3 consecutive rows); fetches the 3 corner
  **intensities** from `fxobj[$54 + (code & 0x1C)]` → the `_i1.._i6` params
  (audio-reactive Gouraud corners, `gpu/gamma.gas:294-306`); sign-extends coords.
- Rasteriser (`gpu/gamma.gas:513-1146`): sort 3 vertices by Y (533-568); edge slopes
  `dX = ((x_b−x_a)<<16)/dY` and intensity slopes via the signed-div idiom (578-618);
  walk scanlines in two halves (top 633-857, bottom 858-1023); per scanline the span
  fill (`gpu/gamma.gas:1033-1145`) programs the **blitter**: A1_PIXEL = (x0,y),
  B_PATD = face colour doubled, B_I0..I3 = 4 pixel-phased intensity start values
  stepped by dI/dX, B_IINC = 4·dI/dX (phrase mode), B_COUNT = width|1<<16,
  B_CMD = `$11040` = PATDSEL|GOURD|CLIPA1. Narrow spans (<16 px) take an unshaded
  path (1069-1083); the left edge is pre-stepped 0-3 pixels to phrase-align the
  intensity phases (1085-1105).

**Category**: polygon/shape drawer with Gouraud intensity — the one module where the
blitter's Gouraud unit does per-pixel work. CPU port: classic top/bottom scanline
triangle fill; interpolate intensity in 16.16 with the same truncating divides; colour
byte constant per face. Verify mesh topology (which emitted vertices pair into
triangles) and the 4-pixel Gouraud phasing against a capture.

### delta — Colour plasma area / logo colour-wash (`gpu/delta.gas`)

Bitmap = `bmobj[(thang>>16)&3]` (jlogo2/jaglogo/blokk, `vlm.s:8344-8347`) via `lol[0]`
(`gpu/delta.gas:5-16`); destination read from `dscreen` directly
(`gpu/delta.gas:30-34`). Common setup: dest =
`screen + (dstoffy>>16 − h/2)*0x300 + (dstoffx>>16 − w*16)*2`; bitmap header words
w (in 32-pixel longs), h; colour phases `p1 = colx($E0), p2 = coly($E4)`, per-pixel
steps `cvx>>2, cvy>>2` ($B8/$BC), per-row steps `cvx2>>4, cvy2>>4` ($D8/$DC)
(`gpu/delta.gas:26-81, 125-131`).

Sub-modes (dispatch `gpu/delta.gas:17-25, 83-110`):

- **0** (`gpu/delta.gas:111-190`) — logo plasma: gate on `height/_mtrig` ($F0/$F4,
  111-123); walk the 32 bits of each bitmap long MSB-first (`btst #$1f`, `shlq #1`):
  where set, `CC = fold21(p1)<<4 | fold21(p2)`, `Y = (fold21(p2)>>13) + 64`,
  `storew (CC<<8)|Y`; dest and phases always advance.
- **default/1** (`gpu/delta.gas:191-268`) — logo, fixed chroma: CC folded once; per
  pixel intensity from accumulator `a = _i1($54)<<8` stepped by `cvy>>2`, folded over
  $1FFFFFE, `>>17`, +64 (227-248). Bitmap-gated likewise.
- **2** (`gpu/delta.gas:269-376`) — **solid plasma area**: no bitmap; `w = _i1>>8`,
  `h = _i2>>8` ($84/$88); XOR strobe: if `(height>>8) & _mtrig` → CC ^= $FF (306-351);
  pixel = wide fold CC, `Y = fold>>13 + 64`.
- **3** (`gpu/delta.gas:377-437`) — object recolour: writes `dstoffx`, `dy<<1`, type 5
  into a `beasties` display object via `lol[2]` and stores CRY word
  `fold(colx,coly) | (_i1>>8)` into `clut_sha` (the long before `lol`, `vlm.s:8339`) —
  positions a hardware 1-bit object and colour-cycles it through the CLUT. No pixel
  writes.
- **4** (`gpu/delta.gas:438-554`) — waveform-textured plasma: like 2 but
  `CC = (table($A4)[fold(p1)>>12]>>4)<<4 | (table[fold(p2)>>12]>>4)` (511-521),
  same XOR strobe, `w = _i1>>7` (double width).

**Category**: plasma/interference colour field + colour cycling. Pure CPU loop, plain
overwrites, no feedback.

### epsilon — particle object renderer (`gpu/epsilon.gas`)

Renders the pool maintained by theta. Buffer = `pobjs[(thang>>16)&1]`
(cubeobj/genobj memory reused) via `lol[3]` (`gpu/epsilon.gas:194-206`).

Setup (`gpu/epsilon.gas:26-245`): CC wide fold from colx/coly; three sin/cos pairs via
§4.2 (+$400000 for cos): `(s4,c4)=sincos(phase4($7C))`, `(s2,c2)=sincos(phase2($74)<<4)`,
`(s1,c1)=sincos(phase1($70))`; scales `sx=$84, sy=$88, sz=zamp($6C)`; world offset
`dstoffx/y − 0xC00000`, `dstoffz($34)`; plot table from pixsize/plot_mod
(`gpu/epsilon.gas:35-42`, table at offset $9C0 = lines 1090-1098).

Per particle (32-byte records; loop `gpu/epsilon.gas:246-637`):
```c
if (life_word == 0) { skip; }                      // 265-274
x = (int16)(X>>16)*sx << 8;  y = ..*sy << 8;  z = ..*sz << 8;  // 253-264
CC = colour word (+12); intensity = life word (+14);
three chained 2D rotations via fmult (275-623): tumble by (s1,c1),(s2,c2),(s4,c4)
x += dstoffx; y += dstoffy; z += dstoffz;          // 624-629
z >>= 14 (arith); if (z < 1) reject;               // 1053-1057
X = ((x<<14)/z) >> 7 + 0xC0;  Y likewise;          // 1058-1078
plot via mirror/rotational symmetry chain (867-923) and plot table;
intensity × pixsize variant for the blitter plot (1082-1088)
```
Self-update (only when `draw_screen+4` bit0 = 1, i.e. echo replays,
`gpu/epsilon.gas:638-679`): `$84 += d_x($38)>>12; $88 += ($40)>>12; $6C += deltaz($94)>>12;
_i1($54) += (_i2($58)<<16)>>19` (arith shifts).

**Category**: particle system (render half). Pure CPU loop.

### theta — particle motion integrator (`gpu/theta.gas`)

No drawing. Buffer via `lol[3]` (`gpu/theta.gas:83-90`); `buf[0].l` = live count,
records from +4, stride 32:

```
+0 x.32 (16.16)  +4 y.32  +8 z.32  +12 colour.w  +14 life.w
+16 vx.32  +20 vy.32  +24 vz.32  (+28 pad)
```

- Spawn (`gpu/theta.gas:230-296`): frame divider `dvf_ws_x($04)` counts down; on
  expiry reload from `i($80)>>16`; if `count < 500`, find a slot with life==0 and
  emit: pos = `dstoff* − 0xC00000` (x,y) / `dstoffz`; vel =
  `(vectorx>>4 + cvx<<1, vectory>>4 + cvy<<1, deltaz>>4 + phase5>>1)`
  (`gpu/theta.gas:127-151`); colour = wide fold of colx/coly (194-217);
  `life = _i1($54)|1`; slot marker $FFFF.
- Integrate 512 slots (`gpu/theta.gas:301-349`): live particles:
  `pos += vel; vel += _i3/_i4/_i5 − 0x8000` (per-axis gravity, 154-171);
  `life −= _i2($58)>>4`; `life < 1` → kill, decrement count (335-341, 354-356).

**Category**: particle system (simulation half). Trivial C port.

### sigma — mono particle/vertex object, UNUSED (`gpu/sigma.gas`)

Same skeleton as epsilon (identical sin/cos setup `gpu/sigma.gas:44-193`, rotation
chains 258-605, symmetry 869-925, plot table offset $9D8 = lines 1092-1101).
Differences: vertex source = `mpobjs[(thang>>16)&1]` (monoobj/spherobj) via `lol[4]`
(`gpu/sigma.gas:194-206`); records are 3 words (x,y,z) — a fixed vertex cloud;
colour = wide fold for all points; intensity `_i1>>8`; per-frame update
(`draw_screen+4`-gated, `gpu/sigma.gas:616-681`) advances the scale factors by
`vectorx>>12, ($94)>>12, dy>>12`, colour phases by `cvx/cvy`, `_i1 += (_i2<<16)>>19`.
Marked unused in `vlmgpu.s:120`.

### tau — matrix/voxel object, UNUSED (`gpu/tau.gas`)

Two render paths (exact mode dispatch unresolved; module unused):

- **Voxel lattice** (`gpu/tau.gas:22-96`, loop 487-528): mask =
  `mmasks[(thang>>16)&3]` via `lol[5]` (8×8 bit masks, `vlm.s:8352-8358`).
  Dimensions `nx = vectorx($38)>>16, ny = vectory($40)>>16, nz = deltaz($94)>>16`;
  iterate z,y,x from −n/2; sample byte `mask[(y&7)*8 + (x&7)]` replicated to 32 bits,
  `rorq #1` per z-plane; plot only where the bit is set.
- **1-bit image as dot matrix** (`gpu/tau.gas:530-619`): source =
  `bmobj[(thang>>16)&3]` via `lol[0]`; header w,h; plots set bits as a lattice.

Point pipeline = same two-angle fmult rotation + perspective as epsilon (97-486 via
shared setup 782-968); colour: CC from a narrow fold of `colx/coly>>16` accumulators
with fold-derived intensity `r27 = fold<<11 | 0x8000` (`gpu/tau.gas:97-122`); shared
blitter plot / z-divide trailer (670-780).

**Category**: rotating voxel / dot-matrix object renderer.

### shu — spectrum as intensities (`gpu/shu.gas`)

64-band spectrum display. Spectrum = `absdelta` (64 longs of per-band magnitude from
the DSP) via `lol+$1C` (`gpu/shu.gas:87-91`).

Setup (`gpu/shu.gas:25-140`): post-symmetry translation `($18,$1C)>>16 − 0xC0` stored
in `(r14+$1e/$1f)` and added *after* the symmetry chain (`gpu/shu.gas:468-477`) —
unique to shu; base position `bx = dstoffx>>16 − 32*pix, by = dstoffy>>16 − pix/2`,
`pix = pixsize($EC)>>16`; `ysca = (_i2($88)>>8)|1`; colour steps `cvx2>>4 / cvy2>>4`
($D8/$DC as `sharq #4` of $D8/$DC, 92-101); plot routine via `end_end` table
(`gpu/shu.gas:68-75`, 641-649); intensity fixed $FFFF.

Per band i = 63..0 (`gpu/shu.gas:141-208`):
```c
v = *spec++;  v = sat16(v << 2);
phase3($78) += v << 5;               // stored back: audio spins downstream phases
y = by - ((v << 1) >> 11);           // amplitude displacement
CC = narrow fold of (p9 += cvx2>>4, p10 += cvy2>>4);
x = i * pix + bx;
plot(x, y) through mirror/rotational symmetry, then + ($1e/$1f) offset
```
Blitter plot variant receives `pixsize<<16` as its size (`gpu/shu.gas:636-640`).

**Category**: spectrum/waveform display. Pure CPU loop.

---

## 6. Shared infrastructure summary

- **Sine/waveform tables**: built once by the 68k (`vlm.s:1955-2001`); modules take the
  table *pointer* from fxobj (`sine_bas $A4`, `wave_2 $D0`) so any effect can run on
  saw/square/ramp/rectified tables. 257 entries each — keep the wrap byte for the
  `[idx+1]` interpolation read.
- **CRY intensity arithmetic**: module pixel writes are non-blending overwrites; the
  only saturating adds are (a) beta's blitter SRCSHADE intensity add, (b) `sat16` in
  psi/shu envelope math, (c) gamma's Gouraud intensity stepping inside the blitter.
- **Blitter usage inventory** (everything else is pure GPU stores):
  module-code loads (phrase LFU copies: `gpu/omega.gas:590-649`,
  `gpu/dbeast.gas:133-192`, `vlmgpu.s:41-83`); beta's DVF pass + border fills; gamma's
  Gouraud span fills; the N×N "big pixel" PATDSEL plots in alpha/epsilon/sigma/tau/shu;
  psi's editor line draw.
- **Randomness**: only alpha's three 16-bit LFSRs (taps $3500), reseeded per frame.
- **Audio plumbing**: DSP FFT → psi envelopes (`cobuf`) → psi triggers (`trig`) →
  omega → `fxobj[$F0]` → gamma/delta gating; DSP `absdelta` → shu; oscillators +
  envelopes → omega BigLoop64 → every animated fxobj parameter.

---

## 7. Porting assessment (CPU CRY16 buffer + GLES2 CRY→RGB pass)

Target: render every effect on CPU into a 16bpp CRY buffer (384-wide, stride $300,
double-buffered) exactly as the original; one GLES2 textured-quad pass converts
CRY→RGB. One shared code path; nothing below needs a per-effect shader.

| Module | Difficulty | Framebuffer feedback? | Notes |
|---|---|---|---|
| omega | easy (pure int math) | – | must be ported faithfully: oscillators + BigLoop64 *are* the animation. Keep 6-object order, echo replay, `draw_screen+4` gating, `trig` injection. |
| dbeast | trivial | – | 768-byte memcpy into a 64×$600 ring + index math. |
| psi | easy | – | envelope/trigger math is small; editor commands optional (DDA line, table plots). Needs an FFT stand-in feeding the `$F1B034` bins. |
| alpha | easy | no | straight loops; keep LFSR, unsigned `div`, `sharq` semantics, and the shared plot/symmetry chain as common C routines. |
| beta | moderate | **yes** (reads previous frame `cscreen`) | replace the blitter pass with a per-pixel loop: 16.16 source stepping (truncating), constant signed intensity add clamped 0..255. The one module whose output depends on blitter behaviour (SRCSHADE scaling) — validate against an emulator capture. |
| gamma | moderate | no | scanline Gouraud triangle fill emulating blitter stepping; verify mesh topology ($3000-row vertex buffer) and 4-pixel GOURD phasing against capture. Beat-gated by `_mtrig`. |
| delta | easy | no | four plasma variants + CLUT mode (map CLUT mode to recolouring the 1-bit overlay in the frontend). Wide fold + `>>13 + 64` intensity must be exact. |
| epsilon | easy-moderate | no | long but mechanical fmult rotation chains; implement fmult sign-magnitude (§4.3) or results drift ±1 LSB. |
| theta | trivial | – | pure simulation; shares the particle record layout with epsilon. |
| sigma | easy | no | unused in shipped banks; 90% shared with epsilon. |
| tau | easy-moderate | no | unused; voxel loops trivial, mode dispatch needs runtime confirmation. |
| shu | easy | no | needs the `absdelta` spectrum feed; otherwise 64 iterations of the shared plot pipeline. |

**Recommended shared C core** (mirrors the duplicated GPU code):
`cry_fold_narrow/wide()`, `wave_interp()` (§4.2), `fmult16_16()` (sign-magnitude),
`sdiv()` (truncate toward zero), `plot_pipeline(x, y, cc, intensity, fxobj)`
implementing rotational symmetry → mirror chain → {1×1, 2×2, N×N} plot with the $17E
clip, and `persp_divide()` (both flavours). Each module then reduces to its literal
inner loop.

**GLES2 pass**: upload the CRY buffer as a 2-channel texture; fragment shader:
`cc = texel.hi; y = texel.lo; rgb = cryLUT[cc] * y/255.0` using a 256×1 RGB LUT texture
built from the standard Jaguar CRY chroma table (hardware computes
`component * Y >> 8`). Border/overlay 1-bit objects (delta mode 3, logos) are separate
small quads or pre-composited into the CPU buffer.

**Bit-accuracy checklist**: unsigned `mult` vs signed `imult` at each site; `sharq`
(arith) vs `shrq` (logical) exactly as written; truncating sign-magnitude divides;
free 16.16 wraparound (never clamp phase accumulators); the $17E clip; 257-entry
tables; `sat16` only where the GPU used it; and the frame-order side-effects
(parameter self-updates inside modules run only when `draw_screen+4` bit0 = 1, i.e.
during echo replays — live parameters advance solely via omega's BigLoop64/oscbank).

**Open items to verify at runtime** (flagged above): beta's SRCSHADE intensity
scaling; gamma's vertex-buffer topology and Gouraud phrase alignment; tau's
second-entry dispatch; the DSP-side producers of `absdelta`/`avbank`/`envvals`
(`ians.s`, outside this analysis).
