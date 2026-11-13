// setup.cpp — установщик Zen Island: один exe, внутри которого лежит ZenIsland.exe.
//   ZenIsland-Setup.exe                — окно установки (папка, ярлыки, автозапуск)
//   ZenIsland-Setup.exe /S [/D=папка]  — тихая установка/обновление (так ставит автообновление), после — запуск острова
// Ставит без прав администратора (в папку пользователя или, если есть, на диск D), пишет себя
// в «Установленные приложения», удаление делает сам остров: ZenIsland.exe --uninstall
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <string>
#include <vector>

static const wchar_t* kName = L"Zen Island";
static const wchar_t* kVer = ZEN_VERSION;  // из src/version.h
static const wchar_t* kExe = L"ZenIsland.exe";
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZenIsland";
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"DerkIsland";  // то же имя, что использует сам остров

static HINSTANCE g_inst;
static HWND g_wnd, g_edDir, g_cbDesk, g_cbAuto, g_cbRun, g_btnGo, g_status;
static HFONT g_font, g_fontBig;
static HICON g_iconBig;
static float g_k = 1.f;  // масштаб DPI
static int S(int v) { return (int)(v * g_k + 0.5f); }

static std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* val) {
    wchar_t b[1024];
    DWORD sz = sizeof(b);
    if (RegGetValueW(root, key, val, RRF_RT_REG_SZ, nullptr, b, &sz) == ERROR_SUCCESS) return b;
    return L"";
}
static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}
static std::wstring DefaultDir() {
    std::wstring prev = RegStr(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation");
    if (!prev.empty()) return prev;
    UINT t = GetDriveTypeW(L"D:\\");
    if (t == DRIVE_FIXED) return L"D:\\Zen Island";  // второй диск есть — ставим туда, системный не засоряем
    return KnownDir(FOLDERID_LocalAppData) + L"\\Programs\\Zen Island";
}

// ---- остров, который сейчас запущен (его надо закрыть и, может быть, забрать его данные)
static std::wstring RunningIslandDir() {
    HWND w = FindWindowW(L"DerkDynamicIsland", nullptr);
    if (!w) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    std::wstring r;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t b[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, b, &n)) { r.assign(b, n); r = r.substr(0, r.find_last_of(L'\\')); }
        CloseHandle(h);
    }
    return r;
}
static void CloseIsland() {
    for (int i = 0; i < 50; i++) {
        HWND w = FindWindowW(L"DerkDynamicIsland", nullptr);
        if (!w) break;
        if (i == 0) PostMessageW(w, WM_CLOSE, 0, 0);
        Sleep(100);
    }
    // ждём, пока процесс действительно отпустит exe
    for (int i = 0; i < 50; i++) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\DerkDynamicIsland");
        if (!m) break;
        CloseHandle(m);
        Sleep(100);
    }
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
// копируем папку целиком (данные старой версии: история буфера, скриншоты)
static void CopyTree(const std::wstring& from, const std::wstring& to) {
    SHCreateDirectoryExW(nullptr, to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CopyTree(from + L"\\" + n, to + L"\\" + n);
        else CopyFileW((from + L"\\" + n).c_str(), (to + L"\\" + n).c_str(), TRUE);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& dir) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    sl->SetDescription(L"Остров сверху экрана: музыка, уведомления, запись");
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

struct Opts { std::wstring dir; bool desk = true, autostart = true, run = true, silent = false; };

// главный шаг: возвращает текст ошибки или пустую строку
static std::wstring Install(const Opts& o) {
    std::wstring dir = o.dir;
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.size() < 3) return L"Выбери папку";
    std::wstring oldDir = RunningIslandDir();
    CloseIsland();
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) != ERROR_SUCCESS && !Exists(dir))
        return L"Не получилось создать папку " + dir + L" — выбери другую";
    // остров лежит внутри установщика ресурсом
    HRSRC rs = FindResourceW(g_inst, MAKEINTRESOURCEW(100), RT_RCDATA);
    HGLOBAL hg = rs ? LoadResource(g_inst, rs) : nullptr;
    const void* data = hg ? LockResource(hg) : nullptr;
    DWORD size = rs ? SizeofResource(g_inst, rs) : 0;
    if (!data || !size) return L"Установщик повреждён — скачай заново";
    std::wstring exe = dir + L"\\" + kExe, tmp = exe + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"Нет доступа к папке " + dir;
    DWORD wr = 0;
    bool ok = WriteFile(f, data, size, &wr, nullptr) && wr == size;
    CloseHandle(f);
    if (!ok) { DeleteFileW(tmp.c_str()); return L"Не хватило места на диске"; }
    bool moved = false;
    for (int i = 0; i < 30 && !(moved = MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)); i++) Sleep(200);
    if (!moved) { DeleteFileW(tmp.c_str()); return L"Старый остров ещё работает — закрой его в трее и попробуй снова"; }
    // данные старой версии из другой папки — забираем, если на новом месте их ещё нет
    if (!oldDir.empty() && _wcsicmp(oldDir.c_str(), dir.c_str()) != 0) {
        for (const wchar_t* sub : {L"data", L"screenshots", L"cache"})
            if (Exists(oldDir + L"\\" + sub) && !Exists(dir + L"\\" + sub)) CopyTree(oldDir + L"\\" + sub, dir + L"\\" + sub);
    }
    // ярлыки
    std::wstring programs = KnownDir(FOLDERID_Programs);
    if (!programs.empty()) MakeShortcut(programs + L"\\Zen Island.lnk", exe, dir);
    std::wstring desk = KnownDir(FOLDERID_Desktop) + L"\\Zen Island.lnk";
    if (!o.silent) {
        if (o.desk) MakeShortcut(desk, exe, dir);
        else DeleteFileW(desk.c_str());
    } else if (Exists(desk)) MakeShortcut(desk, exe, dir);  // при обновлении — только обновляем путь
    // автозапуск
    std::wstring q = L"\"" + exe + L"\"";
    bool hadAuto = !RegStr(HKEY_CURRENT_USER, kRunKey, kRunName).empty();
    if ((!o.silent && o.autostart) || (o.silent && hadAuto))
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, q.c_str(), (DWORD)((q.size() + 1) * sizeof(wchar_t)));
    else if (!o.silent) RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
    // «Установленные приложения»
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        auto sv = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)); };
        auto dv = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, 4); };
        sv(L"DisplayName", kName);
        sv(L"DisplayVersion", kVer);
        sv(L"Publisher", L"derk");
        sv(L"DisplayIcon", exe + L",0");
        sv(L"InstallLocation", dir);
        sv(L"UninstallString", q + L" --uninstall");
        sv(L"QuietUninstallString", q + L" --uninstall --quiet");
        sv(L"URLInfoAbout", L"https://github.com/etoderk1/zen-island");
        dv(L"NoModify", 1);
        dv(L"NoRepair", 1);
        dv(L"EstimatedSize", size / 1024 + 200);
        RegCloseKey(k);
    }
    if (o.run) {
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        std::wstring cmd = q + (o.silent ? L" --updated" : L" --welcome");
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    return L"";
}

// ---- окно
enum { ID_DIR = 10, ID_BROWSE, ID_DESK, ID_AUTO, ID_RUN, ID_GO, ID_CANCEL };
static void PickDir() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Куда установить Zen Island");
    if (SUCCEEDED(d->Show(g_wnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                std::wstring s = p;
                CoTaskMemFree(p);
                if (s.find(L"Zen Island") == std::wstring::npos) s += (s.back() == L'\\' ? L"" : L"\\") + std::wstring(L"Zen Island");
                SetWindowTextW(g_edDir, s.c_str());
            }
            it->Release();
        }
    }
    d->Release();
}
static HWND Ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: {
            g_wnd = h;
            Ctl(L"STATIC", L"Папка для программы:", 0, 24, 108, 400, 20, 0);
            g_edDir = Ctl(L"EDIT", DefaultDir().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, 130, 330, 26, ID_DIR);
            Ctl(L"BUTTON", L"Обзор…", BS_PUSHBUTTON | WS_TABSTOP, 362, 129, 94, 28, ID_BROWSE);
            g_cbDesk = Ctl(L"BUTTON", L"Ярлык на рабочем столе", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 172, 420, 22, ID_DESK);
            g_cbAuto = Ctl(L"BUTTON", L"Запускать вместе с Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 198, 420, 22, ID_AUTO);
            g_cbRun = Ctl(L"BUTTON", L"Запустить после установки", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 224, 420, 22, ID_RUN);
            SendMessageW(g_cbDesk, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(g_cbAuto, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(g_cbRun, BM_SETCHECK, BST_CHECKED, 0);
            g_status = Ctl(L"STATIC", L"", 0, 24, 262, 432, 20, 0);
            g_btnGo = Ctl(L"BUTTON", L"Установить", BS_DEFPUSHBUTTON | WS_TABSTOP, 254, 292, 110, 32, ID_GO);
            Ctl(L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, 372, 292, 84, 32, ID_CANCEL);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            RECT top = {0, 0, rc.right, S(90)};
            HBRUSH br = CreateSolidBrush(RGB(28, 26, 52));
            FillRect(dc, &top, br);
            DeleteObject(br);
            DrawIconEx(dc, S(22), S(17), g_iconBig, S(56), S(56), 0, nullptr, DI_NORMAL);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            HGDIOBJ old = SelectObject(dc, g_fontBig);
            RECT t = {S(92), S(18), rc.right - S(10), S(50)};
            DrawTextW(dc, kName, -1, &t, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, g_font);
            SetTextColor(dc, RGB(190, 185, 230));
            std::wstring sub = std::wstring(L"Версия ") + kVer + L" · остров сверху экрана для Windows";
            RECT t2 = {S(92), S(50), rc.right - S(10), S(74)};
            DrawTextW(dc, sub.c_str(), -1, &t2, DT_LEFT | DT_SINGLELINE);
            SelectObject(dc, old);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_BROWSE: PickDir(); break;
                case ID_CANCEL: DestroyWindow(h); break;
                case ID_GO: {
                    Opts o;
                    wchar_t b[1024];
                    GetWindowTextW(g_edDir, b, 1024);
                    o.dir = b;
                    o.desk = SendMessageW(g_cbDesk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    o.autostart = SendMessageW(g_cbAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    o.run = SendMessageW(g_cbRun, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    EnableWindow(g_btnGo, FALSE);
                    SetWindowTextW(g_status, L"Устанавливаю…");
                    UpdateWindow(h);
                    std::wstring err = Install(o);
                    if (err.empty()) {
                        MessageBoxW(h, L"Готово! Остров уже сверху экрана — кликни по нему.\nНастройки — за шестерёнкой.", kName, MB_ICONINFORMATION);
                        DestroyWindow(h);
                    } else {
                        SetWindowTextW(g_status, err.c_str());
                        EnableWindow(g_btnGo, TRUE);
                    }
                    break;
                }
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    g_inst = hi;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring c = cmd ? cmd : L"";
    if (c.find(L"/S") != std::wstring::npos) {  // тихо: так обновляет сам остров
        Opts o;
        o.silent = true;
        size_t d = c.find(L"/D=");
        o.dir = d != std::wstring::npos ? c.substr(d + 3) : DefaultDir();
        while (!o.dir.empty() && (o.dir.back() == L'"' || o.dir.back() == L' ')) o.dir.pop_back();
        if (!o.dir.empty() && o.dir[0] == L'"') o.dir.erase(0, 1);
        std::wstring err = Install(o);
        if (!err.empty()) MessageBoxW(nullptr, (L"Обновление не установилось: " + err).c_str(), kName, MB_ICONWARNING);
        return err.empty() ? 0 : 1;
    }
    HDC sdc = GetDC(nullptr);
    g_k = GetDeviceCaps(sdc, LOGPIXELSX) / 96.f;
    ReleaseDC(nullptr, sdc);
    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBig = CreateFontW(-S(26), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_iconBig = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 256, 256, 0);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"ZenIslandSetup";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    RECT r = {0, 0, S(480), S(342)};
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, st, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Установка Zen Island", st, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                               (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, hi, nullptr);
    ShowWindow(wnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
