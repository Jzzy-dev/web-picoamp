// news_web.c - T: the news page. A browser may not read other sites' feeds
// (they do not allow it: CORS), so the news needs a small relay of its own;
// until there is one this page says so.
#include <stdio.h>
#include <string.h>
#include "pages.h"
#include "gfx.h"
#include "ui.h"

static void nw_open(void) {}
static void nw_close(void) {}

static void nw_frame(void)
{
    gfx_clear(NAVY);
    gfx_fill(0, 0, 32, COL(WHITE, BLUE));
    gfx_str(0, 1, "PICOAMP NEWS", 30, COL(WHITE, BLUE));
    static const char *const lines[] = {
        "The news is not in the web",
        "version yet.",
        "",
        "A browser may only read news",
        "feeds that allow it, and most",
        "do not. The news comes with a",
        "small relay of its own.",
        "",
        "The Windows version has it.",
    };
    for (unsigned i = 0; i < sizeof lines / sizeof lines[0]; i++)
        gfx_text6(4 + (int)i * 2, 1, 30, lines[i], COL(i < 2 ? YELLOW : WHITE, NAVY), 0, 0);
    gfx_fill(23, 0, 32, COL(GREY, NAVY));
    gfx_text6(23, 0, 32, "C clock  W weather  T or ESC back", COL(GREY, NAVY), 0, 0);
}

static void nw_key(int k)
{
    int u = (k >= 'a' && k <= 'z') ? k - 32 : k;
    switch (u)
    {
    case K_ESC: case K_ENTER: case K_BS: case 'T': case 'Z': ui_page(PG_PLAYER); return;
    case 'C': ui_page(PG_CLOCK); return;
    case 'W': ui_page(PG_WX); return;
    }
    ui_playback_key(k);
}

static void nw_mouse(int x, int y, int b)
{
    (void)x; (void)y;
    if (b == MOUSE_RIGHT || b == MOUSE_LEFT) ui_page(PG_PLAYER);
}

static void nw_describe(char *out, size_t n) { snprintf(out, n, "news: not in the web version"); }

const page_t page_news = { nw_open, nw_frame, nw_key, nw_mouse, nw_close, nw_describe };

int news_import(const char *opml_path) { (void)opml_path; return -1; }
