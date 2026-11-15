// pics.c - I: the pictures page, as PICOAMP's on the MSX (engine 0.17), in the
// browser: a folder of pictures the person picks (as the music folder: Chrome
// and Edge remember it), its folders and pictures; Enter shows one full screen
// in SCREEN 8 (256 x 192, 256 colours: picture.c), Space marks pictures, G
// plays the marked ones (or all of the folder) as a slideshow; the settings:
// O the screensaver (off, or after 1-30 minutes without a key), R how long
// each picture shows, U in order or shuffled, F this folder for the
// screensaver. The screensaver (ui.c) opens this page with pics_saver().
//
// The page reads and decodes the pictures (the browser decodes every kind it
// knows); they come back as RGB (web_pic_rgb) and are made SCREEN 8 here. The
// next one of a slideshow is made while the one before shows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"
#include "sys.h"
#include "util.h"
#include "config.h"
#include "picture.h"
#include "web.h"

#define ROWS 19                         // the list: rows 2-20
enum { M_BROWSE, M_VIEW, M_SHOW, M_SAVER };

typedef struct { char *name; bool dir; bool mark; } entry;

static char   path[1024];               // the folder, in the picked one ("": the picked one)
static entry *ent;
static int    n_ent, n_dir, top, cur;   // (at the top: entry 0 picks another folder)
static int    mode;
static int    shown = -1;
static char   msg[80];
static bool   saver_start, saver_wait;  // the screensaver: asked for; its folder being read
static bool   listing;                  // a folder being read
static unsigned list_gen;

// ---------------------------------------------------------------- the pictures, from the page
static unsigned want_gen;               // the picture asked for last
static int      want_idx = -1;
static struct { bool done; int err; uint8_t px[192][256]; } res;
static uint8_t  scr[192][256];          // what shows
static bool     have_scr;
static uint32_t t_shown;
static int      fails;

static bool special(int i) { return !path[0] && i == 0; }      // "choose another folder"
static int  first_pic(void) { return n_dir; }

static void load(int idx)
{
    if (idx < 0 || idx >= n_ent || ent[idx].dir) return;
    char p[2048];
    snprintf(p, sizeof p, "%s%s%s", path, path[0] ? "/" : "", ent[idx].name);
    want_gen = (want_gen + 1) & 0x3FFFFFFF;
    want_idx = idx;
    res.done = false;
    js_pic_file((int)want_gen, p, (int)strlen(p), 564, 384);
}

void pics_pic_rgb(int id, int w, int h, const uint8_t *rgb)
{
    if ((unsigned)id != want_gen) return;
    res.done = true;
    res.err = (w > 0 && h > 0 && rgb) ? PIC_OK : PIC_E_FORMAT;
    if (res.err == PIC_OK) pic_to_s8(rgb, w, h, res.px);
}

// the picture asked for, when it is made: 1 made (in scr), -1 it cannot be read, 0 not yet
static int take(void)
{
    if (!res.done) return 0;
    res.done = false;
    if (res.err != PIC_OK) return -1;
    memcpy(scr, res.px, sizeof scr);
    return 1;
}

// ---------------------------------------------------------------- the folder
static void free_list(void)
{
    for (int i = 0; i < n_ent; i++) free(ent[i].name);
    free(ent);
    ent = NULL;
    n_ent = n_dir = 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return u_stricmp(x->name, y->name);
}

static int  next_pic(int from);
static void start(int m, int idx);

typedef struct { unsigned gen; char path[1024]; char select[256]; } list_req;

static void listed(void *user, const char *lines, int ok)
{
    list_req *q = user;
    if (q->gen != list_gen) { free(q); return; }
    listing = false;
    if (!ok)
    {
        snprintf(msg, sizeof msg, "%.70s", lines && lines[0] ? lines : "That folder cannot be read");
        if (saver_wait) { saver_wait = false; ui_page(PG_PLAYER); }
        free(q);
        return;
    }
    free_list();
    u_copy(path, sizeof path, q->path);
    int cap = 1;
    for (const char *s = lines; *s; s++) cap += *s == '\n';
    ent = calloc((size_t)cap + 1, sizeof *ent);
    if (!path[0] && ent) { ent[0].name = strdup("Choose another folder..."); ent[0].dir = true; n_ent = n_dir = 1; }
    for (const char *s = lines; ent && *s; )
    {
        const char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l > 2 && s[2] != '.')
        {
            char nm[512];
            snprintf(nm, sizeof nm, "%.*s", (int)(l - 2), s + 2);
            bool dir = s[0] == 'd';
            if (dir || pic_is_picture_name(nm))
            {
                ent[n_ent].name = strdup(nm);
                ent[n_ent].dir = dir;
                n_dir += dir;
                n_ent++;
            }
        }
        if (!e) break;
        s = e + 1;
    }
    int skip = path[0] ? 0 : 1;
    qsort(ent + skip, (size_t)(n_ent - skip), sizeof *ent, ent_cmp);
    top = cur = 0;
    for (int i = 0; q->select[0] && i < n_ent; i++) if (!strcmp(ent[i].name, q->select)) { cur = i; break; }
    if (cur >= ROWS) top = cur - ROWS / 2;
    free(q);
    if (saver_wait)
    {
        saver_wait = false;
        int first = next_pic(n_ent);
        if (first < 0) { ui_page(PG_PLAYER); return; }
        start(M_SAVER, first);
    }
}

static void read_folder(const char *p, const char *select)
{
    list_req *q = malloc(sizeof *q);
    if (!q) return;
    q->gen = ++list_gen;
    u_copy(q->path, sizeof q->path, p);
    u_copy(q->select, sizeof q->select, select ? select : "");
    listing = true;
    pics_list(p, listed, q);
}

static int n_pics(void) { return n_ent - n_dir; }
static int n_marked(void) { int m = 0; for (int i = n_dir; i < n_ent; i++) m += ent[i].mark; return m; }

static void parent(void)
{
    if (!path[0]) return;
    char p[1024];
    u_copy(p, sizeof p, path);
    char *sl = strrchr(p, '/');
    const char *was = sl ? sl + 1 : p;
    char w[256];
    u_copy(w, sizeof w, was);
    if (sl) *sl = 0;
    else p[0] = 0;
    read_folder(p, w);
}

// ---------------------------------------------------------------- the slideshow's order
static unsigned rng = 0x9E3779B9u;

static int next_pic(int from)
{
    int np = n_pics(), nm = n_marked();
    if (!np) return -1;
    if (cfg.pic_shuffle)
    {
        rng = rng * 1103515245u + 12345u + sys_millis();
        from = first_pic() + (int)((rng >> 8) % (unsigned)np) - 1;
    }
    for (int k = 0; k < n_ent; k++)
    {
        from++;
        if (from >= n_ent || from < n_dir) from = n_dir;
        if (!nm || ent[from].mark) return from;
    }
    return -1;
}

static void start(int m, int idx)
{
    mode = m;
    shown = idx;
    have_scr = false;
    fails = 0;
    t_shown = 0;
    load(idx);
}

static void back(void)
{
    if (mode == M_SAVER) { ui_page(PG_PLAYER); return; }
    mode = M_BROWSE;
    want_gen = (want_gen + 1) & 0x3FFFFFFF;     // (a picture still being made: not wanted)
    if (shown >= 0 && shown < n_ent)
    {
        cur = shown;
        if (cur < top || cur >= top + ROWS) top = cur > ROWS / 2 ? cur - ROWS / 2 : 0;
    }
}

// ---------------------------------------------------------------- the page
static const int savers[] = { 0, 1, 2, 5, 10, 15, 30 };
static const int intervals[] = { 5, 10, 20, 30, 60 };

void pics_saver(void) { saver_start = true; ui_page(PG_PICS); }

static void pc_open(void)
{
    mode = M_BROWSE;
    msg[0] = 0;
    free_list();
    if (saver_start)
    {
        saver_start = false;
        char nm[8];
        if (!js_pics_name(nm, sizeof nm)) { ui_page(PG_PLAYER); return; }    // (no folder picked yet)
        saver_wait = true;
        read_folder(cfg.pic_folder, NULL);
        return;
    }
    char nm[8];
    if (js_pics_name(nm, sizeof nm)) read_folder(cfg.pic_folder, NULL);
    else
    {
        path[0] = 0;
        ent = calloc(1, sizeof *ent);
        if (ent) { ent[0].name = strdup("Choose a folder of pictures..."); ent[0].dir = true; n_ent = n_dir = 1; }
        top = cur = 0;
    }
}

static void pc_close(void)
{
    want_gen = (want_gen + 1) & 0x3FFFFFFF;
    list_gen++;
    listing = saver_wait = false;
    free_list();
}

static void draw_browser(void)
{
    gfx_clear(NAVY);
    gfx_fill(0, 0, 32, COL(WHITE, NAVY));
    ui_row0(0, 0, 32, COL(WHITE, NAVY));
    char t[1200], root[200] = "";
    js_pics_name(root, sizeof root);
    snprintf(t, sizeof t, "Pictures  %s%s%s", root[0] ? root : "(no folder)", path[0] ? "/" : "", path);
    gfx_fill(1, 0, 32, COL(WHITE, BLUE));
    gfx_text6(1, 0, 32, t, COL(WHITE, BLUE), 0, 0);
    for (int r = 0; r < ROWS; r++)
    {
        int i = top + r;
        gfx_fill(2 + r, 0, 32, COL(WHITE, NAVY));
        if (i >= n_ent) continue;
        if (special(i)) snprintf(t, sizeof t, " %s", ent[i].name);
        else snprintf(t, sizeof t, "%c%s%s", ent[i].mark ? '*' : ' ', ent[i].name, ent[i].dir ? "/" : "");
        uint8_t c = i == cur ? COL(NAVY, CYAN) : special(i) ? COL(GREEN, NAVY) : ent[i].mark ? COL(YELLOW, NAVY) : ent[i].dir ? COL(CYAN, NAVY) : COL(WHITE, NAVY);
        if (i == cur) gfx_fill(2 + r, 0, 32, c);
        gfx_text6(2 + r, 0, 32, t, c, 0, 0);
    }
    const char *note = ui_note_text();
    if (msg[0]) snprintf(t, sizeof t, "%s", msg);
    else if (listing) snprintf(t, sizeof t, "Reading the folder...");
    else if (note[0]) snprintf(t, sizeof t, "%s", note);
    else if (n_marked()) snprintf(t, sizeof t, "%d pictures, %d marked", n_pics(), n_marked());
    else snprintf(t, sizeof t, "%d pictures", n_pics());
    gfx_text6(21, 0, 32, t, COL(msg[0] || (note[0] && ui_note_error()) ? RED : GREEN, NAVY), 0, 0);
    gfx_text6(22, 0, 32, "Enter view  Space mark  A all  G slideshow", COL(GREY, NAVY), 0, 0);
    char sv[16];
    if (cfg.pic_saver) snprintf(sv, sizeof sv, "%dm", cfg.pic_saver);
    else snprintf(sv, sizeof sv, "off");
    snprintf(t, sizeof t, "O saver %s  R %ds  U shuffle %s  F here", sv, cfg.pic_interval, cfg.pic_shuffle ? "on" : "off");
    gfx_text6(23, 0, 32, t, COL(GREY, NAVY), 0, 0);
}

static void pc_frame(void)
{
    if (mode == M_BROWSE) { if (saver_wait) gfx_clear(BLK); else draw_browser(); return; }
    bool show = mode == M_SHOW || mode == M_SAVER;
    // the picture asked for: at once the first one and one picture; in a slideshow after its time
    if (!have_scr || mode == M_VIEW || sys_millis() - t_shown >= (uint32_t)cfg.pic_interval * 1000u)
    {
        int r = take();
        if (r > 0)
        {
            have_scr = true;
            shown = want_idx;
            t_shown = sys_millis();
            fails = 0;
            if (show) { int nx = next_pic(shown); if (nx >= 0 && nx != shown) load(nx); }
        }
        else if (r < 0)
        {
            if (mode == M_VIEW)
            {
                snprintf(msg, sizeof msg, "Not a picture this browser can read");
                if (!have_scr) { back(); draw_browser(); return; }
            }
            else if (++fails >= n_pics())
            {
                if (mode == M_SAVER) { ui_page(PG_PLAYER); return; }
                snprintf(msg, sizeof msg, "None of them can be read");
                back();
                draw_browser();
                return;
            }
            else { int nx = next_pic(want_idx); if (nx >= 0) load(nx); }
        }
    }
    if (have_scr)
    {
        memcpy(gfx_s8, scr, sizeof scr);
        gfx_s8_on = true;
    }
    else gfx_clear(BLK);
}

static void move(int d)
{
    if (!n_ent) return;
    cur += d;
    if (cur < 0) cur = 0;
    if (cur >= n_ent) cur = n_ent - 1;
    if (cur < top) top = cur;
    if (cur >= top + ROWS) top = cur - ROWS + 1;
}

static void enter(void)
{
    if (!n_ent || listing) return;
    if (special(cur)) { js_pics_pick(); read_folder("", NULL); return; }
    if (ent[cur].dir)
    {
        char p[1024];
        snprintf(p, sizeof p, "%s%s%s", path, path[0] ? "/" : "", ent[cur].name);
        read_folder(p, NULL);
        return;
    }
    start(M_VIEW, cur);
}

static int step_of(const int *v, int n, int x) { for (int i = 0; i < n; i++) if (v[i] == x) return (i + 1) % n; return 0; }

static void pc_key(int k)
{
    msg[0] = 0;
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    if (mode == M_SAVER || saver_wait) { ui_page(PG_PLAYER); return; }
    if (mode != M_BROWSE)
    {
        if (u == K_RIGHT || u == K_LEFT)
        {
            if (mode != M_VIEW) { if (u == K_RIGHT) t_shown = 0; return; }   // the slideshow: the next one now
            int d = u == K_RIGHT ? 1 : -1;
            for (int i = want_idx + d; i >= n_dir && i < n_ent; i += d)
                if (!ent[i].dir) { load(i); return; }
            return;
        }
        if (u == ' ' && mode == M_VIEW) { if (shown >= n_dir && shown < n_ent) ent[shown].mark = !ent[shown].mark; return; }
        back();
        return;
    }
    switch (u)
    {
    case K_UP: move(-1); return;
    case K_DOWN: move(1); return;
    case K_LEFT: case K_PGUP: move(-ROWS); return;
    case K_RIGHT: case K_PGDN: move(ROWS); return;
    case K_HOME: move(-n_ent); return;
    case K_END: move(n_ent); return;
    case K_ENTER: enter(); return;
    case K_BS: parent(); return;
    case ' ':
        if (n_ent && !ent[cur].dir) { ent[cur].mark = !ent[cur].mark; move(1); }
        return;
    case 'A':
    {
        bool all = n_marked() == n_pics();
        for (int i = n_dir; i < n_ent; i++) ent[i].mark = !all;
        return;
    }
    case 'G':
    {
        int first = next_pic(n_ent);
        if (first < 0) { snprintf(msg, sizeof msg, "No pictures here"); return; }
        start(M_SHOW, first);
        return;
    }
    case 'O': cfg.pic_saver = savers[step_of(savers, 7, cfg.pic_saver)]; return;
    case 'R': cfg.pic_interval = intervals[step_of(intervals, 5, cfg.pic_interval)]; return;
    case 'U': cfg.pic_shuffle = !cfg.pic_shuffle; return;
    case 'F':
        u_copy(cfg.pic_folder, sizeof cfg.pic_folder, path);
        snprintf(msg, sizeof msg, "The screensaver shows this folder");
        return;
    case K_ESC: case 'I': ui_page(PG_PLAYER); return;
    case 'C': ui_page(PG_CLOCK); return;
    case 'W': ui_page(PG_WX); return;
    case 'T': ui_page(PG_NEWS); return;
    case 'Z': ui_page(PG_FULL); return;
    }
    if (u != 'S' && u != 'H' && u != 'R') ui_playback_key(k);
}

static void pc_mouse(int x, int y, int b)
{
    (void)x;
    if (mode != M_BROWSE) { if (b == MOUSE_LEFT || b == MOUSE_RIGHT) pc_key(K_ESC); return; }
    if (b == MOUSE_RIGHT) { ui_page(PG_PLAYER); return; }
    if (b == MOUSE_WHEEL_UP) { move(-3); return; }
    if (b == MOUSE_WHEEL_DOWN) { move(3); return; }
    if (b != MOUSE_LEFT) return;
    int row = y / 8;
    if (row >= 2 && row < 2 + ROWS && top + row - 2 < n_ent)
    {
        int i = top + row - 2;
        if (i == cur) enter();
        else cur = i;
    }
}

static void pc_describe(char *out, size_t n)
{
    static const char *const mn[] = { "browse", "view", "slideshow", "screensaver" };
    snprintf(out, n, "pictures %s folder='%s' entries=%d folders=%d pictures=%d marked=%d cur=%d shown=%d have=%d saver=%d interval=%d shuffle=%d%s",
             mn[mode], path, n_ent, n_dir, n_pics(), n_marked(), cur, shown, have_scr, cfg.pic_saver, cfg.pic_interval, cfg.pic_shuffle,
             listing ? " reading" : "");
}

const page_t page_pics = { pc_open, pc_frame, pc_key, pc_mouse, pc_close, pc_describe };
