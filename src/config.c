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
    cfg.cover_rows = true;
    cfg.keep_logos = 100;
    cfg.keep_covers = 200;
    cfg.pic_interval = 10;
    cfg.mk_refresh = 60;
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
        else if (!u_stricmp(k, "view")) cfg.view = !u_stricmp(v, "scope") ? 1 : !u_stricmp(v, "cover") ? 2 : 0;
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
        else if (!u_stricmp(k, "rows")) cfg.cover_rows = yes(v);
        else if (!u_stricmp(k, "sharp")) cfg.cover_sharp = yes(v);
        else if (!u_stricmp(k, "region"))
        {
            for (int i = 0; i < 3; i++) if (!u_stricmp(v, (const char *const[]){ "europe", "americas", "asia" }[i])) cfg.mk_region = i;
        }
        else if (!u_stricmp(k, "order")) cfg.mk_sort = !u_stricmp(v, "risers") ? 1 : !u_stricmp(v, "fallers") ? 2 : 0;
        else if (!u_stricmp(k, "range"))
        {
            for (int i = 0; i < 8; i++) if (!u_stricmp(v, (const char *const[]){ "1d", "5d", "1m", "6m", "ytd", "1y", "5y", "all" }[i])) cfg.mk_range = i;
        }
        else if (!u_stricmp(k, "refresh")) cfg.mk_refresh = atoi(v);
        else if (!u_stricmp(k, "list")) u_copy(cfg.mk_list, sizeof cfg.mk_list, v);
        else if (!u_stricmp(k, "keeplogos")) cfg.keep_logos = atoi(v);
        else if (!u_stricmp(k, "keepcovers")) cfg.keep_covers = atoi(v);
        else if (!u_stricmp(k, "saver")) cfg.pic_saver = atoi(v);
        else if (!u_stricmp(k, "interval")) cfg.pic_interval = atoi(v);
        else if (!u_stricmp(k, "pictures_shuffle")) cfg.pic_shuffle = yes(v);
        else if (!u_stricmp(k, "pictures")) u_copy(cfg.pic_folder, sizeof cfg.pic_folder, v);
    }
    free(text);
    if (cfg.volume < 0) cfg.volume = 0;
    if (cfg.volume > 100) cfg.volume = 100;
    if (cfg.win_w < 256 || cfg.win_h < 192) { cfg.win_w = 768; cfg.win_h = 576; }
    if (!cfg.loc_ip && !cfg.loc_name[0]) cfg.loc_ip = true;
    if (cfg.pic_saver < 0 || cfg.pic_saver > 240) cfg.pic_saver = 0;
    if (cfg.pic_interval < 2 || cfg.pic_interval > 3600) cfg.pic_interval = 10;
    if (cfg.mk_refresh < 20) cfg.mk_refresh = 60;
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
        "sort = %s\r\n"
        "[markets]\r\n"
        "; region: europe, americas or asia; order: list, risers or fallers; range: the chart's (1d 5d 1m 6m ytd 1y 5y all);\r\n"
        "; refresh: seconds between the prices; list: My list, Yahoo Finance symbols with a comma between them\r\n"
        "region = %s\r\norder = %s\r\nrange = %s\r\nrefresh = %d\r\nlist = %s\r\n"
        "[cover]\r\n"
        "; rows: five colours of its own for every row of the cover's cells (as the MSX); off: five for all of it\r\n"
        "rows = %s\r\n"
        "; sharp: the cover in its own resolution over the MSX's (F5); off: as the MSX shows it (8-bit)\r\n"
        "sharp = %s\r\n"
        "; keeplogos, keepcovers: how many are kept (the ones shown longest ago go; 0: all)\r\n"
        "keeplogos = %d\r\nkeepcovers = %d\r\n"
        "[pictures]\r\n"
        "; saver: the slideshow after this many minutes without a key (0: off); interval: seconds a picture;\r\n"
        "; pictures: the screensaver's folder (empty: the Pictures folder)\r\n"
        "saver = %d\r\ninterval = %d\r\npictures_shuffle = %s\r\npictures = %s\r\n",
        cfg.volume, (cfg.mode & 1) ? "yes" : "no", (cfg.mode & 2) ? "yes" : "no",
        cfg.eq[0], cfg.eq[1], cfg.eq[2], cfg.eq[3], cfg.eq[4], cfg.balance,
        cfg.view == 1 ? "scope" : cfg.view == 2 ? "cover" : "spectrum", cfg.music_dir, cfg.start_path,
        cfg.win_x, cfg.win_y, cfg.win_w, cfg.win_h,
        cfg.maximized ? "yes" : "no", cfg.fullscreen ? "yes" : "no",
        scale_names[cfg.scale % 3], cfg.scanlines ? "yes" : "no", cfg.on_top ? "yes" : "no",
        cfg.clock_analog ? "analog" : "digital", cfg.clock_pc_time ? "pc" : "internet",
        (const char *const[]){ "music", "cyan", "green", "amber", "red", "purple", "white" }[cfg.clock_colours % 7],
        cfg.clock_spectrum ? "on" : "off",
        cfg.loc_ip ? "ip" : "place", cfg.loc_name, cfg.loc_lat, cfg.loc_lon,
        cfg.alarm_on ? "on" : "off", cfg.alarm_h, cfg.alarm_m, cfg.alarm_weekdays ? "weekdays" : "daily",
        cfg.alarm_play, cfg.alarm_name, cfg.news_by_feed ? "feed" : "date",
        (const char *const[]){ "europe", "americas", "asia" }[cfg.mk_region % 3], (const char *const[]){ "list", "risers", "fallers" }[cfg.mk_sort % 3],
        (const char *const[]){ "1d", "5d", "1m", "6m", "ytd", "1y", "5y", "all" }[cfg.mk_range % 8], cfg.mk_refresh, cfg.mk_list,
        cfg.cover_rows ? "on" : "off", cfg.cover_sharp ? "on" : "off", cfg.keep_logos, cfg.keep_covers, cfg.pic_saver, cfg.pic_interval, cfg.pic_shuffle ? "yes" : "no", cfg.pic_folder);
    if (n > 0) sys_write_file(ini_path(), buf, (size_t)n);
}
