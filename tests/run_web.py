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
# covers: the folder's (a PNG: the browser decodes it) and a tag's (a JPEG: the program's TJpgDec)
ff('-f', 'lavfi', '-i', 'color=c=0x2040E0:s=200x200', '-frames:v', '1', os.path.join(M, 'Album', 'cover.png'))
ff('-f', 'lavfi', '-i', 'color=c=0xE02010:s=300x300', '-frames:v', '1', '-pix_fmt', 'yuvj420p', os.path.join(T, 'red.jpg'))
# a station's logo: green
ff('-f', 'lavfi', '-i', 'color=c=0x20D040:s=128x128', '-frames:v', '1', os.path.join(T, 'logo.png'))
ff('-f', 'lavfi', '-i', "aevalsrc='%s':s=44100:d=10" % E, '-i', os.path.join(T, 'red.jpg'), '-map', '0', '-map', '1',
   '-c:a', 'libmp3lame', '-b:a', '128k', '-c:v', 'copy', '-id3v2_version', '3', '-metadata', 'title=Tagged',
   '-metadata:s:v', 'comment=Cover (front)', os.path.join(M, 'Album', 'Tagged.mp3'))

# the pictures (I): a big JPEG, a PNG (the browser decodes it), a tall one, a folder
P = os.path.join(T, 'Pictures')
os.makedirs(os.path.join(P, 'Holiday'))
ff('-f', 'lavfi', '-i', 'testsrc=size=1600x1200:rate=1', '-frames:v', '1', '-pix_fmt', 'yuvj420p', os.path.join(P, 'a.jpg'))
ff('-f', 'lavfi', '-i', 'smptebars=size=640x480:rate=1', '-frames:v', '1', os.path.join(P, 'b.png'))
ff('-f', 'lavfi', '-i', 'testsrc=size=300x900:rate=1', '-frames:v', '1', '-pix_fmt', 'yuvj420p', os.path.join(P, 'c.jpg'))
ff('-f', 'lavfi', '-i', 'color=c=0xE02010:s=300x300', '-frames:v', '1', '-pix_fmt', 'yuvj420p', os.path.join(P, 'Holiday', 'd.jpg'))
open(os.path.join(P, 'notes.txt'), 'w').write('text')

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

URL = 'http://127.0.0.1:%d/?rb=%s&meteo=%s/meteo&geo=%s/geo&ipapi=%s/ipapi&logoproxy=%s/proxy%%3Furl%%3D&itunes=%s/itunes&npproxy=&relay=off' % (PAGE_PORT, SVC, SVC, SVC, SVC, SVC, SVC)
URL_RELAY = URL.replace('relay=off', 'relay=%s/relay/' % SVC)   # (the fake relay; else none, not the real default)
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
      check('the page starts (wasm)', out, r'list / \(3\)')
      check('top: Sonos', out, r'1 Sonos')
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

      # the cover view (V a third time): the folder's cover.png, then a tag's picture
      out = run('key Home; key Down; key Enter; wait 100; key Down; key Enter; wait 1500; key V; key V; wait 1500; page')
      shot('cover')
      check('cover: the folder\'s PNG', out, r'page 0 cover=ready')
      px = page.evaluate('picoamp.pixel(48, 88)')
      if not (px[2] > px[0] + 60): fails += 1; print('FAIL the folder cover is not blue: %s' % px)
      else: print('ok   the folder cover is blue')
      out = run('key Down; key Enter; wait 1500; page')
      check('cover: the tag\'s JPEG', out, r'page 0 cover=ready')
      px = page.evaluate('picoamp.pixel(48, 88)')
      if not (px[0] > px[2] + 60): fails += 1; print('FAIL the tag cover is not red: %s' % px)
      else: print('ok   the tag cover is red')
      out = run('key V; page; key BS')
      check('cover: V goes on to the spectrum', out, r'^page 0$')
      # a file without a picture: a default one, then its cover online (iTunes, by artist and title)
      out = run('key Home; key Down; key Down; key Enter; wait 500; key V; key V; wait 2000; page')
      check('cover: no picture: a default one, then online', out, r'page 0 cover=ready')
      run('wait 2500')
      check('cover: iTunes asked (artist and title)', open(os.path.join(T, 'svc.log')).read(), r'itunes Unit Tester Sine Caf')
      px = page.evaluate('picoamp.pixel(48, 88)')
      if not (px[1] > px[0] + 60 and px[1] > px[2] + 60): fails += 1; print('FAIL the online cover is not green: %s' % px)
      else: print('ok   the online cover (green)')
      out = run('key Z; key Left; wait 400; page')
      shot('fullcover')
      px = page.evaluate('picoamp.pixel(128, 96)')
      if not (px[1] > px[0] + 60 and px[1] > px[2] + 60): fails += 1; print('FAIL the full-screen cover is not green: %s' % px)
      else: print('ok   the full-screen cover')
      # F5 (the browser's own key: not a reload): the cover sharp, its own pixels (0x20D040 exactly,
      # the MSX's palette only near it); full screen whole (black beside it), on the player its square
      exact = lambda px: abs(px[0] - 0x20) <= 2 and abs(px[1] - 0xD0) <= 2 and abs(px[2] - 0x40) <= 2
      check('sharp covers: as the MSX first', str(page.evaluate('picoamp.pixel(128, 96)')), r'.')
      if exact(page.evaluate('picoamp.pixel(128, 96)')): fails += 1; print('FAIL sharp covers: already exact before F5')
      page.evaluate('window.notReloaded = 1')
      page.keyboard.press('F5')
      out = run('wait 400; page')
      shot('fullcover_sharp')
      check('sharp covers: F5 on full screen (whole)', out, r'page 1 sharp=full 0,8 256x168')
      px, side = page.evaluate('picoamp.pixel(128, 96)'), page.evaluate('picoamp.pixel(8, 96)')
      if not exact(px) or side != [0, 0, 0]: fails += 1; print('FAIL sharp covers: full screen not the picture\'s own pixels: %s, beside %s' % (px, side))
      else: print('ok   sharp covers: full screen: its own pixels, black beside')
      out = run('key Z; wait 400; page')
      shot('cover_sharp')
      check('sharp covers: on the player (its square)', out, r'page 0 cover=ready sharp=player 8,48 80x80')
      px = page.evaluate('picoamp.pixel(48, 88)')
      if not exact(px): fails += 1; print('FAIL sharp covers: the player\'s not its own pixels: %s' % px)
      else: print('ok   sharp covers: the player: its own pixels')
      page.keyboard.press('F5')
      out = run('wait 300; page')
      check('sharp covers: F5 again: as the MSX', out, r'page 0 cover=ready$')
      if exact(page.evaluate('picoamp.pixel(48, 88)')): fails += 1; print('FAIL sharp covers: still sharp after F5')
      check('sharp covers: the page was not reloaded by F5', str(page.evaluate('window.notReloaded')), r'^1$')
      run('key V')

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

      # the help (the ? box, F1, ?) and the wheel on the volume
      out = run('click 244 4; page; key x; key F1; page; key F1; page; key ?; page; key Esc; status; click 200 188 wheelup; click 230 188 wheelup; status; click 170 188 wheeldown; status')
      check('help: the ? box', out, r'^page 0 help')
      check('help: F1', out, r'page 0 help\npage 0\npage 0 help')
      check('wheel on VOL: louder', out, r'vol=90')
      check('wheel on VOL: softer', out, r'vol=85')
      page.keyboard.press('F1')
      check('help: the F1 key of the page', run('page'), r'page 0 help')
      page.keyboard.press('Escape')
      canvas = page.locator('#screen').bounding_box()
      page.mouse.move(canvas['x'] + canvas['width'] * 0.85, canvas['y'] + canvas['height'] * 0.98)
      page.mouse.wheel(0, -100)
      time.sleep(0.2)
      check('wheel on VOL: the real mouse', run('status'), r'vol=90')

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

      # a station's logo as its cover: by its directory id; a logo whose server allows no CORS (the
      # proxy); kept in the browser's cache (not asked again); a station without one
      load({'folder': 'radio:'})
      out = run('key Home; key Down; key Down; key Enter; wait 500; key Down; key Down; key Enter; wait 1500; key V; key V; wait 1500; page')
      shot('logo')
      check('logo: the directory\'s station', out, r'page 0 cover=ready')
      px = page.evaluate('picoamp.pixel(48, 88)')
      if not (px[1] > px[0] + 60 and px[1] > px[2] + 60): fails += 1; print('FAIL the logo is not green: %s' % px)
      else: print('ok   the logo is green')
      out = run('key Down; key Enter; wait 2000; page; key V')
      check('logo: through the proxy', out, r'page 0 cover=ready')
      check('logo: the proxy asked', open(os.path.join(T, 'svc.log')).read(), r'proxy http://127\.0\.0\.1:\d+/logos/nocors\.png')
      n1 = open(os.path.join(T, 'svc.log')).read().count('byuuid uuid-1')
      load({'folder': 'radio:'})
      out = run('key Home; key Down; key Down; key Enter; wait 500; key Down; key Down; key Enter; wait 1500; key V; key V; wait 500; page')
      check('logo: from the cache', out, r'page 0 cover=ready')
      if open(os.path.join(T, 'svc.log')).read().count('byuuid uuid-1') != n1: fails += 1; print('FAIL logo: asked again')
      else: print('ok   logo: not asked again')
      out = run('key Up; key Enter; wait 2000; page; key V')
      check('logo: a station without one: a default picture', out, r'page 0 cover=ready')
      # what a station plays (Icecast's status), its song's cover; the stream's own titles (ICY)
      run('key V; key V; wait 2500')
      check('station: its song read (Icecast status)', run('status'), r"title='Sine Cafe Song' artist='Unit Tester'")
      run('wait 1500')
      shot('songcover')
      px = page.evaluate('picoamp.pixel(48, 88)')
      if not (px[1] > px[0] + 60 and px[1] > px[2] + 60): fails += 1; print('FAIL the song cover is not green: %s' % px)
      else: print('ok   the song\'s cover (green)')
      t = page.evaluate('picoamp.npIcy("%s/icy.mp3")' % SVC)
      check('station: the stream\'s own title (ICY)', str(t), r'^Artist One - Song A$')
      run('key V')

      # how many are kept: past 200 covers the ones shown longest ago go (to 180); a "none" older
      # than 30 days is asked again
      page.evaluate('async () => { for (let i = 0; i < 215; i++) await picoamp.art.put("cover", "old " + i, false, 1577836800000 + i * 60000); }')
      gone = page.evaluate('picoamp.art.prune("cover")')
      check('covers: at most 200 kept', '%d' % page.evaluate('picoamp.art.count("cover")'), r'^180$')
      if page.evaluate('picoamp.art.has("cover", "old 0")') or not page.evaluate('picoamp.art.has("cover", "old 214")'): fails += 1; print('FAIL covers: not the oldest went (%s)' % gone)
      else: print('ok   covers: the oldest went')
      page.evaluate('''async () => { const c = await caches.open("picoamp-logos");
          for (const r of await c.keys()) if (r.url.includes("/picoamp-logo/"))
              await c.put(r, new Response("", { headers: { "x-none": "1", "x-time": String(Date.now() - 31 * 864e5) } })); }''')
      n1 = open(os.path.join(T, 'svc.log')).read().count('byuuid uuid-1')
      load({'folder': 'radio:'})
      run('key Home; key Down; key Down; key Enter; wait 500; key Down; key Down; key Enter; wait 1500; key V; key V; wait 500')
      if open(os.path.join(T, 'svc.log')).read().count('byuuid uuid-1') == n1: fails += 1; print('FAIL logos: an old none not asked again')
      else: print('ok   logos: a "none" older than 30 days: asked again')
      run('key V')

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
      # the pictures (I): a folder picked, its folders and pictures, one in SCREEN 8, the next (a PNG), the slideshow
      out = run('key Esc; key I; wait 100; page')
      check('pictures: no folder yet', out, r"pictures browse folder='' entries=1 folders=1 pictures=0")
      with page.expect_file_chooser() as fc:
          page.keyboard.press('Enter')
      fc.value.set_files(P)
      time.sleep(0.6)
      out = run('wait 300; page')
      check('pictures: the folder (pictures and folders only)', out, r"pictures browse folder='' entries=5 folders=2 pictures=3")
      out = run('key Down; key Down; key Enter; wait 1500; page')
      shot('picture')
      check('pictures: one shows', out, r'pictures view .*shown=2 have=1')
      dots = page.evaluate('''() => { const c = document.getElementById("screen"), g = c.getContext("2d");
        const d = g.getImageData(0, 0, c.width, c.height).data, s = new Set();
        for (let i = 0; i < d.length; i += 4 * 97) s.add(d[i] << 16 | d[i + 1] << 8 | d[i + 2]); return s.size; }''')
      check('pictures: in many colours (SCREEN 8)', str(dots), r'^([3-9]\d|\d{3,})$')
      out = run('key Right; wait 1500; page; key x; key Home; key Down; key Enter; wait 300; page; key BS; wait 300; key R; key R; key R; key R; key G; wait 1500; page; wait 5800; page; key x')
      check('pictures: a PNG (the browser decodes it)', out, r'pictures view .*shown=3 have=1')
      check('pictures: a folder', out, r"pictures browse folder='Holiday' entries=1 folders=0 pictures=1")
      check('pictures: the slideshow', out, r'pictures slideshow .*shown=2 have=1')
      check('pictures: the slideshow goes on', out, r'pictures slideshow .*shown=[34] have=1')
      # the screensaver: a minute without a key on the player's screen; a key: back
      out = run('key O; key Esc; wait 59000; page; wait 3000; page; key x; page')
      check('screensaver: after a minute', out, r'pictures screensaver .*have=1')
      check('screensaver: a key, back to the player', out.strip().split('\n')[-1], r'^page 0')
      out = run('key I; key O; key O; key O; key O; key O; key O; key Esc')
      # the cover big (Z, Left: COVER): the station's logo (green), in SCREEN 8 (black beside it)
      out = run('key Esc; key Z; key Left; wait 800; page')
      shot('fullcover')
      px = page.evaluate('''() => { const c = document.getElementById("screen"), g = c.getContext("2d");
        const m = g.getImageData(c.width >> 1, c.height >> 1, 1, 1).data, e = g.getImageData(c.width * 0.05 | 0, c.height >> 1, 1, 1).data;
        return [m[0], m[1], m[2], e[0], e[1], e[2]].join(","); }''')
      check('the cover big (SCREEN 8): the logo, black beside it', px, r'^\d{1,2},(1[5-9]\d|2\d\d),\d{1,3},0,0,0$')
      run('key Z')

      # the markets ($) and the news (T): their sites do not let pages read them, so through the relay;
      # without one they say so
      page.goto(URL)
      page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
      out = run('key Esc; key $; wait 800; page; key Esc; key T; wait 100; key A; type https://feeds.bbci.co.uk/news/rss.xml; key Enter; wait 500; page; key T')
      check('markets: no relay, it says so', out, r"markets table region=Europe list='Indices' rows=20 state=3 .*err='needs the relay")
      check('news: no relay, it says so', out, r"news \w+ .*state=3 .*err='needs the relay")
      page.goto(URL_RELAY)
      page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
      out = run('key Esc; key $; wait 1500; page; key Down; key Down; key Down; key Down; key I; wait 1500; page; key BS; key Home; key Enter; wait 1500; page; key T; wait 600; page; key Esc; page')
      shot('markets')
      check('markets: Europe\'s indices (the relay)', out, r"markets table region=Europe list='Indices' rows=20 state=2")
      check('markets: AEX\'s price', out, r'\^AEX AEX [0-9]')
      check('markets: the tape', out, r'tape: \^AEX,\^GDAXI,\^FTSE,\^FCHI story 0 & more \+\+\+')
      check('markets: FTSE 100\'s members', out, r"list='FTSE 100' rows=98 state=2")
      check('markets: the day\'s chart', out, r'sym=\^AEX range=1D chart=2 points=33')
      check('markets: its headlines', out, r'head: \^AEX story 5 & more')
      check('markets: the headlines in the news reader', out, r"news headlines feed='Market news: AEX' state=2 stories=6")
      check('markets: and back', out, r'markets chart .* sym=\^AEX range=1D')
      page.goto(URL_RELAY)
      page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
      out = run('key Esc; key T; wait 100; key A; type https://feeds.bbci.co.uk/news/rss.xml; key Enter; wait 800; page; key End; key Enter; wait 800; page; key Down; key Enter; key Enter; wait 1000; page; key Esc; key Esc; key T')
      shot('news')
      check('news: a feed (the relay)', out, r"news headlines feed='Test News' state=2 stories=30")
      check('news: the full article (the story\'s page)', out, r'news story .*article=2 lines=3\d[\s\S]*Paragraph 1 of the article 24')
      check('relay: asked for Yahoo and the feed', open(os.path.join(T, 'svc.log')).read(), r'relay https://query1\.finance\.yahoo\.com/v8/finance/spark\?symbols=[\s\S]*relay https://feeds\.bbci\.co\.uk/news/rss\.xml')
      page.goto(URL)
      page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')


      # Sonos, through PicoAmp for Windows (the stand-in for its bridge): the rooms, one followed as the
      # player (its song, time, cover, the keys to it), a station (no next), back to the page's own music
      page.goto(URL + '&bridge=' + SVC)
      page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')
      out = run('wait 300; key Esc; key BS; key BS; key End; key Enter; wait 800; list; key End; key Enter; wait 1800; status; key V; key V; wait 600; page; '
                'key Space; wait 1300; status; key N; wait 1300; status; key -; key M; wait 1300; status; key H; wait 1200; status; '
                'key Home; key Down; key Enter; wait 1600; status; key N; key +; wait 1300; status')
      shot('sonos')
      check('sonos: the rooms', out, r'list /Sonos \(3\)[\s\S]*9 Kitchen \+ 1[\s\S]*9 Living Room')
      check('sonos: a room as the player', out, r"state=1 elapsed=\d+/210 .*vol=30 title='Fake Song' artist='Fake Artist' album='Fake Album'")
      check('sonos: its cover', out, r'page 0 cover=ready')
      check('sonos: Space pauses it', out, r"state=2 .*title='Fake Song'")
      check('sonos: N its next song', out, r"title='Fake Song 2'")
      check('sonos: volume and mute', out, r"vol=25 .*title='Fake Song 2'.* muted=1")
      check('sonos: shuffle', out, r"title='Fake Song 2'.* mode=2")
      check('sonos: a station', out, r"state=1 elapsed=60/0 .*vol=45 title='Radio Song' artist='Radio Artist' album='Radio Fake'")
      told = page.evaluate('u => fetch(u).then(r => r.text())', SVC + '/sonos/log')
      check('sonos: told: pause, next, volume, mute, shuffle', told, r'RINCON_A c=pause&on=0\nRINCON_A c=next&on=0\nRINCON_A v=25\n(RINCON_A m=1\n)?[\s\S]*RINCON_A c=shuffle&on=1')
      check_not('sonos: a station: no next', told, r'RINCON_B c=next')
      check('sonos: the group\'s volume', told, r'RINCON_B v=45')
      out = run('key BS; key Home; key Enter; wait 200; key Home; key Down; key Down; key Down; key Down; key Down; key Down; key Down; key Enter; wait 1500; status')
      page.goto(URL + '&bridge=off')
      page.wait_for_function('window.picoamp && document.body.classList.contains("ready")')

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
