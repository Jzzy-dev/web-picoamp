#!/usr/bin/env python3
# A stand-in for the web services PicoAmp uses, for the tests (no internet),
# with the CORS headers a browser needs (the page comes from another origin):
#   the Radio Browser directory    /json/countries, /json/stations/search, /json/url/<uuid>
#   GeoJS                          /ipapi/v1/ip/geo.json
#   Open-Meteo                     /meteo/v1/forecast, /geo/v1/search
#   streams                        /stream.mp3 (CORS), /nocors.mp3 (none), /dead.mp3 (404)
#   what a station plays           /status-json.xsl (Icecast's: /stream.mp3 plays "Unit Tester - Sine
#                                  Cafe Song"); /icy.mp3 sends ICY titles ("Artist One - Song A")
#   files                          /music/<name> (the test's music, CORS)
#   station logos                  /json/stations/byuuid/<uuid>, /json/stations/byurl; /logos/green.png
#                                  (CORS), /logos/nocors.png (none: through /proxy?url=, as wsrv.nl)
#   the relay                      /relay/?url=: Yahoo Finance (/v8/finance/spark, /rss/2.0/headline), the
#                                  news (feeds.bbci.co.uk: /feeds/rss.xml; news.example.com/articles/<n>.html)
#
#   web_server.py <port> <mp3 for the streams> <music folder>
import http.server, json, os, sys, urllib.parse, time, threading

PORT, MP3, MUSIC = int(sys.argv[1]), sys.argv[2], sys.argv[3]
STREAM = 'http://127.0.0.1:%d/stream.mp3' % PORT

def station(i, name=None, codec='MP3', hls=0):
    u = STREAM + '?s=%d' % i                # (each its own address, as real ones)
    return {"stationuuid": "uuid-%d" % i, "name": name or ("Station %d" % i), "url": u,
            "url_resolved": u, "codec": codec, "bitrate": 128, "hls": hls, "lastcheckok": 1,
            "countrycode": "NL", "tags": "jazz,pop"}

clicks = []

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, body, ctype='application/json', cors=True):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(200)
        if cors: self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Headers', 'Icy-MetaData')
        self.end_headers()

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        p = u.path
        if p == '/json/countries':
            self.reply(json.dumps([{"name": "Germany", "iso_3166_1": "DE", "stationcount": 5000},
                                   {"name": "Netherlands", "iso_3166_1": "NL", "stationcount": 1500},
                                   {"name": "Nowhere", "iso_3166_1": "XX", "stationcount": 0}]))
        elif p == '/json/stations/search':
            limit, off = int(q.get('limit', 60)), int(q.get('offset', 0))
            if 'name' in q:
                n = q['name'].lower()
                v = [station(1, "Jazz FM"), station(2, "Smooth Jazz 24"), station(3, "Jazz AAC", "AAC"), station(4, "Jazz HLS", hls=1)]
                v = [s for s in v if n in s['name'].lower()]
            elif q.get('tag') == 'jazz':
                v = [station(i, "Jazz %d" % i) for i in range(100)][off:off + limit]
            elif q.get('countrycode') == 'NL':
                v = [station(10, "NPO Radio 1"), station(11, "Radio 538")]
            else:
                v = [station(i) for i in range(5)] + [station(9, "An AAC one", "AAC")]
            self.reply(json.dumps(v))
        elif p.startswith('/json/stations/byuuid/'):
            u = p[22:]
            print("byuuid", u, flush=True)
            s = station(int(u[5:]) if u.startswith('uuid-') else 99)
            s['favicon'] = {'uuid-1': 'http://127.0.0.1:%d/logos/green.png' % PORT,
                            'uuid-2': 'http://127.0.0.1:%d/logos/nocors.png' % PORT}.get(u, '')
            self.reply(json.dumps([s]))
        elif p == '/json/stations/byurl':
            print("byurl", q.get('url'), flush=True)
            self.reply('[]')
        elif p in ('/logos/green.png', '/logos/nocors.png'):
            print("logo", p, flush=True)
            self.reply(open(os.path.join(os.path.dirname(MUSIC), 'logo.png'), 'rb').read(), 'image/png', cors=p == '/logos/green.png')
        elif p == '/proxy':
            print("proxy", q.get('url'), flush=True)
            self.reply(open(os.path.join(os.path.dirname(MUSIC), 'logo.png'), 'rb').read(), 'image/png')
        elif p == '/itunes/search':
            t, cb = q.get('term', ''), q.get('callback', '')
            print("itunes", t, flush=True)
            j = {"resultCount": 0, "results": []}
            if t.startswith('Unit Tester') or t.startswith('Artist One'):
                j = {"resultCount": 1, "results": [{"artistName": "Unit Tester", "artworkUrl100": "http://127.0.0.1:%d/art/100x100bb.jpg" % PORT}]}
            body = json.dumps(j)
            self.reply('%s(%s);' % (cb, body) if cb else body, 'text/javascript')
        elif p.startswith('/art/'):
            print("art", p, flush=True)
            self.reply(open(os.path.join(os.path.dirname(MUSIC), 'logo.png'), 'rb').read(), 'image/png')
        elif p.startswith('/json/url/'):
            clicks.append(p[10:])
            print("click", p[10:], flush=True)
            self.reply(json.dumps({"ok": True}))
        elif p == '/ipapi/v1/ip/geo.json':
            self.reply(json.dumps({"city": "Utrecht", "region": "Utrecht", "country_code": "NL", "country": "Netherlands",
                                   "latitude": "52.0908", "longitude": "5.1222", "timezone": "Europe/Amsterdam"}))
        elif p == '/geo/v1/search':
            self.reply(json.dumps({"results": [
                {"name": "Sydney", "admin1": "New South Wales", "country_code": "AU", "latitude": -33.87, "longitude": 151.21, "timezone": "Australia/Sydney"},
                {"name": "Sydney", "admin1": "Nova Scotia", "country_code": "CA", "latitude": 46.15, "longitude": -60.18, "timezone": "America/Glace_Bay"}]}))
        elif p == '/meteo/v1/forecast':
            self.reply(json.dumps({
                "utc_offset_seconds": 3600, "timezone": "Europe/Amsterdam",
                "current": {"time": "2026-11-05T18:00", "temperature_2m": 8.4, "apparent_temperature": 5.9,
                            "relative_humidity_2m": 87, "wind_speed_10m": 15.2, "weather_code": 61, "is_day": 0},
                "daily": {"time": ["2026-11-05", "2026-11-06", "2026-11-07", "2026-11-08", "2026-11-09", "2026-11-10"],
                          "weather_code": [61, 3, 0, 71, 95, 2],
                          "temperature_2m_max": [10.2, 11.0, 12.5, 3.1, 15.0, 9.9],
                          "temperature_2m_min": [4.1, 3.0, 4.4, -6.2, 10.0, 2.2],
                          "precipitation_probability_max": [80, 20, 0, 65, 100, 10]}}))
        elif p == '/relay/':                               # PicoAmp's relay (relay/worker.js), its sites faked here
            t = urllib.parse.urlparse(q.get('url', ''))
            print('relay', q.get('url', ''), flush=True)
            if not self.headers.get('Origin'):
                self.send_response(403); self.end_headers(); return
            here = {'query1.finance.yahoo.com', 'query2.finance.yahoo.com', 'feeds.finance.yahoo.com', 'news.example.com', '127.0.0.1:%d' % PORT}
            if t.netloc in here: self.path = t.path + ('?' + t.query if t.query else '')
            elif t.netloc == 'feeds.bbci.co.uk': self.path = '/feeds/rss.xml'
            else: self.send_response(404); self.send_header('Access-Control-Allow-Origin', '*'); self.end_headers(); return
            return self.do_GET()
        elif p == '/v8/finance/spark':                     # Yahoo Finance's chart service (as it answered in November 2026)
            syms = q.get('symbols', '').split(',')
            rng, out = q.get('range', '1d'), {}
            for k, sy in enumerate(syms):
                h = sum(ord(c) for c in sy)
                price = 100 + h % 900 + (h % 97) / 100.0
                prev = round(price * (1 - ((h % 7) - 3) / 100.0), 2)
                n = {'1d': 1, '5d': 46, '1mo': 24, '6mo': 133, 'ytd': 223, '1y': 256, '5y': 262, 'max': 410}.get(rng, 1)
                if q.get('interval') == '15m': n = 34
                step = {'15m': 900, '60m': 3600, '1wk': 604800, '1mo': 2629800}.get(q.get('interval'), 86400)
                end = 1794587400
                ts = [end - (n - 1 - i) * step for i in range(n)] if n > 1 else [1794572130]
                cl = [round(prev + (price - prev) * i / max(n - 1, 1) + (2 if (i // 4) % 2 else -2), 2) for i in range(n)] if n > 1 else [price]
                cl[-1] = price
                if n > 3: cl[2] = None
                out[sy] = {"timestamp": ts, "symbol": sy, "end": end if q.get('interval') == '15m' else None,
                           "start": 1794556800 if q.get('interval') == '15m' else None, "close": cl, "previousClose": None,
                           "chartPreviousClose": prev, "fulldayChange": round(price - prev, 2), "fulldayPrice": price, "dataGranularity": 300}
                if sy == 'NOPE.XX': del out[sy]
            self.reply(json.dumps(out))
        elif p == '/rss/2.0/headline':                     # Yahoo Finance's feed for symbols
            s = q.get('s', '')
            items = ''.join('<item><title>%s story %d &amp; more</title><link>https://news.example.com/articles/%d.html</link>'
                            '<description>About %s.</description><pubDate>Fri, 13 Nov 2026 1%d:00:00 +0000</pubDate></item>' % (s, i, i, s, i) for i in range(6))
            self.reply('<?xml version="1.0" encoding="UTF-8"?><rss version="2.0"><channel><title>Yahoo! Finance: %s News</title>%s</channel></rss>' % (s, items), 'application/xml')
        elif p == '/feeds/rss.xml':
            items = ''.join('<item><title>Story %d &amp; more</title><link>https://news.example.com/articles/%d.html</link>'
                            '<description>&lt;p&gt;Summary of story %d, with some words to wrap over the lines of the screen.&lt;/p&gt;</description>'
                            '<pubDate>Thu, 05 Nov 2026 %02d:%02d:00 GMT</pubDate></item>' % (i, i, i, 8 + i // 6, (i * 7) % 60) for i in range(30))
            self.reply('<?xml version="1.0"?><rss version="2.0"><channel><title>Test News</title>%s</channel></rss>' % items, 'application/rss+xml')
        elif p == '/feeds/atom.xml':
            self.reply('<?xml version="1.0"?><feed xmlns="http://www.w3.org/2005/Atom"><title>Atom Feed</title>'
                       '<entry><title>An atom story</title><link href="https://news.example.com/articles/1.html"/>'
                       '<updated>2026-11-05T09:30:00Z</updated><summary>Atom summary.</summary></entry></feed>' , 'application/atom+xml')
        elif p.startswith('/articles/'):
            n = p.split('/')[-1].split('.')[0]
            paras = ''.join('<p>Paragraph %d of the article %s. It has a few sentences, enough to fill a line or two of the reader.</p>' % (k, n) for k in range(12))
            self.reply('<html><head><title>x</title><script>var a=1;</script></head><body><nav>Menu Home News</nav>'
                       '<article><h1>Article %s</h1>%s</article><footer>Footer text</footer></body></html>' % (n, paras), 'text/html')
        elif p == '/status-json.xsl':
            print("status", flush=True)
            self.reply(json.dumps({"icestats": {"source": [
                {"listenurl": STREAM, "title": "Unit Tester - Sine Cafe Song"},
                {"listenurl": 'http://127.0.0.1:%d/other' % PORT, "title": "Someone - Else"}]}}))
        elif p == '/icy.mp3':
            data, mi = open(MP3, 'rb').read(), 4096
            meta = b"StreamTitle='Artist One - Song A';"
            meta += b'\0' * (-len(meta) % 16)
            out = bytearray()
            for i in range(0, len(data), mi):
                out += data[i:i + mi]
                out += bytes([len(meta) // 16]) + meta if self.headers.get('Icy-MetaData') == '1' else b''
            self.send_response(200)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Expose-Headers', 'icy-metaint')
            if self.headers.get('Icy-MetaData') == '1': self.send_header('icy-metaint', str(mi))
            self.send_header('Content-Type', 'audio/mpeg')
            self.send_header('Content-Length', str(len(out)))
            self.end_headers()
            try: self.wfile.write(out)
            except Exception: pass
        elif p in ('/stream.mp3', '/nocors.mp3'):    # (?s=<n>: the same)
            self.reply(open(MP3, 'rb').read(), 'audio/mpeg', cors=p == '/stream.mp3')
        elif p.startswith('/music/'):
            f = os.path.join(MUSIC, urllib.parse.unquote(p[7:]))
            if os.path.isfile(f): self.reply(open(f, 'rb').read(), 'application/octet-stream')
            else: self.send_response(404); self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()

http.server.ThreadingHTTPServer.allow_reuse_address = True
http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
