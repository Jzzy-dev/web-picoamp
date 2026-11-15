#!/usr/bin/env python3
# What real stations play, read as the page reads it (CI: the internet is there): the most
# listened https stations of Radio Browser (worldwide and in the Netherlands), each asked in every
# way the page knows (picoamp.npProbe). Information: printed, and as one notice (GitHub's
# annotations), for reading without the log.
#
#   python3 tests/live_np.py
import os, sys, json, threading, functools, http.server, urllib.request
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
srv = http.server.ThreadingHTTPServer(('127.0.0.1', 8773), functools.partial(Quiet, directory=os.path.join(ROOT, 'web')))
threading.Thread(target=srv.serve_forever, daemon=True).start()

def rb(path):
    r = urllib.request.urlopen(urllib.request.Request('https://de1.api.radio-browser.info/json/' + path, headers={'User-Agent': 'PicoAmp-live-check'}), timeout=20)
    return json.loads(r.read())
st = []
seen = set()
for path in ['stations/search?countrycode=NL&order=clickcount&reverse=true&limit=40&hidebroken=true',
             'stations/topclick/60']:
    for s in rb(path):
        u = s.get('url_resolved') or s.get('url')
        if not u.startswith('https://') or u in seen or s.get('hls'): continue
        seen.add(u)
        st.append((s['name'].strip()[:30], u))
st = st[:70]

with sync_playwright() as pw:
    br = pw.chromium.launch()
    page = br.new_page()
    page.goto('http://127.0.0.1:8773/')
    page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
    res = page.evaluate('''async list => {
        const out = await Promise.all(list.map(async ([n, u]) => {
            const t = await Promise.race([picoamp.npProbe(u), new Promise(ok => setTimeout(() => ok('(too slow)'), 90000))]);
            return n + ' | ' + u + ' | ' + t;
        }));
        return out; }''', st)
    br.close()
srv.shutdown()

ways = {}
found = 0
for l in res:
    print(l)
    got = [w for w in l.split(' | ')[-1].split(' ') if '=' in w and not w.endswith('=-')]
    if got: found += 1
    for w in got: ways[w.split('=')[0]] = ways.get(w.split('=')[0], 0) + 1
summary = '%d of %d stations told what they play. Ways: %s' % (found, len(res), ', '.join('%s %d' % kv for kv in sorted(ways.items(), key=lambda x: -x[1])))
print(summary)
body = summary + '\n' + '\n'.join(res)
print('::notice title=What stations play::' + body.replace('%', '%25').replace('\r', '').replace('\n', '%0A'))
