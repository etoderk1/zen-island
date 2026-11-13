// cmdindex.cpp — индекс программ и файлов, курсы валют
#include "cmdindex.h"
#include "cmdlogic.h"
#include "net.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <thread>
#include <atomic>
#include <algorithm>

static std::atomic<bool> g_building{false}, g_fetching{false};

static void AddApps(std::vector<CmdEntry>& out) {
    IShellItem* folder = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder))) || !folder) return;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en))) && en) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK && it) {
            LPWSTR name = nullptr, parse = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name &&
                SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parse)) && parse) {
                CmdEntry e;
                e.name = name;
                e.lower = LowerW(e.name);
                e.target = std::wstring(L"shell:AppsFolder\\") + parse;
                e.kind = CE_APP;
                // деинсталляторы и справку прячем
                bool junk = e.lower.find(L"uninstall") != std::wstring::npos || e.lower.find(L"удалить") != std::wstring::npos ||
                            e.lower.find(L"удаление") != std::wstring::npos;
                if (!junk) out.push_back(std::move(e));
            }
            if (name) CoTaskMemFree(name);
            if (parse) CoTaskMemFree(parse);
            it->Release();
            it = nullptr;
            if (out.size() > 3000) break;
        }
        en->Release();
    }
    folder->Release();
}

static bool SkipDir(const wchar_t* n) {
    static const wchar_t* bad[] = {L"node_modules", L".git", L".svn", L"__pycache__", L".gradle", L".idea", L".vs", L"build",
                                   L"target", L"cache", L"Cache", L"$RECYCLE.BIN", L"System Volume Information", L"AppData",
                                   L".cache", L"venv", L".venv", L"obj", L"bin"};
    if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) return true;
    for (auto b : bad) if (_wcsicmp(n, b) == 0) return true;
    return false;
}
static void Walk(const std::wstring& dir, int depth, std::vector<CmdEntry>& out, size_t cap) {
    if (out.size() >= cap) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> sub;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0 || fd.cFileName[1] == L'.')) continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir && SkipDir(fd.cFileName)) continue;
        CmdEntry e;
        e.name = fd.cFileName;
        e.lower = LowerW(e.name);
        e.target = dir + L"\\" + fd.cFileName;
        e.sub = dir;
        e.kind = isDir ? CE_FOLDER : CE_FILE;
        e.mtime = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (!isDir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        out.push_back(std::move(e));
        if (isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) sub.push_back(dir + L"\\" + fd.cFileName);
        if (out.size() >= cap) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (depth > 0)
        for (auto& s : sub) Walk(s, depth - 1, out, cap);
}
static std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}

// запасной путь: ярлыки меню «Пуск» (если папка AppsFolder недоступна)
static void WalkLnk(const std::wstring& dir, int depth, std::vector<CmdEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth > 0) WalkLnk(p, depth - 1, out); continue; }
        std::wstring n = fd.cFileName;
        size_t dot = n.rfind(L'.');
        if (dot == std::wstring::npos || (_wcsicmp(n.c_str() + dot, L".lnk") && _wcsicmp(n.c_str() + dot, L".url"))) continue;
        CmdEntry e;
        e.name = n.substr(0, dot);
        e.lower = LowerW(e.name);
        if (e.lower.find(L"uninstall") != std::wstring::npos || e.lower.find(L"удал") != std::wstring::npos) continue;
        e.target = p;
        e.kind = CE_APP;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd) && out.size() < 3000);
    FindClose(h);
}
static std::wstring Known(REFKNOWNFOLDERID id);
void CmdIndexBuildAsync(HWND notify, const std::vector<std::wstring>& extraDirs) {
    if (g_building.exchange(true)) return;
    std::vector<std::wstring> extra = extraDirs;
    std::thread([notify, extra] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CmdIndexData* d = new CmdIndexData;
        AddApps(d->apps);
        if (d->apps.size() < 5) {
            WalkLnk(Known(FOLDERID_CommonPrograms), 3, d->apps);
            WalkLnk(Known(FOLDERID_Programs), 3, d->apps);
        }
        std::sort(d->apps.begin(), d->apps.end(), [](const CmdEntry& a, const CmdEntry& b) { return a.lower < b.lower; });
        d->apps.erase(std::unique(d->apps.begin(), d->apps.end(), [](const CmdEntry& a, const CmdEntry& b) { return a.lower == b.lower; }),
                      d->apps.end());
        const size_t cap = 60000;
        std::vector<std::wstring> roots = {Known(FOLDERID_Desktop), Known(FOLDERID_Downloads), Known(FOLDERID_Documents),
                                           Known(FOLDERID_Pictures), Known(FOLDERID_Videos), Known(FOLDERID_Music)};
        for (auto& r : roots) if (!r.empty()) Walk(r, 4, d->files, cap);
        for (auto& r : extra) if (!r.empty()) Walk(r, 3, d->files, cap);
        CoUninitialize();
        g_building = false;
        if (!PostMessageW(notify, WM_APP_INDEX, 0, (LPARAM)d)) delete d;
    }).detach();
}

// ---------------------------------------------------------------- курсы
static bool ParseRates(const std::string& body, std::map<std::string, double>& out) {
    net::JVal j;
    if (!net::ParseJson(body, j)) return false;
    const net::JVal* r = j.Get("rates");
    if (!r || r->t != net::JVal::Obj) return false;
    for (auto& kv : r->o)
        if (kv.second.t == net::JVal::Num && kv.second.n > 0) out[kv.first] = kv.second.n;
    return out.count("USD") && out.count("RUB");
}
void RatesFetchAsync(HWND notify, const std::wstring& cacheFile) {
    if (g_fetching.exchange(true)) return;
    std::thread([notify, cacheFile] {
        std::string cached;
        bool fresh = false;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(cacheFile.c_str(), GetFileExInfoStandard, &fa)) {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            long long a = ((long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
            long long b = ((long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
            fresh = b - a < 6LL * 3600 * 10000000LL;
            HANDLE f = CreateFileW(cacheFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD sz = GetFileSize(f, nullptr), rd = 0;
                if (sz < (4u << 20)) { cached.resize(sz); ReadFile(f, &cached[0], sz, &rd, nullptr); cached.resize(rd); }
                CloseHandle(f);
            }
        }
        auto* m = new std::map<std::string, double>;
        bool ok = !cached.empty() && ParseRates(cached, *m);
        if (!ok || !fresh) {
            DWORD st = 0;
            std::string body;
            std::map<std::string, double> nm;
            if (net::HttpGet(L"https://open.er-api.com/v6/latest/USD", st, body, 8000) && st == 200 && ParseRates(body, nm)) {
                *m = nm;
                ok = true;
                HANDLE f = CreateFileW(cacheFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f, body.data(), (DWORD)body.size(), &wr, nullptr); CloseHandle(f); }
            }
        }
        g_fetching = false;
        if (!ok) { delete m; return; }
        if (!PostMessageW(notify, WM_APP_RATES, 0, (LPARAM)m)) delete m;
    }).detach();
}
