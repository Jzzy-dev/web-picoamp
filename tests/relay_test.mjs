// the relay (relay/worker.js) with a pretend web: node tests/relay_test.mjs
import worker from '../relay/worker.js';

let fails = 0;
const check = (what, ok) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) fails++; };

const sites = {
    'https://feeds.example.com/rss.xml': () => new Response('<rss><channel><title>T</title></channel></rss>', { headers: { 'Content-Type': 'application/rss+xml' } }),
    'https://query1.finance.yahoo.com/v8/finance/spark?symbols=X': () => new Response('busy', { status: 429, headers: { 'Content-Type': 'text/plain' } }),
    'https://query2.finance.yahoo.com/v8/finance/spark?symbols=X': () => new Response('{"X":{}}', { headers: { 'Content-Type': 'application/json' } }),
    'https://radio.example.com/stream': () => new Response(new Uint8Array(100), { headers: { 'Content-Type': 'audio/mpeg' } }),
    'https://big.example.com/page': () => new Response('x'.repeat(5 << 20), { headers: { 'Content-Type': 'text/html', 'Content-Length': String(5 << 20) } }),
    'https://nolen.example.com/page': () => new Response(new ReadableStream({ start(c) { for (let i = 0; i < 6; i++) c.enqueue(new Uint8Array(1 << 20)); c.close(); } }), { headers: { 'Content-Type': 'text/html' } }),
};
let asked = [];
globalThis.fetch = async (u, o) => {
    u = String(u);
    asked.push([u, o && o.headers && o.headers['User-Agent']]);
    const s = sites[u];
    if (!s) throw new Error('no such site');
    const r = s();
    Object.defineProperty(r, 'url', { value: u });
    return r;
};

const PAGE = 'https://jzzy-dev.github.io';
const ask = (url, origin = PAGE, method = 'GET') =>
    worker.fetch(new Request('https://relay.test/?url=' + encodeURIComponent(url), { method, headers: origin ? { Origin: origin } : {} }), {});

let r = await ask('https://feeds.example.com/rss.xml');
check('a feed: passed on', r.status === 200 && (await r.text()).startsWith('<rss') && r.headers.get('Access-Control-Allow-Origin') === PAGE);
check('a feed: with a browser\'s User-Agent', /Mozilla/.test(asked.at(-1)[1]));
r = await ask('https://feeds.example.com/rss.xml', 'https://evil.example');
check('another page: refused', r.status === 403 && !r.headers.get('Access-Control-Allow-Origin'));
r = await ask('https://feeds.example.com/rss.xml', null);
check('no Origin (not a browser page): refused', r.status === 403);
r = await ask('https://feeds.example.com/rss.xml', 'http://localhost:8000');
check('localhost (make serve): answered', r.status === 200);
r = await worker.fetch(new Request('https://relay.test/?url=x', { headers: { Origin: 'https://mine.example' } }), { ORIGINS: 'https://other.example https://mine.example' });
check('ORIGINS (the Worker\'s variable): a page more', r.status === 400);
r = await ask('https://feeds.example.com/rss.xml', PAGE, 'OPTIONS');
check('the browser\'s question first (OPTIONS)', r.status === 204 && r.headers.get('Access-Control-Allow-Origin') === PAGE);
r = await ask('https://query1.finance.yahoo.com/v8/finance/spark?symbols=X');
check('Yahoo busy (429): the other host', r.status === 200 && (await r.text()) === '{"X":{}}');
r = await ask('https://radio.example.com/stream');
check('audio: not relayed', r.status === 415);
r = await ask('https://big.example.com/page');
check('more than 4 MB: refused', r.status === 413);
r = await ask('https://nolen.example.com/page');
const got = (await r.arrayBuffer()).byteLength;
check('no length given: cut at 4 MB (' + got + ')', got <= 4 << 20);
for (const u of ['http://127.0.0.1/', 'http://localhost:8080/', 'http://192.168.1.1/', 'http://10.0.0.1/', 'http://[::1]/', 'http://169.254.169.254/latest', 'file:///etc/passwd', 'ftp://x.example/'])
    check('refused: ' + u, [400, 403].includes((await ask(u)).status));
r = await ask('not an address');
check('not an address: 400', r.status === 400);
r = await ask('https://nowhere.example/');
check('a site that does not answer: 502', r.status === 502);

console.log(fails ? fails + ' failed' : 'all relay checks passed');
process.exit(fails ? 1 : 0);
