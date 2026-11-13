// media.cpp — чтение системной медиа-сессии через «сырой» WinRT ABI.
// Без C++/WinRT, чтобы собиралось и MinGW, и MSVC, и exe оставался крошечным.
#include "media.h"
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <wincodec.h>
#include <shcore.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <tlhelp32.h>
#include <cwctype>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Минимальные описания WinRT-интерфейсов (порядок методов = порядок в vtable)
// ---------------------------------------------------------------------------
struct IAsyncInfoRaw : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;  // 0 Started,1 Completed,2 Canceled,3 Error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncOpRaw : public IInspectable {  // IAsyncOperation<T>
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void* result) = 0;
};
struct IGSMTCManagerStatics : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(IInspectable** op) = 0;
};
struct IGSMTCManager : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSession(IInspectable** session) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSessions(IInspectable** sessions) = 0;
};
struct IGSMTCSession : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceAppUserModelId(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetMediaPropertiesAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTimelineProperties(IInspectable** r) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaybackInfo(IInspectable** r) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPlayAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryPauseAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryStopAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRecordAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryFastForwardAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryRewindAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipNextAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TrySkipPreviousAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelUpAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeChannelDownAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryTogglePlayPauseAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeAutoRepeatModeAsync(int mode, IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackRateAsync(double rate, IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangeShuffleActiveAsync(unsigned char active, IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryChangePlaybackPositionAsync(INT64 pos, IInspectable** op) = 0;
};
struct IGSMTCMediaProps : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Title(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Subtitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumArtist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Artist(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTitle(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TrackNumber(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Genres(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AlbumTrackCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackType(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Thumbnail(IInspectable**) = 0;
};
struct IGSMTCPlaybackInfo : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Controls(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PlaybackStatus(int*) = 0;
};
struct IGSMTCControls : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_IsPlayEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPauseEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsStopEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRecordEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsFastForwardEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsRewindEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsNextEnabled(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPreviousEnabled(unsigned char*) = 0;
};
struct IGSMTCTimeline : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_StartTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_EndTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinSeekTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSeekTime(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Position(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LastUpdatedTime(INT64*) = 0;
};
struct IRASReference : public IInspectable {  // IRandomAccessStreamReference
    virtual HRESULT STDMETHODCALLTYPE OpenReadAsync(IInspectable** op) = 0;
};

static const IID IID_AsyncInfoRaw    = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID IID_MgrStatics      = {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
static const IID IID_Mgr             = {0xcace8eac, 0xe86e, 0x504a, {0xab, 0x31, 0x5f, 0xf8, 0xff, 0x1b, 0xce, 0x49}};
static const IID IID_Session         = {0x7148c835, 0x9b14, 0x5ae2, {0xab, 0x85, 0xdc, 0x9b, 0x1c, 0x14, 0xe1, 0xa8}};
static const IID IID_MediaProps      = {0x68856cf6, 0xadb4, 0x54b2, {0xac, 0x16, 0x05, 0x83, 0x79, 0x07, 0xac, 0xb6}};
static const IID IID_Controls        = {0x6501a3e6, 0xbc7a, 0x503a, {0xbb, 0x1b, 0x68, 0xf1, 0x58, 0xf3, 0xfb, 0x03}};
static const IID IID_PlaybackInfo    = {0x94b4b6cf, 0xe8ba, 0x51ad, {0x87, 0xa7, 0xc1, 0x0a, 0xde, 0x10, 0x61, 0x27}};
static const IID IID_Timeline        = {0xede34136, 0x6f25, 0x588d, {0x8e, 0xcf, 0xea, 0x5b, 0x67, 0x35, 0xaa, 0xa5}};
static const IID IID_RASReference    = {0x33ee3134, 0x1dd6, 0x4e3a, {0x80, 0x67, 0xd1, 0xc1, 0x62, 0xe8, 0x64, 0x2b}};
static const IID IID_IStream_        = {0x0000000c, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

template <class T> static void SafeRel(T*& p) { if (p) { p->Release(); p = nullptr; } }
template <class T> static T* QI(IUnknown* p, const IID& iid) {
    T* r = nullptr;
    if (p) p->QueryInterface(iid, (void**)&r);
    return r;
}

static std::wstring TakeH(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0;
    const wchar_t* s = WindowsGetStringRawBuffer(h, &n);
    std::wstring r(s ? s : L"", s ? n : 0);
    WindowsDeleteString(h);
    return r;
}

// Ждём IAsyncOperation<T> (мы в MTA-потоке, поэтому блокирующее ожидание безопасно)
template <class T> static bool Await(IInspectable* op, T* out, DWORD timeoutMs) {
    IAsyncInfoRaw* info = QI<IAsyncInfoRaw>(op, IID_AsyncInfoRaw);
    if (!info) return false;
    int st = 0;
    DWORD t0 = GetTickCount();
    while (SUCCEEDED(info->get_Status(&st)) && st == 0) {
        if (GetTickCount() - t0 > timeoutMs) { info->Cancel(); break; }
        Sleep(1);
    }
    info->Release();
    if (st != 1) return false;
    return SUCCEEDED(static_cast<IAsyncOpRaw*>(op)->GetResults(out));
}

D2D1_COLOR_F DefaultAccent() { return D2D1_COLOR_F{0.56f, 0.50f, 1.0f, 1.0f}; }

// Яркий акцентный цвет из обложки (взвешиваем насыщенные пиксели)
static D2D1_COLOR_F AccentFrom(const std::vector<BYTE>& px) {
    double rs = 0, gs = 0, bs = 0, ws = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 8) {
        double b = px[i], g = px[i + 1], r = px[i + 2];
        double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        double sat = (mx - mn) / (mx + 1.0);
        double w = sat * sat * (mx / 255.0) + 0.003;
        rs += r * w; gs += g * w; bs += b * w; ws += w;
    }
    if (ws <= 0) return DefaultAccent();
    double r = rs / ws / 255, g = gs / ws / 255, b = bs / ws / 255;
    double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    double v = mx, s = mx > 0 ? (mx - mn) / mx : 0, h = 0;
    if (mx - mn > 1e-6) {
        if (mx == r) h = fmod((g - b) / (mx - mn), 6.0);
        else if (mx == g) h = (b - r) / (mx - mn) + 2;
        else h = (r - g) / (mx - mn) + 4;
        h *= 60; if (h < 0) h += 360;
    }
    if (s < 0.12) return D2D1_COLOR_F{0.86f, 0.86f, 0.92f, 1.0f};  // ч/б обложка
    s = std::min(1.0, s * 1.25 + 0.1);
    v = std::max(v, 0.92);
    double c = v * s, x = c * (1 - fabs(fmod(h / 60.0, 2.0) - 1)), m = v - c;
    double rr = 0, gg = 0, bb = 0;
    if (h < 60) { rr = c; gg = x; } else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb = x; } else if (h < 240) { gg = x; bb = c; }
    else if (h < 300) { rr = x; bb = c; } else { rr = c; bb = x; }
    return D2D1_COLOR_F{(float)(rr + m), (float)(gg + m), (float)(bb + m), 1.0f};
}

// Обложка: поток -> WIC -> квадрат 128x128 PBGRA
static int g_lastSrcW = 0, g_lastSrcH = 0;  // размер последней загруженной обложки до обрезки
static bool LoadArt(IWICImagingFactory* wic, IInspectable* thumbRef, std::vector<BYTE>& out, int& w,
                    int& h, D2D1_COLOR_F& accent) {
    if (!wic) return false;
    bool ok = false;
    IRASReference* ref = QI<IRASReference>(thumbRef, IID_RASReference);
    IInspectable *op = nullptr, *stream = nullptr;
    IStream* ist = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    do {
        if (!ref || FAILED(ref->OpenReadAsync(&op)) || !op) break;
        if (!Await(op, &stream, 3000) || !stream) break;
        if (FAILED(CreateStreamOverRandomAccessStream(stream, IID_IStream_, (void**)&ist))) break;
        if (FAILED(wic->CreateDecoderFromStream(ist, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) break;
        if (FAILED(dec->GetFrame(0, &frame))) break;
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        if (!fw || !fh) break;
        g_lastSrcW = (int)fw;
        g_lastSrcH = (int)fh;
        UINT side = std::min(fw, fh);
        WICRect rc = {(INT)((fw - side) / 2), (INT)((fh - side) / 2), (INT)side, (INT)side};
        if (FAILED(wic->CreateBitmapClipper(&clip)) || FAILED(clip->Initialize(frame, &rc))) break;
        const UINT S = 128;
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(clip, S, S, WICBitmapInterpolationModeFant))) break;
        if (FAILED(wic->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom))) break;
        out.resize(S * S * 4);
        if (FAILED(conv->CopyPixels(nullptr, S * 4, (UINT)out.size(), out.data()))) { out.clear(); break; }
        w = h = (int)S;
        accent = AccentFrom(out);
        ok = true;
    } while (false);
    SafeRel(conv); SafeRel(scaler); SafeRel(clip); SafeRel(frame); SafeRel(dec);
    SafeRel(ist); SafeRel(stream); SafeRel(op); SafeRel(ref);
    return ok;
}

// ---------------------------------------------------------------------------
// Приложение-источник: красивое имя и иконка по AppUserModelId
// ---------------------------------------------------------------------------
static const GUID kFolderAppsFolder = {0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());  // понимает кириллицу, в отличие от towlower
    return s;
}
static std::wstring KnownName(const std::wstring& id) {
    std::wstring l = Lower(id);
    struct { const wchar_t* key; const wchar_t* name; } map[] = {
        {L"spotify", L"Spotify"},          {L"yandex.music", L"Яндекс Музыка"}, {L"yandexmusic", L"Яндекс Музыка"},
        {L"yandex", L"Яндекс Браузер"},    {L"chrome", L"Google Chrome"},       {L"msedge", L"Microsoft Edge"},
        {L"308046b0af4a39cb", L"Firefox"}, {L"firefox", L"Firefox"},            {L"opera", L"Opera"},
        {L"brave", L"Brave"},              {L"vivaldi", L"Vivaldi"},            {L"telegram", L"Telegram"},
        {L"discord", L"Discord"},          {L"zunemusic", L"Медиаплеер"},       {L"aimp", L"AIMP"},
        {L"vlc", L"VLC"},                  {L"foobar", L"foobar2000"},          {L"itunes", L"iTunes"},
        {L"applemusic", L"Apple Music"},   {L"soundcloud", L"SoundCloud"},      {L"deezer", L"Deezer"},
        {L"tidal", L"TIDAL"},              {L"vk", L"VK"},
    };
    for (auto& m : map)
        if (l.find(m.key) != std::wstring::npos) return m.name;
    std::wstring n = id;
    size_t bang = n.find(L'!');
    if (bang != std::wstring::npos) n = n.substr(bang + 1);
    size_t slash = n.find_last_of(L"\\/");
    if (slash != std::wstring::npos) n = n.substr(slash + 1);
    if (n.size() > 4 && Lower(n.substr(n.size() - 4)) == L".exe") n.resize(n.size() - 4);
    size_t dot = n.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < n.size()) n = n.substr(dot + 1);
    if (!n.empty()) CharUpperBuffW(&n[0], 1);
    return n;
}

static bool BitmapPixels(HBITMAP hb, std::vector<BYTE>& px, int& w, int& h) {
    BITMAP bm = {};
    if (!GetObjectW(hb, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    w = bm.bmWidth; h = bm.bmHeight;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    px.resize((size_t)w * h * 4);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines != h) { px.clear(); return false; }
    bool anyAlpha = false;
    for (size_t i = 3; i < px.size(); i += 4) if (px[i]) { anyAlpha = true; break; }
    if (!anyAlpha) for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    return true;
}

static bool ItemInfo(IShellItem* it, std::wstring* name, std::vector<BYTE>& icon, int& w, int& h) {
    bool got = false;
    if (name) {
        LPWSTR dn = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_NORMALDISPLAY, &dn)) && dn) { *name = dn; CoTaskMemFree(dn); }
    }
    IShellItemImageFactory* f = nullptr;
    if (SUCCEEDED(it->QueryInterface(IID_IShellItemImageFactory, (void**)&f)) && f) {
        SIZE sz = {48, 48};
        HBITMAP hb = nullptr;
        if (SUCCEEDED(f->GetImage(sz, SIIGBF_ICONONLY, &hb)) && hb) {
            got = BitmapPixels(hb, icon, w, h);
            DeleteObject(hb);
        }
        f->Release();
    }
    return got;
}

static std::wstring ProcessPathByExe(const std::wstring& exe) {
    std::wstring want = Lower(exe), result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (Lower(pe.szExeFile) != want) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t buf[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) result.assign(buf, n);
        CloseHandle(p);
        if (!result.empty()) break;
    }
    CloseHandle(snap);
    return result;
}

// Выполняется в отдельном коротком STA-потоке (shell-расширениям так спокойнее)
void ResolveAppInfo(const std::wstring& id, std::wstring& name, std::vector<BYTE>& icon, int& w, int& h) {
    name = KnownName(id);
    icon.clear(); w = h = 0;
    if (id.empty()) return;
    std::thread([&] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellItem* it = nullptr;
        std::wstring shellName;
        if (SUCCEEDED(SHCreateItemInKnownFolder(kFolderAppsFolder, 0, id.c_str(), IID_IShellItem, (void**)&it)) && it) {
            ItemInfo(it, &shellName, icon, w, h);
            it->Release();
            if (!shellName.empty() && shellName != id) name = shellName;
        }
        if (icon.empty()) {
            std::wstring exe = id;
            if (Lower(exe).find(L".exe") == std::wstring::npos) exe += L".exe";
            std::wstring path = ProcessPathByExe(exe);
            if (!path.empty() && SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, (void**)&it)) && it) {
                ItemInfo(it, nullptr, icon, w, h);
                it->Release();
            }
        }
        CoUninitialize();
    }).join();
}

static bool Same(const MediaSnap& a, const MediaSnap& b) {
    return a.has == b.has && a.title == b.title && a.artist == b.artist && a.album == b.album && a.status == b.status &&
           a.start == b.start && a.end == b.end && a.pos == b.pos && a.lastUpd == b.lastUpd &&
           a.canNext == b.canNext && a.canPrev == b.canPrev && a.artVer == b.artVer && a.appVer == b.appVer;
}

void MediaWatcher::Start(HWND notify) {
    notify_ = notify;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void MediaWatcher::Stop() {
    quit_ = true;
    if (wake_) SetEvent(wake_);
    if (th_.joinable()) th_.join();
    if (wake_) { CloseHandle(wake_); wake_ = nullptr; }
}
void MediaWatcher::Command(int c) { cmd_ = c; SetEvent(wake_); }
void MediaWatcher::Seek(INT64 pos) { seekPos_ = pos; cmd_ = CMD_SEEK; SetEvent(wake_); }
void MediaWatcher::SetInterval(int ms) {
    if (interval_.exchange(ms) != ms && wake_) SetEvent(wake_);
}
MediaSnap MediaWatcher::Get() {
    std::lock_guard<std::mutex> g(mx_);
    return snap_;
}

void MediaWatcher::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    RoInitialize(RO_INIT_MULTITHREADED);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&wic);

    IGSMTCManager* mgr = nullptr;
    DWORD lastMgrTry = 0;
    auto acquire = [&]() {
        lastMgrTry = GetTickCount();
        const wchar_t* name = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HSTRING cls = nullptr;
        if (FAILED(WindowsCreateString(name, (UINT32)wcslen(name), &cls))) return;
        IGSMTCManagerStatics* st = nullptr;
        if (SUCCEEDED(RoGetActivationFactory(cls, IID_MgrStatics, (void**)&st)) && st) {
            IInspectable* op = nullptr;
            if (SUCCEEDED(st->RequestAsync(&op)) && op) {
                IInspectable* res = nullptr;
                if (Await(op, &res, 10000) && res) { mgr = QI<IGSMTCManager>(res, IID_Mgr); res->Release(); }
                op->Release();
            }
            st->Release();
        }
        WindowsDeleteString(cls);
    };
    acquire();

    std::wstring lastKey;
    int artTries = 0;
    UINT artVer = 0;
    std::vector<BYTE> art;
    int artW = 0, artH = 0, srcW = 0, srcH = 0;
    D2D1_COLOR_F accent = DefaultAccent();
    std::wstring artKey;  // для какого трека текущая обложка
    std::wstring appId, appName;
    std::vector<BYTE> appIcon;
    int appIconW = 0, appIconH = 0;
    UINT appVer = 0;
    bool fast = true;

    while (!quit_) {
        WaitForSingleObject(wake_, fast ? 250 : (DWORD)interval_.load());
        fast = false;
        if (quit_) break;
        if (!mgr && GetTickCount() - lastMgrTry > 10000) acquire();

        IGSMTCSession* s = nullptr;
        if (mgr) {
            IInspectable* cur = nullptr;
            if (SUCCEEDED(mgr->GetCurrentSession(&cur)) && cur) { s = QI<IGSMTCSession>(cur, IID_Session); cur->Release(); }
        }

        int c = cmd_.exchange(0);
        if (c && s) {
            IInspectable* op = nullptr;
            HRESULT hr = c == CMD_TOGGLE ? s->TryTogglePlayPauseAsync(&op)
                       : c == CMD_NEXT   ? s->TrySkipNextAsync(&op)
                       : c == CMD_SEEK   ? s->TryChangePlaybackPositionAsync((INT64)seekPos_.load(), &op)
                                         : s->TrySkipPreviousAsync(&op);
            if (SUCCEEDED(hr) && op) { unsigned char okb = 0; Await(op, &okb, 1500); op->Release(); }
            fast = true;  // быстро перечитать состояние после команды
        }

        MediaSnap ns;
        if (s) {
            IInspectable* op = nullptr;
            if (SUCCEEDED(s->TryGetMediaPropertiesAsync(&op)) && op) {
                IInspectable* r = nullptr;
                if (Await(op, &r, 2000) && r) {
                    if (IGSMTCMediaProps* p = QI<IGSMTCMediaProps>(r, IID_MediaProps)) {
                        HSTRING h = nullptr;
                        if (SUCCEEDED(p->get_Title(&h))) ns.title = TakeH(h);
                        h = nullptr;
                        if (SUCCEEDED(p->get_Artist(&h))) ns.artist = TakeH(h);
                        if (ns.artist.empty()) { h = nullptr; if (SUCCEEDED(p->get_AlbumArtist(&h))) ns.artist = TakeH(h); }
                        h = nullptr;
                        if (SUCCEEDED(p->get_AlbumTitle(&h))) ns.album = TakeH(h);
                        ns.has = !ns.title.empty();
                        std::wstring key = ns.title + L"\x1f" + ns.artist;
                        if (key != lastKey) { lastKey = key; artTries = 0; }
                        // Старую обложку не убираем сразу — иначе мигает заглушка. Приложения часто
                        // отдают новую обложку чуть позже названия, поэтому пробуем несколько раз.
                        if (ns.has && (artTries < 2 || (artKey != key && artTries < 6))) {
                            artTries++;
                            bool got = false;
                            IInspectable* thumb = nullptr;
                            if (SUCCEEDED(p->get_Thumbnail(&thumb)) && thumb) {
                                std::vector<BYTE> a; int w = 0, hh = 0; D2D1_COLOR_F ac;
                                if (LoadArt(wic, thumb, a, w, hh, ac)) {
                                    got = true;
                                    if (a != art) { art.swap(a); artW = w; artH = hh; accent = ac; artVer++; }
                                    srcW = g_lastSrcW; srcH = g_lastSrcH;
                                    artKey = key;
                                }
                                thumb->Release();
                            }
                            if (!got && artKey != key && artTries >= 3 && !art.empty()) {
                                art.clear(); artW = artH = 0; srcW = srcH = 0; accent = DefaultAccent(); artVer++;
                            }
                            if (artTries < 2) fast = true;
                        }
                        p->Release();
                    }
                    r->Release();
                }
                op->Release();
            }
            {
                HSTRING h = nullptr;
                std::wstring id;
                if (SUCCEEDED(s->get_SourceAppUserModelId(&h))) id = TakeH(h);
                if (id != appId) {
                    appId = id;
                    ResolveAppInfo(appId, appName, appIcon, appIconW, appIconH);
                    appVer++;
                }
            }
            IInspectable* pi = nullptr;
            if (SUCCEEDED(s->GetPlaybackInfo(&pi)) && pi) {
                if (IGSMTCPlaybackInfo* p = QI<IGSMTCPlaybackInfo>(pi, IID_PlaybackInfo)) {
                    int st = 0;
                    if (SUCCEEDED(p->get_PlaybackStatus(&st))) ns.status = st;
                    IInspectable* ci = nullptr;
                    if (SUCCEEDED(p->get_Controls(&ci)) && ci) {
                        if (IGSMTCControls* cc = QI<IGSMTCControls>(ci, IID_Controls)) {
                            unsigned char b = 0;
                            if (SUCCEEDED(cc->get_IsNextEnabled(&b))) ns.canNext = b != 0;
                            b = 0;
                            if (SUCCEEDED(cc->get_IsPreviousEnabled(&b))) ns.canPrev = b != 0;
                            cc->Release();
                        }
                        ci->Release();
                    }
                    p->Release();
                }
                pi->Release();
            }
            IInspectable* ti = nullptr;
            if (SUCCEEDED(s->GetTimelineProperties(&ti)) && ti) {
                if (IGSMTCTimeline* t = QI<IGSMTCTimeline>(ti, IID_Timeline)) {
                    t->get_StartTime(&ns.start); t->get_EndTime(&ns.end);
                    t->get_Position(&ns.pos); t->get_LastUpdatedTime(&ns.lastUpd);
                    t->Release();
                }
                ti->Release();
            }
            s->Release();
        }
        if (!ns.has && !lastKey.empty()) {
            lastKey.clear(); artKey.clear(); art.clear(); artW = artH = 0; srcW = srcH = 0; accent = DefaultAccent(); artVer++; artTries = 0;
        }
        ns.artVer = artVer;
        ns.accent = accent;
        ns.appId = appId;
        ns.appName = appName;
        ns.appVer = appVer;

        bool changed;
        {
            std::lock_guard<std::mutex> g(mx_);
            changed = !Same(snap_, ns);
            if (changed) {
                if (snap_.artVer != ns.artVer) { ns.art = art; ns.artW = artW; ns.artH = artH; }
                else { ns.art.swap(snap_.art); ns.artW = snap_.artW; ns.artH = snap_.artH; }
                ns.artSrcW = srcW;
                ns.artSrcH = srcH;
                if (snap_.appVer != ns.appVer) { ns.appIcon = appIcon; ns.appIconW = appIconW; ns.appIconH = appIconH; }
                else { ns.appIcon.swap(snap_.appIcon); ns.appIconW = snap_.appIconW; ns.appIconH = snap_.appIconH; }
                snap_ = std::move(ns);
            }
        }
        if (changed) PostMessageW(notify_, WM_APP_MEDIA, 0, 0);
    }
    SafeRel(mgr);
    SafeRel(wic);
    RoUninitialize();
}
