// node tools/cdp_send.mjs <url部分文字列> <method> '<json params>'
const [match, method, params] = process.argv.slice(2);
const list = await (await fetch("http://127.0.0.1:9222/json/list")).json();
const t = list.find(x => x.type === "page" && x.url.includes(match));
const ws = new WebSocket(t.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
ws.onmessage = (ev) => { const m = JSON.parse(ev.data); if (m.id === 1) { console.log(JSON.stringify(m)); ws.close(); process.exit(0); } };
ws.send(JSON.stringify({ id: 1, method, params: JSON.parse(params ?? "{}") }));
