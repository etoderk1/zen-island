// rec.inc — вкладка «Запись экрана» (OBS на минималках): звук, микрофон, курсор, FPS, качество,
// своя горячая клавиша, папка, отсчёт, пауза и последние записи

// IAudioMeterInformation (в заголовках mingw только объявлен) — пиковый уровень звука устройства
struct MeterInfo : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* n) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 n, float* v) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* m) = 0;
};
static const IID kIidMeter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
static int g_recCd = 0;            // отсчёт перед стартом (3, 2, 1)
static RECT g_recPendRc = {};
static HWND g_recPendWin = nullptr;
struct WinItem { HWND hwnd; std::wstring title, exe; std::vector<BYTE> px; int w = 0, h = 0; Pic icon; bool tried = false; };
static std::vector<WinItem> g_recWins;
static D2D1_RECT_F g_recSlRc[2] = {};   // дорожки ползунков микшера
static float g_meterSys = 0, g_meterMic = 0;  // плавные уровни для индикаторов
static MeterInfo *g_mtSys = nullptr, *g_mtMic = nullptr;
static int g_recDevPick = 0;                 // 1 — выбираем микрофон, 2 — выход (наушники/колонки)
static std::vector<AudioDev> g_recDevs;
static std::wstring g_recMicName, g_recOutName;  // имена выбранных устройств
static bool g_hkCap = false;       // ждём новое сочетание клавиш
struct RecFile { std::wstring path, name, when; long long size = 0; Pic thumb; std::vector<BYTE> px; int w = 0, h = 0; bool tried = false; };
static std::vector<RecFile> g_recFiles;
static double g_recScanT = -100;

static std::wstring FitText(IDWriteTextFormat* f, const std::wstring& s, float w) {
    if (MeasureW(f, s) <= w) return s;
    std::wstring t = s;
    while (t.size() > 1 && MeasureW(f, t + L"…") > w) t.pop_back();
    while (!t.empty() && t.back() == L' ') t.pop_back();
    return t + L"…";
}
static std::wstring RecDir() {
    if (!g_set.recDir.empty()) return g_set.recDir;
    static const GUID kVideos = {0x18989B1D, 0x99B5, 0x455B, {0x84, 0x1C, 0xAB, 0x7C, 0x74, 0xE4, 0xDD, 0xFC}};
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(kVideos, 0, nullptr, &p)) && p) r = std::wstring(p) + L"\\Zen Island";
    if (p) CoTaskMemFree(p);
    return r.empty() ? g_baseDir + L"\\recordings" : r;
}
static std::wstring HotkeyName(DWORD hk) {
    UINT mods = hk >> 16, vk = hk & 0xFFFF;
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    wchar_t b[64] = {};
    if (vk >= VK_F1 && vk <= VK_F24) swprintf(b, 64, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) swprintf(b, 64, L"%c", (wchar_t)vk);
    else {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        LONG lp = (LONG)(sc << 16);
        if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
            (vk >= VK_LEFT && vk <= VK_DOWN))
            lp |= 1 << 24;
        if (!GetKeyNameTextW(lp, b, 64)) swprintf(b, 64, L"#%u", vk);
    }
    return s + b;
}
static void RecRegisterHotkey() {
    UnregisterHotKey(g_hwnd, HK_REC);
    if (!g_set.tools || g_hkCap) return;
    if (!RegisterHotKey(g_hwnd, HK_REC, (g_set.recHotkey >> 16) | 0x4000 /*MOD_NOREPEAT*/, g_set.recHotkey & 0xFFFF))
        ShowActionToast(TK_ERROR, L"Сочетание " + HotkeyName(g_set.recHotkey) + L" занято другой программой", L"", TA_NONE, L"", kWhite, 3500);
}

// ---- старт/стоп
static void RecStartNow(RECT rc, HWND win) {
    std::wstring dir = RecDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring path = dir + L"\\" + StampName(L"Запись", L".mp4");
    RecOptions o;
    o.fps = g_set.recFps ? 60 : 30;
    o.quality = g_set.recQuality;
    o.cursor = g_set.recCursor;
    o.sysAudio = g_set.recSys;
    o.mic = g_set.recMic;
    o.win = win;
    o.micId = g_set.recMicId;
    o.outId = g_set.recOutId;
    o.sysGain = g_set.recSysVol / 100.f;
    o.micGain = g_set.recMicVol / 100.f;
    if (win && IsWindow(win)) {  // окно — на передний план, чтобы его не загораживали
        if (IsIconic(win)) ShowWindow(win, SW_RESTORE);
        SetForegroundWindow(win);
    }
    if (g_rec.Start(rc, path, g_hwnd, o)) {
        SetWindowDisplayAffinity(g_hwnd, 0x11 /*WDA_EXCLUDEFROMCAPTURE*/);  // сам остров в запись не попадёт
        g_recBlink = true;
        SetTimer(g_hwnd, T_REC, 500, nullptr);
        PlayUi(SN_DONE);
        SetMode(Collapsed());
    } else UpdateAffinity();
}
static void RecBegin(RECT rc, HWND win) {
    if (g_rec.Active()) return;
    if (win && IsWindow(win)) {  // окно сразу вперёд — пока идёт отсчёт, успеешь приготовиться
        if (IsIconic(win)) ShowWindow(win, SW_RESTORE);
        SetForegroundWindow(win);
    }
    if (!g_set.recCountdown) { RecStartNow(rc, win); return; }
    g_recPendRc = rc;
    g_recPendWin = win;
    g_recCd = 3;
    SetWindowDisplayAffinity(g_hwnd, 0x11);  // отсчёт тоже не должен попасть в кадр
    if (g_mode == M_EXPANDED) SetMode(Collapsed());
    ShowActionToast(TK_REC, L"Запись через 3…", L"", TA_NONE, L"", Col(1.f, 0.35f, 0.35f), 1200);
    SetTimer(g_hwnd, T_RECCD, 1000, nullptr);
}
static void RecCdTick() {
    if (--g_recCd <= 0) {
        KillTimer(g_hwnd, T_RECCD);
        if (g_mode == M_TOAST) SetMode(Collapsed());
        RecStartNow(g_recPendRc, g_recPendWin);
        return;
    }
    ShowActionToast(TK_REC, L"Запись через " + std::to_wstring(g_recCd) + L"…", L"", TA_NONE, L"", Col(1.f, 0.35f, 0.35f), 1200);
}
static void RecCancelCountdown() {
    if (g_recCd <= 0) return;
    g_recCd = 0;
    KillTimer(g_hwnd, T_RECCD);
    UpdateAffinity();
    ShowActionToast(TK_REC, L"Запись отменена", L"", TA_NONE, L"", kWhite, 1500);
}
// ---- выбор окна
static BOOL CALLBACK RecEnumWin(HWND h, LPARAM) {
    if (h == g_hwnd || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    typedef HRESULT(WINAPI * DwmAttr)(HWND, DWORD, PVOID, DWORD);
    static DwmAttr fn = (DwmAttr)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute");
    DWORD cloaked = 0;
    if (fn && SUCCEEDED(fn(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;  // скрытые окна UWP
    wchar_t t[256];
    if (GetWindowTextW(h, t, 256) <= 0) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return TRUE;
    WinItem it;
    it.hwnd = h;
    it.title = t;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t b[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, b, &n)) it.exe.assign(b, n);
        CloseHandle(p);
    }
    if (pid == GetCurrentProcessId()) return TRUE;
    g_recWins.push_back(std::move(it));
    return g_recWins.size() < 40;
}
static void RecPickOpen() {
    for (auto& w : g_recWins) w.icon.Release();
    g_recWins.clear();
    EnumWindows(RecEnumWin, 0);
    g_recPick = true;
    g_recPickScroll = 0;
    g_dirty = true;
}
static void RecWindowChosen(int i) {
    if (i < 0 || i >= (int)g_recWins.size()) return;
    HWND w = g_recWins[i].hwnd;
    g_recPick = false;
    if (!IsWindow(w)) { Shake(); return; }
    RECT r;
    GetWindowRect(w, &r);
    SetMode(Collapsed());
    RecBegin(r, w);
}
// с вкладки: окно — выбор из списка, весь экран — сразу, область — через выделение
static void RecStartFromUi() {
    if (g_rec.Active()) { g_rec.Stop(); return; }
    if (g_recCd > 0) { RecCancelCountdown(); return; }
    if (g_set.recArea == 0) RecPickOpen();
    else if (g_set.recArea == 1) {
        SetMode(Collapsed());
        RecBegin(g_mon);
    } else RunTool(TM_REC);
}
// горячая клавиша: в режиме «Окно» пишем то окно, что сейчас активно
static void RecHotkeyPressed() {
    if (g_rec.Active()) { g_rec.Stop(); return; }
    if (g_recCd > 0) { RecCancelCountdown(); return; }
    if (g_set.recArea == 0) {
        HWND fg = GetForegroundWindow();
        wchar_t cls[64] = {};
        if (fg) GetClassNameW(fg, cls, 64);
        if (fg && fg != g_hwnd && wcscmp(cls, L"Progman") && wcscmp(cls, L"WorkerW") && wcscmp(cls, L"Shell_TrayWnd")) {
            RECT r;
            GetWindowRect(fg, &r);
            RecBegin(r, fg);
        } else RecBegin(g_mon);
    } else if (g_set.recArea == 1) RecBegin(g_mon);
    else RunTool(TM_REC);
}

// ---- микшер
static MeterInfo* OpenMeter(EDataFlow flow, const std::wstring& id) {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    MeterInfo* m = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
        ((!id.empty() && SUCCEEDED(en->GetDevice(id.c_str(), &dev))) || SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev))))
        dev->Activate(kIidMeter, CLSCTX_ALL, nullptr, (void**)&m);
    if (dev) dev->Release();
    if (en) en->Release();
    return m;
}
static void RecMetersReopen() {
    SafeRelease(g_mtSys);
    SafeRelease(g_mtMic);
    g_mtSys = OpenMeter(eRender, g_set.recOutId);
    g_mtMic = OpenMeter(eCapture, g_set.recMicId);
    // имена выбранных устройств (если выбранное отключили — пишем «по умолчанию»)
    auto nameOf = [](bool cap, const std::wstring& id) -> std::wstring {
        std::wstring def;
        for (auto& d : ListAudioDevices(cap)) {
            if (!id.empty() && d.id == id) return d.name;
            if (d.isDefault) def = d.name;
        }
        return def.empty() ? L"по умолчанию" : def;
    };
    g_recOutName = nameOf(false, g_set.recOutId);
    g_recMicName = nameOf(true, g_set.recMicId);
}
static void RecMetersTick() {
    float s = 0, m = 0;
    if (g_rec.Active()) {  // во время записи — то, что реально уходит в файл (с учётом громкости)
        s = g_rec.SysPeak();
        m = g_rec.MicPeak();
    } else {
        if (g_mtSys) g_mtSys->GetPeakValue(&s);
        if (g_mtMic) g_mtMic->GetPeakValue(&m);
        s *= g_set.recSysVol / 100.f;
        m *= g_set.recMicVol / 100.f;
    }
    if (!g_set.recSys) s = 0;
    if (!g_set.recMic) m = 0;
    // быстро вверх, плавно вниз
    g_meterSys = s > g_meterSys ? s : g_meterSys * 0.86f;
    g_meterMic = m > g_meterMic ? m : g_meterMic * 0.86f;
}
static void RecSliderSet(int which, int x) {
    D2D1_RECT_F r = g_recSlRc[which == 51 ? 1 : 0];
    float fx = x / g_scale;
    if (r.right <= r.left) return;
    int v = (int)std::lround(Clamp((fx - r.left) / (r.right - r.left), 0.f, 1.f) * 200.f / 5.f) * 5;
    if (std::abs(v - 100) <= 5) v = 100;  // «залипает» на 100%
    (which == 51 ? g_set.recMicVol : g_set.recSysVol) = v;
    g_rec.SetGains(g_set.recSysVol / 100.f, g_set.recMicVol / 100.f);
    g_dirty = true;
}

// ---- последние записи
static void RecScan() {
    g_recScanT = NowSec();
    std::vector<RecFile> found;
    WIN32_FIND_DATAW fd;
    std::wstring dir = RecDir();
    HANDLE f = FindFirstFileW((dir + L"\\*.mp4").c_str(), &fd);
    std::vector<std::pair<long long, RecFile>> all;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            RecFile r;
            r.path = dir + L"\\" + fd.cFileName;
            r.name = fd.cFileName;
            r.size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            FILETIME lt;
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt);
            FileTimeToSystemTime(&lt, &st);
            static const wchar_t* mon[12] = {L"янв", L"фев", L"мар", L"апр", L"мая", L"июн", L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек"};
            wchar_t b[64];
            swprintf(b, 64, L"%d %ls, %02d:%02d", st.wDay, mon[(st.wMonth + 11) % 12], st.wHour, st.wMinute);
            r.when = b;
            long long t = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            all.push_back({t, std::move(r)});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < all.size() && i < 3; i++) {
        RecFile& r = all[i].second;
        for (auto& old : g_recFiles)  // миниатюру не грузим заново
            if (old.path == r.path && old.size == r.size) { r.px = old.px; r.w = old.w; r.h = old.h; r.tried = old.tried; }
        found.push_back(std::move(r));
    }
    for (auto& o : g_recFiles) o.thumb.Release();
    g_recFiles = std::move(found);
    g_dirty = true;
}
static void RecScanSoon() { g_recScanT = -100; g_dirty = true; }
static void DrawRecDevPicker(D2D1_RECT_F R) {
    RRect(R, 22, Lerp(kBgSolid, kWhite, 0.07f));
    float x = R.left + 16, w = R.right - R.left - 32, y = R.top + 10;
    D2D1_RECT_F back = RF(x - 4, y, 30, 30);
    AddHit(back, H_RECUI, 399);
    Circle(x + 11, y + 15, 15, Hot(H_RECUI, 399) ? Ov(0.14f) : Ov(0.06f));
    Line(x + 13, y + 9, x + 7, y + 15, 1.8f, kWhite);
    Line(x + 7, y + 15, x + 13, y + 21, 1.8f, kWhite);
    Text(g_fTitle, g_recDevPick == 1 ? L"Микрофон" : L"Чей звук писать (наушники, колонки)", RF(x + 34, y + 2, w - 34, 26),
         DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    y += 42;
    const std::wstring& cur = g_recDevPick == 1 ? g_set.recMicId : g_set.recOutId;
    for (int i = -1; i < (int)g_recDevs.size() && y + 40 <= R.bottom - 6; i++) {
        std::wstring name = i < 0 ? L"Как в Windows (по умолчанию)" : g_recDevs[i].name;
        bool sel = i < 0 ? cur.empty() : g_recDevs[i].id == cur;
        D2D1_RECT_F r = RF(R.left + 8, y, R.right - R.left - 16, 38);
        AddHit(r, H_RECUI, 400 + i + 1);
        RRect(r, 12, sel ? WithA(g_accent, 0.22f) : Hot(H_RECUI, 400 + i + 1) ? Ov(0.08f) : Ov(0.0f));
        if (g_recDevPick == 1) IconMic(r.left + 20, y + 19, sel ? kWhite : kGray);
        else IconSpeaker(r.left + 13, y + 19, 15, !sel);
        Text(g_fClip, FitText(g_fClip, name, r.right - r.left - 80), RF(r.left + 38, y, r.right - r.left - 80, 38), DWRITE_TEXT_ALIGNMENT_LEADING,
             sel ? kWhite : WithA(kWhite, 0.85f));
        if (i >= 0 && g_recDevs[i].isDefault)
            Text(g_fSmall, L"сейчас в Windows", RF(r.right - 130, y, 120, 38), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        y += 40;
    }
}
static void DrawRecPicker(D2D1_RECT_F c) {
    float cw = c.right - c.left;
    D2D1_RECT_F back = RF(c.left, c.top, 44, 44);
    AddHit(back, H_RECUI, 299);
    Circle(c.left + 22, c.top + 22, 22, Hot(H_RECUI, 299) ? Ov(0.14f) : kCard);
    Line(c.left + 25, c.top + 15, c.left + 18, c.top + 22, 2.f, kWhite);
    Line(c.left + 18, c.top + 22, c.left + 25, c.top + 29, 2.f, kWhite);
    Text(g_fTitle, L"Какое окно записать?", RF(c.left + 58, c.top, cw - 60, 26), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fSmall, L"Окно выйдет на передний план; если его подвинуть — запись поедет следом", RF(c.left + 58, c.top + 24, cw - 60, 18),
         DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    D2D1_RECT_F area = RF(c.left, c.top + 56, cw, c.bottom - c.top - 56);
    if (g_recWins.empty()) {
        RRect(area, 20, kCard);
        Text(g_fToast, L"Открытых окон не нашлось", area, DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
        return;
    }
    const int cols = 3;
    const float gap = 10, cellW = (cw - gap * (cols - 1)) / cols, cellH = 62;
    int rowsVis = std::max(1, (int)((area.bottom - area.top + gap) / (cellH + gap)));
    int rowsAll = ((int)g_recWins.size() + cols - 1) / cols;
    g_recPickScroll = Clamp(g_recPickScroll, 0, std::max(0, rowsAll - rowsVis));
    int budget = 4;
    for (int k = g_recPickScroll * cols; k < (int)g_recWins.size() && k < (g_recPickScroll + rowsVis) * cols; k++) {
        int v = k - g_recPickScroll * cols;
        WinItem& it = g_recWins[k];
        D2D1_RECT_F r = RF(area.left + (v % cols) * (cellW + gap), area.top + (v / cols) * (cellH + gap), cellW, cellH);
        AddHit(r, H_RECUI, 300 + k);
        RRect(r, 16, Hot(H_RECUI, 300 + k) ? kCardHot : kCard);
        if (!it.tried && budget-- > 0) { it.tried = true; if (!it.exe.empty()) LoadShellIcon(it.exe, it.px, it.w, it.h); }
        if (!it.icon.brush && !it.px.empty()) MakePic(it.icon, it.px, it.w, it.h);
        D2D1_RECT_F ic = RF(r.left + 12, r.top + (cellH - 32) / 2, 32, 32);
        if (it.icon.brush) DrawPicRounded(it.icon, ic, 6, 1.f);
        else RRect(ic, 8, Ov(0.1f));
        std::wstring app = it.exe.substr(it.exe.find_last_of(L'\\') + 1);
        if (app.size() > 4) app.resize(app.size() - 4);
        float tw = r.right - ic.right - 20;
        Text(g_fQa, FitText(g_fQa, it.title, tw), RF(ic.right + 10, r.top + 11, tw, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, FitText(g_fSmall, app, tw), RF(ic.right + 10, r.top + 32, tw, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
    if (rowsAll > rowsVis) {
        float h = area.bottom - area.top, th = std::max(24.f, h * rowsVis / rowsAll);
        float ty = area.top + (h - th) * g_recPickScroll / (float)(rowsAll - rowsVis);
        RRect(RF(c.right + 8, ty, 3, th), 1.5f, Ov(0.25f));
    }
}

static void RecPickDir() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))) || !d) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"Куда сохранять записи экрана");
    IShellItem* cur = nullptr;
    std::wstring dir = RecDir();
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&cur))) && cur) { d->SetFolder(cur); cur->Release(); }
    if (SUCCEEDED(d->Show(nullptr))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it)) && it) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                g_set.recDir = p;
                CoTaskMemFree(p);
                SaveSettings();
                RecScan();
            }
            it->Release();
        }
    }
    d->Release();
}

// ---- своё сочетание клавиш
static void RecHkStart() {
    g_hkCap = true;
    UnregisterHotKey(g_hwnd, HK_REC);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    g_dirty = true;
}
static void RecHkEnd() {
    g_hkCap = false;
    if (g_ieFocus < 0 && !g_clipOpen && !g_cmdOpen)
        SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    RecRegisterHotkey();
    g_dirty = true;
}
static void RecHkKey(UINT vk) {
    if (vk == VK_ESCAPE) { RecHkEnd(); return; }
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT)
        return;  // ждём основную клавишу
    UINT mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    bool fkey = vk >= VK_F1 && vk <= VK_F24;
    if (!mods && !fkey) { Shake(); return; }  // одиночную букву брать нельзя — будет мешать печатать
    g_set.recHotkey = (mods << 16) | (vk & 0xFFFF);
    SaveSettings();
    RecHkEnd();
    ShowActionToast(TK_REC, L"Запись: " + HotkeyName(g_set.recHotkey), L"", TA_NONE, L"", kWhite, 1800);
}

// ---- вкладка
static void RecChip(D2D1_RECT_F r, const std::wstring& t, bool sel, int arg) {
    AddHit(r, H_RECUI, arg);
    RRect(r, (r.bottom - r.top) / 2, sel ? g_accent : Ov(Hot(H_RECUI, arg) ? 0.16f : 0.08f));
    Text(g_fSmall, t, r, DWRITE_TEXT_ALIGNMENT_CENTER, sel ? Col(1, 1, 1) : kWhite);
}
static void DrawRecTab(D2D1_RECT_F c) {
    if (g_recPick) { DrawRecPicker(c); return; }
    if (NowSec() - g_recScanT > 5) { RecScan(); RecMetersReopen(); }
    RecMetersTick();
    g_dirty = true;  // индикаторы уровня живые, пока вкладка открыта
    const float lw = 300, gap = 12;
    bool act = g_rec.Active(), paused = act && g_rec.Paused();
    D2D1_COLOR_F red = Col(1.f, 0.27f, 0.27f);

    // --- большая кнопка
    D2D1_RECT_F top = RF(c.left, c.top, lw, 168);
    RRect(top, 22, act ? WithA(red, 0.12f) : kCard);
    float bx = top.left + 56, by = top.top + 58;
    bool hot = Hot(H_RECUI, 0);
    AddHit(RF(bx - 38, by - 38, 76, 76), H_RECUI, 0);
    float pulse = act && !paused ? 0.5f + 0.5f * (float)std::sin(NowSec() * 4) : 0.f;
    if (act) Circle(bx, by, 36 + pulse * 3, WithA(red, 0.25f));
    Circle(bx, by, hot ? 33.f : 31.f, red);
    if (act) RRect(RF(bx - 10, by - 10, 20, 20), 4, Col(1, 1, 1));
    else if (g_recCd > 0) Text(g_fWBig, std::to_wstring(g_recCd), RF(bx - 30, by - 14, 60, 28), DWRITE_TEXT_ALIGNMENT_CENTER, Col(1, 1, 1));
    else Circle(bx, by, 11, Col(1, 1, 1));
    float tx = bx + 50;
    if (act) {
        int s = (int)g_rec.Elapsed();
        wchar_t b[32];
        swprintf(b, 32, L"%d:%02d", s / 60, s % 60);
        Text(g_fWBig, b, RF(tx, by - 30, top.right - tx - 10, 28), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, paused ? L"Пауза" : L"Идёт запись", RF(tx, by, top.right - tx - 10, 18), DWRITE_TEXT_ALIGNMENT_LEADING,
             paused ? Col(1.f, 0.8f, 0.3f) : red);
        g_dirty = true;  // секунды и пульс
    } else {
        Text(g_fTitle, g_recCd > 0 ? L"Приготовься…" : L"Начать запись", RF(tx, by - 28, top.right - tx - 10, 24), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, HotkeyName(g_set.recHotkey), RF(tx, by, top.right - tx - 10, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
    float cy2 = top.top + 118, cw2 = (lw - 28 - 8) / 2;
    if (!act) {
        float cw3 = (lw - 28 - 12) / 3;
        RecChip(RF(top.left + 14, cy2, cw3, 34), L"Окно", g_set.recArea == 0, 2);
        RecChip(RF(top.left + 14 + cw3 + 6, cy2, cw3, 34), L"Область", g_set.recArea == 2, 4);
        RecChip(RF(top.left + 14 + 2 * (cw3 + 6), cy2, cw3, 34), L"Весь экран", g_set.recArea == 1, 3);
    } else {
        RecChip(RF(top.left + 14, cy2, cw2, 34), paused ? L"Продолжить" : L"Пауза", paused, 1);
        if (g_rec.HasMic()) RecChip(RF(top.left + 14 + cw2 + 8, cy2, cw2, 34), g_rec.MicMuted() ? L"Микрофон выкл" : L"Микрофон вкл", !g_rec.MicMuted(), 5);
        else RecChip(RF(top.left + 14 + cw2 + 8, cy2, cw2, 34), L"Стоп", false, 0);
    }

    // --- последние записи
    D2D1_RECT_F lst = RF(c.left, top.bottom + gap, lw, c.bottom - top.bottom - gap);
    RRect(lst, 22, kCard);
    Text(g_fSmall, L"ПОСЛЕДНИЕ ЗАПИСИ", RF(lst.left + 16, lst.top + 10, 200, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    D2D1_RECT_F op = RF(lst.right - 86, lst.top + 6, 76, 26);
    AddHit(op, H_RECUI, 42);
    if (Hot(H_RECUI, 42)) RRect(op, 13, Ov(0.1f));
    Text(g_fSmall, L"Папка ›", op, DWRITE_TEXT_ALIGNMENT_CENTER, Hot(H_RECUI, 42) ? kWhite : kGray);
    float ry = lst.top + 36, rowH = std::min(50.f, (lst.bottom - ry - 8) / 3);
    if (g_recFiles.empty())
        Text(g_fSmall, L"Здесь появятся твои записи", RF(lst.left, ry, lw, lst.bottom - ry - 10), DWRITE_TEXT_ALIGNMENT_CENTER, WithA(kGray, 0.8f));
    int budget = 1;
    for (int i = 0; i < (int)g_recFiles.size(); i++) {
        RecFile& f = g_recFiles[i];
        D2D1_RECT_F r = RF(lst.left + 8, ry + i * rowH, lw - 16, rowH - 4);
        AddHit(r, H_RECUI, 100 + i);
        if (Hot(H_RECUI, 100 + i)) RRect(r, 12, Ov(0.07f));
        D2D1_RECT_F th = RF(r.left + 6, r.top + (rowH - 4 - 34) / 2, 58, 34);
        if (!f.tried && budget-- > 0) { f.tried = true; LoadShellIcon(f.path, f.px, f.w, f.h); }
        if (!f.thumb.brush && !f.px.empty()) MakePic(f.thumb, f.px, f.w, f.h);
        if (f.thumb.brush) DrawPicRounded(f.thumb, th, 6, 1.f);
        else { RRect(th, 6, Ov(0.1f)); DrawToolIcon(TM_REC, (th.left + th.right) / 2, (th.top + th.bottom) / 2, kGray); }
        Text(g_fQa, f.when, RF(th.right + 10, r.top + 4, r.right - th.right - 46, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, FmtBytes((double)f.size), RF(th.right + 10, r.top + 23, r.right - th.right - 46, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        D2D1_RECT_F sb = RF(r.right - 34, r.top + (rowH - 4 - 28) / 2, 28, 28);
        AddHit(sb, H_RECUI, 200 + i);
        if (Hot(H_RECUI, 200 + i)) Circle((sb.left + sb.right) / 2, (sb.top + sb.bottom) / 2, 14, Ov(0.12f));
        IconFolder((sb.left + sb.right) / 2, (sb.top + sb.bottom) / 2, kGray);
    }

    // --- настройки справа
    D2D1_RECT_F R = RF(c.left + lw + gap, c.top, c.right - c.left - lw - gap, c.bottom - c.top);
    RRect(R, 22, kCard);
    float x = R.left + 16, w = R.right - R.left - 32, y = R.top + 8;
    const float rh = 42;
    // микшер: включатель, ползунок громкости и живой индикатор уровня
    struct Mx { const wchar_t* t; bool* on; int* vol; float lvl; int argOn, argSl; };
    Mx mx[2] = {{L"Звук компьютера", &g_set.recSys, &g_set.recSysVol, g_meterSys, 10, 50},
                {L"Микрофон", &g_set.recMic, &g_set.recMicVol, g_meterMic, 11, 51}};
    for (int i = 0; i < 2; i++) {
        Mx& m = mx[i];
        const float mh = 60;
        D2D1_RECT_F sw = RF(x + w - 50, y + 2, 50, 26);
        AddHit(sw, H_RECUI, m.argOn);
        if (i == 0) IconSpeaker(x + 1, y + 15, 16, !*m.on);
        else IconMic(x + 8, y + 15, *m.on ? kWhite : kGray);
        Text(g_fClip, m.t, RF(x + 24, y + 4, w - 140, 20), DWRITE_TEXT_ALIGNMENT_LEADING, *m.on ? kWhite : kGray);
        {   // устройство — нажми, чтобы выбрать
            float tx = x + 24 + MeasureW(g_fClip, m.t) + 10, maxW = x + w - 124 - tx;
            const std::wstring& dn = i == 0 ? g_recOutName : g_recMicName;
            std::wstring lab = FitText(g_fSmall, dn.empty() ? L"по умолчанию" : dn, std::max(30.f, maxW - 24)) + L" ›";
            float bw = std::min(maxW, std::max(60.f, MeasureW(g_fSmall, lab) + 18));
            if (bw > 30) {
                D2D1_RECT_F b = RF(tx, y + 2, bw, 24);
                AddHit(b, H_RECUI, i == 0 ? 15 : 14);
                RRect(b, 12, Ov(Hot(H_RECUI, i == 0 ? 15 : 14) ? 0.16f : 0.07f));
                Text(g_fSmall, lab, b, DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
            }
        }
        wchar_t vb[16];
        swprintf(vb, 16, L"%d%%", *m.vol);
        Text(g_fSmall, vb, RF(x + w - 120, y + 4, 60, 20), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        SwitchGfx(x + w - 44, y + 15, *m.on, Hot(H_RECUI, m.argOn));
        // дорожка: снизу уровень (зелёный → жёлтый → красный), сверху бегунок громкости
        float ty = y + 40;
        D2D1_RECT_F tr = RF(x + 2, ty - 4, w - 4, 8);
        g_recSlRc[i] = RF(tr.left, ty - 12, tr.right - tr.left, 24);
        AddHit(g_recSlRc[i], H_RECUI, m.argSl);
        RRect(tr, 4, Ov(0.10f));
        float lv = std::min(1.f, m.lvl);
        if (lv > 0.005f) {
            D2D1_COLOR_F lc = lv > 0.9f ? Col(1.f, 0.35f, 0.3f) : lv > 0.6f ? Col(1.f, 0.8f, 0.3f) : Col(0.35f, 0.85f, 0.45f);
            RRect(RF(tr.left, tr.top, std::max(8.f, (tr.right - tr.left) * lv), 8), 4, WithA(lc, *m.on ? 0.9f : 0.3f));
        }
        float f = *m.vol / 200.f, kx = tr.left + (tr.right - tr.left) * f;
        Line(tr.left + (tr.right - tr.left) * 0.5f, ty - 6, tr.left + (tr.right - tr.left) * 0.5f, ty + 6, 1.f, Ov(0.25f));  // отметка 100%
        bool hk = Hot(H_RECUI, m.argSl) || g_recDrag == m.argSl;
        Circle(kx, ty, hk ? 8.f : 7.f, Col(1, 1, 1));
        y += mh;
    }
    // курсор и отсчёт — двумя чипами
    {
        Text(g_fClip, L"В кадре", RF(x, y, 120, rh - 4), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        float bw1 = MeasureW(g_fSmall, L"Курсор") + 28, bw2 = MeasureW(g_fSmall, L"Отсчёт 3-2-1") + 28;
        RecChip(RF(x + w - bw2, y + 5, bw2, 30), L"Отсчёт 3-2-1", g_set.recCountdown, 13);
        RecChip(RF(x + w - bw2 - 6 - bw1, y + 5, bw1, 30), L"Курсор", g_set.recCursor, 12);
        y += rh;
    }
    auto label = [&](const wchar_t* t) { Text(g_fClip, t, RF(x, y, 160, rh - 4), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite); };
    // кадры
    label(L"Кадров в секунду");
    RecChip(RF(x + w - 132, y + 5, 62, 30), L"30", g_set.recFps == 0, 20);
    RecChip(RF(x + w - 64, y + 5, 62, 30), L"60", g_set.recFps == 1, 21);
    y += rh;
    // качество
    label(L"Качество");
    {
        const wchar_t* q[3] = {L"Эконом", L"Обычное", L"Высокое"};
        float cx = x + w;
        for (int i = 2; i >= 0; i--) {
            float bw = MeasureW(g_fSmall, q[i]) + 24;
            cx -= bw;
            RecChip(RF(cx, y + 5, bw, 30), q[i], g_set.recQuality == i, 30 + i);
            cx -= 6;
        }
    }
    y += rh;
    // сочетание
    label(L"Сочетание клавиш");
    {
        std::wstring t = g_hkCap ? L"Нажми сочетание… (Esc — отмена)" : HotkeyName(g_set.recHotkey);
        float bw = MeasureW(g_fSmall, t) + 28;
        D2D1_RECT_F b = RF(x + w - bw, y + 5, bw, 30);
        AddHit(b, H_RECUI, 40);
        RRect(b, 15, g_hkCap ? WithA(g_accent, 0.35f) : Ov(Hot(H_RECUI, 40) ? 0.16f : 0.08f));
        if (g_hkCap) StrokeRRect(b, 15, 1.4f, g_accent);
        Text(g_fSmall, t, b, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
    y += rh;
    // папка
    {
        Text(g_fClip, L"Папка", RF(x, y + 2, 120, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        float bw = MeasureW(g_fSmall, L"Изменить") + 28;
        D2D1_RECT_F b = RF(x + w - bw, y + 5, bw, 30);
        RecChip(b, L"Изменить", false, 41);
        Text(g_fSmall, FitText(g_fSmall, RecDir(), w - bw - 14), RF(x, y + 21, w - bw - 10, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
    if (g_recDevPick) {  // список устройств поверх правой карточки
        g_hits.erase(std::remove_if(g_hits.begin(), g_hits.end(),
                                    [&](const Hit& h) { return h.r.left >= R.left - 1 && h.r.top >= R.top - 1 && h.r.right <= R.right + 1 && h.r.bottom <= R.bottom + 1; }),
                     g_hits.end());
        DrawRecDevPicker(R);
    }
}

static void RecClick(int a) {
    switch (a) {
        case 0: RecStartFromUi(); break;
        case 1: g_rec.SetPaused(!g_rec.Paused()); break;
        case 2: case 3: case 4: g_set.recArea = a - 2; SaveSettings(); break;
        case 299: g_recPick = false; break;
        case 14: case 15:
            g_recDevPick = a == 14 ? 1 : 2;
            g_recDevs = ListAudioDevices(a == 14);
            break;
        case 399: g_recDevPick = 0; break;
        case 5: g_rec.SetMicMuted(!g_rec.MicMuted()); break;
        case 50: case 51: break;  // ползунки — через PanelDown
        case 10: g_set.recSys = !g_set.recSys; SaveSettings(); break;
        case 11: g_set.recMic = !g_set.recMic; SaveSettings(); break;
        case 12: g_set.recCursor = !g_set.recCursor; SaveSettings(); break;
        case 13: g_set.recCountdown = !g_set.recCountdown; SaveSettings(); break;
        case 20: case 21: g_set.recFps = a - 20; SaveSettings(); break;
        case 30: case 31: case 32: g_set.recQuality = a - 30; SaveSettings(); break;
        case 40: if (g_hkCap) RecHkEnd(); else RecHkStart(); break;
        case 41: RecPickDir(); break;
        case 42: {
            std::wstring d = RecDir();
            SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", d.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        default:
            if (a >= 100 && a < 100 + (int)g_recFiles.size()) ShellExecuteW(nullptr, L"open", g_recFiles[a - 100].path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (a >= 200 && a < 200 + (int)g_recFiles.size()) SelectInExplorer(g_recFiles[a - 200].path);
            if (a >= 300 && a < 300 + (int)g_recWins.size()) RecWindowChosen(a - 300);
            if (a >= 400 && a <= 400 + (int)g_recDevs.size()) {
                std::wstring id = a == 400 ? L"" : g_recDevs[a - 401].id;
                (g_recDevPick == 1 ? g_set.recMicId : g_set.recOutId) = id;
                if (g_recDevPick == 1 && !g_set.recMic) g_set.recMic = true;  // выбрал микрофон — значит, хочет его писать
                g_recDevPick = 0;
                SaveSettings();
                RecMetersReopen();
            }
            break;
    }
    g_dirty = true;
}
