# TVTDataBroadcastingHybrid

[otya128/TVTDataBroadcastingWV2](https://github.com/otya128/TVTDataBroadcastingWV2) を基に、**ハイブリッドキャスト（放送通信連携HTML5アプリ）** と **BS4K（新4K）のデータ放送** の表示に対応させた TVTest 用データ放送プラグインのフォークです。

従来の BML データ放送（地上デジタル／BS／CS）に加えて、以下に対応します。

- **ハイブリッドキャスト（ARIBフェーズ0運用）**
  - BML から `startReceivingAIT` / `getReceivedAIT` / `startAITControlledApp` でXML-AITを取得し、HTML5アプリケーションを起動
  - TSで伝送されるAIT（`table_id` 0x74）による起動
  - 放送映像オブジェクト（`<object>`）の映像プレーン合成、リモコンキー、EIT・イベントメッセージ、NVRAM/Greg の共有など
- **BS4K（高度BSデジタル）データ放送（ARIB-HTML5 / MMT）**
  - MH-AIT とデータ伝送（DDMT/DAMT）で配信される HTML5 アプリケーションの表示
  - 受信には BS4K の MMT/TLV を扱える BonDriver が必要です（[BS4Kについて](#bs4k)参照）

これらの HTML5 アプリケーションは、BML とは別の 2 枚目の WebView2 を映像の上に重ねて提示します。BML の表示処理には引き続き [web-bml](https://github.com/otya128/web-bml) を使用します。

## 謝辞・基にしたプロジェクト

- [otya128/TVTDataBroadcastingWV2](https://github.com/otya128/TVTDataBroadcastingWV2) — 本プラグインのベース
- [otya128/web-bml](https://github.com/otya128/web-bml) — BML ブラウザ（本リポジトリではフォークをサブモジュールとして使用）
- [nekohkr/dantto4k](https://github.com/nekohkr/dantto4k) — BS4K データ放送の取り出しに使用（フォークに `DB4K_*` エクスポートを追加）

## 動作環境

* TVTest 0.9.0 正式版以上
* Windows 10 以上
* WebView2 ランタイム（最低 89.0.774.44 以上）
    * 未インストールの場合は [WebView2 配布ページ](https://developer.microsoft.com/microsoft-edge/webview2/) からインストール
* Visual C++ 2015-2022 ランタイム

映像レンダラは EVR / madVR / MPC Video Renderer などで動作します。

## ビルド

### 1. 取得

web-bml はサブモジュールです。`--recursive` を付けてクローンしてください。

```sh
git clone --recursive https://github.com/SomeDena786/TVTDataBroadcastingHybrid.git
```

### 2. web-bml のビルド

```sh
cd web-bml
npm ci
npx tsc
cd ..
```

### 3. フロントエンド（BML/HTML5ランタイム）のビルド

```sh
cd browser
npm ci
npm run build
cd ..
```

`browser/dist/` に `TVTDataBroadcastingHybrid.js`（BML ブラウザ）と `aribhtml5_runtime.js`（HTML5アプリに注入する受信機エミュレーションランタイム）が出力されます。

### 4. プラグイン（ネイティブ）のビルド

`TVTDataBroadcastingHybrid.sln` を Visual Studio 2022 で開き、NuGet パッケージ（WebView2 SDK など）を復元して Release / x64 でビルドします。

### 5. 配置

TVTest の `Plugins` フォルダに以下を配置します。

```
Plugins/
  TVTDataBroadcastingHybrid.tvtp
  TVTDataBroadcastingHybrid.ini            （任意。設定）
  TVTDataBroadcastingHybrid/
    resources/
      TVTDataBroadcastingHybrid.html
      dist/
        TVTDataBroadcastingHybrid.js
        aribhtml5_runtime.js
        （web-bml 用フォント .woff2）
```

フォント（丸ゴシック等）が必要な場合は、BML の表示用に woff2 を `resources/dist/` に置いてください。

## 使い方

TVTest 起動時には無効になっているため、右クリックメニューやサイドバーからプラグインを有効にしてください。有効にしたタイミングで WebView2 が起動します。

キー割り当て、パネル、サイドバー、ステータスバーの設定は TVTest の設定で行えます。ステータスバーの項目には、読み込み中の BML／HTML5 文書が表示されます。

### 設定（`Plugins/TVTDataBroadcastingHybrid.ini`）

```ini
[TVTDataBroadcastingHybrid]
; 通信（ハイブリッドキャスト含む）を有効化
EnableNetwork=1
; ハイブリッドキャスト（放送通信連携HTML5）を有効化
EnableHybridcast=1
```

主な設定キー:

| キー | 説明 |
|---|---|
| `EnableNetwork` | 通信コンテンツを有効化（ハイブリッドキャストに必須） |
| `EnableHybridcast` | ハイブリッドキャスト／ARIB-HTML5 の起動を有効化 |
| `AutoStartBroadcastAIT` | TSで伝送されるAIT(0x74)のAUTOSTARTを自動起動 |
| `Html5UserAgent` / `Html5UserAgentBS4K` | HTML5アプリに送るUser-Agent |
| `CardID` | getIRDID で返すB-CASカードID（16進12桁）。省略時は挿入中のカードから読み取り |
| `Html5KeyCodes` / `Html5KeySetValues` | リモコンキーコードの上書き |
| `BroadcastObjectTypes` | 放送映像objectとして扱うMIMEタイプ |
| `Html5Debug` | 1でHTML5エンジンのDevToolsとログを有効化 |

### ハイブリッドキャストについて

ハイブリッドキャストのアプリケーションは、放送局のサーバーと通信して提示されます。アプリケーションによっては、実機の受信機と同様に **B-CAS カードのカードID** を参照し、放送局のサーバーで契約状態などを確認します。本プラグインは、挿入されている B-CAS カードのカードIDを標準のスマートカード API で読み取り、受信機として `getIRDID(5)` / `getDeviceIdentifier(5)` で返します（実機と同じ動作です）。契約の確認は放送局側の仕組みに従います。

### BS4K

BS4K（新4K）のデータ放送を表示するには、MMT/TLV を扱える BonDriver が必要です。本プラグインは [nekohkr/dantto4k](https://github.com/nekohkr/dantto4k) のフォーク（`DB4K_*` エクスポートを追加したもの）から、`GetModuleHandle` 経由でデータ放送のファイルと MH-AIT を取得します。該当の BonDriver を TVTest が読み込んだ状態で BS4K を選局してください。

## ライセンス

MIT License（[LICENSE](LICENSE)）。Copyright (c) 2022 otya ほかコントリビュータ。
