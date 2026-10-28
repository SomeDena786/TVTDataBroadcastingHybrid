// STD-B24 第二編 7.6.20 AITコントロールドアプリケーション連携機能
// IPTVFJ STD-0013 6.1.5 BMLコンテンツからのHTMLアプリケーション起動
import type { AITControlledApp } from "web-bml";
import type { AITApplication, AITSection } from "web-bml/protocol";

export type Html5Application = {
    organizationId: number,
    applicationId: number,
    controlCode: number,
    applicationType: number,
    // "ISDB-HTML" / "ARIB-HTML5" など (XML-AITのみ)
    typeName?: string,
    priority: number,
    url: string | null,
    boundaries: { permissionBitmaps: number[], managedURLs: string[] }[],
};

export type Html5AIT = {
    // 取得元 (放送で伝送されるAITの場合null)
    source: string | null,
    applications: Html5Application[],
};

const controlCodes: Record<string, number> = {
    "AUTOSTART": 0x01,
    "PRESENT": 0x02,
    "DESTROY": 0x03,
    "KILL": 0x04,
    "PREFETCH": 0x05,
    "REMOTE": 0x06,
    "DISABLED": 0x07,
    "PLAYBACK_AUTOSTART": 0x08,
};

function children(e: Element, localName: string): Element[] {
    return [...e.children].filter(x => x.localName === localName);
}

function child(e: Element, localName: string): Element | undefined {
    return children(e, localName)[0];
}

function text(e: Element | undefined): string | undefined {
    return e?.textContent?.trim() ?? undefined;
}

function joinUrl(base: string | undefined, location: string | undefined): string | null {
    if (location == null) {
        return base ?? null;
    }
    if (/^https?:\/\//i.test(location)) {
        return location;
    }
    if (base == null) {
        return null;
    }
    try {
        return new URL(location, base).toString();
    } catch {
        return base + location;
    }
}

/**
 * XML形式AIT (STD-B24 第四編 第6章, STD-B62 5.3, IPTVFJ STD-0013 5.1.3)
 */
export function parseXMLAIT(xml: string, source: string): Html5AIT | null {
    const doc = new DOMParser().parseFromString(xml, "application/xml");
    if (doc.getElementsByTagName("parsererror").length > 0) {
        return null;
    }
    const applications: Html5Application[] = [];
    for (const app of doc.getElementsByTagNameNS("*", "Application")) {
        const identifier = child(app, "applicationIdentifier");
        const descriptor = child(app, "applicationDescriptor");
        if (identifier == null || descriptor == null) {
            continue;
        }
        const orgId = Number.parseInt(text(child(identifier, "orgId")) ?? "", 10);
        const appId = Number.parseInt(text(child(identifier, "appId")) ?? "", 10);
        const typeElement = child(descriptor, "type");
        const typeName = typeElement == null ? undefined : text(typeElement.firstElementChild ?? typeElement);
        const controlCode = controlCodes[(text(child(descriptor, "controlCode")) ?? "").toUpperCase()] ?? 0;
        const priority = Number.parseInt(text(child(descriptor, "priority")) ?? "0", 16);
        let urlBase: string | undefined;
        for (const transport of children(app, "applicationTransport")) {
            urlBase ??= text(child(transport, "URLBase"));
        }
        const location = text(child(app, "applicationLocation"));
        const boundaries: Html5Application["boundaries"] = [];
        for (const bp of app.getElementsByTagNameNS("*", "boundaryAndPermission")) {
            boundaries.push({
                permissionBitmaps: children(bp, "permissionBitmap").map(x => Number.parseInt(text(x) ?? "0", 16)),
                managedURLs: children(bp, "managedURL").map(x => text(x) ?? "").filter(x => x !== ""),
            });
        }
        applications.push({
            organizationId: orgId,
            applicationId: appId,
            controlCode,
            // ISDB-HTML: 放送通信連携HTML5 (0x0010), ARIB-HTML5 (0x0011)
            applicationType: typeName === "ARIB-HTML5" ? 0x0011 : 0x0010,
            typeName,
            priority,
            url: joinUrl(urlBase, location),
            boundaries,
        });
    }
    return { source, applications };
}

/**
 * セクション形式AIT
 */
export function convertAITSection(section: AITSection): Html5AIT {
    return {
        source: null,
        applications: section.applications.map((app: AITApplication) => {
            const transports = [...app.transports, ...section.commonTransports];
            const transport = transports.find(t => app.transportProtocolLabels.length === 0 || app.transportProtocolLabels.includes(t.transportProtocolLabel)) ?? transports[0];
            let url: string | null = null;
            const u = transport?.urls?.[0];
            if (u != null) {
                url = joinUrl(u.urlBase, app.location ?? u.urlExtensions[0]);
            }
            return {
                organizationId: app.organizationId,
                applicationId: app.applicationId,
                controlCode: app.controlCode,
                applicationType: section.applicationType,
                priority: app.priority,
                url,
                boundaries: app.boundaries,
            };
        }),
    };
}

type AITEntry = {
    state: "receiving" | "received" | "error",
    ait?: Html5AIT,
};

export type AITControlledAppHostOptions = {
    // ait_uriのXML-AITを取得する
    fetchXMLAIT(uri: string): Promise<string | null>;
    // HTML5アプリケーションを起動する
    launch(app: Html5Application, ait: Html5AIT): boolean;
    // 対応するapplication_type
    supportedApplicationTypes: Set<number>;
    log?(...args: any[]): void;
};

/**
 * BML文書スコープでのAIT取得状況を管理する
 */
export class AITControlledAppHost implements AITControlledApp {
    // 文書スコープで保持 (IPTVFJ STD-0013 6.1.5: 最大10)
    private readonly entries = new Map<string, AITEntry>();
    private activeDocument: string | null = null;
    private broadcastAIT: Html5AIT | null = null;
    private readonly options: AITControlledAppHostOptions;
    private static readonly MAX_ENTRIES = 10;

    constructor(options: AITControlledAppHostOptions) {
        this.options = options;
    }

    public setBroadcastAIT(section: AITSection) {
        if (!this.options.supportedApplicationTypes.has(section.applicationType)) {
            return;
        }
        this.broadcastAIT = convertAITSection(section);
    }

    public clearBroadcastAIT() {
        this.broadcastAIT = null;
    }

    public getBroadcastAIT(): Html5AIT | null {
        return this.broadcastAIT;
    }

    private checkDocument(activeDocument: string | null) {
        if (this.activeDocument !== activeDocument) {
            // 別の文書に遷移した後は改めてstartReceivingAIT()の呼び出しを行う必要がある
            this.entries.clear();
            this.activeDocument = activeDocument;
        }
    }

    private static toHex(n: number): string {
        return n.toString(16);
    }

    private toArray(ait: Html5AIT): any[] {
        const apps = ait.applications.filter(x => this.options.supportedApplicationTypes.has(x.applicationType));
        return [
            [apps[0]?.applicationType ?? 0x0010],
            ...apps.map(x => [AITControlledAppHost.toHex(x.organizationId), AITControlledAppHost.toHex(x.applicationId), x.controlCode]),
        ];
    }

    private fetch(uri: string): Promise<Html5AIT | null> {
        return this.options.fetchXMLAIT(uri).then(xml => xml == null ? null : parseXMLAIT(xml, uri)).catch(() => null);
    }

    public startReceivingAIT(aitUri: string | undefined, activeDocument: string | null): number {
        this.checkDocument(activeDocument);
        if (aitUri == null) {
            // 放送で伝送されるAITは常に受信している
            return this.broadcastAIT != null ? 1 : NaN;
        }
        if (!/^https?:\/\//i.test(aitUri)) {
            return NaN;
        }
        if (!this.entries.has(aitUri) && this.entries.size >= AITControlledAppHost.MAX_ENTRIES) {
            return NaN;
        }
        const entry: AITEntry = { state: "receiving" };
        this.entries.set(aitUri, entry);
        this.fetch(aitUri).then(ait => {
            if (this.entries.get(aitUri) !== entry) {
                return;
            }
            if (ait == null) {
                entry.state = "error";
            } else {
                entry.state = "received";
                entry.ait = ait;
            }
            this.options.log?.("AIT received", aitUri, ait);
        });
        return 1;
    }

    public getReceivedAIT(aitUri: string | undefined, activeDocument: string | null): any[] | number {
        this.checkDocument(activeDocument);
        if (aitUri == null) {
            return this.broadcastAIT == null ? 0 : this.toArray(this.broadcastAIT);
        }
        const entry = this.entries.get(aitUri);
        if (entry == null) {
            return -2;
        }
        if (entry.state === "receiving") {
            return 0;
        }
        if (entry.state === "error" || entry.ait == null) {
            return -1;
        }
        return this.toArray(entry.ait);
    }

    public startAITControlledApp(organizationId: string, applicationId: string, aitUri: string | undefined, activeDocument: string | null): number {
        this.checkDocument(activeDocument);
        const orgId = Number.parseInt(organizationId, 16);
        const appId = Number.parseInt(applicationId, 16);
        if (Number.isNaN(orgId) || Number.isNaN(appId)) {
            return NaN;
        }
        const launch = (ait: Html5AIT | null): boolean => {
            if (ait == null) {
                return false;
            }
            // isdb:orgID 0は全てのorgIDに合致する
            const app = ait.applications.find(x =>
                (orgId === 0 || x.organizationId === orgId) &&
                x.applicationId === appId &&
                this.options.supportedApplicationTypes.has(x.applicationType));
            if (app == null || app.url == null) {
                this.options.log?.("startAITControlledApp: application not found", organizationId, applicationId, ait);
                return false;
            }
            // AUTOSTART, PRESENTのみ起動を許可
            if (app.controlCode !== 0x01 && app.controlCode !== 0x02) {
                this.options.log?.("startAITControlledApp: not allowed by control code", app.controlCode);
                return false;
            }
            return this.options.launch(app, ait);
        };
        if (aitUri == null) {
            return launch(this.broadcastAIT) ? 1 : NaN;
        }
        const entry = this.entries.get(aitUri);
        if (entry?.state === "received" && entry.ait != null) {
            // 最新の状態に基づいて処理を行う
            this.fetch(aitUri).then(ait => launch(ait ?? entry.ait!));
            return 1;
        }
        // 取得してから起動 (非同期で起動するのでスクリプトは継続する)
        this.fetch(aitUri).then(ait => launch(ait));
        return 1;
    }
}
