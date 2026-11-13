// panel_logic.inc — поведение раскрытого острова: клики по вкладкам, редакторы,
// лаунчер, напоминания, «Управление». Подключается внутрь island.cpp.
// ----------------------------------------------------------------------------

static void ShowToast(Toast t, UINT ms);
static void ShowActionToast(int kind, const std::wstring& text, const std::wstring& right, int action,
                            const std::wstring& arg, D2D1_COLOR_F color, UINT ms);
static void OpenClip();
static void OpenCmd();
static void NotifReplySend();
static void NotifReplyEnd(bool next);
static int g_tabBeforeSettings = 0;
static void SettingsTextCommit();
static void SettingsOpened();
static bool SettingsClick(const Hit& h, int x);
static bool SettingsDown(const Hit& h, int x);
static void SliderSet(int idx, int x);
static int g_setDrag = -1, g_setSec = 0;
static float g_setContentH = 0;
static D2D1_RECT_F g_setClip = {};
static bool g_sbarDrag = false;
static void SbarSet(int y) {
    float vh = g_setClip.bottom - g_setClip.top, maxS = std::max(0.f, g_setContentH - vh);
    if (maxS <= 0) return;
    float th = std::max(30.f, vh * vh / g_setContentH);
    float f = Clamp((y / g_scale - g_setClip.top - th / 2) / std::max(1.f, vh - th), 0.f, 1.f);
    g_setScroll = g_setScrollT = f * maxS;
    g_dirty = true;
}
static void SelectInExplorer(const std::wstring& path);
static bool LoadShellIcon(const std::wstring& path, std::vector<BYTE>& px, int& w, int& h);
static void AppPickOpen();
static void AppPickClose();
static void AppPickToggle(int idx);
static void RecClick(int a);
static void RecSliderSet(int which, int x);
static void PollCursorPull();
static int Sty();

static std::wstring DataPath(const wchar_t* name) { return g_baseDir + L"\\data\\" + name; }
static std::string ReadFileU8(const std::wstring& p) {
    std::string s;
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return s;
    DWORD sz = GetFileSize(f, nullptr), rd = 0;
    if (sz != INVALID_FILE_SIZE && sz < (64u << 20)) {
        s.resize(sz);
        if (!ReadFile(f, &s[0], sz, &rd, nullptr)) rd = 0;
        s.resize(rd);
    }
    CloseHandle(f);
    return s;
}
static void WriteFileU8(const std::wstring& p, const std::string& s) {
    std::wstring tmp = p + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    bool ok = WriteFile(f, s.data(), (DWORD)s.size(), &wr, nullptr) && wr == s.size();
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(tmp.c_str());
}
static std::wstring U8ToW(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WToU8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- лаунчер
static void LauncherSave() {
    std::wstring all;
    for (auto& it : g_launch) all += it.path + L"\t" + it.name + L"\n";
    WriteFileU8(DataPath(L"launcher.txt"), WToU8(all));
}
static bool IsShellPath(const std::wstring& p) { return p.size() > 6 && _wcsnicmp(p.c_str(), L"shell:", 6) == 0; }
static int LauncherFind(const std::wstring& p) {
    for (int i = 0; i < (int)g_launch.size(); i++) if (_wcsicmp(g_launch[i].path.c_str(), p.c_str()) == 0) return i;
    return -1;
}
static void LauncherAddPath(const std::wstring& p, bool save, const std::wstring& nameIn = L"") {
    if (p.empty() || (!IsShellPath(p) && GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)) return;
    if (LauncherFind(p) >= 0) return;
    LaunchItem it;
    it.path = p;
    if (!nameIn.empty()) it.name = nameIn;
    else {
        size_t s = p.find_last_of(L"\\/");
        it.name = s == std::wstring::npos ? p : p.substr(s + 1);
        size_t d = it.name.rfind(L'.');
        if (d != std::wstring::npos && d > 0) it.name.resize(d);
    }
    LoadShellIcon(p, it.px, it.iw, it.ih);
    g_launch.push_back(std::move(it));
    if (save) LauncherSave();
    g_dirty = true;
}
static void LauncherLoad() {
    std::wstring all = U8ToW(ReadFileU8(DataPath(L"launcher.txt")));
    size_t a = 0;
    while (a < all.size()) {
        size_t b = all.find(L'\n', a);
        if (b == std::wstring::npos) b = all.size();
        std::wstring line = all.substr(a, b - a);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t tab = line.find(L'\t');
        if (tab != std::wstring::npos) LauncherAddPath(line.substr(0, tab), false, line.substr(tab + 1));
        else LauncherAddPath(line, false);
        a = b + 1;
    }
}
static void LauncherAddDialog() {
    wchar_t buf[MAX_PATH * 4] = {};
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = nullptr;
    of.lpstrFilter = L"Программы и ярлыки\0*.exe;*.lnk;*.url;*.bat;*.cmd\0Все файлы\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH * 4;
    of.lpstrTitle = L"Добавить в лаунчер острова";
    of.Flags = OFN_FILEMUSTEXIST | OFN_NODEREFERENCELINKS | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&of)) LauncherAddPath(buf, true);
}
static void LauncherRemove(int i) {
    if (i < 0 || i >= (int)g_launch.size()) return;
    g_launch[i].icon.Release();
    g_launch.erase(g_launch.begin() + i);
    LauncherSave();
    g_dirty = true;
}

// ---------------------------------------------------------------- напоминания
static long long FtNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static void RemSave() { SaveReminders(DataPath(L"reminders.txt"), g_rem); }
static void RemSort() { std::sort(g_rem.begin(), g_rem.end(), [](const Reminder& a, const Reminder& b) { return a.due < b.due; }); }
static void RemCheck() {
    long long now = FtNow();
    bool changed = false;
    for (size_t i = 0; i < g_rem.size();) {
        if (g_rem[i].due <= now) {
            Reminder r = g_rem[i];
            MessageBeep(MB_ICONASTERISK);
            ShowActionToast(TK_REMIND, r.text, L"", TA_NONE, L"", Col(1.f, 0.8f, 0.3f), 9000);
            changed = true;
            if (r.daily) {
                while (g_rem[i].due <= now) g_rem[i].due += 864000000000LL;
                i++;
            } else g_rem.erase(g_rem.begin() + i);
        } else i++;
    }
    if (changed) { RemSort(); RemSave(); g_dirty = true; }
}

// ---------------------------------------------------------------- «Управление»
#define WM_APP_CTRL (WM_APP + 20)
struct CtrlInfo { int wifi, bt; bool eth; std::wstring ssid; };
static volatile LONG g_ctrlBusy = 0;
static void CtrlRefreshAsync(int toggleKind = 0, bool toggleOn = false) {
    if (InterlockedCompareExchange(&g_ctrlBusy, 1, 0) != 0) return;
    HWND h = g_hwnd;
    std::thread([h, toggleKind, toggleOn] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (toggleKind) SetRadio(toggleKind, toggleOn);
        CtrlInfo* ci = new CtrlInfo;
        ci->wifi = RadioState(1);
        ci->bt = RadioState(3);
        ci->ssid = ci->wifi == 1 ? WifiName() : L"";
        ci->eth = EthernetUp();
        CoUninitialize();
        InterlockedExchange(&g_ctrlBusy, 0);
        if (!PostMessageW(h, WM_APP_CTRL, 0, (LPARAM)ci)) delete ci;
    }).detach();
}
static void OnCtrlInfo(CtrlInfo* ci) {
    g_wifiState = ci->wifi;
    g_btState = ci->bt;
    g_wifiName = ci->ssid;
    g_ethUp = ci->eth;
    delete ci;
    g_dirty = true;
}

// ---------------------------------------------------------------- громкость/устройство
static void RefreshVolumeUi() {
    if (g_audio.Ensure()) {
        float v = 0;
        BOOL m = FALSE;
        g_audio.vol->GetMasterVolumeLevelScalar(&v);
        g_audio.vol->GetMute(&m);
        g_vol = v;
        g_muted = m != FALSE;
    }
    g_volUi = -1;
    g_outName.clear();
    for (auto& d : ListOutputDevices())
        if (d.isDefault) { g_outName = d.name; break; }
}
static void SetVolumeFromX(int x) {
    float fx = x / g_scale;
    float v = Clamp((fx - g_volTrack.left) / std::max(1.f, g_volTrack.right - g_volTrack.left), 0.f, 1.f);
    v = std::round(v * 100) / 100;
    g_volUi = v;
    g_vol = v;
    if (g_audio.Ensure()) {
        g_audio.vol->SetMasterVolumeLevelScalar(v, nullptr);
        if (g_muted && v > 0) { g_audio.vol->SetMute(FALSE, nullptr); g_muted = false; }
    }
    g_dirty = true;
}
static void DeviceMenu() {
    auto devs = ListOutputDevices();
    if (devs.empty()) return;
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < devs.size(); i++)
        AppendMenuW(m, MF_STRING | (devs[i].isDefault ? MF_CHECKED : 0), 1000 + i, devs[i].name.c_str());
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + (int)devs.size()) {
        SetDefaultOutput(devs[cmd - 1000].id);
        g_audio.Reset();
        RefreshVolumeUi();
    }
}

// ---------------------------------------------------------------- редакторы (RichEdit поверх острова)
static COLORREF ToRef(D2D1_COLOR_F c) { return RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f)); }
#define kEdBg ToRef(kEdCol)
static void EdSetDefaults(HWND re, bool rich, int sizeTw) {
    SendMessageW(re, EM_SETBKGNDCOLOR, 0, kEdBg);
    if (!rich) SendMessageW(re, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.crTextColor = ToRef(kWhite);
    cf.yHeight = sizeTw;
    cf.bCharSet = DEFAULT_CHARSET;
    wcscpy(cf.szFaceName, L"Segoe UI");
    SendMessageW(re, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(re, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    SendMessageW(re, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
    SendMessageW(re, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_KEYEVENTS);
    SendMessageW(re, EM_EXLIMITTEXT, 0, 4 * 1024 * 1024);
}
static std::wstring EdText(HWND re) {
    GETTEXTLENGTHEX gl = {GTL_NUMCHARS | GTL_PRECISE, 1200};
    int n = (int)SendMessageW(re, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    std::wstring s(n + 1, L'\0');
    GETTEXTEX gt = {};
    gt.cb = (DWORD)((n + 1) * sizeof(wchar_t));
    gt.flags = GT_USECRLF;
    gt.codepage = 1200;
    int got = (int)SendMessageW(re, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)&s[0]);
    s.resize(std::max(0, got));
    return s;
}
static void EdSetText(HWND re, const std::wstring& s) {
    SETTEXTEX st = {ST_DEFAULT | ST_UNICODE, 1200};
    SendMessageW(re, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)s.c_str());
}
static void UpdateTextStats() {
    if (!g_reText) return;
    std::wstring s = EdText(g_reText);
    int chars = 0, words = 0, lines = s.empty() ? 0 : 1;
    bool inW = false;
    for (wchar_t c : s) {
        if (c == L'\n') lines++;
        if (c != L'\r' && c != L'\n') chars++;
        bool sp = iswspace(c) != 0;
        if (!sp && !inW) words++;
        inW = !sp;
    }
    wchar_t b[96];
    swprintf(b, 96, L"%d симв. · %d слов · %d строк", chars, words, lines);
    g_textStats = b;
    g_dirty = true;
}
// RTF заметки
static DWORD CALLBACK RtfOut(DWORD_PTR ck, LPBYTE buf, LONG cb, LONG* pcb) {
    ((std::string*)ck)->append((const char*)buf, cb);
    *pcb = cb;
    return 0;
}
struct RtfInState { const std::string* s; size_t pos; };
static DWORD CALLBACK RtfIn(DWORD_PTR ck, LPBYTE buf, LONG cb, LONG* pcb) {
    RtfInState* st = (RtfInState*)ck;
    LONG n = (LONG)std::min<size_t>(cb, st->s->size() - st->pos);
    memcpy(buf, st->s->data() + st->pos, n);
    st->pos += n;
    *pcb = n;
    return 0;
}
static bool g_noteDirty = false;
static void NoteSave() {
    if (!g_reNote || !g_noteDirty) return;
    g_noteDirty = false;
    std::string out;
    EDITSTREAM es = {(DWORD_PTR)&out, 0, RtfOut};
    SendMessageW(g_reNote, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
    WriteFileU8(DataPath(L"note.rtf"), out);
}
static void NoteLoad() {
    std::string in = ReadFileU8(DataPath(L"note.rtf"));
    if (in.empty()) return;
    RtfInState st = {&in, 0};
    EDITSTREAM es = {(DWORD_PTR)&st, 0, RtfIn};
    SendMessageW(g_reNote, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    SendMessageW(g_reNote, EM_SETBKGNDCOLOR, 0, kEdBg);
    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = ToRef(kWhite);
    SendMessageW(g_reNote, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}
static void NoteFormat(int what) {
    if (!g_reNote) return;
    if (what == 4) {
        PARAFORMAT2 pf = {};
        pf.cbSize = sizeof(pf);
        pf.dwMask = PFM_NUMBERING;
        SendMessageW(g_reNote, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
        bool on = pf.wNumbering == PFN_BULLET;
        pf.dwMask = PFM_NUMBERING | PFM_OFFSET;
        pf.wNumbering = on ? 0 : PFN_BULLET;
        pf.dxOffset = on ? 0 : 280;
        SendMessageW(g_reNote, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    } else {
        CHARFORMAT2W cf = {};
        cf.cbSize = sizeof(cf);
        SendMessageW(g_reNote, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        if (what == 3) {
            const COLORREF hl = g_light ? RGB(255, 226, 110) : RGB(120, 96, 18);
            bool on = !(cf.dwEffects & CFE_AUTOBACKCOLOR) && cf.crBackColor == hl;
            cf.dwMask = CFM_BACKCOLOR;
            cf.dwEffects = on ? CFE_AUTOBACKCOLOR : 0;
            cf.crBackColor = hl;
        } else {
            DWORD mask = what == 0 ? CFM_BOLD : what == 1 ? CFM_ITALIC : CFM_STRIKEOUT;
            DWORD eff = what == 0 ? CFE_BOLD : what == 1 ? CFE_ITALIC : CFE_STRIKEOUT;
            bool on = (cf.dwMask & mask) && (cf.dwEffects & eff);
            cf.dwMask = mask;
            cf.dwEffects = on ? 0 : eff;
        }
        SendMessageW(g_reNote, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    }
    g_noteDirty = true;
    SetTimer(g_hwnd, T_NOTESAVE, 800, nullptr);
}
static void RemindSubmit() {
    std::wstring s = g_ie[IE_REMIND].text;
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    if (s.empty()) return;
    Reminder r;
    std::wstring err;
    if (!ParseReminder(s, FtNow(), r, err)) {
        Shake();
        g_remErr = err.empty() ? L"Не понял, когда напомнить" : err;
        g_remErrUntil = NowSec() + 4;
        g_dirty = true;
        return;
    }
    r.id = FtNow();
    g_rem.push_back(r);
    RemSort();
    RemSave();
    g_ie[IE_REMIND] = IEdit{};
    g_remErr.clear();
    g_dirty = true;
}

static LRESULT CALLBACK EdHostProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE) {
                HWND re = (HWND)lp;
                if (re == g_reText) UpdateTextStats();
                else if (re == g_reCity || re == g_reIcs) SetTimer(g_hwnd, T_SETTEXT, 1200, nullptr);
                else if (re == g_reNote) { g_noteDirty = true; SetTimer(g_hwnd, T_NOTESAVE, 1200, nullptr); }
            }
            return 0;
        case WM_NOTIFY: {
            NMHDR* nh = (NMHDR*)lp;
            if (nh->code == EN_MSGFILTER) {
                MSGFILTER* mf = (MSGFILTER*)lp;
                if (mf->msg == WM_KEYDOWN && mf->wParam == VK_ESCAPE) {
                    if (nh->hwndFrom == g_reReply && g_reReply) { NotifReplyEnd(false); return 1; }
                    SetMode(Collapsed());
                    return 1;
                }
                if (nh->hwndFrom == g_reRemind && mf->msg == WM_KEYDOWN && mf->wParam == VK_RETURN) {
                    RemindSubmit();
                    return 1;
                }
                if (nh->hwndFrom == g_reReply && g_reReply && mf->msg == WM_KEYDOWN && mf->wParam == VK_RETURN) {
                    NotifReplySend();
                    return 1;
                }
                if ((nh->hwndFrom == g_reCity || nh->hwndFrom == g_reIcs) && mf->msg == WM_KEYDOWN && mf->wParam == VK_RETURN) {
                    KillTimer(g_hwnd, T_SETTEXT);
                    SettingsTextCommit();
                    return 1;
                }
                if ((nh->hwndFrom == g_reRemind || nh->hwndFrom == g_reCity || nh->hwndFrom == g_reIcs || (g_reReply && nh->hwndFrom == g_reReply)) && mf->msg == WM_CHAR &&
                    (mf->wParam == L'\r' || mf->wParam == L'\n'))
                    return 1;
            }
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND to = (HWND)lp;
                if (to != g_hostText && to != g_hostNote && to != g_hostRemind && to != g_hostCity && to != g_hostIcs)
                    SetTimer(g_hwnd, T_COLLAPSE, 250, nullptr);
                NoteSave();
                if (w == g_hostCity || w == g_hostIcs) { KillTimer(g_hwnd, T_SETTEXT); SettingsTextCommit(); }
            }
            break;
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
        case WM_SIZE: {
            HWND re = GetWindow(w, GW_CHILD);
            if (re) MoveWindow(re, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(w, &rc);
            HBRUSH b = CreateSolidBrush(kEdBg);
            FillRect((HDC)wp, &rc, b);
            DeleteObject(b);
            return 1;
        }
    }
    return DefWindowProcW(w, msg, wp, lp);
}
static HWND MakeEditor(HWND& host, bool rich, bool multiline, int sizeTw) {
    static bool reg = false;
    if (!reg) {
        LoadLibraryW(L"Msftedit.dll");
        WNDCLASSW wc = {};
        wc.lpfnWndProc = EdHostProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
        wc.lpszClassName = L"DerkIslandEdHost";
        RegisterClassW(&wc);
        reg = true;
    }
    host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"DerkIslandEdHost", L"", WS_POPUP, 0, 0, 10, 10, g_hwnd, nullptr,
                           g_inst, nullptr);
    DWORD st = WS_CHILD | WS_VISIBLE | (multiline ? (ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL) : ES_AUTOHSCROLL);
    HWND re = CreateWindowExW(0, L"RICHEDIT50W", L"", st, 0, 0, 10, 10, host, nullptr, g_inst, nullptr);
    if (re) EdSetDefaults(re, rich, sizeTw);
    return re;
}
// смена темы: перекрашиваем фон и весь текст (жирный/курсив и т.п. не трогаем)
static void ApplyEditorTheme() {
    for (HWND re : {g_reText, g_reNote, g_reRemind, g_reCity, g_reIcs}) {
        if (!re) continue;
        SendMessageW(re, EM_SETBKGNDCOLOR, 0, kEdBg);
        CHARFORMAT2W cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = ToRef(kWhite);
        SendMessageW(re, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        SendMessageW(re, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
        InvalidateRect(GetParent(re), nullptr, TRUE);
    }
}
static void EnsureEditors() {
    if (g_hostText) return;
    g_reText = MakeEditor(g_hostText, false, true, 210);
    g_reNote = MakeEditor(g_hostNote, true, true, 220);
    g_reRemind = MakeEditor(g_hostRemind, false, false, 200);
    if (g_reNote) NoteLoad();
    g_noteDirty = false;
    UpdateTextStats();
}
static void PlaceEditor(HWND host, D2D1_RECT_F r, bool show) {
    if (!host) return;
    if (show && IsWindowVisible(host)) {  // иногда поднимаем поверх острова (на случай, если его кто-то перекрыл)
        static std::map<HWND, double> last;
        if (NowSec() - last[host] > 1.0) {
            last[host] = NowSec();
            SetWindowPos(host, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }
    if (!show || r.right - r.left < 4) {
        if (IsWindowVisible(host)) ShowWindow(host, SW_HIDE);
        return;
    }
    int x = g_winPos.x + (int)std::lround(r.left * g_scale), y = g_winPos.y + (int)std::lround(r.top * g_scale);
    int w = (int)std::lround((r.right - r.left) * g_scale), h = (int)std::lround((r.bottom - r.top) * g_scale);
    RECT cur;
    GetWindowRect(host, &cur);
    bool vis = IsWindowVisible(host) != 0;
    if (!vis || cur.left != x || cur.top != y || cur.right - cur.left != w || cur.bottom - cur.top != h)
        SetWindowPos(host, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | (vis ? 0 : SWP_SHOWWINDOW));
}
static void EnsureSetEditors() {
    if (g_hostCity) return;
    g_reCity = MakeEditor(g_hostCity, false, false, 200);
    g_reIcs = MakeEditor(g_hostIcs, false, false, 200);
    if (g_reCity) EdSetText(g_reCity, g_set.city);
    if (g_reIcs) EdSetText(g_reIcs, g_set.icsUrl);
}
// клик по полю ввода на острове — явно отдаём фокус редактору (и поднимаем его поверх)
static void FocusEditor(int which) {
    HWND host = nullptr, re = nullptr;
    switch (which) {
        case 0: host = g_hostCity; re = g_reCity; break;
        case 1: host = g_hostIcs; re = g_reIcs; break;
        case 2: host = g_hostText; re = g_reText; break;
        case 3: host = g_hostNote; re = g_reNote; break;
        case 4: host = g_hostRemind; re = g_reRemind; break;
    }
    if (!host || !re) { g_dirty = true; return; }
    if (!IsWindowVisible(host)) return;
    SetWindowPos(host, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(host);
    SetFocus(re);
    int n = GetWindowTextLengthW(re);
    SendMessageW(re, EM_SETSEL, n, n);
}
static bool PanelSettled() { return !SpringsMoving() && g_content >= 1.f; }
static void SyncEditors() {
    if (g_hostReply) {
        bool show = g_mode == M_NOTIF && g_notifReply && g_visible && PanelSettled();
        bool wasHidden = !IsWindowVisible(g_hostReply);
        PlaceEditor(g_hostReply, g_rcReply, show);
        if (show && wasHidden) { SetForegroundWindow(g_hostReply); SetFocus(g_reReply); }
    }
    bool panel = g_mode == M_EXPANDED && g_visible && PanelSettled();
    if (panel && (g_tab == TAB_TEXT || g_tab == TAB_NOTE)) EnsureEditors();
    if (panel && g_tab == TAB_SETTINGS && g_setSec == 6) EnsureSetEditors();
    if (g_hostCity) {
        PlaceEditor(g_hostCity, g_rcCity, panel && g_tab == TAB_SETTINGS);
        PlaceEditor(g_hostIcs, g_rcIcs, panel && g_tab == TAB_SETTINGS);
    }
    if (!g_hostText) return;
    PlaceEditor(g_hostText, g_rcText, panel && g_tab == TAB_TEXT);
    PlaceEditor(g_hostNote, g_rcNote, panel && g_tab == TAB_NOTE);
    PlaceEditor(g_hostRemind, g_rcRemind, panel && g_tab == TAB_NOTE);
    if (!(panel && g_tab == TAB_NOTE)) NoteSave();
}
static bool EditorFocused() {
    HWND f = GetForegroundWindow();
    return f && (f == g_hostText || f == g_hostNote || f == g_hostRemind || f == g_hostCity || f == g_hostIcs || (g_hostReply && f == g_hostReply));
}
static bool CursorOverIsland() {
    POINT pt;
    GetCursorPos(&pt);
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    // нижние 14 DIP окна — прозрачный запас под волну
    rc.bottom -= (LONG)(14 * g_scale);
    return PtInRect(&rc, pt) != 0;
}

// ---------------------------------------------------------------- ввод
static int g_volPressPending = 0;
static void PanelHover(int x, int y) {
    const Hit* h = HitAt(x, y);
    int id = h ? h->id : H_NONE, arg = h ? h->arg : -1;
    if (id == H_TTSCROLL || id == H_SSCROLL) id = H_NONE, arg = -1;
    if (g_setDrag >= 0) SliderSet(g_setDrag, x);
    if (g_sbarDrag) SbarSet(y);
    if (g_recDrag >= 0) RecSliderSet(g_recDrag, x);
    if (id != g_hotId || arg != g_hotArg) { g_hotId = id; g_hotArg = arg; g_dirty = true; }
    if (g_volDrag) SetVolumeFromX(x);
}
static bool PanelDown(int x, int y) {
    const Hit* h = HitAt(x, y);
    if (!h) return false;
    if (h->id == H_VOL) {
        g_volDrag = true;
        SetCapture(g_hwnd);
        SetVolumeFromX(x);
        return true;
    }
    if (SettingsDown(*h, x)) return true;
    if (h->id == H_SBAR) { g_sbarDrag = true; SetCapture(g_hwnd); SbarSet(y); return true; }
    if (h->id == H_RECUI && (h->arg == 50 || h->arg == 51)) { g_recDrag = h->arg; SetCapture(g_hwnd); RecSliderSet(g_recDrag, x); return true; }
    if (h->id == H_SHELFTILE || h->id == H_SHELFALL) {
        g_shelfPress = h->id == H_SHELFALL ? -2 : h->arg;
        g_shelfPressPt = {x, y};
        SetCapture(g_hwnd);
        return true;
    }
    return false;
}
static void SwitchTab(int t) {
    if (t == TAB_CLIP) {
        if (g_mode != M_CLIP) OpenClip();
        return;
    }
    if (g_mode == M_CLIP) {
        g_tab = t;
        CloseClip(false);
        SetMode(M_EXPANDED);
    }
    if (g_tab == TAB_NOTE) NoteSave();
    if (t != g_tab && g_set.anim > 0) {  // содержимое «въезжает» с той стороны, куда переключили
        int a = g_tab == TAB_SETTINGS ? 7 : (int)g_set.tabOrder.find((wchar_t)(L'0' + g_tab));
        int b = t == TAB_SETTINGS ? 7 : (int)g_set.tabOrder.find((wchar_t)(L'0' + t));
        sTab.x = b > a ? 1.0 : -1.0;
        sTab.v = 0;
        sTab.target = 0;
    }
    if (t != TAB_SETTINGS) g_tabBeforeSettings = t;
    g_tab = t;
    if (t == TAB_SETTINGS) SettingsOpened();
    if (t == TAB_CTRL) { g_darkTheme = IsDarkTheme(); CtrlRefreshAsync(); }
    if (t == TAB_HOME) RefreshVolumeUi();
    if (t == TAB_WEATHER && !g_wx.ok) g_wxs.Refresh();
    g_ttScroll = 0;
    g_dirty = true;
}
static void PlainTextClipboard() {
    std::wstring s;
    if (OpenClipboard(g_hwnd)) {
        if (HANDLE hd = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) { s = p; GlobalUnlock(hd); }
        }
        CloseClipboard();
    }
    if (s.empty()) { ShowActionToast(TK_ERROR, L"В буфере нет текста", L"", TA_NONE, L"", kWhite, 1600); return; }
    CopyTextToClipboard(g_hwnd, s);
    ShowActionToast(TK_TEXT, L"Форматирование убрано", L"", TA_NONE, L"", g_accent, 1600);
}
static void RunTextOp(int op) {
    EnsureEditors();
    std::wstring in = EdText(g_reText), out, err;
    if (in.empty()) {
        // пусто — берём из буфера
        if (OpenClipboard(g_hwnd)) {
            if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
                if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) { in = p; GlobalUnlock(hd); }
            CloseClipboard();
        }
    }
    if (in.empty()) { g_remErr.clear(); return; }
    if (!ApplyTextOp(op, in, out, err)) {
        g_textErr = err.empty() ? L"Не получилось" : err;
        g_textErrUntil = NowSec() + 3;
        Shake();
        g_dirty = true;
        return;
    }
    EdSetText(g_reText, out);
    UpdateTextStats();
}
// что делаем с текстом поля
static void IECommit(int id, bool enter) {
    switch (id) {
        case IE_CITY:
        case IE_ICS:
        case IE_EXCLUDE:
            SettingsTextCommit();
            if (enter) IEBlur(false);
            break;
        case IE_REMIND: if (enter) RemindSubmit(); break;
        case IE_REPLY: if (enter) NotifReplySend(); break;
        case IE_APPS:
            if (enter) AppPickToggle(0);   // Enter — первая найденная
            else if (!g_appPick) break;
            break;
    }
}
// true — клик обработан панелью
static bool PanelClick(int x, int y) {
    const Hit* hp = HitAt(x, y);
    if (g_ieFocus >= 0 && !(hp && hp->id == H_IEDIT && hp->arg == g_ieFocus) && !(hp && hp->id == H_APPPICK && g_ieFocus == IE_APPS))
        IEBlur(true);  // кликнули мимо поля
    if (!hp) return CursorOverIsland();  // пустое место панели — ничего не делаем
    Hit h = *hp;
    switch (h.id) {
        case H_TAB: SwitchTab(h.arg); return true;
        case H_PIN: g_pinned = !g_pinned; g_dirty = true; return true;
        case H_SETTINGS:
            if (g_tab == TAB_SETTINGS) SwitchTab(g_tabBeforeSettings);
            else SwitchTab(TAB_SETTINGS);
            return true;
        case H_SEARCHBTN: OpenCmd(); return true;
        case H_EDFOCUS: FocusEditor(h.arg); return true;
        case H_IEDIT: IEFocus(h.arg); return true;
        case H_DEVICE: DeviceMenu(); return true;
        case H_QA:
            switch (h.arg) {
                case 0: RunTool(TM_SHOT_AREA); break;
                case 1: RunTool(TM_SHOT_FULL); break;
                case 2: RunTool(TM_OCR); break;
                case 3: RunTool(TM_QR); break;
                case 4: RunTool(TM_PICK); break;
                case 5: PlainTextClipboard(); break;
                case 6: RunTool(TM_REC); break;
                case 7: OpenClip(); break;
            }
            return true;
        case H_LAUNCH:
            if (h.arg < (int)g_launch.size()) {
                std::wstring p = g_launch[h.arg].path;
                std::wstring dir = IsShellPath(p) ? L"" : p.substr(0, p.find_last_of(L"\\/") == std::wstring::npos ? 0 : p.find_last_of(L"\\/"));
                ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
                if (!g_pinned) SetMode(Collapsed());
            }
            return true;
        case H_LAUNCHADD: AppPickOpen(); return true;
        case H_APPPICK: AppPickToggle(h.arg); return true;
        case H_RECUI: RecClick(h.arg); return true;
        case H_APPCLOSE: AppPickClose(); return true;
        case H_APPBROWSE: LauncherAddDialog(); return true;
        case H_SHELFX: ShelfRemove(h.arg); return true;
        case H_SHELFCLEAR: ShelfClear(); return true;
        case H_TTOOL: RunTextOp(h.arg); return true;
        case H_TT_PASTE: {
            EnsureEditors();
            std::wstring s;
            if (OpenClipboard(g_hwnd)) {
                if (HANDLE hd = GetClipboardData(CF_UNICODETEXT))
                    if (const wchar_t* p = (const wchar_t*)GlobalLock(hd)) { s = p; GlobalUnlock(hd); }
                CloseClipboard();
            }
            EdSetText(g_reText, s);
            UpdateTextStats();
            return true;
        }
        case H_TT_COPY:
            if (g_reText) {
                std::wstring s = EdText(g_reText);
                if (!s.empty()) { CopyTextToClipboard(g_hwnd, s); g_textErr = L"Скопировано"; g_textErrUntil = NowSec() + 1.5; g_dirty = true; }
            }
            return true;
        case H_TT_CLEAR: if (g_reText) { EdSetText(g_reText, L""); UpdateTextStats(); } return true;
        case H_NOTEFMT: NoteFormat(h.arg); return true;
        case H_TPRESET: {
            static const double s[4] = {5 * 60, 10 * 60, 25 * 60, 25 * 60};
            StartTimer(s[h.arg], h.arg == 3 ? 1 : 0);
            if (g_mode != M_EXPANDED) SetMode(M_EXPANDED);  // StartTimer мог свернуть остров
            return true;
        }
        case H_TPAUSE: ToggleTimerPause(); return true;
        case H_TPLUS:
            if (g_tm.on) {
                g_tm.total += 60;
                if (g_tm.paused) g_tm.left += 60; else g_tm.endAt += 60;
                g_dirty = true;
            }
            return true;
        case H_TRESET: StopTimer(); if (g_mode != M_EXPANDED) SetMode(M_EXPANDED); return true;
        case H_REMDEL:
            if (h.arg < (int)g_rem.size()) { g_rem.erase(g_rem.begin() + h.arg); RemSave(); g_dirty = true; }
            return true;
        case H_WXREFRESH: g_wxs.Refresh(); return true;
        case H_CTRL:
            if (h.arg == 0 && g_wifiState >= 0) { g_wifiState = g_wifiState ? 0 : 1; CtrlRefreshAsync(1, g_wifiState == 1); }
            if (h.arg == 1 && g_btState >= 0) { g_btState = g_btState ? 0 : 1; CtrlRefreshAsync(3, g_btState == 1); }
            if (h.arg == 2) { SetDarkTheme(!g_darkTheme); g_darkTheme = IsDarkTheme(); }
            if (h.arg == 4) { g_set.dnd = !g_set.dnd; SaveSettings(); }
            if (h.arg == 5) { g_vpnForceToast = true; VpnCheckAsync(g_hwnd, true); }
            g_dirty = true;
            return true;
        case H_POWER:
            if (h.arg == 0) { PowerAction(0); return true; }
            if (g_powerArm == h.arg && NowSec() < g_powerArmUntil) {
                g_powerArm = -1;
                SetMode(Collapsed());
                PowerAction(h.arg);
            } else {
                g_powerArm = h.arg;
                g_powerArmUntil = NowSec() + 3;
            }
            g_dirty = true;
            return true;
    }
    SettingsClick(h, x);
    return true;
}
static bool PanelRightClick(int x, int y) {
    const Hit* h = HitAt(x, y);
    if (!h) return false;
    if (h->id == H_LAUNCH) {
        int i = h->arg;
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"Открыть расположение");
        AppendMenuW(m, MF_STRING, 2, L"Убрать из лаунчера");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(g_hwnd);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 1 && i < (int)g_launch.size()) SelectInExplorer(g_launch[i].path);
        if (cmd == 2) LauncherRemove(i);
        return true;
    }
    return false;
}
static bool PanelWheel(int x, int y, int delta) {
    const Hit* h = HitAt(x, y);
    // настройки: крутим, где бы ни была мышь в правой части (карточки и переключатели перекрывают фон)
    if (g_tab == TAB_SETTINGS && g_mode == M_EXPANDED) {
        float fx = x / g_scale, fy = y / g_scale;
        if (fx >= g_setClip.left && fx <= g_setClip.right && fy >= g_setClip.top && fy <= g_setClip.bottom) {
            float maxS = std::max(0.f, g_setContentH - (g_setClip.bottom - g_setClip.top));
            g_setScrollT = Clamp(g_setScrollT - delta / (float)WHEEL_DELTA * 90.f, 0.f, maxS);  // плавно доедет в Frame
            g_dirty = true;
            return true;
        }
        return true;  // над списком разделов колесо ничего не делает (раньше крутило громкость)
    }
    if (g_recPick && g_tab == TAB_REC && g_mode == M_EXPANDED) {
        g_recPickScroll = std::max(0, g_recPickScroll - (delta > 0 ? 1 : -1));
        g_dirty = true;
        return true;
    }
    if (g_appPick && g_tab == TAB_HOME && g_mode == M_EXPANDED) {
        g_appScroll = std::max(0, g_appScroll - (delta > 0 ? 1 : -1));
        g_dirty = true;
        return true;
    }
    if (g_tab == TAB_TEXT && h && h->id == H_TTOOL) {
        g_ttScroll -= (int)(delta / (float)WHEEL_DELTA * 50);
        if (g_ttScroll < 0) g_ttScroll = 0;
        g_dirty = true;
        return true;
    }
    if (h && h->id == H_SSCROLL) {
        g_setScrollT = std::max(0.f, g_setScrollT - delta / (float)WHEEL_DELTA * 90.f);
        g_dirty = true;
        return true;
    }
    if (h && h->id == H_TTSCROLL) {
        g_ttScroll -= (int)(delta / (float)WHEEL_DELTA * 50);
        if (g_ttScroll < 0) g_ttScroll = 0;
        g_dirty = true;
        return true;
    }
    return g_mode == M_EXPANDED && g_tab != TAB_HOME;  // громкость колесом — только на «Главной»
}
// при раскрытии панели
static void OnPanelOpened() {
    g_powerArm = -1;
    if (g_tab == TAB_HOME) RefreshVolumeUi();
    if (g_tab == TAB_CTRL) { g_darkTheme = IsDarkTheme(); CtrlRefreshAsync(); }
}
// перетащить всю полку разом
static void ShelfDragAll() {
    static const GUID kBHID_DataObject = {0xB8C0BD9F, 0xED24, 0x455C, {0x83, 0xE6, 0xD5, 0x39, 0x0C, 0x4F, 0xE8, 0xC4}};
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (auto& it : g_shelf) {
        PIDLIST_ABSOLUTE p = nullptr;
        if (SUCCEEDED(SHParseDisplayName(it.path.c_str(), nullptr, &p, 0, nullptr)) && p) ids.push_back(p);
    }
    if (!ids.empty()) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &arr)) && arr) {
            IDataObject* dobj = nullptr;
            if (SUCCEEDED(arr->BindToHandler(nullptr, kBHID_DataObject, IID_IDataObject, (void**)&dobj)) && dobj) {
                DWORD eff = 0;
                SHDoDragDrop(g_hwnd, dobj, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
                dobj->Release();
            }
            arr->Release();
        }
    }
    for (auto p : ids) CoTaskMemFree(p);
    SetTimer(g_hwnd, T_SHELFCHECK, 1500, nullptr);
}

// «Жидкий» край: каждые 90 мс смотрим, не подкрался ли курсор снизу к острову.
// Стоит копейки: один GetCursorPos, рисуем только если что-то поменялось.
static void UpdateMetrics();
static void PollCursorPull() {
    if (!g_visible || g_toolHidden) return;
    POINT cp;
    GetCursorPos(&cp);
    CritterLook(cp);
    if (Sty() == ST_GLASS && !g_hover) {  // блик стекла поворачивается к мыши
        float lx = Clamp((float)(cp.x - g_winPos.x) / g_scale - g_L.px, -200.f, g_L.pw + 200.f) / std::max(1.f, g_L.pw);
        lx = Clamp(0.5f + (lx - 0.5f) * 0.6f, 0.f, 1.f);
        if (std::fabs(lx - g_lightX) > 0.03f) { g_lightX = lx; g_dirty = true; }
    }
    double now = NowSec();
    bool collapsedish = g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_DL || g_mode == M_TIMER;
    // остров на мониторе с мышью
    if (g_set.followMouse && collapsedish && !g_hover && !g_dropActive) {
        HMONITOR m = MonitorFromPoint(cp, MONITOR_DEFAULTTOPRIMARY);
        if (m != g_curMon) UpdateMetrics();
    }
    // автоскрытие за край экрана
    {
        bool music = g_media.has && g_media.status == 4;
        bool hideable = g_set.autoHide && (g_mode == M_IDLE || g_mode == M_DL || ((g_mode == M_COMPACT || g_mode == M_LYRICS) &&
                                                                               !(g_set.autoHideMusic && music)));
        float cx = (g_mon.left + g_mon.right) / 2.f;
        float half = (g_L.pw / 2 + 70) * g_scale;
        bool hidden = sHide.x > 0.5;
        float band = hidden ? 3.f : (g_set.topOffset + g_L.ph + 16) * g_scale;
        bool isNear = cp.y <= g_mon.top + band && cp.y >= g_mon.top - 2 && std::fabs(cp.x - cx) <= half;
        if (!hideable || g_hover || isNear || g_dropActive) g_nearT = now;
        double target = (hideable && now - g_nearT > 0.8) ? 1.0 : 0.0;
        if (target != sHide.target) { sHide.target = target; g_dirty = true; }
    }
    bool allow = g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_TIMER || g_mode == M_DL ||
                 g_mode == M_REC || g_mode == M_CALL;
    float target = 0, px = g_pullX;
    if (allow) {
        float cx = (cp.x - g_winPos.x) / g_scale, cy = (cp.y - g_winPos.y) / g_scale;
        float bottom = g_L.py + g_L.ph;
        float lx = cx - g_L.px;
        const float R = 64;
        if (lx > -20 && lx < g_L.pw + 20 && cy > bottom - 2 && cy < bottom + R) {
            float k = 1 - (cy - bottom) / R;
            target = 7.f * k * k;
            px = Clamp(lx, 0.f, g_L.pw);
        }
    }
    if (std::fabs(target - (float)sPull.target) > 0.05f || (target > 0 && std::fabs(px - g_pullX) > 0.5f)) {
        sPull.target = target;
        if (target > 0) g_pullX = px;
        g_dirty = true;
    }
}

