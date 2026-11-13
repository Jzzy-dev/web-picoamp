#!/usr/bin/env python3
# The tests of PicoAmp for the web: the page in headless Chromium (Playwright),
# the web services and streams faked by tests/web_server.py on another
# origin (so CORS is tested too). Needs ffmpeg and playwright (with Chromium).
#
#   python3 tests/run_web.py [--shots DIR]
import os, re, subprocess, sys, tempfile, time, threading, functools, http.server
from playwright.sync_api import sync_playwright

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE_PORT, SVC_PORT = 8770, 8771
SVC = 'http://127.0.0.1:%d' % SVC_PORT
shots = sys.argv[sys.argv.index('--shots') + 1] if '--shots' in sys.argv else None
if shots: os.makedirs(shots, exist_ok=True)

T = tempfile.mkdtemp(prefix='picoamp-web-')
M = os.path.join(T, 'Music')
os.makedirs(os.path.join(M, 'Album'))
E = '0.3*sin(2*PI*440*t)+0.2*sin(2*PI*3000*t)'
def ff(*a): subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', *a], check=True)
ff('-f', 'lavfi', '-i', "aevalsrc='%s|%s*0.8':s=44100:d=30" % (E, E), '-c:a', 'libmp3lame', '-b:a', '192k',
   '-metadata', 'title=Sine Café Song', '-metadata', 'artist=Unit Tester', '-id3v2_version', '3', os.path.join(M, '01 Sine.mp3'))
ff('-f', 'lavfi', '-i', "aevalsrc='%s':s=22050:d=12" % E, '-ac', '1', '-c:a', 'libmp3lame', '-b:a', '64k',
   '-metadata', 'title=A very long title that is much longer than its place on the screen', os.path.join(M, '02 Long.mp3'))
ff('-f', 'lavfi', '-i', "aevalsrc='%s|%s':s=48000:d=4" % (E, E), os.path.join(M, 'Album', 'Demo.wav'))

# the page (one origin) and the services (another)
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()
http.server.ThreadingHTTPServer.allow_reuse_address = True
page_srv = http.server.ThreadingHTTPServer(('127.0.0.1', PAGE_PORT), functools.partial(Quiet, directory=os.path.join(ROOT, 'web')))
threading.Thread(target=page_srv.serve_forever, daemon=True).start()
log = open(os.path.join(T, 'svc.log'), 'w')
svc = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tests', 'web_server.py'), str(SVC_PORT), os.path.join(M, '01 Sine.mp3'), M], stdout=log, stderr=log)
time.sleep(0.6)

URL = 'http://127.0.0.1:%d/?rb=%s&meteo=%s/meteo&geo=%s/geo&ipapi=%s/ipapi' % (PAGE_PORT, SVC, SVC, SVC, SVC)
fails = 0
def check(name, out, pattern):
    global fails
    if re.search(pattern, out): print('ok   ' + name)
    else:
        fails += 1
        print('FAIL %s: no /%s/ in\n%s' % (name, pattern, '\n'.join('     | ' + l for l in out.splitlines()[-25:])))
def check_not(name, out, pattern):
    global fails
    if re.search(pattern, out): fails += 1; print('FAIL ' + name)
    else: print('ok   ' + name)

try:
  with sync_playwright() as pw:
      br = pw.chromium.launch(args=['--autoplay-policy=no-user-gesture-required', '--use-fake-ui-for-media-stream'])
      ctx = br.new_context(viewport={'width': 1024, 'height': 768})
      # the folder picker of the tests: the <input webkitdirectory> one
      ctx.add_init_script('delete window.showDirectoryPicker;')
      page = ctx.new_page()
      errors = []
      page.on('pageerror', lambda e: errors.append(str(e)))
      page.on('console', lambda m: errors.append(m.text) if m.type == 'error' else None)

      def load(ini_changes=None, lists=None):
          page.goto(URL)
          page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
          if ini_changes or lists:
              page.evaluate('picoamp.save(); picoamp.stopSaving()')
              ini = page.evaluate('picoamp.fs.get("/PicoAmp/picoamp.ini")') or ''
              for k, v in (ini_changes or {}).items():
                  ini = re.sub(r'(?m)^%s = .*$' % re.escape(k), '%s = %s' % (k, v), ini)
              page.evaluate('([a, b]) => picoamp.fs.set(a, b)', ['/PicoAmp/picoamp.ini', ini])
              for name, text in (lists or {}).items():
                  page.evaluate('([a, b]) => picoamp.fs.set(a, b)', ['/PicoAmp/Radio/' + name, text])
              page.goto(URL)
              page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
      def run(script):
          return page.evaluate('t => picoamp.script(t)', script)
      def shot(name):
          page.evaluate('picoamp.hideHint()')
          if shots: page.screenshot(path=os.path.join(shots, name + '.png'))

      # the first start: the top of the list, the station lists written
      load()
      out = run('wait 300; key Home; list; key Enter; wait 100; list')
      shot('start')
      check('the page starts (wasm)', out, r'list / \(2\)')
      check('top: internet radio', out, r'1 Internet radio')
      check('top: choose a music folder', out, r'1 Choose a music folder')
      check('internet radio: the lists', out, r'list /Internet radio \(\d+\).*[\s\S]*1 Favourites[\s\S]*1 Top stations[\s\S]*1 Stations')
      page.evaluate('() => window.dispatchEvent(new Event("pagehide"))')
      ini = page.evaluate('picoamp.fs.get("/PicoAmp/picoamp.ini")') or ''
      check('settings kept in the browser', ini, r'\[player\]')
      check('default stations over https', page.evaluate('picoamp.fs.get("/PicoAmp/Radio/Stations.m3u")') or '', r'https://ice1\.somafm\.com/groovesalad')

      # the music folder: picked (the keys of a real keyboard: the browser asks for a gesture)
      run('key BS; key Home; key Down')
      with page.expect_file_chooser() as fc:
          page.keyboard.press('Enter')
      fc.value.set_files(M)
      time.sleep(0.6)
      out = run('wait 200; list')
      check('music folder: its files', out, r'list /Music \(4\)[\s\S]*1 Album[\s\S]*2 01 Sine\.mp3[\s\S]*2 02 Long\.mp3')
      out = run('key Home; key Down; key Down; key Enter; wait 2500; status; key .; wait 400; status; key N; wait 1500; status')
      shot('playing')
      check('mp3 tags (ID3)', out, r"title='Sine Café Song' artist='Unit Tester'")
      check('a file plays', out, r'state=1 elapsed=[12]/30 rate=44100 ch=2 br=19\d')
      check('seek +10', out, r'elapsed=1[1-4]/30')
      check('next track', out, r"pl=2/2 .*title='A very long title")
      check('mono 22 kHz', out, r'rate=22050 ch=1 br=6\d')
      a = page.evaluate('picoamp.audio')
      check('through Web Audio', str(a.get('ctx')), 'running')
      out = run('key BS; list')
      check('back to the top', out, r'1 Music \(Music\)')

      # the spectrum: bars after a second of sound (the analysers feed the C visualiser)
      out = run('key Home; key Down; key Enter; key Home; key Down; key Down; key Enter; wait 2500')
      shot('spectrum')
      bars = page.evaluate('''() => { const c = document.getElementById("screen"), g = c.getContext("2d");
        const y0 = Math.round(c.height * 0.45), d = g.getImageData(0, y0, Math.round(c.width * 0.75), 1).data; let n = 0;
        for (let i = 0; i < d.length; i += 4) if (d[i + 1] > 150 && d[i + 2] < 120) n++; return n; }''')
      check('the spectrum shows the sound', str(bars), r'^[1-9]\d+$')
      img = page.evaluate('''() => { const c = document.getElementById("screen"); const g = c.getContext("2d");
          const d = g.getImageData(0, 0, c.width, c.height).data; const s = new Set();
          for (let i = 0; i < d.length; i += 4 * 97) s.add(d[i] << 16 | d[i + 1] << 8 | d[i + 2]); return s.size; }''')
      check('the picture has colours', str(img), r'^([5-9]|\d\d+)$')

      # the radio: a station with CORS (Web Audio), one without (as it is), one that is gone
      lists = {'Test.m3u': '#EXTM3U\r\n#EXTINF:-1,Local FM\r\n%s/stream.mp3\r\n#EXTINF:-1,No CORS FM\r\n%s/nocors.mp3\r\n#EXTINF:-1,Gone FM\r\n%s/dead.mp3\r\n' % (SVC, SVC, SVC)}
      load({'folder': 'radio:Test.m3u'}, lists)
      out = run('key Home; key Down; key Enter; wait 2000; status')
      check('radio plays', out, r"state=1 .*stream=1 buf=0.*album='Local FM'")
      a = page.evaluate('picoamp.audio')
      check('radio through Web Audio', str(a['cur'].get('usingPlain')), 'False')
      out = run('key Down; key Enter; wait 2500; status')
      check('no CORS: still plays', out, r"state=1 .*stream=1 buf=0.*album='No CORS FM'")
      a = page.evaluate('picoamp.audio')
      check('no CORS: as it is', str(a['cur'].get('usingPlain')), 'True')
      out = run('key Down; key Enter; wait 2500; status')
      check('a dead station: an error', out, r"state=0 .*album='Gone FM' err='The station does not answer'")
      out = run('key Up; key Up; key Enter; wait 1200; key N; wait 1500; status; key P; wait 1500; status')
      check('N: the next station', out, r"pl=2/3 .*album='No CORS FM'")
      check('P: the one before', out, r"pl=1/3 .*album='Local FM'")

      # the equalizer and the balance reach the browser's filters
      out = run('key G; key Right; key Up; key Up; key Up; key Left; key Left; key Up; wait 100')
      shot('equalizer')
      run('key G; wait 50')
      a = page.evaluate('picoamp.audio')
      check('equalizer: 250 Hz +3', str(a['eqGains']), r'^\[0, 3, 0, 0, 0\]$')
      check('balance: right louder', '%.3f %.3f' % (a['gainL'], a['gainR']), r'^0\.\d+ 0\.\d+$')
      if not a['gainL'] < a['gainR']: fails += 1; print('FAIL balance %s' % a)
      else: print('ok   balance to the right')
      page.evaluate('() => window.dispatchEvent(new Event("pagehide"))')
      ini = page.evaluate('picoamp.fs.get("/PicoAmp/picoamp.ini")')
      check('equalizer kept', ini, r'(?m)^eq = 0,3,0,0,0')

      # favourites: F adds, Ctrl+Up moves, D removes
      out = run('key Esc; key Home; key Down; key F; key BS; key Home; key Down; key Enter; wait 100; list')
      check('F adds a favourite', out, r'list /Internet radio/Favourites \(2\)[\s\S]*Local FM')
      out = run('key Down; key D; list')
      check('D removes it', out, r'list /Internet radio/Favourites \(1\)')

      # a dropped list
      page.evaluate('''() => { const dt = new DataTransfer();
          dt.items.add(new File(["#EXTM3U\\n#EXTINF:-1,Dropped\\nhttps://x.example/s.mp3\\n"], "Dropped.m3u"));
          window.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true })); }''')
      time.sleep(0.3)
      out = run('list')
      check('a dropped list is added', out, r'1 Dropped')
      check('stored', page.evaluate('picoamp.fs.get("/PicoAmp/Radio/Dropped.m3u")') or '', 'Dropped')

      # the station lists' editor (O)
      run('key O')
      page.wait_for_selector('#lists[open]')
      page.select_option('#lists-select', 'Dropped.m3u')
      page.fill('#lists-text', '#EXTM3U\n#EXTINF:-1,Edited FM\nhttps://e.example/s.mp3\n')
      page.fill('#lists-name', 'My list')
      page.click('#lists-rename')
      page.click('#lists-close')
      out = run('list')
      check('editor: renamed and edited', out, r'1 My list')
      check('editor: the text', page.evaluate('picoamp.fs.get("/PicoAmp/Radio/My list.m3u")') or '', r'Edited FM\r\nhttps://e\.example')

      # the station directory: lists, a country, more stations, search
      load({'folder': 'radio:'})
      out = run('key Home; key Down; key Down; key Enter; wait 500; list; key Down; key Down; key Enter; wait 2500; status; key BS; key Down; key Down; key Down; key Enter; wait 500; list; key Down; key Down; key Enter; wait 500; list; key BS; key BS; key Down; key Enter; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Enter; wait 500; key End; key Enter; wait 500; list; key /; type jazz; key Enter; wait 500; list')
      shot('directory')
      check('directory: top stations', out, r'list /Internet radio/Top stations \(\d+\)')
      check('directory: a station plays', out, r"state=1 .*album='Station 1'")
      check('directory: countries', out, r'list /Internet radio/By country \(3\)')
      check('directory: a country', out, r'NPO Radio 1 = http')
      check('directory: more stations', out, r'list /Internet radio/By genre/Jazz \(101\)')
      check('directory: search', out, r'list /Internet radio/Search/jazz')
      check_not('directory: HLS left out', out, r'Jazz HLS')
      time.sleep(0.3)
      check('directory: the play is counted', open(os.path.join(T, 'svc.log')).read(), r'click uuid-1')

      # the clock (place by IP, by name; sleep timer; alarm) and the weather
      out = run('key C; wait 1500; page')
      shot('clock')
      out += run('key V; key O; key A; type 6:45w; key Enter; key L; type Sydney; key Enter; wait 800; key Down; key Enter; wait 1000; page; key W; wait 400; page')
      shot('weather')
      check('clock: the place by IP (GeoJS)', out, r"place='Utrecht, NL' weather='8\.4 C Rain'")
      check('clock: internet time', out, r'\(internet time\)')
      check('clock: sleep and alarm', out, r'Sleep 15 min  Alarm 06:45 Mo-Fr')
      check('clock: a place by name', out, r"place='Sydney, Nova Scotia, CA'")
      check('weather: now', out, r"weather place='Sydney, Nova Scotia, CA' now=8\.4 code=61 \(Rain\)")
      check('weather: the days', out, r'2026-11-08 Sun code=71 max=31 min=-62 rain=65%')
      out = run('key C; key T; wait 50; page; key T')
      check('news: says it is not here yet', out, r'news: not in the web version')

      # the codecs the browser plays
      codecs = page.evaluate('''() => ["MP3","AAC","OGG","OPUS","FLAC"].map(c => c + "=" + (new Audio().canPlayType({MP3:"audio/mpeg",AAC:"audio/aac",OGG:"audio/ogg; codecs=\\"vorbis\\"",OPUS:"audio/ogg; codecs=\\"opus\\"",FLAC:"audio/flac"}[c]) || "no")).join(" ")''')
      print('     browser codecs: ' + codecs)

      # (the station without CORS and the dead one fail to load on purpose)
      real = [e for e in errors if not re.search(r'favicon|dead\.mp3|nocors|CORS|404|Failed to load resource', e)]
      if real: fails += 1; print('FAIL page errors:\n  ' + '\n  '.join(real[:10]))
      else: print('ok   no page errors')
      br.close()

finally:
    svc.terminate()
    page_srv.shutdown()
print('all checks passed' if not fails else '%d checks FAILED' % fails)
sys.exit(1 if fails else 0)
