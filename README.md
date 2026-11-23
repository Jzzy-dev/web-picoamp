# PicoAmp for the web

[PICOAMP](https://github.com/Jzzy-dev/msx-picoplay-picoamp), the MSX2 music and internet radio player
for the PicoVerse 2350 cartridge, in the browser. It is the same program as
[PicoAmp for Windows](https://github.com/Jzzy-dev/win-picoamp): the C code that draws every page cell for
cell as on the MSX is compiled to WebAssembly, and the browser does the sound, the files and the web.

**Open it:** <https://jzzy-dev.github.io/web-picoamp/>. There is nothing to install, so there is no
SmartScreen, no admin rights and nothing for IT to approve. It runs in a recent Chrome, Edge, Firefox
or Safari, on a PC, a Mac, a Chromebook, Linux or a tablet.

| Playing | Internet radio: search | Equalizer (G) |
|---|---|---|
| ![Playing](docs/spectrum.png) | ![Search](docs/directory.png) | ![Equalizer](docs/equalizer.png) |
| **Clock (C)** | **Weather (W)** | **A picture (I)** |
| ![Clock](docs/clock.png) | ![Weather](docs/weather.png) | ![A picture](docs/picture.png) |

*(The screenshots come from the tests, with their test music, stations and places.)*

## Spotify, other players and Sonos

* **Sonos:** the Sonos entry at the top of the list makes this page the screen and remote of a
  room of the house (song, time, cover, the keys and the volume), through
  [PicoAmp for Windows](https://github.com/Jzzy-dev/win-picoamp) on a PC in the same network.
  On a phone or a tablet, open `http://<the PC>:8765/web/`. See Sonos under What it does.
* **Spotify and other players** of a PC are followed by PicoAmp for Windows (through Windows's
  media controls; a web page cannot see other programs) and by PicoAmp on the MSX through its
  MSX link; this page does not follow them (yet).

## What it does

* **Your own music:** an entry at the top picks a folder (Chrome and Edge remember it between visits;
  other browsers ask again). Its folders and files are the list, with the title and artist from the
  ID3 tags. Plays MP3, WAV, FLAC, AAC/M4A, Ogg and Opus, whatever the browser itself plays.
  You can also drop music files on the page.
* **Internet radio:** your lists, Favourites, and the free [Radio Browser](https://www.radio-browser.info)
  directory (Top stations, Most voted, Trending now, By country, By genre, By decade, and `/` search).
  The directory only shows stations in codecs this browser plays. N and P go through the list's stations.
* **The picture:** the 24-band spectrum analyser, VU meters, oscilloscope (V), equalizer and balance (G),
  presets (E) and the full-screen pictures (Z). The analyser and equalizer are the PicoVerse Audio
  Engine's code; the equalizer's five bands are the browser's own filters.
* **The cover (V a third time),** as PICOAMP on the MSX shows it: 80×80 pixels in MSX SCREEN 4 cells
  (two colours in every 8 pixels of a row) with five palette entries of its own for each row of cells
  (as the MSX with engine 0.17; `rows = off` in the settings: five for all of it), 12 bars beside it.
  **Z** then **Cover** shows it big in MSX SCREEN 8 (256 × 192, 256 colours, dithered), as the MSX
  does: the track's own picture, the folder's, the one from the internet (iTunes covers at 600 × 600),
  the default pictures whole.
  It comes from the track's tag (MP3, FLAC), else from `cover`, `folder`, `front`, `album`,
  `albumart` or `AlbumArtSmall` (`.jpg`, `.jpeg`, `.png`) in its folder.
  A station shows its logo, found as the MSX engine finds it. Radio Browser is asked for the station
  by its directory id, else by its address, else by its exact name. The station's `favicon` is kept
  in the browser's cache, and "no logo" is kept too, so the next start asks nothing. Most logo
  servers do not let other pages read their pictures (CORS), and a page on https cannot fetch from
  http. Those logos come through the public image proxy [wsrv.nl](https://wsrv.nl/), which gets the
  logo's address and nothing else.
  The song that plays on a station, and a file without a picture (by its tags: artist and album,
  else artist and title), get their cover from the iTunes search, kept in the browser's cache too.
  A song's cover shows over the station's logo; a song without one shows the logo again. When
  nothing has a cover, one of three default pictures shows (Copacabana, New York, Amsterdam), a new
  one at random for each track or station.
  How many are kept: at most 100 logos and 200 covers. Past that, the ones shown longest ago go
  (down to 90 %). A "no logo" or "no cover" is asked again after 30 days. Other limits go in the
  address: `?keeplogos=50&keepcovers=400` (0: keep all).
* **Pictures (I):** pick a folder of pictures (Chrome and Edge remember it, as the music folder); its
  folders and pictures (anything the browser shows: JPEG, PNG, GIF, WebP, AVIF ...).
  * Enter shows one in MSX SCREEN 8 (256 × 192, 256 colours, fitted with the MSX's dot shape and
    dithered); Left/Right the one before or after, Space marks it, any other key goes back.
  * Space marks pictures (A all); **G** plays the marked ones, or all of the folder, as a slideshow.
  * **O** sets the screensaver (off, 1, 2, 5, 10, 15 or 30 minutes), **R** how long a picture shows
    (5 to 60 s), **U** shuffle, **F** makes this folder the screensaver's. "Choose another folder..." at
    the top picks another one.
  * The screensaver plays that folder's slideshow after the minutes without a key or a click, while the
    player shows; any key goes back. It needs the folder: in Chrome and Edge after a reload too (once
    the browser may read it again), in other browsers once it was picked in this visit.
* **Clock (C)** with sleep timer and alarm, and **Weather (W)** for the place of your internet connection
  ([GeoJS](https://www.geojs.io)) or a place found by name, from [Open-Meteo](https://open-meteo.com).
  * The alarm and sleep timer only work while the page is open (in a background tab is fine).
* **Lyrics (L)**, as in PicoAmp for Windows: the words of what plays, from [LRCLIB](https://lrclib.net) (free,
  no account): synced for many songs (the line sung now in the middle; Enter: that line is now, [ ]
  half a second either way, Up/Down scroll, R looks again), else plain. A file's by its artist, title,
  album and length; a station's song by its artist and title. Straight from LRCLIB, else through
  [the relay](#the-relay). Kept in memory while the page is open.
* **Markets ($)** and **News (T)**, as in PicoAmp for Windows: the market monitor (indices, currencies,
  commodities, crypto and My list from Yahoo Finance, delayed; a chart per instrument; its headlines)
  and the news reader (feeds by topic, My feeds, the full story). Both need [the relay](#the-relay):
  their sites do not let other pages read them. Without it they say so.
  * A feed list (OPML) dropped on the page goes into My feeds.
* **Sonos:** **Sonos** at the top of the list shows the rooms of the house and makes PicoAmp the screen
  and remote of one: its song, time and cover, play / pause, next, previous, seeking, shuffle, repeat,
  the volume and mute. The sound stays on the speakers (the bars move on a pretend sound).
  * A web page cannot reach the speakers itself (it cannot search the network, and a page on https may
    not ask an http address there), so [PicoAmp for Windows](https://github.com/Jzzy-dev/win-picoamp)
    on a PC in the same network asks them for it: turn on its MSX link (**K**) there.
  * On that PC: this page finds it at `http://localhost:8765` (Chrome asks once whether the page may
    reach apps on this device).
  * On a phone or a tablet: open `http://<the PC>:8765/web/`. The PC serves this page itself, and Sonos
    goes through it. (A page on http cannot pick a music folder; the radio and Sonos work.)
  * Elsewhere: `?bridge=http://<the PC>:8765` in the address (kept; `?bridge=off` forgets it).
* **Media keys and the lock screen:** play/pause, next and previous work through the browser.
* **Any window size:** fit at 4:3 and sharp (whole-pixel enlargement, only the last step smoothed).
  F2 switches to whole multiples or stretch, F3 adds scan lines, and F11, Alt+Enter or a double click
  beside the picture goes full screen. **F5: sharp covers**: the cover in its own resolution (the cover
  view and full screen) instead of the MSX's 8-bit colours; F5 again goes back (kept in the browser). **?** or **F1** (or the **?** box at the top right) shows all the keys.
* **The mouse:** click to choose, click again to play, right button goes back. The wheel scrolls the list; over **VOL** it sets the volume.
* **Your lists:** **O** opens an editor for the station lists. You can create, rename, delete and edit
  lists as `.m3u` text, and import or export `.m3u`/`.pls` files. You can also drop an `.m3u`/`.pls` file
  on the page. F adds a station to Favourites, D removes it, and Ctrl+Up/Down moves it.
  * The settings and lists stay in this browser (its local storage). Export a list to keep a copy.

## What the browser does not allow (yet)

A web page may only read another site when that site allows it (CORS). This has three effects:

* **Stations without CORS** still play, but as they are, without spectrum or equalizer. PicoAmp says so
  when it happens. Most stations (Icecast, SomaFM, Radio Paradise and others) do allow it.
* **http-only stations** cannot be played from an https page. PicoAmp says so.
* **Song titles of stations** (ICY) are inside the stream, which a page may not read. PicoAmp asks
  for them beside it, every 20 s, in the first way that answers: the station's own service (Triton
  Digital's for StreamTheWorld stations such as 538, Radio 10, Veronica; SomaFM; laut.fm; Zeno), its
  server's status page (Icecast, SHOUTcast), the stream's metadata where the server allows it (the NPO
  stations, many Icecast stations), and last [onlineradiobox.com](https://onlineradiobox.com)'s now
  playing, found by the station's name and country (Qmusic, Sky Radio and most big stations). In a live
  check of the 53 most listened https stations, 32 tell their song this way; the rest show their name.
* **The markets and the news** (Yahoo Finance, the feeds and the stories' pages) come through
  [the relay](#the-relay).

With the relay the song titles ask the stations' status pages through it too, so more stations show
their song.

## The relay

`relay/worker.js` is a [Cloudflare Worker](https://workers.cloudflare.com) (free) that fetches a page for
PicoAmp and hands it on with CORS allowed. It answers PicoAmp's own page only (jzzy-dev.github.io and
localhost; add others in the Worker's `ORIGINS` variable, spaces between them), only http(s) to public
hosts, only text (feeds, pages, JSON: no audio or video), up to 4 MB. On Cloudflare's free plan it cannot
cost anything: past 100,000 requests a day it stops answering until the next day.

1. In the Cloudflare dashboard: **Workers & Pages → Create → Create Worker**, name it (for example
   `picoamp-relay`), **Deploy**.
2. **Edit code**: replace it all with `relay/worker.js`, **Deploy**.
3. Open PicoAmp once with `?relay=https://picoamp-relay.<you>.workers.dev/`. The browser keeps it.
   `?relay=off` uses none, `?relay=default` goes back to the built-in one.

`RELAY_DEFAULT` at the top of `web/picoamp.js` is the built-in relay (PicoAmp's own, at
`picoamp-relay.jzzycool.workers.dev`): the page on GitHub Pages uses it unless told otherwise.
`node tests/relay_test.mjs` tests the Worker against pretend sites.

## Build and run

You need [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) 25 (clang for WebAssembly) and Python 3.

```sh
make WASI_SDK=/path/to/wasi-sdk     # web/picoamp.wasm
make serve                          # http://localhost:8000
make test                           # the tests in headless Chromium (needs ffmpeg and playwright)
```

* `web/` is the whole site: `index.html`, `picoamp.js` (the page: picture, keys, sound, storage, files),
  `style.css` and `picoamp.wasm`.
* `src/` is the C program. Most of it is shared with PicoAmp for Windows. `web.c` is its link to the
  page, `audio_web.c` the player, `sonos_web.c` Sonos through PicoAmp for Windows. `mktsvc.c` and `newsfeed.c` fetch in a thread on Windows and through
  the relay here (`__wasm__`).
* The services' addresses can be changed for tests: `?rb=…&meteo=…&geo=…&ipapi=…`.

**GitHub Pages:** the workflow builds, tests and publishes `web/`. In the repository's
Settings → Pages, set Source to *GitHub Actions* once.

## Licence

As PICOAMP and PicoAmp for Windows: see [LICENSE.txt](LICENSE.txt). The font comes from C-BIOS
(`third_party/cbios`, with its own licence).
