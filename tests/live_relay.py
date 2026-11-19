#!/usr/bin/env python3
# The relay for real (CI: the internet is there): the page (its own default relay) shows the markets
# (Yahoo Finance) and the news (a BBC feed and one of its stories), as a person would open them.
# Information: printed, and as one notice (GitHub's annotations), for reading without the log.
#
#   python3 tests/live_relay.py
import os, re, threading, functools, http.server, time
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
srv = http.server.ThreadingHTTPServer(('127.0.0.1', 8774), functools.partial(Quiet, directory=os.path.join(ROOT, 'web')))
threading.Thread(target=srv.serve_forever, daemon=True).start()

notes = []
with sync_playwright() as p:
    br = p.chromium.launch()
    page = br.new_page()
    page.goto('http://127.0.0.1:8774/')
    page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
    run = lambda s: page.evaluate('t => picoamp.script(t)', s)
    out = ''
    for _ in range(10):
        out = run('key Esc; key $; wait 2000; page')
        if re.search(r'state=[23]', out): break
        run('key Esc')
    print(out[:1500])
    m = re.search(r"state=(\d)(?: .*err='([^']*)')?", out)
    prices = len(re.findall(r'\n  \S+ .* [1-9][\d.]* -?\d', out))
    notes.append('markets: %s (%d prices)' % ('ok' if m and m.group(1) == '2' else 'failed: ' + (m.group(2) if m and m.group(2) else out[:120]), prices))
    out = run('key Down; key Enter; wait 4000; page')
    print(out[:800])
    c = re.search(r'chart=(\d) points=(\d+)', out)
    notes.append('chart: %s' % ('%s points' % c.group(2) if c and c.group(1) == '2' else 'failed'))
    run('key Esc; key Esc')
    out = run('key T; wait 100; key A; type https://feeds.bbci.co.uk/news/rss.xml; key Enter; wait 4000; page; key End; key Enter; wait 4000; page; key Down; key Enter; key Enter; wait 6000; page')
    print(out[-2500:])
    f = re.findall(r"news headlines feed='([^']*)' state=(\d) stories=(\d+)", out)
    a = re.search(r'news story .*article=(\d) lines=(\d+)', out)
    notes.append('news: %s' % ('%s, %s stories' % (f[-1][0], f[-1][2]) if f and f[-1][1] == '2' else 'failed'))
    notes.append('a story: %s' % ('%s lines' % a.group(2) if a and a.group(1) == '2' else 'failed'))
    br.close()
srv.shutdown()
msg = 'The relay, live: ' + '; '.join(notes)
print(msg)
print('::notice title=The relay, live::' + msg)
