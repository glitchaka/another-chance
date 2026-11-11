#include "net/HttpClient.h"
#include "util/Url.h"
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <vector>
#include <string>

namespace ac::net {
namespace {
struct InternetHandle {
    HINTERNET handle = nullptr;
    explicit InternetHandle(HINTERNET value) : handle(value) {}
    ~InternetHandle() { if (handle) WinHttpCloseHandle(handle); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    operator HINTERNET() const { return handle; }
};
std::wstring ErrorCode(const wchar_t* where) {
    return std::wstring(where) + L" (Windows " +
        std::to_wstring(GetLastError()) + L")";
}
std::wstring QueryStringHeader(HINTERNET request, DWORD flag) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, flag, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return {};
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, flag, WINHTTP_HEADER_NAME_BY_INDEX,
                             data.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) return {};
    return data.data();
}
} // namespace

Response HttpClient::Get(const std::wstring& url, size_t maxBytes) const {
    return Request(url,L"GET",{},maxBytes);
}

Response HttpClient::Post(const std::wstring& url,
                          const std::string& urlEncodedBody,
                          size_t maxBytes) const {
    if (urlEncodedBody.size() > 64 * 1024) {
        Response rejected;
        rejected.url = url;
        rejected.error = L"El formulario supera el límite de envío.";
        return rejected;
    }
    return Request(url,L"POST",urlEncodedBody,maxBytes);
}

Response HttpClient::Request(const std::wstring& url,
                             const wchar_t* method,
                             const std::string& urlEncodedBody,
                             size_t maxBytes) const {
    Response response;
    response.url = url;
    if (!ac::url::IsWeb(url)) {
        response.error = L"Solo se admiten direcciones HTTP y HTTPS.";
        return response;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        response.error = ErrorCode(L"URL no válida");
        return response;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        response.error = L"Esquema de URL no admitido.";
        return response;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path = components.dwUrlPathLength ?
        std::wstring(components.lpszUrlPath, components.dwUrlPathLength) : L"/";
    if (components.dwExtraInfoLength) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    InternetHandle session(WinHttpOpen(
        L"AnotherChance/0.1 (Windows; motor original)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) {
        response.error = ErrorCode(L"No se pudo abrir WinHTTP");
        return response;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 10000);

    InternetHandle connection(WinHttpConnect(session, host.c_str(),
        components.nPort, 0));
    if (!connection.handle) {
        response.error = ErrorCode(L"No se pudo conectar");
        return response;
    }

    const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    InternetHandle request(WinHttpOpenRequest(connection, method, path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.handle) {
        response.error = ErrorCode(L"No se pudo crear la solicitud");
        return response;
    }

    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP |
                          WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION,
                     &decompression, sizeof(decompression));
    std::wstring headers =
        L"Accept: text/html,application/xhtml+xml,text/plain,*/*;q=0.5\r\n"
        L"Accept-Language: es-CL,es;q=0.9,en;q=0.6\r\n";
    const bool post = std::wstring_view(method) == L"POST";
    if (post) headers +=
        L"Content-Type: application/x-www-form-urlencoded; charset=UTF-8\r\n";
    LPVOID body = post && !urlEncodedBody.empty() ?
        const_cast<char*>(urlEncodedBody.data()) : WINHTTP_NO_REQUEST_DATA;
    const DWORD bytes = post ? static_cast<DWORD>(urlEncodedBody.size()) : 0;
    if (!WinHttpSendRequest(request, headers.c_str(),
                            static_cast<DWORD>(-1L),
                            body, bytes, bytes, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        response.error = ErrorCode(L"Error al solicitar la página");
        return response;
    }

    DWORD status = 0;
    DWORD length = sizeof(status);
    if (WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
            WINHTTP_NO_HEADER_INDEX)) {
        response.status = status;
    }
    response.contentType = QueryStringHeader(request, WINHTTP_QUERY_CONTENT_TYPE);

    DWORD urlBytes = 0;
    WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &urlBytes);
    if (urlBytes >= sizeof(wchar_t)) {
        std::vector<wchar_t> actual(urlBytes / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, actual.data(),
                               &urlBytes)) {
            response.url = actual.data();
        }
    }

    std::array<char, 16384> chunk{};
    while (true) {
        DWORD count = 0;
        if (!WinHttpReadData(request, chunk.data(),
                             static_cast<DWORD>(chunk.size()), &count)) {
            response.error = ErrorCode(L"Error al leer la respuesta");
            return response;
        }
        if (count == 0) break;
        if (response.body.size() + count > maxBytes) {
            response.error = L"Respuesta demasiado grande para esta versión.";
            response.body.clear();
            return response;
        }
        response.body.append(chunk.data(), count);
    }
    return response;
}
} // namespace ac::net
