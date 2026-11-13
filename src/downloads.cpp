// downloads.cpp — браузеры качают во временный файл (Chrome/Edge/Яндекс: .crdownload, Firefox: .part),
// а по окончании переименовывают его. Следим за папкой и ловим оба момента.
#include "downloads.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <map>
#include <cwctype>

static const GUID kFolderDownloads = {0x374DE290, 0x123F, 0x4565, {0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B}};

std::wstring DownloadsFolder() {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(kFolderDownloads, 0, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}

static bool EndsWithI(const std::wstring& s, const wchar_t* suf, size_t* cut) {
    size_t n = wcslen(suf);
    if (s.size() <= n || _wcsicmp(s.c_str() + s.size() - n, suf) != 0) return false;
    if (cut) *cut = s.size() - n;
    return true;
}
// Временный файл загрузки? В final — имя готового файла (пусто, если ещё неизвестно)
static bool IsTemp(const std::wstring& name, std::wstring& final) {
    static const wchar_t* exts[] = {L".crdownload", L".part", L".partial", L".download", L".opdownload"};
    for (auto* e : exts) {
        size_t cut;
        if (EndsWithI(name, e, &cut)) {
            final = name.substr(0, cut);
            if (_wcsnicmp(name.c_str(), L"Unconfirmed ", 12) == 0) final.clear();
            return true;
        }
    }
    return false;
}

// Куда качают браузеры: Firefox/Zen/LibreWolf — prefs.js (browser.download.dir),
// Chrome/Edge/Яндекс/Brave/Opera — Preferences (download.default_directory)
static std::string ReadSmall(const std::wstring& p) {
    std::string s;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    DWORD sz = GetFileSize(f, nullptr), rd = 0;
    if (sz != INVALID_FILE_SIZE && sz < (8u << 20)) {
        s.resize(sz);
        if (!ReadFile(f, &s[0], sz, &rd, nullptr)) rd = 0;
        s.resize(rd);
    }
    CloseHandle(f);
    return s;
}
static std::wstring U8W(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
// значение строки после key (JSON/JS-экранирование \\ → \)
static void FindPathsAfter(const std::string& text, const std::string& key, std::vector<std::wstring>& out) {
    size_t p = 0;
    while ((p = text.find(key, p)) != std::string::npos) {
        p += key.size();
        size_t q = text.find('"', p);
        if (q == std::string::npos) return;
        size_t e = q + 1;
        std::string v;
        while (e < text.size() && text[e] != '"') {
            if (text[e] == '\\' && e + 1 < text.size()) { v += text[e + 1]; e += 2; }
            else v += text[e++];
        }
        if (v.size() > 2) out.push_back(U8W(v));
        p = e;
    }
}
static std::wstring Env(const wchar_t* n) {
    wchar_t b[MAX_PATH * 2];
    DWORD k = GetEnvironmentVariableW(n, b, MAX_PATH * 2);
    return k && k < MAX_PATH * 2 ? std::wstring(b, k) : L"";
}
static void BrowserDirs(std::vector<std::wstring>& out) {
    std::wstring app = Env(L"APPDATA"), loc = Env(L"LOCALAPPDATA");
    // Firefox-семейство
    for (const wchar_t* sub : {L"\\zen\\Profiles", L"\\Mozilla\\Firefox\\Profiles", L"\\librewolf\\Profiles", L"\\Waterfox\\Profiles",
                               L"\\Floorp\\Profiles"}) {
        std::wstring root = app + sub;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            FindPathsAfter(ReadSmall(root + L"\\" + fd.cFileName + L"\\prefs.js"), "\"browser.download.dir\",", out);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    // Chromium-семейство
    for (const wchar_t* sub : {L"\\Google\\Chrome\\User Data", L"\\Microsoft\\Edge\\User Data", L"\\Yandex\\YandexBrowser\\User Data",
                               L"\\BraveSoftware\\Brave-Browser\\User Data", L"\\Vivaldi\\User Data", L"\\Chromium\\User Data"}) {
        std::wstring root = loc + sub;
        for (const wchar_t* prof : {L"\\Default", L"\\Profile 1", L"\\Profile 2"})
            FindPathsAfter(ReadSmall(root + prof + L"\\Preferences"), "\"default_directory\":", out);
    }
    FindPathsAfter(ReadSmall(app + L"\\Opera Software\\Opera Stable\\Preferences"), "\"default_directory\":", out);
    FindPathsAfter(ReadSmall(app + L"\\Opera Software\\Opera GX Stable\\Preferences"), "\"default_directory\":", out);
}

void DownloadWatcher::Start(HWND notify, const std::wstring& dir) {
    notify_ = notify;
    dir_ = dir;
    if (dir_.empty()) return;
    dirs_.clear();
    std::vector<std::wstring> cand = {dir_};
    BrowserDirs(cand);
    PWSTR dp = nullptr;
    static const GUID kDesktop = {0xB4BFCC3A, 0xDB2C, 0x424C, {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}};
    if (SUCCEEDED(SHGetKnownFolderPath(kDesktop, 0, nullptr, &dp)) && dp) cand.push_back(dp);
    if (dp) CoTaskMemFree(dp);
    std::wstring tmp = Env(L"TEMP");
    if (!tmp.empty()) cand.push_back(tmp);  // Firefox/Zen иногда качают сначала во временную папку
    for (auto& c : cand) {
        std::wstring d = c;
        while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        DWORD a = GetFileAttributesW(d.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
        bool dup = false;
        for (auto& x : dirs_) if (_wcsicmp(x.c_str(), d.c_str()) == 0) dup = true;
        if (!dup && dirs_.size() < 8) dirs_.push_back(d);
    }
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void DownloadWatcher::Stop() {
    if (!stopEv_) return;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    CloseHandle(stopEv_);
    stopEv_ = nullptr;
}

static void Post(HWND n, int kind, DlInfo* d) {
    if (!PostMessageW(n, WM_APP_DL, kind, (LPARAM)d)) delete d;
}

void DownloadWatcher::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::vector<HANDLE> chs;
    std::vector<std::wstring> dirs;
    for (auto& d : dirs_) {
        HANDLE ch = FindFirstChangeNotificationW(d.c_str(), FALSE,
                                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (ch == INVALID_HANDLE_VALUE) continue;
        chs.push_back(ch);
        dirs.push_back(d);
    }
    if (chs.empty()) return;
    std::wstring tmpDir = Env(L"TEMP");
    while (!tmpDir.empty() && tmpDir.back() == L'\\') tmpDir.pop_back();
    struct Tmp { long long size = 0; std::wstring final, dir; };
    std::map<std::wstring, Tmp> tmps;  // ключ — полный путь временного файла
    long long lastTotal = 0;
    DWORD lastScan = 0, lastT = GetTickCount();
    bool wasActive = false;
    for (;;) {
        std::vector<HANDLE> waits = {stopEv_};
        waits.insert(waits.end(), chs.begin(), chs.end());
        DWORD w = WaitForMultipleObjects((DWORD)waits.size(), waits.data(), FALSE, tmps.empty() ? INFINITE : 1000);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        if (w > WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + waits.size()) FindNextChangeNotification(waits[w - WAIT_OBJECT_0]);
        // не чаще двух раз в секунду: во время загрузки уведомления сыплются постоянно
        DWORD since = GetTickCount() - lastScan;
        if (since < 500 && WaitForSingleObject(stopEv_, 500 - since) == WAIT_OBJECT_0) break;
        lastScan = GetTickCount();
        for (size_t i = 0; i < chs.size(); i++) if (WaitForSingleObject(chs[i], 0) == WAIT_OBJECT_0) FindNextChangeNotification(chs[i]);

        std::map<std::wstring, Tmp> now;
        for (auto& d : dirs) {
            bool isTemp = _wcsicmp(d.c_str(), tmpDir.c_str()) == 0;
            WIN32_FIND_DATAW fd;
            // во временной папке тысячи файлов — смотрим только *.part
            HANDLE f = FindFirstFileExW((d + (isTemp ? L"\\*.part" : L"\\*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (f == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                Tmp t;
                if (!IsTemp(fd.cFileName, t.final)) continue;
                t.size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (isTemp && t.size < 64 * 1024) continue;  // мелочь в TEMP — не загрузки
                t.dir = d;
                now[d + L"\\" + fd.cFileName] = t;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        // пропавшие временные файлы → готово, если появился итоговый файл (в той же папке или в «Загрузках»)
        for (auto& kv : tmps) {
            if (now.count(kv.first) || kv.second.final.empty()) continue;
            std::wstring path;
            for (const std::wstring& d : {kv.second.dir, dir_}) {
                std::wstring p = d + L"\\" + kv.second.final;
                DWORD a = GetFileAttributesW(p.c_str());
                if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) { path = p; break; }
            }
            if (!path.empty()) {
                DlInfo* d = new DlInfo;
                d->name = kv.second.final;
                d->path = path;
                d->active = (int)now.size();
                Post(notify_, DL_DONE, d);
            }
        }
        long long total = 0;
        std::wstring biggest;
        long long bigSize = -1;
        for (auto& kv : now) {
            total += kv.second.size;
            if (kv.second.size > bigSize) {
                bigSize = kv.second.size;
                std::wstring nm = kv.first.substr(kv.first.find_last_of(L'\\') + 1);
                biggest = kv.second.final.empty() ? nm : kv.second.final;
            }
        }
        DWORD t = GetTickCount();
        double dt = (t - lastT) / 1000.0;
        bool sameSet = now.size() == tmps.size();
        if (!now.empty() || wasActive) {
            DlInfo* d = new DlInfo;
            d->active = (int)now.size();
            d->bytes = total;
            d->speed = (sameSet && dt > 0.2 && total >= lastTotal) ? (total - lastTotal) / dt : -1;
            d->name = biggest;
            Post(notify_, DL_PROGRESS, d);
        }
        wasActive = !now.empty();
        lastTotal = total;
        lastT = t;
        tmps.swap(now);
    }
    for (HANDLE h : chs) FindCloseChangeNotification(h);
}
