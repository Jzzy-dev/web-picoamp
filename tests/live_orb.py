#!/usr/bin/env python3
# Experiment (information only): onlineradiobox's now-playing for stations, by aliases made from
# Radio Browser's name and country.
import urllib.request, urllib.parse, json, re, unicodedata
def get(u, t=10):
    r = urllib.request.urlopen(urllib.request.Request(u, headers={'Origin': 'https://jzzy-dev.github.io', 'User-Agent': 'Mozilla/5.0'}), timeout=t)
    return r.read().decode('utf-8', 'replace'), r.headers.get('Access-Control-Allow-Origin')
def rbq(path):
    return json.loads(get('https://de1.api.radio-browser.info/json/' + path, 20)[0])
def slugs(name):
    n = unicodedata.normalize('NFKD', name).encode('ascii', 'ignore').decode().lower()
    n = re.sub(r'\(.*?\)|\[.*?\]|\|.*$| - .*$', '', n).strip()
    a = re.sub(r'[^a-z0-9]', '', n)
    w = re.findall(r'[a-z0-9]+', n)
    c = [a, '-'.join(w), re.sub(r'^radio', '', a), re.sub(r'radio$', '', a), w[0] if w else '', ''.join(x for x in w if x not in ('radio', 'fm', 'hd', 'mp3', 'aac', 'live', 'stream'))]
    out = []
    for x in c:
        if x and x not in out: out.append(x)
    return out
st = []
seen = set()
for path in ['stations/search?countrycode=NL&order=clickcount&reverse=true&limit=40&hidebroken=true', 'stations/topclick/60']:
    for s in rbq(path):
        u = s.get('url_resolved') or s.get('url')
        if not u.startswith('https://') or u in seen: continue
        seen.add(u); st.append(s)
res = []
hits = 0
for s in st[:70]:
    cc = (s.get('countrycode') or '').lower()
    found = None
    for sl in slugs(s['name']):
        try:
            t, acao = get('https://scraper2.onlineradiobox.com/%s.%s' % (cc, sl), 6)
            j = json.loads(t)
            if j.get('title') or j.get('iName'):
                found = '%s.%s=%s (ACAO %s)' % (cc, sl, (j.get('title') or '')[:50], acao); break
        except Exception as e: pass
    hits += bool(found)
    res.append('%s [%s]: %s' % (s['name'][:30], cc, found or '-'))
body = 'onlineradiobox by alias: %d of %d\n' % (hits, len(res)) + '\n'.join(res)
print(body)
print('::notice title=onlineradiobox::' + body.replace('%', '%25').replace('\n', '%0A'))
