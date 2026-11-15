#!/usr/bin/env python3
# Which now-playing services answer a page on another site (CORS), from CI (information only).
import urllib.request, json
urls = [
    'https://somafm.com/songs/groovesalad.json',
    'https://api.somafm.com/songs/groovesalad.json',
    'https://somafm.com/channels.json',
    'https://api.radioparadise.com/api/now_playing?chan=0',
    'https://api.laut.fm/station/mangoradio/current_song',
    'https://api.qmusic.nl/2.4/tracks/plays?limit=1&next=false',
    'https://api.qmusic.nl/2.4/app/qmusic_nl/tracks/plays?limit=1',
    'https://api.zeno.fm/mounts/metadata/subscribe/6n6ewddtad0uv',
    'https://ice6.somafm.com/status-json.xsl',
    'https://icecast-qmusicnl-cdp.triple-it.nl/status-json.xsl',
    'https://liveradio.swr.de/sw282p3/swr3/status-json.xsl',
    'https://api.allorigins.win/raw?url=' + urllib.parse.quote('https://icecast.omroep.nl/status-json.xsl', safe=''),
    'https://corsproxy.io/?url=' + urllib.parse.quote('https://icecast.omroep.nl/status-json.xsl', safe=''),
    'https://api.codetabs.com/v1/proxy?quest=' + urllib.parse.quote('https://icecast.omroep.nl/status-json.xsl', safe=''),
    'https://www.radioparadise.com/api/now_playing?chan=0',
    'https://feed.tunein.com/profiles/s6707/nowPlaying',
    'https://onlineradiobox.com/json/nl/qmusic/play',
    'https://scraper2.onlineradiobox.com/nl.qmusic',
]
import urllib.parse
out = []
for u in urls:
    try:
        r = urllib.request.urlopen(urllib.request.Request(u, headers={'Origin': 'https://jzzy-dev.github.io', 'User-Agent': 'Mozilla/5.0'}), timeout=12)
        b = r.read(400)
        out.append('%s -> %d ACAO=%s %s' % (u[:90], r.status, r.headers.get('Access-Control-Allow-Origin'), b[:160].decode('utf-8', 'replace').replace('\n', ' ')))
    except Exception as e:
        out.append('%s -> %s' % (u[:90], str(e)[:80]))
body = '\n'.join(out)
print(body)
print('::notice title=Now-playing services::' + body.replace('%', '%25').replace('\r', '').replace('\n', '%0A'))
