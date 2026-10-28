#pragma once

class OneSegWindow
{
    int scale;
    int dpi;
    HWND hWebView2ContainerWnd;
    HWND hWnd;
    wil::com_ptr<ICoreWebView2Controller> webViewController;
    std::function<void()> destroyCallback;
    LRESULT WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
public:
    void DestroyOneSegWindow();
    HWND GetWindowHandle();
    OneSegWindow(HWND appWindow, HINSTANCE hInstance, HWND hWebView2ContainerWnd, wil::com_ptr<ICoreWebView2Controller> webViewController, std::function<void()> destroyCallback);
    ~OneSegWindow();
    OneSegWindow(const OneSegWindow&) = delete;
    OneSegWindow& operator=(const OneSegWindow&) = delete;
};

UINT GetDpi(HWND hWnd);
