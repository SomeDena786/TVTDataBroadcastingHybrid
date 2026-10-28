// Cプロファイル(ワンセグ)のデータ放送を表示するウィンドウ

#include "pch.h"
#include "OneSeg.h"
#include <ShellScalingApi.h>

static decltype(&AdjustWindowRectExForDpi) pAdjustWindowRectExForDpi = (decltype(&AdjustWindowRectExForDpi))GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
static decltype(&GetDpiForWindow) pGetDpiForWindow = (decltype(&GetDpiForWindow))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
static decltype(&GetDpiForSystem) pGetDpiForSystem = (decltype(&GetDpiForSystem))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");

static UINT GetDpi()
{
    if (pGetDpiForSystem)
    {
        return pGetDpiForSystem();
    }
    HDC hDC = GetDC(nullptr);
    auto dpiY = GetDeviceCaps(hDC, LOGPIXELSY);
    ReleaseDC(nullptr, hDC);
    return dpiY;
}

// TODO: 別翻訳単位に
UINT GetDpi(HWND hWnd)
{
    if (pGetDpiForWindow)
    {
        return pGetDpiForWindow(hWnd);
    }
    auto monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONULL);
    UINT dpiX, dpiY;
    if (monitor == nullptr || FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
    {
        return GetDpi();
    }
    return dpiY;
}

static LONG GetRectWidth(RECT* rect)
{
    return rect->right - rect->left;
}

static LONG GetRectHeight(RECT* rect)
{
    return rect->bottom - rect->top;
}

static void AdjustWindow(HWND hWnd, int scaledWidth, int scaledHeight, UINT dpi, int x, int y, UINT swpFlags)
{
    RECT currentClient;
    if (!GetClientRect(hWnd, &currentClient))
    {
        return;
    }
    if (GetRectWidth(&currentClient) == scaledWidth && GetRectHeight(&currentClient) == scaledHeight)
    {
        return;
    }
    if (pAdjustWindowRectExForDpi)
    {
        RECT rect = { 0, 0, scaledWidth, scaledHeight };
        if (!pAdjustWindowRectExForDpi(&rect, GetWindowLongW(hWnd, GWL_STYLE), GetMenu(hWnd) != nullptr, GetWindowLongW(hWnd, GWL_EXSTYLE), dpi))
        {
            return;
        }
        if (!SetWindowPos(hWnd, nullptr, x, y, GetRectWidth(&rect), GetRectHeight(&rect), SWP_NOACTIVATE | SWP_NOZORDER | swpFlags))
        {
            return;
        }
    }
    else
    {
        RECT ncRect = { 0, 0, 0, 0 };
        auto systemDpi = GetDpi();
        if (!AdjustWindowRectEx(&ncRect, GetWindowLongW(hWnd, GWL_STYLE), GetMenu(hWnd) != nullptr, GetWindowLongW(hWnd, GWL_EXSTYLE)))
        {
            return;
        }
        auto ncWidth = MulDiv(GetRectWidth(&ncRect), dpi, systemDpi);
        auto ncHeight = MulDiv(GetRectHeight(&ncRect), dpi, systemDpi);
        if (!SetWindowPos(hWnd, nullptr, x, y, ncWidth + scaledWidth, ncHeight + scaledHeight, SWP_NOACTIVATE | SWP_NOZORDER | swpFlags))
        {
            return;
        }
        // AdjustWindowRectExForDpiと違って2pxくらいずれることがある
    }
    RECT adjustedClient;
    RECT adjustedNonClient;
    if (!GetClientRect(hWnd, &adjustedClient))
    {
        return;
    }
    if (GetRectWidth(&adjustedClient) == scaledWidth && GetRectHeight(&adjustedClient) == scaledHeight)
    {
        return;
    }
    if (!GetWindowRect(hWnd, &adjustedNonClient))
    {
        return;
    }
    auto ncWidth = GetRectWidth(&adjustedNonClient) - GetRectWidth(&adjustedClient);
    auto ncHeight = GetRectHeight(&adjustedNonClient) - GetRectHeight(&adjustedClient);
    SetWindowPos(hWnd, nullptr, 0, 0, ncWidth + scaledWidth, ncHeight + scaledHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | swpFlags);
}

const int scaleList[] =
{
    100,
    125,
    150,
    175,
    200,
    225,
    250,
    300,
    350,
    400,
};

const int SCALE_INVALID = 0;

const int CPROFILE_WIDTH = 240;
const int CPROFILE_HEIGHT = 480;

LRESULT OneSegWindow::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // テーマの変更などで非クライアント領域の大きさが変わる可能性はあるけどDPIの変更と比べると稀なので考慮しない
    switch (uMsg)
    {
    case WM_THEMECHANGED:
    {
        if (this->scale != SCALE_INVALID)
        {
            CheckMenuItem(GetMenu(hWnd), this->scale, MF_BYCOMMAND | MF_UNCHECKED);
            CheckMenuRadioItem(GetMenu(hWnd), scaleList[0], scaleList[_countof(scaleList) - 1], this->scale, MF_BYCOMMAND);
            AdjustWindow(hWnd, CPROFILE_WIDTH * this->scale / 100, CPROFILE_HEIGHT * this->scale / 100, this->dpi, 0, 0, SWP_NOMOVE);
        }
        break;
    }
    case WM_DPICHANGED:
    {
        RECT* newRect = (RECT*)lParam;
        auto dpi = HIWORD(wParam);
        WINDOWPLACEMENT placement = { sizeof(placement) };
        if (GetWindowPlacement(hWnd, &placement))
        {
            if (placement.showCmd == SW_MAXIMIZE)
            {
                break;
            }
        }
        if (this->scale != SCALE_INVALID)
        {
            CheckMenuItem(GetMenu(hWnd), this->scale, MF_BYCOMMAND | MF_UNCHECKED);
            this->scale = MulDiv(this->scale, dpi, this->dpi);
            this->dpi = dpi;
            CheckMenuRadioItem(GetMenu(hWnd), scaleList[0], scaleList[_countof(scaleList) - 1], this->scale, MF_BYCOMMAND);
            AdjustWindow(hWnd, CPROFILE_WIDTH * this->scale / 100, CPROFILE_HEIGHT * this->scale / 100, dpi, newRect->left, newRect->top, 0);
        }
        else
        {
            this->dpi = dpi;
            SetWindowPos(hWnd, nullptr, newRect->left, newRect->top, GetRectWidth(newRect), GetRectHeight(newRect), SWP_NOACTIVATE | SWP_NOZORDER);
        }
        break;
    }
    case WM_DESTROY:
    {
        this->hWnd = nullptr;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        DestroyMenu(GetMenu(hWnd));
        this->destroyCallback();
        break;
    }
    case WM_SIZE:
    {
        if (this->scale != SCALE_INVALID)
        {
            auto dpi = GetDpi(hWnd);
            auto expectedWidth = CPROFILE_WIDTH * this->scale / 100;
            auto expectedHeight = CPROFILE_HEIGHT * this->scale / 100;
            if (LOWORD(lParam) != expectedWidth || HIWORD(lParam) != expectedHeight)
            {
                CheckMenuItem(GetMenu(hWnd), this->scale, MF_BYCOMMAND | MF_UNCHECKED);
                this->scale = SCALE_INVALID;
            }
        }
        if (this->scale == SCALE_INVALID)
        {
            auto dpi = GetDpi(hWnd);
            for (auto scale : scaleList)
            {
                auto expectedWidth = CPROFILE_WIDTH * scale / 100;
                auto expectedHeight = CPROFILE_HEIGHT * scale / 100;
                if (LOWORD(lParam) == expectedWidth && HIWORD(lParam) == expectedHeight)
                {
                    this->scale = scale;
                    CheckMenuRadioItem(GetMenu(hWnd), scaleList[0], scaleList[_countof(scaleList) - 1], this->scale, MF_BYCOMMAND);
                    break;
                }
            }
        }
        this->webViewController->put_Bounds({ 0, 0, LOWORD(lParam), HIWORD(lParam) });
        break;
    }
    case WM_COMMAND:
    {
        if (HIWORD(wParam) == 0 && lParam == 0)
        {
            this->scale = LOWORD(wParam);
            CheckMenuRadioItem(GetMenu(hWnd), scaleList[0], scaleList[_countof(scaleList) - 1], LOWORD(wParam), MF_BYCOMMAND);
            WINDOWPLACEMENT placement = { sizeof(placement) };
            if (GetWindowPlacement(hWnd, &placement))
            {
                if (placement.showCmd == SW_MAXIMIZE)
                {
                    ShowWindow(hWnd, SW_RESTORE);
                }
            }
            AdjustWindow(hWnd, CPROFILE_WIDTH * this->scale / 100, CPROFILE_HEIGHT * this->scale / 100, GetDpi(hWnd), 0, 0, SWP_NOMOVE);
        }
        break;
    }
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

OneSegWindow::OneSegWindow(HWND appWindow, HINSTANCE hInstance, HWND hWebView2ContainerWnd, wil::com_ptr<ICoreWebView2Controller> webViewController, std::function<void()> destroyCallback)
    : hWebView2ContainerWnd(hWebView2ContainerWnd), webViewController(std::move(webViewController)), destroyCallback(std::move(destroyCallback))
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpfnWndProc = [](HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) -> LRESULT
    {
        auto pThis = (OneSegWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (!pThis)
        {
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
        }
        return pThis->WndProc(hWnd, uMsg, wParam, lParam);
    };
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TVTDataBroadcastingHybrid 1Seg";
    RegisterClassExW(&wc);
    HMENU oneSegMenu = CreateMenu();
    HMENU oneSegScaleMenu = CreateMenu();
    RECT appRect = {};
    GetWindowRect(appWindow, &appRect);
    int windowX = appRect.left;
    int windowY = appRect.top;
    auto dpi = GetDpi(appWindow);
    for (auto index = 1; auto size : scaleList)
    {
        WCHAR buf[sizeof(L"9999.999% (&9)")];
        swprintf_s(buf, L"%d%% (&%d)", size, index);
        AppendMenuW(oneSegScaleMenu, MF_ENABLED | MF_STRING | MFT_RADIOCHECK, size, buf);
        index++;
    }
    AppendMenuW(oneSegMenu, MF_ENABLED | MF_STRING | MF_POPUP, (UINT_PTR)oneSegScaleMenu, L"拡大率 (&S)");
    int width, height;
    auto clientWidth = MulDiv(CPROFILE_WIDTH, dpi, 96);
    auto clientHeight = MulDiv(CPROFILE_HEIGHT, dpi, 96);
    auto style = WS_POPUPWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
    auto exStyle = WS_EX_DLGMODALFRAME;
    if (pAdjustWindowRectExForDpi)
    {
        RECT rect = { 0, 0, clientWidth, clientHeight };
        pAdjustWindowRectExForDpi(&rect, style, oneSegMenu != nullptr, exStyle, dpi);
        width = GetRectWidth(&rect);
        height = GetRectHeight(&rect);
    }
    else
    {
        RECT ncRect = { 0, 0, 0, 0 };
        auto systemDpi = GetDpi();
        AdjustWindowRectEx(&ncRect, style, oneSegMenu != nullptr, exStyle);
        auto ncWidth = MulDiv(GetRectWidth(&ncRect), dpi, systemDpi);
        auto ncHeight = MulDiv(GetRectHeight(&ncRect), dpi, systemDpi);
        width = ncWidth + clientWidth;
        height = ncHeight + clientHeight;
    }
    auto oneSegWnd = CreateWindowExW(exStyle, wc.lpszClassName, L"ﾜﾝｾｸﾞ ﾃﾞｰﾀ放送", style, windowX, windowY, width, height, appWindow, oneSegMenu, hInstance, nullptr);
    ShowWindow(oneSegWnd, SW_SHOW);
    this->hWnd = oneSegWnd;
    dpi = GetDpi(oneSegWnd);
    clientWidth = MulDiv(CPROFILE_WIDTH, dpi, 96);
    clientHeight = MulDiv(CPROFILE_HEIGHT, dpi, 96);
    this->dpi = dpi;
    this->scale = dpi * 100 / 96;
    CheckMenuRadioItem(GetMenu(hWnd), scaleList[0], scaleList[_countof(scaleList) - 1], this->scale, MF_BYCOMMAND);
    AdjustWindow(oneSegWnd, clientWidth, clientHeight, GetDpi(oneSegWnd), 0, 0, SWP_NOMOVE);
    SetWindowLongPtrW(oneSegWnd, GWLP_USERDATA, (LONG_PTR)this);
    this->webViewController->put_ParentWindow(this->hWnd);
    this->webViewController->put_Bounds({ 0, 0, clientWidth, clientHeight });
}

HWND OneSegWindow::GetWindowHandle()
{
    return this->hWnd;
}

void OneSegWindow::DestroyOneSegWindow()
{
    DestroyWindow(this->hWnd);
}

OneSegWindow::~OneSegWindow()
{
    this->DestroyOneSegWindow();
}
