#include "pch.h"
#include "NVRAMSettingsDialog.h"
#include "resource.h"
std::string wstrToUTF8String(const wchar_t* ws);
std::wstring utf8StrToWString(const char* s);

struct Region
{
    LPCWSTR name;
    unsigned char prefecture;
    unsigned short regioncode;
};

static Region regionList[] =
{
    { L"未設定", 255, 0 },
    { L"東北海道", 1, 0b000101101011 },
    { L"西北海道", 2, 0b000101101011 },
    { L"青森県", 3, 0b010001100111 },
    { L"岩手県", 4, 0b010111010100 },
    { L"宮城県", 5, 0b011101011000 },
    { L"秋田県", 6, 0b101011000110 },
    { L"山形県", 7, 0b111001001100 },
    { L"福島県", 8, 0b000110101110 },
    { L"茨城県", 9, 0b110001101001 },
    { L"栃木県", 10, 0b111000111000 },
    { L"群馬県", 11, 0b100110001011 },
    { L"埼玉県", 12, 0b011001001011 },
    { L"千葉県", 13, 0b000111000111 },
    { L"東京都(島部を除く)", 14, 0b101010101100 },
    { L"神奈川県", 15, 0b010101101100 },
    { L"新潟県", 16, 0b010011001110 },
    { L"富山県", 17, 0b010100111001 },
    { L"石川県", 18, 0b011010100110 },
    { L"福井県", 19, 0b100100101101 },
    { L"山梨県", 20, 0b110101001010 },
    { L"長野県", 21, 0b100111010010 },
    { L"岐阜県", 22, 0b101001100101 },
    { L"静岡県", 23, 0b101001011010 },
    { L"愛知県", 24, 0b100101100110 },
    { L"三重県", 25, 0b001011011100 },
    { L"滋賀県", 26, 0b110011100100 },
    { L"京都府", 27, 0b010110011010 },
    { L"大阪府", 28, 0b110010110010 },
    { L"兵庫県", 29, 0b011001110100 },
    { L"奈良県 ", 30, 0b101010010011 },
    { L"和歌山県", 31, 0b001110010110 },
    { L"鳥取県 ", 32, 0b110100100011 },
    { L"島根県", 33, 0b001100011011 },
    { L"岡山県", 34, 0b001010110101 },
    { L"広島県", 35, 0b101100110001 },
    { L"山口県", 36, 0b101110011000 },
    { L"徳島県", 37, 0b111001100010 },
    { L"香川県", 38, 0b100110110100 },
    { L"愛媛県", 39, 0b000110011101 },
    { L"高知県 ", 40, 0b001011100011 },
    { L"福岡県", 41, 0b011000101101 },
    { L"佐賀県", 42, 0b100101011001 },
    { L"長崎県", 43, 0b101000101011 },
    { L"熊本県", 44, 0b100010100111 },
    { L"大分県", 45, 0b110010001101 },
    { L"宮崎県", 46, 0b110100011100 },
    { L"鹿児島県(南西諸島を除く)", 47, 0b110101000101 },
    { L"沖縄県", 48, 0b001101110010 },
    { L"東京都島部(伊豆・小笠原諸島)", 49, 0b101010101100 },
    { L"鹿児島県島部(南西諸島の鹿児島県域)", 50, 0b110101000101 },
};



HRESULT NVRAMSettingsDialog::ReadNVRAM(std::function<HRESULT(HRESULT, nlohmann::json&)> callback, const char *filename, const char *structure)
{
    nlohmann::json msg{ { "type", "nvramRead" }, { "filename", filename }, { "structure",structure } };
    std::ostringstream ss;
    // メッセージで通信するのは面倒なのでこうする
    ss << "window.sendMessage(" << msg << ")";
    auto ws = utf8StrToWString(ss.str().c_str());
    return this->webView->ExecuteScript(ws.c_str(), Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [callback](HRESULT result, LPCWSTR resultObjectAsJson) -> HRESULT {
        if (SUCCEEDED(result))
        {
            auto json = nlohmann::json::parse(wstrToUTF8String(resultObjectAsJson));
            return callback(result, json);
        }
        return S_OK;
    }).Get());
}
INT_PTR CALLBACK NVRAMSettingsDialog::DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void* pClientData)
{
    NVRAMSettingsDialog* pThis = static_cast<NVRAMSettingsDialog*>(pClientData);
    switch (uMsg) {
    case WM_INITDIALOG:
    {
        pThis->hDlg = hDlg;
        if (pThis->webView)
        {
            SendDlgItemMessageW(hDlg, IDC_EDIT_ZIP, EM_LIMITTEXT, 7, 0);
            pThis->ReadNVRAM([pThis](HRESULT result, nlohmann::json& json) -> HRESULT {
                auto&& data = json["data"];
                if (!data.is_array() || data.size() != 1)
                {
                    return S_OK;
                }
                auto&& zip = data[0];
                if (!zip.is_string())
                {
                    return S_OK;
                }
                auto zipStr = zip.get<std::string>();
                std::string filteredZip;
                std::copy_if(zipStr.begin(), zipStr.end(), std::back_inserter(filteredZip), [](char c) { return c >= '0' && c <= '9'; });
                SetDlgItemTextW(pThis->hDlg, IDC_EDIT_ZIP, utf8StrToWString(filteredZip.c_str()).c_str());
                EnableWindow(GetDlgItem(pThis->hDlg, IDC_EDIT_ZIP), TRUE);
                SetFocus(GetDlgItem(pThis->hDlg, IDC_EDIT_ZIP));
                return S_OK;
            }, "nvram://receiverinfo/zipcode", "S:7B");
            for (auto i = 0; i < _countof(regionList); i++)
            {
                SendDlgItemMessageW(hDlg, IDC_COMBO_REGION, CB_INSERTSTRING, i, (LPARAM)regionList[i].name);
            }
            pThis->ReadNVRAM([pThis](HRESULT result, nlohmann::json& json) -> HRESULT {
                auto&& data = json["data"];
                if (!data.is_array() || data.size() != 1)
                {
                    return S_OK;
                }
                auto&& regionCodeJson = data[0];
                if (!regionCodeJson.is_number_unsigned())
                {
                    return S_OK;
                }
                auto regionCode = regionCodeJson.get<int>();
                pThis->ReadNVRAM([regionCode, pThis](HRESULT result, nlohmann::json& json) -> HRESULT {
                    auto&& data = json["data"];
                    if (!data.is_array() || data.size() != 1)
                    {
                        return S_OK;
                    }
                    auto&& prefectureJson = data[0];
                    if (!prefectureJson.is_number_integer())
                    {
                        return S_OK;
                    }
                    auto prefecture = prefectureJson.get<int>();
                    for (auto i = 0; i < _countof(regionList); i++)
                    {
                        if (regionList[i].prefecture == prefecture && regionList[i].regioncode == regionCode)
                        {
                            SendDlgItemMessageW(pThis->hDlg, IDC_COMBO_REGION, CB_SETCURSEL, i, 0) ;
                        }
                    }
                    EnableWindow(GetDlgItem(pThis->hDlg, IDC_COMBO_REGION), TRUE);
                    return S_OK;
                }, "nvram://receiverinfo/prefecture", "U:1B");
                return S_OK;
            }, "nvram://receiverinfo/regioncode", "U:2B");
        }
        return 1;
    }
    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (LOWORD(wParam) == IDOK && IsWindowEnabled(GetDlgItem(hDlg, IDC_EDIT_ZIP)))
            {
                WCHAR zip[100]{};
                GetDlgItemTextW(hDlg, IDC_EDIT_ZIP, zip, 100);
                std::string buf;
                buf.reserve(8);
                WCHAR invalidChar = 0;
                // 半角数字以外の数字とかも入力できるのでいい感じにする
                for (auto i = 0; zip[i]; i++)
                {
                    auto c = zip[i];
                    if (c == L'-' || c == L'ー' || c == L'－')
                    {
                        continue;
                    }
                    if (c >= L'０' && c <= L'９')
                    {
                        c -= L'０';
                        c += L'0';
                    }
                    if (c >= L'0' && c <= L'9')
                    {
                        buf.push_back((char)c);
                    }
                    else
                    {
                        invalidChar = c;
                        break;
                    }
                }
                if (invalidChar)
                {
                    MessageBoxW(hDlg, (std::wstring(L"郵便番号に不正な文字「") + invalidChar + L"」が含まれています。").c_str(), L"NVRAMの設定", MB_ICONERROR | MB_OK);
                    return 1;
                }
                else if (buf.length() != 7 && buf.length() != 0)
                {
                    MessageBoxW(hDlg, L"郵便番号は7桁の数字または未入力である必要があります。", L"NVRAMの設定", MB_ICONERROR | MB_OK);
                    return 1;
                }
                if (pThis->webView)
                {
                    nlohmann::json msg{
                        { "type", "nvramWrite" },
                        { "filename", "nvram://receiverinfo/zipcode" },
                        { "structure", "S:7B" },
                        { "data", { buf } },
                    };
                    std::ostringstream ss;
                    ss << msg;
                    auto ws = utf8StrToWString(ss.str().c_str());
                    pThis->webView->PostWebMessageAsJson(ws.c_str());
                    auto regionIndex = SendDlgItemMessageW(hDlg, IDC_COMBO_REGION, CB_GETCURSEL, 0, 0);
                    if (regionIndex >= 0 && regionIndex < _countof(regionList))
                    {
                        auto&& region = regionList[regionIndex];
                        {
                            nlohmann::json msg{
                                { "type", "nvramWrite" },
                                { "filename", "nvram://receiverinfo/prefecture" },
                                { "structure", "U:1B" },
                                { "data", { region.prefecture } },
                            };
                            std::ostringstream ss;
                            ss << msg;
                            auto ws = utf8StrToWString(ss.str().c_str());
                            pThis->webView->PostWebMessageAsJson(ws.c_str());
                        }
                        {
                            nlohmann::json msg{
                                { "type", "nvramWrite" },
                                { "filename", "nvram://receiverinfo/regioncode" },
                                { "structure", "U:2B" },
                                { "data", { region.regioncode } },
                            };
                            std::ostringstream ss;
                            ss << msg;
                            auto ws = utf8StrToWString(ss.str().c_str());
                            pThis->webView->PostWebMessageAsJson(ws.c_str());
                        }
                    }
                }
            }
            EndDialog(hDlg, LOWORD(wParam));
        }
        else if (LOWORD(wParam) == IDC_BUTTON_DELETE_NVRAM)
        {
            if (MessageBoxW(hDlg, L"保存領域をすべて削除しますか?", L"TVTDataBroadcastingHybrid", MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                nlohmann::json msg{
                    { "type", "nvramDelete" },
                };
                std::ostringstream ss;
                ss << msg;
                auto ws = utf8StrToWString(ss.str().c_str());
                pThis->webView->PostWebMessageAsJson(ws.c_str());
                EndDialog(hDlg, IDCANCEL);
            }
            else
            {
                return 1;
            }
        }
        return 1;
    }
    case WM_CLOSE:
    {
        EndDialog(hDlg, IDCANCEL);
        return 1;
    }
    case WM_DESTROY:
    {
        return 1;
    }
    }
    return 0;
}
