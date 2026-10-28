import type { ComponentPMT, ResponseMessage } from "web-bml/protocol";
import { BMLBrowser, BMLBrowserFontFace, EPG, Indicator, IP, InputApplication, InputCancelReason, InputCharacterType } from "web-bml";
import { decodeTS } from "web-bml/ts";
import { CaptionPlayer } from "./caption_player";
import { AITControlledAppHost, Html5AIT, Html5Application } from "./ait_controlled_app";

declare global {
    interface Window {
        chrome: {
            webview: {
                postMessage(msg: any): any;
                addEventListener(type: string, listener: EventListenerOrEventListenerObject): void;
            }
        };
        sendMessage?(data: ToWebViewMessage): FromWebViewMessage | null;
        bmlBrowser?: BMLBrowser;
    }
}
function postMessage(message: FromWebViewMessage): void {
    window.chrome.webview.postMessage(message);
}

export class StatusBarIndicator implements Indicator {
    private readonly receivingStatusElement: HTMLElement;
    private readonly networkingStatusElement: HTMLElement;
    constructor(receivingStatusElement: HTMLElement, networkingStatusElement: HTMLElement) {
        this.receivingStatusElement = receivingStatusElement;
        this.networkingStatusElement = networkingStatusElement;
    }
    url = "";
    receiving = false;
    eventName: string | null = "";
    loading = false;
    networkingPost = false;
    private update() {
        postMessage({
            type: "status",
            loading: this.loading,
            url: this.url,
            receiving: this.receiving,
        });
        if (this.receiving) {
            this.receivingStatusElement.style.display = "";
        } else {
            this.receivingStatusElement.style.display = "none";
        }
        if (this.networkingPost) {
            this.networkingStatusElement.style.display = "";
        } else {
            this.networkingStatusElement.style.display = "none";
        }
    }
    public setUrl(name: string, loading: boolean): void {
        this.url = name;
        this.loading = loading;
        this.update();
    }
    public setReceivingStatus(receiving: boolean): void {
        this.receiving = receiving;
        this.update();
    }
    public setNetworkingGetStatus(get: boolean): void {

    }
    public setNetworkingPostStatus(post: boolean): void {
        this.networkingPost = post;
        this.update();
    }
    public setEventName(eventName: string | null): void {
        this.eventName = eventName;
        this.update();
    }
}

const params = new URLSearchParams(location.search);
const networkId = Number(params.get("networkId")) ?? undefined;
const serviceId = Number(params.get("serviceId")) ?? undefined;

// BML文書と動画と字幕が入る要素
const browserElement = document.getElementById("data-broadcasting-browser")!;
// 動画が入っている要素
const videoContainer = browserElement.querySelector(".arib-video-container") as HTMLElement;
// BML文書が入る要素
const contentElement = browserElement.querySelector(".data-broadcasting-browser-content") as HTMLElement;
// BML用フォント
const roundGothic: BMLBrowserFontFace = { source: "url('../dist/KosugiMaru-Regular.woff2'), local('MS Gothic')" };
const boldRoundGothic: BMLBrowserFontFace = { source: "url('../dist/KosugiMaru-Bold.woff2'), local('MS Gothic')" };
const squareGothic: BMLBrowserFontFace = { source: "url('../dist/Kosugi-Regular.woff2'), local('MS Gothic')" };
const ccContainer = browserElement.querySelector(".arib-video-cc-container") as HTMLElement;
const player = new CaptionPlayer(document.createElement("video"), ccContainer);
// リモコン
const remoteControl = new StatusBarIndicator(browserElement.querySelector(".remote-control-receiving-status")!, browserElement.querySelector(".remote-control-networking-status")!);

const remoteControlStatusContainer = document.querySelector(".remote-control-status-container") as HTMLElement;
remoteControlStatusContainer.style.visibility = "hidden";

const ccStatus = document.getElementById("cc-status")!;

const ccStatusAnimation = ccStatus.animate([{ visibility: "hidden", offset: 1 }], {
    duration: 2000,
    fill: "forwards",
});

const epg: EPG = {
    tune(originalNetworkId, transportStreamId, serviceId) {
        console.error("tune", originalNetworkId, transportStreamId, serviceId);
        postMessage({
            type: "tune",
            originalNetworkId,
            transportStreamId,
            serviceId,
        });
        return true;
    }
};

let networkEnabled = false;
let isRecord = false;

const apiIP: IP = {
    getConnectionType() {
        return 403;
    },
    isIPConnected() {
        return (networkEnabled && !isRecord) ? 1 : 0;
    },
    async transmitTextDataOverIP(uri, body) {
        if (!(networkEnabled && !isRecord)) {
            return { resultCode: NaN, statusCode: "", response: new Uint8Array() };
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastinghybrid-api.invalid/api/post/" + uri, {
                method: "POST",
                headers: {
                    "Content-Type": "application/x-www-form-urlencoded"
                },
                body,
            });
            return { resultCode: 1, statusCode: res.status.toString(), response: new Uint8Array(await res.arrayBuffer()) };
        } catch {
            return { resultCode: NaN, statusCode: "", response: new Uint8Array() };
        }
    },
    async get(uri) {
        if (!(networkEnabled && !isRecord)) {
            return {};
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastinghybrid-api.invalid/api/get/" + uri, {
                method: "GET",
            });
            return { statusCode: res.status, headers: res.headers, response: new Uint8Array(await res.arrayBuffer()) };
        } catch {
            return {};
        }
    },
};

const audioContext = new AudioContext();
const gainNode = audioContext.createGain();
gainNode.connect(audioContext.destination);

player.setPRAAudioNode(gainNode);

let changeCallback: ((value: string) => void) | undefined;

const inputApplication: InputApplication = {
    launch(opts) {
        if (changeCallback != null) {
            this.cancel("other");
        }
        postMessage({
            type: "input",
            characterType: opts.characterType,
            maxLength: opts.maxLength,
            value: opts.value,
            allowedCharacters: opts.allowedCharacters,
            inputMode: opts.inputMode,
            multiline: opts.multiline,
        });
        changeCallback = opts.callback;
    },
    cancel(reason) {
        if (changeCallback != null) {
            changeCallback = undefined;
            postMessage({
                type: "cancelInput",
                reason,
            });
        }
    },
};

function X_DPA_startResidentApp(appName: string, showAV: number, returnURI: string, Ex_info: string[]): number {
    if (appName === "ComBrowser") {
        const uri = Ex_info[0];
        const mode = Ex_info[1];
        const fullscreen = Ex_info[2] === "1";
        if (!uri.startsWith("http://") && !uri.startsWith("https://")) {
            return NaN;
        }
        if (mode === "0") {
            // 0: 本規定に準じたコンテンツを表示可能なブラウザ
            // 非リンクコンテンツへと遷移 (TR-B14 第三分冊 図8-9)
            return NaN;
        }
        // 1: 通信事業者仕様ブラウザ
        // 2: HTMLブラウザ
        // fullscreenが1ならば原則としてデータ放送ブラウザの表示を終了する
        postMessage({
            type: "startBrowser",
            uri,
            fullscreen,
        });
        // データ放送ブラウザは，当該拡張関数を実行後，引き続きスクリプトの実行を継続することが望ましい
        // TR-B14 第三分冊 7.10.8
        return 1;
    } else if (appName === "BookmarkList") {
        return NaN;
    }
    return NaN;
}

// Hybridcast (放送通信連携HTML5) 設定 nativeからhtml5Configで設定される
let html5Enabled = false;
let html5AutoStartBroadcastAIT = true;
let residentAppVersion: { [appName: string]: any[] } = {};
// nativeが挿入中のB-CASカードから読み取ったカードID (16進12桁)
let cardIdHex: string | null = null;

let currentServiceIds: { originalNetworkId?: number, transportStreamId?: number, serviceId?: number } = {};

function aitToMessage(app: Html5Application, ait: Html5AIT) {
    return {
        profile: "hybridcast",
        ...currentServiceIds,
        url: app.url!,
        organizationId: app.organizationId,
        applicationId: app.applicationId,
        controlCode: app.controlCode,
        applicationType: app.applicationType,
        priority: app.priority,
        boundaries: app.boundaries,
        aitSource: ait.source,
        applications: ait.applications,
    };
}

const aitHost = new AITControlledAppHost({
    async fetchXMLAIT(uri) {
        if (!(networkEnabled && !isRecord && html5Enabled)) {
            return null;
        }
        try {
            const res = await window.fetch("https://tvtdatabroadcastinghybrid-api.invalid/api/get/" + uri, { method: "GET" });
            if (!res.ok) {
                return null;
            }
            return await res.text();
        } catch {
            return null;
        }
    },
    launch(app, ait) {
        if (!html5Enabled) {
            return false;
        }
        console.log("launchHtml5", app, ait);
        postMessage({
            type: "launchHtml5",
            ...aitToMessage(app, ait),
        });
        return true;
    },
    supportedApplicationTypes: new Set([0x0010]),
    log: (...args) => console.log(...args),
});

// 放送で伝送されるAITのAUTOSTART/KILL処理 (STD-B24 第四編)
let broadcastAutoStarted: string | null = null;
function processBroadcastAIT() {
    const ait = aitHost.getBroadcastAIT();
    if (ait == null || !html5Enabled) {
        return;
    }
    const running = broadcastAutoStarted;
    if (running != null) {
        const app = ait.applications.find(x => `${x.organizationId}.${x.applicationId}` === running);
        if (app == null || app.controlCode === 0x04) {
            broadcastAutoStarted = null;
            postMessage({ type: "killHtml5", reason: "ait" });
        }
        return;
    }
    if (!html5AutoStartBroadcastAIT || !(networkEnabled && !isRecord)) {
        return;
    }
    const autostart = ait.applications.filter(x => x.controlCode === 0x01 && x.url != null && x.applicationType === 0x0010).sort((a, b) => b.priority - a.priority)[0];
    if (autostart != null) {
        broadcastAutoStarted = `${autostart.organizationId}.${autostart.applicationId}`;
        postMessage({ type: "launchHtml5", ...aitToMessage(autostart, ait) });
    }
}

const bmlBrowser = new BMLBrowser({
    containerElement: contentElement,
    mediaElement: videoContainer,
    indicator: remoteControl,
    fonts: {
        roundGothic,
        boldRoundGothic,
        squareGothic
    },
    epg,
    videoPlaneModeEnabled: true,
    audioNodeProvider: {
        getAudioDestinationNode() {
            return gainNode;
        }
    },
    ip: apiIP,
    inputApplication,
    greg: {
        getReg(index) {
            return window.sessionStorage.getItem(`Greg[${index}]`) ?? "";
        },
        setReg(index, value) {
            window.sessionStorage.setItem(`Greg[${index}]`, value);
        },
    },
    setMainAudioStreamCallback(componentId, channelId) {
        const index = audioESList.findIndex(x => x.componentId === componentId);
        const es = audioESList[index];
        if (es == null) {
            return false;
        }
        postMessage({
            type: "changeMainAudioStream",
            componentId,
            index,
            pid: es.pid,
            channelId,
        });
        return true;
    },
    X_DPA_startResidentApp,
    extension: {
        getBrowserSupport(sProvider, functionname, additionalinfoList) {
            if (sProvider !== "ARIB" || !html5Enabled) {
                return undefined;
            }
            const [a1, a2] = additionalinfoList;
            // IPTVFJ STD-0013 6.1.5 / TR-B14 第三編 第2部 8.8.1(1)
            if (functionname === "APIGroup" && a1 === "AITControlledApp.Start") {
                return 1;
            }
            if (functionname === "AITControlledAppEngineFunction") {
                return a1 === "IPTV-F" && a2 === "HTML5_ph0" ? 1 : 0;
            }
            if (functionname === "AITTransferMethod" || functionname === "AITTransportMethod") {
                return a1 === "XML" && a2 === "HTTP" ? 1 : 0;
            }
            return undefined;
        },
        getResidentAppVersion(appName) {
            if (!html5Enabled) {
                return null;
            }
            return residentAppVersion[appName] ?? null;
        },
        getIRDID(type) {
            // type===5: B-CASカードID (nativeが挿入中のカードから読み取って通知する)
            if (type === 5) {
                return cardIdHex;
            }
            return null;
        },
        aitControlledApp: aitHost,
    },
});

function isFullScreenVideo(): boolean {
    return videoContainer.clientWidth === browserElement.clientWidth && videoContainer.clientHeight === browserElement.clientHeight;
}

// trueであればデータ放送の上に動画を表示させる非表示状態
bmlBrowser.addEventListener("invisible", (evt) => {
    console.log("invisible", evt.detail);
    if (evt.detail) {
        contentElement.style.clipPath = "inset(0px)";
    } else {
        contentElement.style.clipPath = "";
    }
    postMessage({
        type: "invisible",
        invisible: evt.detail || isFullScreenVideo(),
    });
});

bmlBrowser.addEventListener("load", (evt) => {
    console.log("load", evt.detail);
    const width = evt.detail.resolution.width + "px";
    const aspectNum = evt.detail.displayAspectRatio.numerator;
    const aspectDen = evt.detail.displayAspectRatio.denominator;
    const scaleY = (evt.detail.resolution.width / evt.detail.resolution.height) / (aspectNum / aspectDen);
    const transform = `scaleY(${scaleY})`;
    const height = (evt.detail.resolution.height * scaleY) + "px";
    if (browserElement.style.width !== width || browserElement.style.height !== height || contentElement.style.transform !== transform) {
        browserElement.style.width = width;
        browserElement.style.height = height;
        contentElement.style.transform = transform;
        ccContainer.style.width = width;
        ccContainer.style.height = height;
        onResized();
    }
});

type FromWebViewMessage = {
    type: "videoChanged",
    left: number,
    top: number,
    right: number,
    bottom: number,
    invisible: boolean,
} | {
    type: "status",
    url: string,
    receiving: boolean,
    loading: boolean,
} | {
    type: "invisible",
    invisible: boolean,
} | {
    type: "tune",
    originalNetworkId: number,
    transportStreamId: number,
    serviceId: number,
} | {
    type: "nvramRead",
    filename: string,
    structure: string,
    data: any[] | null,
} | {
    type: "usedKeyList",
    usedKeyList: { [key: string]: boolean },
} | {
    type: "cancelInput",
    reason: InputCancelReason,
} | {
    type: "input",
    characterType: InputCharacterType,
    allowedCharacters?: string,
    maxLength: number,
    value: string,
    inputMode: "text" | "password",
    multiline: boolean,
} | {
    type: "changeAudioStream",
    componentId: number,
    pid: number,
    // -1
    index: number,
    channelId?: number,
} | {
    type: "changeMainAudioStream",
    componentId: number,
    pid: number,
    index: number,
    channelId?: number,
} | {
    type: "serviceInfo",
    networkId: number,
    serviceId: number,
    cProfile: boolean,
} | {
    type: "startBrowser",
    uri: string,
    fullscreen: boolean,
} | ({
    type: "launchHtml5",
} & ReturnType<typeof aitToMessage>) | {
    type: "killHtml5",
    reason: string,
} | {
    type: "html5Data",
    data: any,
} | {
    type: "rpcResult",
    result: any,
};

bmlBrowser.addEventListener("videochanged", (evt) => {
    const { left, top, right, bottom } = evt.detail.boundingRect;
    postMessage({
        type: "videoChanged",
        left: left * window.devicePixelRatio,
        top: top * window.devicePixelRatio,
        right: right * window.devicePixelRatio,
        bottom: bottom * window.devicePixelRatio,
        invisible: (bmlBrowser.content.invisible ?? true) || isFullScreenVideo(),
    });
});

bmlBrowser.addEventListener("usedkeylistchanged", (evt) => {
    const { usedKeyList } = evt.detail;
    postMessage({
        type: "usedKeyList",
        usedKeyList: Object.fromEntries([...usedKeyList.values()].map(x => [x, true])),
    });
});

bmlBrowser.addEventListener("audiostreamchanged", (evt) => {
    const { componentId, channelId } = evt.detail;
    const index = audioESList.findIndex(x => x.componentId === componentId);
    postMessage({
        type: "changeAudioStream",
        componentId,
        index,
        pid: audioESList[index]?.pid,
        channelId,
    });
});

let pcr: number | undefined;
let currentTimeCounter = 0;
let lastEIT: any = null;

// LibISDB/Filters/AnalyzerFilter.cppを参照
const audioStreamType = new Set([
    0x03,
    0x04,
    0x0f,
    0x11,
    0x81,
    0x82,
    0x83,
    0x87,
]);

let audioESList: ComponentPMT[] = [];

let pmtRetrieved = false;
// ワンセグのデータ放送の場合dボタンを押すまで起動しない状態にしておく
let cProfile = false;
let oneSegLaunched = false;

function onMessage(msg: ResponseMessage) {
    if (msg.type === "pes") {
        player.push(msg.streamId, Uint8Array.from(msg.data), msg.pts);
    } else if (msg.type === "pcr") {
        pcr = (msg.pcrBase + msg.pcrExtension / 300) / 90;
    } else if (msg.type === "pmt") {
        audioESList = msg.components.filter(x => audioStreamType.has(x.streamType)).sort((a, b) => a.componentId - b.componentId);
        if (!pmtRetrieved) {
            // 地上デジタル放送向けマルチメディア符号化方式(Cプロファイル) 0x000d
            cProfile = msg.components.some(x => x.dataComponentId === 0x000d);
            if (cProfile) {
                // loadが呼ばれる前はまだ960pxが設定されていてデータ取得中の表示が小さくなってしまうので変えておく
                browserElement.style.width = "240px";
                browserElement.style.height = "480px";
                onResized();
                if (!oneSegLaunched) {
                    browserElement.style.visibility = "hidden";
                }
                remoteControlStatusContainer.style.visibility = "visible";
            }
            postMessage({
                type: "serviceInfo",
                networkId,
                serviceId,
                cProfile,
            });
        }
        pmtRetrieved = true;
    } else if (msg.type === "currentTime") {
        isRecord = new Date().getTime() - msg.timeUnixMillis >= 5 * 60 * 1000;
    } else if (msg.type === "eitPresentFollowing") {
        lastEIT = msg;
    } else if (msg.type === "programInfo") {
        if (msg.originalNetworkId != null && msg.transportStreamId != null && msg.serviceId != null) {
            currentServiceIds = { originalNetworkId: msg.originalNetworkId, transportStreamId: msg.transportStreamId, serviceId: msg.serviceId };
        }
    } else if (msg.type === "ait") {
        aitHost.setBroadcastAIT(msg.section);
        processBroadcastAIT();
    }
    // HTML5アプリケーションエンジン向けに中継する
    if (html5Enabled && (msg.type === "eitPresentFollowing" || msg.type === "programInfo" || msg.type === "esEventUpdated" || msg.type === "ait" || msg.type === "pmt" || (msg.type === "currentTime" && ++currentTimeCounter % 10 === 0))) {
        postMessage({ type: "html5Data", data: msg });
    }
    bmlBrowser.emitMessage(msg);
}

const tsStream = decodeTS({
    sendCallback: onMessage,
    serviceId,
    parsePES: true,
});

type ToWebViewMessage = {
    type: "stream",
    data: number[],
    time?: number,
} | {
    type: "streamBase64",
    data: string,
    time?: number,
} | {
    type: "key",
    keyCode: number,
} | {
    type: "caption",
    enable: boolean,
    showIndicator: boolean,
} | {
    type: "nvramRead",
    filename: string,
    structure: string,
} | {
    type: "nvramWrite",
    filename: string,
    structure: string,
    data: any[],
} | {
    type: "volume",
    value: number,
} | {
    type: "nvramDelete",
} | {
    type: "enableNetwork",
    enable: boolean,
} | {
    type: "changeInput",
    value: string,
} | {
    type: "cancelInput",
} | {
    type: "mainAudioStreamChanged",
    pid?: number,
    index: number,
    componentId?: number,
    channelId: number,
} | {
    type: "launchOneSeg",
} | {
    // nativeが挿入中のB-CASカードから読み取ったカードID (16進12桁) またはnull
    type: "irdid",
    value: string | null,
} | {
    type: "html5Config",
    enabled: boolean,
    autoStartBroadcastAIT: boolean,
    residentAppVersion: { [appName: string]: any[] },
} | {
    type: "html5Stopped",
} | {
    type: "bmlVisibility",
    visible: boolean,
} | {
    // HTML5アプリケーションからのreplaceApplication
    type: "html5Replace",
    organizationId: number,
    applicationId: number,
    aitUrl: string | null,
} | {
    // HTML5エンジンからの同期呼び出し
    type: "rpc",
    method: string,
    params: any,
};

// 1分間無操作であればデータ取得中の表示を消す
let remoteControlStatusTimeoutMillis = 60 * 1000;
let remoteControlStatusTimeout = Number.MAX_VALUE;

function fromBase64(input: string): Uint8Array<ArrayBuffer> {
    if ("fromBase64" in globalThis.Uint8Array) {
        return Uint8Array.fromBase64(input);
    } else {
        return Uint8Array.from(window.atob(input), c => c.charCodeAt(0));
    }
}

function onWebViewMessage(data: ToWebViewMessage, reply: (data: FromWebViewMessage) => void) {
    if (!cProfile && data.type !== "key" && performance.now() >= remoteControlStatusTimeout) {
        remoteControlStatusContainer.style.visibility = "hidden";
        remoteControlStatusTimeout = Number.MAX_VALUE;
    }
    if (data.type === "stream") {
        if (!oneSegLaunched && cProfile) {
            return;
        }
        const ts = data.data;
        const prevPCR = pcr;
        tsStream.push(Uint8Array.from(ts));
        const curPCR = pcr;
        if (prevPCR !== curPCR && curPCR != null) {
            player.updateTime(curPCR - 450);
        }
    } else if (data.type === "streamBase64") {
        if (!oneSegLaunched && cProfile) {
            return;
        }
        const ts = data.data;
        const prevPCR = pcr;
        tsStream.push(fromBase64(ts));
        const curPCR = pcr;
        if (prevPCR !== curPCR && curPCR != null) {
            player.updateTime(curPCR - 450);
        }
    } else if (data.type === "key") {
        remoteControlStatusContainer.style.visibility = "visible";
        bmlBrowser.content.processKeyDown(data.keyCode);
        bmlBrowser.content.processKeyUp(data.keyCode);
        remoteControlStatusTimeout = performance.now() + remoteControlStatusTimeoutMillis;
    } else if (data.type === "caption") {
        if (data.enable) {
            if (data.showIndicator) {
                ccStatus.style.display = "";
                ccStatus.textContent = "字幕オン";
                ccStatusAnimation.cancel();
                ccStatusAnimation.play();
            }
            player.showCC();
        } else {
            if (data.showIndicator) {
                ccStatus.style.display = "";
                ccStatus.textContent = "字幕オフ";
                ccStatusAnimation.cancel();
                ccStatusAnimation.play();
            }
            player.hideCC();
        }
    } else if (data.type === "nvramRead") {
        reply({
            type: "nvramRead",
            filename: data.filename,
            structure: data.structure,
            data: bmlBrowser.nvram.readPersistentArray(data.filename, data.structure),
        });
    } else if (data.type === "nvramWrite") {
        bmlBrowser.nvram.writePersistentArray(data.filename, data.structure, data.data, undefined, true);
    } else if (data.type === "volume") {
        gainNode.gain.value = data.value;
    } else if (data.type === "nvramDelete") {
        const keys: string[] = [];
        for (let i = 0; i < window.localStorage.length; i++) {
            const key = window.localStorage.key(i);
            if (key != null && key.startsWith("nvram_")) {
                keys.push(key);
            }
        }
        for (const key of keys) {
            window.localStorage.removeItem(key);
        }
    } else if (data.type === "enableNetwork") {
        networkEnabled = data.enable;
    } else if (data.type === "changeInput") {
        changeCallback?.(data.value);
        changeCallback = undefined;
    } else if (data.type === "cancelInput") {
        changeCallback = undefined;
    } else if (data.type === "mainAudioStreamChanged") {
        if (data.componentId != null) {
            bmlBrowser.setMainAudioStream(data.componentId, data.channelId);
        } else {
            const index = data.pid != null ? audioESList.findIndex(x => x.pid === data.pid) : data.index;
            const es = audioESList[index];
            if (es != null) {
                bmlBrowser.setMainAudioStream(es.componentId, data.channelId);
            }
        }
    } else if (data.type === "irdid") {
        cardIdHex = data.value;
    } else if (data.type === "html5Config") {
        html5Enabled = data.enabled;
        html5AutoStartBroadcastAIT = data.autoStartBroadcastAIT;
        residentAppVersion = data.residentAppVersion ?? {};
    } else if (data.type === "html5Stopped") {
        broadcastAutoStarted = null;
    } else if (data.type === "bmlVisibility") {
        // 子要素のvisibilityで打ち消されないようにopacityで隠す
        browserElement.style.opacity = data.visible ? "" : "0";
        ccContainer.style.opacity = data.visible ? "" : "0";
    } else if (data.type === "html5Replace") {
        const org = data.organizationId.toString(16);
        const app = data.applicationId.toString(16);
        let result: number;
        if (data.aitUrl == null) {
            result = aitHost.startAITControlledApp(org, app, undefined, null);
        } else {
            aitHost.startReceivingAIT(data.aitUrl, null);
            result = aitHost.startAITControlledApp(org, app, data.aitUrl, null);
        }
        if (Number.isNaN(result)) {
            // 遷移が不可能な場合はdestroyApplicationと同様に再選局と同等の動作を行う
            postMessage({ type: "killHtml5", reason: "replace" });
        }
    } else if (data.type === "rpc") {
        reply({ type: "rpcResult", result: handleRPC(data.method, data.params) });
    } else if (data.type === "launchOneSeg") {
        oneSegLaunched = true;
        browserElement.style.visibility = "visible";
    }
}

// HTML5アプリケーションエンジン (navigator.bmlCompat.browserPseudoなど) からの同期呼び出し
function handleRPC(method: string, params: any): any {
    switch (method) {
        case "readPersistentArray":
            return bmlBrowser.nvram.readPersistentArray(params.filename, params.structure);
        case "writePersistentArray":
            return bmlBrowser.nvram.writePersistentArray(params.filename, params.structure, params.data, undefined, true);
        case "getGreg":
            return Array.from({ length: 64 }, (_, i) => window.sessionStorage.getItem(`Greg[${i}]`) ?? "");
        case "setGreg":
            window.sessionStorage.setItem(`Greg[${params.index}]`, String(params.value));
            return true;
        case "getCurrentEvent":
            return lastEIT;
        case "getBroadcastAIT":
            return aitHost.getBroadcastAIT();
    }
    return null;
}

window.chrome.webview.addEventListener("message", (ev: any) => onWebViewMessage(ev.data as ToWebViewMessage, window.chrome.webview.postMessage));

window.sendMessage = (data: ToWebViewMessage): FromWebViewMessage | null => {
    let result: FromWebViewMessage | null = null;
    onWebViewMessage(data, (data) => result = data);
    return result;
};

function onResized() {
    const windowWidth = document.documentElement.clientWidth;
    const windowHeight = document.documentElement.clientHeight;
    const contentWidth = browserElement.clientWidth;
    const contentHeight = browserElement.clientHeight;
    const scaleX = windowWidth / contentWidth;
    const scaleY = windowHeight / contentHeight;
    const s = Math.min(scaleX, scaleY);
    document.body.style.transform = `translate(${Math.ceil((contentWidth * s + windowWidth) / 2 - contentWidth * s)}px, ${Math.ceil((contentHeight * s + windowHeight) / 2 - contentHeight * s)}px) scale(${s})`;
    document.body.style.transformOrigin = `0px 0px`;
    const { left, top, right, bottom } = videoContainer.getBoundingClientRect();
    postMessage({
        type: "videoChanged",
        left: left * window.devicePixelRatio,
        top: top * window.devicePixelRatio,
        right: right * window.devicePixelRatio,
        bottom: bottom * window.devicePixelRatio,
        invisible: (bmlBrowser.content.invisible ?? true) || isFullScreenVideo(),
    });
}

window.addEventListener("resize", onResized);

onResized();

window.bmlBrowser = bmlBrowser;
