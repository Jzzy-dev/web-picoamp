#!/usr/bin/env python3
# A stand-in for the web services PicoAmp uses, for the tests (no internet),
# with the CORS headers a browser needs (the page comes from another origin):
#   the Radio Browser directory    /json/countries, /json/stations/search, /json/url/<uuid>
#   GeoJS                          /ipapi/v1/ip/geo.json
#   Open-Meteo                     /meteo/v1/forecast, /geo/v1/search
#   streams                        /stream.mp3 (CORS), /nocors.mp3 (none), /dead.mp3 (404)
#   files                          /music/<name> (the test's music, CORS)
#   station logos                  /json/stations/byuuid/<uuid>, /json/stations/byurl; /logos/green.png
#                                  (CORS), /logos/nocors.png (none: through /proxy?url=, as wsrv.nl)
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
