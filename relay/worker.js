// PicoAmp's relay: a Cloudflare Worker that fetches a page for PicoAmp on the web and hands
// it on with CORS allowed. The market monitor (Yahoo Finance), the news (the feeds and the
// stories' pages) and the song titles (the stations' status pages) come through it: those
// sites do not let other pages read them.
//
//   GET https://<your worker>/?url=<the address, encoded>
//
// It answers only PicoAmp's own page (ORIGINS below; more in the Worker's ORIGINS variable,
// spaces between them), only http(s) to public hosts, only text (feeds, pages, JSON: no
// audio or video), up to 4 MB. On Cloudflare's free plan it cannot cost anything: past
// 100,000 requests a day it stops answering until the next day.

const ORIGINS = [
    /^https:\/\/jzzy-dev\.github\.io$/,
    /^http:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/,
];
const TYPES = /^(text\/|application\/((rss|atom|rdf|xhtml|ld)\+)?(xml|json)|application\/(javascript|x-javascript|x-rss\+xml)|application\/[\w.+-]*\+(xml|json))/i;
const MAX = 4 << 20;
const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0 Safari/537.36 PicoAmp-relay';

function allowed(origin, env) {
    if (!origin) return false;
    if (ORIGINS.some(r => r.test(origin))) return true;
    return String((env && env.ORIGINS) || '').split(/\s+/).filter(Boolean).includes(origin);
}

// not the Worker's own network, nor a private one
function privateHost(h) {
    h = h.toLowerCase().replace(/^\[|\]$/g, '');
    if (h === 'localhost' || h.endsWith('.localhost') || h.endsWith('.local') || h.endsWith('.internal')) return true;
    const m = h.match(/^(\d+)\.(\d+)\.(\d+)\.(\d+)$/);
    if (m) {
        const [a, b] = [+m[1], +m[2]];
        return a === 0 || a === 10 || a === 127 || (a === 169 && b === 254) || (a === 172 && b >= 16 && b <= 31) ||
               (a === 192 && b === 168) || (a === 100 && b >= 64 && b <= 127) || a >= 224;
    }
    if (h.includes(':')) return h === '::1' || h === '::' || /^f[cd]/.test(h) || /^fe[89ab]/.test(h) || h.startsWith('::ffff:');
    return false;
}

function reply(status, text, cors) {
    return new Response(text, { status, headers: { ...cors, 'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store' } });
}

async function get(target) {
    return fetch(target, {
        headers: {
            'User-Agent': UA,
            'Accept': 'text/html,application/xhtml+xml,application/xml,application/rss+xml,application/atom+xml,application/json;q=0.9,*/*;q=0.8',
            'Accept-Language': 'en,*;q=0.5',
        },
        redirect: 'follow',
    });
}

export default {
    async fetch(req, env) {
        const origin = req.headers.get('Origin') || '';
        const ok = allowed(origin, env);
        const cors = ok ? { 'Access-Control-Allow-Origin': origin, 'Vary': 'Origin' } : { 'Vary': 'Origin' };
        if (req.method === 'OPTIONS')
            return new Response(null, { status: ok ? 204 : 403, headers: { ...cors, 'Access-Control-Allow-Methods': 'GET', 'Access-Control-Max-Age': '86400' } });
        if (!ok) return reply(403, 'This relay answers PicoAmp\'s page only.', cors);
        if (req.method !== 'GET') return reply(405, 'GET only', cors);

        let target;
        try { target = new URL(new URL(req.url).searchParams.get('url') || ''); } catch { return reply(400, 'url= is not an address', cors); }
        if (!/^https?:$/.test(target.protocol)) return reply(400, 'http and https only', cors);
        if (target.username || target.password) return reply(400, 'no user names in the address', cors);
        if (privateHost(target.hostname)) return reply(403, 'not a public host', cors);

        let r;
        try {
            r = await get(target);
            // (Yahoo Finance's two hosts answer the same; the other one when this one is busy)
            if (r.status === 429 && /^query[12]\.finance\.yahoo\.com$/.test(target.hostname)) {
                const t2 = new URL(target);
                t2.hostname = target.hostname.startsWith('query1') ? 'query2.finance.yahoo.com' : 'query1.finance.yahoo.com';
                r = await get(t2);
            }
        } catch (e) {
            return reply(502, 'The site did not answer: ' + (e && e.message || e), cors);
        }

        const type = r.headers.get('Content-Type') || 'text/plain';
        if (!TYPES.test(type)) { r.body && r.body.cancel(); return reply(415, 'not text: ' + type.split(';')[0], cors); }
        const len = +r.headers.get('Content-Length') || 0;
        if (len > MAX) { r.body && r.body.cancel(); return reply(413, 'more than 4 MB', cors); }

        // (up to MAX bytes, also when the site gave no length)
        let body = r.body;
        if (body && !len) {
            let n = 0;
            body = body.pipeThrough(new TransformStream({
                transform(chunk, c) { n += chunk.byteLength; if (n > MAX) c.terminate(); else c.enqueue(chunk); },
            }));
        }
        return new Response(body, {
            status: r.status,
            headers: { ...cors, 'Content-Type': type, 'Cache-Control': 'no-store', 'X-Final-URL': r.url, 'Access-Control-Expose-Headers': 'X-Final-URL' },
        });
    },
};
