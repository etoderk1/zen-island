// update.cpp — см. update.h. Сеть — WinHTTP с системным прокси (как и всё остальное в острове).
#include "update.h"
#include "net.h"
#include "version.h"
#include <winhttp.h>
#include <thread>
#include <atomic>
#include <vector>

static std::atomic<bool> g_checking{false}, g_downloading{false};

bool VersionNewer(const std::wstring& a, const std::wstring& b) {
    auto parse = [](const std::wstring& s) {
        std::vector<int> v;
        int cur = -1;
        for (wchar_t c : s) {
            if (c >= L'0' && c <= L'9') cur = (cur < 0 ? 0 : cur) * 10 + (c - L'0');
            else if (cur >= 0) { v.push_back(cur); cur = -1; }
        }
        if (cur >= 0) v.push_back(cur);
        while (v.size() < 3) v.push_back(0);
        return v;
    };
    auto x = parse(a), y = parse(b);
    for (size_t i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

static HINTERNET OpenSession() {
    HINTERNET s = WinHttpOpen(L"ZenIsland-Updater/" ZEN_VERSION, 4 /*AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"ZenIsland-Updater/" ZEN_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 15000, 30000);
    return s;
}
// разбить https://host/path
static bool SplitUrl(const std::wstring& url, std::wstring& host, std::wstring& path) {
    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t h[256], p[2048];
    uc.lpszHostName = h;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = p;
    uc.dwUrlPathLength = 2048;
    wchar_t x[1024];
    uc.lpszExtraInfo = x;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    host.assign(h, uc.dwHostNameLength);
    path.assign(p, uc.dwUrlPathLength);
    path.append(x, uc.dwExtraInfoLength);
    return true;
}

void UpdateCheckAsync(HWND notify, const std::wstring& repo, bool manual) {
    if (repo.empty() || g_checking.exchange(true)) return;
    std::thread([notify, repo, manual] {
        UpdateInfo* u = new UpdateInfo;
        bool newer = false;
        std::string body;
        if (HINTERNET s = OpenSession()) {
            HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            std::wstring path = L"/repos/" + repo + L"/releases/latest";
            HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr;
            const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
            if (r && WinHttpSendRequest(r, hdr, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 2 * 1024 * 1024) {
                    std::string chunk(avail, '\0');
                    DWORD rd = 0;
                    if (!WinHttpReadData(r, &chunk[0], avail, &rd) || !rd) break;
                    body.append(chunk.data(), rd);
                }
                if (code == 404) u->error = L"релизов пока нет";
                else if (code != 200) u->error = L"GitHub ответил " + std::to_wstring(code);
            } else u->error = L"нет связи с GitHub";
            if (r) WinHttpCloseHandle(r);
            if (c) WinHttpCloseHandle(c);
            WinHttpCloseHandle(s);
        }
        net::JVal j;
        if (u->error.empty() && net::ParseJson(body, j)) {
            u->version = net::FromU8(j.GetStr("tag_name"));
            if (!u->version.empty() && (u->version[0] == L'v' || u->version[0] == L'V')) u->version.erase(0, 1);
            std::wstring notes = net::FromU8(j.GetStr("body"));
            size_t cut = 0;
            for (int line = 0; line < 4 && cut != std::wstring::npos; line++) cut = notes.find(L'\n', cut + 1);
            u->notes = notes.substr(0, cut == std::wstring::npos ? notes.size() : cut);
            if (const net::JVal* as = j.Get("assets"))
                for (auto& a : as->a) {
                    std::string n = a.GetStr("name");
                    if (n.size() > 9 && n.find("Setup") != std::string::npos && n.compare(n.size() - 4, 4, ".exe") == 0)
                        u->url = net::FromU8(a.GetStr("browser_download_url"));
                }
            newer = !u->url.empty() && VersionNewer(u->version, ZEN_VERSION);
            if (!newer && u->url.empty() && !u->version.empty() && VersionNewer(u->version, ZEN_VERSION)) u->error = L"в релизе нет установщика";
        } else if (u->error.empty()) u->error = L"не понял ответ GitHub";
        g_checking = false;
        if (!newer && !manual) { delete u; return; }
        if (!PostMessageW(notify, WM_APP_UPDATE, newer ? 1 : 0, (LPARAM)u)) delete u;
    }).detach();
}

void UpdateDownloadAsync(HWND notify, const std::wstring& url) {
    if (g_downloading.exchange(true)) return;
    std::thread([notify, url] {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring file = std::wstring(tmp) + L"ZenIsland-Setup.exe";
        bool ok = false;
        std::wstring host, path;
        if (HINTERNET s = SplitUrl(url, host, path) ? OpenSession() : nullptr) {
            HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
            HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr;
            // GitHub отдаёт файл через переадресацию на свой CDN — WinHTTP идёт по ней сам
            if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
                DWORD code = 0, sz = sizeof(code);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
                DWORD total = 0;
                sz = sizeof(total);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &total, &sz, nullptr);
                HANDLE f = code == 200 ? CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr) : INVALID_HANDLE_VALUE;
                if (f != INVALID_HANDLE_VALUE) {
                    DWORD got = 0, avail = 0;
                    int lastPct = -1;
                    std::vector<char> buf(64 * 1024);
                    ok = true;
                    while (WinHttpQueryDataAvailable(r, &avail) && avail) {
                        DWORD rd = 0, wr = 0;
                        if (!WinHttpReadData(r, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &rd) || !rd) { ok = false; break; }
                        if (!WriteFile(f, buf.data(), rd, &wr, nullptr) || wr != rd) { ok = false; break; }
                        got += rd;
                        int pct = total ? (int)(got * 100.0 / total) : 0;
                        if (pct != lastPct && pct < 100) { lastPct = pct; PostMessageW(notify, WM_APP_UPDDL, pct, 0); }
                    }
                    CloseHandle(f);
                    if (total && got != total) ok = false;
                    if (got < 100 * 1024) ok = false;  // явно не установщик
                }
            }
            if (r) WinHttpCloseHandle(r);
            if (c) WinHttpCloseHandle(c);
            WinHttpCloseHandle(s);
        }
        g_downloading = false;
        if (!ok) { DeleteFileW(file.c_str()); PostMessageW(notify, WM_APP_UPDDL, 102, 0); return; }
        std::wstring* p = new std::wstring(file);
        if (!PostMessageW(notify, WM_APP_UPDDL, 101, (LPARAM)p)) delete p;
    }).detach();
}
