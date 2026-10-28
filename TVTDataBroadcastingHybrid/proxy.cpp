#include "pch.h"
#include "proxy.h"

ProxyRequest::ProxyRequest
(
    HINTERNET connect,
    HINTERNET request,
    std::function<void()> errorCallback,
    std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback,
    std::vector<BYTE> payload
) : connect(connect), request(request), errorCallback(std::move(errorCallback)), callback(std::move(callback)), payload(std::move(payload))
{

}

void CALLBACK ProxyRequest::StaticAsyncCallback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
    auto self = (ProxyRequest*)dwContext;
    if (self)
    {
        self->AsyncCallback(hInternet, dwInternetStatus, lpvStatusInformation, dwStatusInformationLength);
    }
}

void ProxyRequest::Fail()
{
    if (errorCallback)
    {
        std::exchange(errorCallback, nullptr)();
        Close();
    }
}

void ProxyRequest::AsyncCallback(HINTERNET hInternet, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
    switch (dwInternetStatus)
    {
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    {
        if (!WinHttpReceiveResponse(hInternet, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
    {
        if (!WinHttpQueryDataAvailable(hInternet, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
    {
        auto const size = *(DWORD*)lpvStatusInformation;
        if (size == 0)
        {
            DWORD statusCode = 0, statusCodeSize = sizeof(statusCode);
            if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusCodeSize, nullptr))
            {
                Fail();
                return;
            }
            DWORD statusTextSize;
            std::unique_ptr<WCHAR[]> statusText;
            if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_TEXT, nullptr, WINHTTP_NO_OUTPUT_BUFFER, &statusTextSize, nullptr) && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                statusText = std::unique_ptr<WCHAR[]>(new WCHAR[statusTextSize / sizeof(WCHAR)]);
                if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_TEXT, nullptr, statusText.get(), &statusTextSize, nullptr))
                {
                    Fail();
                    return;
                }
            }
            DWORD headersSize;
            std::unique_ptr<WCHAR[]> headers;
            if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &headersSize, WINHTTP_NO_HEADER_INDEX) && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                headers = std::unique_ptr<WCHAR[]>(new WCHAR[headersSize / sizeof(WCHAR)]);
                if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, headers.get(), &headersSize, WINHTTP_NO_HEADER_INDEX))
                {
                    Fail();
                    return;
                }
            }
            callback(statusCode, statusText.get(), headers.get(), data.size(), data.data());
            Close();
            break;
        }
        auto prevSize = data.size();
        data.resize(prevSize + size);
        if (!WinHttpReadData(hInternet, data.data() + prevSize, size, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_FLAG_READ_COMPLETE:
    {
        if (!WinHttpQueryDataAvailable(hInternet, nullptr))
        {
            Fail();
            return;
        }
        break;
    }
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
    {
        auto asyncResut = (LPWINHTTP_ASYNC_RESULT*)lpvStatusInformation;
        Fail();
        break;
    }
    }
}

void ProxyRequest::Close()
{
    delete this;
}

bool ProxyRequest::RequestAsync
(
    ProxySession& session,
    LPCWSTR url,
    LPCWSTR verb,
    std::vector<BYTE> payload,
    std::vector<std::pair<LPCWSTR, LPCWSTR>>& headers,
    std::function<void()> errorCallback,
    std::function<void(DWORD statusCode, LPCWSTR statusCodeText, LPCWSTR headers, size_t contentLength, BYTE* content)> callback
)
{
    URL_COMPONENTSW components = { sizeof(URL_COMPONENTSW) };
    components.dwHostNameLength = 1;
    components.dwUrlPathLength = 1;
    if (!WinHttpCrackUrl(url, 0, 0, &components))
    {
        return false;
    }
    std::wstring hostName(components.lpszHostName, components.lpszHostName + components.dwHostNameLength);
    std::wstring urlPath(components.lpszUrlPath, components.lpszUrlPath + components.dwUrlPathLength);
    auto connect = WinHttpConnect(session.GetSession(), hostName.c_str(), components.nPort, 0);
    if (!connect)
    {
        return false;
    }
    LPCWSTR acceptTypes[] = { L"*/*", nullptr };
    auto request = WinHttpOpenRequest(connect, verb, urlPath.c_str(), nullptr, WINHTTP_NO_REFERER, acceptTypes, components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        return false;
    }
    DWORD securityFlags = SECURITY_FLAG_IGNORE_ALL_CERT_ERRORS;
    WinHttpSetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &securityFlags, sizeof(securityFlags));
    std::unique_ptr<ProxyRequest> preq(new ProxyRequest(connect, request, std::move(errorCallback), std::move(callback), std::move(payload)));
    if (WinHttpSetStatusCallback(request, StaticAsyncCallback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
    {
        return false;
    }
    for (auto&& header : headers)
    {
        auto headers = std::wstring(header.first) + L": " + header.second;
        // CRLFが含まれている不正なヘッダを除外
        // CRLFの直後に空白かタブがあればヘッダの区切りではなくトークンの区切りとして扱われるためこの処理は正しくない
        // ただし含めたリクエストを送る処理はないしfetchに含めることもできないし問題ない
        if (headers.find(L'\r') != std::wstring::npos || headers.find(L'\n') != std::wstring::npos)
        {
            continue;
        }
        if (!WinHttpAddRequestHeaders(request, headers.data(), headers.size(), WINHTTP_ADDREQ_FLAG_ADD))
        {
            return false;
        }
    }
    auto payloadSize = preq->payload.size();
    if (payloadSize > std::numeric_limits<DWORD>::max())
    {
        return false;
    }
    WCHAR additionalHeader[100];
    if (payloadSize != 0)
    {
        swprintf_s(additionalHeader, L"Content-Length: %zu", payloadSize);
    }
    if (!WinHttpSendRequest(request, payloadSize == 0 ? WINHTTP_NO_ADDITIONAL_HEADERS : additionalHeader, -1, preq->payload.data(), payloadSize, WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, (DWORD_PTR)preq.get()))
    {
        return false;
    }
    // AsyncCallback中でdelete thisされる
    preq.release();
    return true;
}

ProxyRequest::~ProxyRequest()
{
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
}

ProxySession::ProxySession()
{
    this->session = WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
}

ProxySession::~ProxySession()
{
    WinHttpCloseHandle(this->session);
}

HINTERNET ProxySession::GetSession()
{
    return this->session;
}
