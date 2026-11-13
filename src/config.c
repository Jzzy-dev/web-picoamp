// config.c - picoamp.ini (see config.h)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sys.h"
#include "util.h"

config cfg;

static void defaults(void)
{
    memset(&cfg, 0, sizeof cfg);
    cfg.volume = 80;
    cfg.balance = 50;
    u_copy(cfg.music_dir, sizeof cfg.music_dir, sys_music_dir());
    cfg.win_x = cfg.win_y = -1;
    cfg.win_w = 768;
    cfg.win_h = 576;
    cfg.scale = SCALE_FIT;
    cfg.clock_spectrum = true;
    cfg.loc_ip = true;
    cfg.alarm_h = 7;
    cfg.alarm_m = 30;
}

static const char *ini_path(void)
{
    static char p[1100];
    u_join(p, sizeof p, sys_config_dir(), "picoamp.ini");
    return p;
}

const char *config_radio_dir(void)
{
    static char p[1100];
    u_join(p, sizeof p, sys_config_dir(), "Radio");
    return p;
}

static bool yes(const char *v) { return !u_stricmp(v, "1") || !u_stricmp(v, "yes") || !u_stricmp(v, "on") || !u_stricmp(v, "true"); }

void config_load(void)
{
    defaults();
    char *text = sys_read_file(ini_path(), NULL);
    if (!text) { cfg.first_run = true; return; }
    char *cur = text;
    for (char *line; (line = u_token(&cur, "\r\n")); )
    {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        *eq = 0;
        char *k = u_trim(line), *v = u_trim(eq + 1);
        if (!u_stricmp(k, "volume")) cfg.volume = atoi(v);
        else if (!u_stricmp(k, "repeat")) cfg.mode = (cfg.mode & ~1) | (yes(v) ? 1 : 0);
        else if (!u_stricmp(k, "shuffle")) cfg.mode = (cfg.mode & ~2) | (yes(v) ? 2 : 0);
        else if (!u_stricmp(k, "eq"))
        {
            char *p = v;
            for (int i = 0; i < EQ_BANDS && *p; i++)
            {
                int g = (int)strtol(p, &p, 10);
                if (g > EQ_MAX_DB) g = EQ_MAX_DB;
                if (g < -EQ_MAX_DB) g = -EQ_MAX_DB;
                cfg.eq[i] = (int8_t)g;
                while (*p == ',' || *p == ' ') p++;
            }
        }
        else if (!u_stricmp(k, "balance")) { cfg.balance = atoi(v); if (cfg.balance < 0 || cfg.balance > 100) cfg.balance = 50; }
        else if (!u_stricmp(k, "view")) cfg.view = !u_stricmp(v, "scope") ? 1 : 0;
        else if (!u_stricmp(k, "music")) { if (*v) u_copy(cfg.music_dir, sizeof cfg.music_dir, v); }
        else if (!u_stricmp(k, "folder")) u_copy(cfg.start_path, sizeof cfg.start_path, v);
        else if (!u_stricmp(k, "window_x")) cfg.win_x = atoi(v);
        else if (!u_stricmp(k, "window_y")) cfg.win_y = atoi(v);
        else if (!u_stricmp(k, "window_w")) cfg.win_w = atoi(v);
        else if (!u_stricmp(k, "window_h")) cfg.win_h = atoi(v);
        else if (!u_stricmp(k, "maximized")) cfg.maximized = yes(v);
        else if (!u_stricmp(k, "fullscreen")) cfg.fullscreen = yes(v);
        else if (!u_stricmp(k, "scale")) cfg.scale = !u_stricmp(v, "integer") ? SCALE_INTEGER : !u_stricmp(v, "stretch") ? SCALE_STRETCH : SCALE_FIT;
        else if (!u_stricmp(k, "scanlines")) cfg.scanlines = yes(v);
        else if (!u_stricmp(k, "on_top")) cfg.on_top = yes(v);
        else if (!u_stricmp(k, "face")) cfg.clock_analog = !u_stricmp(v, "analog");
        else if (!u_stricmp(k, "time")) cfg.clock_pc_time = !u_stricmp(v, "pc");
        else if (!u_stricmp(k, "colours"))
        {
            static const char *const n[] = { "music", "cyan", "green", "amber", "red", "purple", "white" };
            for (int i = 0; i < 7; i++) if (!u_stricmp(v, n[i])) cfg.clock_colours = i;
        }
        else if (!u_stricmp(k, "spectrum")) cfg.clock_spectrum = yes(v);
        else if (!u_stricmp(k, "location")) cfg.loc_ip = u_stricmp(v, "place") != 0;
        else if (!u_stricmp(k, "place")) u_copy(cfg.loc_name, sizeof cfg.loc_name, v);
        else if (!u_stricmp(k, "lat")) cfg.loc_lat = atof(v);
        else if (!u_stricmp(k, "lon")) cfg.loc_lon = atof(v);
        else if (!u_stricmp(k, "alarm")) cfg.alarm_on = yes(v);
        else if (!u_stricmp(k, "alarm_time")) sscanf(v, "%d:%d", &cfg.alarm_h, &cfg.alarm_m);
        else if (!u_stricmp(k, "alarm_days")) cfg.alarm_weekdays = !u_stricmp(v, "weekdays");
        else if (!u_stricmp(k, "alarm_play")) u_copy(cfg.alarm_play, sizeof cfg.alarm_play, v);
        else if (!u_stricmp(k, "alarm_name")) u_copy(cfg.alarm_name, sizeof cfg.alarm_name, v);
        else if (!u_stricmp(k, "sort")) cfg.news_by_feed = !u_stricmp(v, "feed");
    }
    free(text);
    if (cfg.volume < 0) cfg.volume = 0;
    if (cfg.volume > 100) cfg.volume = 100;
    if (cfg.win_w < 256 || cfg.win_h < 192) { cfg.win_w = 768; cfg.win_h = 576; }
    if (!cfg.loc_ip && !cfg.loc_name[0]) cfg.loc_ip = true;
    if (cfg.alarm_h < 0 || cfg.alarm_h > 23 || cfg.alarm_m < 0 || cfg.alarm_m > 59) cfg.alarm_on = false;
}

void config_save(void)
{
    sys_mkdir(sys_config_dir());
    char buf[16384];
    static const char *scale_names[] = { "fit", "integer", "stretch" };
    int n = snprintf(buf, sizeof buf,
        "; PicoAmp for Windows - settings (this file is written when PicoAmp closes)\r\n"
        "[player]\r\n"
        "volume = %d\r\nrepeat = %s\r\nshuffle = %s\r\n"
        "; equalizer: 60 Hz, 250 Hz, 1 kHz, 4 kHz, 12 kHz in dB (-12..12)\r\n"
        "eq = %d,%d,%d,%d,%d\r\n"
        "; balance: 0 left, 50 centre, 100 right\r\n"
        "balance = %d\r\n"
        "view = %s\r\n"
        "; the folder shown as [Music] in the list\r\n"
        "music = %s\r\n"
        "folder = %s\r\n"
        "[window]\r\n"
        "window_x = %d\r\nwindow_y = %d\r\nwindow_w = %d\r\nwindow_h = %d\r\n"
        "maximized = %s\r\nfullscreen = %s\r\n"
        "; fit: as large as fits, 4:3, sharp; integer: whole multiples only; stretch: fill the window\r\n"
        "scale = %s\r\nscanlines = %s\r\non_top = %s\r\n"
        "[clock]\r\n"
        "; face: digital or analog; time: internet (the place's zone) or pc\r\n"
        "face = %s\r\ntime = %s\r\n"
        "; colours: music, cyan, green, amber, red, purple or white\r\n"
        "colours = %s\r\nspectrum = %s\r\n"
        "[location]\r\n"
        "; location: ip (where the internet connection is) or place (the one below)\r\n"
        "location = %s\r\nplace = %s\r\nlat = %.5f\r\nlon = %.5f\r\n"
        "[alarm]\r\n"
        "; alarm_days: daily or weekdays; alarm_play: a station's address or a music file\r\n"
        "alarm = %s\r\nalarm_time = %02d:%02d\r\nalarm_days = %s\r\nalarm_play = %s\r\nalarm_name = %s\r\n"
        "[news]\r\n"
        "; My feeds are in MyFeeds.opml (beside this file); sort: date (newest first) or feed\r\n"
        "sort = %s\r\n",
        cfg.volume, (cfg.mode & 1) ? "yes" : "no", (cfg.mode & 2) ? "yes" : "no",
        cfg.eq[0], cfg.eq[1], cfg.eq[2], cfg.eq[3], cfg.eq[4], cfg.balance,
        cfg.view ? "scope" : "spectrum", cfg.music_dir, cfg.start_path,
        cfg.win_x, cfg.win_y, cfg.win_w, cfg.win_h,
        cfg.maximized ? "yes" : "no", cfg.fullscreen ? "yes" : "no",
        scale_names[cfg.scale % 3], cfg.scanlines ? "yes" : "no", cfg.on_top ? "yes" : "no",
        cfg.clock_analog ? "analog" : "digital", cfg.clock_pc_time ? "pc" : "internet",
        (const char *const[]){ "music", "cyan", "green", "amber", "red", "purple", "white" }[cfg.clock_colours % 7],
        cfg.clock_spectrum ? "on" : "off",
        cfg.loc_ip ? "ip" : "place", cfg.loc_name, cfg.loc_lat, cfg.loc_lon,
        cfg.alarm_on ? "on" : "off", cfg.alarm_h, cfg.alarm_m, cfg.alarm_weekdays ? "weekdays" : "daily",
        cfg.alarm_play, cfg.alarm_name, cfg.news_by_feed ? "feed" : "date");
    if (n > 0) sys_write_file(ini_path(), buf, (size_t)n);
}
