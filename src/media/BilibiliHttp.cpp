#include "BilibiliHttp.h"

#include <windows.h>
#include <winhttp.h>

#include <format>

#pragma comment(lib, "winhttp.lib")

namespace veyra::media::bili {
namespace {

constexpr wchar_t kUserAgent[] =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    L"(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

// Every request in this module is one round-trip; 10 s per phase is long enough
// for a slow link and short enough that a stuck request does not hold the player
// for half a minute (the future blocks the UI thread on exit while it runs).
constexpr int kTimeoutMs = 10000;

HINTERNET openSession() {
    HINTERNET session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return nullptr;
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    // See the header: the danmaku CDN always sends gzip.
    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(session, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
    return session;
}

} // namespace

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n);
    return out;
}

bool httpGet(const std::wstring& host, const std::wstring& path, const std::wstring& referer,
             const std::wstring& cookie, std::string& body, std::wstring& error) {
    HINTERNET session = openSession();
    if (!session) { error = L"WinHttpOpen 失败"; return false; }

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect) {
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request) {
            WinHttpAddRequestHeaders(request, (L"User-Agent: " + std::wstring(kUserAgent)).c_str(), -1L,
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            if (!referer.empty())
                WinHttpAddRequestHeaders(request, (L"Referer: " + referer).c_str(), -1L,
                                         WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            // The user's own session: member renditions and CC subtitles are only
            // served to it.
            if (!cookie.empty())
                WinHttpAddRequestHeaders(request, (L"Cookie: " + cookie).c_str(), -1L,
                                         WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                const bool haveStatus = WinHttpQueryHeaders(
                    request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) != FALSE;
                if (!haveStatus) {
                    error = L"没有返回状态码";
                } else if (status == 200) {
                    // Custom: a failed read mid-body used to fall through to ok=true,
                    // so a truncated JSON/XML response was reported as success and then
                    // surfaced as a confusing "错误码 -1" or a silently short danmaku
                    // track. Only a clean end of body counts as success now.
                    DWORD available = 0;
                    bool readFailed = false;
                    do {
                        available = 0;
                        if (!WinHttpQueryDataAvailable(request, &available)) { readFailed = true; break; }
                        if (available == 0) break;
                        std::string chunk(available, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(request, chunk.data(), available, &read)) { readFailed = true; break; }
                        body.append(chunk.data(), read);
                    } while (available > 0);
                    if (readFailed) error = L"响应读取中断（内容不完整）";
                    else ok = true;
                } else {
                    error = std::format(L"HTTP {}", status);
                }
            } else {
                error = L"请求失败";
            }
            WinHttpCloseHandle(request);
        } else {
            error = L"WinHttpOpenRequest 失败";
        }
        WinHttpCloseHandle(connect);
    } else {
        error = L"WinHttpConnect 失败";
    }
    WinHttpCloseHandle(session);
    return ok;
}

bool httpFollowLocation(const std::wstring& host, const std::wstring& path,
                        std::wstring& location, std::wstring& error) {
    HINTERNET session = openSession();
    if (!session) { error = L"WinHttpOpen 失败"; return false; }
    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect) {
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request) {
            // Custom: WinHTTP follows redirects by default, so WinHttpReceiveResponse
            // would land on the final 200 page and the Location header of the 302
            // would already be gone - which is exactly why every b23.tv link failed
            // here with "短链没有返回跳转地址". Turn automatic redirection off and
            // read the 3xx response ourselves.
            DWORD disable = WINHTTP_DISABLE_REDIRECTS;
            WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
            WinHttpAddRequestHeaders(request, (L"User-Agent: " + std::wstring(kUserAgent)).c_str(), -1L,
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, statusSize = sizeof(status);
                const bool haveStatus = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                                            WINHTTP_NO_HEADER_INDEX) != FALSE;
                if (!haveStatus) {
                    error = L"短链请求没有返回状态码";
                } else if (status < 300 || status >= 400) {
                    error = std::format(L"短链返回 {}，不是跳转响应", status);
                } else {
                    DWORD size = 0;
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                        nullptr, &size, WINHTTP_NO_HEADER_INDEX);
                    if (size > sizeof(wchar_t)) {
                        std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
                        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                                                WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                                                WINHTTP_NO_HEADER_INDEX)) {
                            value.resize(wcslen(value.c_str()));
                            location = value;
                            ok = !location.empty();
                        }
                    }
                    if (!ok) error = std::format(L"短链的 {} 响应里没有 Location", status);
                }
            } else {
                error = L"请求失败";
            }
            WinHttpCloseHandle(request);
        } else {
            error = L"WinHttpOpenRequest 失败";
        }
        WinHttpCloseHandle(connect);
    } else {
        error = L"WinHttpConnect 失败";
    }
    WinHttpCloseHandle(session);
    return ok;
}

} // namespace veyra::media::bili
