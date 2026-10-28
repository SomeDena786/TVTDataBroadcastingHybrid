// WebView2のremote debugging (--remote-debugging-port=9222) に接続して調べる
// node tools/cdp.mjs list
// node tools/cdp.mjs eval <url部分文字列> "<式>"
// node tools/cdp.mjs console <url部分文字列> <秒>
// node tools/cdp.mjs screenshot <url部分文字列> <out.png>
import { writeFileSync } from "node:fs";

const port = process.env.CDP_PORT ?? "9222";
const [cmd, match, arg] = process.argv.slice(2);

async function targets() {
    const res = await fetch(`http://127.0.0.1:${port}/json/list`);
    return await res.json();
}

async function connect(m) {
    const list = await targets();
    const t = list.find(x => x.type === "page" && x.url.includes(m));
    if (t == null) {
        throw new Error("target not found: " + m + "\n" + list.map(x => x.url).join("\n"));
    }
    const ws = new WebSocket(t.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    let id = 0;
    const pending = new Map();
    const handlers = [];
    ws.onmessage = (ev) => {
        const msg = JSON.parse(ev.data);
        if (msg.id != null && pending.has(msg.id)) {
            pending.get(msg.id)(msg);
            pending.delete(msg.id);
        } else {
            for (const h of handlers) h(msg);
        }
    };
    return {
        send(method, params = {}) {
            const i = ++id;
            ws.send(JSON.stringify({ id: i, method, params }));
            return new Promise(resolve => pending.set(i, resolve));
        },
        on(h) { handlers.push(h); },
        close() { ws.close(); },
    };
}

if (cmd === "list") {
    for (const t of await targets()) {
        console.log(t.type, t.url, t.title);
    }
} else if (cmd === "eval") {
    const c = await connect(match);
    const r = await c.send("Runtime.evaluate", { expression: arg, returnByValue: true, awaitPromise: true });
    console.log(JSON.stringify(r.result?.result?.value ?? r.result, null, 1));
    c.close();
} else if (cmd === "console") {
    const c = await connect(match);
    c.on((msg) => {
        if (msg.method === "Runtime.consoleAPICalled") {
            const args = msg.params.args.map(a => a.value ?? a.description ?? a.type).join(" ");
            console.log(`[${msg.params.type}]`, args.substring(0, 2000));
        } else if (msg.method === "Runtime.exceptionThrown") {
            console.log("[exception]", msg.params.exceptionDetails?.exception?.description ?? msg.params.exceptionDetails?.text);
        } else if (msg.method === "Log.entryAdded") {
            console.log(`[log:${msg.params.entry.level}]`, msg.params.entry.text, msg.params.entry.url ?? "");
        }
    });
    await c.send("Runtime.enable");
    await c.send("Log.enable");
    await new Promise(r => setTimeout(r, Number(arg ?? 10) * 1000));
    c.close();
    process.exit(0);
} else if (cmd === "screenshot") {
    const c = await connect(match);
    const r = await c.send("Page.captureScreenshot", { format: "png" });
    writeFileSync(arg, Buffer.from(r.result.data, "base64"));
    console.log("saved", arg);
    c.close();
}
