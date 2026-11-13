#!/usr/bin/env python3
# The page against the real services (CI: the internet is there): the Radio
# Browser directory, GeoJS, Open-Meteo and a real station, all from the
# browser (so their CORS headers are tested too).
#
#   python3 tests/live_web.py
import os, re, sys, threading, functools, http.server, time
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
srv = http.server.ThreadingHTTPServer(('127.0.0.1', 8772), functools.partial(Quiet, directory=os.path.join(ROOT, 'web')))
threading.Thread(target=srv.serve_forever, daemon=True).start()

fails = 0
def check(name, out, pattern):
    global fails
    if re.search(pattern, out): print('ok   ' + name)
    else: fails += 1; print('FAIL %s: no /%s/ in\n%s' % (name, pattern, '\n'.join('     | ' + l for l in out.splitlines()[-20:])))

with sync_playwright() as pw:
    br = pw.chromium.launch(args=['--autoplay-policy=no-user-gesture-required'])
    page = br.new_page(viewport={'width': 1024, 'height': 768})
    page.on('console', lambda m: print('     console:', m.text) if m.type == 'error' else None)
    page.goto('http://127.0.0.1:8772/')
    page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
    run = lambda s: page.evaluate('t => picoamp.script(t)', s)

    # a station of the default list (SomaFM, https): through Web Audio, the spectrum moves
    out = run('key Home; key Enter; wait 100; key End; key Enter; wait 100; key Home; key Down; key Down; key Enter; wait 6000; status')
    check('a real station plays', out, r"state=1 .*stream=1 buf=0")
    a = page.evaluate('picoamp.audio')
    print('     through Web Audio:', not a['cur']['usingPlain'] if a['cur'] else None)
    page.screenshot(path='live-radio.png')

    # the directory
    out = run('key BS; key Home; key Down; key Down; key Enter; wait 6000; list')
    check('Radio Browser: top stations', out, r'list /Internet radio/Top stations \([1-9]\d+\)')
    out = run('key Down; key Enter; wait 6000; status')
    check('Radio Browser: a station plays', out, r'state=1 .*stream=1')
    out = run('key /; type jazz; key Enter; wait 6000; list')
    check('Radio Browser: search', out, r'list /Internet radio/Search/jazz \([1-9]\d*\)')

    # the clock's place (GeoJS) and the weather (Open-Meteo)
    out = run('key C; wait 6000; page; key W; wait 500; page')
    check('GeoJS: a place', out, r"place='[^']+'")
    check('Open-Meteo: the weather', out, r"weather='-?\d+\.\d C ")
    check('Open-Meteo: the days', out, r'\d{4}-\d\d-\d\d \w{3} code=\d+')
    out = run('key C; key L; type Amsterdam; key Enter; wait 4000; page')
    check('Open-Meteo: a place by name', out, r'Amsterdam')
    page.screenshot(path='live-clock.png')
    br.close()
srv.shutdown()
print('all live checks passed' if not fails else '%d live checks FAILED' % fails)
sys.exit(1 if fails else 0)
