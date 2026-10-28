// TSをweb-bmlのdecodeTSに通してHybridcast関連の情報を表示する
// node tools/scan_ts.mjs <file.ts> [MB]
import { createReadStream } from "node:fs";
import { decodeTS } from "../web-bml/dist/server/decode_ts.js";

const file = process.argv[2];
const limitMB = Number(process.argv[3] ?? 300);
const keywords = /startAITControlledApp|startReceivingAIT|getReceivedAIT|xmlait|AITControlledApp|getResidentAppVersion/;
const seenModules = new Set();
const found = [];
let eit = null;
const aits = [];
let pmt = null;

const tsStream = decodeTS({
    sendCallback(msg) {
        if (msg.type === "pmt") {
            pmt = msg.components;
        } else if (msg.type === "eitPresentFollowing") {
            eit = msg;
        } else if (msg.type === "ait") {
            aits.push(msg);
        } else if (msg.type === "moduleDownloaded") {
            const key = `${msg.componentId}/${msg.moduleId}/${msg.version}`;
            if (seenModules.has(key)) {
                return;
            }
            seenModules.add(key);
            for (const f of msg.files) {
                const data = Buffer.from(f.dataBase64, "base64");
                // BMLはEUC-JP, スクリプト部分はASCIIで検索できる
                const text = data.toString("latin1");
                if (keywords.test(text)) {
                    const idx = text.search(keywords);
                    found.push({ key, location: f.contentLocation, snippet: text.substring(Math.max(0, idx - 600), idx + 1200) });
                }
            }
        }
    },
});

let total = 0;
const stream = createReadStream(file, { highWaterMark: 188 * 1024 * 4 });
stream.on("data", (chunk) => {
    tsStream.push(new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
    total += chunk.length;
    if (total >= limitMB * 1024 * 1024) {
        stream.destroy();
        report();
    }
});
stream.on("end", report);
let reported = false;
function report() {
    if (reported) return;
    reported = true;
    console.log(JSON.stringify({
        file,
        scannedMB: Math.round(total / 1024 / 1024),
        pmt: pmt?.map(c => ({ tag: c.componentId.toString(16), pid: c.pid, st: c.streamType, dcid: c.dataComponentId })),
        eit,
        aits: aits.map(a => ({ pid: a.pid, componentId: a.componentId, section: a.section })),
        modules: seenModules.size,
        found: found.map(f => ({ key: f.key, location: f.location })),
    }, null, 1));
    for (const f of found) {
        console.log("=====", f.key, f.location);
        console.log(f.snippet.replace(/[^\x09\x0a\x0d\x20-\x7e]/g, "."));
    }
    process.exit(0);
}
