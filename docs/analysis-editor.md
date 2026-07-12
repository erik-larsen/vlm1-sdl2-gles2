# VLM-1 Editor / UI Layer Analysis — Port Specification

Analysis of the hidden effects editor and all text/HUD machinery in
`/Users/larsens/Github/vlm-ref/src/vlm/vlm.s` (line refs are to that file
unless marked `psi.gas:` or `font.s:`). Companion to
`analysis-68k-engine.md`. This document is the implementation spec for the
editor; the goal is pixel-faithful reproduction.

Build caveat used throughout: this is the **Jaguar CD (cdfront) build**. A
number of editor screens exist in the binary but are orphaned (no reference
from any live menu). They are documented anyway, flagged **[ORPHAN]**, since
the port may choose to re-wire them (the strings for the full dev menu are
all present).

---

## 0. How the editor runs: three cooperating loops

1. **`Frame` vsync ISR (4244)** — the *input* side. Every vblank, after
   `readpad` (4363), if `vlm_mode != 0` and `editing != 0` and no `action`
   pending, it dispatches `editvex[editing-1]` (4377–4388). These routines
   read `pad_now` and mutate state (`selected`, `ixcon`, params...). They
   never draw.
2. **`editloop`/`eloop` (1526–1543)** — the *drawing/menu* side, on the 68k
   main loop (only when `freerun == 0`). Waits for `_fsync != 0` (set by the
   GPU), calls `yakedit` (1552), then clears `_fsync`.
   `yakedit`:
   - handles the `actime` auto-clear countdown (1553–1558: when it hits −1,
     prints `clearstr` `"@"` = clear board);
   - refreshes `fxobj` = `fx[fxed]` (1561–1565);
   - if `action != 0`: calls it with `a6 = fxedbase`, then clears it
     (1568–1573). **`action` is the single-shot deferred-call mechanism**
     used for every screen transition (it must run on the main loop, not in
     the ISR).
   - snapshots `ixcon`→`iixcon`, `iycon`→`iiycon` (1575–1583);
   - if `symed != 0`, calls `edvex[symed-1]` (1586–1592) — the *control
     display* routine for the current screen (sliders, crosshair, spectrum,
     ...). Several of these also **write the edited value back into the
     fxobj** via the GPU `_fmode` services (see §5.2).
   - calls `shad` (2322) — draws the ADSR overlay if `aded != 0`.
3. **`psi.gas` (GPU)** — the *blit server*. Runs once per frame after all
   sub-effect modules. When `freerun == 0` it sets `_fsync = 1`
   (psi.gas:120–123), then **busy-polls `_fmode`**, servicing draw/compute
   requests issued by the 68k display routines, until the 68k clears
   `_fsync` (psi.gas:124–199). Handshake variables `_fsync/_fmode/_ein_buf/
   _monptr` live in `vidinit.s:31–36` (contiguous — `_ein_buf+12` **is**
   `_monptr`, and psi relies on that).

For the port: one `editor_tick()` per frame replaces (1)+(2), and the
`_fmode` services become plain C functions (§5.2).

---

## 1. Text / print pipeline

### 1.1 The board

- `board` (`vlm.inc:174`, $1B00F8) is a **320×240, 1 bpp** overlay bitmap,
  stride 40 bytes, MSB = leftmost pixel.
- It is displayed by **beastie 2** (created at 355–367): screen position
  x = 24 − `palside`, y = 60 + `paltop`, ObType 1 (`ObTypes` 7316: 5 phrases
  wide = 40 bytes = 320 px, 240 tall, depth 0 = 1 bpp), transparent. Its
  colour comes from CLUT entries 0/1, which are set to $FFFF (white) at 251.
- The overlay is **off** by default (`move #-1,beasties+140`, 367) and
  turned on (`move #1,beasties+140`) by `setedit` (1293) and the (dead)
  `vlm_on` (4400).

### 1.2 Character cell and `charblit` (6291)

- Glyphs are 8×8 1-bpp. Cell pitch is **8 px horizontally, 9 px
  vertically**: `charblit` computes A1_PIXEL x = `col*8`, y = `row*9`
  (6304–6309). So there are 40 columns and 26 text rows on the board.
- Blit: 8×8, `B_PATD = 0`, command `$1800609` normal, `$600609` when
  `inverse != 0` (6315–6318). Effect: the whole 8×8 cell is replaced —
  glyph bits set → white, clear → black (inverted when `inverse`), i.e.
  printing a space erases a cell.
- `inverse` is a global word toggled by the `~i` print command and by
  `ud_butts` for highlighted keypad buttons.

### 1.3 `myFont` format (font.s:18) — **exact spec, with off-by-8 trap**

```
myFont+0   dc.l $80008          ; 8x8 (B_COUNT-style h<<16|w)
myFont+4   142 dc.w entries     ; index table, one word per char code 0..141
myFont+280 glyph data           ; 8 bytes per glyph, 1 row per byte, MSB left
```

- Char code c → word at `myFont + 4 + 2*c` → **byte offset from `myFont`**
  of the 8-byte glyph (`charblit` 6296–6298).
- **The `; 280`-style comments in font.s are off by 8.** The index table is
  4 + 142×2 = 288 bytes, so the first data line (`$20100804,...`, labelled
  "280") actually sits at offset **288**. Index value 280 (used for space,
  all control chars and all undefined codes) points into the last 8 bytes of
  the index table itself, which are `0,0,0,0` — i.e. **offset 280 = the
  blank glyph**. Verified: '0'($30)→424 = line labelled 416
  (`$7C82868A,$92A2C27C`, a zero), '1'→432 = line 424, '<'→296 = line 288
  chevron, etc. When embedding the font in C, either keep the byte-exact
  layout or re-base every index by −8 onto the data block and special-case
  280→blank.
- Special glyphs: char **0** = 920 → `$C0F0FCFE,$FEFCF0C0` = solid
  right-pointing triangle — **the menu cursor**. Chars **1–4**
  (1016/1024/1032/1040) are the delay-line gauge blocks: 1 = solid $FF
  block, 2 = block open on the right ($FF8F8F8F...), 3 = open on the left
  ($FFF1F1F1...), 4 = hollow box ($FF818181...). (Char 4's offset 1040 is
  8 past the last data line — i.e. whatever follows; harmless garbage in
  ROM, give it the hollow-box data in the port.) Chars **$80–$89** =
  rounded "keypad" digits 0–9 (offsets 936–1008), char $23 '#', $2A '*'
  use the normal set.

### 1.4 `print` (3928) — the control-code language

State: `cursx/cursy` (text col/row, persisted across calls — 4046–4048
stores the final position back), local cursor d0/d1 while printing.
`a0` = NUL-terminated string. Every printable byte: `charblit` at (d0,d1),
then d0++ (4041–4044).

| Code | Handler | Meaning |
|---|---|---|
| `/` | 3936–3940 | newline: row++, col = `cursx` (the call-entry column) |
| `` ` `` | 3957–3962 | row−−, col = `cursx` |
| `@` | 3942–3949 | clear whole board (`cleol`), cursor preserved |
| `^` | 3951–3955 | clear whole board, cursor becomes (0,0) (d0/d1 clobbered by `cleol`) |
| `~cN:` | 3969, 4036–4039 | set `actime = N` → board auto-clears N frames later (via `yakedit` 1553) |
| `~gX:Y:` | 3971, 4029–4034 | goto: col = X, row = Y (decimal, `:`-terminated, parsed by `gnum` 4054) |
| `~eX:Y:` | 3973, 3990–4027 | render the current menu (`editlist`) — see below |
| `~i+` / `~i-` (any non-`+`) | 3975, 3978–3987 | inverse video on / off |
| other `~x` | 3976 | ignored |

`gnum` (4054): decimal accumulator (`d2 = d2*10 + digit`), stops at `:`.

`cleol` (6329) despite the name clears the **entire** 320×240 board
(blit 0,0 size $140×$F0, value 0).

`cprint` (3919) = print `clearhom` `"@~g1:1:"` (7505) then print `a0` —
"clear screen, home to (1,1), print".

### 1.5 `~e` menu rendering (`edtable` 3990) and the `editlist` structure

`editlist` points to an option table:

```
+0  .w  selectable      ; LAST valid index (count-1)
+2  .w  selected        ; initial selection
+4  .l  bottom-line string (printed first; strings start with ~g so they self-position)
+8  pairs of { .l text, .l action-routine }   ; (selectable+1) pairs
```

`edtable`:
1. prints the bottom-line string (3993–3995);
2. parses X:Y → `selx = X`, `sely = Y` (3996–4001);
3. loads `selectable`/`selected` from the table (4004–4006);
4. prints each entry's text at (`selx`, `sely+i`) (4009–4017);
5. draws the cursor glyph (char 0, the triangle) at
   (`selx`−1, `sely`+`selected`), records it in `cursorx/cursory`
   (4019–4025);
6. resumes printing the rest of the heading string.

`ud_selcu` (3588) moves the cursor: blit `' '` at old `cursorx/y`, blit
char 0 at (`selx`−1, `sely`+`selected`). It is always invoked as an
`action` (`sud` 5584).

### 1.6 elcon (1628) — building parameter menus from `pbinfo`

`elcon(a0=elspace, a1=param-index list, d0=$10 max)` walks a byte list of
pbinfo indices (FF-terminated, see §4.6), dedupes via `elinx[64]`, and
fills:

- `elspace`: the editlist header (count = entries−1, selected = 0, bottom
  line = `bline2`) followed by pairs { text, `eparam` } — or { text,
  `unquit` } when the pbinfo entry's edvex byte (+32) is 0 (1659–1663).
- `elvex`: parallel array of pointers to each entry's 10-byte var block
  (pbinfo entry offset +30) — `eparam` later indexes this with `selected`.
- For entries whose +30 bit0 is set (X of an X/Y pair), it synthesizes
  composite text in `eltxt`: copy up to the `:`, then `" (X,Y)"` built from
  the char after "`: `" of each linked entry, following the +31 chain
  (1675–1720). E.g. `DVF window size: (X,Y)`.

Max 16 entries (`d0=$10`). The count register trick: d1 starts at −1 (left
over from the `elc1` clear loop's `dbf`) and pre-increments per entry, so
the stored `selectable` = N−1.

`unquit` (3899) prints `unimp` (7507)
`"This function not yet implemented~c30:"`.

---

## 2. Normal-mode HUD (non-edit)

### 2.1 Boot banner

`everything` prints `versionp` at (1,1) (324–328):

```
Virtual Light Machine v0.9//(c) 1994 Virtual Light Company Ltd./Jaguar
CD-ROM version/(c) 1994 Atari Corporation//FFT code by ib2/Grafix code
by Yak/~c100:
```

(7463–7465). `~c100:` auto-clears the board after 100 frames. Note the
board overlay object is off at this point in CD mode; on the console build
it was visible.

### 2.2 Bank display — vestigial in this build

- `banset` (7467) `"~g1:20:Bank>"`, `banclr` (7468) `"~g1:20:     "` —
  text row 20 = board pixel row 180.
- `printban` (7119) prints `banset`; **nothing calls it in this build**
  (the CD front-end draws its own overlay UI). `gm` ends by clearing
  `bank_mod` and printing `banclr` (7102–7105) — i.e. bank loads erase the
  "Bank>" area. `bank_mod` is never used elsewhere; there is no timing
  logic left. For a faithful console-style port: print `"Bank>"` + typed
  digits while `*` is held, clear on load (see §2.4).
- `prin` (7109): register-preserving `print` of the string in d7 —
  external entry for the CD front-end ("routine to print errors",
  `vlm.equ:19`).

### 2.3 Always-on overlays / VLM-mode behaviours (`vlm_mode == 1`)

- **VLM logo** (beastie 7, 424–433, `vlmlogo` 8490): switched on by
  `vlm_on` (mode 7 at `davesobj+204` = `beasties+460`) with
  `vlmtim = 500`; `Frame` decrements `vlmtim` and turns the logo off when
  it expires (4354–4358). NOTE: in this build `vlm_on` is dead code — the
  Option-button test at 4393–4395 ends in an unconditional `bra.w nothash`,
  and cdfront manages `vlm_mode` itself. Original behaviour: Option button
  → `vlm_mode=1`, board overlay on, logo on for 500 frames.
- **Strobe**: C button while `vlm_mode==1` → `flash = 256`, `scron = 1`
  (4339–4349); `Frame` fades BG colour by 16/frame (4324–4336) and
  `RunBeasties` skips the main screen object while strobing (5707–5713).
  Debounced by `flash_db`.
- **Joypad → effect interaction**: `routine` = `symadj` (859) every frame:
  d-pad drives the `pixcon`/`piycon` inertial controllers (§5.1) →
  `px`/`py` (8445–8446), the "User control X/Y" wave sources;
  A/B/C buttons drive the three ADSR envelopes via `doadsr` (906–1023,
  state machine `adsrvex`: trigwait/attack/decay/sustain/release over
  `envvals+10/+12/+14`). **Quirk: while `editing != 0`, `symadj` reads
  `pad_now+4` (player 2!) for the d-pad (868–871).**

### 2.4 Bank/effect selection (`Frame` 4404–4450), only when `editing == 0`

- While `*` (asterisk, bit16) is held: each **newly debounced** digit 1–9
  (mask test 4415, `dcode` translate, `'0'` explicitly ignored 4421–4423)
  is pushed into a 2-digit shift register: `star_on.w0 ← star_on.w2`,
  `star_on.w2 ← digit` (4427–4429), with `seldb` debounce.
- When `*` is released (`knobby` 4433): if `star_on.w0` (two digits typed)
  → `imatrix = first-1`, `skid = second`, `action = gm` (full bank load +
  effect select). If only one digit → `skid = digit`, `action = skidoo`
  (effect select within current bank). Then `star_on` cleared.
- `dcode` (6501) + `codez` (6517): maps the highest set bit of a pad mask
  to an ASCII code, scanning bit 31→0 against
  `codez[0..31] = 'x','x','A','P','x','x','B','x','R','L','D','U','1','4',
  '7','*','x','x','C','x','x','x','O','x','2','5','8','0','3','6','9','#'`.
  (Caller pre-masks to digit bits only.)
- `seldb` debounce (4364–4372): while set, all of this is skipped until a
  frame where **no** button is down on either pad.

### 2.5 pad_now bit layout (`readpad` 6556)

32-bit, active-high: `xxAPxxBx RLDU741* xxCxxxOx 2580369#`

```
pause $10000000  A $20000000  B $02000000  C $00002000  Option $00000200
right $00800000  left $00400000  down $00200000  up $00100000
7 $00080000  4 $00040000  1 $00020000  * $00010000
2 $00000080  5 $00000040  8 $00000020  0 $00000010
3 $00000008  6 $00000004  9 $00000002  # $00000001
```

`pad_shot` = newly-pressed edges. Player 2 in `pad_now+4`/`pad_shot+4`.

---

## 3. Edit mode entry / exit

### 3.1 Entry (`no_ksel` 4452–4464)

```asm
        move.l  pad_now,d0
        ; cmp.l  #(seven|asterisk|zerobutton|three),d0   ; original cheat
        cmp.l   #pausebutton,d0     ; mwenge hack: just Pause
        bne.w   nse1
        jsr     setedit
        bra.w   nse2
nse1:   clr.w   vedit
```

- **Original combo**: `pad_now == seven|asterisk|zerobutton|three`, i.e.
  keypad **7 + * + 0 + 3 held simultaneously and nothing else** (exact
  equality, so no other button may be down). The reconstruction replaces
  it with the Pause button (4456–4458). The port should support the real
  combo (and may keep Pause as a convenience).
- `setedit` (1287): if `vedit` already 1, return (edge-trigger; `vedit` is
  re-cleared every frame the combo isn't held, 4464). Else:
  `ovlm_mod ← vlm_mode`; `vedit = 1`; `vlm_mode = 3` (EDITING_MODE);
  `beasties+140 = 1` (board overlay on); `action = initedit`.
- External inhibit: cdfront can set bit15 of the word at `no_ksel+8` to
  disable editor entry (`vlm.equ:31` comment).

### 3.2 Exit

`editquit` (3907): print `normal` = `"Standard Mode~c30:"` via `cprint`;
`editing = 0`; `star_on` cleared; `vlm_mode = ovlm_mod`. Reached by:

- pressing a fire button at the root menu when the edit stack is empty
  (`onstak` 5545 → `bexit` 5226 → `action = editquit`);
- `dofass` (4934) after assigning an effect to the keypad **[ORPHAN]**.

The board keeps showing "Standard Mode" for 30 frames, then auto-clears.

### 3.3 Mode words

`vlm_mode`: 0 = audio-control (CD UI), 1 = VLM interactive, 3 = editing.
`editing`: 0 = none, else 1-based index into `editvex` (§4.2).
`vedit`: combo edge-detect latch only.

---

## 4. The editor state machine

### 4.1 Screen stack

Screens are pushed on a stack at `e_stac` via `esp`:
`estack` (3623) pushes {editlist, selected, selectable};
each screen-constructor pushes its *re-entry routine* on `esp`
(e.g. `isp1`), so "back" = `onstak` (5545): pop; if the popped value is 0
(the `e_stac` sentinel written by `initedit`) → `editquit`, else
`action = popped-routine`. `reedit`/`eunstack` (3607–3617) restore a full
{editlist, selected, selectable} triple (used when leaving waveform-attach
back to the parameter list). `ledit` always holds the constructor of the
current *menu* screen (so `bbexit` 5231 can re-enter it: "Menu" button).

### 4.2 `editvex` — per-frame input routines (4664–4686)

```
1 SELECTOR selector   2 XY1 xy1      3 X_ONE x_one   4 X_ONE_2 x_one
5 XY1_2 xy1           6 XY1_3 xy1    7 KPAD kpad     8 AWF_X awf_x
9 AWF_Y awf_y        10 SPDINC spdinc 11 ADSRED adsred 12 DLADJ dladj
13 PADEX padex       14 SPECED speced 15 SPECED2 speced2 16 SPECED3 speced3
```

### 4.3 `edvex` — per-frame control-display routines (2520–2533)

```
1 CROT crot     2 SNGLX snglx   3 SISNGLX sisnglx  4 SIDBL sidbl
5 DVECT dvect   6 RRTS rrts     7 WSSHOW wsshow    8 SHSPEC shspec
```

`symed` holds the 1-based edvex index (0 = none).

### 4.4 `selector` (5524) — the generic menu handler

Up/Down → `selup` (5580)/`seldn` (5600) with wraparound; both set
`action = ud_selcu` to move the cursor.
A or B → `seuss`/`sted` (5559–5574): `action = editlist[8 + selected*8 + 4]`
(the routine paired with the selected text), `editing = 0`, `seldb = 1`.
C → back: if `cbuttf` set (waveform-attach menu, `ogiedit`), `action =
reedit`; else `onstak` (pop screen or quit).

**The hidden-menu counter**: `seldn` wrapping past the bottom while
`sec_en == 1` increments `sec_cnt`; on the **8th** wrap → `action =
old_edit` (5606–5615). `initedit` (3522) sets `sec_en = 1`; `old_edit`
clears it.

`giedit` (3553): common screen finisher — `cbuttf = 0` (`ogiedit` variant
keeps caller's), `actime = −1`, `editlist = a1`, `print(a0)` (the heading
containing `~e`), `editing = SELECTOR`.

### 4.5 Screen catalogue (reachable set)

Entry: `initedit` (3522) → stack reset (`esp = e_stac`, sentinel 0) →
`ispec`.

#### 4.5.1 "Spectrum and Triggers" (`ispec` 3069)

- Heading `isphead1` (7478): `"@~g1:1:Spectrum and Triggers~e3:3:"`.
- Options `option7` (8308): selectable=4, bottom `bline1` (7537)
  `"~g1:20:<A> Edit   <B> Edit   <C> Back"`; entries `Trigger 1..5` (op71–75,
  7529–7533) all → `ispec2`.
- `symed = SHSPEC` (spectrum display, §4.5.9), `editing = SPECED`.
- `speced` (4792): `band = selected`, then behaves as `selector`.

#### 4.5.2 "Trigger Settings" (`ispec2` 3091)

- `isphead2` (7480) `"@~g1:1:Trigger Settings~e3:3:"`; `option8` (8319):
  selectable=1, `bline1`; `Set Width` (op81) → `ispec3`,
  `Set Trigger Minimum` (op82) → `ispec4`. `symed = SHSPEC`.

#### 4.5.3 `ispec3` (3112): prints `isphead3` (7482)
`"@~g1:1:Adjust width using joypad"` (no menu), `editing = SPECED2`.

`speced2` (4723), throttled to every 8th frame, edits
`avbank[band]` (8522: 5 records of 4 words {start, end, min-trigger,
unused}):
- Up: start−− (if >0) and end++ (max 63) — widen;
- Down: if end−start > 2: start++, end−− — narrow;
- Left: start−−, end−− (if start > 0); Right: start++, end++ (if end < 63);
- any of A/B/C (`padex` 5338) → `owwt` (5420): `editing = SELECTOR`,
  `symed = 0`, `onstak` (back to Trigger Settings).

#### 4.5.4 `ispec4` (3129): prints `isphead4` (7484)
`"@~g1:1:Adjust trigger minimum with pad"`, `editing = SPECED3`.
`speced3` (4696): Up/Down = `avbank[band].min ± $100`; A/B/C exit.

#### 4.5.5 "Edit Mode" (`old_edit` 3535) — the hidden root menu

- Heading `edithead` (7469) `"@~g1:1:Edit Mode~e3:4:"`.
- `option1` (8277): selectable=1 (`star` = $10000), bottom `bline1`;
  entries: `Edit this effect` (op11) → `thisfx`; `Spectrum and triggers`
  (op19) → `ispec`.
- The other root strings exist but are unwired **[ORPHAN]** (7509–7517):
  op12 `Assign this effect to keypad` (→`kpass`), op13
  `Change system settings` (no routine), op14 `Edit envelopes and triggers`
  (→`ahead1`), op15 `Delayline settings` (→`edline`), op16
  `Compress and store matrix` (→`compr`), op17
  `Reset save pointers in ROMulator` (→`rset`), op18
  `Test matrix retrieve` (→`mset`).

#### 4.5.6 "Choose a subeffect slot to edit" (`thisfx` 2956)

Builds `elspace` dynamically: selectable=5, bottom `bline2` (7539)
`"~g1:20:Joypad to select, any FIRE to edit"`; for each of the 6 slots in
`fx[]`: if empty → text `empt` `"<Empty>"` (7555), action `initedit`
(bounces back to Spectrum screen); else text = `vars[info-1]` type name
(§4.6), action `edit2`. Heading `subfxhea` (7494)
`"@~g1:1:Choose a subeffect slot to edit~e3:3:"`.

#### 4.5.7 "Editing: Effect" (`edit2` 3279)

On entry from thisfx: `fxed = og = selected`; `fxedbase = fx[fxed]`.
Heading `edit2hea` (7471) `"@~g1:1:Editing: Effect~e3:4:"`; `option2`
(8285): selectable=2, `bline1`; `Edit source function` (op21) → `foredit`;
`Edit symmetry generator` (op22) → `symedit`; `Edit source waves` (op23)
→ `wsedit`. (op24 `Change effect` → `iiiogo` object-giver is **[ORPHAN]**.)

#### 4.5.8 Parameter list screens

- `foredit` (3000): `_m = 0`; takes `vars[info(fx[fxed])-1]`'s param list
  (bytes after the 30-char name), `elcon`s it, heading `wavedhea` (7476)
  `"@~g1:1:Editing: Wave Plotter~e3:3:"` (yes — this heading is used for
  every object type). Sets `ledit = fored`.
- `symedit` (3256): same with `symvars` list, heading `symedhea` (7473)
  `"@~g1:1:Editing: Symmetry Generator~e3:3:"`.
- `dvfedit` (3166) **[ORPHAN]**: `dvfvars` list, heading `dvfedhea` (7475)
  `"@~g1:1:Editing: Digital Video Feedback~e3:3:"`.

Selecting an entry runs `eparam` (§5).

#### 4.5.9 `shspec` (2171) — the Spectrum & Triggers display (edvex 8)

Drawn each frame onto **`draw_screen`** (the fx screen, 384-wide CRY16)
via `sblitblockk` (6343; divides all coords/sizes by `skale`, min 1):

- 64 spectrum bars: x = 60 + i*4, width 3, height = `absdelta[i]`>>8 + 1,
  bottom anchored at y = `mony`+$10 (mony = 240 → baseline 256); colour
  $8FC0 (green) outside the selected band, $F0C0 (red) inside
  [`bandl`,`bandh`] (from `avbank[band]`, swapped if reversed).
- band envelope underline: x = 60 + bandl*4, width (bandh−bandl)*4+3,
  height 1, colour $FFFF, at y = baseline − `envvals[band]`>>9.
- trigger-minimum line: x = 60, width 256, colour $88FF, y = baseline −
  `avbank[band].min`>>9.
- a second full-width line, colour $4480, y = baseline −
  `avbank[band].w3`>>8 (the unused 4th word).
- 5 trigger lamps: 10×10 at x = $96 + i*$10, y = $78; colour $8FF0 when
  `trig` bit i set else $F0F0.

### 4.6 Parameter metadata: `pbinfo` (7722) and per-type lists (8177–8263)

`pbinfo`/`editinginfo` = 64 records × **40 bytes**:

```
+0   30 bytes  name (NUL-padded 29 chars)
+30  .b  bit0 = X of an X/Y pair
+31  .b  pbinfo index of the Y partner
+32  .b  edvex index (display/edit style); 0 = not editable (unquit);
         6 = call routine at +36 (init_sym); 7 = init_byt
+33  .b  value type: 1 = word-scaled, 2 = 16:16-scaled, 3 = special;
         bit7 = signed/centred (value stored − max/2)
+34  .w  fxobj long index (byte offset = idx*4; see ifxobj table 1740)
+36  .l  max value (or routine ptr when +32 == 6)
```

Full table (idx: name / edvex / type / fxobj-long / max; ± = type bit7):

```
 1 DVF window size: X      4 sidbl  2  $01 $1800000   (pair→2)
 2 DVF window size: Y      4        2  $02 $1800000
 3 DVF scale: X            4        2  $03 $3FFFFFF   (pair→4)
 4 DVF scale: Y            4        2  $04 $3FFFFFF
 5 DVF rotate angle        2 snglx  2± $05 $1FFFFFF
 6 DVF centre of rot: X    1 crot   2  $06 $1800000   (pair→7)
 7 DVF centre of rot: Y    1        2  $07 $1800000
 8 DVF Delta Intensity     2        2  $08 $FFFFFF
 9 Destination pos: X      1        2  $09 $1800000   (pair→10)
10 Destination pos: Y      1        2  $0A $1800000
11 DVF window centre: X    1        2  $0B $1800000   (pair→12)
12 DVF window centre: Y    1        2  $0C $1800000
13 Destination pos: Z      3 sisnglx 2 $0D $7FFFFFF
14 Vector: X               5 dvect  2± $0E $FFFFFF    (pair→16!)
15 Destination Y offset    3        2± $0F $1800000
16 Vector: Y               5        2± $10 $FFFFFF
17 Symmetry Types          6→init_sym  $11
18 Rot. Symmetry Order     2        1  $12 $100
19 Rotational Angle Step   2        2  $13 $FFFFFF
20 Rot. Angle Step Delta   3        2± $14 $FFFFF
21..26 Intensity 1..6      2        2  $15..$1A $FFFF
27 Z amplitude             2        1  $1B $FFFF
28 Fixed point phase 1     3        2± $1C $1FFFFFF
29 Delta phase 1           3        2± $1D $1FFFFF
30 Rot. sym overall phase  3        2± $1E $1FFFFFF
31 Fixed point phase 2     3        2± $1F $1FFFFFF
32 Number of iterations    2        2  $20 $1000000
33 X amplitude             2        1  $21 $FFFF
34 Y amplitude             2        1  $22 $FFFF
35 Number of other iters   2        2  $23 $1000000
37 delta Z                 2        2± $25 $FFFFFF
38 choice of Thang         3        2  $26 $FFFFFF
42 Rot. Sym centre: X      1        2  $2A $1800000   (pair→43)
43 Rot. Sym centre: Y      0(unquit) 2 $2B $1800000
44 Rot. Symmetry scale     3        1± $2C $3FFF
45 Rot. scale delta: X     4        1± $2D $3FF       (pair→48)
46 Colour gen vector: X    5        2± $2E $7FFFF     (pair→47)
47 Colour gen vector: Y    5        2± $2F $7FFFF
48 Rot. scale delta: Y     5        1± $30 $1000
49 Rot. centre delta: X    5        2± $31 $FFFFF     (pair→50)
50 Rot. centre delta: Y    4        2± $32 $FFFFF
51 Delta phase 2           3        2± $33 $1FFFFF
53 Radius                  3        2± $35 $FFFFFFF
54 Base col gen vec: X     5        2± $36 $1FFFFF    (pair→55)
55 Base col gen vec: Y     5        2± $37 $1FFFFF
56 Base colour: X          5        2± $38 $1FFFFF    (pair→57)
57 Base colour: Y          5        2± $39 $1FFFFF
58 Destination plot routine 6→init_sym 3 $3A
59 Maximum pixel size      2        2  $3B $7FFFFF
61 Trigger mask            7 init_byt 3 $3D
(0,36,39,40,41,52,60,62,63 = "Parameter not yet defined", not editable)
```

Per-object param lists (`vars` 8179; FF-terminated index bytes after the
name — the name is also the text shown in `thisfx`):

```
polyvars  'Draw a polygon object'      : 09 21 22 1C 15 16 17 18 19 1A 26 3D
sfvars    'Draw 3D starfield'          : 09 0D 20 3B 38 39
obvars    'Draw particle object'       : 09 0D 3B 0E 21 22 38 39 2E 2F 1C 1D 1F 15 16 1B 25 26
pmvars    'Do particle motion'         : 16 20 23 09 0D 0E 25 17 18 19 38 15 2E 33
ringvars  'Draw a ring of pixels'      : 09 1C 1F 20 35 36 38 3B 0E 15 16
dvf_vars  'Digital Video Feedback area': 01 03 05 06 08 0B 38
wsu_vars  'Wave Surface Thang'         : 09 0D 1C 1D 33 20 23 22 0E 3B 36 38
monomapv  'Draw mono bitmap coloured'  : 09 2E 36 38 15 26 3D
monomapv2 'Draw mono bitmap i-shaded'  : 09 2E 36 38 15 26 3D
plas1var  'Colour plasma area type 1'  : 09 2E 36 38 21 22 3D
logovars  'Big Jaguar hardware sprite' : 09 38 15
shuuvars  'Spectrum as intensities'    : 09 38 36 3B 22
symvars   (symmetry editor)            : 11 1E 12 13 14 2A 2B 2C 2D 30 2E 2F 31 32 06
dvfvars   (orphan dvfedit)             : 01 03 05 06 08 0B 38
```

`vars[]` maps `info` 1..14 → polyvars, sfvars, ringvars, dvf_vars,
wsu_vars, monomapv, monomapv2, dvf_vars, dvf_vars, plas1var, logovars,
obvars, pmvars, shuuvars (8179–8192).

---

## 5. Editing a parameter (`eparam` 3635 → `crunt` 3723)

`eparam`: `eddie = elvex[selected]` (the +30 var block). Dispatch on the
edvex byte (+32): 7 → `init_byt` (§5.6); 6 → jsr the routine at +36
(`init_sym`, §5.5); else `crunt`:

- `symed = edvex-byte`; `editing = edvex-byte + 1` (so crot→XY1,
  snglx→X_ONE, sisnglx→X_ONE_2, sidbl→XY1_2, dvect→XY1_3);
- `primexy` (3790): `lselecte = selected`; `monitor = param-idx+1`;
  `_m = 1`; seed `ixcon` from the current value via `s16`, and if the
  entry has an X/Y pair, seed `iycon` from the partner entry (3801–3823);
- draws: `cprint(eparm0)` `"Edit: "` (7550), then the parameter name, then
  `eparm2` (7553) `"~g1:18:Press ~i+*~i- to attach waveforms"`, then
  `ppmes` (3746) → `eparm1` (7551)
  `"~g1:20:<A> Prev   <B> Menu   <C> Next"`.

### 5.1 The inertial controllers `ixcon`/`iycon` (8492) and `inertcon` (6741)

9-long struct: `+0` value (24.8, usable range 0..$FFFF00), `+4` velocity,
`+8` acceleration = $400 (`UPDA2`, blitter.inc:38), `+$C` friction = $20000,
`+$10` upper limit $FFFF00, `+$14` lower limit 0, `+$18` max velocity
$20000. (`pixcon`/`piycon` 8432 use accel $1600, friction $3000, maxvel
$40000; `pinertco` 6729 is the variant used by `symadj`.)

Input byte: `pad_now` byte 1 (`RLDU741*`), `rol.b #2` → bit0=Left,
bit1=Right (X axis); `rol.b #4` → bit0=Up, bit1=Down (Y axis). Per tick:

- exactly one direction bit set: if |vel| < maxvel, vel ±= accel;
  value += vel; clamp to [lo,hi] (vel cleared on hit; no clamp if lo==hi).
  Bit0 (Left/Up) path decrements, bit1 (Right/Down) increments.
- no/both bits: friction — |vel| < friction → vel = 0, else vel −=
  sign(vel)·friction; value += vel.

So parameter sliders have Minter's trademark accelerate/coast feel.
Note Up = bit0 = **decrease** for the Y controller (display routines
compensate).

`editvex` input routines: `x_one` (5156) = X axis only; `xy1` (5167) =
both axes; both fall into `spn_butt` (5181): `*` pressed →
`action = wfa` (waveform attach, §5.7), `editing = 0`; else `pn_butte`
(5192): B → `bbexit` (back to the menu via `ledit`); A → `prevexit`
(selected−1); C → selected+1, wrapping — A/C move to the **previous/next
parameter** of the list and re-enter it via `sted`.

### 5.2 Value write-back and the GPU `_fmode` services (psi.gas)

The **display** routines (`avx` 2815 / `avxy` 2831) run in `yakedit` each
frame and push the slider value into the fxobj through GPU service 2:

`_ein_buf[0]` = var-block ptr (X: `eddie`; Y: partner block),
`_ein_buf[4]` = `iixcon>>8` (0..$FFFF), `_ein_buf[8]` = fxobj **or
fxobj+512 when the param is waveform-linked** (`lnkchk` 2875: link word at
`fxobj+256+idx*4` non-zero → edit the base copy at +512), `_fmode = 2`,
spin `fmodewai` (2476).

psi.gas service table (dispatch psi.gas:137–174; requests are serviced
inside the per-frame `_fsync` window):

| `_fmode` | psi.gas | Semantics |
|---|---|---|
| 2 | routine18e (222) | **Write param**: reads type(+33), idx(+34), max(+36) from the var block; `value = (slider × max) >> 16` — full 32×16 fixed multiply for type 2, 16-bit `mult` with `max & $FFFF` for type 1; if type bit7: `value −= max>>1`; stores long at target+idx*4. |
| 3 | routine22e (304) | Line draw: `_ein_buf[0]`=screen, `[4]`→{X_1,Y_1,X_2,Y_2,LINCOL} words; blitter line. (Only referenced from unreachable code after `dvect`, 2572–2610.) |
| 1 | routine512 (608) | ADSR curve plot (§5.9). |
| 5 | routine4ac (565) | Waveform monitor plot (§6). |
| 6 | routine3e4 (488) | Scope, single trace, horizontal deflection (§6). |
| 7 | routine3ae (464) | Scope, single trace, vertical deflection. |
| 8 | routine41a (512) | Scope, XY (Lissajous) from two buffers. |
| 9 | (no case!) | set by `shenv` (2286) — dead code; envelope/trigger update runs unconditionally every frame in psi (routine65a, psi.gas:732). |

The reverse mapping (param → slider) is `s16` (3835) via `vtypes` (3865),
indexed **directly** by the type byte: type 1 → `xword`: slider =
value·65536 / (max & $FFFF), stored <<8 into ixcon; type 2 → `x1616`:
ixcon = (value / (max>>12)) << 12, masked $FFFFFF; type 0 → `xbyte`:
no-op. (Type-3 entries never reach s16 — eparam branches to the custom
routines first.)

For the C port: replace all of this with
`param_set(fx, entry, slider01)` / `slider01 = param_get(...)` using the
formulas above — keep the exact integer truncations for fidelity.

### 5.3 Control displays (`edvex`), exact geometry

All drawn on the 384-wide CRY16 `draw_screen` via `sblitblockk` (6343 —
coords and sizes are >> `skale`, minimum size 1) except where noted.

- **snglx** (2733), horizontal fill-bar: background (x $3E, y $94, w $102,
  h $0E, colour $80FF); bar (x $40, y $96, w v+1 where v = slider>>8 ∈
  0..255, h $0A, colour $FF00|v).
- **sisnglx** (2689), centred bar: same background; d = (slider>>8)−$7F;
  bar at y $96, h $0A, colour $FF00|d; d ≥ 0 → x $C0 w d+1, else
  x $C0+d w −d+1.
- **sidbl** (2638), centred X+Y bars: horizontal background (x $3E, y $BD,
  w $102, h 7) and vertical background (x $BD, y $3E, w 7, h $102), both
  $80FF; X bar: y $BF, h 3, white ($FFFF), centred on x $C0 using
  (sliderX>>8)−$7F; Y bar mirrored vertically with sliderY.
- **crot** (2917), crosshair: uses the **actual param words**
  (`fxobj[idx*4].w` = integer part, 0..$180): 1-px vertical line at
  x = valX (h $180) and 1-px horizontal at y = valY (w $180), colour
  $88FF, via `blitblock` (unscaled).
- **dvect** (2555), vector dot: 10×10, colour $80FF, at
  x = (sliderX>>8)−$7F+$BB, y = (sliderY>>8)−$7F+$BB.
- **wsshow** §5.8, **wfm** §6, **shspec** §4.5.9, **rrts** nothing.

### 5.4 Symmetry planes editor (`init_sym` 3703) — pbinfo 17 & 58

Reached from "Symmetry Types" (and, as shipped, "Destination plot
routine" shares the same routine pointer, so it edits `asym_fla` too).

- `isymbut` (3754): builds highlight state of `symbutts` from
  `asym_fla(a6)` bits 0–7 (+ bit8 for rotational) using `padbits` (7694)
  = bit→button-slot map {3,$B,$A,6,9,7,2,1}; `cpad = sympad`,
  `cpad+4 = symbutts`. `symed = 6` (RRTS), `editing = 7` (KPAD).
- Draws `symplane` (7556):
  `"Editing: Symmetry Planes and Types//Press number keys to turn off or
  on~g9:8:----- Rotational symmetry~g12:14:- Clear~g12:16:- Invert"`
  via `cprint`, then `ppmes`, then `ud_butts`.
- `ud_butts` (6259) draws the 12 keypad glyphs from `padchars` (7687)
  {'*','7','4','1','#','9','6','3','0','8','5','2'} at the (col,row) from
  the button table at `cpad+4`; 4-byte entries {x, y, flags (bit7 =
  highlighted → inverse video), flash-timer}; x = $FF hides the button.
  `symbutts` (8410) coords (col,row): *(8,14) 7(5,9) 4(11,9) 1(7,5)
  #(8,16) 9(8,8) 6(7,11) 3(11,7) 0=hidden 8(5,7) 5(9,11) 2(9,5):

  ```
       1  2
   8         3
         9        ----- Rotational symmetry
   7         4
      6   5
     * - Clear
     # - Invert
  ```

- `kpad` (1028) = `gkpad` + `pn_butte`. `gkpad`/`gkp` (1035–1105): when
  editing, `flashem` runs over the 12 button entries: non-zero flash
  timers decrement, toggling the highlight bit every 4 ticks and posting
  `action = ud_butts` — pressed keys blink. Then keys are scanned:
  pad word 0 bits 0–3 = `*`,1,4,7; pad word 1 bits 0–7 = `#`,9,6,3,0,8,5,2
  (that 12-entry order is the canonical `cpad` table order). A newly
  pressed key (its flash timer must be 0) runs the matching `cpad` jump
  entry via `execute` (1221), which sets the flash timer ($1C, or $20 when
  the entry's flags bit0 is set) and swaps `routine` to `keydb` (1251)
  until all number keys are released.
- `sympad` (7331), in that key order: `symclr`(*)(asym_fla=1),
  `sp8`(1), `sp7`(4), `sp1`(7), `syminv`(#)(`eor.l #$100FF`), `sp9`(9)
  (toggle rotational, bit0 of byte $A1), `sp4`(6), `sp6`(3), `rrts`(0),
  `sp5`(8), `sp3`(5), `sp2`(2). `spN` (1312–1346) toggles bit N−1 of
  byte $A3 (= asym_fla bits 0–7). Net key→bit: 1→b7, 2→b1, 3→b5, 4→b6,
  5→b2, 6→b3, 7→b0, 8→b4, 9→rot. After clear/invert, `setstat` (1306)
  re-runs `isymbut` and posts `action = ud_butts`.
- `sympad` entries 12–15 (`srcentre` 1209, `dorsym1..3` 1151–1168 with
  `gjoy` 1174 d-pad tables `syminvxdec`/`sympadrsmyd`/`sympaddstep`
  7379–7389 adjusting roscal2/roscalei, rsym_ord/roscale,
  rsym_ste/rsym_ist, and display styles CROT/SNGLX/SISNGLX/SIDBL) are
  **unreachable** from `gkp` (only 12 keys scanned); `someeother` (1107)
  is dead too. Dev leftovers.
- Exit: `pn_butte` — A/C = prev/next parameter, B = menu.

### 5.5 Trigger-mask editor (`init_byt` 3658) — pbinfo 61

- Builds `awfb2` (8403) button highlights from `_mtrig` byte 3 using
  `pad8bits` (7699) {3,$B,7,2,$A,6,1,(9)} → bits 0–7 light glyphs
  '1'..'8'. `cpad = dorsymrts` (7340: rrts(*), bp7(1), bp4(4), bp1(7),
  rrts(#), rrts(9), bp6(6), bp3(3), rrts(0), bp8(8), bp5(5), bp2(2));
  `bpN` (1348–1378) toggles bit N−1 of `_mtrig+3`. Net key→bit: 7→b0,
  2→b1, 3→b2, 4→b3, 5→b4, 6→b5, 1→b6, 8→b7. `symed = RRTS`,
  `editing = KPAD`.
- Text `bytemask` (7559):
  `"Byte bitmap.  Press buttons to//change a bit."` via `cprint`, then
  `ud_butts`.

### 5.6 Waveform attach (`wfa` 3500 / `rwfa` 3511 / `iawfx` 4800 /
`iawfy` 4998)

Triggered by `*` during parameter editing (`spn_butt` 5181). `wfa`:
`estack`; if the param is an X/Y pair (eddie +30 bit0) → `rwfa`: heading
`wfahead` (7493) `"@~g1:1:Attach and Adjust Waveforms~e3:3:"`, options
`option3` (8291) {`Waveform attach (x)` op31 → `iawfx`,
`Waveform attach (y)` op32 → `iawfy`}, `cbuttf = 1` (C = `reedit`, back
to the param list). Single params go straight to `iawfx`.

`iwf` (4969) common setup: `cprint(awhead)` (7496)
`"Attach and Adjust Waveforms//Press keys 1 to 8 to link waveforms//Use
the joypad to change amplitude~g1:20:Press any FIRE button to exit"`;
`cwave1 = monitor` (param idx+1), `cwed1 = fxobj+idx*4`; `cwave2/cwed2`
from the partner entry; `cpad = gskirts-11*4` table, `cpad+4 = awfbutts`.

`iawfx`: `editing = AWF_X`; if no link yet (word at `fxobj+256+idx*4` ==
0), create one via `wavelink`/`wlink` (838–851) with amplitude $8000 and
no waves. `istat` (4814): set highlight bits on `awfbutts` (8398, attach
bits = link byte 1) and `awfb2` (secondary bits = link byte 0) via
`pad8bits`; seed `ixcon = amplitude << 8`; `dud_butt` (6241) draws both
button sets. `iawfy` (4998) same for the Y partner.

**Link data layout** (`wlink` 844) for param long-index i:
- `fxobj+512+i*4` ← copy of the current param value (the modulation
  base — this is why `lnkchk` redirects slider writes there);
- `fxobj+256+i*4` = link long: byte0 = secondary wave bits 1–8, byte1 =
  attach wave bits 1–8, word +2 = amplitude 0..$FFFF. Link "active" iff
  word 0 (bytes 0–1) ≠ 0. (omega's BigLoop64 computes
  live = base + Σ wave_n × amp.)

`awf_x`/`awf_y` (5022/5012, ISR): d-pad L/R → `inertcon` on ixcon;
amplitude word = `ixcon>>8` (`wset` 5031); keypad 1–8 (jump table at
7372–7377: lulN in the canonical key order, keys 1..8 → waves — with the
same row-inverted mapping as §5.4) toggles attach bit N−1 (`lulN`
5056–5102 → link byte1); pressing `#`-slot key (`setantelope` 5052,
antelope=1) makes the next key toggle the **secondary** bit instead
(`antN` 5107–5151 → link byte0, then `clant` antelope=2 which `execute`
uses to clear flags and re-debounce). Any fire button: pair →
`action = rwfa`, else `action = reedit` (5040–5047 via `soxx`).

### 5.7 Source-wave editor (`wsedit` 3145)

- `edwave = fxedbase+$300` — the 8×16-byte oscillator bank at fxobj bytes
  768–895: {+0 phase accumulator .l, +4 rate .l, +8 table ptr .l,
  +$0E type .w}.
- `symed = WSSHOW`; heading `wshead` (7499)
  `"@~g1:1:Edit basic waveforms~e3:3:"`; options `option4` (8296):
  selectable=7, bottom `bline3` (7503)
  `"~g1:20:<A> ADD    <B> EXIT   <C> SUB"`, entries `1:`..`8:` (op41–48,
  actions `rrts`); `action = wtud`; `editing = SPDINC`.
- `wtud` (3566): prints `wt` (7664) `"~g2:12:"` then the wave-type name
  from `wts` (7666): `Sine wave`, `Sawtooth wave`, `Square wave`, `Ramp`,
  `Rectified sine wave`, `Noise`, `Constant`, `User control Y`,
  `User control X` (19-char padded, 7654–7662); reloads the table ptr
  from `sineslookup` (7677: p_sines, p_saw, p_square, p_ramp, p_rect,
  rantab, p_sines, p_sines, p_sines) unless type == 6 (Constant).
- `spdinc` (5361, ISR): Up/Down = `xselup`/`xseldn` (select wave,
  reposting `wtud`); Left/Right = `ph_dec`/`ph_inc` (phase ∓$10000,
  5497–5506); keypad via `wfpad` (8327, canonical key order):
  key 7→Sine(0), 2→Saw(1), 3→Square(2), 4→Ramp(3), 5→Rect sine(4),
  6→Noise(5), 1→Constant(6), 8→User Y(7), 9→User X(8)
  (`swfN` 5445–5494); fire buttons (5406–5439): B → `owwt` exit,
  C → rate −$1000 (`zinc`), A → rate +$1000 (`stinc`), rate masked to
  $1FFFFF.
- Display `wsshow` (2783): for wave n at (x $50, y $71 + n*9): speed bar
  h 3 colour $58FF, width = high word of `rol32(rate,3)` + 1; 1-px phase
  bar below, colour $80FF, width = (phase & $FF) + 1; then `wfm` (§6)
  plots the selected wave in the monitor box.

### 5.8 ADSR editors **[ORPHAN in shipped menus]**

- `ahead1` (3300): heading `adedhead` (7490)
  `"@~g1:1:Edit ADSR channel settings~e3:3:"`, `option5` (8300):
  `Edit ADSR a/b/c` (op51–53) → `ahead2`; `aded = 0`.
- `ahead2` (3036): `aded = adsra + selected*10` (structs at 8439: words
  {attack rate, decay rate, sustain level, release rate, state});
  heading `adedhead2` (7491) `"@~g1:1:Edit ADSR envelope shape~e3:3:"`,
  `option6` (8302): selectable=3, bottom `bline4` (7541)
  `"~g1:20:Up,Down to choose, L,R to change"`, entries `A:` `D:` `S:`
  `R:` (op61–64); `editing = ADSRED`.
- `adsred` (5315): Up/Down select row; Left/Right ∓$80 on the selected
  word; fire = `padex` exit.
- Display `shad` (2322, runs whenever `aded != 0`): `_fmode 1` draws the
  envelope curve — psi routine512 (psi.gas:608) simulates attack
  (level += rate>>2 per step to $FFFF, colour $00FF), decay (−rate>>2 to
  sustain, $F0FF), 16×4 sustain steps ($8FFF), release (to 0, $80FF),
  plotting 2×2 dots at x = 64 + step/2, y = 224 − level>>10; then the 68k
  draws 4 value bars at x $4C, y $6E + row*9, w value>>8+1, h 7, colours
  `adsrcols` (2349) {$00FF, $F0FF, $8FFF, $80FF}.

### 5.9 Delay-line editor (`edline` 3429) **[ORPHAN]**

- `editlist = dlset` (8500): selectable=6, selected = current `delayf`
  (stored into `dlset+2`, −1 → 0), bottom `bline5` (7543)
  `"~g1:18:Hold down b and use up,down to//change channel"`, entries
  `Off`, `channel 1`..`channel 6` (dlo1–7, 7580–7586).
- Heading `edlhead` (7545): `"@~g1:1:Delay line settings//U,D changes
  number L,R changes spacing~e3:7:"`; `editing = DLADJ`.
- `dladj` (5249): Up/Down = `delayn` ±1 (&$3F), Left/Right = `delayt` ±1
  (&$3F), each posting `action = udedg`; B+Up/Down changes channel
  (`delayf = selected−1`, 0 = off); other fire = `padex` exit.
- `udedg` (3449): rebuilds the gauge `edlgauge` (8455) `"~g3:5:"` + 32
  cells, all char 1 (solid block); then for `delayn`+1 taps stepping
  `delayt` half-cells, `setblok` (3472) mutates each cell char via the
  `evens` lookup (3492; effective bytes {2,2,4,4,3,4,3,4} — the two
  `btst d1,d4` opcodes supply the trailing $03,$04 pairs), giving a
  64-slot tap display in 32 characters; prints at (3,5).

### 5.10 Object creation (`iiiogo` 3185 / `setp1` 3224 / `setp2` 3241)
**[ORPHAN]**

- `iiiogo`: `og = selected`; heading `fxphead` (7501)
  `"@~g1:1:Choose fx page~e3:3:"`; options `fxopt` (7590):
  {`FX page 1` → setp1, `FX page 2` → setp2}.
- setp1/setp2 → heading `ogohead` (7488)
  `"@~g1:1:Object Giver Outer~e3:3:"` with `availobj` (7601) /
  `avail2` (7628).
- `availobj`: selectable = $10 → **17 of the 18 entries shown** (the
  last, `Mono particle object`, is unreachable): `Nothing`/makeno,
  `Poly object`/makepoly, `DVF area plus clear`/makecfb,
  `DVF area no clear`/makefb, `Ring of pixels`/makering,
  `3D starfield`/makestar, `WaveSurf`/amkesurf,
  `Psychedelic Bitmap`/makemono, `I-Ripple Bitmap`/makeiri,
  `DVF --> scale2`/mdvf2, `DVF --> scale4`/mdvf4,
  `Colour plasma 1`/mplaz1, `Colour plasma 2`/mplaz2,
  `Jaguar Logo`/mjaglogo, `Particle Object`/mpo, `Particle Motion`/ipm,
  `Matrix`/itau, (`Mono particle object`/mopo).
- `avail2`: {`Matrix`/itau, `Spectrum as intensities`/…} — **the second
  action pointer is truncated in the reconstruction** (`dc.w $0019`,
  7632; should be `makeshun`).
- The makers (477–832) `gadd` (743) a fresh fxobj into slot `og` (reuse
  the slot's storage, or `fx1 + og*1024` if empty), `ifxobj`-init it,
  set `info`/`gpu` (+ extras, e.g. `initwsur` 778, `mdvf2` 560), then
  `ggg` (484) → `initedit` — after creating an object you land back at
  the editor root.

### 5.11 Keypad-assign & matrix stash **[ORPHAN]**

- `kpass` (4861): prints `kpasshea` (7486) `"@~g1:1:Assign effect to
  keypad//Press the number key 1-9 to which//you want this effect
  attached"`; `editing = KPAD`, `cpad = bpsymrts` (7348: in key order
  rrts(*), ass7? — entries ass7,ass4,ass1,rrts,ass9,ass6,ass3,rrts,ass8,
  ass5,ass2 follow the sentinel), buttons `kpassbut` (8407). `assN`
  (4870–4913): `fass = N`, `action = dofass` (4918): copies the live fx
  set (with current pixcon..delayn block) over effect N of `matrix`,
  then `editquit`.
- `compr` (3314): prints `stashhea` (7547) `"@~g1:1:Compressing matrix to
  ROM//Press any fire to exit"`, runs `dostash` (3359),
  `editing = PADEX`. `dostash` delta-compresses refblock→matrix (base +
  8 blocks of $1800) to the ROM-emulator window at `datp` ($900200) via
  `deltablo` (6933), printing per block `"DELTABLOCK generated "` +
  decimal (`xxnum` 6867 into the bytes below `fxedbase`) + `" bytes/"`
  (tp1/tp2 7635–7636), finally `"55296 bytes ---> "` + total (tp3 7637).
  **Port: stub the ROM writes, keep the text.**
- `rset` (3329): prints `rsethead` (7548) `"@~g1:1:Reset ROM save
  pointers//Press any fire to exit"`, resets `datn/datp/dattp`.
  `mset` (3346): re-runs `getmatrix`. Both `editing = PADEX`.
- `ski1..9`/`selett`/`gski1..9` (4500–4589) and the `kselbutt` table
  (8324) are further unreferenced keypad-select leftovers.

---

## 6. The monitor / scope (`domonito` 2387)

**Status: dead code in this build.** `domonito` has no caller anywhere in
the tree; `mon1`, `mon2`, `monptr` and `monbuf` are never written. It was
clearly meant to run per frame while a parameter is being edited (it keys
off `_m`/`monitor`, armed by `primexy` 3794–3796, cleared by the list
screens). The GPU services it depends on *are* present (psi fmodes
6/7/8). Behaviour if wired up:

- Monitor box at `monx = $104` (260), `mony = $F0` (240), `monitand = 3`
  (460–463).
- `_m < 0` → `wfm`; `_m > 0`: clear a 64×64 box at (monx, mony−64),
  fill colour $00C0 (2397–2404).
- If `mon1 ≥ 0`: two 1-px vertical range markers at x = monx+32 ±
  ((link byte2 of param mon1)>>3 & $1F), colour $80C0; `mon2` the same
  horizontally (2405–2445).
- Scope traces from `monbuf` (64-byte ring, read ptr `monptr` & $3F):
  fmode 6 = 64 2×2 dots at x = xbase + monbuf[i], y = ybase − i
  (vertical sweep, horizontal deflection); fmode 7 = second buffer
  (monbuf+$40), x = xbase + i, y = ybase − monbuf2[i]; fmode 8 = XY:
  (xbase + monbuf[i], ybase − monbuf2[i]). Colours passed in the
  `_monptr` long's high word: $8800 single/XY, $FF00 + $8800 for the dual
  pair (2447–2503). Clipped 0..$17F, plotted by psi's shared 2×2 plotter
  (routine626, psi.gas:697).
- Intended feed: `results` (in_buf+$C; 64 per-parameter values written by
  omega's BigLoop64 each frame) — `results[monitor-1]` appended to the
  monbuf ring once per frame. The appending code did not survive into
  this binary.

**`wfm` (2135) is alive** (called from `wsshow` 2809): clears the same
64×64 box, then for wave types < 7 issues fmode 5 (psi.gas:565): 64 2×2
dots, colour $88FF, x = monx + i, y = mony − table[(phase>>16) & $FF]>>2,
phase advancing by the wave's rate per dot — a live plot of the selected
oscillator waveform (skale-scaled). Types 7/8 (User controls) instead draw
a marker (`joyxy` 2361): type 8 → 1×64 vertical bar at
x = monx + (pixcon>>2 & $3F); type 7 → 64×1 horizontal bar at
y = mony + $3F − (piycon>>2 & $3F); colour $88FF.

**Port recommendation**: implement the box + fmode-5 waveform plot (needed
by the wave editor); optionally revive `domonito` fed by
`results[monitor-1]` into a 64-entry ring — matches the intended editor
behaviour and costs little.

---

## 7. Recommended C port structure

### 7.1 State

```c
typedef enum {         // == the 'editing' word
  ED_OFF=0, ED_SELECTOR, ED_XY1, ED_X_ONE, ED_X_ONE_2, ED_XY1_2, ED_XY1_3,
  ED_KPAD, ED_AWF_X, ED_AWF_Y, ED_SPDINC, ED_ADSRED, ED_DLADJ, ED_PADEX,
  ED_SPECED, ED_SPECED2, ED_SPECED3
} EditInput;
typedef enum {         // == 'symed'
  SY_NONE=0, SY_CROT, SY_SNGLX, SY_SISNGLX, SY_SIDBL, SY_DVECT, SY_RRTS,
  SY_WSSHOW, SY_SHSPEC
} EditDisplay;

typedef struct {
  uint8_t  board[240][40];         // 1bpp text overlay, 320x240
  int      cursx, cursy, inverse, actime;
  const OptionList *editlist;      // mirror the asm tables verbatim
  int selected, selectable, selx, sely, cursorx, cursory;
  void (*action)(void);            // one-shot, run in the frame tick
  void (*screen_stack[32])(void);  int esp;   // e_stac
  void (*ledit)(void);
  EditInput editing; EditDisplay symed;
  Inert ixcon, iycon;              // + the global pixcon/piycon
  int fxed, og, band, sec_cnt, sec_en, seldb, vedit, ovlm_mod;
  const PbInfo *eddie; const PbInfo *elvex[64];
  int monitor, _m;                 // + monx=260, mony=240
  // waveform attach
  int cwave1, cwave2; uint32_t *cwed1, *cwed2; int antelope, cbuttf;
  // keypad tables
  const KpadEntry *cpad_jump; KpadButton *cpad_butt; // 12 each
} Editor;
```

Data to transcribe verbatim: `pbinfo` (§4.6), the per-type var lists,
every option table (§4.5), `sympad`/`dorsymrts`/`gskirts`-area/`wfpad`
key tables, `padchars`/`padbits`/`pad8bits`, `symbutts`/`awfbutts`/
`awfb2`/`kpassbut`, `wts`/`sineslookup`, `adsrcols`, `evens`, all strings
(keep the `~` control codes and reimplement `print`), `codez`, `myFont`.

### 7.2 Per-frame tick (replaces the Frame-ISR piece + editloop piece)

```c
void editor_frame(uint32_t pad_now, uint32_t pad_shot) {
  // 1. entry combo (7+*+0+3 exact, or Pause) -> setedit()
  // 2. debounce: if (seldb) { if (any button down) return; seldb = 0; }
  // 3. if (vlm_mode && editing && !action) editvex[editing-1]();
  //    else normal-mode *-digit bank select, C strobe, etc.
  // 4. run 'routine' (symadj) unless action pending
}
void editor_draw(void) {        // once per rendered frame, after fx draw
  if (actime >= 0 && --actime < 0) board_clear();
  if (action) { void (*a)(void) = action; action = NULL; a(); }
  ix_snapshot();                 // iixcon/iiycon
  if (symed) edvex[symed-1]();   // widgets + param write-back
  shad_draw();                   // ADSR overlay if armed
  composite_board_over_screen(); // 1bpp white overlay at (24,60)
}
```

The `_fmode` services become direct calls: `svc_param_write(entry,
slider, target)` (fmode 2 — exact integer formulas of §5.2),
`svc_adsr_plot`, `svc_wave_plot`, `svc_scope_{h,v,xy}`, all drawing into
the CRY16 framebuffer with the 2×2-dot plotter and 0..$17F clipping.

### 7.3 Minimal-first implementation order

1. **print pipeline**: board buffer, font (mind §1.3's off-by-8), `print`
   with all control codes, `cleol`, `cprint`, overlay compositing.
   Test: boot banner, `"Standard Mode~c30:"`.
2. **menu core**: OptionList structs, `~e` rendering, `selector`,
   selup/seldn/cursor, action + stack (`estack`/`onstak`/`reedit`),
   `giedit`. Test: entry combo → Spectrum and Triggers, 8-wrap →
   Edit Mode, C backing out to `editquit`.
3. **spectrum screens**: `shspec` display + `speced2/3` — self-contained,
   immediately verifiable against live audio.
4. **param editing**: pbinfo tables, `elcon` (incl. composite "(X,Y)"
   text), `eparam`/`crunt`/`primexy`/`s16`, `inertcon`, the five widget
   displays, param write-back. Test on Ring-of-pixels Radius etc.
5. **symmetry + trigger-mask keypad screens**: `ud_butts` machinery,
   flash timers, `sympad`/`dorsymrts` tables.
6. **wave editor** (`wsedit` + `wsshow` + `wfm`/fmode 5) and
   **waveform attach** (link longs, `awf_x/y`).
7. Optional/orphans: ADSR editor (+ fmode-1 curve), delay-line editor,
   object giver, keypad assign, `dostash` (stubbed persistence), revived
   `domonito` scope, full 9-entry root menu.

### 7.4 Fidelity traps checklist

- Character rows are **9 px** tall; glyph cells fully overwrite (space
  erases). Cursor glyph is char 0 (solid triangle), erased with `' '`.
- `~gX:Y:` coords are **decimal**; `banset` really is row 20, not $20.
- `'^'` homes the cursor to (0,0); `'@'` preserves it.
- `selectable` = count−1 everywhere; `availobj`'s 18th entry is cut off;
  `avail2`'s second action is a broken pointer in the reconstruction
  (fix to `makeshun`).
- Up on the d-pad *decreases* the Y inertial controller.
- While editing, `symadj` reads the **player-2** pad for px/py.
- Editing a waveform-linked parameter writes `fxobj+512+idx*4`, not the
  live value.
- `elcon` starts its counter at −1 via a `dbf` side effect; 16-entry cap.
- Keypad→function maps are row-inverted vs the printed glyphs on several
  screens (e.g. key 7 toggles plane "1"; key 7 selects "1: Sine wave") —
  copy the tables, don't "fix" them.
- Bank select digits: '0' ignored; `*` must be held; two digits = bank
  then effect; applied on `*` release; `seldb` blocks everything until
  all buttons are up.
- The edit-entry combo test is an exact-equality compare against the
  whole `pad_now` long — any extra button defeats it.
