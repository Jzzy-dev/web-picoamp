// picoamp.js - PicoAmp in the browser: the page around the C program
// (picoamp.wasm). It draws the picture, takes the keys and the mouse, plays
// the sound (an <audio> element through Web Audio: equalizer, balance,
// volume, analysers for the visualiser), keeps the settings and station
// lists in the browser's storage, fetches for the program and reads the
// music folder the person picks.
'use strict';

(() => {
const params = new URLSearchParams(location.search);
const enc = new TextEncoder();
const dec = new TextDecoder();
let wasm, mem, X;            // the instance's memory and exports

// ---------------- memory helpers ----------------
const u8 = () => new Uint8Array(mem.buffer);
const str = (p, n) => {
    if (!p) return '';
    const b = u8();
    if (n === undefined) { n = 0; while (b[p + n]) n++; }
    return dec.decode(b.subarray(p, p + n));
};
// a string (or bytes) into the program's memory, 0-terminated: [ptr, len]
function put(v) {
    const bytes = typeof v === 'string' ? enc.encode(v) : v;
    const p = X.web_alloc(bytes.length + 1);
    const b = u8();
    b.set(bytes, p);
    b[p + bytes.length] = 0;
    return [p, bytes.length];
}
function withStr(s, fn) { const [p, n] = put(s); try { return fn(p, n); } finally { X.web_free(p); } }
function writeOut(s, out, max) {
    const bytes = enc.encode(s);
    const n = Math.min(bytes.length, max);
    u8().set(bytes.subarray(0, n), out);
    if (n < max) u8()[out + n] = 0;
    return n;
}

// ---------------- WASI: the little the C library asks for ----------------
// the environment: the services' addresses for the tests (?rb=...&meteo=...)
const envMap = { rb: 'PICOAMP_RB', meteo: 'PICOAMP_METEO', geo: 'PICOAMP_GEO', ipapi: 'PICOAMP_IPAPI' };
const env = [];
for (const [k, v] of Object.entries(envMap)) if (params.get(k)) env.push(`${v}=${params.get(k)}`);
const lineBuf = { 1: '', 2: '' };
const wasi = {
    environ_sizes_get(cp, sp) {
        const dv = new DataView(mem.buffer);
        dv.setUint32(cp, env.length, true);
        dv.setUint32(sp, env.reduce((a, e) => a + enc.encode(e).length + 1, 0), true);
        return 0;
    },
    environ_get(ep, bp) {
        const dv = new DataView(mem.buffer);
        for (const e of env) {
            const b = enc.encode(e);
            dv.setUint32(ep, bp, true); ep += 4;
            u8().set(b, bp); u8()[bp + b.length] = 0; bp += b.length + 1;
        }
        return 0;
    },
    fd_write(fd, iov, n, wp) {
        const dv = new DataView(mem.buffer);
        let total = 0;
        for (let i = 0; i < n; i++) {
            const p = dv.getUint32(iov + i * 8, true), l = dv.getUint32(iov + i * 8 + 4, true);
            if (fd === 1 || fd === 2) {
                lineBuf[fd] += str(p, l);
                let k;
                while ((k = lineBuf[fd].indexOf('\n')) >= 0) {
                    (fd === 2 ? console.warn : console.log)('[picoamp]', lineBuf[fd].slice(0, k));
                    lineBuf[fd] = lineBuf[fd].slice(k + 1);
                }
            }
            total += l;
        }
        dv.setUint32(wp, total, true);
        return 0;
    },
    fd_close: () => 0,
    fd_seek: () => 70,          // ESPIPE
    fd_fdstat_get: () => 8,     // EBADF
    proc_exit(c) { throw new Error('exit ' + c); },
};

// ---------------- the settings' files (localStorage) ----------------
const FS = 'picoamp:fs:';
const fsDirs = new Set(['/PicoAmp', '/PicoAmp/Radio', '/drop']);
const memFiles = new Map();          // dropped files (not kept)
const fsGet = path => memFiles.has(path) ? memFiles.get(path) : (() => {
    try { return localStorage.getItem(FS + path); } catch { return null; }
})();
function fsSet(path, text) {
    if (path.startsWith('/drop/')) { memFiles.set(path, text); return true; }
    try { localStorage.setItem(FS + path, text); return true; } catch { return false; }
}
function fsDel(path) { try { localStorage.removeItem(FS + path); } catch { } }
function fsList(dir) {
    dir = dir.replace(/\/+$/, '');
    const names = new Map();
    const keys = [];
    try { for (let i = 0; i < localStorage.length; i++) { const k = localStorage.key(i); if (k.startsWith(FS)) keys.push(k.slice(FS.length)); } } catch { }
    for (const k of memFiles.keys()) keys.push(k);
    for (const k of keys) {
        if (!k.startsWith(dir + '/')) continue;
        const rest = k.slice(dir.length + 1);
        const sl = rest.indexOf('/');
        if (sl < 0) names.set(rest, 'f'); else names.set(rest.slice(0, sl), 'd');
    }
    if (!names.size && !fsDirs.has(dir)) return null;
    return [...names].map(([n, t]) => `${t} ${n}`);
}
const radioDir = '/PicoAmp/Radio';

// ---------------- the music folder ----------------
// a tree of { name, kind: 'dir'|'file', children: Map, file / handle }
let music = null;              // the root: { name, handle } or { name, tree }
let musicPick = null;          // a pick in progress (a promise)
const dropped = [];            // dropped music files: "drop:N/name"

function idb() {
    return new Promise((ok, bad) => {
        const r = indexedDB.open('picoamp', 1);
        r.onupgradeneeded = () => r.result.createObjectStore('kv');
        r.onsuccess = () => ok(r.result);
        r.onerror = () => bad(r.error);
    });
}
async function idbSet(k, v) { const db = await idb(); return new Promise(ok => { const t = db.transaction('kv', 'readwrite'); t.objectStore('kv').put(v, k); t.oncomplete = ok; t.onerror = ok; }); }
async function idbGet(k) { const db = await idb(); return new Promise(ok => { const r = db.transaction('kv').objectStore('kv').get(k); r.onsuccess = () => ok(r.result); r.onerror = () => ok(undefined); }); }

async function restoreMusic() {
    try {
        const h = await idbGet('music');
        if (h && h.kind === 'directory') music = { name: h.name, handle: h };
    } catch { }
}

function pickWithInput() {
    return new Promise(ok => {
        const inp = document.createElement('input');
        inp.type = 'file';
        inp.webkitdirectory = true;
        inp.multiple = true;
        inp.style.display = 'none';
        document.body.appendChild(inp);
        let done = false;
        inp.onchange = () => {
            done = true;
            const files = [...inp.files];
            inp.remove();
            if (!files.length) { ok(false); return; }
            const root = { name: files[0].webkitRelativePath.split('/')[0] || 'Music', kind: 'dir', children: new Map() };
            for (const f of files) {
                const parts = f.webkitRelativePath.split('/').slice(1);
                let d = root;
                for (let i = 0; i < parts.length - 1; i++) {
                    if (!d.children.has(parts[i])) d.children.set(parts[i], { name: parts[i], kind: 'dir', children: new Map() });
                    d = d.children.get(parts[i]);
                }
                d.children.set(parts[parts.length - 1], { name: parts[parts.length - 1], kind: 'file', file: f });
            }
            music = { name: root.name, tree: root };
            ok(true);
        };
        inp.addEventListener('cancel', () => { if (!done) { inp.remove(); ok(false); } });
        inp.click();
    });
}

function js_music_pick() {
    if (window.showDirectoryPicker) {
        musicPick = window.showDirectoryPicker({ id: 'picoamp-music', mode: 'read', startIn: 'music' })
            .then(async h => { music = { name: h.name, handle: h }; await idbSet('music', h).catch(() => { }); return true; })
            .catch(() => false);
    } else musicPick = pickWithInput();
}

async function musicDir(path) {
    if (!music) throw new Error('No music folder chosen');
    const parts = path ? path.split('/') : [];
    if (music.tree) {
        let d = music.tree;
        for (const p of parts) { d = d.children.get(p); if (!d || d.kind !== 'dir') throw new Error('Folder not found'); }
        return { list: async () => [...d.children.values()].map(c => [c.kind === 'dir' ? 'd' : 'f', c.name]) };
    }
    const h0 = music.handle;
    if (h0.queryPermission && (await h0.queryPermission({ mode: 'read' })) !== 'granted') {
        if ((await h0.requestPermission({ mode: 'read' })) !== 'granted') throw new Error('The music folder may not be read');
    }
    let h = h0;
    for (const p of parts) h = await h.getDirectoryHandle(p);
    return {
        list: async () => {
            const out = [];
            for await (const [name, e] of h.entries()) out.push([e.kind === 'directory' ? 'd' : 'f', name]);
            return out;
        }
    };
}

async function musicFile(path) {
    if (path.startsWith('drop:')) {
        const i = parseInt(path.slice(5), 10);
        if (dropped[i]) return dropped[i];
        throw new Error('The file is gone (dropped files are kept until the page is closed)');
    }
    const rel = path.slice(6);
    const parts = rel.split('/');
    const name = parts.pop();
    if (!music) throw new Error('No music folder');
    if (music.tree) {
        let d = music.tree;
        for (const p of parts) d = d.children.get(p);
        const f = d && d.children.get(name);
        if (!f) throw new Error('File not found');
        return f.file;
    }
    let h = music.handle;
    for (const p of parts) h = await h.getDirectoryHandle(p);
    return (await h.getFileHandle(name)).getFile();
}

function js_music_list(id, p, n) {
    const path = str(p, n);
    const pick = musicPick;
    (async () => {
        if (pick) { const ok = await pick; if (musicPick === pick) musicPick = null; if (!ok) throw new Error(music ? 'No new folder chosen' : 'No folder chosen'); }
        const d = await musicDir(path);
        const items = await d.list();
        return items.filter(([, nm]) => !nm.startsWith('.')).map(([k, nm]) => `${k} ${nm}`).join('\n');
    })().then(text => withStr(text, p2 => X.dir_done(id, p2, 1)),
              e => withStr(String(e.message || e), p2 => X.dir_done(id, p2, 0)));
}

function js_music_name(out, max) { return music ? writeOut(music.name, out, max) : 0; }

// ---------------- the web ----------------
function js_fetch(id, p, n) {
    const url = str(p, n);
    fetch(url, { cache: 'no-store', signal: AbortSignal.timeout ? AbortSignal.timeout(20000) : undefined }).then(async r => {
        const b = new Uint8Array(await r.arrayBuffer());
        return [r.status, b, ''];
    }).catch(e => [0, new Uint8Array(0), navigator.onLine === false ? 'No internet connection' : e && e.name === 'TimeoutError' ? 'No answer in 20 seconds' : 'No answer (blocked or offline)'])
      .then(([status, b, err]) => {
          const [bp, bl] = put(b);
          const [ep] = put(err);
          try { X.net_done(id, status, bp, bl, ep); } finally { X.web_free(bp); X.web_free(ep); }
      });
}

// ---------------- the sound ----------------
let ctx = null, graph = null;
const el = new Audio();           // through Web Audio (needs CORS for streams)
el.crossOrigin = 'anonymous';
el.preload = 'auto';
const plain = new Audio();        // a station without CORS: as it is (no spectrum, no equalizer)
plain.preload = 'none';
let cur = null;                   // { src, isFile, url (blob), tryPlain, usingPlain, gen }
let gen = 0;
let gainL = 1, gainR = 1;
let eqGains = [0, 0, 0, 0, 0];
let lastT = 0;

function ensureCtx() {
    if (ctx) { if (ctx.state === 'suspended') ctx.resume(); return; }
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    ctx = new AC();
    const src = ctx.createMediaElementSource(el);
    const pre = ctx.createGain();
    const bands = [['lowshelf', 120], ['peaking', 250], ['peaking', 1000], ['peaking', 4000], ['highshelf', 7200]]
        .map(([t, f]) => { const b = ctx.createBiquadFilter(); b.type = t; b.frequency.value = f; b.Q.value = 1; return b; });
    const split = ctx.createChannelSplitter(2);
    const aL = ctx.createAnalyser(), aR = ctx.createAnalyser();
    aL.fftSize = aR.fftSize = 4096;
    const gL = ctx.createGain(), gR = ctx.createGain();
    const merge = ctx.createChannelMerger(2);
    src.connect(pre);
    let n = pre;
    for (const b of bands) { n.connect(b); n = b; }
    n.connect(split);
    split.connect(aL, 0); split.connect(aR, 1);
    split.connect(gL, 0); split.connect(gR, 1);
    gL.connect(merge, 0, 0); gR.connect(merge, 0, 1);
    merge.connect(ctx.destination);
    graph = { pre, bands, aL, aR, gL, gR, bufL: new Float32Array(4096), bufR: new Float32Array(4096) };
    applyGain();
    applyEq();
}

function applyGain() {
    if (graph) {
        graph.gL.gain.setTargetAtTime(gainL, ctx.currentTime, 0.015);
        graph.gR.gain.setTargetAtTime(gainR, ctx.currentTime, 0.015);
    }
    plain.volume = Math.min(1, (gainL + gainR) / 2);
}
function applyEq() {
    if (!graph) return;
    eqGains.forEach((g, i) => graph.bands[i].gain.setTargetAtTime(g, ctx.currentTime, 0.02));
    // room for what is raised (as the engine's pre-amp)
    const top = Math.max(0, ...eqGains);
    graph.pre.gain.setTargetAtTime(Math.pow(10, -top / 20), ctx.currentTime, 0.02);
}

function js_audio_gain(l, r) { gainL = l; gainR = r; applyGain(); }
function js_audio_eq(...g) { eqGains = g; applyEq(); }

function stopElements() {
    for (const a of [el, plain]) { a.pause(); a.removeAttribute('src'); a.load(); }
    if (cur && cur.url) URL.revokeObjectURL(cur.url);
}

function js_audio_stop() { gen++; stopElements(); cur = null; mediaState('none'); }
function js_audio_pause(on) {
    const a = cur && cur.usingPlain ? plain : el;
    if (on) a.pause(); else { ensureCtx(); a.play().catch(() => { }); }
    mediaState(on ? 'paused' : 'playing');
}
function js_audio_seek(s) { if (cur && !cur.usingPlain) el.currentTime = s; }

function cErr(msg) { withStr(msg, p => X.web_audio_error(p)); }

function js_audio_play(p, n, isFile) {
    const src = str(p, n);
    ensureCtx();
    stopElements();
    const g = ++gen;
    cur = { src, isFile: !!isFile, gen: g, usingPlain: false };
    lastT = 0;
    if (isFile) {
        musicFile(src).then(async f => {
            if (g !== gen) return;
            cur.url = URL.createObjectURL(f);
            cur.size = f.size;
            el.src = cur.url;
            el.play().catch(e => { if (g === gen && e.name !== 'AbortError') cErr(playError(e)); });
            // the tags: the first 512 KB and the last 128 bytes
            const head = new Uint8Array(await f.slice(0, 512 * 1024).arrayBuffer());
            const tail = f.size > head.length ? new Uint8Array(await f.slice(Math.max(head.length, f.size - 128)).arrayBuffer()) : new Uint8Array(0);
            if (g !== gen) return;
            const all = new Uint8Array(head.length + tail.length);
            all.set(head); all.set(tail, head.length);
            const [bp, bl] = put(all);
            try { X.web_tags(bp, bl); } finally { X.web_free(bp); }
        }).catch(e => { if (g === gen) cErr(String(e.message || e)); });
        return;
    }
    let url = src;
    if (location.protocol === 'https:' && /^http:/i.test(url)) cur.insecure = true;
    el.src = url;
    el.play().catch(e => { if (g === gen && e.name === 'NotAllowedError') cErr(playError(e)); });
}

function playError(e) {
    if (e && e.name === 'NotAllowedError') return 'Press a key or click first: the browser holds the sound back';
    return 'Cannot play this';
}

function tryPlain() {
    // the station does not allow Web Audio (no CORS): play it as it is
    cur.usingPlain = true;
    plain.src = cur.src;
    applyGain();
    const g = cur.gen;
    plain.play().catch(e => { if (g === gen && e.name !== 'AbortError') failStream(); });
    withStr('No spectrum or equalizer for this station (it does not allow it)', p => X.web_message(p, 0));
}

function failStream() {
    if (!cur) return;
    if (cur.insecure) cErr('This station is http only: a web page on https may not play it');
    else cErr('The station does not answer');
}

el.addEventListener('error', () => {
    if (!cur || !el.getAttribute('src')) return;
    if (cur.isFile) { cErr('Cannot play this file here'); return; }
    if (!cur.usingPlain) tryPlain();
});
plain.addEventListener('error', () => { if (cur && cur.usingPlain && plain.getAttribute('src')) failStream(); });
for (const a of [el, plain]) {
    a.addEventListener('playing', () => {
        if (!cur || (a === plain) !== !!cur.usingPlain) return;
        X.web_audio_playing(ctx ? ctx.sampleRate : 0, 2);
        mediaState('playing');
        if (cur.isFile && cur.size && !/\.wav$/i.test(cur.src) && isFinite(a.duration) && a.duration > 0) X.web_audio_bitrate(Math.round(cur.size * 8 / a.duration / 1000));
    });
    a.addEventListener('waiting', () => { if (cur && (a === plain) === !!cur.usingPlain) X.web_audio_waiting(); });
    a.addEventListener('timeupdate', () => {
        if (!cur || (a === plain) !== !!cur.usingPlain) return;
        X.web_audio_time(a.currentTime, cur.isFile ? a.duration || 0 : 0);
        mediaPosition(a);
    });
    a.addEventListener('ended', () => { if (cur && (a === plain) === !!cur.usingPlain) X.web_audio_ended(); });
}

// the analysers -> the visualiser: the samples that came since the last frame
function feedViz() {
    if (!graph || !cur || cur.usingPlain || el.paused) { lastT = 0; return; }
    const t = ctx.currentTime;
    const rate = ctx.sampleRate;
    let n = lastT ? Math.round((t - lastT) * rate) : 1024;
    lastT = t;
    if (n <= 0) return;
    if (n > 4096) n = 4096;
    graph.aL.getFloatTimeDomainData(graph.bufL);
    graph.aR.getFloatTimeDomainData(graph.bufR);
    const ptr = X.web_viz_buf();
    const out = new Int16Array(mem.buffer, ptr, n * 2);
    const off = 4096 - n;
    for (let i = 0; i < n; i++) {
        out[i * 2] = Math.max(-32768, Math.min(32767, graph.bufL[off + i] * 32767));
        out[i * 2 + 1] = Math.max(-32768, Math.min(32767, graph.bufR[off + i] * 32767));
    }
    X.web_viz_feed(n, rate);
}

const codecTypes = {
    'MP3': 'audio/mpeg', 'AAC': 'audio/aac', 'AAC+': 'audio/aac', 'OGG': 'audio/ogg; codecs="vorbis"',
    'OPUS': 'audio/ogg; codecs="opus"', 'FLAC': 'audio/flac',
};
function js_can_play(p, n) {
    const t = codecTypes[str(p, n).toUpperCase()];
    return t && el.canPlayType(t) ? 1 : 0;
}

// ---------------- media keys, the lock screen ----------------
const ms = navigator.mediaSession;
function js_audio_meta(tp, tn, ap, an, bp, bn) {
    if (!ms || !window.MediaMetadata) return;
    const title = str(tp, tn), artist = str(ap, an), album = str(bp, bn);
    ms.metadata = new MediaMetadata({ title: title || album || 'PicoAmp', artist: artist || (title ? album : ''), album,
        artwork: [{ src: 'icon-512.png', sizes: '512x512', type: 'image/png' }] });
}
function mediaState(s) { if (ms) ms.playbackState = s; }
function mediaPosition(a) {
    if (!ms || !ms.setPositionState || !cur || !cur.isFile || !isFinite(a.duration) || !a.duration) return;
    try { ms.setPositionState({ duration: a.duration, position: Math.min(a.currentTime, a.duration), playbackRate: 1 }); } catch { }
}
const K = { ENTER: 13, BS: 8, ESC: 27, UP: 0x100, DOWN: 0x101, LEFT: 0x102, RIGHT: 0x103, PGUP: 0x104, PGDN: 0x105,
    HOME: 0x106, END: 0x107, DEL: 0x108, PLAY: 0x109, STOP: 0x10A, NEXT: 0x10B, PREV: 0x10C, VUP: 0x10D, VDOWN: 0x10E,
    MUTE: 0x10F, CUP: 0x110, CDOWN: 0x111 };
if (ms) {
    const h = (a, k) => { try { ms.setActionHandler(a, () => { ensureCtx(); X.web_key(k); }); } catch { } };
    h('play', K.PLAY); h('pause', K.PLAY); h('stop', K.STOP); h('nexttrack', K.NEXT); h('previoustrack', K.PREV);
    try { ms.setActionHandler('seekto', d => { if (cur && cur.isFile) el.currentTime = d.seekTime; }); } catch { }
}

// ---------------- the picture ----------------
const canvas = document.getElementById('screen');
const g2 = canvas.getContext('2d', { alpha: false });
const small = document.createElement('canvas');
const sctx = small.getContext('2d');
const big = document.createElement('canvas');
const bctx = big.getContext('2d');
let img = null, dest = { x: 0, y: 0, w: 1, h: 1 };

function resize() {
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(canvas.clientWidth * dpr));
    canvas.height = Math.max(1, Math.round(canvas.clientHeight * dpr));
}
window.addEventListener('resize', resize);

function computeDest(cw, ch, scale) {
    let dw, dh;
    if (scale === 2) { dw = cw; dh = ch; }
    else if (scale === 1) { let k = Math.max(1, Math.min(Math.floor(cw / 256), Math.floor(ch / 192))); dw = 256 * k; dh = 192 * k; }
    else if (cw * 192 <= ch * 256) { dw = cw; dh = Math.floor(cw * 192 / 256); }
    else { dh = ch; dw = Math.floor(ch * 256 / 192); }
    dest = { x: Math.floor((cw - dw) / 2), y: Math.floor((ch - dh) / 2), w: Math.max(1, dw), h: Math.max(1, dh) };
}

function draw() {
    const w = X.web_width(), h = X.web_height(), stride = X.web_fb_stride();
    const fb = new Uint8Array(mem.buffer, X.web_fb(), stride * h);
    const pal = new Uint32Array(mem.buffer, X.web_palette(), 16);
    if (!img || img.width !== w || img.height !== h) {
        img = sctx.createImageData(w, h);
        small.width = w; small.height = h;
    }
    const px = new Uint32Array(img.data.buffer);
    const lut = new Uint32Array(16);
    for (let i = 0; i < 16; i++) { const c = pal[i]; lut[i] = 0xFF000000 | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
    for (let y = 0; y < h; y++) {
        const r = y * stride, o = y * w;
        for (let x = 0; x < w; x++) px[o + x] = lut[fb[r + x] & 15];
    }
    sctx.putImageData(img, 0, 0);

    computeDest(canvas.width, canvas.height, X.web_cfg_scale());
    // whole multiples below the size, each way (sharp), then smoothed to the size
    const kx = Math.min(12, Math.max(1, Math.floor(dest.w / w))), ky = Math.min(12, Math.max(1, Math.floor(dest.h / h)));
    if (big.width !== w * kx || big.height !== h * ky) { big.width = w * kx; big.height = h * ky; }
    bctx.imageSmoothingEnabled = false;
    bctx.drawImage(small, 0, 0, w * kx, h * ky);
    if (X.web_cfg_scanlines() && ky >= 2) {
        // the last third of every line darker
        const d = Math.ceil((ky + 2) / 3);
        bctx.fillStyle = 'rgba(0,0,0,0.25)';
        for (let y = 0; y < h; y++) bctx.fillRect(0, y * ky + ky - d, w * kx, d);
    }
    const back = pal[14];
    g2.fillStyle = `rgb(${back >> 16 & 255},${back >> 8 & 255},${back & 255})`;
    g2.fillRect(0, 0, canvas.width, canvas.height);
    const exact = dest.w === w * kx && dest.h === h * ky;
    g2.imageSmoothingEnabled = !exact;
    g2.imageSmoothingQuality = 'high';
    g2.drawImage(big, dest.x, dest.y, dest.w, dest.h);
}

// ---------------- keys and the mouse ----------------
const named = { Enter: K.ENTER, Backspace: K.BS, Escape: K.ESC, ArrowLeft: K.LEFT, ArrowRight: K.RIGHT,
    PageUp: K.PGUP, PageDown: K.PGDN, Home: K.HOME, End: K.END, Delete: K.DEL, MediaPlayPause: K.PLAY,
    MediaStop: K.STOP, MediaTrackNext: K.NEXT, MediaTrackPrevious: K.PREV, Tab: 9 };
const scaleNames = ['Scaling: fit, 4:3, sharp', 'Scaling: whole multiples', 'Scaling: stretch to the window'];

function message(s, err) { withStr(s, p => X.web_message(p, err ? 1 : 0)); }

function toggleFullscreen() {
    if (document.fullscreenElement) document.exitFullscreen();
    else document.documentElement.requestFullscreen?.().catch(() => { });
}

window.addEventListener('keydown', e => {
    if (dialogOpen()) return;
    if (e.ctrlKey && !['ArrowUp', 'ArrowDown'].includes(e.key) || e.metaKey) return;   // the browser's own
    if (e.altKey) { if (e.key === 'Enter') { toggleFullscreen(); e.preventDefault(); } return; }
    let k = 0;
    if (e.key === 'ArrowUp') k = e.ctrlKey ? K.CUP : K.UP;
    else if (e.key === 'ArrowDown') k = e.ctrlKey ? K.CDOWN : K.DOWN;
    else if (e.key === 'F11') { toggleFullscreen(); e.preventDefault(); return; }
    else if (e.key === 'F2') { X.web_cfg_set_view((X.web_cfg_scale() + 1) % 3, X.web_cfg_scanlines()); message(scaleNames[X.web_cfg_scale()]); e.preventDefault(); return; }
    else if (e.key === 'F3') { const s = !X.web_cfg_scanlines(); X.web_cfg_set_view(X.web_cfg_scale(), s ? 1 : 0); message(s ? 'Scan lines on' : 'Scan lines off'); e.preventDefault(); return; }
    else if (named[e.key] !== undefined) k = named[e.key];
    else if (e.key.length === 1) k = e.key.charCodeAt(0);
    if (!k) return;
    e.preventDefault();
    ensureCtx();
    hideHint();
    X.web_key(k);
});

function toPicture(e) {
    const r = canvas.getBoundingClientRect();
    const dpr = canvas.width / r.width;
    const x = ((e.clientX - r.left) * dpr - dest.x) * 256 / dest.w;
    const y = ((e.clientY - r.top) * dpr - dest.y) * 192 / dest.h;
    return [Math.floor(x), Math.floor(y)];
}
let down = false;
canvas.addEventListener('pointerdown', e => {
    ensureCtx();
    hideHint();
    canvas.setPointerCapture(e.pointerId);
    down = true;
    const [x, y] = toPicture(e);
    X.web_mouse(x, y, e.button === 2 ? 2 : 1);
});
canvas.addEventListener('pointermove', e => { if (down) { const [x, y] = toPicture(e); X.web_mouse(x, y, 6); } });
canvas.addEventListener('pointerup', e => { if (!down) return; down = false; const [x, y] = toPicture(e); X.web_mouse(x, y, 7); });
canvas.addEventListener('pointercancel', () => { down = false; });
canvas.addEventListener('contextmenu', e => e.preventDefault());
canvas.addEventListener('dblclick', e => { const [, y] = toPicture(e); if (y < 0 || y >= 192) toggleFullscreen(); });
canvas.addEventListener('wheel', e => {
    e.preventDefault();
    const [x, y] = toPicture(e);
    X.web_mouse(x, y, e.deltaY < 0 ? 4 : 5);
}, { passive: false });

// ---------------- dropped files ----------------
window.addEventListener('dragover', e => { e.preventDefault(); e.dataTransfer.dropEffect = 'copy'; });
window.addEventListener('drop', async e => {
    e.preventDefault();
    ensureCtx();
    const names = [];
    for (const f of e.dataTransfer.files) {
        if (/\.(m3u8?|pls)$/i.test(f.name)) { const p = '/drop/' + f.name; memFiles.set(p, await f.text()); names.push(p); }
        else if (/\.(opml|xml)$/i.test(f.name)) names.push('/drop/' + f.name);
        else { dropped.push(f); names.push(`drop:${dropped.length - 1}/${f.name}`); }
    }
    if (names.length) withStr(names.join('\n'), p => X.web_drop(p));
});

// ---------------- the station lists' editor (O) ----------------
const dlg = document.getElementById('lists');
const dlgSel = document.getElementById('lists-select');
const dlgText = document.getElementById('lists-text');
const dlgName = document.getElementById('lists-name');
let dlgFile = null;
function dialogOpen() { return dlg.open; }
function listFiles() { return (fsList(radioDir) || []).filter(l => l[0] === 'f' && /\.(m3u8?|pls)$/i.test(l.slice(2))).map(l => l.slice(2)).sort((a, b) => a.localeCompare(b)); }
function showList(name) {
    dlgFile = name;
    dlgSel.innerHTML = '';
    for (const f of listFiles()) { const o = document.createElement('option'); o.value = f; o.textContent = f.replace(/\.(m3u8?|pls)$/i, ''); dlgSel.appendChild(o); }
    if (!dlgFile || !listFiles().includes(dlgFile)) dlgFile = listFiles()[0] || null;
    dlgSel.value = dlgFile || '';
    dlgText.value = dlgFile ? fsGet(radioDir + '/' + dlgFile) || '' : '';
    dlgName.value = dlgFile ? dlgFile.replace(/\.(m3u8?|pls)$/i, '') : '';
    dlgText.disabled = !dlgFile;
}
function saveCurrent() { if (dlgFile) fsSet(radioDir + '/' + dlgFile, dlgText.value.replace(/\r?\n/g, '\r\n')); }
function js_open_lists() { showList(dlgFile); dlg.showModal(); }
dlgSel.addEventListener('change', () => { saveCurrent(); showList(dlgSel.value); });
document.getElementById('lists-new').onclick = () => {
    saveCurrent();
    let n = 'New list', i = 1;
    while (listFiles().includes(n + '.m3u')) n = `New list ${++i}`;
    fsSet(`${radioDir}/${n}.m3u`, '#EXTM3U\r\n#EXTINF:-1,Name of the station\r\nhttps://address.of/the/stream\r\n');
    showList(n + '.m3u');
    dlgName.focus(); dlgName.select();
};
document.getElementById('lists-rename').onclick = () => {
    if (!dlgFile) return;
    const nm = dlgName.value.trim().replace(/[\/\\:*?"<>|]/g, '');
    if (!nm || /^favourites$/i.test(nm) && !/^favourites/i.test(dlgFile)) return;
    const ext = (dlgFile.match(/\.(m3u8?|pls)$/i) || ['.m3u'])[0];
    if (nm + ext === dlgFile) return;
    saveCurrent();
    fsSet(`${radioDir}/${nm}${ext}`, fsGet(`${radioDir}/${dlgFile}`) || '');
    fsDel(`${radioDir}/${dlgFile}`);
    showList(nm + ext);
};
document.getElementById('lists-delete').onclick = () => {
    if (!dlgFile || /^favourites\.m3u$/i.test(dlgFile)) return;
    if (!confirm(`Delete the list "${dlgFile.replace(/\.(m3u8?|pls)$/i, '')}"?`)) return;
    fsDel(`${radioDir}/${dlgFile}`);
    showList(null);
};
document.getElementById('lists-import').onclick = () => {
    const inp = document.createElement('input');
    inp.type = 'file'; inp.accept = '.m3u,.m3u8,.pls'; inp.multiple = true;
    inp.onchange = async () => {
        saveCurrent();
        let last = null;
        for (const f of inp.files) { fsSet(`${radioDir}/${/^favourites\.m3u$/i.test(f.name) ? 'Favourites (imported).m3u' : f.name}`, await f.text()); last = f.name; }
        showList(last);
    };
    inp.click();
};
document.getElementById('lists-export').onclick = () => {
    if (!dlgFile) return;
    saveCurrent();
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([fsGet(`${radioDir}/${dlgFile}`) || ''], { type: 'audio/x-mpegurl' }));
    a.download = dlgFile;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
};
document.getElementById('lists-close').onclick = () => dlg.close();
dlg.addEventListener('close', () => { saveCurrent(); X.web_reload(); save(); canvas.focus(); });

// ---------------- the hint (sound needs a key or a click first) ----------------
const hint = document.getElementById('hint');
function hideHint() { if (hint) hint.hidden = true; }
setTimeout(hideHint, 10000);

// ---------------- the scripts (the tests) ----------------
let scriptRun = null;
let saving = true;
function save() { if (saving) X.web_save(); }
window.picoamp = {
    script(text) {
        return new Promise(ok => { withStr(text, p => X.web_script(p)); scriptRun = ok; });
    },
    fs: { get: fsGet, set: fsSet, list: fsList },
    key: k => X.web_key(k),
    save: () => X.web_save(),
    stopSaving() { saving = false; },
    hideHint,     // (the tests: the settings are changed under it)
    get audio() { return { cur, paused: el.paused, plainPaused: plain.paused, gainL, gainR, eqGains, ctx: ctx && ctx.state }; },
};

// ---------------- start ----------------
let lastFrame = 0;
function loop() {
    lastFrame = performance.now();
    try {
        feedViz();
        X.web_frame();
        if (scriptRun && !X.web_script_step()) { const ok = scriptRun; scriptRun = null; ok(str(X.web_script_out())); }
        draw();
    } catch (e) { console.error(e); }
    requestAnimationFrame(loop);
}

async function start() {
    resize();
    await restoreMusic();
    const imports = {
        wasi_snapshot_preview1: wasi,
        env: {
            js_millis: () => performance.now(),
            js_now: () => Date.now() / 1000,
            js_tz_offset: t => -new Date(t * 1000).getTimezoneOffset() * 60,
            js_log: (p, n) => console.log('[picoamp]', str(p, n)),
            js_fs_size: (p, n) => { const t = fsGet(str(p, n)); return t === null ? -1 : enc.encode(t).length; },
            js_fs_read: (p, n, buf, max) => { const t = fsGet(str(p, n)); if (t === null) return -1; const b = enc.encode(t); const k = Math.min(b.length, max); u8().set(b.subarray(0, k), buf); return k; },
            js_fs_write: (p, n, buf, len) => fsSet(str(p, n), str(buf, len)) ? len : -1,
            js_fs_list: (p, n, out, max) => { const l = fsList(str(p, n)); return l === null ? -1 : writeOut(l.join('\n'), out, max); },
            js_fetch, js_music_pick, js_music_list, js_music_name,
            js_audio_play, js_audio_pause, js_audio_stop, js_audio_seek, js_audio_gain, js_audio_eq, js_audio_meta,
            js_can_play, js_open_lists, js_fullscreen: toggleFullscreen,
            js_geolocate: () => { },
        },
    };
    const r = await WebAssembly.instantiateStreaming(fetch('picoamp.wasm'), imports)
        .catch(() => fetch('picoamp.wasm').then(r => r.arrayBuffer()).then(b => WebAssembly.instantiate(b, imports)));
    wasm = r.instance;
    X = wasm.exports;
    mem = X.memory;
    X._initialize?.();
    X.web_start();
    setInterval(save, 15000);
    // a hidden page gets no frames: the sleep timer and the alarm go on
    setInterval(() => { if (performance.now() - lastFrame > 900) { try { X.web_frame(); } catch (e) { console.error(e); } } }, 1000);
    addEventListener('pagehide', save);
    document.addEventListener('visibilitychange', () => { if (document.hidden) save(); });
    canvas.focus();
    requestAnimationFrame(loop);
    document.body.classList.add('ready');
}
start().catch(e => {
    console.error(e);
    document.getElementById('fail').hidden = false;
});
})();
