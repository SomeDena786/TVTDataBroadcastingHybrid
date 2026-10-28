#pragma once
#include <winhttp.h>

class ProxySession
{
    HINTERNET session;
public:
    HINTERNET GetSession();
    ProxySession();
    ~ProxySession();
    ProxySession(const ProxySession&) = delete;
    ProxySession& operator=(const ProxySession&) = delete;
};

class ProxyRequest
{
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    // ストリーミングさせるのは面倒かつ必要がないので一旦全て読み込んでから返す
    std::vector<BYTE> data;
    std::function<void()> errorCallback;
    std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback;
    std::vector<BYTE> payload;
    ProxyRequest
    (
        HINTERNET connect,
        HINTERNET request,
        std::function<void()> errorCallback,
        std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback,
        std::vector<BYTE> payload
    );
    static void CALLBACK StaticAsyncCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength);
    void Fail();
    void AsyncCallback(HINTERNET hInternet, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength);
    void Close();
public:
    // 非同期HTTPリクエストを行う
    // 完了すると別スレッドでcallbackが呼ばれる
    static bool RequestAsync
    (
        ProxySession& session,
        LPCWSTR url,
        LPCWSTR verb,
        std::vector<BYTE> payload,
        std::vector<std::pair<LPCWSTR, LPCWSTR>>& headers,
        std::function<void()> errorCallback,
        std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback
    );
    ~ProxyRequest();
    ProxyRequest(const ProxyRequest&) = delete;
    ProxyRequest& operator=(const ProxyRequest&) = delete;
};
