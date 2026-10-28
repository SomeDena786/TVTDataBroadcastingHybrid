// ARIB-HTML5 (Hybridcast / BS4K) アプリケーションエンジン
// TVTDataBroadcastingHybrid.cppからincludeされる
//
// BML (WebView#1) からstartAITControlledApp等でlaunchHtml5が送られると
// 2つ目のWebView2 (WebView#2) を映像の上に重ねてHTML5アプリケーションを提示する。
// 放送関連API (navigator.receiverDevice等) はresources/dist/aribhtml5_runtime.jsが文書生成時に注入され提供する。

#include <fstream>
#include <map>

static const wchar_t* HTML5_RPC_URL = L"https://tvtdatabroadcastinghybrid-api.invalid/html5rpc";

static std::map<std::string, int> ParseNameValueList(const std::wstring& s)
{
    // "VK_RED:403,VK_GREEN:404"
    std::map<std::string, int> result;
    std::wstringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, L','))
    {
        auto pos = item.find(L':');
        if (pos == std::wstring::npos)
        {
            pos = item.find(L'=');
        }
        if (pos == std::wstring::npos)
        {
            continue;
        }
        auto name = item.substr(0, pos);
        auto value = item.substr(pos + 1);
        name.erase(0, name.find_first_not_of(L" \t"));
        name.erase(name.find_last_not_of(L" \t") + 1);
        try
        {
            result[wstrToUTF8String(name.c_str())] = std::stoi(value, nullptr, 0);
        }
        catch (...)
        {
        }
    }
    return result;
}

void CDataBroadcastingWV2::InitHtml5()
{
    this->html5Enabled = this->GetIniItem(L"EnableHybridcast", 1) != 0;
}

void CDataBroadcastingWV2::SendHtml5Config()
{
    if (!this->webView)
    {
        return;
    }
    // getResidentAppVersion("AITControlledApp")の戻り値 (IPTVFJが割り当てる値なので設定で与える)
    // ResidentAppVersion=AITControlledApp:IPTVF,HTML5,1.0,0
    nlohmann::json residentAppVersion = nlohmann::json::object();
    auto rav = this->GetIniItem(L"ResidentAppVersion", L"AITControlledApp:IPTV-F,HTML5_ph0,1,0");
    {
        std::wstringstream ss(rav);
        std::wstring item;
        while (std::getline(ss, item, L';'))
        {
            auto pos = item.find(L':');
            if (pos == std::wstring::npos)
            {
                continue;
            }
            auto name = wstrToUTF8String(item.substr(0, pos).c_str());
            nlohmann::json values = nlohmann::json::array();
            std::wstringstream vs(item.substr(pos + 1));
            std::wstring v;
            while (std::getline(vs, v, L','))
            {
                values.push_back(wstrToUTF8String(v.c_str()));
            }
            residentAppVersion[name] = values;
        }
    }
    nlohmann::json msg{
        { "type", "html5Config" },
        { "enabled", this->html5Enabled && this->proxySession != nullptr },
        { "autoStartBroadcastAIT", this->GetIniItem(L"AutoStartBroadcastAIT", 1) != 0 },
        { "residentAppVersion", residentAppVersion },
    };
    this->webView->PostWebMessageAsJson(utf8StrToWString(msg.dump().c_str()).c_str());
}

void CDataBroadcastingWV2::TuneToService(int originalNetworkId, int transportStreamId, int serviceId)
{
    TVTest::ChannelSelectInfo info = {};
    info.Size = sizeof(info);
    info.Flags = TVTest::CHANNEL_SELECT_FLAG_STRICTSERVICE | TVTest::CHANNEL_SELECT_FLAG_ALLOWDISABLED;
    // FIXME: original_network_idでない
    info.NetworkID = originalNetworkId;
    info.TransportStreamID = transportStreamId;
    info.ServiceID = serviceId;
    info.Channel = -1;
    info.Space = -1;
    if (!this->m_pApp->SelectChannel(&info))
    {
        int numSpace = 0;
        this->m_pApp->GetTuningSpace(&numSpace);
        bool success = false;
        for (int space = 0; space < numSpace && !success; space++)
        {
            for (int channel = 0; ; channel++)
            {
                TVTest::ChannelInfo channelInfo = {};
                if (!this->m_pApp->GetChannelInfo(space, channel, &channelInfo))
                {
                    break;
                }
                // FIXME: original_network_idでない
                if (channelInfo.NetworkID == originalNetworkId && channelInfo.TransportStreamID == transportStreamId)
                {
                    success = this->m_pApp->SetChannel(space, channel, serviceId);
                    break;
                }
            }
        }
        if (!success)
        {
            auto msg = (L"データ放送からの選局に失敗しました。映像/音声のないサービスまたはチャンネルスキャンされていない可能性があります。(original_network_id=" + std::to_wstring(originalNetworkId) + L",transport_stream_id=" + std::to_wstring(transportStreamId) + L",service_id=" + std::to_wstring(serviceId) + L")");
            MessageBoxW(this->m_pApp->GetFullscreen() ? this->GetFullscreenWindow() : this->m_pApp->GetAppWindow(), msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
            this->m_pApp->AddLog(msg.c_str(), TVTest::LOG_TYPE_ERROR);
            if (this->html5Active)
            {
                this->StopHtml5(true);
            }
            else if (this->webView)
            {
                this->webView->Reload();
            }
        }
    }
}

std::string CDataBroadcastingWV2::BuildHtml5ConfigScript()
{
    auto&& launch = this->html5Launch;
    auto profile = launch.value("profile", std::string("hybridcast"));
    bool bs4k = profile == "bs4k";

    std::map<std::string, int> keyCodes{
        { "VK_LEFT", 37 }, { "VK_UP", 38 }, { "VK_RIGHT", 39 }, { "VK_DOWN", 40 },
        { "VK_ENTER", 13 }, { "VK_BACK", 461 },
        { "VK_RED", 403 }, { "VK_GREEN", 404 }, { "VK_YELLOW", 405 }, { "VK_BLUE", 406 },
        { "VK_0", 48 }, { "VK_1", 49 }, { "VK_2", 50 }, { "VK_3", 51 }, { "VK_4", 52 },
        { "VK_5", 53 }, { "VK_6", 54 }, { "VK_7", 55 }, { "VK_8", 56 }, { "VK_9", 57 },
        { "VK_10", 522 }, { "VK_11", 523 }, { "VK_12", 524 },
        { "VK_DBUTTON", 528 },
        { "VK_PLAY", 415 }, { "VK_PAUSE", 19 }, { "VK_PLAY_PAUSE", 402 }, { "VK_STOP", 413 },
        { "VK_FAST_FWD", 417 }, { "VK_REWIND", 412 }, { "VK_TRACK_NEXT", 425 }, { "VK_TRACK_PREV", 424 },
        { "VK_VCR_OTHER", 529 }, { "VK_PAGE_UP", 33 }, { "VK_PAGE_DOWN", 34 }, { "VK_TAB", 9 },
    };
    for (auto&& [k, v] : ParseNameValueList(this->GetIniItem(L"Html5KeyCodes", L"")))
    {
        keyCodes[k] = v;
    }
    std::map<std::string, int> keySetValues{
        { "RED", 0x1 }, { "GREEN", 0x2 }, { "YELLOW", 0x4 }, { "BLUE", 0x8 },
        { "NAVIGATION", 0x10 }, { "VCR", 0x20 }, { "NUMERIC", 0x100 }, { "DBUTTON", 0x800 },
    };
    for (auto&& [k, v] : ParseNameValueList(this->GetIniItem(L"Html5KeySetValues", L"")))
    {
        keySetValues[k] = v;
    }
    nlohmann::json broadcastObjectTypes = nlohmann::json::array();
    {
        auto types = this->GetIniItem(L"BroadcastObjectTypes", L"video/x-arib2-broadcast,video/x-iptvf-broadcast,video/broadcast,video/x-arib-broadcast");
        std::wstringstream ss(types);
        std::wstring t;
        while (std::getline(ss, t, L','))
        {
            if (!t.empty())
            {
                broadcastObjectTypes.push_back(wstrToUTF8String(t.c_str()));
            }
        }
    }
    nlohmann::json deviceIdentifiers = nlohmann::json::object();
    for (auto&& [k, v] : ParseNameValueList(this->GetIniItem(L"DeviceIdentifier", L"")))
    {
        deviceIdentifiers[k] = std::to_string(v);
    }
    // getDeviceIdentifier(5) = B-CASカードID (CA_system_id 0x0005)
    // 挿入中の実カードから読み取った値を返す (STD-B62 3.3.11 / STD-0013 6.1.4.6)
    // 読み取りは別スレッドで行う。ここではブロックせずキャッシュ済みの値を使う
    {
        this->StartReadCardIdAsync();
        std::string id;
        {
            std::lock_guard<std::mutex> lock(this->cardIdMutex);
            id = this->cardId;
        }
        if (!id.empty() && !deviceIdentifiers.contains("5"))
        {
            deviceIdentifiers["5"] = id;
        }
    }
    nlohmann::json config{
        { "profile", profile },
        { "app", {
            { "organizationId", launch.value("organizationId", 0) },
            { "applicationId", launch.value("applicationId", 0) },
            { "controlCode", launch.value("controlCode", 1) },
            { "applicationType", launch.value("applicationType", 0x10) },
            { "priority", launch.value("priority", 0) },
            { "url", launch.value("url", std::string()) },
            { "boundaries", this->html5Unmanaged ? nlohmann::json::array({ { { "permissionBitmaps", nlohmann::json::array() }, { "managedURLs", nlohmann::json::array({ "invalid.invalid/" }) } } }) : launch.value("boundaries", nlohmann::json::array()) },
        } },
        { "applications", launch.value("applications", nlohmann::json::array()) },
        { "aitSource", launch.value("aitSource", nlohmann::json()) },
        { "originalNetworkId", launch.value("originalNetworkId", (int)this->currentChannel.NetworkID) },
        { "transportStreamId", launch.value("transportStreamId", (int)this->currentChannel.TransportStreamID) },
        { "serviceId", launch.value("serviceId", (int)this->currentService.ServiceID) },
        { "broadcastObjectTypes", broadcastObjectTypes },
        { "keyCodes", keyCodes },
        { "keySetValues", keySetValues },
        { "systemInformation", {
            { "makerid", wstrToUTF8String(this->GetIniItem(L"Html5MakerId", L"00").c_str()) },
            { "browsername", "TVTDataBroadcastingHybrid" },
            { "browserversion", "1.0" },
            { "modelname", wstrToUTF8String(this->GetIniItem(L"Html5ModelName", L"TVTest").c_str()) },
            { "baseurl", bs4k ? "https://arib4k.invalid/" : "" },
        } },
        { "deviceIdentifiers", deviceIdentifiers },
        { "defaultResolution", bs4k ? nlohmann::json{ { "width", 3840 }, { "height", 2160 } } : nlohmann::json{ { "width", 1920 }, { "height", 1080 } } },
        { "rpcUrl", wstrToUTF8String(HTML5_RPC_URL) },
        { "debug", this->GetIniItem(L"Html5Debug", 0) != 0 },
    };
    if (this->html5Unmanaged)
    {
        // exitFromManagedState後は放送マネージド状態ではない
        config["app"]["boundaries"] = nlohmann::json::array({ { { "permissionBitmaps", nlohmann::json::array({ 0 }) }, { "managedURLs", nlohmann::json::array({ "unmanaged.invalid" }) } } });
    }
    // </script>などが混ざらないようにJSONとして埋め込む
    return "window.__aribHtml5Config = " + config.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ";\n";
}

void CDataBroadcastingWV2::LaunchHtml5(const nlohmann::json& launch)
{
    if (!this->html5Enabled || !this->webViewEnvironment)
    {
        return;
    }
    auto url = launch.value("url", std::string());
    if (!(url.starts_with("http://") || url.starts_with("https://")))
    {
        this->m_pApp->AddLog((L"HTML5アプリケーションのURLが不正です: " + utf8StrToWString(url.c_str())).c_str(), TVTest::LOG_TYPE_WARNING);
        return;
    }
    this->m_pApp->AddLog((L"HTML5アプリケーションを起動します: " + utf8StrToWString(url.c_str())).c_str(), TVTest::LOG_TYPE_INFORMATION);
    this->html5Launch = launch;
    this->html5Unmanaged = false;
    // 毎回読み込む (開発中に差し替えられるように)
    this->html5RuntimeScript.clear();
    {
        auto resourceDirectory = this->GetIniItem(L"ResourceDirectory", this->resourceDirectory.c_str());
        std::ifstream ifs(std::filesystem::path(resourceDirectory) / L"dist" / L"aribhtml5_runtime.js", std::ios::binary);
        if (ifs)
        {
            this->html5RuntimeScript.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        }
        if (this->html5RuntimeScript.empty())
        {
            this->m_pApp->AddLog(L"aribhtml5_runtime.jsが見つかりません", TVTest::LOG_TYPE_ERROR);
            return;
        }
    }
    if (!this->html5Controller)
    {
        this->CreateHtml5WebView();
        return;
    }
    this->NavigateHtml5();
}

void CDataBroadcastingWV2::CreateHtml5WebView()
{
    if (this->html5Creating)
    {
        return;
    }
    this->html5Creating = true;
    this->webViewEnvironment->CreateCoreWebView2Controller(this->hMessageWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
        this->html5Creating = false;
        if (FAILED(result) || controller == nullptr || !this->webView)
        {
            this->m_pApp->AddLog(L"HTML5エンジン用WebView2を初期化できませんでした", TVTest::LOG_TYPE_ERROR);
            return S_OK;
        }
        this->html5Controller = controller;
        this->html5Controller->get_CoreWebView2(this->html5WebView.put());
        // 表示する前に透過の設定を行う (表示後にWS_EX_LAYEREDを付けると背景が不透明になる)
        this->html5Controller->put_IsVisible(FALSE);
        if (auto controller2 = this->html5Controller.try_query<ICoreWebView2Controller2>())
        {
            COREWEBVIEW2_COLOR c = {};
            controller2->put_DefaultBackgroundColor(c);
        }
        wil::com_ptr<ICoreWebView2Settings> settings;
        this->html5WebView->get_Settings(settings.put());
        settings->put_IsScriptEnabled(TRUE);
        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
        settings->put_IsWebMessageEnabled(TRUE);
        settings->put_AreDefaultContextMenusEnabled(this->GetIniItem(L"Html5Debug", 0) != 0);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        if (auto settings2 = settings.try_query<ICoreWebView2Settings2>())
        {
            wil::unique_cotaskmem_string ua;
            settings2->get_UserAgent(ua.put());
            auto profile = this->html5Launch.value("profile", std::string("hybridcast"));
            std::wstring custom = profile == "bs4k"
                ? this->GetIniItem(L"Html5UserAgentBS4K", L"")
                : this->GetIniItem(L"Html5UserAgent", L"");
            if (custom.empty())
            {
                // 既定のUAに受信機情報を付加する
                custom = std::wstring(ua.get()) + (profile == "bs4k"
                    ? L" A-PAB/1.0 (4K browser,2160p;000000;TVTest;1;0;)"
                    : L" Hybridcast/1.0 (;TVTest;TVTDataBroadcastingHybrid;1.0;)");
            }
            settings2->put_UserAgent(custom.c_str());
        }
        HWND parent = this->hContainerWnd;
        this->webViewController->get_ParentWindow(&parent);
        this->html5Controller->put_ParentWindow(parent);
        // 親に付け替えた後でWebView#1以外のWebView2ウィンドウを探す
        {
            HWND hWnd = nullptr;
            while ((hWnd = FindWindowExW(parent, hWnd, L"Chrome_WidgetWin_0", nullptr)) != nullptr)
            {
                if (hWnd != this->hWebViewWnd)
                {
                    this->hHtml5WebViewWnd = hWnd;
                    break;
                }
            }
        }
        // 最初の描画より前に透過の設定を行う
        if (this->hHtml5WebViewWnd)
        {
            SetWindowLongW(this->hHtml5WebViewWnd, GWL_EXSTYLE, GetWindowLongW(this->hHtml5WebViewWnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
            this->html5Layered = true;
        }
        else
        {
            this->m_pApp->AddLog(L"HTML5エンジンのウィンドウが見つかりません", TVTest::LOG_TYPE_WARNING);
        }
        this->UpdateHtml5Bounds();

        EventRegistrationToken token;
        this->html5WebView->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        this->html5WebView->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            return this->OnHtml5WebResourceRequested(args);
        }).Get(), &token);
        this->html5WebView->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2* webview, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string message;
            if (SUCCEEDED(args->get_WebMessageAsJson(message.put())))
            {
                try
                {
                    auto json = nlohmann::json::parse(wstrToUTF8String(message.get()));
                    this->OnHtml5WebMessage(json);
                }
                catch (...)
                {
                }
            }
            return S_OK;
        }).Get(), &token);
        // ステータスバーに読み込み中のHTML5文書を表示する
        this->html5WebView->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string uri;
            if (SUCCEEDED(args->get_Uri(uri.put())))
            {
                this->SetHtml5Status(uri.get(), true);
            }
            return S_OK;
        }).Get(), &token);
        this->html5WebView->add_SourceChanged(Callback<ICoreWebView2SourceChangedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string uri;
            if (this->html5WebView && SUCCEEDED(this->html5WebView->get_Source(uri.put())))
            {
                this->SetHtml5Status(uri.get(), true);
            }
            return S_OK;
        }).Get(), &token);
        this->html5WebView->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            if (!this->html5Layered && this->hHtml5WebViewWnd)
            {
                // 動画ウィンドウと合成させる
                SetWindowLongW(this->hHtml5WebViewWnd, GWL_EXSTYLE, GetWindowLongW(this->hHtml5WebViewWnd, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
                this->html5Layered = true;
            }
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            wil::unique_cotaskmem_string uri;
            if (this->html5WebView && SUCCEEDED(this->html5WebView->get_Source(uri.put())))
            {
                this->SetHtml5Status(uri.get(), false);
            }
            if (!success && this->html5Active)
            {
                COREWEBVIEW2_WEB_ERROR_STATUS status;
                args->get_WebErrorStatus(&status);
                this->m_pApp->AddLog((L"HTML5アプリケーションの取得に失敗しました (WebErrorStatus=" + std::to_wstring((int)status) + L")").c_str(), TVTest::LOG_TYPE_WARNING);
            }
            return S_OK;
        }).Get(), &token);
        // 新しいウィンドウは開かせない
        this->html5WebView->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
        }).Get(), &token);
        this->NavigateHtml5();
        return S_OK;
    }).Get());
}

void CDataBroadcastingWV2::NavigateHtml5()
{
    if (!this->html5WebView)
    {
        return;
    }
    if (!this->html5ScriptId.empty())
    {
        this->html5WebView->RemoveScriptToExecuteOnDocumentCreated(this->html5ScriptId.c_str());
        this->html5ScriptId.clear();
    }
    auto script = utf8StrToWString((this->BuildHtml5ConfigScript() + this->html5RuntimeScript).c_str());
    this->html5WebView->AddScriptToExecuteOnDocumentCreated(script.c_str(), Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
        [this](HRESULT error, LPCWSTR id) -> HRESULT {
        if (SUCCEEDED(error) && id)
        {
            this->html5ScriptId = id;
        }
        if (!this->html5WebView)
        {
            return S_OK;
        }
        auto url = utf8StrToWString(this->html5Launch.value("url", std::string()).c_str());
        this->html5Active = true;
        this->html5KeySet = {};
        // 履歴は消去される (STD-0013 6.1.4.4)
        this->html5WebView->Navigate(L"about:blank");
        this->html5WebView->Navigate(url.c_str());
        this->html5Controller->put_IsVisible(TRUE);
        if (this->hHtml5WebViewWnd)
        {
            SetWindowPos(this->hHtml5WebViewWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        // BMLを隠す (TS処理は継続する)
        if (this->webView)
        {
            this->webView->PostWebMessageAsJson(LR"({"type":"bmlVisibility","visible":false})");
        }
        // 映像オブジェクトが提示されるまでは全画面
        this->invisible = true;
        this->ResizeVideoWindow();
        return S_OK;
    }).Get());
}

void CDataBroadcastingWV2::StopHtml5(bool reloadBML)
{
    if (!this->html5Active)
    {
        return;
    }
    this->html5Active = false;
    this->html5Unmanaged = false;
    this->html5LastVideo = nullptr;
    if (this->html5WebView)
    {
        this->html5WebView->Stop();
        this->html5WebView->Navigate(L"about:blank");
    }
    if (this->html5Controller)
    {
        this->html5Controller->put_IsVisible(FALSE);
    }
    if (this->caption)
    {
        this->UpdateCaptionState(false);
    }
    this->RestoreMainAudio();
    if (this->html5Muted)
    {
        this->html5Muted = false;
        this->m_pApp->SetMute(false);
    }
    // BMLの映像位置に戻す
    this->invisible = this->bmlInvisible;
    this->videoRect = this->bmlVideoRect;
    this->ResizeVideoWindow();
    if (this->webView)
    {
        this->webView->PostWebMessageAsJson(LR"({"type":"bmlVisibility","visible":true})");
        this->webView->PostWebMessageAsJson(LR"({"type":"html5Stopped"})");
        if (reloadBML)
        {
            // 再選局した直後の状態と同様に動作する
            this->webView->Reload();
        }
    }
    if (reloadBML)
    {
        // BS4K: 再選局と同等なのでAUTOSTARTのアプリケーションを改めて起動する
        this->db4kRunningKey.clear();
        this->db4kWaitingEntry.clear();
    }
    // ステータス表示をBMLに戻す (次のBML文書遷移で更新される)
    if (this->status.html5)
    {
        this->status.html5 = false;
        this->status.url.clear();
        this->status.loading = false;
        this->status.receiving = false;
        this->m_pApp->StatusItemNotify(1, TVTest::STATUS_ITEM_NOTIFY_REDRAW);
    }
    this->m_pApp->AddLog(L"HTML5アプリケーションを終了しました", TVTest::LOG_TYPE_INFORMATION);
}

void CDataBroadcastingWV2::SetHtml5Status(LPCWSTR uri, bool loading)
{
    if (!this->html5Active)
    {
        return;
    }
    std::wstring url = uri ? uri : L"";
    // about:blankなどは無視する
    if (url.empty() || url.compare(0, 6, L"about:") == 0)
    {
        return;
    }
    // 仮想ホスト (BS4K) はパスのみ表示する
    for (auto host : { L"https://arib4k.invalid", L"https://tvtdatabroadcastinghybrid.invalid" })
    {
        if (url.compare(0, wcslen(host), host) == 0)
        {
            url = url.substr(wcslen(host));
            break;
        }
    }
    if (this->status.url == url && this->status.loading == loading && this->status.html5)
    {
        return;
    }
    this->status.url = url;
    this->status.loading = loading;
    this->status.receiving = false;
    this->status.html5 = true;
    this->m_pApp->StatusItemNotify(1, TVTest::STATUS_ITEM_NOTIFY_REDRAW);
}

void CDataBroadcastingWV2::UpdateHtml5Bounds()
{
    if (!this->html5Controller || !this->webViewController)
    {
        return;
    }
    HWND parent = nullptr;
    if (SUCCEEDED(this->webViewController->get_ParentWindow(&parent)))
    {
        HWND current = nullptr;
        if (SUCCEEDED(this->html5Controller->get_ParentWindow(&current)) && current != parent)
        {
            this->html5Controller->put_ParentWindow(parent);
        }
    }
    RECT rect;
    if (FAILED(this->webViewController->get_Bounds(&rect)))
    {
        return;
    }
    // viewportの解像度をアスペクト比を維持して拡大縮小する
    auto width = rect.right - rect.left;
    auto height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0 || this->html5DocWidth <= 0 || this->html5DocHeight <= 0)
    {
        return;
    }
    double scale = std::min((double)width / this->html5DocWidth, (double)height / this->html5DocHeight);
    int w = (int)std::round(this->html5DocWidth * scale);
    int h = (int)std::round(this->html5DocHeight * scale);
    RECT bounds{};
    bounds.left = rect.left + (width - w) / 2;
    bounds.top = rect.top + (height - h) / 2;
    bounds.right = bounds.left + w;
    bounds.bottom = bounds.top + h;
    bool boundsChanged = memcmp(&bounds, &this->html5Bounds, sizeof(RECT)) != 0;
    if (boundsChanged)
    {
        this->html5Bounds = bounds;
        this->html5Controller->put_Bounds(bounds);
    }
    this->html5Controller->put_ZoomFactor(scale);
    if (this->html5Scale != scale)
    {
        this->html5Scale = scale;
        // 表示位置・倍率が変わったので映像の位置を計算し直す (最大化など)
        this->ApplyHtml5Video();
    }
    else if (boundsChanged)
    {
        this->ApplyHtml5Video();
    }
    if (this->hHtml5WebViewWnd && this->html5Active)
    {
        SetWindowPos(this->hHtml5WebViewWnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void CDataBroadcastingWV2::PostToHtml5(const nlohmann::json& message)
{
    if (this->html5WebView && this->html5Active)
    {
        this->html5WebView->PostWebMessageAsJson(utf8StrToWString(message.dump().c_str()).c_str());
    }
}

void CDataBroadcastingWV2::OnHtml5WebMessage(const nlohmann::json& a)
{
    if (!this->html5Active)
    {
        return;
    }
    auto type = a.value("type", std::string());
    if (type == "html5Video")
    {
        this->html5LastVideo = a;
        this->ApplyHtml5Video();
    }
    else if (type == "html5Viewport")
    {
        auto w = a.value("width", 1920);
        auto h = a.value("height", 1080);
        if (w > 0 && h > 0 && (w != this->html5DocWidth || h != this->html5DocHeight))
        {
            this->html5DocWidth = w;
            this->html5DocHeight = h;
            this->UpdateHtml5Bounds();
        }
    }
    else if (type == "html5KeySet")
    {
        auto&& g = a["groups"];
        if (g.is_object())
        {
            this->html5KeySet.red = g.value("RED", false);
            this->html5KeySet.green = g.value("GREEN", false);
            this->html5KeySet.yellow = g.value("YELLOW", false);
            this->html5KeySet.blue = g.value("BLUE", false);
            this->html5KeySet.navigation = g.value("NAVIGATION", false);
            this->html5KeySet.numeric = g.value("NUMERIC", false);
            this->html5KeySet.vcr = g.value("VCR", false);
            this->html5KeySet.dbutton = g.value("DBUTTON", false);
        }
    }
    else if (type == "html5KeyUnhandled")
    {
        auto name = a.value("name", std::string());
        if (name == "VK_BACK" && a.value("default", false))
        {
            BOOL canGoBack = FALSE;
            if (this->html5WebView && SUCCEEDED(this->html5WebView->get_CanGoBack(&canGoBack)) && canGoBack)
            {
                this->html5WebView->GoBack();
            }
        }
    }
    else if (type == "html5Tune")
    {
        this->TuneToService(a.value("originalNetworkId", 0), a.value("transportStreamId", 0), a.value("serviceId", 0));
    }
    else if (type == "html5Destroy")
    {
        this->StopHtml5(true);
    }
    else if (type == "html5LaunchDataBroadcasting")
    {
        this->StopHtml5(true);
    }
    else if (type == "html5Replace")
    {
        // BML側でAITを取得して起動し直す
        if (this->webView)
        {
            nlohmann::json msg{
                { "type", "html5Replace" },
                { "organizationId", a.value("organizationId", 0) },
                { "applicationId", a.value("applicationId", 0) },
                { "aitUrl", a.contains("aitUrl") ? a["aitUrl"] : nlohmann::json() },
            };
            this->webView->PostWebMessageAsJson(utf8StrToWString(msg.dump().c_str()).c_str());
        }
    }
    else if (type == "html5ExitManaged")
    {
        auto url = a.value("url", std::string());
        if (url.starts_with("http://") || url.starts_with("https://"))
        {
            this->html5Launch["url"] = url;
            this->html5Unmanaged = true;
            this->NavigateHtml5();
        }
        else
        {
            this->StopHtml5(true);
        }
    }
    else if (type == "html5Caption")
    {
        this->SetCaptionState(a.value("visible", false));
    }
}

bool CDataBroadcastingWV2::OnHtml5Command(int ID)
{
    const char* name = nullptr;
    bool inKeySet = false;
    LPCWSTR fallbackCommand = nullptr;
    auto&& k = this->html5KeySet;
    switch (ID)
    {
    case IDC_KEY_UP: name = "VK_UP"; inKeySet = k.navigation; break;
    case IDC_KEY_DOWN: name = "VK_DOWN"; inKeySet = k.navigation; break;
    case IDC_KEY_LEFT: name = "VK_LEFT"; inKeySet = k.navigation; break;
    case IDC_KEY_RIGHT: name = "VK_RIGHT"; inKeySet = k.navigation; break;
    case IDC_KEY_ENTER: name = "VK_ENTER"; inKeySet = k.navigation; break;
    case IDC_KEY_BACK: name = "VK_BACK"; inKeySet = k.navigation; break;
    case IDC_KEY_RED: name = "VK_RED"; inKeySet = k.red; break;
    case IDC_KEY_GREEN: name = "VK_GREEN"; inKeySet = k.green; break;
    case IDC_KEY_YELLOW: name = "VK_YELLOW"; inKeySet = k.yellow; break;
    case IDC_KEY_BLUE: name = "VK_BLUE"; inKeySet = k.blue; break;
    case IDC_KEY_0: name = "VK_0"; inKeySet = k.numeric; break;
    case IDC_KEY_1: name = "VK_1"; inKeySet = k.numeric; fallbackCommand = L"Channel1"; break;
    case IDC_KEY_2: name = "VK_2"; inKeySet = k.numeric; fallbackCommand = L"Channel2"; break;
    case IDC_KEY_3: name = "VK_3"; inKeySet = k.numeric; fallbackCommand = L"Channel3"; break;
    case IDC_KEY_4: name = "VK_4"; inKeySet = k.numeric; fallbackCommand = L"Channel4"; break;
    case IDC_KEY_5: name = "VK_5"; inKeySet = k.numeric; fallbackCommand = L"Channel5"; break;
    case IDC_KEY_6: name = "VK_6"; inKeySet = k.numeric; fallbackCommand = L"Channel6"; break;
    case IDC_KEY_7: name = "VK_7"; inKeySet = k.numeric; fallbackCommand = L"Channel7"; break;
    case IDC_KEY_8: name = "VK_8"; inKeySet = k.numeric; fallbackCommand = L"Channel8"; break;
    case IDC_KEY_9: name = "VK_9"; inKeySet = k.numeric; fallbackCommand = L"Channel9"; break;
    case IDC_KEY_10: name = "VK_10"; inKeySet = k.numeric; fallbackCommand = L"Channel10"; break;
    case IDC_KEY_11: name = "VK_11"; inKeySet = k.numeric; fallbackCommand = L"Channel11"; break;
    case IDC_KEY_12: name = "VK_12"; inKeySet = k.numeric; fallbackCommand = L"Channel12"; break;
    case IDC_KEY_D:
    case IDC_KEY_D_OR_ENABLE_PLUGIN:
        name = "VK_DBUTTON";
        inKeySet = k.dbutton;
        if (!inKeySet)
        {
            // dボタン: データ放送 (BML) に戻る
            this->StopHtml5(true);
            return true;
        }
        break;
    case IDC_KEY_DEVTOOL:
        if (this->html5WebView)
        {
            this->html5WebView->OpenDevToolsWindow();
        }
        return true;
    case IDC_KEY_RELOAD:
        if (this->html5WebView)
        {
            this->html5WebView->Reload();
        }
        return true;
    default:
        return false;
    }
    if (inKeySet)
    {
        this->PostToHtml5({ { "type", "key" }, { "name", name }, { "action", "press" } });
    }
    else if (fallbackCommand && this->useTVTestChannelCommand)
    {
        this->m_pApp->DoCommand(fallbackCommand);
    }
    return true;
}

void CDataBroadcastingWV2::LoadHtml5Storage()
{
    if (this->html5StorageLoaded)
    {
        return;
    }
    this->html5StorageLoaded = true;
    std::ifstream ifs(std::filesystem::path(this->baseDirectory) / L"aribhtml5_storage.json", std::ios::binary);
    if (!ifs)
    {
        return;
    }
    try
    {
        auto json = nlohmann::json::parse(ifs);
        for (auto&& [k, v] : json.items())
        {
            if (v.is_string())
            {
                this->html5Storage[k] = v.get<std::string>();
            }
        }
    }
    catch (...)
    {
    }
}

void CDataBroadcastingWV2::SaveHtml5Storage()
{
    nlohmann::json json = nlohmann::json::object();
    for (auto&& [k, v] : this->html5Storage)
    {
        // greg/uregは不揮発ではない
        if (k.starts_with("greg") || k.starts_with("ureg"))
        {
            continue;
        }
        json[k] = v;
    }
    std::ofstream ofs(std::filesystem::path(this->baseDirectory) / L"aribhtml5_storage.json", std::ios::binary | std::ios::trunc);
    ofs << json.dump();
}

nlohmann::json CDataBroadcastingWV2::HandleHtml5NativeRPC(const std::string& method, const nlohmann::json& params, bool& handled)
{
    handled = true;
    if (method == "storageGet" || method == "storageSet" || method == "storageRemove")
    {
        this->LoadHtml5Storage();
        auto key = params.value("key", std::string());
        std::string scoped = key;
        // 事業者専用領域はoriginごとに分ける
        if (key.starts_with("_wlocal") || key.starts_with("_local"))
        {
            scoped = std::to_string(this->currentChannel.NetworkID) + ":" + key;
        }
        if (method == "storageGet")
        {
            auto it = this->html5Storage.find(scoped);
            if (it == this->html5Storage.end())
            {
                if (key == "_prefecture" || key == "_regioncode" || key == "_zipcode")
                {
                    return "";
                }
                return nullptr;
            }
            return it->second;
        }
        if (method == "storageSet")
        {
            auto value = params.value("value", std::string());
            // TR-B39 表10-8 (greg/ureg: 512B)
            if ((key.starts_with("greg") || key.starts_with("ureg")) && value.size() > 512 * 3)
            {
                return "quota";
            }
            this->html5Storage[scoped] = value;
        }
        else
        {
            this->html5Storage.erase(scoped);
        }
        if (!key.starts_with("greg") && !key.starts_with("ureg"))
        {
            this->SaveHtml5Storage();
        }
        return true;
    }
    if (method == "isCommonEvent")
    {
        return false;
    }
    if (method == "confirmIPNetwork")
    {
        return this->proxySession != nullptr && this->enableNetwork;
    }
    if (method == "storeDataResource")
    {
        return true;
    }
    if (method == "db4kList")
    {
        // デバッグ用: dantto4kが保持しているファイル一覧
        return this->ListDB4KFiles();
    }
    handled = false;
    return nullptr;
}

static HRESULT CreateJsonResponse(ICoreWebView2Environment* env, const std::string& body, ICoreWebView2WebResourceResponse** response)
{
    wil::com_ptr<IStream> stm;
    auto hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD, std::max<size_t>(body.size(), 1));
    if (!hGlobal)
    {
        return E_OUTOFMEMORY;
    }
    auto mem = GlobalLock(hGlobal);
    memcpy(mem, body.data(), body.size());
    GlobalUnlock(hGlobal);
    HRESULT hr = CreateStreamOnHGlobal(hGlobal, TRUE, stm.put());
    if (FAILED(hr))
    {
        GlobalFree(hGlobal);
        return hr;
    }
    ULARGE_INTEGER size;
    size.QuadPart = body.size();
    stm->SetSize(size);
    return env->CreateWebResourceResponse(stm.get(), 200, L"OK", L"Content-Type: application/json; charset=utf-8\nAccess-Control-Allow-Origin: *\nCache-Control: no-store", response);
}

HRESULT CDataBroadcastingWV2::OnHtml5WebResourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args)
{
    wil::com_ptr<ICoreWebView2WebResourceRequest> request;
    HRESULT hr = args->get_Request(request.put());
    if (FAILED(hr))
    {
        return hr;
    }
    wil::unique_cotaskmem_string uri;
    hr = request->get_Uri(uri.put());
    if (FAILED(hr))
    {
        return hr;
    }
    if (!this->webViewEnvironment)
    {
        return S_OK;
    }
    auto env = this->webViewEnvironment;
    if (!_wcsnicmp(uri.get(), HTML5_RPC_URL, wcslen(HTML5_RPC_URL)))
    {
        wil::unique_cotaskmem_string method;
        request->get_Method(method.put());
        if (_wcsicmp(method.get(), L"POST"))
        {
            // CORSのpreflightなど
            wil::com_ptr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 204, L"No Content", L"Access-Control-Allow-Origin: *\nAccess-Control-Allow-Methods: POST\nAccess-Control-Allow-Headers: Content-Type", response.put())))
            {
                args->put_Response(response.get());
            }
            return S_OK;
        }
        std::string body;
        wil::com_ptr<IStream> content;
        if (SUCCEEDED(request->get_Content(content.put())) && content)
        {
            STATSTG statstg = {};
            if (SUCCEEDED(content->Stat(&statstg, STATFLAG_NONAME)) && statstg.cbSize.QuadPart < 16 * 1024 * 1024)
            {
                body.resize((size_t)statstg.cbSize.QuadPart);
                ULONG read = 0;
                content->Read(body.data(), (ULONG)body.size(), &read);
                body.resize(read);
            }
        }
        nlohmann::json req;
        try
        {
            req = nlohmann::json::parse(body);
        }
        catch (...)
        {
            req = nlohmann::json::object();
        }
        auto rpcMethod = req.value("method", std::string());
        auto params = req.contains("params") ? req["params"] : nlohmann::json();
        bool handled = false;
        auto result = this->HandleHtml5NativeRPC(rpcMethod, params, handled);
        if (handled || !this->webView || !this->html5Active)
        {
            wil::com_ptr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(CreateJsonResponse(env.get(), nlohmann::json{ { "result", result } }.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), response.put())))
            {
                args->put_Response(response.get());
            }
            return S_OK;
        }
        // BML側 (web-bml) で処理する
        wil::com_ptr<ICoreWebView2Deferral> deferral;
        args->GetDeferral(deferral.put());
        nlohmann::json msg{ { "type", "rpc" }, { "method", rpcMethod }, { "params", params } };
        auto script = L"JSON.stringify(window.sendMessage(" + utf8StrToWString(msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str()) + L"))";
        hr = this->webView->ExecuteScript(script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [env, deferral, args = wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>(args)](HRESULT error, LPCWSTR resultObjectAsJson) -> HRESULT {
            nlohmann::json result;
            if (SUCCEEDED(error) && resultObjectAsJson)
            {
                try
                {
                    // JSON.stringifyの結果 (文字列) がJSONとして返される
                    auto str = nlohmann::json::parse(wstrToUTF8String(resultObjectAsJson));
                    if (str.is_string())
                    {
                        auto reply = nlohmann::json::parse(str.get<std::string>());
                        if (reply.is_object() && reply.contains("result"))
                        {
                            result = reply["result"];
                        }
                    }
                }
                catch (...)
                {
                }
            }
            wil::com_ptr<ICoreWebView2WebResourceResponse> response;
            if (SUCCEEDED(CreateJsonResponse(env.get(), nlohmann::json{ { "result", result } }.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), response.put())))
            {
                args->put_Response(response.get());
            }
            deferral->Complete();
            return S_OK;
        }).Get());
        if (FAILED(hr))
        {
            deferral->Complete();
        }
        return S_OK;
    }
    // BS4K: 放送で伝送されるファイル
    if (!_wcsnicmp(uri.get(), L"https://arib4k.invalid/", wcslen(L"https://arib4k.invalid/")))
    {
        std::wstring path(uri.get() + wcslen(L"https://arib4k.invalid"));
        if (this->RespondDB4KFile(args, path))
        {
            return S_OK;
        }
        // まだ受信していないファイルは受信されるまで待つ (受信機はキャッシュに揃うまで待つ)
        wil::com_ptr<ICoreWebView2Deferral> deferral;
        if (SUCCEEDED(args->GetDeferral(deferral.put())))
        {
            this->db4kPending.push_back({ wil::com_ptr<ICoreWebView2WebResourceRequestedEventArgs>(args), deferral, path, GetTickCount64() + 15000 });
            return S_OK;
        }
        wil::com_ptr<ICoreWebView2WebResourceResponse> response;
        if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", response.put())))
        {
            args->put_Response(response.get());
        }
        return S_OK;
    }
    if (!_wcsnicmp(uri.get(), L"about:", 6) || !_wcsnicmp(uri.get(), L"data:", 5) || !_wcsnicmp(uri.get(), L"blob:", 5))
    {
        return S_OK;
    }
    // 通信が無効の場合は外部へのリクエストを全てブロックする
    if (!(this->proxySession && this->enableNetwork))
    {
        wil::com_ptr<ICoreWebView2WebResourceResponse> response;
        if (SUCCEEDED(env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", response.put())))
        {
            args->put_Response(response.get());
        }
    }
    return S_OK;
}

// ---------------------------------------------------------------
// BS4K (BonDriver_dantto4kのDB4K_* API)
// ---------------------------------------------------------------

struct DB4KAppInfo
{
    uint16_t applicationType;
    uint32_t organizationId;
    uint16_t applicationId;
    uint8_t controlCode;
    char entryPointUrl[512];
    int valid;
};

typedef void(__stdcall* DB4K_SetCallbackFunc)(void(*cb)(int event, void* userData), void* userData);
typedef int(__stdcall* DB4K_GetApplicationInfoFunc)(DB4KAppInfo* info);
typedef int(__stdcall* DB4K_GetFileByPathFunc)(const char* path, uint8_t* buffer, uint32_t* size);
typedef int(__stdcall* DB4K_GetFileContentTypeFunc)(const char* path, char* buffer, uint32_t bufferSize);
typedef int(__stdcall* DB4K_ListFilesFunc)(char* buffer, uint32_t* size);

static struct
{
    HMODULE module = nullptr;
    DB4K_SetCallbackFunc setCallback = nullptr;
    DB4K_GetApplicationInfoFunc getApplicationInfo = nullptr;
    DB4K_GetFileByPathFunc getFileByPath = nullptr;
    DB4K_GetFileContentTypeFunc getFileContentType = nullptr;
    DB4K_ListFilesFunc listFiles = nullptr;
} g_db4k;

static bool LoadDB4K()
{
    // TVTestが読み込んでいるBonDriverを使う (二重に読み込まない)
    HMODULE modules[] = {
        GetModuleHandleW(L"BonDriver_dantto4k.dll"),
    };
    HMODULE module = nullptr;
    for (auto m : modules)
    {
        if (m && GetProcAddress(m, "DB4K_GetApplicationInfo"))
        {
            module = m;
            break;
        }
    }
    if (!module)
    {
        // 名前を変えている場合
        HMODULE list[1024];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &needed))
        {
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < _countof(list); i++)
            {
                if (GetProcAddress(list[i], "DB4K_GetApplicationInfo") && GetProcAddress(list[i], "DB4K_GetFileByPath"))
                {
                    module = list[i];
                    break;
                }
            }
        }
    }
    if (module != g_db4k.module)
    {
        g_db4k.module = module;
        g_db4k.setCallback = module ? (DB4K_SetCallbackFunc)GetProcAddress(module, "DB4K_SetCallback") : nullptr;
        g_db4k.getApplicationInfo = module ? (DB4K_GetApplicationInfoFunc)GetProcAddress(module, "DB4K_GetApplicationInfo") : nullptr;
        g_db4k.getFileByPath = module ? (DB4K_GetFileByPathFunc)GetProcAddress(module, "DB4K_GetFileByPath") : nullptr;
        g_db4k.getFileContentType = module ? (DB4K_GetFileContentTypeFunc)GetProcAddress(module, "DB4K_GetFileContentType") : nullptr;
        g_db4k.listFiles = module ? (DB4K_ListFilesFunc)GetProcAddress(module, "DB4K_ListFiles") : nullptr;
    }
    return g_db4k.getApplicationInfo && g_db4k.getFileByPath;
}

static HWND g_db4kNotifyWnd = nullptr;
static std::atomic<int> g_db4kPendingEvents = 0;

static void DB4KCallback(int event, void* userData)
{
    // dantto4kのスレッドから呼ばれる
    // 0: AitUpdated, 1: DirectoryUpdated, 2: FileUpdated, 3: ContentReset
    g_db4kPendingEvents |= 1 << event;
    if (g_db4kNotifyWnd)
    {
        PostMessageW(g_db4kNotifyWnd, WM_APP_DB4K, 0, 0);
    }
}

static std::wstring GuessContentType(const std::string& path)
{
    auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    static const std::unordered_map<std::string, const wchar_t*> types{
        { "html", L"text/html; charset=utf-8" }, { "htm", L"text/html; charset=utf-8" },
        { "js", L"text/javascript; charset=utf-8" }, { "css", L"text/css; charset=utf-8" },
        { "json", L"application/json" }, { "xml", L"application/xml" },
        { "png", L"image/png" }, { "jpg", L"image/jpeg" }, { "jpeg", L"image/jpeg" },
        { "gif", L"image/gif" }, { "svg", L"image/svg+xml" }, { "webp", L"image/webp" },
        { "woff", L"font/woff" }, { "woff2", L"font/woff2" }, { "ttf", L"font/ttf" },
        { "mp3", L"audio/mpeg" }, { "aac", L"audio/aac" }, { "mp4", L"video/mp4" },
        { "ttml", L"application/ttml+xml" },
    };
    auto it = types.find(ext);
    return it == types.end() ? L"application/octet-stream" : it->second;
}

bool CDataBroadcastingWV2::GetDB4KFile(LPCWSTR path, std::vector<BYTE>& data, std::wstring& contentType)
{
    // TS側伝送 (nanamitm dantto の D4KA PID) があればそちらを優先、無ければ DB4K DLL
    bool useTs = this->tsAppData.hasPid();
    if (!useTs && !LoadDB4K())
    {
        return false;
    }
    auto p = wstrToUTF8String(path);
    auto q = p.find_first_of("?#");
    if (q != std::string::npos)
    {
        p.resize(q);
    }
    // %エスケープを戻す
    std::string decoded;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i] == '%' && i + 2 < p.size() && isxdigit((unsigned char)p[i + 1]) && isxdigit((unsigned char)p[i + 2]))
        {
            decoded += (char)std::stoi(p.substr(i + 1, 2), nullptr, 16);
            i += 2;
        }
        else
        {
            decoded += p[i];
        }
    }
    // URL = baseurl + base_directory_path + directory_node_path + file_name
    // DB4K_GetFileByPathはdirectory_node_path + file_nameで引けるので先頭から1階層ずつ削って探す
    std::vector<std::string> candidates;
    std::string rest = decoded;
    while (!rest.empty())
    {
        candidates.push_back(rest);
        if (rest[0] == '/')
        {
            candidates.push_back(rest.substr(1));
        }
        auto slash = rest.find('/', 1);
        if (slash == std::string::npos)
        {
            break;
        }
        rest = rest.substr(slash);
    }
    for (auto&& c : candidates)
    {
        if (useTs)
        {
            std::vector<uint8_t> tsData;
            std::string tsType;
            if (this->tsAppData.getFileByPath(c, tsData, tsType))
            {
                data.assign(tsData.begin(), tsData.end());
                contentType = tsType.empty() ? GuessContentType(c) : utf8StrToWString(tsType.c_str());
                if (contentType.starts_with(L"text/") && contentType.find(L"charset") == std::wstring::npos)
                {
                    contentType += L"; charset=utf-8";
                }
                return true;
            }
            continue; // TS優先時はDLLにフォールバックしない
        }
        uint32_t size = 0;
        if (!g_db4k.getFileByPath(c.c_str(), nullptr, &size))
        {
            continue;
        }
        data.resize(size);
        if (!g_db4k.getFileByPath(c.c_str(), data.data(), &size))
        {
            continue;
        }
        data.resize(size);
        contentType = GuessContentType(c);
        char type[256] = {};
        if (g_db4k.getFileContentType && g_db4k.getFileContentType(c.c_str(), type, sizeof(type)) && type[0])
        {
            contentType = utf8StrToWString(type);
            if (contentType.starts_with(L"text/") && contentType.find(L"charset") == std::wstring::npos)
            {
                contentType += L"; charset=utf-8";
            }
        }
        return true;
    }
    return false;
}

void CDataBroadcastingWV2::CheckDB4K()
{
    this->ProcessDB4KPending();
    if (!this->html5Enabled || !this->webViewEnvironment)
    {
        return;
    }
    // TS側伝送 (nanamitm dantto の D4KA PID) があればそちらを優先、無ければ DB4K DLL
    bool useTs = this->tsAppData.hasPid();
    if (!useTs && !LoadDB4K())
    {
        return;
    }
    if (!useTs && g_db4k.setCallback && g_db4kNotifyWnd != this->hMessageWnd)
    {
        g_db4kNotifyWnd = this->hMessageWnd;
        g_db4k.setCallback(DB4KCallback, this);
    }
    int events = g_db4kPendingEvents.exchange(0);
    DB4KAppInfo info = {};
    bool valid;
    if (useTs)
    {
        AppDataTs::Receiver::AppInfo a;
        valid = this->tsAppData.getApplicationInfo(a);
        if (valid)
        {
            info.applicationType = a.applicationType;
            info.organizationId = a.organizationId;
            info.applicationId = a.applicationId;
            info.controlCode = a.controlCode;
            strncpy_s(info.entryPointUrl, a.entryUrl.c_str(), _TRUNCATE);
            info.valid = 1;
        }
    }
    else
    {
        valid = g_db4k.getApplicationInfo(&info) && info.valid;
    }
    std::string key = valid ? std::to_string(info.organizationId) + "." + std::to_string(info.applicationId) + ":" + info.entryPointUrl : "";
    if (this->html5Active && this->html5Launch.value("profile", std::string()) == "bs4k")
    {
        // KILLまたはAITが無くなった場合は終了 (TR-B39 表11-1)
        if (!valid || info.controlCode == 0x04 || key != this->db4kRunningKey)
        {
            this->db4kRunningKey.clear();
            this->StopHtml5(false);
        }
        else if (events & ((1 << 2) | (1 << 1)))
        {
            this->PostToHtml5({ { "type", "cacheEvent" }, { "path", "/" }, { "event", "updated" } });
        }
        if (this->html5Active || !valid)
        {
            return;
        }
    }
    if (!valid || info.controlCode != 0x01 || (info.applicationType != 0x0011 && info.applicationType != 0x0010))
    {
        return;
    }
    if (this->html5Active || key == this->db4kRunningKey)
    {
        return;
    }
    std::string entry = info.entryPointUrl;
    std::string url;
    if (entry.starts_with("http://") || entry.starts_with("https://"))
    {
        // protocol_id=0x0003
        url = entry;
    }
    else
    {
        // protocol_id=0x0005 (MMTで伝送)
        // エントリが揃うまで待つ
        std::vector<BYTE> data;
        std::wstring contentType;
        if (!this->GetDB4KFile(utf8StrToWString(("/" + entry).c_str()).c_str(), data, contentType))
        {
            if (this->db4kWaitingEntry != entry)
            {
                this->db4kWaitingEntry = entry;
                std::string list;
                if (useTs)
                {
                    for (auto& f : this->tsAppData.listFiles()) list += f + "\n";
                }
                else if (g_db4k.listFiles)
                {
                    uint32_t size = 0;
                    g_db4k.listFiles(nullptr, &size);
                    list.resize(size);
                    g_db4k.listFiles(list.data(), &size);
                }
                this->m_pApp->AddLog((L"BS4K: エントリ " + utf8StrToWString(entry.c_str()) + L" を待っています。受信済みファイル: " + utf8StrToWString(list.c_str())).c_str(), TVTest::LOG_TYPE_INFORMATION);
            }
            return;
        }
        while (!entry.empty() && entry[0] == '/')
        {
            entry.erase(0, 1);
        }
        url = "https://arib4k.invalid/" + entry;
    }
    this->db4kRunningKey = key;
    nlohmann::json launch{
        { "profile", "bs4k" },
        { "url", url },
        { "organizationId", info.organizationId },
        { "applicationId", info.applicationId },
        { "controlCode", info.controlCode },
        { "applicationType", info.applicationType },
        { "typeName", "ARIB-HTML5" },
        { "priority", 0xff },
        { "boundaries", nlohmann::json::array() },
        { "applications", nlohmann::json::array({ {
            { "organizationId", info.organizationId },
            { "applicationId", info.applicationId },
            { "controlCode", info.controlCode },
            { "applicationType", info.applicationType },
            { "priority", 0xff },
            { "url", url },
            { "boundaries", nlohmann::json::array() },
        } }) },
        { "aitSource", nullptr },
        { "originalNetworkId", (int)this->currentChannel.NetworkID },
        { "transportStreamId", (int)this->currentChannel.TransportStreamID },
        { "serviceId", (int)this->currentService.ServiceID },
    };
    this->LaunchHtml5(launch);
}

std::string CDataBroadcastingWV2::ListDB4KFiles()
{
    if (!LoadDB4K() || !g_db4k.listFiles)
    {
        return "";
    }
    uint32_t size = 0;
    g_db4k.listFiles(nullptr, &size);
    std::string list(size, ' ');
    g_db4k.listFiles(list.data(), &size);
    list.resize(strlen(list.c_str()));
    return list;
}

bool CDataBroadcastingWV2::RespondDB4KFile(ICoreWebView2WebResourceRequestedEventArgs* args, const std::wstring& path)
{
    if (!this->webViewEnvironment)
    {
        return false;
    }
    std::vector<BYTE> data;
    std::wstring contentType;
    if (!this->GetDB4KFile(path.c_str(), data, contentType))
    {
        return false;
    }
    wil::com_ptr<IStream> stm;
    auto hGlobal = GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD, std::max<size_t>(data.size(), 1));
    if (!hGlobal)
    {
        return false;
    }
    memcpy(GlobalLock(hGlobal), data.data(), data.size());
    GlobalUnlock(hGlobal);
    if (FAILED(CreateStreamOnHGlobal(hGlobal, TRUE, stm.put())))
    {
        GlobalFree(hGlobal);
        return false;
    }
    ULARGE_INTEGER size;
    size.QuadPart = data.size();
    stm->SetSize(size);
    auto headers = L"Content-Type: " + contentType + L"\nCache-Control: no-store\nAccess-Control-Allow-Origin: *";
    wil::com_ptr<ICoreWebView2WebResourceResponse> response;
    if (FAILED(this->webViewEnvironment->CreateWebResourceResponse(stm.get(), 200, L"OK", headers.c_str(), response.put())))
    {
        return false;
    }
    args->put_Response(response.get());
    return true;
}

void CDataBroadcastingWV2::ProcessDB4KPending()
{
    if (this->db4kPending.empty())
    {
        return;
    }
    auto now = GetTickCount64();
    std::vector<DB4KPendingRequest> rest;
    for (auto&& req : this->db4kPending)
    {
        if (this->RespondDB4KFile(req.args.get(), req.path))
        {
            req.deferral->Complete();
        }
        else if (now >= req.deadline || !this->html5Active)
        {
            wil::com_ptr<ICoreWebView2WebResourceResponse> response;
            if (this->webViewEnvironment && SUCCEEDED(this->webViewEnvironment->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", response.put())))
            {
                req.args->put_Response(response.get());
            }
            req.deferral->Complete();
        }
        else
        {
            rest.push_back(std::move(req));
        }
    }
    this->db4kPending = std::move(rest);
}

void CDataBroadcastingWV2::ApplyHtml5Video()
{
    if (!this->html5Active || !this->html5LastVideo.is_object() || !this->webViewController)
    {
        return;
    }
    auto&& a = this->html5LastVideo;
    bool present = a.value("present", false);
    bool fullscreen = a.value("fullscreen", false);
    if (!present || fullscreen)
    {
        // 放送映像objectが無い場合と全画面指定の場合は全画面で表示
        this->invisible = true;
    }
    else
    {
        // CSSピクセル × 倍率 + WebView#2の表示位置 (レターボックス分のオフセット)
        double scale = this->html5Scale > 0 ? this->html5Scale : 1.0;
        RECT r;
        r.left = this->html5Bounds.left + (int)std::floor(a.value("cssLeft", 0.0) * scale);
        r.top = this->html5Bounds.top + (int)std::floor(a.value("cssTop", 0.0) * scale);
        r.right = this->html5Bounds.left + (int)std::ceil(a.value("cssRight", 0.0) * scale);
        r.bottom = this->html5Bounds.top + (int)std::ceil(a.value("cssBottom", 0.0) * scale);
        RECT full{};
        this->webViewController->get_Bounds(&full);
        this->videoRect = r;
        this->invisible = (r.left <= full.left + 1 && r.top <= full.top + 1 && r.right >= full.right - 1 && r.bottom >= full.bottom - 1);
    }
    this->ResizeVideoWindow();
    // enableAudioMute / display:none (STD-0013 6.1.4.1.1)
    bool mute = present && a.value("audioMute", false);
    if (mute != this->html5Muted)
    {
        this->html5Muted = mute;
        this->m_pApp->SetMute(mute);
    }
}
