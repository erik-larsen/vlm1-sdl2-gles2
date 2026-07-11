# VLM-1 Bank Data (`banks.s`) — Analysis for the C Port

Source tree analyzed: `/Users/larsens/Github/vlm-ref/src/vlm/`
Key files: `banks.s` (42,139 lines), `vlm.s` (8,562 lines), `vlm.inc`, `vlmgpu.s`, `sines.s`, `images.s`, `font.s`, `images/*.cry`.

---

## 1. Overall layout

### 1.1 What banks.s actually is

`banks.s` contains **no code, no labels, no expressions** — it is a single
anonymous run of `dc.b` lines (verified by scan, see counts in §4) totalling
**46,800 bytes** of *compressed* bank data, plus extensive explanatory
comments. It is textually `.include`d at the very end of `vlm.s`
(`vlm.s:8559`), immediately after the `av1` label, so the bytes land directly
after the `av1` header in ROM.

The header comment block (`banks.s:15-58`) explains the scheme; the
authoritative decompressor is `gm` at `vlm.s:7017`.

### 1.2 The av1 container (vlm.s:8522-8559)

```
avbank: (vlm.s:8522)      6 x 4 dc.w  = 48 bytes   spectrum/trigger bands (start, end, min-trigger, unused)
av1:    (vlm.s:8535)      16 bytes header:
        +0  dc.l $0000000A          ; number of banks (10)
        +4  dc.l $0090B69C          ; end-of-data marker (offset $B69C from av1)
        +8  dc.l $00900038          ; offset of first (unused) bank region ($38)
        +12 dc.l $F4352AC6          ; junk/padding
av1+16: 10 x dc.l bank offset table (vlm.s:8542-8551):
        $00900200 Bank 1 ... $00909A72 Bank 9, $0090A6B4 Bank 10 (splash only)
av1+56 ($38): first byte of banks.s data
```

The `$0090xxxx` values are pseudo-pointers: `gm` subtracts `$00900000` and adds
the runtime address of `av1` (`vlm.s:7027-7035`), so the low 16 bits are simply
**byte offsets relative to the `av1` label**. `av1ref` (`vlm.s:7462`) holds the
address of `av1`.

### 1.3 Bank regions inside banks.s (offsets relative to av1)

Verified by summing every `dc.b` byte against the bank divider comments:

| Region              | av1 offset      | banks.s line | size (bytes) |
|---------------------|-----------------|--------------|--------------|
| Bank 0 (unused junk)| $0038           | 63           | 456          |
| Bank 1              | $0200           | 121          | 5,718        |
| Bank 2              | $1856           | 4458         | 7,246        |
| Bank 3              | $34A4           | 8986         | 4,062        |
| Bank 4              | $4482           | 13116        | 4,852        |
| Bank 5              | $5776           | 17345        | 2,658        |
| Bank 6              | $61D8 (*)       | 21300        | 5,070        |
| Bank 7              | $75A6           | 25556        | 4,704        |
| Bank 8              | $8806           | 29766        | 4,716        |
| Bank 9              | $9A72           | 33978        | 3,138        |
| Bank 10 (splash)    | $A6B4           | 37993        | 4,072        |
| "Bank 11" (trailing)| $B69C..$B708    | 42123        | 108          |

(*) The `; Bank 6` divider comment in banks.s sits 3 bytes late (computed
$61DB); the `av1` table value `$61D8` is authoritative. Always slice banks by
the `av1` offsets, never by the comments. The trailing 108 bytes after the
declared end marker `$B69C` ("Bank 11", `banks.s:42123-42138`) are never
referenced by the table — dead data, but keep them in the blob so offsets stay
exact.

### 1.4 Logical structure: 9(+1) banks x 9 effects x 6 sub-effects

- Each **bank** decompresses to 9 effects x 6,144 bytes = **55,296 bytes**
  placed at `matrix` (RAM `$1D83F8`, `vlm.inc:177`).
- Each **effect** = 6 sub-effect "fx objects" of **1,024 bytes** each.
- Each 1,024-byte **fx object** (`banks.s:36-41`, `vlm.s:7008-7013`):
  - bytes 0-255: 64 big-endian longs of parameters (full field list in §2)
  - bytes 256-511: buffer, initialized to `$0000FFFF` longs by `ifxobj`
  - bytes 512-767: buffer, initialized to 0
  - bytes 768-895: 8 oscillator records x 16 bytes (copy of `oscbank`,
    `vlm.s:8423-8430`)
  - bytes 896-1003: copy of the `pixcon`..`delayn` global block (108 bytes,
    `vlm.s:8432-8451`)
  - bytes 1004-1023: slack

The bank data is stored as a **delta/patch stream** against a "base layer":
effect 1 is patched against `refblock` (six pristine fx objects built by
`ifxobj` at startup, `vlm.s:292-297`, `refblock` = RAM `$1B4BF8`); each
subsequent effect is patched against a copy of the previous effect
(`banks.s:43-54`).

For human reference, every bank's section in banks.s is preceded by comments
dumping the decompressed first 256 bytes of all 54 fx objects with field
names/descriptions (e.g. `banks.s:124-189` for Bank 1 / Effect 1 /
Sub-Effect 1) — extremely useful for validating the port's decompressor.

---

## 2. Exact binary layouts

### 2.1 Compressed stream (what's in banks.s)

A sequence of **(skip, value) byte pairs**. Decompression per effect
(`gm`, `vlm.s:7052-7063`):

```
pos = 0
loop:
    skip = *src++; value = *src++          # unsigned bytes
    if pos >= 6144: break                  # checked BEFORE adding skip
    pos += skip
    if pos >= 6144: break
    dest[pos] = value
    pos += 1
```

Termination quirk that the port MUST replicate: the pair that causes the bail
is **re-consumed as the first pair of the next effect** — `gm` rewinds with
`lea -2(a0),a0` (`vlm.s:7083`) before decoding effect N+1. There is no length
field and no terminator byte; the stream for one effect simply ends when the
running position crosses 6,144.

Patches address **individual bytes** (not longs), so decompression must be
done on a raw byte image in big-endian layout; only interpret fields as 32-bit
values *after* patching.

### 2.2 The 1,024-byte fx object — parameter block (bytes 0-255)

All fields are **32-bit big-endian longs**. Offsets are the `EQU`s in
`vlm.inc:210-294`; the annotated table lives at `vlm.s:1740-1806` and
`banks.s:124-189`. Complete list:

| Off | Name | Meaning | | Off | Name | Meaning |
|-----|------|---------|-|-----|------|---------|
| $00 | —        | (scratch/flags)          | | $80 | i        | iteration count |
| $04 | dvf_ws_x | DVF window size X        | | $84 | j        | X amplitude |
| $08 | dvf_ws_y | DVF window size Y        | | $88 | k        | Y amplitude |
| $0C | vfb_xsca | DVF scale X              | | $8C | —        | other-iteration count |
| $10 | vfb_ysca | DVF scale Y              | | $90 | col1     | (colour param) |
| $14 | vfb_angl | DVF rotate angle         | | $94 | deltaz   | delta Z |
| $18 | dvf_crot_x | DVF rot centre X       | | $98 | thang    | choice of Thang |
| $1C | dvf_crot_y | DVF rot centre Y       | | $9C | —        | undefined |
| $20 | dvf_deltai | DVF delta intensity    | | $A0 | asym_fla | (flags) |
| $24 | dstoffx  | dest position X          | | $A4 | sine_bas | **pointer**: waveform table |
| $28 | dstoffy  | dest position Y          | | $A8 | rxcen    | rot-sym centre X |
| $2C | vfb_xpos | DVF window centre X      | | $AC | rycen    | rot-sym centre Y |
| $30 | vfb_ypos | DVF window centre Y      | | $B0 | roscale  | rot-sym scale |
| $34 | dstoffz  | dest position Z          | | $B4 | roscal2  | rot scale delta X |
| $38 | vectorx  | vector X                 | | $B8 | cvx      | colour gen vector X |
| $3C | dy       | dest Y offset            | | $BC | cvy      | colour gen vector Y |
| $40 | vectory  | vector Y                 | | $C0 | roscalei | rot scale delta Y |
| $44 | —        | symmetry types           | | $C4 | drxcen   | rot centre delta X |
| $48 | rsym_ord | rot symmetry order       | | $C8 | drycen   | rot centre delta Y |
| $4C | rsym_ste | rot angle step           | | $CC | phase5   | delta phase 2 |
| $50 | rsym_ist | rot angle step delta     | | $D0 | wave_2   | **pointer**: waveform table |
| $54-$68 | _i1.._i6 | intensities 1-6      | | $D4 | radius   | radius |
| $6C | zamp     | Z amplitude              | | $D8 | cvx2     | base col gen vector X |
| $70 | phase1   | fixed-point phase 1      | | $DC | cvy2     | base col gen vector Y |
| $74 | phase2   | delta phase 1            | | $E0 | colx     | base colour X |
| $78 | —        | rot-sym overall phase    | | $E4 | coly     | base colour Y |
| $7C | phase4   | fixed-point phase 2      | | $E8 | plot_mod | dest plot routine |
|     |          |                          | | $EC | pixsize  | max pixel size |
|     |          |                          | | $F0 | height   | undefined |
|     |          |                          | | $F4 | _mtrig   | trigger mask |
|     |          |                          | | $F8 | info     | sub-effect type (index into `vars`; 0 = empty slot) |
|     |          |                          | | $FC | gpu      | GPU module selector |

`gpu` field encoding: **high word = index into `gpumods`**
(`vlmgpu.s:113-124`: 0 alpha, 1 beta, 2 gamma, 3 psi, 4 delta, 5 epsilon,
6 theta, 7 sigma, 8 tau, 9 shu, 10 dbeast); **low word = variant parameter**
(e.g. `$00040001..$00040004` select plasma variants, `vlm.s:625-771`;
`$00000009` = alpha variant 9 "ring of pixels", see table at `vlm.s:79-114`).
It is an index, not a pointer — no fixup needed.

### 2.3 Oscillator records (bytes 768-895)

8 records x 16 bytes (`vlm.s:8415-8430`, consumed by `omega.gas` and the
`skidoo` fixup loop `vlm.s:4616-4625`):

| Off in record | Size | Meaning |
|---------------|------|---------|
| +0  | long | current value / initial value |
| +4  | long | increment per frame |
| +8  | long | **pointer** to waveform table (p_sines etc.) |
| +12 | word | (unused / flags) |
| +14 | word | waveform **index** into `sineslookup` (`vlm.s:7677-7685`: 0 p_sines, 1 p_saw, 2 p_square, 3 p_ramp, 4 p_rect, 5 rantab, 6-8 p_sines) |

### 2.4 pixcon block (bytes 896-1003)

Straight 108-byte image of the globals `pixcon`..`delayn`
(`vlm.s:8432-8451`): `pixcon`/`piycon` (7 longs each: position, velocity,
bounds, etc.), `adsra/adsrb/adsrc` envelope definitions (10/10/12 bytes),
`py`, `px` (longs), `delayf`, `delayp` (words), `dline` (long), `delayt`,
`delayn` (words). `skidoo` copies this block from fx-object 1 back into the
live globals (`vlm.s:4631-4635`).

### 2.5 Endianness and embedded pointers

- The compressed stream is byte-oriented — **endian-neutral**. The
  decompressed image is a byte array whose fields are **big-endian**.
- Embedded **absolute Jaguar addresses** appear in exactly three places:
  - `sine_bas` ($A4) and `wave_2` ($D0): values like `$001E7540` = `p_sines`
    (`vlm.inc:179`; the five tables are consecutive: p_sines `$1E7540`,
    p_saw `$1E7644`, p_square `$1E7748`, p_ramp `$1E784C`, p_rect `$1E7950`,
    each 260 bytes apart). These are *constants* baked into the data because
    the RAM layout was fixed by EQUs — not link-time relocations.
  - Oscillator field +8: also a waveform pointer, but **it is dead data on
    load** — `skidoo` (`vlm.s:4619-4623`) rewrites it from the index word at
    +14 via `sineslookup` (index 6 is skipped/left as-is: `cmp.w #24,d3`).
- There are **no relocation records**; the data is consumed via the
  `av1`-relative offset table only.

---

## 3. Runtime load path (vlm.s)

1. **Startup** (`vlm.s:292-308`): `ifxobj` (`vlm.s:1808-1901`) builds six
   default 1,024-byte fx objects at `refblock` ($1B4BF8) — this is the
   compression base layer. Default values are the `move.l #...,field(a6)`
   list at `vlm.s:1816-1871` (they match the Bank 1/Effect 1 comment dump).
2. **Bank select** (`selmat`, `vlm.s:4576`; also `vlm.s:4446-4448`): stores
   bank number 0-9 in `imatrix`, arms `action = getmatrix/gm`.
3. **`gm`** (`vlm.s:7017-7104`):
   - fills `matrix` (55,296 bytes at $1D83F8) with `$FFFFFFFF`;
   - computes source = `av1 + (av1[16 + 4*imatrix] - $00900000)`;
   - blits `refblock` -> `matrix` (6,144 bytes, `blitcopy` vlm.s:7127);
   - patch-decompresses effect 1 (loop `unp`, vlm.s:7054-7063);
   - for effects 2-9: blit previous effect forward 6,144 bytes, rewind
     source by 2, patch-decompress (`rrest`/`unpk2`, vlm.s:7073-7097);
   - falls through to `skidoo`.
4. **`skidoo`** (`vlm.s:4595-4639`) — effect select (`skid` = 1..9):
   - does **not** copy the effect; it stores **pointers to the six fx objects
     inside `matrix`** into the slot array at `fxedbase+8` (NULL if
     `info == 0`). Effects therefore run **in place in `matrix`**;
   - fixes up the 8 oscillator waveform pointers per §2.5;
   - copies fx-object-1 bytes 896-1003 into the `pixcon`..`delayn` globals.
5. The GPU main loop (`omega.gas`) walks the fx-object pointers, dispatching
   on `gpu >> 16` through `gpumods` (`vlmgpu.s:113`).

Bank 10 ($A6B4) is loaded only for the splash screen (`imatrix = 9`,
`vlm.s:234`). The editor's save path (`dostash`, `vlm.s:3359`) re-derives
deltas against `refblock`, confirming the format symmetry.

---

## 4. Extraction strategy for the port

### 4.1 Survey of banks.s syntax (real counts)

Scanned all 42,139 lines:

| Element | Count | Notes |
|---|---|---|
| `dc.b` lines | **5,854** | the ONLY directive present |
| bytes total | **46,800** | 5,845 lines x 8 bytes; short lines: 1x2, 1x3, 3x4, 1x5, 3x6 |
| operand forms | **1** | only `$XX` two-digit hex literals, comma-separated (regex-verified: zero non-conforming lines) |
| labels | **0** | none |
| expressions / symbol refs | **0** | none |
| `dc.w` / `dc.l` / `dcb` / `.incbin` | 0 | |
| comment lines | ~35,700 `;` lines + 4 bare `*...` divider lines | includes full decompressed-field dumps per effect |

### 4.2 Options

- **(a) Python parser -> C header/blob — RECOMMENDED.** The grammar is one
  regex. Deterministic, no toolchain dependency, byte counts are
  self-checking, and the same script can also emit the offset table and
  (optionally) pre-decompressed banks. A worked Python decompressor already
  exists in the reference repo:
  `/Users/larsens/Github/vlm-ref/notebooks/Unpack Banks and Effects from av1.ipynb`
  (crib the unpack loop from it / from `gm`).
- **(b) rmac + extract section**: the repo's Makefile uses `./rmac/rmac`, but
  the binary is **not present/built** in `/Users/larsens/Github/vlm-ref/rmac/`
  (empty dir), and you'd still need to locate the data inside a linked `.abs`
  and re-derive the `av1` base. All cost, no benefit for a label-free
  byte-only file.
- **(c) Hand conversion**: 46,800 bytes — not viable.

### 4.3 Concrete steps (option a)

Script `scripts/extract_banks.py`:

1. **Parse** `vlm-ref/src/vlm/banks.s`: for each line matching
   `^\s*dc\.b\s+(.*)` strip any `;` comment, split on `,`, require each token
   to match `\$([0-9A-Fa-f]{2})`, append bytes. Assert total == 46,800.
2. **Prepend the 56-byte av1 header** so runtime offsets match the original
   exactly (recommended): emit
   `00 00 00 0A | 00 90 B6 9C | 00 90 00 38 | F4 35 2A C6` + the ten
   `$0090xxxx` longs (from `vlm.s:8536-8551`), then the 46,800 bytes.
   Blob size = 46,856. Alternatively skip the header and emit a C table
   `bank_offsets[10] = {0x0200-0x38, 0x1856-0x38, ...}` — but keeping
   av1-relative offsets ($0200 etc.) verbatim minimizes porting friction.
3. **Emit** either `assets/banks.bin` + a tiny generated header with the
   offsets, or a single `src/banks_data.h` with
   `static const uint8_t vlm_av1[46856] = {...};` and
   `static const uint16_t vlm_bank_off[10] = {0x0200, 0x1856, 0x34A4, 0x4482,
   0x5776, 0x61D8, 0x75A6, 0x8806, 0x9A72, 0xA6B4};`
   (endianness of the blob is irrelevant — it's a byte array).
4. **Port the decompressor in C**, mirroring `gm` exactly (including the
   pre-add bail check and the "terminating pair re-read" rewind) and
   `ifxobj` for the refblock base layer. Decompress into
   `uint8_t matrix[9][6][1024]` kept in **big-endian byte layout**.
5. **Field access / endianness**: because patches hit individual bytes, do
   NOT store the matrix as native-endian structs. Either
   - keep bytes BE and read fields with `be32(&obj[off])` / `be16()`
     helpers (simplest, recommended), or
   - after decompression, convert each fx object once: bytes 0-255 as 64
     BE longs, oscillator records as long/long/long/word/word, pixcon block
     per its mixed long/word/byte layout (§2.4) — then use native structs.
6. **Pointer/index fixups** (replaces absolute Jaguar addresses):
   - map `sine_bas`/`wave_2` values to waveform-table indices:
     `idx = (value - 0x1E7540) / 0x104` (p_sines..p_rect are 260 bytes
     apart, `vlm.inc:179-183`); treat anything else defensively as sine.
   - ignore stored oscillator pointers (+8); reproduce `skidoo`'s fixup from
     the index word at +14 via a C `sineslookup[]` (skip index 6).
   - `gpu` (module = hi word, variant = lo word) and `info` are indices —
     use directly.
7. **Validate**: decompress Bank 1 and diff the first 256 bytes of each of
   the 54 fx objects against the comment dumps in `banks.s` (e.g. Effect 1 /
   Sub-Effect 1 must read `$00148000, $01483800, $01122000, $01850000, ...`,
   `banks.s:126-189`). This gives 9 x 6 x 64 checked longs per bank for free.

---

## 4.5 Validation results (port, 2026-07-11)

- `scripts/extract_banks.py` output (46,856-byte av1 blob) is
  **byte-identical** to the av1 region of the original binary
  `vlm-ref/src/unused/objs/vlm.abs` (found at file offset 0x90f8) —
  the strongest possible check; the comment dumps below are secondary.
- The C decompressor (`src/banks.c`, runtime `gm` semantics: refblock
  base per bank) matches all 3,456 documented longs for Bank 1.
- The comment dumps in banks.s were generated by a **continuous**
  decompression across banks (bank N effect 1 layered on bank N-1
  effect 9, stream rewound 2 bytes across the boundary) — this does
  NOT match runtime `gm`, which resets the base to refblock on every
  bank load. Under the continuous model, banks 1-5 dumps match 100%
  (13,824/13,824 longs). Banks 6-10 dumps match neither model and show
  near-miss values, indicating they were generated from a slightly
  different (older) revision of the data; ignore them for validation.
- Practical upshot: **the port's `vlm_load_bank` (refblock base) is
  what the Jaguar executes at runtime.** What players saw for banks
  2-10 effect 1 differs from the reference repo's comment dumps.

## 5. Other binary assets (brief)

| File | Contents |
|---|---|
| `sines.s` | Label `sines:` — pure `dc.b` tables: a 256-entry signed 8-bit sine followed by 16-bit waveform data; startup code (`vlm.s:1955-...`) expands these into the five 260-byte tables at `p_sines`..`p_rect` plus `rantab` (`vlm.s:7394`). Trivial to parse the same way as banks.s. |
| `images.s` | `polyos:` table of 8 pointers (`dc.l`) to small polygon-object blobs (`chev*`, `square`, word-structured `dc.b` data) + `vlmlogo2: .incbin "images/vlmlogo2.cry"`. |
| `images/` | 5 CRY-format bitmaps: `jaglogo.cry`, `jlogo2.cry`, `vlm-grafix.cry`, `vlmlogo.cry`, `vlmlogo2.cry` (Jaguar CRY colour space; see `vlm-ref/notebooks/Parse CRY Files.ipynb` and `cry_rgb.py` for conversion). |
| `font.s` | `myFont:` — `dc.l` header, `dc.w` index table (char -> glyph offset), then 1-bpp glyph bitmaps. |
| `avbank` (in vlm.s:8522) | 6 x 4 `dc.w` audio trigger-band records (start band, end band, threshold, unused) — needed by the port's audio-reactive path; not part of banks.s. |

Reference notebooks in `/Users/larsens/Github/vlm-ref/notebooks/` worth
mining: `Unpack Banks and Effects from av1.ipynb`, `Reformat banks.s.ipynb`,
`Regenerate avbank.s.ipynb`, `Parse CRY Files.ipynb`.
