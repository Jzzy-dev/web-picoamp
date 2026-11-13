// script.c - see script.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "script.h"
#include "gfx.h"
#include "ui.h"
#include "audio.h"
#include "sys.h"
#include "util.h"

static char    *text, *cur;
static FILE    *out;
static uint32_t until;

void script_start(const char *t, FILE *o)
{
    free(text);
    text = strdup(t);
    cur = text;
    out = o ? o : stdout;
    until = 0;
}

static int key_code(const char *k)
{
    static const struct { const char *n; int c; } names[] = {
        { "Up", K_UP }, { "Down", K_DOWN }, { "Left", K_LEFT }, { "Right", K_RIGHT }, { "Enter", K_ENTER },
        { "BS", K_BS }, { "Esc", K_ESC }, { "Space", ' ' }, { "Del", K_DEL }, { "Home", K_HOME }, { "End", K_END },
        { "PgUp", K_PGUP }, { "PgDn", K_PGDN }, { "Tab", '\t' }, { "CtrlUp", K_CTRL_UP }, { "CtrlDown", K_CTRL_DOWN },
    };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) if (!u_stricmp(k, names[i].n)) return names[i].c;
    return (unsigned char)k[0];
}

bool script_step(void)
{
    if (!cur) return false;
    if (until && (int32_t)(sys_millis() - until) < 0) return true;
    until = 0;
    char *cmd;
    while ((cmd = u_token(&cur, ";\n")))
    {
        char a[1024] = "", c[64] = "";
        cmd = u_trim(cmd);
        if (!*cmd) continue;
        char *sp = strchr(cmd, ' ');
        if (sp) { *sp = 0; u_copy(a, sizeof a, u_trim(sp + 1)); }
        if (!strcmp(cmd, "wait")) { until = sys_millis() + (uint32_t)atoi(a); if (!until) until = 1; return true; }
        else if (!strcmp(cmd, "key")) { ui_key(key_code(a)); until = sys_millis() + 20; return true; }
        else if (!strcmp(cmd, "type")) { for (char *p = a; *p; p++) ui_key((unsigned char)*p); }
        else if (!strcmp(cmd, "click"))
        {
            int mx = 0, my = 0;
            sscanf(a, "%d %d %63s", &mx, &my, c);
            ui_mouse(mx, my, !strcmp(c, "right") ? MOUSE_RIGHT : MOUSE_LEFT);
            ui_mouse(mx, my, MOUSE_UP);
            until = sys_millis() + 20;
            return true;
        }
        else if (!strcmp(cmd, "play")) audio_play_url(a, "Script");
        else if (!strcmp(cmd, "drop")) { const char *p[1] = { a }; ui_drop(p, 1); }
        else if (!strcmp(cmd, "shot")) { ui_frame(); fprintf(out, "shot %s: the page takes it\n", a); }   // (the tests: the canvas)
        else if (!strcmp(cmd, "status"))
        {
            audio_status s;
            audio_status_get(&s);
            fprintf(out, "state=%d elapsed=%d/%d rate=%d ch=%d br=%d stream=%d buf=%d(%d%%) pl=%d/%d vol=%d title='%s' artist='%s' album='%s' err='%s'\n",
                    s.state, s.elapsed, s.total, s.rate, s.channels, s.bitrate, s.stream, s.buffering, s.buf_pct,
                    s.pl_pos + 1, s.pl_count, s.volume, s.title, s.artist, s.album, s.error);
        }
        else if (!strcmp(cmd, "list") || !strcmp(cmd, "page"))
        {
            char *d = ui_describe(!strcmp(cmd, "list"));
            fprintf(out, "%s\n", d);
            free(d);
        }
        else if (!strcmp(cmd, "where")) fprintf(out, "settings %s\n", sys_config_dir());
        else if (!strcmp(cmd, "quit")) { cur = NULL; fflush(out); return false; }
        else fprintf(out, "unknown command %s\n", cmd);
        fflush(out);
    }
    cur = NULL;
    return false;
}
