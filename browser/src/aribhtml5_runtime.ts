// ARIB-HTML5 / Hybridcast 受信機エミュレーション
// WebView2のAddScriptToExecuteOnDocumentCreatedで文書生成前に注入される
//
// 参照:
//   IPTVFJ STD-0013 (ハイブリッドキャスト運用規定) 6.1.4, 8.1.4
//   ARIB STD-B62 第2部 2.2, 3.3
//   ARIB TR-B39 第1部 (マルチメディアサービス運用規定) 10.7, 10.10
//   ARIB STD-B24 第四編 (AIT)

import { romsoundData } from "../../web-bml/dist/client/romsound_data.js";

type Profile = "hybridcast" | "bs4k";

type AppInfo = {
    organizationId: number,
    applicationId: number,
    controlCode: number,
    applicationType: number,
    typeName?: string,
    priority: number,
    url: string | null,
    boundaries: { permissionBitmaps: number[], managedURLs: string[] }[],
};

type RuntimeConfig = {
    profile: Profile,
    app: AppInfo,
    applications: AppInfo[],
    aitSource: string | null,
    // 現在のサービス
    originalNetworkId: number,
    transportStreamId: number,
    tlvStreamId?: number,
    serviceId: number,
    // 映像オブジェクトとして扱うMIME type
    broadcastObjectTypes: string[],
    // VK_*の値
    keyCodes: { [name: string]: number },
    // KeySetの定数値
    keySetValues: { [name: string]: number },
    systemInformation: { makerid: string, browsername: string, browserversion: string, modelname: string, baseurl: string },
    deviceIdentifiers: { [type: string]: string },
    // viewport未指定時の解像度
    defaultResolution: { width: number, height: number },
    rpcUrl: string,
    debug: boolean,
};

declare global {
    interface Window {
        __aribHtml5Config?: RuntimeConfig;
        __aribHtml5?: any;
    }
}

(() => {
    if (window.top !== window) {
        return;
    }
    const config = window.__aribHtml5Config;
    if (config == null || window.__aribHtml5 != null) {
        return;
    }
    const webview = (window as any).chrome?.webview;
    const log = (...args: any[]) => {
        if (config.debug) {
            console.log("[aribhtml5]", ...args);
        }
    };

    // ---------------------------------------------------------------
    // native通信
    // ---------------------------------------------------------------
    function post(msg: any) {
        webview?.postMessage(msg);
    }

    // 同期呼び出し (同期XHR, nativeのWebResourceRequestedで処理される)
    function rpc(method: string, params?: any): any {
        try {
            const xhr = new XMLHttpRequest();
            xhr.open("POST", config!.rpcUrl, false);
            xhr.setRequestHeader("Content-Type", "text/plain");
            xhr.send(JSON.stringify({ method, params: params ?? null }));
            if (xhr.status !== 200) {
                return undefined;
            }
            const res = JSON.parse(xhr.responseText);
            return res?.result;
        } catch (e) {
            log("rpc failed", method, e);
            return undefined;
        }
    }

    const listeners = new Map<string, Set<(data: any) => void>>();
    function on(type: string, cb: (data: any) => void) {
        let s = listeners.get(type);
        if (s == null) {
            s = new Set();
            listeners.set(type, s);
        }
        s.add(cb);
    }
    webview?.addEventListener("message", (ev: any) => {
        const data = ev.data;
        if (data == null || typeof data.type !== "string") {
            return;
        }
        for (const cb of listeners.get(data.type) ?? []) {
            try {
                cb(data);
            } catch (e) {
                console.error(e);
            }
        }
    });

    // ---------------------------------------------------------------
    // エラー (TR-B39 10.7.2 文字列として送出)
    // ---------------------------------------------------------------
    const errors = {
        INVALID_PARAM_ERR: "Error:Invalid parameter",
        TIMEOUT_ERR: "Error:Timeout",
        NOT_FOUND_ERR: "Error:File not found",
        NOT_AUTHORIZED_ERR: "Error:Not authorized",
        OUT_OF_RANGE_ERR: "Error:Index out of range",
        MISC_ERR: "Error:Unknown error",
    } as const;
    function raise(name: keyof typeof errors): never {
        throw errors[name];
    }

    // ---------------------------------------------------------------
    // パーミッション (STD-0013 6.1.3, 8.1.4.3.17.1 / TR-B39 10.7.1)
    // 文書のURLに最長一致するバウンダリのpermission_bitmapを適用
    // ---------------------------------------------------------------
    function matchBoundary(url: URL, managed: string): number {
        let m: URL;
        try {
            m = new URL(/^https?:\/\//i.test(managed) ? managed : "http://" + managed);
        } catch {
            return -1;
        }
        if (m.protocol !== url.protocol && /^https?:\/\//i.test(managed)) {
            return -1;
        }
        const hasPath = m.pathname !== "/" || /^https?:\/\/[^/]+\//i.test(managed);
        if (hasPath) {
            if (url.hostname !== m.hostname) {
                return -1;
            }
            if (!url.pathname.startsWith(m.pathname)) {
                return -1;
            }
            return 1000 + m.pathname.length;
        }
        if (url.hostname === m.hostname || url.hostname.endsWith("." + m.hostname)) {
            return m.hostname.length;
        }
        return -1;
    }
    function currentPermission(): number {
        if (config!.profile === "bs4k" && location.hostname.endsWith(".invalid")) {
            // 放送で伝送されたコンテンツは全権限
            return 0x1fff;
        }
        const boundaries = config!.app.boundaries;
        if (boundaries.length === 0) {
            // 記述子が無い場合はバウンダリ無限大かつ全て許可
            return 0x1fff;
        }
        let url: URL;
        try {
            url = new URL(location.href);
        } catch {
            return 0;
        }
        let best = -2;
        let bitmap = -1;
        for (const b of boundaries) {
            const urls = b.managedURLs.length === 0 ? [null] : b.managedURLs;
            for (const u of urls) {
                const score = u == null ? -1 + 0.5 : matchBoundary(url, u);
                if (score > best && (u == null || score >= 0)) {
                    best = score;
                    bitmap = b.permissionBitmaps.find(x => (x >> 13) === 0) ?? b.permissionBitmaps[0] ?? 0;
                }
            }
        }
        // バウンダリ外
        return bitmap < 0 ? -1 : (bitmap & 0x1fff);
    }
    const permission = currentPermission();
    const managed = permission >= 0;
    log("permission", permission.toString(16), "managed", managed);
    if (!managed) {
        // バウンダリ外: 非マネージド状態 (放送関連APIは使用不可)
        post({ type: "html5Unmanaged", url: location.href });
    }
    const PERM = {
        AV: 1 << 12,
        OVERLAY: 1 << 11,
        EVENT_INFO: 1 << 10,
        NVRAM: 1 << 9,
        ADD_PERMISSION: 1 << 8,
        DEVICE_ID: 1 << 7,
        CONFIRM_IP: 1 << 6,
        MISC: 1 << 5,
        RESERVE: 1 << 4,
        CAS: 1 << 3,
    };
    function requirePermission(bit: number) {
        if (!managed || (permission & bit) === 0) {
            raise("NOT_AUTHORIZED_ERR");
        }
    }

    // ---------------------------------------------------------------
    // 番組情報・イベントの状態
    // ---------------------------------------------------------------
    type EITInfo = { eventId: number, startTimeUnixMillis: number | null, durationSeconds: number | null, eventName: string | null, eventText: string | null };
    let eit: { originalNetworkId: number, transportStreamId: number, serviceId: number, present?: EITInfo, following?: EITInfo } | null = rpc("getCurrentEvent") ?? null;
    const eventIdListeners = new Set<(ref: any) => void>();
    const aitListeners = new Set<(ait: any) => void>();
    on("html5Data", ({ data }) => {
        if (data.type === "eitPresentFollowing") {
            const prevEventId = eit?.present?.eventId;
            eit = data;
            if (data.present != null && data.present.eventId !== prevEventId) {
                const ref = serviceRef({ event_id: data.present.eventId });
                for (const l of [...eventIdListeners]) {
                    queueMicrotask(() => l(ref));
                }
            }
        } else if (data.type === "esEventUpdated") {
            processEventMessages(data);
        } else if (data.type === "ait") {
            const ait = makeAITObject(convertAIT(data.section));
            for (const l of [...aitListeners]) {
                queueMicrotask(() => l(ait));
            }
        }
    });

    function serviceRef(extra?: object) {
        if (config!.profile === "bs4k") {
            return { original_network_id: config!.originalNetworkId, tlv_stream_id: config!.tlvStreamId ?? config!.transportStreamId, service_id: config!.serviceId, ...extra };
        }
        return { original_network_id: config!.originalNetworkId, transport_stream_id: config!.transportStreamId, service_id: config!.serviceId, ...extra };
    }

    function convertAIT(section: any): AppInfo[] {
        return (section?.applications ?? []).map((a: any) => ({
            organizationId: a.organizationId,
            applicationId: a.applicationId,
            controlCode: a.controlCode,
            applicationType: section.applicationType,
            priority: a.priority,
            url: null,
            boundaries: a.boundaries ?? [],
        }));
    }

    // ---------------------------------------------------------------
    // 汎用イベントメッセージ (STD-0013 6.1.4.11 / STD-B62 3.3.12)
    // ---------------------------------------------------------------
    type GEMListener = { params: any, listener: (msg: any) => void, last?: string };
    const gemListeners: GEMListener[] = [];
    const decoderEUCJP = (() => {
        try {
            return new TextDecoder("euc-jp");
        } catch {
            return new TextDecoder("utf-8");
        }
    })();
    function processEventMessages(data: { componentId: number, events: any[] }) {
        for (const ev of data.events) {
            if (ev.type !== "immediateEvent" && ev.type !== "nptEvent") {
                continue;
            }
            // event_msg_id: 上位8bit message_id, 下位8bit message_version
            const messageId = (ev.eventMessageId >> 8) & 0xff;
            const messageVersion = ev.eventMessageId & 0xff;
            for (const l of gemListeners) {
                const p = l.params ?? {};
                const tag = p.es_ref?.component_tag ?? p.source?.component_tag ?? 0x40;
                if (tag !== data.componentId) {
                    continue;
                }
                const groupId = p.message_group_id ?? 1;
                if (groupId !== ev.eventMessageGroupId && !(groupId === 0 || ev.eventMessageGroupId === 0)) {
                    continue;
                }
                if (p.message_id != null && p.message_id !== messageId) {
                    continue;
                }
                if (p.message_id != null && p.message_version != null && p.message_version !== messageVersion) {
                    continue;
                }
                const key = `${data.componentId}:${ev.eventMessageGroupId}:${messageId}:${messageVersion}`;
                if (l.last === key) {
                    continue;
                }
                l.last = key;
                const msg = {
                    es_ref: serviceRef({ component_tag: data.componentId }),
                    source: serviceRef({ component_tag: data.componentId }),
                    message_group_id: ev.eventMessageGroupId,
                    message_id: messageId,
                    message_version: messageVersion,
                    private_data_byte: decoderEUCJP.decode(new Uint8Array(ev.privateDataByte ?? [])),
                };
                queueMicrotask(() => l.listener(msg));
            }
        }
    }

    // ---------------------------------------------------------------
    // StreamEventTarget
    // ---------------------------------------------------------------
    const streamEvent = {
        addGeneralEventMessageListener(params: any, listener: (msg: any) => void) {
            requirePermission(PERM.MISC);
            if (typeof listener !== "function") {
                raise("INVALID_PARAM_ERR");
            }
            const refs = new Set(gemListeners.map(x => x.params?.es_ref?.component_tag ?? x.params?.source?.component_tag ?? 0x40));
            refs.add(params?.es_ref?.component_tag ?? params?.source?.component_tag ?? 0x40);
            if (refs.size > 3) {
                raise("OUT_OF_RANGE_ERR");
            }
            gemListeners.push({ params, listener });
        },
        removeGeneralEventMessageListener(params: any, listener?: (msg: any) => void) {
            const same = (a: any, b: any) => JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
            for (let i = gemListeners.length - 1; i >= 0; i--) {
                if (same(gemListeners[i].params, params) && (listener == null || gemListeners[i].listener === listener)) {
                    gemListeners.splice(i, 1);
                }
            }
        },
        addEventIDUpdateListener(listener: (ref: any) => void) {
            requirePermission(PERM.MISC);
            eventIdListeners.add(listener);
            // 登録した直後にその時点で最新のEIT p/fに基づいて一度割り込みが発生する
            if (eit?.present != null) {
                const ref = serviceRef({ event_id: eit.present.eventId });
                setTimeout(() => {
                    if (eventIdListeners.has(listener)) {
                        listener(ref);
                    }
                }, 0);
            }
        },
        removeEventIDUpdateListener(listener?: (ref: any) => void) {
            if (listener == null) {
                eventIdListeners.clear();
            } else {
                eventIdListeners.delete(listener);
            }
        },
        addAITUpdateListener(listener: (ait: any) => void) {
            requirePermission(PERM.MISC);
            aitListeners.add(listener);
        },
        removeAITUpdateListener(listener?: (ait: any) => void) {
            if (listener == null) {
                aitListeners.clear();
            } else {
                aitListeners.delete(listener);
            }
        },
        sourceIs(name: string): boolean {
            // "onair"のみ (STD-0013 6.1.4.10)
            return name === "onair" && !rpc("isRecord");
        },
        // NPT (TSのNPTは運用しない)
        addNPTReferenceMessageListener() { },
        removeNPTReferenceMessageListener() { },
        setAlarmByNPT() { raise("NOT_AUTHORIZED_ERR"); },
        unsetAlarmByNPT() { },
        getNPT() { raise("NOT_AUTHORIZED_ERR"); },
        // UTC-NPT (MMT) BS4K用
        addUTCNPTReferenceMessageListener(_params: any, listener: (x: any) => void) {
            utcnptListeners.add(listener);
            if (lastUTCNPT != null) {
                setTimeout(() => listener({ source: serviceRef() }), 0);
            }
        },
        removeUTCNPTReferenceMessageListener(_params?: any, listener?: (x: any) => void) {
            if (listener == null) {
                utcnptListeners.clear();
            } else {
                utcnptListeners.delete(listener);
            }
        },
        setAlarmByUTCNPT(params: any, handler: (x: any) => void): number {
            const handle = nextAlarmHandle++;
            utcnptAlarms.set(handle, { value: Number(params?.utcnpt_value ?? 0), handler });
            return handle;
        },
        unsetAlarmByUTCNPT(handle: number) {
            utcnptAlarms.delete(handle);
        },
        getUTCNPT(): number {
            if (lastUTCNPT == null) {
                raise("NOT_FOUND_ERR");
            }
            return lastUTCNPT.npt + (Date.now() - lastUTCNPT.receivedAt) / 1000;
        },
    };
    const utcnptListeners = new Set<(x: any) => void>();
    const utcnptAlarms = new Map<number, { value: number, handler: (x: any) => void }>();
    let nextAlarmHandle = 1;
    let lastUTCNPT: { npt: number, receivedAt: number } | null = null;
    on("utcnpt", ({ npt }) => {
        const first = lastUTCNPT == null;
        lastUTCNPT = { npt, receivedAt: Date.now() };
        if (first) {
            for (const l of [...utcnptListeners]) {
                l({ source: serviceRef() });
            }
        }
    });
    setInterval(() => {
        if (lastUTCNPT == null) {
            return;
        }
        const now = lastUTCNPT.npt + (Date.now() - lastUTCNPT.receivedAt) / 1000;
        for (const [handle, alarm] of [...utcnptAlarms]) {
            if (alarm.value <= now) {
                utcnptAlarms.delete(handle);
                alarm.handler({ source: serviceRef(), utcnpt_value: alarm.value });
            }
        }
    }, 100);

    // ---------------------------------------------------------------
    // CacheEventTarget (BS4K)
    // ---------------------------------------------------------------
    const cacheListeners = new Map<string, Set<(path: string, event: string) => void>>();
    on("cacheEvent", ({ path, event }) => {
        for (const [p, ls] of cacheListeners) {
            if (path === p || path.startsWith(p.endsWith("/") ? p : p + "/")) {
                for (const l of [...ls]) {
                    l(path, event);
                }
            }
        }
    });
    const cacheEvent = {
        addCacheEventListener(path: string, listener: (path: string, event: string) => void) {
            requirePermission(PERM.MISC);
            let s = cacheListeners.get(path);
            if (s == null) {
                s = new Set();
                cacheListeners.set(path, s);
            }
            s.add(listener);
        },
        removeCacheEventListener(path: string, listener?: (path: string, event: string) => void) {
            if (listener == null) {
                cacheListeners.delete(path);
            } else {
                cacheListeners.get(path)?.delete(listener);
            }
        },
        storeDataResource(path: string, listener?: (path: string, event: string) => void) {
            requirePermission(PERM.MISC);
            const ok = rpc("storeDataResource", { path });
            if (listener != null) {
                setTimeout(() => listener(path, ok ? "store_finished" : "store_failed"), 0);
            }
        },
        releaseDataResource(_path?: string) { },
    };

    // ---------------------------------------------------------------
    // ReceiverDevice (STD-B62 3.3.11 / STD-0013 6.1.4.5～6.1.4.9)
    // ---------------------------------------------------------------
    function toDate(ms: number | null | undefined): Date | null {
        return ms == null ? null : new Date(ms);
    }
    const receiverDevice: any = {
        getDeviceIdentifier(type: number, callback: (id: string | null) => void) {
            requirePermission(PERM.DEVICE_ID);
            const id = config!.deviceIdentifiers[String(type)] ?? config!.deviceIdentifiers["0x" + Number(type).toString(16)] ?? null;
            setTimeout(() => callback(id), 0);
        },
        getSystemInformation(_query?: string[] | null): object {
            requirePermission(PERM.MISC);
            return { ...config!.systemInformation };
        },
        tuneTo(service_ref: any, _resultCallback?: any, _options?: any) {
            requirePermission(PERM.MISC);
            if (service_ref == null || typeof service_ref !== "object") {
                raise("INVALID_PARAM_ERR");
            }
            const onid = service_ref.original_network_id ?? config!.originalNetworkId;
            const tsid = service_ref.transport_stream_id ?? service_ref.tlv_stream_id ?? config!.transportStreamId;
            const sid = service_ref.service_id ?? config!.serviceId;
            post({ type: "html5Tune", originalNetworkId: onid, transportStreamId: tsid, serviceId: sid });
        },
        getCurrentEventInformation(callback: (info: any) => void) {
            requirePermission(PERM.EVENT_INFO);
            if (typeof callback !== "function") {
                raise("INVALID_PARAM_ERR");
            }
            const latest = rpc("getCurrentEvent") ?? eit;
            if (latest != null) {
                eit = latest;
            }
            const p = eit?.present;
            const f = eit?.following;
            const info: any = {
                ...serviceRef(),
                event_id: p?.eventId ?? null,
                start_time: toDate(p?.startTimeUnixMillis),
                duration: p?.durationSeconds == null ? null : p.durationSeconds * 1000,
                name: p?.eventName ?? null,
                desc: p?.eventText ?? null,
                f_event_id: f?.eventId ?? null,
                f_start_time: toDate(f?.startTimeUnixMillis),
                f_duration: f?.durationSeconds == null ? null : f.durationSeconds * 1000,
                f_name: f?.eventName ?? null,
                f_desc: f?.eventText ?? null,
            };
            setTimeout(() => callback(info), 0);
        },
        isCommonEvent(service_ref: any): boolean {
            requirePermission(PERM.MISC);
            const r = rpc("isCommonEvent", service_ref);
            return !!r;
        },
        // EPG予約 (未対応)
        isScheduledToTune() { requirePermission(PERM.RESERVE); return false; },
        scheduleToTune() { requirePermission(PERM.RESERVE); raise("MISC_ERR"); },
        unscheduleToTune() { requirePermission(PERM.RESERVE); },
        isScheduledToRecord() { requirePermission(PERM.RESERVE); return false; },
        scheduleToRecord() { requirePermission(PERM.RESERVE); raise("MISC_ERR"); },
        unscheduleToRecord() { requirePermission(PERM.RESERVE); },
        confirmIPNetwork(destination: string, confirmType: number, timeout?: number): boolean {
            const r = rpc("confirmIPNetwork", { destination, confirmType, timeout: timeout ?? 5000 });
            return !!r;
        },
        streamEvent,
        cacheEvent,
    };
    // typoされた名前も使われている (STD-0013の記述例)
    receiverDevice.getCurrentEventInforamtion = receiverDevice.getCurrentEventInformation;

    // ---------------------------------------------------------------
    // KeySet (STD-0013 6.1.4.17 / TR-B39 10.10.2.3)
    // ---------------------------------------------------------------
    const ks = config.keySetValues;
    const keySetGroups: { [group: string]: string[] } = {
        RED: ["VK_RED"],
        GREEN: ["VK_GREEN"],
        YELLOW: ["VK_YELLOW"],
        BLUE: ["VK_BLUE"],
        NAVIGATION: ["VK_UP", "VK_DOWN", "VK_LEFT", "VK_RIGHT", "VK_ENTER", "VK_BACK", "VK_PAGE_UP", "VK_PAGE_DOWN", "VK_TAB"],
        NUMERIC: ["VK_0", "VK_1", "VK_2", "VK_3", "VK_4", "VK_5", "VK_6", "VK_7", "VK_8", "VK_9", "VK_10", "VK_11", "VK_12"],
        VCR: ["VK_PLAY", "VK_PAUSE", "VK_PLAY_PAUSE", "VK_STOP", "VK_FAST_FWD", "VK_REWIND", "VK_TRACK_NEXT", "VK_TRACK_PREV", "VK_VCR_OTHER"],
        DBUTTON: ["VK_DBUTTON"],
    };
    const defaultKeySetValue = (ks.NAVIGATION | ks.RED | ks.GREEN | ks.YELLOW | ks.BLUE | ks.DBUTTON) >>> 0;
    let keySetValue = defaultKeySetValue;
    function notifyKeySet() {
        const groups: { [g: string]: boolean } = {};
        for (const g of Object.keys(keySetGroups)) {
            groups[g] = (keySetValue & (ks[g] ?? 0)) !== 0;
        }
        post({ type: "html5KeySet", value: keySetValue, groups });
    }
    const keySet: any = {
        get value() {
            return keySetValue;
        },
        setValue(value: number): number {
            let v = 0;
            for (const g of Object.keys(keySetGroups)) {
                if ((Number(value) & (ks[g] ?? 0)) !== 0) {
                    v |= ks[g];
                }
            }
            keySetValue = v >>> 0;
            notifyKeySet();
            return keySetValue;
        },
    };
    for (const [name, value] of Object.entries(ks)) {
        Object.defineProperty(keySet, name, { value, enumerable: true });
    }
    for (const [name, value] of Object.entries(config.keyCodes)) {
        Object.defineProperty(window, name, { value, enumerable: false, configurable: true });
        // KeyEventにもVK_*を生やす (OIPF互換)
        try {
            Object.defineProperty(KeyboardEvent, name, { value, configurable: true });
            Object.defineProperty(KeyboardEvent.prototype, name, { value, configurable: true });
        } catch { }
    }
    // keyCode名 → keySetのグループ
    const keyToGroup = new Map<string, string>();
    for (const [g, names] of Object.entries(keySetGroups)) {
        for (const n of names) {
            keyToGroup.set(n, g);
        }
    }

    const keyEventKey: { [name: string]: string } = {
        VK_UP: "ArrowUp", VK_DOWN: "ArrowDown", VK_LEFT: "ArrowLeft", VK_RIGHT: "ArrowRight",
        VK_ENTER: "Enter", VK_BACK: "BrowserBack", VK_RED: "ColorF0Red", VK_GREEN: "ColorF1Green",
        VK_YELLOW: "ColorF2Yellow", VK_BLUE: "ColorF3Blue", VK_PLAY: "MediaPlay", VK_PAUSE: "MediaPause",
        VK_PLAY_PAUSE: "MediaPlayPause", VK_STOP: "MediaStop", VK_FAST_FWD: "MediaFastForward",
        VK_REWIND: "MediaRewind", VK_TRACK_NEXT: "MediaTrackNext", VK_TRACK_PREV: "MediaTrackPrevious",
        VK_PAGE_UP: "PageUp", VK_PAGE_DOWN: "PageDown", VK_TAB: "Tab",
    };

    function dispatchKey(type: "keydown" | "keyup", name: string): boolean {
        const keyCode = config!.keyCodes[name];
        if (keyCode == null) {
            return false;
        }
        const key = keyEventKey[name] ?? (name.startsWith("VK_") && /^\d+$/.test(name.substring(3)) ? name.substring(3) : "Unidentified");
        const ev = new KeyboardEvent(type, { key, bubbles: true, cancelable: true, composed: true });
        Object.defineProperty(ev, "keyCode", { get: () => keyCode });
        Object.defineProperty(ev, "which", { get: () => keyCode });
        Object.defineProperty(ev, "charCode", { get: () => 0 });
        const target = (document.activeElement ?? document.body ?? document.documentElement) as Element;
        return target.dispatchEvent(ev);
    }

    const handleKey = ({ name, action }: { name: string, action: string }) => {
        const group = keyToGroup.get(name);
        if (group == null || (keySetValue & (ks[group] ?? 0)) === 0) {
            post({ type: "html5KeyUnhandled", name });
            return;
        }
        if (action === "down" || action === "press") {
            const notCanceled = dispatchKey("keydown", name);
            if (action === "press") {
                dispatchKey("keyup", name);
            }
            if (notCanceled && name === "VK_BACK") {
                // 既定の動作 (ブラウザの戻る)
                post({ type: "html5KeyUnhandled", name, default: true });
            }
        } else {
            dispatchKey("keyup", name);
        }
    };
    on("key", handleKey);

    // ---------------------------------------------------------------
    // Application / ApplicationManager (STD-B62 3.3.7～3.3.9 / STD-0013 6.1.4.4)
    // ---------------------------------------------------------------
    const controlCodeNames: { [code: number]: string } = { 1: "AUTOSTART", 2: "PRESENT", 3: "DESTROY", 4: "KILL", 5: "PREFETCH", 6: "REMOTE", 7: "DISABLED", 8: "PLAYBACK_AUTOSTART" };
    function makeApplication(info: AppInfo, owner: boolean): any {
        const app: any = {
            type: info.typeName ?? (info.applicationType === 0x0011 ? "ARIB-HTML5" : "ISDB-HTML"),
            organization_id: info.organizationId,
            application_id: info.applicationId,
            control_code: controlCodeNames[info.controlCode] ?? String(info.controlCode),
            autostart_priority: info.priority,
        };
        if (!owner) {
            return app;
        }
        app.keySet = keySet;
        app.replaceApplication = (organization_id: number, application_id: number, ait_url?: string | null) => {
            requirePermission(PERM.MISC);
            if (ait_url != null && !/^https?:\/\//i.test(String(ait_url))) {
                raise("INVALID_PARAM_ERR");
            }
            post({ type: "html5Replace", organizationId: Number(organization_id), applicationId: Number(application_id), aitUrl: ait_url ?? null });
        };
        app.destroyApplication = () => {
            post({ type: "html5Destroy" });
        };
        app.exitFromManagedState = (url: string) => {
            if (typeof url !== "string" || url.length > 250 || !/^https?:\/\//i.test(url)) {
                raise("INVALID_PARAM_ERR");
            }
            post({ type: "html5ExitManaged", url });
        };
        app.getOwnerAIT = () => {
            return makeAITObject(config!.applications);
        };
        const addedAreas: { permission: number[] | null, urls: string[] | null }[] = [];
        app.getApplicationBoundaryAndPermissionDescriptor = () => ({
            getCurrentBoundary() {
                return [
                    ...config!.app.boundaries.map(b => ({ permission: b.permissionBitmaps, urls: b.managedURLs })),
                    ...addedAreas,
                ];
            },
            addPermissionManagedArea(pma: any) {
                requirePermission(PERM.ADD_PERMISSION);
                addedAreas.push({ permission: pma?.permission ?? null, urls: pma?.urls ?? null });
            },
        });
        return app;
    }
    function makeAITObject(apps: AppInfo[]) {
        const applications = apps.map(x => makeApplication(x, false));
        return {
            getApplications() {
                return applications.slice();
            },
        };
    }
    const ownerApplication = makeApplication(config.app, true);
    const applicationManager = {
        getOwnerApplication(_document?: Document) {
            return managed ? ownerApplication : null;
        },
        launchDataBroadcastingBrowser(startup?: any) {
            requirePermission(PERM.MISC);
            post({ type: "html5LaunchDataBroadcasting", componentTag: startup?.component_tag ?? null });
        },
    };

    // ---------------------------------------------------------------
    // Capabilities (STD-B62 3.3.10 / TR-B39 10.7.11)
    // ---------------------------------------------------------------
    const capabilities = {
        hasCapability(query: string, ...params: string[]): boolean {
            switch (query) {
                case "KeyGroup":
                    return params[0] === "VCR";
                case "BrowserResolution":
                    return params[0] === "2K" || (config!.profile === "bs4k" && params[0] === "4K");
                case "InputDevice":
                    return params[1] === "keyboard" || params[1] === "mouse";
                case "AudioElement":
                    return true;
                case "VideoElement":
                    return false;
                case "MMTVideoStream":
                    return config!.profile === "bs4k";
                case "CASystem":
                case "CASsystemAPI":
                    return false;
            }
            return false;
        },
    };

    // ---------------------------------------------------------------
    // bmlCompat (Hybridcast: STD-0013 6.1.4.14, 6.1.4.15)
    // ---------------------------------------------------------------
    let gregCache: string[] | null = null;
    function greg(): string[] {
        if (gregCache == null) {
            gregCache = rpc("getGreg") ?? Array.from({ length: 64 }, () => "");
        }
        return gregCache!;
    }
    const gregProxy = new Proxy([] as string[], {
        get(_target, prop) {
            if (!managed) {
                raise("NOT_AUTHORIZED_ERR");
            }
            if (prop === "length") {
                return 64;
            }
            const i = typeof prop === "string" ? Number(prop) : NaN;
            if (Number.isInteger(i) && i >= 0 && i < 64) {
                return greg()[i];
            }
            return (Array.prototype as any)[prop];
        },
        set(_target, prop, value) {
            if (!managed) {
                raise("NOT_AUTHORIZED_ERR");
            }
            const i = typeof prop === "string" ? Number(prop) : NaN;
            if (Number.isInteger(i) && i >= 0 && i < 64) {
                greg()[i] = String(value);
                rpc("setGreg", { index: i, value: String(value) });
                return true;
            }
            return false;
        },
    });
    const bmlCompat = {
        browserPseudo: {
            readPersistentArray(filename: string, structure?: string) {
                requirePermission(PERM.NVRAM);
                return rpc("readPersistentArray", { filename, structure }) ?? null;
            },
            writePersistentArray(filename: string, structure: string, data: any[], _period?: Date) {
                requirePermission(PERM.NVRAM);
                const r = rpc("writePersistentArray", { filename, structure, data });
                return typeof r === "number" ? r : NaN;
            },
            get Greg() {
                return gregProxy;
            },
        },
    };

    // ---------------------------------------------------------------
    // localStorageの予約キー (BS4K: STD-B62 3.3.5 / TR-B39 10.1)
    // ---------------------------------------------------------------
    // TR-B39 表10-4 視聴者居住情報領域
    //   _prefecture: STD-B10 第2部 付録G 県域指定ビットマップ(56bit)の16進14桁 (例: 東京都 "00040000000000")
    //   _regioncode: STD-B10 第2部 付録D 地域符号(12bit)の16進3桁 (例: "aac")
    //   _zipcode: 郵便番号
    // BMLのnvram://receiverinfo/prefectureは県域指定ビットマップのビット順(1始まり, 1=東北海道)と同じ
    function receiverInfo(key: "_prefecture" | "_regioncode" | "_zipcode"): string {
        try {
            if (key === "_zipcode") {
                const r = rpc("readPersistentArray", { filename: "nvram://receiverinfo/zipcode", structure: "S:7B" });
                return typeof r?.[0] === "string" ? r[0].replace(/[^0-9]/g, "") : "";
            }
            if (key === "_prefecture") {
                const r = rpc("readPersistentArray", { filename: "nvram://receiverinfo/prefecture", structure: "U:1B" });
                const code = Number(r?.[0]);
                if (!(code >= 1 && code <= 56)) {
                    return "";
                }
                const bits = "0".repeat(code - 1) + "1" + "0".repeat(56 - code);
                return BigInt("0b" + bits).toString(16).padStart(14, "0");
            }
            const r = rpc("readPersistentArray", { filename: "nvram://receiverinfo/regioncode", structure: "U:2B" });
            const code = Number(r?.[0]);
            return code > 0 && code <= 0xfff ? code.toString(16).padStart(3, "0") : "";
        } catch {
            return "";
        }
    }

    if (config.profile === "bs4k") {
        const proto = Storage.prototype;
        const origGet = proto.getItem;
        const origSet = proto.setItem;
        const origRemove = proto.removeItem;
        const reserved = (key: string) => /^(greg|ureg)([0-9]|[1-5][0-9]|6[0-3])$/.test(key) || key.startsWith("_");
        const valid = (key: string) => /^(greg|ureg)([0-9]|[1-5][0-9]|6[0-3])$/.test(key) || /^_(common|wlocal|local)([0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5])$/.test(key) || key === "_prefecture" || key === "_regioncode" || key === "_zipcode";
        proto.getItem = function (this: Storage, key: string) {
            if (this === window.localStorage && reserved(String(key))) {
                if (!managed) {
                    throw new DOMException("SecurityError", "SecurityError");
                }
                if (!valid(String(key))) {
                    return null;
                }
                const stored = rpc("storageGet", { key: String(key) }) ?? null;
                if ((stored == null || stored === "") && (key === "_prefecture" || key === "_regioncode" || key === "_zipcode")) {
                    // 視聴者居住情報はデータ放送(BML)側の受信機設定(NVRAM receiverinfo)から作る
                    return receiverInfo(key);
                }
                return stored;
            }
            return origGet.call(this, key);
        };
        proto.setItem = function (this: Storage, key: string, value: string) {
            if (this === window.localStorage && reserved(String(key))) {
                if (!managed || !valid(String(key)) || key === "_prefecture" || key === "_regioncode") {
                    throw new DOMException("SecurityError", "SecurityError");
                }
                const r = rpc("storageSet", { key: String(key), value: String(value) });
                if (r === "quota") {
                    throw new DOMException("QuotaExceededError", "QuotaExceededError");
                }
                return;
            }
            return origSet.call(this, key, value);
        };
        proto.removeItem = function (this: Storage, key: string) {
            if (this === window.localStorage && reserved(String(key))) {
                if (!managed) {
                    throw new DOMException("SecurityError", "SecurityError");
                }
                rpc("storageRemove", { key: String(key) });
                return;
            }
            return origRemove.call(this, key);
        };
    }

    // ---------------------------------------------------------------
    // 放送映像オブジェクト (STD-B62 2.2 / STD-0013 6.1.4.1)
    // ---------------------------------------------------------------
    // ---------------------------------------------------------------
    // romsound:// (STD-0013 6.1.4.3.1 / TR-B39) 受信機内蔵音
    // WebView2はカスタムスキームを読めないのでdata URLに置き換える
    // ---------------------------------------------------------------
    function romsoundUrl(src: string | null): string | null {
        const m = /^romsound:\/\/(\d+)/i.exec(src ?? "");
        if (m == null) {
            return null;
        }
        const data = romsoundData[Number.parseInt(m[1], 10)];
        return data == null ? null : "data:audio/mpeg;base64," + data;
    }
    const srcDesc = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, "src");
    if (srcDesc?.set != null && srcDesc.get != null) {
        Object.defineProperty(HTMLMediaElement.prototype, "src", {
            configurable: true,
            enumerable: srcDesc.enumerable,
            get() {
                return (this as HTMLMediaElement).dataset.romsound ?? srcDesc.get!.call(this);
            },
            set(value: string) {
                const url = romsoundUrl(String(value));
                if (url != null) {
                    (this as HTMLMediaElement).dataset.romsound = String(value);
                    srcDesc.set!.call(this, url);
                } else {
                    delete (this as HTMLMediaElement).dataset.romsound;
                    srcDesc.set!.call(this, value);
                }
            },
        });
    }
    const origElementSetAttribute = Element.prototype.setAttribute;
    Element.prototype.setAttribute = function (this: Element, name: string, value: string) {
        if (name.toLowerCase() === "src" && (this instanceof HTMLMediaElement || this instanceof HTMLSourceElement)) {
            const url = romsoundUrl(String(value));
            if (url != null) {
                (this as HTMLElement).dataset.romsound = String(value);
                return origElementSetAttribute.call(this, name, url);
            }
        }
        return origElementSetAttribute.call(this, name, value);
    };
    function fixRomsound(root: ParentNode) {
        for (const e of root.querySelectorAll("audio[src], source[src]")) {
            const src = e.getAttribute("src");
            const url = romsoundUrl(src);
            if (url != null) {
                (e as HTMLElement).dataset.romsound = src!;
                origElementSetAttribute.call(e, "src", url);
                if (e instanceof HTMLSourceElement && e.parentElement instanceof HTMLMediaElement) {
                    e.parentElement.load();
                } else if (e instanceof HTMLMediaElement) {
                    e.load();
                }
            }
        }
    }

    const broadcastTypes = new Set(config.broadcastObjectTypes.map(x => x.toLowerCase()));
    type VideoState = { fullscreen: boolean, audioMute: boolean, videoSrc: string, audioSrc: string, captionSrc: string | null, captionVisible: boolean };
    const videoObjects = new Map<HTMLObjectElement, VideoState>();
    const videoMethods = new WeakMap<HTMLObjectElement, { [name: string]: (...args: any[]) => any }>();
    const currentServiceUrl = config.profile === "bs4k" ? "arib2://-1.-1.-1/-1" : "arib://-1.-1.-1/-1";
    function hex4(n: number) {
        return n.toString(16).padStart(4, "0");
    }
    function concreteServiceUrl() {
        const tsid = config!.profile === "bs4k" ? (config!.tlvStreamId ?? config!.transportStreamId) : config!.transportStreamId;
        return `${config!.profile === "bs4k" ? "arib2" : "arib"}://${hex4(config!.originalNetworkId)}.${hex4(tsid)}.${hex4(config!.serviceId)}`;
    }
    function isBroadcastObject(e: Element): e is HTMLObjectElement {
        return e instanceof HTMLObjectElement && broadcastTypes.has((e.getAttribute("type") ?? "").toLowerCase().trim());
    }
    function param(obj: HTMLObjectElement, name: string): string | null {
        for (const p of obj.querySelectorAll(":scope > param")) {
            if ((p.getAttribute("name") ?? "").toLowerCase() === name) {
                return p.getAttribute("value");
            }
        }
        return null;
    }
    function upgradeObject(obj: HTMLObjectElement) {
        if (videoObjects.has(obj)) {
            return;
        }
        const state: VideoState = {
            fullscreen: (param(obj, "fullscreen") ?? "disable").toLowerCase() === "enable",
            audioMute: (param(obj, "audio_mute") ?? "disable").toLowerCase() === "enable",
            videoSrc: param(obj, "video_src") ?? obj.getAttribute("data") ?? currentServiceUrl,
            audioSrc: param(obj, "audio_src") ?? obj.getAttribute("data") ?? currentServiceUrl,
            captionSrc: param(obj, "caption_src"),
            captionVisible: false,
        };
        videoObjects.set(obj, state);
        const ok = () => {
            if (!managed || (permission & PERM.AV) === 0) {
                raise("NOT_AUTHORIZED_ERR");
            }
        };
        const methods: { [name: string]: (...args: any[]) => any } = {
            enableFullscreen() { ok(); state.fullscreen = true; scheduleVideoUpdate(); return true; },
            disableFullscreen() { ok(); state.fullscreen = false; scheduleVideoUpdate(); return true; },
            isFullscreen() { return state.fullscreen; },
            enableAudioMute() { ok(); state.audioMute = true; scheduleVideoUpdate(); return true; },
            disableAudioMute() { ok(); state.audioMute = false; scheduleVideoUpdate(); return true; },
            isAudioMute() { return state.audioMute; },
            setVideoSrc(url: string) { ok(); state.videoSrc = String(url); scheduleVideoUpdate(); return true; },
            getVideoSrc() { return state.videoSrc.replace(/^(arib2?):\/\/-1\.-1\.-1/, concreteServiceUrl()); },
            setAudioSrc(url: string) { ok(); state.audioSrc = String(url); scheduleVideoUpdate(); return true; },
            getAudioSrc() { return state.audioSrc.replace(/^(arib2?):\/\/-1\.-1\.-1/, concreteServiceUrl()); },
            setCaptionSrc(url: string) { ok(); state.captionSrc = String(url); return true; },
            getCaptionComponentURL() { return state.captionSrc; },
            isCaptionExistent(_url: string) { return false; },
            setCaptionVisibility(flag: boolean) { ok(); state.captionVisible = !!flag; post({ type: "html5Caption", visible: state.captionVisible }); return true; },
            isCaptionVisible() { return state.captionVisible; },
            addCaptionListener() { },
            removeCaptionListener() { },
        };
        videoMethods.set(obj, methods);
        resizeObserver?.observe(obj);
        scheduleVideoUpdate();
    }

    // 静的に記述されたobjectはMutationObserverより先にスクリプトから参照されるためprototypeに定義する
    for (const name of ["enableFullscreen", "disableFullscreen", "isFullscreen", "enableAudioMute", "disableAudioMute", "isAudioMute",
        "setVideoSrc", "getVideoSrc", "setAudioSrc", "getAudioSrc", "setCaptionSrc", "getCaptionComponentURL", "isCaptionExistent",
        "setCaptionVisibility", "isCaptionVisible", "addCaptionListener", "removeCaptionListener"]) {
        Object.defineProperty(HTMLObjectElement.prototype, name, {
            configurable: true,
            writable: true,
            value: function (this: HTMLObjectElement, ...args: any[]) {
                if (!isBroadcastObject(this)) {
                    throw new TypeError(name + " is not a function");
                }
                upgradeObject(this);
                return videoMethods.get(this)![name](...args);
            },
        });
    }

    let videoUpdateScheduled = false;
    let lastVideoMessage = "";
    function scheduleVideoUpdate() {
        if (videoUpdateScheduled) {
            return;
        }
        videoUpdateScheduled = true;
        requestAnimationFrame(() => {
            videoUpdateScheduled = false;
            updateVideo();
        });
    }
    // 映像プレーンの切り抜き
    // 受信機では放送映像objectの領域には映像プレーンが見える (objectより下に描画される要素は映像に隠れる)
    // ここでは映像をWebViewの背後に表示しているため、objectより下の要素の該当領域を透過させる
    type SavedStyle = { el: HTMLElement, props: [name: string, original: string, priority: string, applied: string][] };
    let holeSaved: SavedStyle[] = [];
    let holeSignature = "";
    let applyingHole = false;
    function saveAndSet(saved: SavedStyle[], el: HTMLElement, props: { [name: string]: string }) {
        const entry: SavedStyle = { el, props: [] };
        for (const [name, value] of Object.entries(props)) {
            const original = el.style.getPropertyValue(name);
            const priority = el.style.getPropertyPriority(name);
            el.style.setProperty(name, value, "important");
            entry.props.push([name, original, priority, el.style.getPropertyValue(name)]);
        }
        saved.push(entry);
    }
    function restoreHole() {
        for (const { el, props } of holeSaved.reverse()) {
            for (const [name, value, priority, applied] of props) {
                // アプリが書き換えた場合はそのままにする
                if (el.style.getPropertyValue(name) !== applied) {
                    continue;
                }
                if (value === "") {
                    el.style.removeProperty(name);
                } else {
                    el.style.setProperty(name, value, priority);
                }
            }
        }
        holeSaved = [];
    }
    // アプリがstyleを書き換えて切り抜きが失われていないか
    function holeIntact(): boolean {
        for (const { el, props } of holeSaved) {
            if (!el.isConnected) {
                return false;
            }
            for (const [name, , , applied] of props) {
                if (el.style.getPropertyValue(name) !== applied || el.style.getPropertyPriority(name) !== "important") {
                    return false;
                }
            }
        }
        return true;
    }
    function isTransparent(color: string) {
        return color === "transparent" || /rgba\([^)]*,\s*0\)$/.test(color);
    }
    function cutVideoHole(obj: HTMLObjectElement | null, rect: DOMRect | null) {
        applyingHole = true;
        try {
            if (obj == null || rect == null || rect.width <= 0 || rect.height <= 0) {
                if (holeSaved.length) {
                    restoreHole();
                }
                holeSignature = "";
                return;
            }
            // 下に描画される要素: 祖先 + objectの領域でヒットする要素のうちobjectより後ろのもの
            restoreHole();
            const below = new Set<Element>();
            for (let e: Element | null = obj.parentElement; e != null; e = e.parentElement) {
                below.add(e);
            }
            // objectの領域を格子状に調べる (一部分だけobjectの下にある要素も対象にする)
            const points: [number, number][] = [];
            const nx = 16, ny = 9;
            for (let iy = 0; iy <= ny; iy++) {
                for (let ix = 0; ix <= nx; ix++) {
                    const x = Math.min(rect.right - 1, Math.max(rect.left + 1, rect.left + rect.width * ix / nx));
                    const y = Math.min(rect.bottom - 1, Math.max(rect.top + 1, rect.top + rect.height * iy / ny));
                    points.push([x, y]);
                }
            }
            for (const [x, y] of points) {
                const list = document.elementsFromPoint(x, y);
                const i = list.indexOf(obj);
                if (i < 0) {
                    continue;
                }
                for (const e of list.slice(i + 1)) {
                    below.add(e);
                }
            }
            const saved: SavedStyle[] = [];
            for (const e of below) {
                if (!(e instanceof HTMLElement)) {
                    continue;
                }
                const cs = getComputedStyle(e);
                const er = e.getBoundingClientRect();
                // 要素内での穴の位置
                const l = Math.max(0, rect.left - er.left), t = Math.max(0, rect.top - er.top);
                const r = Math.min(er.width, rect.right - er.left), b = Math.min(er.height, rect.bottom - er.top);
                if (r <= l || b <= t) {
                    continue;
                }
                const ancestor = e.contains(obj);
                if (ancestor || e === document.body || e === document.documentElement) {
                    // 子孫にobjectより上に描画される要素があり得るので背景のみ穴あきにする
                    const hasImage = cs.backgroundImage !== "none";
                    if (isTransparent(cs.backgroundColor) && !hasImage) {
                        continue;
                    }
                    if (hasImage) {
                        // 背景画像はマスクで切り抜けないので画像ごと消す (映像領域の背景画像は実機でも見えない)
                        log("hole: background-image on ancestor", e);
                    }
                    const c = isTransparent(cs.backgroundColor) ? "transparent" : cs.backgroundColor;
                    const g = `linear-gradient(${c},${c})`;
                    saveAndSet(saved, e, {
                        "background-color": "transparent",
                        "background-image": `${g},${g},${g},${g}`,
                        "background-size": `100% ${t}px,100% ${Math.max(0, er.height - b)}px,${l}px ${b - t}px,${Math.max(0, er.width - r)}px ${b - t}px`,
                        "background-position": `0 0,0 ${b}px,0 ${t}px,${r}px ${t}px`,
                        "background-repeat": "no-repeat",
                        "background-origin": "border-box",
                        "background-clip": "border-box",
                        "background-attachment": "scroll",
                    });
                } else {
                    if (cs.clipPath !== "none") {
                        continue;
                    }
                    saveAndSet(saved, e, {
                        "clip-path": `polygon(evenodd,0 0,100% 0,100% 100%,0 100%,0 0,${l}px ${t}px,${l}px ${b}px,${r}px ${b}px,${r}px ${t}px,${l}px ${t}px)`,
                    });
                }
            }
            holeSaved = saved;
        } finally {
            mutationObserver.takeRecords();
            applyingHole = false;
        }
    }

    function updateVideo() {
        for (const obj of [...videoObjects.keys()]) {
            if (!obj.isConnected) {
                videoObjects.delete(obj);
                resizeObserver?.unobserve(obj);
            }
        }
        // 1文書の映像参照objectは高々1つ (STD-0013 6.1.4.1.2.1)
        const entry = [...videoObjects.entries()][0];
        const dpr = window.devicePixelRatio;
        let msg: any;
        if (entry == null || !managed) {
            if (holeSignature !== "") {
                cutVideoHole(null, null);
            }
            msg = { type: "html5Video", present: false };
        } else {
            const [obj, state] = entry;
            const style = getComputedStyle(obj);
            const visible = style.display !== "none" && style.visibility !== "hidden";
            const r = obj.getBoundingClientRect();
            // 映像プレーンの切り抜き (形が変わったときのみ)
            const sig = visible ? `${r.left},${r.top},${r.width},${r.height},${document.body?.childElementCount}` : "";
            if (sig !== holeSignature || !holeIntact()) {
                cutVideoHole(visible ? obj : null, visible ? r : null);
                holeSignature = sig;
            }
            msg = {
                type: "html5Video",
                present: style.display !== "none",
                visible,
                fullscreen: state.fullscreen,
                audioMute: state.audioMute || style.display === "none",
                videoSrc: state.videoSrc,
                audioSrc: state.audioSrc,
                left: r.left * dpr,
                top: r.top * dpr,
                right: r.right * dpr,
                bottom: r.bottom * dpr,
                cssLeft: r.left,
                cssTop: r.top,
                cssRight: r.right,
                cssBottom: r.bottom,
            };
        }
        const s = JSON.stringify(msg);
        if (s !== lastVideoMessage) {
            lastVideoMessage = s;
            post(msg);
        }
    }
    const resizeObserver = typeof ResizeObserver !== "undefined" ? new ResizeObserver(() => scheduleVideoUpdate()) : null;
    function scan(root: ParentNode) {
        fixRomsound(root);
        if (root instanceof Element && isBroadcastObject(root)) {
            upgradeObject(root);
        }
        for (const e of root.querySelectorAll("object")) {
            if (isBroadcastObject(e)) {
                upgradeObject(e);
            }
        }
    }
    const mutationObserver = new MutationObserver((records) => {
        if (applyingHole) {
            return;
        }
        let changed = false;
        for (const rec of records) {
            if (rec.type === "childList") {
                for (const n of rec.addedNodes) {
                    if (n instanceof Element) {
                        scan(n);
                    }
                }
                if (rec.removedNodes.length) {
                    changed = true;
                }
            } else if (rec.type === "attributes") {
                changed = true;
                holeSignature = "";
                if (rec.target instanceof Element && rec.attributeName === "type") {
                    scan(rec.target);
                }
            }
        }
        if (changed) {
            scheduleVideoUpdate();
        }
    });
    // object要素は文書生成時点でのcreateElementでも生成される
    const origCreateElement = Document.prototype.createElement;
    Document.prototype.createElement = function (this: Document, tagName: string, options?: ElementCreationOptions) {
        const e = origCreateElement.call(this, tagName, options);
        if (e instanceof HTMLObjectElement) {
            const origSetAttribute = e.setAttribute;
            e.setAttribute = function (name: string, value: string) {
                origSetAttribute.call(this, name, value);
                if (name.toLowerCase() === "type" && isBroadcastObject(this)) {
                    upgradeObject(this as HTMLObjectElement);
                }
            };
        }
        return e;
    } as any;
    // 映像は動画プレーン (TVTestの映像ウィンドウ) に表示されるためobject自体は描画しない
    // (未対応プラグインのプレースホルダーを表示させない)
    const objectStyle = document.createElement("style");
    objectStyle.textContent = config.broadcastObjectTypes.map(t => `object[type="${t}" i]`).join(",") + "{opacity:0 !important;}";
    const injectObjectStyle = () => {
        if (!objectStyle.isConnected && document.documentElement != null) {
            (document.head ?? document.documentElement).appendChild(objectStyle);
        }
    };
    const startObserving = () => {
        injectObjectStyle();
        mutationObserver.observe(document.documentElement, { childList: true, subtree: true, attributes: true, attributeFilter: ["style", "class", "type", "width", "height", "hidden"] });
        scan(document);
        scheduleVideoUpdate();
    };
    if (document.documentElement != null) {
        startObserving();
    } else {
        document.addEventListener("readystatechange", startObserving, { once: true });
    }
    window.addEventListener("resize", scheduleVideoUpdate);
    window.addEventListener("scroll", scheduleVideoUpdate, true);
    document.addEventListener("transitionend", scheduleVideoUpdate, true);
    document.addEventListener("animationend", scheduleVideoUpdate, true);
    // CSSアニメーション中など
    setInterval(updateVideo, 500);

    // ---------------------------------------------------------------
    // viewport (STD-0013 6.1.4.1 / TR-B39 10.8.1.2)
    // ---------------------------------------------------------------
    function reportViewport() {
        let width = config!.defaultResolution.width;
        let height = config!.defaultResolution.height;
        const meta = document.querySelector("meta[name=viewport]");
        const content = meta?.getAttribute("content");
        if (content != null) {
            for (const part of content.split(/[,;]/)) {
                const [k, v] = part.split("=").map(x => x.trim().toLowerCase());
                const n = Number.parseInt(v ?? "", 10);
                if (k === "width" && !Number.isNaN(n)) {
                    width = n;
                } else if (k === "height" && !Number.isNaN(n)) {
                    height = n;
                }
            }
        }
        // 片方のみ指定された場合は16:9とする (例: "width=1920")
        if (content != null) {
            const hasW = /(^|[,;\s])width\s*=\s*\d/i.test(content);
            const hasH = /(^|[,;\s])height\s*=\s*\d/i.test(content);
            if (hasW && !hasH) {
                height = Math.round(width * 9 / 16);
            } else if (hasH && !hasW) {
                width = Math.round(height * 16 / 9);
            }
        }
        post({ type: "html5Viewport", width, height });
    }
    document.addEventListener("DOMContentLoaded", () => {
        injectObjectStyle();
        reportViewport();
        // 背景は透過 (映像を重ねるため)
        const root = document.documentElement;
        if (!getComputedStyle(root).backgroundColor || getComputedStyle(root).backgroundColor === "rgba(0, 0, 0, 0)") {
            root.style.backgroundColor = "transparent";
        }
        // 受信機ではスクロールバーは表示されない
        const style = document.createElement("style");
        style.textContent = "::-webkit-scrollbar{display:none}html,body{overflow:hidden}";
        (document.head ?? root).appendChild(style);
    });

    // ---------------------------------------------------------------
    // romsound:// (STD-0013 6.1.4.3.1) は native側で受信機内蔵音を返す
    // ---------------------------------------------------------------

    // ---------------------------------------------------------------
    // VOD (application/x-inettv-vod-plugin) は未対応
    // ---------------------------------------------------------------

    // ---------------------------------------------------------------
    // 公開
    // ---------------------------------------------------------------
    const define = (obj: any, name: string, value: any) => {
        try {
            Object.defineProperty(obj, name, { value, configurable: true, enumerable: true });
        } catch (e) {
            log("define failed", name, e);
        }
    };
    if (managed) {
        define(Navigator.prototype, "applicationManager", applicationManager);
        define(Navigator.prototype, "receiverDevice", receiverDevice);
        // 記述例のtypo
        define(Navigator.prototype, "receiverDeivce", receiverDevice);
        define(Navigator.prototype, "receiverDeivice", receiverDevice);
        define(Navigator.prototype, "capabilities", capabilities);
        // BS4Kでも通信で配信されるHybridcast系アプリが参照することがある
        define(Navigator.prototype, "bmlCompat", bmlCompat);
    }
    window.addEventListener("unload", () => {
        post({ type: "html5Unload" });
    });
    notifyKeySet();
    window.__aribHtml5 = { config, rpc, receiverDevice, applicationManager, keySet, key: (name: string) => handleKey({ name, action: "press" }) };
    log("runtime installed", location.href);
})();

export { };
