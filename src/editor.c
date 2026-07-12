/*
 * The hidden effects editor. Port of the vlm.s editor layer per
 * docs/analysis-editor.md (line references are vlm.s unless noted).
 *
 * Implemented: print pipeline with the full control-code language,
 * menu core (selector, screen stack, hidden-menu 8-wrap), the
 * Spectrum and Triggers screens with the live shspec display and
 * avbank editing, the parameter-list screens built from pbinfo via
 * elcon (with composite "(X,Y)" labels), and parameter editing with
 * the ixcon/iycon inertial sliders, the five widget displays and the
 * exact fmode-2 write-back (base-slot redirect for linked params).
 * Not yet wired: waveform attach, wave editor, symmetry/trigger-mask
 * keypad screens (entries fall back to `unquit`), and the orphaned
 * dev screens.
 */
#include "editor.h"
#include "board.h"
#include "platform.h"
#include "pbinfo_data.h"
#include "fx.h"

#include <stdio.h>
#include <string.h>

/* ---- state ----------------------------------------------------------- */

typedef struct EdOption {
    const char *text;
    void (*act)(void);
} EdOption;

typedef struct EdList {
    int selectable;      /* count - 1 */
    int selected;
    const char *bottom;
    const EdOption *opt;
} EdList;

typedef struct {
    int32_t val, vel;    /* ixcon/iycon (8492): accel $400, fric $20000,
                            cap $20000, range 0..$FFFF00 */
} Inert;

enum {
    ED_OFF = 0, ED_SELECTOR, ED_XY1, ED_X_ONE, ED_X_ONE_2, ED_XY1_2,
    ED_XY1_3, ED_SPECED, ED_SPECED2, ED_SPECED3,
};
enum {
    SY_NONE = 0, SY_CROT, SY_SNGLX, SY_SISNGLX, SY_SIDBL, SY_DVECT,
    SY_RRTS, SY_WSSHOW, SY_SHSPEC,
};

static struct {
    VlmEngine *e;
    uint16_t *screen;
    uint32_t buttons, prev, shot;

    int cursx, cursy, inverse, actime;
    EdList list;         /* live copy (selected mutates) */
    int selx, sely, cursorx, cursory;
    void (*action)(void);
    void (*stack[32])(void);
    int esp;
    void (*ledit)(void); /* current menu constructor (B target) */

    int editing, symed;
    int vedit, seldb, sec_en, sec_cnt;
    int band, fxed;

    Inert ixcon, iycon;

    /* parameter editing */
    const VlmPbInfo *eddie;
    const VlmPbInfo *elvex[16];
    EdOption elopts[16];
    char eltxt[16][40];
    int elcount;
    int param_screen_is_sym; /* which list foredit/symedit rebuilt */
} ed;

/* ---- print pipeline (print, 3928) ------------------------------------ */

static void edtable(int x, int y);

static int gnum(const char **p)
{
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    if (**p == ':')
        (*p)++;
    return v;
}

static void ed_print(const char *s)
{
    int x = ed.cursx, y = ed.cursy;
    int entry_col = ed.cursx;
    while (*s) {
        char c = *s++;
        switch (c) {
        case '/':
            y++;
            x = entry_col;
            break;
        case '`':
            y--;
            x = entry_col;
            break;
        case '@':
            vlm_board_clear();
            break;
        case '^':
            vlm_board_clear();
            x = y = 0;
            break;
        case '~': {
            char k = *s ? *s++ : 0;
            if (k == 'c') {
                ed.actime = gnum(&s);
            } else if (k == 'g') {
                x = gnum(&s);
                y = gnum(&s);
            } else if (k == 'e') {
                int ex = gnum(&s);
                int ey = gnum(&s);
                edtable(ex, ey);
            } else if (k == 'i') {
                ed.inverse = (*s == '+');
                if (*s)
                    s++;
            }
            break;
        }
        default:
            vlm_board_char(x++, y, (uint8_t)c, ed.inverse);
            break;
        }
    }
    ed.cursx = x;
    ed.cursy = y;
}

static void print_at(int x, int y, const char *s)
{
    int sx = ed.cursx, sy = ed.cursy;
    ed.cursx = x;
    ed.cursy = y;
    ed_print(s);
    ed.cursx = sx;
    ed.cursy = sy;
}

/* cprint (3919): clear, home to (1,1), print. */
static void cprint(const char *s)
{
    ed.cursx = 1;
    ed.cursy = 1;
    ed_print("@~g1:1:");
    ed_print(s);
}

/* ~e renderer (edtable, 3990) */
static void edtable(int x, int y)
{
    ed_print(ed.list.bottom);
    ed.selx = x;
    ed.sely = y;
    for (int i = 0; i <= ed.list.selectable; i++)
        print_at(x, y + i, ed.list.opt[i].text);
    vlm_board_char(x - 1, y + ed.list.selected, 0, 0);
    ed.cursorx = x - 1;
    ed.cursory = y + ed.list.selected;
}

/* ud_selcu (3588) as an action */
static void ud_selcu(void)
{
    vlm_board_char(ed.cursorx, ed.cursory, ' ', 0);
    ed.cursorx = ed.selx - 1;
    ed.cursory = ed.sely + ed.list.selected;
    vlm_board_char(ed.cursorx, ed.cursory, 0, 0);
}

/* ---- screen stack ----------------------------------------------------- */

static void push_screen(void (*fn)(void))
{
    if (ed.esp < 32)
        ed.stack[ed.esp++] = fn;
}

static void editquit(void)
{
    cprint("Standard Mode~c30:");
    ed.editing = ED_OFF;
    ed.symed = SY_NONE;
}

static void onstak(void)
{
    if (ed.esp <= 0) {
        editquit();
        return;
    }
    void (*fn)(void) = ed.stack[--ed.esp];
    if (!fn)
        editquit();
    else
        ed.action = fn;
}

/* giedit (3553): editlist = table, print heading, editing = SELECTOR */
static void giedit(const char *heading, const EdList *tpl)
{
    ed.actime = -1;
    ed.list = *tpl;
    ed.cursx = 1;
    ed.cursy = 1;
    ed_print(heading);
    ed.editing = ED_SELECTOR;
}

/* ---- inertial slider (inertcon, 6741; ixcon constants 8492) ---------- */

static void inertcon(Inert *c, int dir)
{
    const int32_t accel = 0x400, fric = 0x20000, cap = 0x20000;
    const int32_t lo = 0, hi = 0xFFFF00;
    if ((dir & 3) == 0 || (dir & 3) == 3) {
        int32_t mag = c->vel < 0 ? -c->vel : c->vel;
        if (mag < fric) {
            c->vel = 0;
            return;
        }
        c->vel -= c->vel < 0 ? -fric : fric;
    } else if (dir & 1) {
        if (c->val < lo) {
            c->val = lo;
            c->vel = 0;
            return;
        }
        if (c->vel >= -cap)
            c->vel -= accel;
    } else {
        if (c->val >= hi) {
            c->val = hi;
            c->vel = 0;
            return;
        }
        if (c->vel < cap)
            c->vel += accel;
    }
    c->val += c->vel;
    if (c->val < lo) {
        c->val = lo;
        c->vel = 0;
    } else if (c->val >= hi) {
        c->val = hi;
        c->vel = 0;
    }
}

/* ---- parameter get/set (fmode 2 / s16) ------------------------------- */

/* Link check (lnkchk, 2875): linked params edit the base copy. */
static uint8_t *param_target(uint8_t *fx, const VlmPbInfo *p)
{
    if (vlm_be16(fx + 256 + p->param_index * 4))
        return fx + 512;
    return fx;
}

/* fmode 2 (psi.gas:222): slider 0..$FFFF -> stored long. */
static void param_write(uint8_t *fx, const VlmPbInfo *p, uint32_t slider)
{
    uint32_t value;
    if ((p->type & 0x7F) == 1)
        value = (uint32_t)(uint16_t)slider * (p->max & 0xFFFF) >> 16;
    else
        value = (uint32_t)(((uint64_t)p->max * slider) >> 16);
    if (p->type & 0x80)
        value -= p->max >> 1;
    vlm_wbe32(param_target(fx, p) + p->param_index * 4, value);
}

/* s16 (3835): current value -> slider<<8 seed for ixcon. */
static int32_t param_slider(uint8_t *fx, const VlmPbInfo *p)
{
    uint32_t raw = vlm_be32(param_target(fx, p) + p->param_index * 4);
    if (p->type & 0x80)
        raw += p->max >> 1;
    uint32_t slider;
    if ((p->type & 0x7F) == 1) {
        uint32_t m = p->max & 0xFFFF;
        slider = m ? (uint32_t)(((uint64_t)raw << 16) / m) : 0;
    } else {
        uint32_t m = p->max >> 12;
        slider = m ? ((raw / m) << 12) & 0xFFFFFF : 0;
        slider >>= 8;
    }
    if (slider > 0xFFFF)
        slider = 0xFFFF;
    return (int32_t)(slider << 8);
}

/* ---- widget drawing on the fx screen (sblitblockk, 6343) ------------- */

static void sfill(int x, int y, int w, int h, uint16_t col)
{
    unsigned s = ed.e->skale <= 2 ? ed.e->skale : 0;
    x >>= s;
    y >>= s;
    w >>= s;
    h >>= s;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= VLM_SCREEN_H)
            continue;
        for (int c = 0; c < w; c++) {
            int px = x + c;
            if (px >= 0 && px < VLM_SCREEN_W)
                ed.screen[py * VLM_SCREEN_W + px] = col;
        }
    }
}

/* blitblock: unscaled fill (crot uses it) */
static void ufill(int x, int y, int w, int h, uint16_t col)
{
    for (int r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= VLM_SCREEN_H)
            continue;
        for (int c = 0; c < w; c++) {
            int px = x + c;
            if (px >= 0 && px < VLM_SCREEN_W)
                ed.screen[py * VLM_SCREEN_W + px] = col;
        }
    }
}

/* ---- edvex display routines (5.3) ------------------------------------ */

static uint8_t *cur_fx(void)
{
    return ed.e->fx[ed.fxed] ? ed.e->fx[ed.fxed] : ed.e->fx[0];
}

static void write_x(void)
{
    if (ed.eddie && cur_fx())
        param_write(cur_fx(), ed.eddie, (uint32_t)(ed.ixcon.val >> 8));
}

static void write_y(void)
{
    if (ed.eddie && ed.eddie->partner && cur_fx())
        param_write(cur_fx(), &vlm_pbinfo[ed.eddie->partner],
                    (uint32_t)(ed.iycon.val >> 8));
}

static void dv_snglx(void)
{
    write_x();
    int v = (int)((ed.ixcon.val >> 16) & 0xFF);
    sfill(0x3E, 0x94, 0x102, 0x0E, 0x80FF);
    sfill(0x40, 0x96, v + 1, 0x0A, (uint16_t)(0xFF00 | v));
}

static void dv_sisnglx(void)
{
    write_x();
    int d = (int)((ed.ixcon.val >> 16) & 0xFF) - 0x7F;
    sfill(0x3E, 0x94, 0x102, 0x0E, 0x80FF);
    if (d >= 0)
        sfill(0xC0, 0x96, d + 1, 0x0A, (uint16_t)(0xFF00 | (d & 0xFF)));
    else
        sfill(0xC0 + d, 0x96, -d + 1, 0x0A, (uint16_t)(0xFF00 | (d & 0xFF)));
}

static void dv_sidbl(void)
{
    write_x();
    write_y();
    sfill(0x3E, 0xBD, 0x102, 7, 0x80FF);
    sfill(0xBD, 0x3E, 7, 0x102, 0x80FF);
    int dx = (int)((ed.ixcon.val >> 16) & 0xFF) - 0x7F;
    int dy = (int)((ed.iycon.val >> 16) & 0xFF) - 0x7F;
    if (dx >= 0)
        sfill(0xC0, 0xBF, dx + 1, 3, 0xFFFF);
    else
        sfill(0xC0 + dx, 0xBF, -dx + 1, 3, 0xFFFF);
    if (dy >= 0)
        sfill(0xBF, 0xC0, 3, dy + 1, 0xFFFF);
    else
        sfill(0xBF, 0xC0 + dy, 3, -dy + 1, 0xFFFF);
}

static void dv_crot(void)
{
    write_x();
    write_y();
    if (!ed.eddie || !cur_fx())
        return;
    uint8_t *fx = cur_fx();
    int vx = vlm_be16(param_target(fx, ed.eddie)
                      + ed.eddie->param_index * 4);
    const VlmPbInfo *py = &vlm_pbinfo[ed.eddie->partner];
    int vy = vlm_be16(param_target(fx, py) + py->param_index * 4);
    ufill(vx & 0x1FF, 0, 1, 0x180, 0x88FF);
    ufill(0, vy & 0x1FF, 0x180, 1, 0x88FF);
}

static void dv_dvect(void)
{
    write_x();
    write_y();
    int x = (int)((ed.ixcon.val >> 16) & 0xFF) - 0x7F + 0xBB;
    int y = (int)((ed.iycon.val >> 16) & 0xFF) - 0x7F + 0xBB;
    sfill(x, y, 10, 10, 0x80FF);
}

/* shspec (2171): the Spectrum & Triggers display. */
static void dv_shspec(void)
{
    VlmEngine *e = ed.e;
    int bandl = e->avbank[ed.band][0], bandh = e->avbank[ed.band][1];
    if (bandl > bandh) {
        int t = bandl;
        bandl = bandh;
        bandh = t;
    }
    const int base = 0xF0 + 0x10; /* mony + $10 */
    for (int i = 0; i < 64; i++) {
        int h = (int)(e->absdelta[i] >> 8) + 1;
        uint16_t col = (i >= bandl && i <= bandh) ? 0xF0C0 : 0x8FC0;
        sfill(60 + i * 4, base - h, 3, h, col);
    }
    sfill(60 + bandl * 4, base - (int)(e->envvals[ed.band] >> 9),
          (bandh - bandl) * 4 + 3, 1, 0xFFFF);
    sfill(60, base - (e->avbank[ed.band][2] >> 9), 256, 1, 0x88FF);
    sfill(60, base - (e->avbank[ed.band][3] >> 8), 256, 1, 0x4480);
    for (int i = 0; i < 5; i++)
        sfill(0x96 + i * 0x10, 0x78, 10, 10,
              (e->trig & (1 << i)) ? 0x8FF0 : 0xF0F0);
}

/* ---- screens ---------------------------------------------------------- */

static void ispec(void);
static void ispec2(void);
static void ispec3(void);
static void ispec4(void);
static void old_edit(void);
static void thisfx(void);
static void edit2(void);
static void foredit(void);
static void symedit(void);
static void eparam(void);
static void unquit(void);

static const EdOption option7[] = {
    { "Trigger 1", ispec2 }, { "Trigger 2", ispec2 },
    { "Trigger 3", ispec2 }, { "Trigger 4", ispec2 },
    { "Trigger 5", ispec2 },
};
static const char bline1[] = "~g1:20:<A> Edit   <B> Edit   <C> Back";
static const char bline2[] = "~g1:20:Joypad to select, any FIRE to edit";

static const EdList list_ispec = { 4, 0, bline1, option7 };

static void ispec(void)
{
    ed.ledit = ispec;
    giedit("@~g1:1:Spectrum and Triggers~e3:3:", &list_ispec);
    ed.symed = SY_SHSPEC;
    ed.editing = ED_SPECED;
}

static const EdOption option8[] = {
    { "Set Width", ispec3 },
    { "Set Trigger Minimum", ispec4 },
};
static const EdList list_ispec2 = { 1, 0, bline1, option8 };

static void ispec2(void)
{
    push_screen(ispec);
    giedit("@~g1:1:Trigger Settings~e3:3:", &list_ispec2);
    ed.symed = SY_SHSPEC;
}

static void ispec3(void)
{
    push_screen(ispec2);
    ed.actime = -1;
    cprint("Adjust width using joypad");
    ed.editing = ED_SPECED2;
    ed.symed = SY_SHSPEC;
}

static void ispec4(void)
{
    push_screen(ispec2);
    ed.actime = -1;
    cprint("Adjust trigger minimum with pad");
    ed.editing = ED_SPECED3;
    ed.symed = SY_SHSPEC;
}

static const EdOption option1[] = {
    { "Edit this effect", thisfx },
    { "Spectrum and triggers", ispec },
};
static const EdList list_oldedit = { 1, 0, bline1, option1 };

static void old_edit(void)
{
    ed.sec_en = 0;
    ed.sec_cnt = 0;
    ed.ledit = old_edit;
    giedit("@~g1:1:Edit Mode~e3:4:", &list_oldedit);
    ed.symed = SY_NONE;
}

/* thisfx (2956): slot chooser built from the live fx set */
static EdOption slot_opts[6];

static void thisfx(void)
{
    push_screen(old_edit);
    for (int s = 0; s < 6; s++) {
        uint8_t *fx = ed.e->fx[s];
        uint32_t info = fx ? vlm_be32(fx + FX_INFO) : 0;
        if (info && info <= VLM_VARS_COUNT) {
            slot_opts[s].text = vlm_vars[info - 1].name;
            slot_opts[s].act = edit2;
        } else {
            slot_opts[s].text = "<Empty>";
            slot_opts[s].act = old_edit;
        }
    }
    static EdList l;
    l.selectable = 5;
    l.selected = 0;
    l.bottom = bline2;
    l.opt = slot_opts;
    ed.ledit = thisfx;
    giedit("@~g1:1:Choose a subeffect slot to edit~e3:3:", &l);
    ed.symed = SY_NONE;
}

static const EdOption option2[] = {
    { "Edit source function", foredit },
    { "Edit symmetry generator", symedit },
    { "Edit source waves", unquit }, /* wave editor: next increment */
};
static const EdList list_edit2 = { 2, 0, bline1, option2 };

static void edit2(void)
{
    ed.fxed = ed.list.selected;
    push_screen(thisfx);
    ed.ledit = edit2;
    giedit("@~g1:1:Editing: Effect~e3:4:", &list_edit2);
    ed.symed = SY_NONE;
}

static void unquit(void)
{
    cprint("This function not yet implemented~c30:");
    ed.editing = ED_SELECTOR;
}

/* elcon (1628): build a parameter menu from a pbinfo index list */
static void elcon(const uint8_t *idxlist)
{
    ed.elcount = 0;
    uint8_t seen[64] = { 0 };
    for (const uint8_t *p = idxlist; *p != 0xFF && ed.elcount < 16; p++) {
        uint8_t i = *p;
        if (i >= 64 || seen[i])
            continue;
        seen[i] = 1;
        const VlmPbInfo *pb = &vlm_pbinfo[i];
        char *txt = ed.eltxt[ed.elcount];
        if (pb->flags & 1) {
            /* X of an X/Y pair: compose "name: (X,Y)" (1675-1720) */
            const char *colon = strchr(pb->text, ':');
            size_t n = colon ? (size_t)(colon - pb->text) + 1
                             : strlen(pb->text);
            if (n > 28)
                n = 28;
            memcpy(txt, pb->text, n);
            const VlmPbInfo *py = &vlm_pbinfo[pb->partner];
            const char *cx = strchr(pb->text, ':');
            const char *cy = strchr(py->text, ':');
            snprintf(txt + n, 40 - n, " (%c,%c)",
                     cx && cx[1] ? cx[2] : 'X', cy && cy[1] ? cy[2] : 'Y');
        } else {
            snprintf(txt, 40, "%s", pb->text);
            /* trim trailing spaces for the menu line */
            for (int k = (int)strlen(txt) - 1; k >= 0 && txt[k] == ' '; k--)
                txt[k] = 0;
        }
        ed.elvex[ed.elcount] = pb;
        ed.elopts[ed.elcount].text = txt;
        ed.elopts[ed.elcount].act = pb->edvex ? eparam : unquit;
        ed.elcount++;
    }
}

static void param_list_screen(const uint8_t *idxlist, const char *heading,
                              void (*self)(void), void (*parent)(void))
{
    push_screen(parent);
    elcon(idxlist);
    static EdList l;
    l.selectable = ed.elcount - 1;
    l.selected = 0;
    l.bottom = bline2;
    l.opt = ed.elopts;
    ed.ledit = self;
    giedit(heading, &l);
    ed.symed = SY_NONE;
}

static void foredit(void)
{
    uint8_t *fx = cur_fx();
    uint32_t info = fx ? vlm_be32(fx + FX_INFO) : 0;
    if (!info || info > VLM_VARS_COUNT) {
        onstak();
        return;
    }
    ed.param_screen_is_sym = 0;
    param_list_screen(vlm_vars[info - 1].params,
                      "@~g1:1:Editing: Wave Plotter~e3:3:",
                      foredit, edit2);
}

static void symedit(void)
{
    ed.param_screen_is_sym = 1;
    param_list_screen(vlm_symvars_params,
                      "@~g1:1:Editing: Symmetry Generator~e3:3:",
                      symedit, edit2);
}

/* eparam (3635) -> crunt (3723): start editing the selected param */
static void eparam(void)
{
    int sel = ed.list.selected;
    if (sel < 0 || sel >= ed.elcount)
        return;
    const VlmPbInfo *pb = ed.elvex[sel];
    ed.eddie = pb;

    if (pb->edvex == 6 || pb->edvex == 7) {
        /* symmetry-planes / trigger-mask keypad screens: next
         * increment */
        unquit();
        return;
    }

    /* crunt: symed = edvex, editing = edvex+1 */
    ed.symed = pb->edvex;
    ed.editing = pb->edvex + 1;

    /* primexy (3790): seed sliders from current values */
    uint8_t *fx = cur_fx();
    if (fx) {
        ed.ixcon.val = param_slider(fx, pb);
        ed.ixcon.vel = 0;
        if (pb->flags & 1) {
            ed.iycon.val = param_slider(fx, &vlm_pbinfo[pb->partner]);
            ed.iycon.vel = 0;
        }
    }

    cprint("Edit: ");
    ed_print(pb->text);
    ed_print("~g1:18:Press ~i+*~i- to attach waveforms");
    ed_print("~g1:20:<A> Prev   <B> Menu   <C> Next");
}

/* A/C during param edit: previous/next parameter (pn_butte, 5192) */
static void param_step(int dir)
{
    int n = ed.list.selectable + 1;
    ed.list.selected = (ed.list.selected + dir + n) % n;
    ed.action = ed.list.opt[ed.list.selected].act;
}

/* ---- input routines (editvex) ----------------------------------------- */

static void selector_input(void)
{
    uint32_t shot = ed.shot;
    if (shot & PAD_UP) {
        if (--ed.list.selected < 0)
            ed.list.selected = ed.list.selectable;
        ed.action = ud_selcu;
    } else if (shot & PAD_DOWN) {
        if (++ed.list.selected > ed.list.selectable) {
            ed.list.selected = 0;
            if (ed.sec_en && ++ed.sec_cnt >= 8) {
                ed.action = old_edit;
                return;
            }
        }
        ed.action = ud_selcu;
    } else if (shot & (PAD_A | PAD_B)) {
        if (ed.editing == ED_SPECED)
            ed.band = ed.list.selected;
        ed.action = ed.list.opt[ed.list.selected].act;
        ed.editing = ED_OFF;
        ed.seldb = 1;
    } else if (shot & PAD_C) {
        onstak();
        ed.editing = ED_OFF;
        ed.seldb = 1;
    }
}

/* speced2 (4723): band width editing, throttled to every 8th frame */
static void speced2_input(void)
{
    static unsigned throttle;
    uint16_t *bp = ed.e->avbank[ed.band];
    if (ed.shot & (PAD_A | PAD_B | PAD_C)) {
        ed.editing = ED_SELECTOR;
        ed.symed = SY_NONE;
        onstak();
        ed.seldb = 1;
        return;
    }
    if ((++throttle & 7) != 0)
        return;
    uint32_t b = ed.buttons;
    if (b & PAD_UP) {
        if (bp[0] > 0)
            bp[0]--;
        if (bp[1] < 63)
            bp[1]++;
    } else if (b & PAD_DOWN) {
        if (bp[1] - bp[0] > 2) {
            bp[0]++;
            bp[1]--;
        }
    } else if (b & PAD_LEFT) {
        if (bp[0] > 0) {
            bp[0]--;
            bp[1]--;
        }
    } else if (b & PAD_RIGHT) {
        if (bp[1] < 63) {
            bp[0]++;
            bp[1]++;
        }
    }
}

static void speced3_input(void)
{
    uint16_t *bp = ed.e->avbank[ed.band];
    if (ed.shot & (PAD_A | PAD_B | PAD_C)) {
        ed.editing = ED_SELECTOR;
        ed.symed = SY_NONE;
        onstak();
        ed.seldb = 1;
        return;
    }
    if (ed.buttons & PAD_UP)
        bp[2] = (uint16_t)(bp[2] + 0x100);
    else if (ed.buttons & PAD_DOWN)
        bp[2] = (uint16_t)(bp[2] - 0x100);
}

/* x_one/xy1 + spn_butt/pn_butte (5156-5226) */
static void param_input(int both_axes)
{
    int dx = ((ed.buttons & PAD_LEFT) ? 1 : 0)
             | ((ed.buttons & PAD_RIGHT) ? 2 : 0);
    inertcon(&ed.ixcon, dx);
    if (both_axes) {
        int dy = ((ed.buttons & PAD_UP) ? 1 : 0)
                 | ((ed.buttons & PAD_DOWN) ? 2 : 0);
        inertcon(&ed.iycon, dy);
    }
    if (ed.shot & PAD_B) {
        ed.action = ed.ledit; /* bbexit: back to the menu */
        ed.editing = ED_OFF;
        ed.symed = SY_NONE;
        ed.seldb = 1;
    } else if (ed.shot & PAD_A) {
        param_step(-1);
        ed.editing = ED_OFF;
        ed.symed = SY_NONE;
        ed.seldb = 1;
    } else if (ed.shot & PAD_C) {
        param_step(1);
        ed.editing = ED_OFF;
        ed.symed = SY_NONE;
        ed.seldb = 1;
    }
}

/* ---- entry / frame ---------------------------------------------------- */

static void initedit(void)
{
    ed.esp = 0;
    push_screen(NULL); /* sentinel: popping it quits */
    ed.sec_en = 1;
    ed.sec_cnt = 0;
    ispec();
}

static void setedit(void)
{
    if (ed.vedit)
        return;
    ed.vedit = 1;
    vlm_board_visible = 1;
    ed.action = initedit;
    ed.editing = ED_SELECTOR; /* placeholder until initedit runs */
}

int vlm_editor_frame(VlmEngine *e, uint32_t buttons, uint16_t *screen)
{
    ed.e = e;
    ed.screen = screen;
    ed.shot = buttons & ~ed.prev;
    ed.buttons = buttons;
    ed.prev = buttons;

    /* Entry: keypad 7 + * + 0 + 3 held, nothing else (exact equality,
     * 4452-4464), on the pad-ish bits only. */
    uint32_t pad = buttons & ~(PAD_SCOPE | PAD_AUDIO);
    uint32_t combo = PAD_DIGIT(7) | PAD_STAR | PAD_DIGIT(0) | PAD_DIGIT(3);
    if (pad == combo || pad == PAD_PAUSE) { /* Pause = mwenge's shortcut */
        if (!ed.editing && !ed.action)
            setedit();
    } else if (pad == PAD_OPTION) {
        /* port convenience: Option jumps straight to the hidden root
         * (the original needs 8 downward menu wraps) */
        if (!ed.editing && !ed.action && !ed.vedit) {
            ed.vedit = 1;
            vlm_board_visible = 1;
            ed.esp = 0;
            push_screen(NULL);
            ed.action = old_edit;
            ed.editing = ED_SELECTOR;
        }
    } else {
        ed.vedit = 0;
    }

    /* seldb debounce: wait for all buttons up (4364-4372) */
    if (ed.seldb) {
        if (buttons & ~(PAD_SCOPE | PAD_AUDIO))
            return ed.editing != ED_OFF || ed.action != NULL;
        ed.seldb = 0;
    }

    /* input side (Frame ISR piece) */
    if (ed.editing && !ed.action) {
        switch (ed.editing) {
        case ED_SELECTOR:
            selector_input();
            break;
        case ED_XY1:      /* crot */
        case ED_XY1_2:    /* sidbl */
        case ED_XY1_3:    /* dvect */
            param_input(1);
            break;
        case ED_X_ONE:    /* snglx */
        case ED_X_ONE_2:  /* sisnglx */
            param_input(0);
            break;
        case ED_SPECED:
            selector_input();
            break;
        case ED_SPECED2:
            speced2_input();
            break;
        case ED_SPECED3:
            speced3_input();
            break;
        default:
            break;
        }
    }

    /* drawing side (yakedit piece) */
    if (ed.actime >= 0 && --ed.actime < 0)
        vlm_board_clear();
    if (ed.action) {
        void (*a)(void) = ed.action;
        ed.action = NULL;
        a();
    }
    switch (ed.symed) {
    case SY_CROT:
        dv_crot();
        break;
    case SY_SNGLX:
        dv_snglx();
        break;
    case SY_SISNGLX:
        dv_sisnglx();
        break;
    case SY_SIDBL:
        dv_sidbl();
        break;
    case SY_DVECT:
        dv_dvect();
        break;
    case SY_SHSPEC:
        dv_shspec();
        break;
    default:
        break;
    }

    return ed.editing != ED_OFF || ed.action != NULL;
}
