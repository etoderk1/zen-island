// notify.cpp — новые уведомления из wpndatabase.db
#include "notify.h"
#include "media.h"
#include "third_party/sqlite3.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <propsys.h>
#include <propvarutil.h>
#include <map>
#include <wincodec.h>
#include <cwctype>
#include <algorithm>

static std::wstring FromU8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring DecodeEntities(const std::wstring& s) {
    std::wstring r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'&') { r += s[i]; continue; }
        size_t e = s.find(L';', i);
        if (e == std::wstring::npos || e - i > 10) { r += s[i]; continue; }
        std::wstring ent = s.substr(i + 1, e - i - 1);
        if (ent == L"amp") r += L'&';
        else if (ent == L"lt") r += L'<';
        else if (ent == L"gt") r += L'>';
        else if (ent == L"quot") r += L'"';
        else if (ent == L"apos") r += L'\'';
        else if (!ent.empty() && ent[0] == L'#') {
            unsigned long cp = ent.size() > 1 && (ent[1] == L'x' || ent[1] == L'X') ? wcstoul(ent.c_str() + 2, nullptr, 16)
                                                                                     : wcstoul(ent.c_str() + 1, nullptr, 10);
            if (cp >= 0x10000) { cp -= 0x10000; r += (wchar_t)(0xD800 + (cp >> 10)); r += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
            else if (cp) r += (wchar_t)cp;
        } else { r += s.substr(i, e - i + 1); }
        i = e;
    }
    return r;
}
static std::wstring Attr(const std::wstring& tag, const wchar_t* name) {
    std::wstring key = std::wstring(name) + L"=\"";
    size_t p = tag.find(key);
    if (p == std::wstring::npos) return L"";
    p += key.size();
    size_t e = tag.find(L'"', p);
    return e == std::wstring::npos ? L"" : DecodeEntities(tag.substr(p, e - p));
}

void ParseToastXml(const std::string& xml8, std::wstring& title, std::wstring& body, std::wstring& image, ToastActions* acts) {
    std::wstring x = FromU8(xml8);
    std::vector<std::wstring> texts;
    size_t p = 0;
    while ((p = x.find(L"<text", p)) != std::wstring::npos) {
        size_t gt = x.find(L'>', p);
        if (gt == std::wstring::npos) break;
        std::wstring tag = x.substr(p, gt - p);
        if (x[gt - 1] == L'/') { p = gt; continue; }  // <text/>
        size_t end = x.find(L"</text>", gt);
        if (end == std::wstring::npos) break;
        std::wstring t = DecodeEntities(x.substr(gt + 1, end - gt - 1));
        if (Attr(tag, L"placement") != L"attribution" && !t.empty()) texts.push_back(t);
        p = end + 7;
    }
    if (!texts.empty()) title = texts[0];
    for (size_t i = 1; i < texts.size(); i++) { if (!body.empty()) body += L" · "; body += texts[i]; }
    p = 0;
    while ((p = x.find(L"<image", p)) != std::wstring::npos) {
        size_t gt = x.find(L'>', p);
        if (gt == std::wstring::npos) break;
        std::wstring tag = x.substr(p, gt - p);
        std::wstring pl = Attr(tag, L"placement");
        if (pl == L"appLogoOverride" || (image.empty() && pl.empty())) {
            image = Attr(tag, L"src");
            if (pl == L"appLogoOverride") break;
        }
        p = gt;
    }
    if (!acts) return;
    // поля ввода и кнопки
    std::vector<std::wstring> inputs;
    p = 0;
    while ((p = x.find(L"<input", p)) != std::wstring::npos) {
        size_t gt = x.find(L'>', p);
        if (gt == std::wstring::npos) break;
        std::wstring tag = x.substr(p, gt - p);
        std::wstring type = Attr(tag, L"type");
        if (type.empty() || _wcsicmp(type.c_str(), L"text") == 0) inputs.push_back(Attr(tag, L"id"));
        p = gt;
    }
    p = 0;
    while ((p = x.find(L"<action", p)) != std::wstring::npos) {
        if (x.compare(p, 8, L"<actions") == 0) { p += 8; continue; }
        size_t gt = x.find(L'>', p);
        if (gt == std::wstring::npos) break;
        std::wstring tag = x.substr(p, gt - p);
        std::wstring args = DecodeEntities(Attr(tag, L"arguments")), hint = Attr(tag, L"hint-inputId"), content = DecodeEntities(Attr(tag, L"content"));
        std::wstring lc = content;
        if (!lc.empty()) CharLowerBuffW(&lc[0], (DWORD)lc.size());
        std::wstring la = args;
        if (!la.empty()) CharLowerBuffW(&la[0], (DWORD)la.size());
        bool isReply = !hint.empty() || la.find(L"reply") != std::wstring::npos || lc.find(L"ответ") != std::wstring::npos ||
                       lc.find(L"reply") != std::wstring::npos || lc.find(L"отправ") != std::wstring::npos || lc.find(L"send") != std::wstring::npos;
        bool isRead = la.find(L"read") != std::wstring::npos || la.find(L"mark") != std::wstring::npos || lc.find(L"прочит") != std::wstring::npos ||
                      lc.find(L"read") != std::wstring::npos;
        if (isReply && acts->replyArgs.empty() && !args.empty() && !inputs.empty()) {
            acts->replyArgs = args;
            acts->replyInput = hint.empty() ? inputs[0] : hint;
        } else if (isRead && acts->readArgs.empty() && !args.empty()) {
            acts->readArgs = args;
            acts->readLabel = content;
        }
        p = gt;
    }
}

// ---------------------------------------------------------------- нажатие кнопок уведомления
struct NOTIF_INPUT { LPCWSTR Key; LPCWSTR Value; };
struct INotifActivationCallback : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Activate(LPCWSTR appUserModelId, LPCWSTR invokedArgs, const NOTIF_INPUT* data, ULONG count) = 0;
};
static const IID kIID_NotifActivation = {0x53E31837, 0x6600, 0x4A81, {0x93, 0x95, 0x75, 0xCF, 0xFE, 0x74, 0x6F, 0x94}};
static const PROPERTYKEY kPK_AumId = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
static const PROPERTYKEY kPK_ToastClsid = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 26};

static bool ClsidFromShortcuts(const std::wstring& dir, const std::wstring& aumid, CLSID& out, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && ClsidFromShortcuts(p, aumid, out, depth - 1)) { found = true; break; }
            continue;
        }
        size_t n = wcslen(fd.cFileName);
        if (n < 4 || _wcsicmp(fd.cFileName + n - 4, L".lnk") != 0) continue;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(p.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&ps))) && ps) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kPK_AumId, &v)) && v.vt == VT_LPWSTR && v.pwszVal && _wcsicmp(v.pwszVal, aumid.c_str()) == 0) {
                PROPVARIANT c;
                PropVariantInit(&c);
                if (SUCCEEDED(ps->GetValue(kPK_ToastClsid, &c)) && c.vt == VT_CLSID && c.puuid) { out = *c.puuid; found = true; }
                PropVariantClear(&c);
            }
            PropVariantClear(&v);
            ps->Release();
        }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}
static bool FindActivator(const std::wstring& aumid, CLSID& out) {
    // 1) зарегистрировано через реестр
    wchar_t buf[64] = {};
    DWORD sz = sizeof(buf);
    std::wstring key = L"Software\\Classes\\AppUserModelId\\" + aumid;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"CustomActivator", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS &&
        SUCCEEDED(CLSIDFromString(buf, &out)))
        return true;
    sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"CustomActivator", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS &&
        SUCCEEDED(CLSIDFromString(buf, &out)))
        return true;
    // 2) ярлык в меню «Пуск» (так делает Telegram Desktop)
    for (REFKNOWNFOLDERID id : {FOLDERID_Programs, FOLDERID_CommonPrograms}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
            std::wstring d = p;
            CoTaskMemFree(p);
            if (ClsidFromShortcuts(d, aumid, out, 2)) return true;
        } else if (p) CoTaskMemFree(p);
    }
    return false;
}
bool ToastInvoke(const std::wstring& aumid, const std::wstring& args, const std::wstring& inputId, const std::wstring& text, std::wstring& err) {
    static std::map<std::wstring, CLSID> cache;
    CLSID clsid;
    auto it = cache.find(aumid);
    if (it != cache.end()) clsid = it->second;
    else {
        if (!FindActivator(aumid, clsid)) { err = L"Приложение не принимает ответы из уведомлений"; return false; }
        cache[aumid] = clsid;
    }
    INotifActivationCallback* cb = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, kIID_NotifActivation, (void**)&cb);
    if (FAILED(hr) || !cb) {
        wchar_t b[96];
        swprintf(b, 96, L"Не достучался до приложения (0x%08X)", (unsigned)hr);
        err = b;
        cache.erase(aumid);
        return false;
    }
    NOTIF_INPUT in = {inputId.c_str(), text.c_str()};
    hr = cb->Activate(aumid.c_str(), args.c_str(), inputId.empty() ? nullptr : &in, inputId.empty() ? 0 : 1);
    cb->Release();
    if (FAILED(hr)) {
        wchar_t b[96];
        swprintf(b, 96, L"Приложение ответило ошибкой 0x%08X", (unsigned)hr);
        err = b;
        return false;
    }
    return true;
}

// file:///C:/... -> C:\...
static std::wstring FilePathFromUri(std::wstring s) {
    if (_wcsnicmp(s.c_str(), L"file:///", 8) == 0) s = s.substr(8);
    else if (_wcsnicmp(s.c_str(), L"file://", 7) == 0) s = s.substr(7);
    else if (s.find(L"://") != std::wstring::npos) return L"";  // ms-appx и т.п. — пропускаем
    std::wstring r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size() && iswxdigit(s[i + 1]) && iswxdigit(s[i + 2])) {
            r += (wchar_t)wcstoul(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else r += s[i] == L'/' ? L'\\' : s[i];
    }
    return r;
}
static bool LoadAvatar(const std::wstring& path, std::vector<BYTE>& out, int& w, int& h) {
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&wic);
    if (!wic) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapClipper* cl = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    do {
        if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        UINT fw = 0, fh = 0;
        fr->GetSize(&fw, &fh);
        if (!fw || !fh) break;
        UINT side = std::min(fw, fh);
        WICRect rc = {(INT)((fw - side) / 2), (INT)((fh - side) / 2), (INT)side, (INT)side};
        if (FAILED(wic->CreateBitmapClipper(&cl)) || FAILED(cl->Initialize(fr, &rc))) break;
        if (FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(cl, 64, 64, WICBitmapInterpolationModeFant))) break;
        if (FAILED(wic->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            break;
        out.resize(64 * 64 * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, 64 * 4, (UINT)out.size(), out.data()));
        w = h = 64;
    } while (false);
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (cl) cl->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    wic->Release();
    return ok;
}

void NotificationWatcher::Start(HWND notify) {
    notify_ = notify;
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void NotificationWatcher::Stop() {
    if (!stopEv_) return;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    CloseHandle(stopEv_);
    stopEv_ = nullptr;
}

static bool LooksLikeCall(const std::wstring& t) {
    std::wstring l = t;
    if (!l.empty()) CharLowerBuffW(&l[0], (DWORD)l.size());
    static const wchar_t* keys[] = {L"входящий звонок", L"звонит", L"incoming call", L"is calling", L"входящий вызов", L"видеозвонок"};
    for (auto* k : keys) if (l.find(k) != std::wstring::npos) return true;
    return false;
}

void NotificationWatcher::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    PWSTR la = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &la)) && la) dir = std::wstring(la) + L"\\Microsoft\\Windows\\Notifications";
    if (la) CoTaskMemFree(la);
    std::wstring dbPath = dir + L"\\wpndatabase.db";

    struct AppInfo { std::wstring name; std::vector<BYTE> icon; int w = 0, h = 0; };
    std::map<std::wstring, AppInfo> apps;
    sqlite3* db = nullptr;
    long long lastId = -1;
    bool reportedError = false;
    HANDLE ch = INVALID_HANDLE_VALUE;

    for (;;) {
        if (!db) {
            std::string p8;
            int n = WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
            p8.resize(n);
            WideCharToMultiByte(CP_UTF8, 0, dbPath.c_str(), -1, &p8[0], n, nullptr, nullptr);
            if (sqlite3_open_v2(p8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
                if (db) { sqlite3_close(db); db = nullptr; }
            } else {
                sqlite3_busy_timeout(db, 1500);
                sqlite3_stmt* s = nullptr;
                if (sqlite3_prepare_v2(db, "SELECT IFNULL(MAX(Id),0) FROM Notification", -1, &s, nullptr) == SQLITE_OK &&
                    sqlite3_step(s) == SQLITE_ROW)
                    lastId = sqlite3_column_int64(s, 0);
                else { sqlite3_close(db); db = nullptr; }
                sqlite3_finalize(s);
            }
            if (!db && !reportedError) {
                NotifData* d = new NotifData;
                d->error = L"Не удалось открыть базу уведомлений Windows";
                if (!PostMessageW(notify_, WM_APP_NOTIF, 0, (LPARAM)d)) delete d;
                reportedError = true;
            }
            if (db && ch == INVALID_HANDLE_VALUE)
                ch = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
        }
        HANDLE waits[2] = {stopEv_, ch};
        DWORD w = ch != INVALID_HANDLE_VALUE ? WaitForMultipleObjects(2, waits, FALSE, db ? 4000 : 30000)
                                               : WaitForSingleObject(stopEv_, db ? 2000 : 30000);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        if (ch != INVALID_HANDLE_VALUE && w == WAIT_OBJECT_0 + 1) {
            FindNextChangeNotification(ch);
            if (WaitForSingleObject(stopEv_, 150) == WAIT_OBJECT_0) break;  // даём Windows дописать
        }
        if (!db) continue;

        sqlite3_stmt* s = nullptr;
        const char* q =
            "SELECT n.Id, n.Payload, h.PrimaryId FROM Notification n LEFT JOIN NotificationHandler h ON h.RecordId = n.HandlerId "
            "WHERE n.Id > ? AND n.Type = 'toast' ORDER BY n.Id LIMIT 20";
        if (sqlite3_prepare_v2(db, q, -1, &s, nullptr) != SQLITE_OK) {
            sqlite3_finalize(s);
            sqlite3_close(db);
            db = nullptr;
            continue;
        }
        sqlite3_bind_int64(s, 1, lastId);
        while (sqlite3_step(s) == SQLITE_ROW) {
            lastId = std::max(lastId, (long long)sqlite3_column_int64(s, 0));
            const void* pb = sqlite3_column_blob(s, 1);
            int pn = sqlite3_column_bytes(s, 1);
            const unsigned char* aid = sqlite3_column_text(s, 2);
            if (!pb || pn <= 0) continue;
            NotifData* d = new NotifData;
            d->aumid = aid ? FromU8((const char*)aid) : L"";
            std::wstring image;
            ToastActions acts;
            ParseToastXml(std::string((const char*)pb, pn), d->title, d->body, image, &acts);
            d->replyArgs = acts.replyArgs;
            d->replyInput = acts.replyInput;
            d->readArgs = acts.readArgs;
            d->readLabel = acts.readLabel;
            if (d->title.empty() && d->body.empty()) { delete d; continue; }
            auto it = apps.find(d->aumid);
            if (it == apps.end()) {
                AppInfo ai;
                ResolveAppInfo(d->aumid, ai.name, ai.icon, ai.w, ai.h);
                it = apps.emplace(d->aumid, std::move(ai)).first;
            }
            d->app = it->second.name;
            std::wstring file = FilePathFromUri(image);
            if (!file.empty() && LoadAvatar(file, d->icon, d->iw, d->ih)) d->avatar = true;
            else { d->icon = it->second.icon; d->iw = it->second.w; d->ih = it->second.h; }
            d->call = LooksLikeCall(d->title + L" " + d->body);
            if (!PostMessageW(notify_, WM_APP_NOTIF, 0, (LPARAM)d)) delete d;
        }
        sqlite3_finalize(s);
    }
    if (ch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(ch);
    if (db) sqlite3_close(db);
    CoUninitialize();
}
