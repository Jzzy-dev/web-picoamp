// picoamp.js - PicoAmp in the browser: the page around the C program
// (picoamp.wasm). It draws the picture, takes the keys and the mouse, plays
// the sound (an <audio> element through Web Audio: equalizer, balance,
// volume, analysers for the visualiser), keeps the settings and station
// lists in the browser's storage, fetches for the program and reads the
// music folder the person picks.
'use strict';

(() => {
const params = new URLSearchParams(location.search);
// the relay (relay/worker.js: a Cloudflare Worker of one's own): the sites that do not let other
// pages read them (Yahoo Finance for the markets, the news' feeds and stories, stations' status
// pages for the song titles) come through it. In the address, kept in the browser:
// relay=https://... another one, relay=off none, relay=default RELAY_DEFAULT again.
const RELAY_DEFAULT = 'https://picoamp-relay.jzzycool.workers.dev/';
const relay = (() => {
    const ok = u => /^https:\/\//i.test(u) || /^http:\/\/(localhost|127\.0\.0\.1)(:\d+)?\//i.test(u);
    let r = params.get('relay');
    if (r !== null && r !== 'off' && !ok(r)) r = 'default';
    try {
        if (r === 'default') localStorage.removeItem('picoamp-relay');
        else if (r !== null) localStorage.setItem('picoamp-relay', r);
        r = localStorage.getItem('picoamp-relay');
    } catch { if (r === 'default') r = null; }
    r = r || RELAY_DEFAULT;
    return r === 'off' ? '' : r.replace(/\?url=$/, '').replace(/\/?$/, '/');
})();
const relayed = u => relay + '?url=' + encodeURIComponent(u);
// the Sonos bridge: PicoAmp for Windows (its link server, K), which asks the speakers for this page.
// bridge=http://<the PC>:8765 in the address (kept; bridge=off forgets it); the page's own address
// when the PC itself serves it (http://<the PC>:8765/web/); else none named (the Sonos entry then
// asks http://localhost:8765 when it is opened).
const bridge = (() => {
    let b = params.get('bridge');
    try {
        if (b === 'off') localStorage.removeItem('picoamp-bridge');
        else if (b && /^https?:\/\/[^/]+/i.test(b)) localStorage.setItem('picoamp-bridge', b.replace(/\/+$/, ''));
        b = localStorage.getItem('picoamp-bridge');
    } catch { if (b === 'off') b = null; }
    if (!b && location.protocol === 'http:' && location.pathname.startsWith('/web/')) b = location.origin;
    return b || '';
})();
// an http address of this network from a page on https (Chrome: the person is asked once)
const isLocal = u => /^http:\/\/(localhost|127\.|10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|[^/]*\.local[:/])/i.test(u);
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
if (bridge) env.push(`PICOAMP_BRIDGE=${bridge}`);
const lineBuf = { 1: '', 2: '' };
const wasi = {
    // the time (C's time(), clock()): realtime, else the page's own clock; nanoseconds
    clock_time_get(id, precision, tp) {
        const ns = id === 0 ? BigInt(Date.now()) * 1000000n : BigInt(Math.round(performance.now() * 1e6));
        new DataView(mem.buffer).setBigUint64(tp, ns, true);
        return 0;
    },
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

function pickWithInput(dflt) {
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
            if (!files.length) { ok(null); return; }
            const root = { name: files[0].webkitRelativePath.split('/')[0] || dflt || 'Music', kind: 'dir', children: new Map() };
            for (const f of files) {
                const parts = f.webkitRelativePath.split('/').slice(1);
                let d = root;
                for (let i = 0; i < parts.length - 1; i++) {
                    if (!d.children.has(parts[i])) d.children.set(parts[i], { name: parts[i], kind: 'dir', children: new Map() });
                    d = d.children.get(parts[i]);
                }
                d.children.set(parts[parts.length - 1], { name: parts[parts.length - 1], kind: 'file', file: f });
            }
            ok({ name: root.name, tree: root });
        };
        inp.addEventListener('cancel', () => { if (!done) { inp.remove(); ok(null); } });
        inp.click();
    });
}

function js_music_pick() {
    if (window.showDirectoryPicker) {
        musicPick = window.showDirectoryPicker({ id: 'picoamp-music', mode: 'read', startIn: 'music' })
            .then(async h => { music = { name: h.name, handle: h }; await idbSet('music', h).catch(() => { }); return true; })
            .catch(() => false);
    } else musicPick = pickWithInput('Music').then(root => { if (root) music = root; return !!root; });
}

async function musicDir(path) { return rootDir(music, path, 'music'); }
async function rootDir(root, path, what) {
    if (!root) throw new Error(`No ${what} folder chosen`);
    const parts = path ? path.split('/') : [];
    if (root.tree) {
        let d = root.tree;
        for (const p of parts) { d = d.children.get(p); if (!d || d.kind !== 'dir') throw new Error('Folder not found'); }
        return { list: async () => [...d.children.values()].map(c => [c.kind === 'dir' ? 'd' : 'f', c.name]) };
    }
    const h0 = root.handle;
    if (h0.queryPermission && (await h0.queryPermission({ mode: 'read' })) !== 'granted') {
        if ((await h0.requestPermission({ mode: 'read' })) !== 'granted') throw new Error(`The ${what} folder may not be read`);
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

// ---------------- the pictures' folder (I), picked apart from the music's ----------------
let pics = null;               // { name, handle }, { name, tree } or { name, net: { base, q } } (from the network)
let picsPick = null;
async function restorePics() {
    try {
        const n = JSON.parse(localStorage.getItem('picoamp-pics-net') || 'null');
        if (n && n.net) { pics = n; return; }
    } catch { }
    try {
        const h = await idbGet('pictures');
        if (h && h.kind === 'directory') pics = { name: h.name, handle: h };
    } catch { }
}
// ---- pictures from the network: a Windows or NAS share, a media server (the Android app does those
// two: /net/ there) and PicoAmp for Windows (its MSX link's port: /pics/)
const hasNet = () => !!(window.PicoAmpAndroid && window.PicoAmpAndroid.getVolume) || params.get('netpics') === '1';
function netSources() { try { return JSON.parse(localStorage.getItem('picoamp-pics-sources') || '[]'); } catch { return []; } }
function setNetSources(a) { try { localStorage.setItem('picoamp-pics-sources', JSON.stringify(a.slice(0, 12))); } catch { } }
function netOf(src) {
    if (src.kind === 'win') return { base: `http://${src.host}/pics/`, q: '' };
    return { base: (params.get('netbase') || '/net/'), q: 'src=' + encodeURIComponent(src.src) + '&' };
}
async function netFetch(net, what, path) {
    const r = await fetch(`${net.base}${what}?${net.q}path=${encodeURIComponent(path)}`, { cache: 'no-store' });
    if (!r.ok) throw new Error((await r.text().catch(() => '')).trim().slice(0, 80) || `The network folder said ${r.status}`);
    return r;
}
function useNet(src) {
    pics = { name: src.label, net: netOf(src) };
    try { localStorage.setItem('picoamp-pics-net', JSON.stringify(pics)); } catch { }
    setNetSources([src, ...netSources().filter(o => o.label !== src.label)]);
    return true;
}
function smbSource() {
    const where = prompt('The share: \\\\PC\\Pictures or \\\\NAS\\photo\\Holidays (a folder in it too)');
    if (!where || !where.trim()) return null;
    const parts = where.trim().replace(/^smb:\/\//i, '').replace(/\//g, '\\').replace(/^\\+/, '').split('\\').filter(Boolean);
    if (parts.length < 2) { alert('A share is \\\\PC\\share: the PC (or NAS) and the share'); return null; }
    const user = prompt('The user name (DOMAIN\\user; empty: as a guest)', '') ?? '';
    const pass = user ? (prompt('The password', '') ?? '') : '';
    const cred = user ? encodeURIComponent(user) + (pass ? ':' + encodeURIComponent(pass) : '') + '@' : '';
    return { kind: 'smb', label: '\\\\' + parts.join('\\'), src: `smb://${cred}${parts[0]}/${parts.slice(1).map(encodeURIComponent).join('/')}` };
}
function winSource() {
    let h = prompt('The PC with PicoAmp for Windows: its address (and the MSX link\'s port, 8765 if not said), e.g. 192.168.1.20', '');
    if (!h || !h.trim()) return null;
    h = h.trim().replace(/^https?:\/\//, '').replace(/\/.*$/, '');
    if (!/:\d+$/.test(h)) h += ':8765';
    return { kind: 'win', label: 'PicoAmp for Windows on ' + h, host: h };
}
// the choice: a list over the page (the remote's arrows and OK, or clicks); resolves true when chosen
let netPick = null;               // { el, items: [[label, act, saved]], i, done }
function netPickShow() {
    if (!netPick) return;
    const list = netPick.el.querySelector('.items');
    list.replaceChildren(...netPick.items.map(([label], i) => {
        const d = document.createElement('div');
        d.textContent = label;
        if (i === netPick.i) d.className = 'on';
        d.onclick = () => { netPick.i = i; netPickAct('ok'); };
        return d;
    }));
    list.children[netPick.i]?.scrollIntoView({ block: 'nearest' });
}
function netPickClose(ok) {
    if (!netPick) return;
    const p = netPick;
    netPick = null;
    p.el.remove();
    p.done(ok);
}
function netPickAct(a) {
    const p = netPick, n = p.items.length;
    if (a === 'up' || a === 'left') p.i = (p.i + n - 1) % n;
    else if (a === 'down' || a === 'right') p.i = (p.i + 1) % n;
    else if (a === 'back' || a === 'menu') { netPickClose(false); return; }
    else if (a === 'ok') {
        const r = p.items[p.i][1]();
        if (r && r.then) r.then(ok => { if (ok) netPickClose(true); });
        else if (r) netPickClose(true);
        return;
    }
    else if (a === 'longok' && p.items[p.i][2]) {
        // a remembered one: forgotten
        const lab = p.items[p.i][0].replace(/^\u2605 /, '');
        setNetSources(netSources().filter(o => o.label !== lab));
        p.items.splice(p.i, 1);
        p.i = Math.min(p.i, p.items.length - 1);
    }
    netPickShow();
}
function netPickOpen() {
    return new Promise(done => {
        const el = document.createElement('div');
        el.id = 'netpick';
        el.setAttribute('role', 'menu');
        el.innerHTML = '<h2>Pictures from</h2><div class="items"></div><p>OK: choose &middot; OK held: forget a remembered one &middot; Back: close</p>';
        document.body.append(el);
        const local = () => {
            // (the folder picker's answer is the choice's)
            const p = netPick;
            netPick = null;
            p.el.remove();
            const forgetNet = () => { try { localStorage.removeItem('picoamp-pics-net'); } catch { } };
            (window.showDirectoryPicker
                ? window.showDirectoryPicker({ id: 'picoamp-pictures', mode: 'read', startIn: 'pictures' })
                    .then(async h => { pics = { name: h.name, handle: h }; forgetNet(); await idbSet('pictures', h).catch(() => { }); return true; })
                    .catch(() => false)
                : pickWithInput('Pictures').then(root => { if (root) { pics = root; forgetNet(); } return !!root; })).then(done);
            return false;
        };
        const items = [];
        for (const s of netSources()) if (s.kind !== 'smb' || hasNet()) items.push(['\u2605 ' + s.label, () => useNet(s), true]);
        const at = items.length;
        if (hasNet()) items.push(['A Windows or NAS share\u2026', () => { const s = smbSource(); return s ? useNet(s) : false; }]);
        items.push(['PicoAmp for Windows (its pictures)\u2026', () => { const s = winSource(); return s ? useNet(s) : false; }]);
        if (!tvMode) items.push(['A folder on this device\u2026', local]);
        if (hasNet()) items.push(['(looking for media servers\u2026)', () => false]);
        netPick = { el, items, i: 0, done };
        netPickShow();
        if (hasNet()) fetch(`${params.get('netbase') || '/net/'}servers`).then(r => r.json()).catch(() => []).then(list => {
            if (!netPick || netPick.el !== el) return;
            const k = items.findIndex(it => it[0].startsWith('(looking'));
            const found = (list || []).filter(sv => !netSources().some(o => o.src === sv.src))
                .map(sv => ['Media server: ' + sv.name, () => useNet({ kind: 'dlna', label: 'Media server ' + sv.name, src: sv.src })]);
            items.splice(k, 1, ...(found.length ? found : [['(no media servers found)', () => false]]));
            items.splice(at, 0, ...items.splice(items.length - (found.length || 1)));     // (the servers after the remembered ones)
            netPickShow();
        });
    });
}
function js_pics_pick() {
    if (tvMode || hasNet()) { picsPick = netPickOpen(); return; }
    if (window.showDirectoryPicker) {
        picsPick = window.showDirectoryPicker({ id: 'picoamp-pictures', mode: 'read', startIn: 'pictures' })
            .then(async h => { pics = { name: h.name, handle: h }; await idbSet('pictures', h).catch(() => { }); return true; })
            .catch(() => false);
    } else picsPick = pickWithInput('Pictures').then(root => { if (root) pics = root; return !!root; });
}
function js_pics_list(id, p, n) {
    const path = str(p, n);
    const pick = picsPick;
    (async () => {
        if (pick) { const ok = await pick; if (picsPick === pick) picsPick = null; if (!ok && !pics) throw new Error('No folder chosen'); }
        const items = pics && pics.net ? (await (await netFetch(pics.net, 'list', path)).text()).split('\n').filter(l => /^[df] ./.test(l)).map(l => [l[0], l.slice(2)])
                                       : await (await rootDir(pics, path, 'pictures')).list();
        return items.filter(([, nm]) => !nm.startsWith('.')).map(([k, nm]) => `${k} ${nm}`).join('\n');
    })().then(text => withStr(text, p2 => X.dir_done(id, p2, 1)),
              e => withStr(String(e.message || e), p2 => X.dir_done(id, p2, 0)));
}
function js_pics_name(out, max) { return pics ? writeOut(pics.name, out, max) : 0; }
async function picsFile(path) {
    const parts = path.split('/');
    const name = parts.pop();
    if (!pics) throw new Error('No pictures folder');
    if (pics.net) return (await netFetch(pics.net, 'file', path)).blob();
    if (pics.tree) {
        let d = pics.tree;
        for (const p of parts) d = d && d.children.get(p);
        const f = d && d.children.get(name);
        if (!f) throw new Error('File not found');
        return f.file;
    }
    let h = pics.handle;
    for (const p of parts) h = await h.getDirectoryHandle(p);
    return (await h.getFileHandle(name)).getFile();
}
// a picture decoded by the browser, made at most mw x mh (its shape kept; transparent parts black):
// the RGB rows to the program (web_pic_rgb; w 0: it could not)
async function picToProgram(id, blob, mw, mh) {
    let w = 0, h = 0, rgb = null;
    try {
        const bm = await createImageBitmap(blob);
        const k = Math.min(1, mw / bm.width, mh / bm.height);
        w = Math.max(1, Math.round(bm.width * k)); h = Math.max(1, Math.round(bm.height * k));
        const c = document.createElement('canvas');
        c.width = w; c.height = h;
        const cx = c.getContext('2d');
        cx.fillStyle = '#000';
        cx.fillRect(0, 0, w, h);
        cx.imageSmoothingQuality = 'high';
        cx.drawImage(bm, 0, 0, w, h);
        const d = cx.getImageData(0, 0, w, h).data;
        rgb = new Uint8Array(w * h * 3);
        for (let i = 0, j = 0; i < d.length; i += 4, j += 3) { rgb[j] = d[i]; rgb[j + 1] = d[i + 1]; rgb[j + 2] = d[i + 2]; }
    } catch { w = h = 0; rgb = null; }
    if (!rgb) { X.web_pic_rgb(id, 0, 0, 0); return; }
    const [q] = put(rgb);
    try { X.web_pic_rgb(id, w, h, q); } finally { X.web_free(q); }
}
function js_pic_file(id, p, n, mw, mh) {
    const path = str(p, n);
    picsFile(path).then(f => picToProgram(id, f, mw, mh), () => X.web_pic_rgb(id, 0, 0, 0));
}
function js_pic_bytes(id, p, n, mw, mh) {
    picToProgram(id, new Blob([u8().slice(p, p + n)]), mw, mh);
}

// ---------------- covers ----------------
// an image in the track's folder (as the MSX engine looks): cover, folder, front, album, albumart,
// AlbumArtSmall (.jpg, .jpeg, .png); its bytes to the program
const coverNames = ['cover', 'folder', 'front', 'album', 'albumart', 'albumartsmall'];
async function folderCover(src, g) {
    if (!src.startsWith('music:')) return;
    const rel = src.slice(6), sl = rel.lastIndexOf('/');
    const dir = sl < 0 ? '' : rel.slice(0, sl);
    try {
        const items = await (await musicDir(dir)).list();
        let best = null, rank = 99;
        for (const [k, nm] of items) {
            const m = k === 'f' && nm.toLowerCase().match(/^(.*)\.(jpe?g|png)$/);
            const i = m ? coverNames.indexOf(m[1]) : -1;
            if (i >= 0 && i < rank) { rank = i; best = nm; }
        }
        if (!best || g !== gen) return;
        const f = await musicFile('music:' + (dir ? dir + '/' : '') + best);
        const b = new Uint8Array(await f.arrayBuffer());
        if (g !== gen) return;
        const [p, n] = put(b);
        try { X.web_cover_bytes(p, n); } finally { X.web_free(p); }
    } catch { }
}

// a picture the program's JPEG decoder cannot read (PNG, progressive JPEG ...): the browser's,
// made at most 320 pixels big; the RGB rows back to the program
function js_decode_image(p, n) {
    const bytes = u8().slice(p, p + n);
    const g = gen;
    if (!window.createImageBitmap) return 0;
    createImageBitmap(new Blob([bytes])).then(bm => {
        if (g !== gen) return;
        const k = Math.min(1, 320 / Math.max(bm.width, bm.height));
        const w = Math.max(1, Math.round(bm.width * k)), h = Math.max(1, Math.round(bm.height * k));
        const c = document.createElement('canvas');
        c.width = w; c.height = h;
        const cx = c.getContext('2d');
        cx.drawImage(bm, 0, 0, w, h);
        const d = cx.getImageData(0, 0, w, h).data;
        const rgb = new Uint8Array(w * h * 3);
        for (let i = 0, j = 0; i < d.length; i += 4, j += 3) {
            // (transparent: on the player's navy)
            const a = d[i + 3] / 255;
            rgb[j] = d[i] * a; rgb[j + 1] = d[i + 1] * a; rgb[j + 2] = d[i + 2] * a + 73 * (1 - a);
        }
        const [q] = put(rgb);
        try { X.web_cover_rgb(w, h, q); } finally { X.web_free(q); }
    }).catch(() => { });
    return 1;
}

// ---------------- a station's logo ----------------
// as the MSX engine 0.15 finds it: Radio Browser is asked for the station (by its directory id,
// else by its address, else by its exact name), its "favicon" is fetched (straight, or through
// an image proxy when its server allows no other page to read it) and kept in the browser's
// cache, "no logo" too; the bytes go to the program (web_logo)
const logoProxy = params.get('logoproxy') || 'https://wsrv.nl/?w=160&h=160&fit=inside&output=png&url=';
// the logos and covers kept in the browser: at most 100 and 200 (keeplogos=, keepcovers= in the
// address; 0: all); past that the ones shown longest ago go (down to 90 %); each keeps the time it
// was last shown (x-time); a "none" (no picture found) is asked again after 30 days
const keepOf = (k, d) => { const n = parseInt(params.get(k), 10); return isNaN(n) ? d : Math.max(0, n); };
const ART = { logo: { pre: '/picoamp-logo/', keep: keepOf('keeplogos', 100) },
              cover: { pre: '/picoamp-cover/', keep: keepOf('keepcovers', 200) } };
const NONE_MS = 30 * 864e5, pruning = new Set();
function artPut(cache, key, b, t) {
    const h = { 'x-time': String(t || Date.now()) };
    if (!b) h['x-none'] = '1';
    return cache.put(key, new Response(b || '', { headers: h }));
}
// undefined: not known (ask); null: none; else the bytes (and shown now)
async function artGet(cache, key) {
    const hit = await cache.match(key);
    if (!hit) return undefined;
    const t = +hit.headers.get('x-time') || 0;
    if (hit.headers.get('x-none')) return Date.now() - t > NONE_MS ? undefined : null;
    const b = new Uint8Array(await hit.arrayBuffer());
    if (Date.now() - t > 3600e3) artPut(cache, key, b).catch(() => { });   // (to the hour is enough)
    return b;
}
async function artPrune(cache, kind) {
    const a = ART[kind], pre = location.origin + a.pre;
    if (!a.keep || pruning.has(kind)) return 0;
    pruning.add(kind);
    try {
        const keys = (await cache.keys()).filter(r => r.url.startsWith(pre));
        if (keys.length <= a.keep) return 0;
        const ts = await Promise.all(keys.map(async r => { const m = await cache.match(r); return [r, (m && +m.headers.get('x-time')) || 0]; }));
        ts.sort((x, y) => x[1] - y[1]);
        const go = ts.slice(0, keys.length - Math.floor(a.keep * 9 / 10));
        for (const [r] of go) await cache.delete(r);
        return go.length;
    } finally { pruning.delete(kind); }
}
let logoTok = 0;
function isPicture(b) {
    if (b.length < 8) return false;
    const s = String.fromCharCode(...b.slice(0, 4));
    return (b[0] === 0xFF && b[1] === 0xD8) || s === '\x89PNG' || s === 'GIF8' || s === 'RIFF' ||
           (b[0] === 0 && b[1] === 0 && b[2] === 1 && b[3] === 0) || (b[0] === 0x42 && b[1] === 0x4D);
}
async function getBytes(u) {
    try {
        const r = await fetch(u, { signal: AbortSignal.timeout ? AbortSignal.timeout(8000) : undefined });
        return r.ok ? new Uint8Array(await r.arrayBuffer()) : null;
    } catch { return null; }
}
function js_station_logo(up, un, ip, inn, np, nn, rp, rn) {
    const url = str(up, un), uuid = str(ip, inn), name = str(np, nn), rb = str(rp, rn);
    const tok = ++logoTok;
    const done = b => {
        if (tok !== logoTok) return;
        const [p, n] = b ? put(b) : [0, 0];
        try { X.web_logo(p, n); } finally { if (p) X.web_free(p); }
    };
    (async () => {
        const key = location.origin + '/picoamp-logo/' + encodeURIComponent(url);
        let cache = null;
        try {
            cache = await caches.open('picoamp-logos');
            const got = await artGet(cache, key);
            if (got !== undefined) { done(got); return; }
        } catch { cache = null; }
        let fav = '', answered = false;
        const ways = [uuid && '/json/stations/byuuid/' + encodeURIComponent(uuid),
                      '/json/stations/byurl?url=' + encodeURIComponent(url),
                      name && '/json/stations/search?name=' + encodeURIComponent(name) + '&nameExact=true&limit=1&order=votes&reverse=true&hidebroken=true'];
        for (const w of ways) {
            if (!w || fav) continue;
            const b = await getBytes(rb + w);
            if (tok !== logoTok) return;
            if (!b) continue;
            answered = true;
            try { const j = JSON.parse(new TextDecoder().decode(b)); fav = (Array.isArray(j) && j[0] && j[0].favicon) || ''; } catch { }
            if (!/^https?:\/\//.test(fav)) fav = '';
        }
        let img = null, known = answered;
        if (fav) {
            img = await getBytes(fav);
            if (!img || !isPicture(img)) img = logoProxy ? await getBytes(logoProxy + encodeURIComponent(fav)) : null;
            if (img && !isPicture(img)) img = null;
            else if (!img) known = false;           // (not reached now: asked again next time)
        }
        if (tok !== logoTok) return;
        if (cache && known) {
            try { await artPut(cache, key, img); artPrune(cache, 'logo').catch(() => { }); } catch { }
        }
        done(img);
    })();
}

// a song's (or an album's) cover, as the MSX engine 0.16 finds it: the iTunes search (as JSONP:
// it allows no other page to read it otherwise), its "artworkUrl100" (as 300 x 300; straight, or
// through the image proxy), kept in the browser's cache ("none" too); the bytes go to web_song
const itunesBase = params.get('itunes') || 'https://itunes.apple.com';
let songTok = 0, jsonpN = 0;
function itunes(term, album) {
    return new Promise(res => {
        const cb = 'picoampItunes' + (++jsonpN), sc = document.createElement('script');
        let over = false;
        const end = v => { if (over) return; over = true; delete window[cb]; sc.remove(); res(v); };
        window[cb] = j => end(j);
        sc.onerror = () => end(null);
        setTimeout(() => end(null), 10000);
        sc.src = itunesBase + '/search?term=' + encodeURIComponent(term) + '&media=music&entity=' + (album ? 'album' : 'song') + '&limit=1&callback=' + cb;
        document.head.appendChild(sc);
    });
}
function js_song_cover(ap, an, wp, wn, album) {
    const term = str(ap, an) + ' ' + str(wp, wn);
    const tok = ++songTok;
    const done = b => {
        if (tok !== songTok) return;
        const [p, n] = b ? put(b) : [0, 0];
        try { X.web_song(p, n); } finally { if (p) X.web_free(p); }
    };
    (async () => {
        const key = location.origin + '/picoamp-cover/' + encodeURIComponent(term.toLowerCase());
        let cache = null;
        try {
            cache = await caches.open('picoamp-logos');
            const got = await artGet(cache, key);
            if (got !== undefined) { done(got); return; }
        } catch { cache = null; }
        const j = await itunes(term, album);
        if (tok !== songTok) return;
        let art = (j && Array.isArray(j.results) && j.results[0] && j.results[0].artworkUrl100) || '';
        art = art.replace('100x100bb', '600x600bb');   // (600: the full screen cover too)
        let img = null, known = !!j;
        if (art) {
            img = await getBytes(art);
            if (!img || !isPicture(img)) img = logoProxy ? await getBytes(logoProxy + encodeURIComponent(art)) : null;
            if (img && !isPicture(img)) img = null;
            else if (!img) known = false;
        }
        if (tok !== songTok) return;
        if (cache && known) {
            try { await artPut(cache, key, img); artPrune(cache, 'cover').catch(() => { }); } catch { }
        }
        done(img);
    })();
}

// ---------------- the web ----------------
const NO_RELAY = 'needs the relay (relay= in the page\'s address: see the README)';
function js_fetch(id, p, n) {
    let url = str(p, n);
    // "relay:" before the address: through the relay, or not at all
    if (url.startsWith('relay:')) {
        if (!relay) {
            const [bp] = put(new Uint8Array(0)), [ep] = put(NO_RELAY);
            queueMicrotask(() => { try { X.net_done(id, 0, bp, 0, ep); } finally { X.web_free(bp); X.web_free(ep); } });
            return;
        }
        url = relayed(url.slice(6));
    }
    const opt = { cache: 'no-store', signal: AbortSignal.timeout ? AbortSignal.timeout(20000) : undefined };
    if (isLocal(url) && location.protocol === 'https:') opt.targetAddressSpace = /^http:\/\/(localhost|127\.)/i.test(url) ? 'loopback' : 'local';
    fetch(url, opt).then(async r => {
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
// light sound (the TV's remote: Up at the top, Sound): stations as they are, without Web Audio (no
// spectrum, no equalizer), for a TV too slow for it
let lightSound = false;
try { lightSound = localStorage.getItem('picoamp-light') === '1'; } catch (e) { }
function setLightSound(on) {
    lightSound = on;
    try { localStorage.setItem('picoamp-light', on ? '1' : '0'); } catch (e) { }
    note(on ? 'Light sound: stations without the spectrum and equalizer (from the next one)' : 'Full sound: the spectrum and equalizer (from the next station)');
}

function ensureCtx() {
    if (ctx) { if (ctx.state === 'suspended') ctx.resume(); return; }
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return;
    // (the TV: larger buffers, the sound before the speed of the knobs: a slow TV's CPU otherwise crackles)
    ctx = new AC(tvMode ? { latencyHint: 'playback' } : undefined);
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

function js_audio_stop() { gen++; npStop(); stopElements(); cur = null; mediaState('none'); }
function js_audio_pause(on) {
    const a = cur && cur.usingPlain ? plain : el;
    if (on) a.pause(); else { ensureCtx(); a.play().catch(() => { }); }
    mediaState(on ? 'paused' : 'playing');
}
function js_audio_seek(s) { if (cur && !cur.usingPlain) el.currentTime = s; }

function cErr(msg) { withStr(msg, p => X.web_audio_error(p)); }

// ---------------- what a station plays ----------------
// the browser's audio element keeps a station's song titles to itself, and most stations let no
// other page read them: they are asked for beside it, every 20 s, in the first way that answers:
//   * the station's own service: Triton Digital's now playing (StreamTheWorld streams: many commercial
//     stations), SomaFM's, laut.fm's, Zeno's;
//   * the server's status: Icecast (status-json.xsl), SHOUTcast 2 (stats?json=1), SHOUTcast 1 (7.html);
//   * the stream itself for a moment (Icy-MetaData: 1, up to its first title; where it is allowed);
//   * last, onlineradiobox.com's now playing, by the station's name and country (Radio Browser's): it
//     lists most stations of the world, and lets other pages read it.
// (A relay that adds CORS can be given: npproxy=https://...?url= ; the status pages go through it too.)
const npProxies = [...(relay ? [relay + '?url='] : []), ...(params.get('npproxy') || '').split(' ').filter(x => x)];
const npRb = params.get('rb') || 'https://all.api.radio-browser.info';
const npStations = new Map();           // a stream's address -> its name and country (Radio Browser), or null
let npTimer = 0, npGen = 0, npLast = '';
function npStop() { clearTimeout(npTimer); npTimer = 0; npGen++; npLast = ''; }
function timeout(ms) { return AbortSignal.timeout ? AbortSignal.timeout(ms) : undefined; }
const insecurePage = () => location.protocol === 'https:';
// a page's text: straight (when the browser may), else through each relay; null: none answered
async function npGet(url, how) {
    const tries = [];
    if (!(insecurePage() && /^http:/i.test(url))) tries.push(['direct', url]);
    for (const p of npProxies) tries.push([p, p + encodeURIComponent(url)]);
    for (const [via, u] of tries) {
        try {
            const r = await fetch(u, { signal: timeout(7000), cache: 'no-store' });
            if (!r.ok) continue;
            const t = await r.text();
            if (t && t.length < 512 * 1024) { if (how) how.via = via === 'direct' ? 'direct' : 'relay'; return t; }
        } catch { }
    }
    return null;
}
// (a title: text a person reads, not a stream's bytes a relay sent instead)
// (and not a station's own numbers for a song: "288527 - 406237")
const clean = t => { const c = String(t || '').replace(/\s+/g, ' ').trim(); return c.length <= 200 && !/[\u0000-\u0008\u000e-\u001f\ufffd]/.test(c) && !/^[\d\s\-\u2013.]*$/.test(c) ? c : ''; };
function npIcecast(text, u) {
    try {
        const j = JSON.parse(text);
        let s = j && j.icestats && j.icestats.source;
        if (!s) return null;
        if (!Array.isArray(s)) s = [s];
        const m = s.find(x => x.listenurl && new URL(x.listenurl, u.origin).pathname === u.pathname) || (s.length === 1 ? s[0] : null);
        if (!m) return null;
        const t = m.artist && m.title ? m.artist + ' - ' + m.title : (m.title || m.yp_currently_playing || '');
        return clean(t) || null;
    } catch { return null; }
}
function npShout2(text) {
    try { const j = JSON.parse(text); return clean(j.songtitle) || null; } catch { return null; }
}
function npShout1(text) {
    const m = /<body[^>]*>(\d+,\d,\d+,\d+,\d+,\d+,[\s\S]*?)<\/body>/i.exec(text);
    return m ? clean(m[1].split(',').slice(6).join(',')) || null : null;
}
// a StreamTheWorld stream's mount (RADIO538, KINK_SC ...)
function tritonMount(url) {
    const m = /streamtheworld\.com\/(?:api\/livestream-redirect\/)?([A-Za-z0-9_]+?)(?:_SC|AAC|\.mp3|\.aac|$|\/)/i.exec(url) ||
              /streamtheworld\.com\/.*?([A-Z0-9_]{3,})(?:\.mp3|\.aac|AAC)/.exec(url);
    return m ? m[1] : null;
}
function npTriton(text) {
    const a = /<property name="track_artist_name"><!\[CDATA\[(.*?)\]\]>/.exec(text), t = /<property name="cue_title"><!\[CDATA\[(.*?)\]\]>/.exec(text);
    if (!t) return null;
    return clean(a && a[1] ? a[1] + ' - ' + t[1] : t[1]) || null;
}
// a JSON's fields
async function npJson(url, how) { const t = await npGet(url, how); try { return t ? JSON.parse(t) : null; } catch { return null; } }
// Zeno: its server-sent events, the first one
async function npZeno(mount) {
    const ac = new AbortController();
    const tm = setTimeout(() => ac.abort(), 8000);
    try {
        const r = await fetch(`https://api.zeno.fm/mounts/metadata/subscribe/${mount}`, { signal: ac.signal, cache: 'no-store' });
        if (!r.ok || !r.body) return null;
        const rd = r.body.getReader(), dec = new TextDecoder();
        let buf = '';
        for (;;) {
            const { value, done } = await rd.read();
            if (done) return null;
            buf += dec.decode(value, { stream: true });
            const m = /^data:(.*)$/m.exec(buf);
            if (m) { try { return clean(JSON.parse(m[1]).streamTitle) || null; } catch { return null; } }
            if (buf.length > 65536) return null;
        }
    } catch { return null; } finally { clearTimeout(tm); ac.abort(); }
}
// onlineradiobox's names for a station: its name in a few forms ("538 Radio": 538radio, 538)
function orbAliases(name) {
    let n = name.normalize('NFKD').replace(/[\u0300-\u036f]/g, '').toLowerCase();
    n = n.replace(/\(.*?\)|\[.*?\]|\|.*$| - .*$/g, '').trim();
    const w = n.match(/[a-z0-9]+/g) || [];
    const all = w.join('');
    const out = [all, all.replace(/^radio/, ''), all.replace(/radio$/, ''),
                 w.filter(x => !['radio', 'fm', 'hd', 'hq', 'mp3', 'aac', 'flac', 'live', 'stream', 'online'].includes(x)).join(''), w.join('-')];
    return [...new Set(out.filter(x => x.length >= 2))];
}
async function npStation(url) {
    if (npStations.has(url)) return npStations.get(url);
    let st = null;
    try {
        const r = await fetch(npRb + '/json/stations/byurl?url=' + encodeURIComponent(url), { signal: timeout(8000) });
        const j = r.ok ? await r.json() : null;
        if (Array.isArray(j) && j[0] && j[0].countrycode) st = { name: j[0].name, cc: j[0].countrycode.toLowerCase(), alias: null };
    } catch { }
    npStations.set(url, st);
    return st;
}
async function npOrb(url, how) {
    const st = await npStation(url);
    if (!st) return null;
    for (const a of st.alias ? [st.alias] : orbAliases(st.name)) {
        try {
            const r = await fetch(`https://scraper2.onlineradiobox.com/${st.cc}.${a}`, { signal: timeout(7000), cache: 'no-store' });
            const j = r.ok ? await r.json() : null;
            const t = j && clean(j.title);
            if (t) { st.alias = a; how.via = `${st.cc}.${a}`; return t; }
        } catch { }
    }
    return null;
}
// the ways for a station, best first: [name, function() -> title or null]
function npWays(url, how) {
    let u;
    try { u = new URL(url); } catch { return []; }
    const ways = [];
    const mount = tritonMount(url);
    if (mount) ways.push(['triton', async () => { const t = await npGet(`https://np.tritondigital.com/public/nowplaying?mountName=${mount}&numberToFetch=1&eventType=track`, how); return t && npTriton(t); }]);
    if (/(^|\.)somafm\.com$/i.test(u.hostname)) {
        const ch = u.pathname.split('/')[1].replace(/-\d+-(mp3|aac|aacp)$/i, '').replace(/\d+$/, '');
        if (ch) ways.push(['somafm', async () => { const j = await npJson(`https://somafm.com/songs/${ch}.json`, how); const x = j && j.songs && j.songs[0]; return x ? clean((x.artist ? x.artist + ' - ' : '') + x.title) || null : null; }]);
    }
    if (/\.stream\.laut\.fm$/i.test(u.hostname)) {
        const name = u.pathname.split('/')[1];
        if (name) ways.push(['lautfm', async () => { const j = await npJson(`https://api.laut.fm/station/${name}/current_song`, how); return j && j.title ? clean((j.artist && j.artist.name ? j.artist.name + ' - ' : '') + j.title) || null : null; }]);
    }
    if (/^stream\.zeno\.fm$/i.test(u.hostname)) {
        const m = u.pathname.split('/')[1];
        if (m) ways.push(['zeno', async () => { how.via = 'direct'; return npZeno(m); }]);
    }
    ways.push(['icecast', async () => { const t = await npGet(u.origin + '/status-json.xsl', how); return t && npIcecast(t, u); }]);
    ways.push(['icy', async () => { if (insecurePage() && u.protocol === 'http:') return null; how.via = 'direct'; return npIcy(url); }]);
    ways.push(['shoutcast2', async () => { const sid = u.searchParams.get('sid') || '1'; const t = await npGet(`${u.origin}/stats?sid=${sid}&json=1`, how); return t && npShout2(t); }]);
    ways.push(['shoutcast1', async () => { const t = await npGet(u.origin + '/7.html', how); return t && npShout1(t); }]);
    ways.push(['onlineradiobox', async () => npOrb(url, how)]);
    return ways;
}
async function npIcy(url) {
    const ac = new AbortController();
    const tm = setTimeout(() => ac.abort(), 8000);
    try {
        const r = await fetch(url, { headers: { 'Icy-MetaData': '1' }, signal: ac.signal, cache: 'no-store' });
        const mi = parseInt(r.headers.get('icy-metaint') || '0', 10);
        if (!r.ok || !mi || !r.body || mi > 256 * 1024) return null;
        const rd = r.body.getReader();
        let skip = mi, need = -1, meta = null, got = 0;
        for (;;) {
            const { value, done } = await rd.read();
            if (done || !value) return null;
            let i = 0;
            while (i < value.length) {
                if (skip > 0) { const k = Math.min(skip, value.length - i); skip -= k; i += k; continue; }
                if (need < 0) { need = value[i++] * 16; if (!need) return ''; meta = new Uint8Array(need); continue; }
                const k = Math.min(need - got, value.length - i);
                meta.set(value.subarray(i, i + k), got); got += k; i += k;
                if (got === need) {
                    const s = new TextDecoder().decode(meta);
                    const m = /StreamTitle='(.*?)';/.exec(s);
                    return m ? clean(m[1]) : '';
                }
            }
        }
    } catch { return null; } finally { clearTimeout(tm); ac.abort(); }
}
// (the tests and the live check: every way for a station, what each gives: [{ way, via, title }])
async function npProbe(url) {
    const out = [];
    for (const [name] of npWays(url, {})) {
        const how = {};
        const w = npWays(url, how).find(x => x[0] === name);
        let t = null;
        try { t = await w[1](); } catch { }
        out.push({ way: name, via: how.via || '', title: t || '' });
    }
    return out;
}
function npStart(url) {
    npStop();
    const g = npGen;
    let way = null;                     // the way that answered (asked first from then on)
    let misses = 0;
    const tick = async () => {
        let t = null;
        const ways = npWays(url, {});
        if (way) ways.sort((a, b) => (b[0] === way) - (a[0] === way));
        for (const [name, fn] of ways) {
            try { t = await fn(); } catch { t = null; }
            if (g !== npGen) return;
            if (t) { way = name; break; }
        }
        if (t && t !== npLast) { npLast = t; withStr(t, p => X.web_audio_title(p)); }
        misses = t ? 0 : misses + 1;    // (nothing answers: asked less often)
        npTimer = setTimeout(tick, misses >= 3 ? 180000 : misses ? 60000 : 20000);
    };
    npTimer = setTimeout(tick, 1500);
}

function js_audio_play(p, n, isFile) {
    const src = str(p, n);
    ensureCtx();
    stopElements();
    npStop();
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
            let pic = 0;
            try { pic = X.web_tags(bp, bl); } finally { X.web_free(bp); }
            if (!pic && g === gen) folderCover(src, g);
        }).catch(e => { if (g === gen) cErr(String(e.message || e)); });
        return;
    }
    let url = src;
    if (location.protocol === 'https:' && /^http:/i.test(url)) cur.insecure = true;
    if (!cur.insecure) npStart(url);
    if (lightSound) {
        cur.usingPlain = true;
        plain.src = url;
        applyGain();
        plain.play().catch(e => { if (g === gen && e.name !== 'AbortError') failStream(); });
        return;
    }
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
    MUTE: 0x10F, CUP: 0x110, CDOWN: 0x111, F1: 0x120, F5: 0x121 };
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

// ---------------- the device's volume (the Android app) ----------------
// the sound at full here, the device's (the TV's) volume is the volume: shown, and set from here
const dev = window.PicoAmpAndroid && window.PicoAmpAndroid.getVolume ? window.PicoAmpAndroid : null;
let devVol = -1, devFixed = false;
function systemVolume(v) {
    if (!X || !dev) return;
    devVol = devFixed ? 100 : Math.round(v);
    X.web_ext_volume_is(devVol);
}
function deviceVolume() {
    if (!dev) return;
    if (devVol < 0) {
        devFixed = !!dev.volumeFixed();
        X.web_ext_volume(1);
        systemVolume(dev.getVolume());
        return;
    }
    const v = X.web_own_volume();
    if (v === devVol) return;
    if (devFixed) { dev.volumeStep(v > devVol ? 1 : -1); X.web_ext_volume_is(100); }   // (the TV's own, by HDMI-CEC: up or down)
    else { devVol = v; dev.setVolume(v); }
}

function draw() {
    const w = X.web_width(), h = X.web_height();
    if (!img || img.width !== w || img.height !== h) {
        img = sctx.createImageData(w, h);
        small.width = w; small.height = h;
    }
    // the program makes the pixels (the palette, the cover's band and its rows' colours, SCREEN 8)
    new Uint32Array(img.data.buffer).set(new Uint32Array(mem.buffer, X.web_rgb(), w * h));
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
    // (around the picture: the border's colour, navy; black around SCREEN 8's pictures)
    const back = X.web_s8() ? 0 : new Uint32Array(mem.buffer, X.web_palette(), 16)[14];
    g2.fillStyle = `rgb(${back >> 16 & 255},${back >> 8 & 255},${back & 255})`;
    g2.fillRect(0, 0, canvas.width, canvas.height);
    const exact = dest.w === w * kx && dest.h === h * ky;
    g2.imageSmoothingEnabled = !exact;
    g2.imageSmoothingQuality = 'high';
    g2.drawImage(big, dest.x, dest.y, dest.w, dest.h);
    sharpCover(g2, dest);
    deviceVolume();
    tvFrame(g2, dest);
}

// ---------------- the cover, sharp (F5) ----------------
// The cover's own picture over the MSX's 8-bit one, where the program says (the player's square:
// the picture's middle; full screen: whole, the rest black). Decoded once per cover (the browser's).
let shGen = -1, shSrc = 0, shImg = null, shBuf = 0;
function sharpCover(g, d) {
    if (!shBuf) shBuf = X.web_alloc(28);
    const kind = X.web_sharp_cover(shBuf);
    if (!kind) return;
    const o = new Int32Array(mem.buffer, shBuf, 7), [rx, ry, rw, rh, p, n, gen] = o;
    if (gen !== shGen || p !== shSrc) {
        shGen = gen; shSrc = p; shImg = null;
        if (p && n > 0 && window.createImageBitmap) {
            const bytes = new Uint8Array(mem.buffer, p, n).slice();
            createImageBitmap(new Blob([bytes])).then(b => { if (shGen === gen && shSrc === p) shImg = b; }).catch(() => { });
        }
    }
    if (!shImg) return;
    const W = X.web_width(), H = X.web_height(), kx = d.w / W, ky = d.h / H;
    const x = d.x + rx * kx, y = d.y + ry * ky, w = rw * kx, h = rh * ky, iw = shImg.width, ih = shImg.height;
    g.save();
    g.imageSmoothingEnabled = true;
    g.imageSmoothingQuality = 'high';
    if (kind === 1) {
        const m = Math.min(iw, ih);
        g.drawImage(shImg, (iw - m) / 2, (ih - m) / 2, m, m, x, y, w, h);
    } else {
        // (as big as the MSX's: its dot is 1.1 as wide as high, so the picture 10/11 as wide)
        const k = Math.min(w / (iw * 10 / 11), h / ih), dw = iw * 10 / 11 * k, dh = ih * k;
        g.fillStyle = '#000';
        g.fillRect(x, y, w, h);
        g.drawImage(shImg, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh);
    }
    g.restore();
}

// ---------------- keys and the mouse ----------------
const named = { Enter: K.ENTER, Backspace: K.BS, Escape: K.ESC, ArrowLeft: K.LEFT, ArrowRight: K.RIGHT,
    PageUp: K.PGUP, PageDown: K.PGDN, Home: K.HOME, End: K.END, Delete: K.DEL, MediaPlayPause: K.PLAY,
    MediaStop: K.STOP, MediaTrackNext: K.NEXT, MediaTrackPrevious: K.PREV, Tab: 9, F1: K.F1, F5: K.F5 };
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

// ---------------- the TV (the Android app, or ?tv=1): its remote ----------------
// The remote has the arrows, OK, Back, and sometimes play/pause and the like. On the player, a
// white frame shows the part of the screen the arrows are in: Up/Down move it between the remote's
// own row (Menu, Help, Sound full/light, Exit), the seek bar, the picture (spectrum, scope, cover),
// the list and the control row; OK goes into that part:
//   the seek bar  Left/Right: 10 s back / on (at once; a track, not a station)
//   the picture   Left/Right: spectrum, scope, cover; OK on the cover: the cover big (full screen)
//   the list      the arrows and OK as ever; Back: the folder up, at the top out of the list;
//                 OK held: the station into the favourites (in the favourites: out of them)
//   the controls  Left/Right choose (previous, 10 s back, play, pause, stop, 10 s on, next, shuffle,
//                 repeat, EQ, volume), OK does it; on EQ the arrows set the equalizer, on the volume
//                 Left/Right set it (in the app: the TV's own volume)
// Back comes out again (and at the top leaves the app: 'exit'). OK held elsewhere (or the menu key)
// opens the menu: the list's places and the other pages. The other pages: a key row chosen word by
// word (Left/Right, OK) under the clock (Down), the markets (Left held; the chart: Down held twice)
// and the news (Down held); the markets' table: Right held its members, Up held now, Down held the
// order; the news' My feeds: OK held on the right a feed in, on the left out. The app calls
// picoamp.tvKey(phase, key) for the arrows and OK (a key held is its long one) and picoamp.tv(action)
// for the rest; a keyboard here (?tv=1): the arrows and Enter (held), Escape is Back.
const tvMode = params.get('tv') === '1';
const tvMenu = document.getElementById('tvmenu'), tvGrid = document.getElementById('tvgrid');
const ch = c => c.charCodeAt(0);
function tvSearch() {
    const t = (prompt('Search the station directory') || '').trim();
    if (!t) return;
    X.web_key(ch('/'));
    for (const c of t) if (c.charCodeAt(0) >= 32 && c.charCodeAt(0) < 127) X.web_key(c.charCodeAt(0));
    X.web_key(K.ENTER);
}
function tvPlayer() { for (let i = 0; i < 4 && X.web_back_level() === 0; i++) X.web_key(K.ESC); }
const goto = place => () => { tvPlayer(); withStr(place, p => X.web_goto(p)); tvZone = 'list'; tvIn = true; };
const TV_ITEMS = [
    ['Internet radio', goto('radio:')], ['Sonos', goto('sonos:')], ['Search stations', () => { tvPlayer(); tvSearch(); tvZone = 'list'; tvIn = true; }],
    ['Clock', () => X.web_key(ch('C'))], ['Weather', () => X.web_key(ch('W'))], ['News', () => X.web_key(ch('T'))],
    ['Markets', () => X.web_key(ch('$'))], ['Pictures', () => X.web_key(ch('I'))], ['Help: the keys', () => X.web_key(K.F1)],
];
const TV_COLS = 3;
let tvSel = 0;
for (const [name] of TV_ITEMS) { const d = document.createElement('div'); d.textContent = name; tvGrid.appendChild(d); }
function tvShow() { [...tvGrid.children].forEach((d, i) => d.classList.toggle('on', i === tvSel)); }
function tvOpen(on) { tvMenu.hidden = !on; if (on) tvShow(); }

// the player's parts (MSX pixels) and the control row's cells (from the mouse's: ui.c)
//   top    the remote's own: Menu, Help, Sound (full / light), Exit   (Left/Right choose, OK)
//   seek   the seek bar: Left/Right 10 s back / on (a track, not a station)
//   pic    the picture   list   the list   ctl   the control row
const PG = { PLAYER: 0, FULL: 1, CLOCK: 2, WX: 3, NEWS: 4, PICS: 5, MARKETS: 6 };
const TV_ZONES = { top: [0, 0, 256, 16], seek: [4, 31, 248, 10], pic: [0, 40, 256, 96], list: [0, 144, 256, 40], ctl: [0, 184, 256, 8] };
const TV_ORDER = ['top', 'seek', 'pic', 'list', 'ctl'];
const TV_CTL = [['prev', 1, 1], ['back10', 2, 2], ['play', 3, 3], ['pause', 4, 4], ['stop', 5, 5], ['on10', 6, 6], ['next', 7, 7],
                ['shuffle', 9, 11], ['repeat', 13, 15], ['eq', 17, 18], ['volume', 20, 31]];
const TV_TOP = ['Menu', 'Help', 'Sound', 'Exit'];
let tvZone = 'list', tvIn = false, tvCtl = 2, tvVol = false, tvAt = 0, tvTop = 0;
let tvBar = null;                 // a page's key rows, chosen one by one: { page, items: [[label, act, x, y, w, h]], i }
let tvHeads = false;              // the markets' chart: its headlines framed
function tvClick(col) { X.web_mouse(col * 8 + 3, 23 * 8 + 3, 1); X.web_mouse(col * 8 + 3, 23 * 8 + 3, 7); }
function cstr(p) { if (!p) return ''; const b = new Uint8Array(mem.buffer, p); let n = 0; while (b[n]) n++; return new TextDecoder().decode(b.subarray(0, n)); }
const describe = () => cstr(X.web_describe());
const field = (d, k) => { const m = d.match(new RegExp('\\b' + k + '=(-?\\d+)')); return m ? +m[1] : null; };
const note = t => withStr(t, p => X.web_message(p, 0));
const keys = s => { for (const c of s) if (c.charCodeAt(0) >= 32 && c.charCodeAt(0) < 127) X.web_key(c.charCodeAt(0)); };
// a key that types text: the TV's on-screen keyboard (prompt()), then the text and Enter; nothing when cancelled
const typed = (key, question, then) => () => {
    const t = prompt(question);
    if (t === null) return;
    X.web_key(typeof key === 'number' ? key : ch(key));
    keys(t.trim());
    X.web_key(K.ENTER);
    if (then) then();
};
const sendKey = k => () => X.web_key(typeof k === 'number' ? k : ch(k));

// ---- the pages' key rows (where each key's words are: the frame goes around them)
// (the 512-pixel pages: 6-pixel letters from x 16; the clock's: from x 0)
function at6(x0, row, text, label, act) {
    const i = text.indexOf(label);
    return [label, act, x0 + 6 * Math.max(0, i), row * 8, 6 * label.length, 8];
}
const MK25 = ' 1 Europe  2 Americas  3 Asia-Pacific   C clock  W weather  T news  $/ESC player';
const MK24 = ' </> list  Up/Down choose  Enter chart  I members (BS back)  O order  U now';
const NW25 = ' C clock   W weather   T the player   I info';
const NW24 = {
    feeds: ' Left/Right side  Enter read/add  D remove  U/J move  A address  BS back  ESC headlines',
    headlines: ' Enter read  Up/Down choose  Left/Right page  1-9 a feed  F My feeds  S sort',
    story: ' Up/Down scroll  Left/Right page  Enter full article/summary  N/P next/prev  ESC',
    info: ' ESC, Enter or BS: back',
};
const CK23 = '</> face I time L place O sleep A alarm';
function barItems(pg, d) {
    if (pg === PG.MARKETS) {
        const chart = /markets chart/.test(d);
        const lr = chart ? [['<', sendKey(K.LEFT), 16 + 6 * 18, 192, 18, 8], ['>', sendKey(K.RIGHT), 16 + 6 * 46, 192, 18, 8]]
                         : [at6(16, 24, MK24, '<', sendKey(K.LEFT)), at6(16, 24, MK24, '>', sendKey(K.RIGHT))];
        return [...lr, at6(16, 25, MK25, '1 Europe', sendKey('1')), at6(16, 25, MK25, '2 Americas', sendKey('2')),
                at6(16, 25, MK25, '3 Asia-Pacific', sendKey('3')), at6(16, 25, MK25, 'C clock', sendKey('C')),
                at6(16, 25, MK25, 'W weather', sendKey('W')), at6(16, 25, MK25, 'T news', sendKey('T')),
                at6(16, 25, MK25, '$/ESC player', sendKey('$'))];
    }
    if (pg === PG.NEWS) {
        const view = (d.match(/news (\w+)/) || [])[1] || 'headlines', r = NW24[view] || NW24.headlines;
        const side = field(d, 'side');
        let it;
        if (view === 'feeds') it = [
            at6(16, 24, r, 'Left', sendKey(K.LEFT)), at6(16, 24, r, 'Right', sendKey(K.RIGHT)),
            at6(16, 24, r, 'read', () => { if (field(describe(), 'side') === 1) X.web_key(K.LEFT); X.web_key(K.ENTER); }),
            at6(16, 24, r, 'add', () => { if (field(describe(), 'side') === 0) X.web_key(K.RIGHT); X.web_key(K.ENTER); }),
            at6(16, 24, r, 'D remove', sendKey('D')), at6(16, 24, r, 'U', sendKey('U')), at6(16, 24, r, 'J', sendKey('J')),
            at6(16, 24, r, 'A address', typed('A', 'A feed\'s address (RSS or Atom)')),
            at6(16, 24, r, 'BS back', sendKey(K.BS)), at6(16, 24, r, 'ESC headlines', sendKey(K.ESC))];
        else if (view === 'headlines') it = [
            at6(16, 24, r, 'Left', sendKey(K.LEFT)), at6(16, 24, r, 'Right', sendKey(K.RIGHT)),
            at6(16, 24, r, 'Enter read', sendKey(K.ENTER)), at6(16, 24, r, 'F My feeds', sendKey('F')),
            at6(16, 24, r, 'S sort', sendKey('S'))];
        else if (view === 'story') it = [
            at6(16, 24, r, 'Left', sendKey(K.LEFT)), at6(16, 24, r, 'Right', sendKey(K.RIGHT)),
            at6(16, 24, r, 'Enter full', sendKey(K.ENTER)), at6(16, 24, r, 'N', sendKey('N')), at6(16, 24, r, 'P', sendKey('P')),
            at6(16, 24, r, 'ESC', sendKey(K.ESC))];
        else it = [at6(16, 24, r, 'ESC', sendKey(K.ESC))];
        void side;
        return [...it, at6(16, 25, NW25, 'C clock', sendKey('C')), at6(16, 25, NW25, 'W weather', sendKey('W')),
                at6(16, 25, NW25, 'T the player', sendKey('T')), at6(16, 25, NW25, 'I info', sendKey('I'))];
    }
    if (pg === PG.CLOCK) return [
        at6(0, 23, CK23, '<', sendKey(K.LEFT)), at6(0, 23, CK23, '>', sendKey(K.RIGHT)), at6(0, 23, CK23, 'I time', sendKey('I')),
        at6(0, 23, CK23, 'L place', typed('L', 'The place (empty: found by the internet address)', () => { tvBar = null; })),
        at6(0, 23, CK23, 'O sleep', sendKey('O')),
        at6(0, 23, CK23, 'A alarm', typed('A', 'The alarm at (e.g. 7:30; w 7:30: Mo-Fr; empty: off)'))];
    return null;
}
function openBar(pg, d, at) {
    const items = barItems(pg, d);
    if (items) tvBar = { page: pg, items, i: Math.min(at || 0, items.length - 1) };
}

// ---- the frame: drawn over the picture (draw()), for a few seconds after a key, and while a part is chosen
function tvFrame(g, d) {
    if (!tvMode || !X || !tvMenu.hidden) return;
    const st = X.web_tv_state(), pg = st & 15;
    const W = X.web_width(), H = X.web_height(), kx = d.w / W, ky = d.h / H, lw = Math.max(2, Math.round(kx * 0.75));
    const box = (r, col) => {
        g.save();
        g.strokeStyle = col;
        g.lineWidth = lw;
        g.strokeRect(d.x + r[0] * kx - lw / 2, d.y + r[1] * ky - lw / 2, r[2] * kx + lw, r[3] * ky + lw);
        g.restore();
    };
    if (tvBar && tvBar.page === pg) { const it = tvBar.items[tvBar.i]; box(it.slice(2), '#ffffff'); return; }
    if (tvHeads && pg === PG.MARKETS) { box([16, 160, 480, 32], '#ffffff'); return; }
    if (pg !== PG.PLAYER || (st & 0x600)) return;
    const recent = performance.now() - tvAt < 6000;
    if (tvZone === 'top') {
        // the remote's own row, over the player's top
        const h = 14 * ky, y = d.y + 1 * ky, w = d.w / TV_TOP.length;
        g.save();
        g.fillStyle = 'rgba(0,0,40,0.92)';
        g.fillRect(d.x, d.y, d.w, 16 * ky);
        g.font = `${Math.round(8 * ky)}px sans-serif`;
        g.textAlign = 'center';
        g.textBaseline = 'middle';
        const names = TV_TOP.map(n => n === 'Sound' ? (lightSound ? 'Sound: light' : 'Sound: full') : n);
        names.forEach((n, i) => {
            g.fillStyle = i === tvTop ? '#ffff24' : '#ffffff';
            g.fillText(n, d.x + w * (i + 0.5), y + h / 2);
        });
        g.strokeStyle = '#ffffff';
        g.lineWidth = lw;
        g.strokeRect(d.x + w * tvTop + 4 * kx, y, w - 8 * kx, h);
        g.restore();
        return;
    }
    if (tvIn && tvZone === 'ctl') {
        const [, c0, c1] = TV_CTL[tvCtl];
        box([c0 * 8, 184, (c1 - c0 + 1) * 8, 8], tvVol || (st & 0x100) ? '#ffff24' : '#ffffff');
    } else if (!tvIn && recent) box(TV_ZONES[tvZone], '#ffffff');
    else if (tvIn && (tvZone === 'pic' || tvZone === 'seek') && recent) box(TV_ZONES[tvZone], '#6dffff');
}

// ---- a page's keys (not the player): the key rows, the long presses
function tvPage(action, st) {
    const pg = st & 15;
    const arrows = { up: K.UP, down: K.DOWN, left: K.LEFT, right: K.RIGHT };
    if (tvBar && tvBar.page !== pg) tvBar = null;
    if (pg !== PG.MARKETS) tvHeads = false;
    if (tvBar) {
        const n = tvBar.items.length;
        if (action === 'left') tvBar.i = (tvBar.i + n - 1) % n;
        else if (action === 'right') tvBar.i = (tvBar.i + 1) % n;
        else if (action === 'ok') {
            const keep = tvBar, i = tvBar.i;
            tvBar.items[i][1]();
            // (the key row stays, its words may have changed: the same place again)
            const s2 = X.web_tv_state();
            if (tvBar === keep && (s2 & 15) === pg && !(s2 & 0x600)) openBar(pg, describe(), i);
            else if ((s2 & 15) !== pg) tvBar = null;
        }
        else if (action === 'up' || action === 'back') tvBar = null;
        return 'ok';
    }
    if (tvHeads) {
        if (action === 'ok') { tvHeads = false; X.web_key(ch('T')); }
        else if (action === 'down') { tvHeads = false; openBar(pg, describe()); }
        else if (action === 'up' || action === 'back') tvHeads = false;
        return 'ok';
    }
    const d = (st & 0x600) ? '' : describe();
    if (pg === PG.MARKETS && d) {
        const chart = /markets chart/.test(d);
        if (!chart && action === 'longright') { X.web_key(ch('I')); return 'ok'; }
        if (!chart && action === 'longup') { X.web_key(ch('U')); return 'ok'; }
        if (!chart && action === 'longdown') { X.web_key(ch('O')); return 'ok'; }
        if (!chart && action === 'longleft') { openBar(pg, d); return 'ok'; }
        if (chart && action === 'longdown') { tvHeads = true; return 'ok'; }
    }
    if (pg === PG.NEWS && d && field(d, 'typing') !== 1) {
        if (action === 'longdown') { openBar(pg, d); return 'ok'; }
        if (action === 'longok' && /news feeds/.test(d)) {
            if (field(d, 'side') === 0) X.web_key(ch('D'));                       // My feeds: out
            else if (field(d, 'topic') >= 0) X.web_key(K.ENTER);                 // a topic's feed: in
            else note(' Choose a topic first (OK), then hold OK on a feed');
            return 'ok';
        }
    }
    if (pg === PG.CLOCK && d && action === 'down' && field(d, 'input') === 0 && field(d, 'choosing') === 0) { openBar(pg, d); return 'ok'; }
    if (action === 'longok') { tvOpen(true); return 'ok'; }
    if (action.startsWith('long')) action = action.slice(4);
    if (action === 'back') X.web_key(K.ESC);
    else if (action === 'ok') X.web_key(K.ENTER);
    else if (arrows[action]) X.web_key(arrows[action]);
    return 'ok';
}

// does a key held mean something else here? (then it acts when let go, or held: its long one)
function tvWantsLong(name) {
    if (name === 'ok') return true;
    if (netPick) return false;
    if (!X || !tvMenu.hidden || tvBar || tvHeads) return false;
    const st = X.web_tv_state(), pg = st & 15;
    if (st & 0x600) return false;
    if (pg === PG.MARKETS) return /markets table/.test(describe()) ? true : name === 'down';
    if (pg === PG.NEWS) return name === 'down' && field(describe(), 'typing') !== 1;
    return false;
}
let tvHeld = null;
// the remote's keys as they go: phase 'down' (pressed), 'repeat' (held, again), 'up' (let go)
function tvKey(phase, name) {
    if (!X) return 'ok';
    if (phase === 'down') {
        if (tvHeld && tvHeld.timer) clearTimeout(tvHeld.timer);
        if (tvWantsLong(name)) {
            const h = tvHeld = { name, later: true };
            h.timer = setTimeout(() => { h.fired = true; const r = tv('long' + name); if (r === 'exit') window.PicoAmpAndroid?.exit?.(); }, 600);
            return 'ok';
        }
        tvHeld = { name };
        return tv(name);
    }
    if (phase === 'repeat') return tvHeld && tvHeld.name === name && !tvHeld.later ? tv(name) : 'ok';
    const h = tvHeld;
    if (!h || h.name !== name) return 'ok';
    tvHeld = null;
    if (h.later && !h.fired) { clearTimeout(h.timer); return tv(name); }
    return 'ok';
}

function tv(action) {
    if (!X) return 'ok';
    ensureCtx();
    hideHint();
    tvAt = performance.now();
    if (netPick) { netPickAct(action.replace(/^long(?!ok)/, '')); return 'ok'; }
    const n = TV_ITEMS.length;
    if (!tvMenu.hidden) {
        if (action === 'left') tvSel = (tvSel + n - 1) % n;
        else if (action === 'right') tvSel = (tvSel + 1) % n;
        else if (action === 'up') tvSel = (tvSel + n - TV_COLS) % n;
        else if (action === 'down') tvSel = (tvSel + TV_COLS) % n;
        else if (action === 'ok') { tvOpen(false); TV_ITEMS[tvSel][1](); return 'ok'; }
        else if (action === 'back' || action === 'menu' || action === 'longok') tvOpen(false);
        tvShow();
        return 'ok';
    }
    if (action === 'menu') { tvOpen(true); return 'ok'; }
    const media = { playpause: K.PLAY, play: K.PLAY, pause: K.PLAY, stop: K.STOP, next: K.NEXT, prev: K.PREV, ff: ch('.'), rew: ch(','), info: K.F1 };
    if (media[action] !== undefined) { X.web_key(media[action]); return 'ok'; }
    const arrows = { up: K.UP, down: K.DOWN, left: K.LEFT, right: K.RIGHT };
    const st = X.web_tv_state();
    // another page, the help, the search line
    if ((st & 15) !== 0 || (st & 0x600)) return tvPage(action, st);
    tvBar = null;
    tvHeads = false;
    // the equalizer (from the control row's EQ): its arrows; Back or OK closes it
    if (st & 0x100) {
        if (action === 'back' || action === 'ok' || action === 'longok') X.web_key(ch('G'));
        else if (arrows[action.replace(/^long/, '')]) X.web_key(arrows[action.replace(/^long/, '')]);
        return 'ok';
    }
    // OK held in the list: a station in (or, in the favourites, out of) the favourites
    if (action === 'longok') {
        if (tvZone === 'list' && (st & 0x2000)) X.web_key(ch((st & 0x800) ? 'D' : 'F'));
        else tvOpen(true);
        return 'ok';
    }
    if (action.startsWith('long')) action = action.slice(4);
    // the remote's own row
    if (tvZone === 'top') {
        if (action === 'left') tvTop = (tvTop + TV_TOP.length - 1) % TV_TOP.length;
        else if (action === 'right') tvTop = (tvTop + 1) % TV_TOP.length;
        else if (action === 'down' || action === 'back') { tvZone = 'seek'; tvIn = false; }
        else if (action === 'ok') {
            const what = TV_TOP[tvTop];
            if (what === 'Menu') { tvZone = 'list'; tvOpen(true); }
            else if (what === 'Help') { tvZone = 'list'; X.web_key(K.F1); }
            else if (what === 'Sound') setLightSound(!lightSound);
            else if (what === 'Exit') return 'exit';
        }
        return 'ok';
    }
    // the seek bar: Left/Right wind at once (OK not needed)
    if (tvZone === 'seek' && (action === 'left' || action === 'right')) {
        if (st & 0x1000) X.web_key(ch(action === 'left' ? ',' : '.'));
        else note('A station cannot be wound: no seeking here');
        return 'ok';
    }
    if (!tvIn) {
        const i = TV_ORDER.indexOf(tvZone);
        if (action === 'up') tvZone = TV_ORDER[Math.max(0, i - 1)];
        else if (action === 'down') tvZone = TV_ORDER[Math.min(TV_ORDER.length - 1, i + 1)];
        else if (action === 'ok') tvIn = tvZone !== 'seek';
        else if (action === 'back') {
            const lv = X.web_back_level();
            if (lv === 2) return 'exit';
            X.web_key(lv === 1 ? K.BS : K.ESC);
        }
        return 'ok';
    }
    if (action === 'back' && !tvVol && !(tvZone === 'list' && X.web_back_level() === 1)) { tvIn = false; return 'ok'; }
    if (tvZone === 'pic') {
        const view = st >> 4 & 3;
        if (action === 'right') X.web_key(ch('V'));
        else if (action === 'left') { X.web_key(ch('V')); X.web_key(ch('V')); }
        else if (action === 'ok') { if (view === 2) X.web_full_cover(); else X.web_key(ch('Z')); }
        else if (action === 'up' || action === 'down') { tvIn = false; return tv(action); }
        return 'ok';
    }
    if (tvZone === 'list') {
        if (action === 'back') X.web_key(K.BS);             // (the folder up: at the top Back comes out, above)
        else if (action === 'ok') X.web_key(K.ENTER);
        else X.web_key(arrows[action]);
        return 'ok';
    }
    // the control row
    if (tvVol) {
        // (a TV whose volume is its own, by HDMI-CEC: up or down there)
        if (dev && devFixed && (action === 'left' || action === 'right') && !(st & 0x10000)) dev.volumeStep(action === 'left' ? -1 : 1);
        else if (action === 'left') X.web_key(ch('-'));
        else if (action === 'right') X.web_key(ch('+'));
        else if (action === 'ok' || action === 'back' || action === 'up' || action === 'down') tvVol = false;
        return 'ok';
    }
    if (action === 'left') tvCtl = (tvCtl + TV_CTL.length - 1) % TV_CTL.length;
    else if (action === 'right') tvCtl = (tvCtl + 1) % TV_CTL.length;
    else if (action === 'up') { tvIn = false; tvZone = 'list'; }
    else if (action === 'ok') {
        const [what, c0] = TV_CTL[tvCtl];
        if (what === 'volume') tvVol = true;
        else tvClick(c0);
    }
    return 'ok';
}
if (tvMode) {
    // a keyboard (the tests, a TV with one): the arrows and Enter as the remote's (held: their long ones),
    // Escape as Back
    const remote = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right', Enter: 'ok' };
    window.addEventListener('keydown', e => {
        const r = remote[e.key];
        const a = { Escape: 'back', ContextMenu: 'menu' }[e.key];
        if (r) {
            e.preventDefault();
            e.stopImmediatePropagation();
            if (tvKey(e.repeat ? 'repeat' : 'down', r) === 'exit') window.PicoAmpAndroid?.exit?.();
            return;
        }
        if (!tvMenu.hidden || a) {
            if (!a && tvMenu.hidden) return;
            e.preventDefault();
            e.stopImmediatePropagation();
            if (a && tv(a) === 'exit') window.PicoAmpAndroid?.exit?.();
        }
    }, true);
    window.addEventListener('keyup', e => {
        const r = remote[e.key];
        if (!r) return;
        e.preventDefault();
        e.stopImmediatePropagation();
        if (tvKey('up', r) === 'exit') window.PicoAmpAndroid?.exit?.();
    }, true);
}

// ---------------- the hint (sound needs a key or a click first) ----------------
const hint = document.getElementById('hint');
if (tvMode && hint) hint.textContent = 'Up/Down: the parts \u00b7 OK: in \u00b7 Back: out \u00b7 Up at the top: Menu, Help, Exit';
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
    tvKey,                                // the remote's keys as they go (the Android app): tvKey('down'|'repeat'|'up', 'up'|'down'|'left'|'right'|'ok')
    systemVolume,                         // the app: the device's volume changed (0-100)
    tv,                                   // the TV's remote (the Android app): 'up' 'down' 'left' 'right' 'ok' 'longok' 'back' 'menu' 'playpause' ...; 'exit': leave
    get tvMenuOpen() { return !tvMenu.hidden; },
    get tvFocus() { return { zone: tvZone, inside: tvIn, control: TV_CTL[tvCtl][0], volume: tvVol, top: TV_TOP[tvTop], bar: tvBar && tvBar.items[tvBar.i][0], heads: tvHeads }; },
    get lightSound() { return lightSound; },
    save: () => X.web_save(),
    stopSaving() { saving = false; },     // (the tests: the settings are changed under it)
    hideHint,
    // (the tests: the colour on the screen at a pixel of the picture, 256 x 192)
    sharp() { return X.web_cfg_sharp(-1); },   // (the tests)
    pixel(x, y) {
        const cx = Math.floor(dest.x + (x + 0.5) * dest.w / 256), cy = Math.floor(dest.y + (y + 0.5) * dest.h / 192);
        return Array.from(g2.getImageData(cx, cy, 1, 1).data.slice(0, 3));
    },
    art: { put: (kind, name, none, t) => caches.open('picoamp-logos').then(c => artPut(c, location.origin + ART[kind].pre + encodeURIComponent(name), none ? null : new Uint8Array([1, 2, 3]), t)),
           count: kind => caches.open('picoamp-logos').then(c => c.keys()).then(k => k.filter(r => r.url.startsWith(location.origin + ART[kind].pre)).length),
           has: (kind, name) => caches.open('picoamp-logos').then(c => c.match(location.origin + ART[kind].pre + encodeURIComponent(name))).then(r => !!r),
           prune: kind => caches.open('picoamp-logos').then(c => artPrune(c, kind)) },   // (the tests)
    npIcy, npProbe,                       // (the tests: a stream's own title; every way for a station)
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
    await restorePics();
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
            js_pics_pick, js_pics_list, js_pics_name, js_pic_file, js_pic_bytes,
            js_audio_play, js_audio_pause, js_audio_stop, js_audio_seek, js_audio_gain, js_audio_eq, js_audio_meta,
            js_can_play, js_open_lists, js_fullscreen: toggleFullscreen, js_decode_image, js_station_logo, js_song_cover,
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
    // the covers sharp: hires=1/0 in the address; the TV and the app: on the first time (F5 after)
    const hires = params.get('hires');
    if (hires === '1' || hires === '0') X.web_cfg_sharp(+hires);
    else if (tvMode || dev) try { if (!localStorage.getItem('picoamp-tv-sharp')) { X.web_cfg_sharp(1); localStorage.setItem('picoamp-tv-sharp', '1'); } } catch { }
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
