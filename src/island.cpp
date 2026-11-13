// island.cpp — Dynamic Island для Windows — Zen Island 1.7
//
// Как устроено и почему это лёгкое:
//  * Одно Win32-окно WS_EX_LAYERED + UpdateLayeredWindow с попиксельной альфой.
//    Прозрачные пиксели не ловят мышь — клики проходят сквозь всё, кроме самой пилюли.
//  * Окно каждый кадр подгоняется под размер пилюли, рисуем Direct2D в программном
//    режиме прямо в маленький DIB — без GPU-readback.
//  * Кадры рисуются ТОЛЬКО пока что-то двигается. В покое поток спит в GetMessage
//    (0% CPU), часы будит таймер раз в минуту.
//  * Медиа читается в отдельном потоке с пониженным приоритетом раз в секунду.
//  * В полноэкранных играх/видео остров прячется и ничего не рисует.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <string>
#include <set>
#include <vector>

#include "media.h"
#include "lyrics.h"
#include "viz.h"
#include "sysevents.h"
#include "clipstore.h"
#include "tools.h"
#include "recorder.h"
#include "downloads.h"
#include "widgets.h"
#include "notify.h"
#include "texttools.h"
#include "reminders.h"
#include "sysctl.h"
#include "cmdlogic.h"
#include "cmdindex.h"
#include "editor.h"
#include "pods.h"
#include "sysstats.h"
#include "vpn.h"
#include "fps.h"
#include "version.h"
#include "update.h"
#include "log.h"
#include <cwctype>
#include "net.h"
#include <memory>
#include <mmsystem.h>
#include <wincodec.h>
#include <richedit.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <oleidl.h>
#include <thread>
#include <map>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#define WM_APP_TRAY (WM_APP + 2)

enum { T_CLOCK = 1, T_TICK, T_FS, T_PEEK, T_COLLAPSE, T_AUDIO, T_VOL, T_LYR, T_TOAST, T_CAPS, T_LAYOUT, T_CLIPREAD,
       T_PASTE, T_CARET, T_MEDIAPOKE, T_TOOLGO, T_REC, T_SHELFCHECK, T_TIMER, T_NOTIF, T_CALL, T_HOVER, T_CURSOR,
       T_REMIND, T_NOTESAVE, T_SETTEXT, T_CMDBLUR, T_STATS, T_PODSMISS, T_GLASS, T_FX, T_NY, T_VPN, T_BLINK, T_MEDIAHIDE, T_RECCD, T_GAMETOP, T_UPDATE, T_WELCOME };
enum { ID_ANIM = 100, ID_AUTO, ID_EXIT, ID_SETTINGS, ID_LYRICS, ID_CLIP, ID_SHOT, ID_OCR, ID_REC, ID_SHOTDIR, ID_PASS };
enum { HK_CLIP = 1, HK_SHOT, HK_OCR, HK_REC, HK_PICK, HK_CMD };
#define WM_APP_WORK (WM_APP + 10)  // результат OCR/QR из фонового потока

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ----------------------------------------------------------------------------
// Утилиты
// ----------------------------------------------------------------------------
static double NowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static D2D1_COLOR_F Col(float r, float g, float b, float a = 1.f) { return D2D1_COLOR_F{r, g, b, a}; }
static D2D1_COLOR_F WithA(D2D1_COLOR_F c, float a) { c.a *= a; return c; }
static D2D1_COLOR_F Lerp(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1_COLOR_F{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
// ---- Тема: всё, что зависит от светлой/тёмной, живёт здесь ----
static bool g_light = false;
static D2D1_COLOR_F kWhite = {0.97f, 0.97f, 0.98f, 1.f};    // основной текст/иконки
static D2D1_COLOR_F kGray = {0.62f, 0.62f, 0.67f, 1.f};     // второстепенный текст
static D2D1_COLOR_F kBgSolid = {0.015f, 0.015f, 0.02f, 1.f}; // фон острова
static D2D1_COLOR_F kEdCol = {0.085f, 0.085f, 0.095f, 1.f};  // поле ввода
static D2D1_COLOR_F kCard = {1, 1, 1, 0.055f}, kCardHot = {1, 1, 1, 0.10f};
static D2D1_COLOR_F Ov(float a) { return g_light ? D2D1_COLOR_F{0, 0, 0.04f, a * 0.85f} : D2D1_COLOR_F{1, 1, 1, a}; }
static void SetThemeColors(bool light) {
    g_light = light;
    if (light) {
        kWhite = {0.07f, 0.07f, 0.09f, 1.f};
        kGray = {0.40f, 0.40f, 0.45f, 1.f};
        kBgSolid = {0.955f, 0.955f, 0.965f, 1.f};
        kEdCol = {1.f, 1.f, 1.f, 1.f};
    } else {
        kWhite = {0.97f, 0.97f, 0.98f, 1.f};
        kGray = {0.62f, 0.62f, 0.67f, 1.f};
        kBgSolid = {0.015f, 0.015f, 0.02f, 1.f};
        kEdCol = {0.085f, 0.085f, 0.095f, 1.f};
    }
    kCard = Ov(0.055f);
    kCardHot = Ov(0.10f);
}
static D2D1_RECT_F RF(float x, float y, float w, float h) { return D2D1_RECT_F{x, y, x + w, y + h}; }
static D2D1_POINT_2F Pt(float x, float y) { return D2D1_POINT_2F{x, y}; }
template <class T> static T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float Smooth(double t) { float c = (float)Clamp(t, 0.0, 1.0); return c * c * (3 - 2 * c); }

// Пружина (как в iOS): немного перелёта, быстро успокаивается
struct Spring {
    double x = 0, v = 0, target = 0;
    void Step(double dt, double k = 330, double zeta = 0.74) {
        double c = 2 * std::sqrt(k) * zeta;
        int n = std::max(1, (int)std::ceil(dt * 240));
        double h = dt / n;
        for (int i = 0; i < n; i++) {
            double a = -k * (x - target) - c * v;
            v += a * h;
            x += v * h;
        }
        if (Settled()) { x = target; v = 0; }
    }
    bool Settled() const { return std::fabs(x - target) < 0.004 && std::fabs(v) < 0.05; }
    void Set(double val) { x = target = val; v = 0; }
};
// Для пикселей хватает грубой точности
static bool SettledPx(const Spring& s) { return std::fabs(s.x - s.target) < 0.06 && std::fabs(s.v) < 0.6; }

// ----------------------------------------------------------------------------
// Настройки (реестр HKCU\Software\DerkIsland)
// ----------------------------------------------------------------------------
struct Settings {
    int sizePct = 100;        // 80..140
    int topOffset = 0;        // DIP (0 — остров «прирос» к краю экрана)
    std::wstring monitor;     // \\.\DISPLAYn, пусто = основной
    bool audioAnim = true;
    bool peek = true;
    bool lyrics = true;       // текст песен из LRCLIB
    int lyricOffsetMs = 0;    // сдвиг текста, -2000..2000
    bool volHook = true;      // свой индикатор громкости вместо системного
    bool sysToasts = true;    // наушники, зарядка, яркость, Caps Lock, раскладка
    bool privacy = true;      // точка «микрофон/камера используются»
    bool clip = true;         // история буфера обмена
    int clipLimit = 200;      // сколько записей хранить (закреплённые не считаются)
    bool tools = true;        // инструменты экрана + горячие клавиши
    bool downloads = true;    // следить за папкой «Загрузки»
    bool shelf = true;        // полка для файлов
    bool widgets = true;      // погода, таймер, календарь в развёрнутом острове
    std::wstring city;        // город для погоды (пусто — по IP)
    std::wstring icsUrl;      // ICS-ссылка календаря
    bool calRemind = true;    // напоминать о событиях за 10 минут
    bool notifs = true;       // уведомления Windows в острове
    bool notifPrivate = false;// не показывать текст уведомлений
    bool calls = true;        // звонки Discord/Telegram/…: таймер и кнопка микрофона
    bool hoverOpen = false;   // раскрывать при наведении (иначе — по клику)
    int theme = 0;            // 0 тёмная, 1 светлая, 2 как в Windows
    int accentMode = 0;       // 0 из обложки, 1 системный, 2 свой
    int accentIdx = 0;        // свой цвет из палитры
    int opacity = 97;         // непрозрачность фона, %
    std::wstring tabOrder = L"0123456";
    int tabHidden = 0;        // битовая маска скрытых вкладок
    bool autoHide = false;    // прятать за край экрана
    bool autoHideMusic = true;// …но не когда играет музыка
    bool dnd = false;         // не беспокоить
    bool dndCalls = true;     // не беспокоить во время звонков
    bool followMouse = false; // остров на мониторе с мышью
    bool cmd = true;          // командная строка Alt+Space
    int search = 1;           // 0 Google, 1 Яндекс
    bool markup = true;       // редактор после снимка области
    int idleWidget = 0;       // в покое справа/слева: 0 погода, 1 сеть, 2 CPU и GPU
    int style = 0;            // пресет дизайна (style.inc)
    int anim = 1;             // 0 спокойные, 1 живые, 2 пружинистые
    int font = 0;             // 0 Segoe UI, 1 Segoe UI Variable, 2 Bahnschrift, 3 Consolas
    // свой стиль (конструктор)
    int cShape = 0, cBg = 0, cBorder = 1, cRadius = 100, cBorderW = 1;
    bool cShadow = true, cSeason = false;
    // фишки
    std::wstring exclude;     // в каких программах прятать остров (exe через запятую)
    bool passThrough = false; // клики проходят сквозь остров
    bool vpn = true;          // следить за VPN
    bool rainAlert = true;    // предупреждать о дожде
    int critter = 1;          // питомец: 0 нет, 1 Капля (по умолчанию) … 8
    bool petReact = true;     // реакции и «бытовые» движения
    bool petSleep = true;     // засыпает, когда ты отошёл
    // запись экрана
    int recFps = 0;           // 0 — 30 к/с, 1 — 60 к/с
    int recQuality = 1;       // 0 эконом, 1 обычное, 2 высокое
    int recArea = 0;          // 0 — окно, 1 — весь экран, 2 — область
    int recSysVol = 100, recMicVol = 100;  // микшер, %
    std::wstring recMicId, recOutId;        // устройства для записи (пусто — как в Windows)
    bool recCursor = true, recSys = true, recMic = false, recCountdown = true;
    std::wstring recDir;      // пусто — «Видео\\Zen Island»
    DWORD recHotkey = ((MOD_CONTROL | MOD_ALT) << 16) | 'R';
    int gameHud = 1;          // в полноэкранной игре: 0 прятать остров, 1 показывать FPS и нагрузку
    bool gameFps = true;      // считать FPS (нужны права администратора)
    bool gameSummary = true;  // итоги игры после выхода
    bool autoUpdate = true;   // сам проверяет обновления на GitHub
    std::wstring updateRepo;  // «владелец/репозиторий» (пусто — из version.h)
    bool welcomed = false;    // знакомство уже показали
    bool browserMusicOnly = true;  // из браузеров — только музыка, не видео
    int pauseHide = 2;        // прятать плеер на паузе: 0 никогда, 1 сразу, 2 через 1 мин, 3 через 5 мин, 4 через 15 мин
    std::wstring mediaBlock;  // |appId| источников, которые не показываем
    std::wstring mediaSeen;   // appId=имя;… — какие источники видели
    bool fxOutline = true, fxSplit = true, fxRoll = true, fxSounds = true, fxParticles = true, fxPull = true, fxNY = true;
};
static Settings g_set;
static const wchar_t* kAppName = L"Zen Island";
static bool g_launchWelcome = false, g_launchUpdated = false, g_launchCrashed = false;
static const wchar_t* kAppVer = ZEN_VERSION;
static const wchar_t* kRegKey = L"Software\\DerkIsland";  // старое имя ключа — чтобы настройки сохранились
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunName = L"DerkIsland";

static DWORD RegDw(const wchar_t* name, DWORD def) {
    DWORD v = def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return def;
    return v;
}
static std::wstring RegStr(const wchar_t* name) {
    wchar_t buf[2048] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}
static void SetRegStr(const wchar_t* name, const std::wstring& v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
static void LoadSettings() {
    g_set.audioAnim = RegDw(L"AudioAnim", 1) != 0;
    g_set.peek = RegDw(L"Peek", 1) != 0;
    g_set.sizePct = Clamp((int)RegDw(L"SizePct", 100), 80, 200);
    g_set.topOffset = Clamp((int)RegDw(L"TopOffset", 0), 0, 40);
    if (RegDw(L"ShapeV", 0) < 1) {  // 0.7: новая форма «чёлки» — прижимаем к краю один раз
        g_set.topOffset = 0;
        DWORD one = 1, z = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"ShapeV", REG_DWORD, &one, sizeof(one));
        RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"TopOffset", REG_DWORD, &z, sizeof(z));
    }
    g_set.hoverOpen = RegDw(L"HoverOpen2", 0) != 0;  // с 1.4 по умолчанию открываем кликом
    g_set.theme = Clamp((int)RegDw(L"Theme", 0), 0, 2);
    g_set.accentMode = Clamp((int)RegDw(L"AccentMode", 0), 0, 2);
    g_set.accentIdx = Clamp((int)RegDw(L"AccentIdx", 0), 0, 7);
    g_set.opacity = Clamp((int)RegDw(L"Opacity", 97), 60, 100);
    g_set.tabHidden = ((int)RegDw(L"TabHidden", 0) & 0x7F) | (1 << 4);  // «Заметка» (с таймером и напоминаниями) пока убрана
    g_set.autoHide = RegDw(L"AutoHide", 0) != 0;
    g_set.autoHideMusic = RegDw(L"AutoHideMusic", 1) != 0;
    g_set.dnd = RegDw(L"Dnd", 0) != 0;
    g_set.dndCalls = RegDw(L"DndCalls", 1) != 0;
    g_set.followMouse = RegDw(L"FollowMouse", 0) != 0;
    g_set.cmd = RegDw(L"Cmd", 1) != 0;
    g_set.search = Clamp((int)RegDw(L"Search", 1), 0, 1);
    g_set.markup = RegDw(L"Markup", 1) != 0;
    g_set.idleWidget = Clamp((int)RegDw(L"IdleWidget", 0), 0, 2);
    g_set.anim = Clamp((int)RegDw(L"Anim", 1), 0, 2);
    g_set.style = Clamp((int)RegDw(L"Style", 0), 0, 12);
    g_set.font = Clamp((int)RegDw(L"Font", 0), 0, 3);
    g_set.cShape = Clamp((int)RegDw(L"CShape", 0), 0, 2);
    g_set.cBg = Clamp((int)RegDw(L"CBg", 0), 0, 5);
    g_set.cBorder = Clamp((int)RegDw(L"CBorder", 1), 0, 5);
    g_set.cRadius = Clamp((int)RegDw(L"CRadius", 100), 0, 100);
    g_set.cBorderW = Clamp((int)RegDw(L"CBorderW", 1), 1, 4);
    g_set.cShadow = RegDw(L"CShadow", 1) != 0;
    g_set.cSeason = RegDw(L"CSeason", 0) != 0;
    g_set.fxOutline = RegDw(L"FxOutline", 1) != 0;
    g_set.fxSplit = RegDw(L"FxSplit", 1) != 0;
    g_set.fxRoll = RegDw(L"FxRoll", 1) != 0;
    g_set.fxSounds = RegDw(L"FxSounds", 1) != 0;
    g_set.fxParticles = RegDw(L"FxParticles", 1) != 0;
    g_set.fxPull = RegDw(L"FxPull", 1) != 0;
    g_set.fxNY = RegDw(L"FxNY", 1) != 0;
    g_set.exclude = RegStr(L"Exclude");
    g_set.passThrough = RegDw(L"PassThrough", 0) != 0;
    g_set.vpn = RegDw(L"Vpn", 1) != 0;
    g_set.rainAlert = RegDw(L"RainAlert", 1) != 0;
    g_set.critter = Clamp((int)RegDw(L"Critter", 1), 0, 8);
    g_set.petReact = RegDw(L"PetReact", 1) != 0;
    g_set.petSleep = RegDw(L"PetSleep", 1) != 0;
    g_set.recFps = Clamp((int)RegDw(L"RecFps", 0), 0, 1);
    g_set.recQuality = Clamp((int)RegDw(L"RecQuality", 1), 0, 2);
    g_set.recArea = Clamp((int)RegDw(L"RecMode", 0), 0, 2);
    g_set.recSysVol = Clamp((int)RegDw(L"RecSysVol", 100), 0, 200);
    g_set.recMicVol = Clamp((int)RegDw(L"RecMicVol", 100), 0, 200);
    g_set.recCursor = RegDw(L"RecCursor", 1) != 0;
    g_set.recSys = RegDw(L"RecSys", 1) != 0;
    g_set.recMic = RegDw(L"RecMic", 0) != 0;
    g_set.recCountdown = RegDw(L"RecCountdown", 1) != 0;
    g_set.recDir = RegStr(L"RecDir");
    g_set.recMicId = RegStr(L"RecMicId");
    g_set.recOutId = RegStr(L"RecOutId");
    g_set.recHotkey = RegDw(L"RecHotkey", ((MOD_CONTROL | MOD_ALT) << 16) | 'R');
    if (!RegDw(L"RecTabShown", 0)) {  // на месте «Полки» теперь «Запись» — показываем её, даже если полку прятали
        g_set.tabHidden &= ~(1 << 2);
        DWORD one = 1;
        RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"RecTabShown", REG_DWORD, &one, sizeof(one));
        DWORD th = (DWORD)g_set.tabHidden;
        RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"TabHidden", REG_DWORD, &th, sizeof(th));
    }
    g_set.gameHud = Clamp((int)RegDw(L"GameHud", 1), 0, 1);
    g_set.gameFps = RegDw(L"GameFps", 1) != 0;
    g_set.gameSummary = RegDw(L"GameSummary", 1) != 0;
    g_set.autoUpdate = RegDw(L"AutoUpdate", 1) != 0;
    g_set.updateRepo = RegStr(L"UpdateRepo");
    g_set.welcomed = RegDw(L"Welcomed", 0) != 0;
    g_set.browserMusicOnly = RegDw(L"BrowserMusicOnly", 1) != 0;
    g_set.pauseHide = Clamp((int)RegDw(L"PauseHide", 2), 0, 4);
    g_set.mediaBlock = RegStr(L"MediaBlock");
    g_set.mediaSeen = RegStr(L"MediaSeen");
    {
        std::wstring o = RegStr(L"TabOrder");
        bool good = o.size() == 7;
        for (wchar_t c = L'0'; c <= L'6' && good; c++) good = o.find(c) != std::wstring::npos;
        if (good) g_set.tabOrder = o;
    }
    g_set.lyrics = RegDw(L"Lyrics", 1) != 0;
    g_set.lyricOffsetMs = Clamp((int)RegDw(L"LyricOffset", 0), -2000, 2000);
    g_set.volHook = RegDw(L"VolHook", 1) != 0;
    g_set.sysToasts = RegDw(L"SysToasts", 1) != 0;
    g_set.privacy = RegDw(L"Privacy", 1) != 0;
    g_set.clip = RegDw(L"Clipboard", 1) != 0;
    g_set.clipLimit = Clamp((int)RegDw(L"ClipLimit", 200), 50, 1000);
    g_set.tools = RegDw(L"Tools", 1) != 0;
    g_set.downloads = RegDw(L"Downloads", 1) != 0;
    g_set.shelf = false;  // полку убрали в 1.4 — на её месте вкладка «Запись»
    g_set.widgets = RegDw(L"Widgets", 1) != 0;
    g_set.calRemind = RegDw(L"CalRemind", 1) != 0;
    g_set.notifs = RegDw(L"Notifs", 1) != 0;
    g_set.notifPrivate = RegDw(L"NotifPrivate", 0) != 0;
    g_set.calls = RegDw(L"Calls", 1) != 0;
    g_set.city = RegStr(L"City");
    g_set.icsUrl = RegStr(L"IcsUrl");
    wchar_t buf[64] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Monitor", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        g_set.monitor = buf;
}
static void SaveSettings() {
    auto dw = [](const wchar_t* n, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, n, REG_DWORD, &v, sizeof(v)); };
    dw(L"AudioAnim", g_set.audioAnim);
    dw(L"Peek", g_set.peek);
    dw(L"SizePct", (DWORD)g_set.sizePct);
    dw(L"TopOffset", (DWORD)g_set.topOffset);
    dw(L"HoverOpen2", g_set.hoverOpen);
    dw(L"Theme", (DWORD)g_set.theme);
    dw(L"AccentMode", (DWORD)g_set.accentMode);
    dw(L"AccentIdx", (DWORD)g_set.accentIdx);
    dw(L"Opacity", (DWORD)g_set.opacity);
    dw(L"TabHidden", (DWORD)g_set.tabHidden);
    dw(L"AutoHide", g_set.autoHide);
    dw(L"AutoHideMusic", g_set.autoHideMusic);
    dw(L"Dnd", g_set.dnd);
    dw(L"DndCalls", g_set.dndCalls);
    dw(L"FollowMouse", g_set.followMouse);
    dw(L"Cmd", g_set.cmd);
    dw(L"Search", (DWORD)g_set.search);
    dw(L"Markup", g_set.markup);
    dw(L"IdleWidget", (DWORD)g_set.idleWidget);
    dw(L"Style", (DWORD)g_set.style);
    dw(L"Anim", (DWORD)g_set.anim);
    dw(L"Font", (DWORD)g_set.font);
    dw(L"CShape", (DWORD)g_set.cShape);
    dw(L"CBg", (DWORD)g_set.cBg);
    dw(L"CBorder", (DWORD)g_set.cBorder);
    dw(L"CRadius", (DWORD)g_set.cRadius);
    dw(L"CBorderW", (DWORD)g_set.cBorderW);
    dw(L"CShadow", g_set.cShadow);
    dw(L"CSeason", g_set.cSeason);
    dw(L"FxOutline", g_set.fxOutline);
    dw(L"FxSplit", g_set.fxSplit);
    dw(L"FxRoll", g_set.fxRoll);
    dw(L"FxSounds", g_set.fxSounds);
    dw(L"FxParticles", g_set.fxParticles);
    dw(L"FxPull", g_set.fxPull);
    dw(L"FxNY", g_set.fxNY);
    SetRegStr(L"Exclude", g_set.exclude);
    dw(L"PassThrough", g_set.passThrough);
    dw(L"Vpn", g_set.vpn);
    dw(L"RainAlert", g_set.rainAlert);
    dw(L"Critter", (DWORD)g_set.critter);
    dw(L"PetReact", g_set.petReact);
    dw(L"PetSleep", g_set.petSleep);
    dw(L"GameHud", (DWORD)g_set.gameHud);
    dw(L"RecFps", (DWORD)g_set.recFps);
    dw(L"RecQuality", (DWORD)g_set.recQuality);
    dw(L"RecMode", (DWORD)g_set.recArea);
    dw(L"RecSysVol", (DWORD)g_set.recSysVol);
    dw(L"RecMicVol", (DWORD)g_set.recMicVol);
    dw(L"RecCursor", g_set.recCursor);
    dw(L"RecSys", g_set.recSys);
    dw(L"RecMic", g_set.recMic);
    dw(L"RecCountdown", g_set.recCountdown);
    dw(L"RecHotkey", g_set.recHotkey);
    SetRegStr(L"RecDir", g_set.recDir);
    SetRegStr(L"RecMicId", g_set.recMicId);
    SetRegStr(L"RecOutId", g_set.recOutId);
    dw(L"GameFps", g_set.gameFps ? 1 : 0);
    dw(L"GameSummary", g_set.gameSummary ? 1 : 0);
    dw(L"AutoUpdate", g_set.autoUpdate ? 1 : 0);
    dw(L"Welcomed", g_set.welcomed ? 1 : 0);
    dw(L"BrowserMusicOnly", g_set.browserMusicOnly);
    dw(L"PauseHide", (DWORD)g_set.pauseHide);
    SetRegStr(L"MediaBlock", g_set.mediaBlock);
    SetRegStr(L"MediaSeen", g_set.mediaSeen);
    SetRegStr(L"TabOrder", g_set.tabOrder);
    dw(L"Lyrics", g_set.lyrics);
    dw(L"LyricOffset", (DWORD)g_set.lyricOffsetMs);
    dw(L"VolHook", g_set.volHook);
    dw(L"SysToasts", g_set.sysToasts);
    dw(L"Privacy", g_set.privacy);
    dw(L"Clipboard", g_set.clip);
    dw(L"ClipLimit", (DWORD)g_set.clipLimit);
    dw(L"Tools", g_set.tools);
    dw(L"Downloads", g_set.downloads);
    dw(L"Shelf", g_set.shelf);
    dw(L"Widgets", g_set.widgets);
    dw(L"CalRemind", g_set.calRemind);
    dw(L"Notifs", g_set.notifs);
    dw(L"NotifPrivate", g_set.notifPrivate);
    dw(L"Calls", g_set.calls);
    SetRegStr(L"City", g_set.city);
    SetRegStr(L"IcsUrl", g_set.icsUrl);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"Monitor", REG_SZ, g_set.monitor.c_str(),
                    (DWORD)((g_set.monitor.size() + 1) * sizeof(wchar_t)));
}
// ---- автозапуск с правами администратора через Планировщик (FPS в играх сразу после включения ПК)
// Задачу создаёт/удаляет schtasks.exe с повышением прав — Windows один раз спросит разрешение.
static bool RunElevatedWait(const std::wstring& file, const std::wstring& args) {
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    WaitForSingleObject(sei.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}
static bool SchedTaskCreate() {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n), dir = path.substr(0, path.find_last_of(L'\\'));
    wchar_t dom[256] = {}, usr[256] = {};
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);
    GetEnvironmentVariableW(L"USERNAME", usr, 256);
    std::wstring who = std::wstring(dom) + L"\\" + usr;
    auto esc = [](std::wstring t) {
        std::wstring o;
        for (wchar_t c : t) o += c == L'&' ? L"&amp;" : c == L'<' ? L"&lt;" : c == L'>' ? L"&gt;" : std::wstring(1, c);
        return o;
    };
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Zen Island с правами администратора — FPS в играх</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + esc(who) + L"</UserId><Delay>PT3S</Delay></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + esc(who) +
        L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>" + esc(path) + L"</Command><WorkingDirectory>" + esc(dir) +
        L"</WorkingDirectory></Exec></Actions>\r\n</Task>\r\n";
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring file = std::wstring(tmp) + L"ZenIslandTask.xml";
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    WORD bom = 0xFEFF;
    WriteFile(f, &bom, 2, &wr, nullptr);
    WriteFile(f, xml.data(), (DWORD)(xml.size() * 2), &wr, nullptr);
    CloseHandle(f);
    bool ok = RunElevatedWait(L"schtasks.exe", L"/Create /TN \"ZenIsland\" /XML \"" + file + L"\" /F");
    DeleteFileW(file.c_str());
    return ok;
}
static bool SchedTaskOn() {
    DWORD v = 0, sz = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\DerkIsland", L"AdminTask", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v;
}
static void SchedTaskMark(bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\DerkIsland", L"AdminTask", REG_DWORD, &v, sizeof(v));
}
static void SchedTaskRemove() {
    if (!SchedTaskOn()) return;
    RunElevatedWait(L"schtasks.exe", L"/Delete /TN \"ZenIsland\" /F");
    SchedTaskMark(false);
}
static bool IsAutostart() {
    if (SchedTaskOn()) return true;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void SetAutostart(bool on) {
    if (on && SchedTaskOn()) { RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName); return; }  // запускает Планировщик
    if (!on) SchedTaskRemove();
    if (on) {
        wchar_t path[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, path + 1, MAX_PATH);
        path[0] = L'"';
        path[n + 1] = L'"';
        path[n + 2] = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, path, (DWORD)((n + 3) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
    }
}

// ----------------------------------------------------------------------------
// Звук: пиковый уровень (для волны и эквалайзера) и громкость колёсиком
// ----------------------------------------------------------------------------
static const CLSID CLSID_MMDevEnum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID IID_MMDevEnum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID IID_Meter = {0xC02216F6, 0x8C67, 0x4B5B, {0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64}};
static const IID IID_EndpointVol = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

// В MinGW-заголовках этот интерфейс только объявлен, описываем сами
struct IAudioMeterRaw : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};

struct Audio {
    IMMDeviceEnumerator* en = nullptr;
    IAudioMeterRaw* meter = nullptr;
    IAudioEndpointVolume* vol = nullptr;
    DWORD lastTry = 0, acquired = 0;

    void Reset() { SafeRelease(meter); SafeRelease(vol); }
    bool Ensure() {
        DWORD now = GetTickCount();
        // раз в 15 с переподключаемся — на случай смены устройства вывода (наушники и т.п.)
        if (meter && vol && now - acquired < 15000) return true;
        if (meter && vol) Reset();
        if (lastTry && now - lastTry < 1500) return false;
        lastTry = now;
        if (!en) CoCreateInstance(CLSID_MMDevEnum, nullptr, CLSCTX_ALL, IID_MMDevEnum, (void**)&en);
        if (!en) return false;
        IMMDevice* d = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &d)) || !d) return false;
        d->Activate(IID_Meter, CLSCTX_ALL, nullptr, (void**)&meter);
        d->Activate(IID_EndpointVol, CLSCTX_ALL, nullptr, (void**)&vol);
        d->Release();
        acquired = now;
        if (!meter || !vol) { Reset(); return false; }
        return true;
    }
    float Peak() {
        if (!Ensure()) return 0;
        float v = 0;
        if (FAILED(meter->GetPeakValue(&v))) { Reset(); return 0; }
        return v;
    }
    void Release() { Reset(); SafeRelease(en); }
};

// ----------------------------------------------------------------------------
// Состояние приложения
// ----------------------------------------------------------------------------
enum Mode { M_IDLE, M_COMPACT, M_PEEK, M_EXPANDED, M_TOAST, M_LYRICS, M_CLIP, M_REC, M_DL, M_DROP, M_TIMER, M_CALL, M_NOTIF,
            M_CMD, M_DEV, M_GAME, M_COUNT_UNUSED };

struct Layout {
    float bw = 0, bh = 0;                          // размер окна в DIP
    float px = 0, py = 0, pw = 0, ph = 0, pr = 0;  // пилюля
    D2D1_RECT_F btn[3] = {};
    int btnCount = 0;
    D2D1_RECT_F tool[6] = {};
    int toolCount = 0;
    D2D1_RECT_F shelf[8] = {}, shelfX[8] = {}, shelfClear = {};
    int shelfCount = 0;
    bool hasProg = false;
    float progX0 = 0, progX1 = 0, progY = 0;
    D2D1_RECT_F chip[3] = {}, tPause = {}, tStop = {}, wxCard = {}, calCard = {}, mic = {};
    bool hasWidgets = false, timerOn = false;
};

// Растровая картинка (обложка/иконка) для отрисовки через bitmap brush
struct Pic {
    ID2D1Bitmap* bmp = nullptr;
    ID2D1BitmapBrush* brush = nullptr;
    float size = 1;
    void Release() { SafeRelease(brush); SafeRelease(bmp); }
};

// Бегущая строка
struct Marquee {
    std::wstring text;
    IDWriteTextFormat* fmt = nullptr;
    IDWriteTextLayout* layout = nullptr;
    float width = 0;
    double t0 = 0;
    void Release() { SafeRelease(layout); text.clear(); fmt = nullptr; }
};

static HINSTANCE g_inst;
static HWND g_hwnd, g_settingsWnd;
static HICON g_icon, g_iconBig;
static UINT g_taskbarCreated;
static RECT g_mon;
static float g_scale = 1.f;  // DPI монитора * размер из настроек

static ID2D1Factory* g_d2f;
static IDWriteFactory* g_dwf;
static ID2D1DCRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_brush;
static ID2D1Layer *g_layer, *g_layer2, *g_layer3;
static ID2D1GradientStopCollection *g_gsRight, *g_gsBoth;
static HDC g_memDC;
static HBITMAP g_dib, g_oldBmp;
static int g_dibW, g_dibH;

static IDWriteTextFormat *g_fTime, *g_fTitle, *g_fArtist, *g_fSmall, *g_fBig, *g_fDate, *g_fPeekTitle,
    *g_fPeekArtist, *g_fSource, *g_fLyric, *g_fLyricBig, *g_fLyricSmall, *g_fToast, *g_fClip, *g_fSearch, *g_fWBig, *g_fPlayerTitle, *g_fCmd, *g_fQa;

static Spring sW, sH, sR, sWave, sPull, sHide;
static double g_nearT = 0;  // когда мышь последний раз была у острова (для автоскрытия)
static float g_pullX = 0;           // где курсор тянет край (DIP от левого края пилюли)
static int g_hotId = 0, g_hotArg = -1;  // элемент панели под мышью
static int g_ieFocus = -1;            // поле ввода на острове, в котором печатают
static bool g_recPick = false;  // открыт выбор окна для записи
static int g_recPickScroll = 0;
static int g_recDrag = -1;      // тянут ползунок микшера на вкладке «Запись» (50 — звук, 51 — микрофон)
static bool g_appPick = false;  // открыт выбор программ с поиском (вкладка «Главная»)
static int g_appScroll = 0;
static void IEBlur(bool commit);
static bool g_panelOpenedPending = false;
static POINT g_winPos = {0, 0};      // где сейчас окно острова (экранные пиксели)
// ---- этапы 14–15 (логика в extras.inc) ----
#define WM_APP_REPLY (WM_APP + 25)
struct ReplyResult { bool ok = false, reply = true; std::wstring err; };
struct DevCard { std::wstring name; int bat[3] = {-1, -1, -1}; bool chg[3] = {false, false, false}; bool pods = false, single = false; };
static DevCard g_dev;
static VpnInfo g_vpn;
static bool g_vpnInit = false, g_vpnForceToast = false;
static bool g_podsWant = false, g_podsManual = false, g_podsGot = false;
static std::wstring g_podsName;
static bool g_toastPress = false;
static POINT g_toastPressPt = {};
static SysStats g_stats;
static SysSample g_ss;
static bool g_ssOk = false, g_statsOn = false;
// игровой режим: полноэкранная игра → пилюля с FPS и нагрузкой
struct GameState { bool on = false, fpsTried = false, fpsOk = false; DWORD pid = 0; std::wstring exe; float fps = -1; };
static GameState g_game;
struct GameProbe { DWORD pid = 0; int hits = 0, tries = 0; };
static GameProbe g_probe;
static std::set<DWORD> g_notGame;  // процессы, которые на весь экран, но не игры (фото, видео)
struct GameSession { bool active = false; DWORD pid = 0; std::wstring title; double start = 0, last = 0; double fpsSum = 0; int fpsN = 0; float gpuMax = 0; };
static GameSession g_sess;
static DWORD g_fsPid = 0;
static void GameOff();
static void GameProbeTick();
static std::wstring g_fsExe;      // последняя программа на весь экран (для подсказки в настройках)
static bool g_fsVideo = false;
static bool IsVideoOrBrowserExe(const std::wstring& exe) {
    // точные имена, а не кусок строки: раньше «opera» или «code» ловили и обычные игры
    static const wchar_t* v[] = {L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe", L"opera_gx.exe", L"brave.exe", L"vivaldi.exe",
                                 L"browser.exe", L"yandex.exe", L"arc.exe", L"thorium.exe", L"librewolf.exe", L"waterfox.exe", L"zen.exe",
                                 L"floorp.exe", L"chromium.exe", L"vlc.exe", L"mpc-hc.exe", L"mpc-hc64.exe", L"mpc-be.exe", L"mpc-be64.exe",
                                 L"potplayermini.exe", L"potplayermini64.exe", L"mpv.exe", L"wmplayer.exe", L"video.ui.exe", L"powerpnt.exe",
                                 L"kmplayer.exe", L"kmplayer64x.exe", L"aimp.exe", L"telegram.exe", L"discord.exe", L"explorer.exe",
                                 L"mstsc.exe", L"vmware.exe", L"virtualboxvm.exe", L"code.exe", L"devenv.exe", L"obs64.exe",
                                 L"microsoft.media.player.exe", L"photos.exe", L"acrord32.exe", L"acrobat.exe", L"sumatrapdf.exe"};
    for (auto b : v) if (exe == b) return true;
    return false;
}
static bool g_notifReply = false, g_replySending = false;
static HWND g_hostReply = nullptr, g_reReply = nullptr;
static D2D1_RECT_F g_rcReply = {}, g_nrReply = {}, g_nrRead = {}, g_nrSend = {}, g_nrCancel = {};
static int g_notifHot = 0;
static std::wstring g_replyErr;
static double g_replyErrUntil = 0;
static void DrawDevCard(float ox, float oy, float tw, float th);
static void RollText(int id, IDWriteTextFormat* f, const std::wstring& s, D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT al, D2D1_COLOR_F c);
static float CoverBreath();
static void DrawCritter(int kind, float cx, float cy, float s);
static void IconShield(float cx, float cy, D2D1_COLOR_F c, bool on);
static void CritterBlinkTick();
static bool g_critterShown = false;
static double g_blinkUntil = 0;
static int g_mood = 0;            // настроение персонажа: 0 обычное, 1 влюблён, 2 обиделся, 3 зевает
static double g_moodUntil = 0;
static double g_actUntil = 0;
static unsigned g_frames = 0;  // сколько кадров нарисовали (для замера простоя)     // питомец сейчас что-то делает (прыжок, оглядывается…)
static void CoverGlow(D2D1_RECT_F r, float rad);
static bool NyCountdown(SYSTEMTIME& st);
static int DaysToNy(const SYSTEMTIME& st);
static void DrawNotifActions(float ox, float oy, float tw, float th);
static void DrawStatsRow(D2D1_RECT_F c);
static void EnsureStats();
struct ToolResult;
static bool ToastDragDown(int x, int y);
static bool ToastDragMove(int x, int y);
static bool NotifClick(int x, int y);
static void NotifHover(int x, int y);
static bool LooksLikePods(const std::wstring& name);
static void PodsLookup(const std::wstring& name, bool manual);
static bool OpenMarkupFor(ToolResult* r);
static void SyncEditors();
static void IconBell(float cx, float cy, D2D1_COLOR_F c);
static void OnPanelOpened();
static int CmdHit(int x, int y);
static void RunCmd(int idx, bool ctrl);
static D2D1_COLOR_F AccentFor(D2D1_COLOR_F cover);
static bool Quiet();
static float EarR();                 // «ушки» чёлки у края экрана (размер зависит от стиля)
#define kEar EarR()
static double g_auroraT = 0;         // сдвиг «авроры»/«неона» между сменами режима
static Spring sTab;                  // сдвиг содержимого при смене вкладки
static ID2D1Bitmap* g_glassBmp = nullptr;
static Mode g_mode = M_IDLE;
static double g_modeT = 0;
static float g_content = 0;
static bool g_hover, g_tracking;
static int g_hoverBtn = -1, g_pressBtn = -1;
static bool g_hoverProg, g_dragSeek;
static float g_dragFrac = 0;
static INT64 g_seekPos = 0, g_seekStamp = 0;
static double g_seekHoldUntil = 0;
static Spring g_btnScale[3], g_btnHov[3], g_playMorph, g_progHov;
static bool g_dirty = true, g_visible = true, g_audioTimer = false, g_marqueeActive = false;
static double g_lastFrame = 0, g_phase = 0;
static float g_level = 0, g_peak = 0;
static double g_volShowUntil = 0;
static float g_vol = 0;
static bool g_muted = false;
static HANDLE g_frameTimer;

static Layout g_L;
static MediaWatcher g_mw;
static MediaSnap g_media;
static Pic g_art, g_artPrev, g_appIcon;
static UINT g_artVer = ~0u, g_appVer = ~0u;
static double g_artFadeT = -10;
static D2D1_COLOR_F g_accent = DefaultAccent(), g_accentFrom = DefaultAccent(), g_accentTo = DefaultAccent();
static std::wstring g_lastKey;
static bool g_firstMedia = true;
static Audio g_audio;
static Marquee g_mqTitle, g_mqArtist, g_mqPeekTitle, g_mqPeekArtist, g_mqSource;

// тексты песен
struct LineLayout {
    int idx = -100;
    std::wstring text;
    IDWriteTextFormat* fmt = nullptr;
    IDWriteTextLayout* lay = nullptr;
    float w = 0;
    void Release() { SafeRelease(lay); idx = -100; text.clear(); fmt = nullptr; w = 0; }
};
static LyricsService g_lyrSvc;
static Lyrics g_lyr;
static int g_lyrState = LYR_NONE;
static UINT g_lyrReq = 0;
static int g_lyrReqDur = -1;
static std::wstring g_lyrReqKey;
static int g_lyrLine = -100, g_lyrPrevLine = -100;
static double g_lyrLineT = -10;
static LineLayout g_llCur, g_llPrev, g_llXCur, g_llXPrev;
static Spring g_lyrScroll, g_lyrScrollX;

// системные события
enum { TK_VOLUME, TK_BRIGHT, TK_BATTERY, TK_AUDIO, TK_MIC, TK_CAM, TK_CAPS, TK_LAYOUT, TK_SHOT, TK_TEXT, TK_QR, TK_COLOR,
       TK_REC, TK_DOWNLOAD, TK_SHELF, TK_ERROR, TK_REMIND, TK_VPN, TK_RAIN };
enum { TA_NONE, TA_OPEN, TA_SELECT, TA_URL, TA_UPDATE };
struct Toast {
    int kind = TK_VOLUME;
    std::wstring text, right, badge;
    float value = -1;   // 0..1 — показать полоску (громкость/яркость)
    bool flag = false;  // выкл звук / зарядка / Caps включён / Bluetooth
    D2D1_COLOR_F color = kWhite;
    float textW = 0, rightW = 0;
    int action = TA_NONE;   // клик по тосту: открыть файл / показать в папке / ссылку
    std::wstring arg;
};
static Toast g_toast;
static AudioWatcher g_audioWatch;
static PrivacyWatcher g_privWatch;
static PrivacyInfo g_priv;
static bool g_privInit = false;
static HWND g_layoutWnd;
static HKL g_layoutHkl;
static int g_pwrSeen = 0;  // первые уведомления питания приходят сразу при подписке — их пропускаем
static int g_batLowLevel = 100;
static HPOWERNOTIFY g_pwrN[3];

// буфер обмена
static ClipStore g_clip;
static bool g_clipStoreOk = false;
static std::vector<ClipItem> g_clipItems;
static std::vector<int> g_clipView;
static std::wstring g_clipQuery;
static int g_clipSel = 0, g_clipHover = -1, g_clipHoverBtn = -1;
static Spring g_clipScroll;
static bool g_clipOpen = false, g_caretOn = true, g_cmdOpen = false;
static HWND g_prevFg;
static DWORD g_ownClipSeq = 0, g_lastClipSeq = 0;
static int g_pasteTries = 0;
static D2D1_RECT_F g_clipList = {};
static std::map<long long, Pic> g_clipThumbs;

// инструменты экрана
static std::wstring g_baseDir;
static int g_toolPending = 0, g_hoverTool = -1;
static bool g_toolHidden = false, g_recBlink = true;
static Recorder g_rec;
// полка
struct ShelfItem { std::wstring path, name; Pic icon; std::vector<BYTE> px; int iw = 0, ih = 0; };
static std::vector<ShelfItem> g_shelf;
static int g_hoverShelf = -1, g_shelfPress = -1;
static bool g_hoverShelfX = false, g_hoverShelfClear = false;
static POINT g_shelfPressPt;
static bool g_dropActive = false;
static Mode g_preDropMode = M_IDLE;
static int g_dropCount = 0;
// загрузки
static DownloadWatcher g_dlw;
static DlInfo g_dl;
// виджеты
static WeatherService g_wxs;
static WeatherData g_wx;
static CalendarService g_cals;
static CalData g_cal;
static std::vector<std::wstring> g_calNotified;
struct TimerState {
    bool on = false, paused = false;
    double endAt = 0, left = 0, total = 0;
    int kind = 0;  // 0 таймер, 1 помодоро-фокус, 2 перерыв
    int focusDone = 0;
};
static TimerState g_tm;
static int g_hoverChip = -1;
// уведомления и звонки
static NotificationWatcher g_nw;
static std::vector<NotifData*> g_notifQ;
static NotifData* g_notif = nullptr;
static Pic g_notifPic;
static bool g_inCall = false, g_micMuted = false, g_hoverMic = false;
static std::wstring g_callApp;
static double g_callStart = 0;

// визуализатор
static Visualizer g_viz;
static Spectrum g_spec;
static bool g_vizOk = false;
static double g_vizLastActive = 0;

// ----------------------------------------------------------------------------
// Ресурсы рисования
// ----------------------------------------------------------------------------
static std::wstring g_fontName = L"Segoe UI";
static IDWriteTextFormat* MakeFormat(float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextFormat* f = nullptr;
    g_dwf->CreateTextFormat(g_fontName.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                            L"ru-ru", &f);
    if (!f) return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* sign = nullptr;
    g_dwf->CreateEllipsisTrimmingSign(f, &sign);
    f->SetTrimming(&tr, sign);
    SafeRelease(sign);
    return f;
}
static void CreateFormats() {
    g_fTime = MakeFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fTitle = MakeFormat(15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fArtist = MakeFormat(13.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSmall = MakeFormat(11.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fBig = MakeFormat(46.f, DWRITE_FONT_WEIGHT_LIGHT);
    g_fDate = MakeFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fPeekTitle = MakeFormat(14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fPeekArtist = MakeFormat(12.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSource = MakeFormat(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fLyric = MakeFormat(14.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fLyricBig = MakeFormat(16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fLyricSmall = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fToast = MakeFormat(13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fClip = MakeFormat(13.5f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fSearch = MakeFormat(14.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fWBig = MakeFormat(20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fPlayerTitle = MakeFormat(19.f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fCmd = MakeFormat(18.f, DWRITE_FONT_WEIGHT_NORMAL);
    g_fQa = MakeFormat(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

static void MakePic(Pic& p, const std::vector<BYTE>& pxIn, int w, int h) {
    p.Release();
    if (!g_rt || pxIn.empty() || w <= 0 || h <= 0) return;
    // картинку не квадратную дополняем прозрачными полями — иначе она вылезает из своей рамки
    std::vector<BYTE> sq;
    const std::vector<BYTE>* src = &pxIn;
    if (w != h && (size_t)w * h * 4 <= pxIn.size()) {
        int n = std::max(w, h), ox = (n - w) / 2, oy = (n - h) / 2;
        sq.assign((size_t)n * n * 4, 0);
        for (int y = 0; y < h; y++) memcpy(&sq[((size_t)(y + oy) * n + ox) * 4], &pxIn[(size_t)y * w * 4], (size_t)w * 4);
        src = &sq;
        w = h = n;
    }
    const std::vector<BYTE>& px = *src;
    D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.f, 96.f};
    if (FAILED(g_rt->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, &p.bmp))) return;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = {D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR};
    g_rt->CreateBitmapBrush(p.bmp, &bbp, nullptr, &p.brush);
    p.size = (float)w;
}

static void DiscardTarget() {
    SafeRelease(g_glassBmp);
    g_notifPic.Release();
    for (auto& it : g_shelf) it.icon.Release();
    for (auto& kv : g_clipThumbs) kv.second.Release();
    g_clipThumbs.clear();
    g_art.Release();
    g_artPrev.Release();
    g_appIcon.Release();
    SafeRelease(g_gsRight);
    SafeRelease(g_gsBoth);
    SafeRelease(g_layer2);
    SafeRelease(g_layer3);
    SafeRelease(g_layer);
    SafeRelease(g_brush);
    SafeRelease(g_rt);
}
static void CreateTarget() {
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0,
        D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(g_d2f->CreateDCRenderTarget(&p, &g_rt))) return;
    g_rt->SetDpi(96.f * g_scale, 96.f * g_scale);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_COLOR_F c = Col(1, 1, 1);
    g_rt->CreateSolidColorBrush(&c, nullptr, &g_brush);
    g_rt->CreateLayer(nullptr, &g_layer);
    g_rt->CreateLayer(nullptr, &g_layer2);
    g_rt->CreateLayer(nullptr, &g_layer3);
    D2D1_GRADIENT_STOP right[3] = {{0.f, Col(0, 0, 0, 1)}, {0.86f, Col(0, 0, 0, 1)}, {1.f, Col(0, 0, 0, 0)}};
    D2D1_GRADIENT_STOP both[4] = {{0.f, Col(0, 0, 0, 0)}, {0.07f, Col(0, 0, 0, 1)}, {0.86f, Col(0, 0, 0, 1)},
                                  {1.f, Col(0, 0, 0, 0)}};
    g_rt->CreateGradientStopCollection(right, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &g_gsRight);
    g_rt->CreateGradientStopCollection(both, 4, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &g_gsBoth);
    MakePic(g_art, g_media.art, g_media.artW, g_media.artH);
    MakePic(g_appIcon, g_media.appIcon, g_media.appIconW, g_media.appIconH);
}

static void CreateDib() {
    if (g_memDC) {
        SelectObject(g_memDC, g_oldBmp);
        DeleteObject(g_dib);
        DeleteDC(g_memDC);
    }
    g_dibW = (int)std::ceil(880 * g_scale) + 8;
    g_dibH = (int)std::ceil(530 * g_scale) + 8;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_dibW;
    bi.bmiHeader.biHeight = -g_dibH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_dib);
}

// ---- Мониторы ----
struct MonInfo { std::wstring device; RECT rc; bool primary; };
static BOOL CALLBACK EnumMonProc(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(m, &mi))
        ((std::vector<MonInfo>*)lp)->push_back({mi.szDevice, mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}
static std::vector<MonInfo> ListMonitors() {
    std::vector<MonInfo> v;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)&v);
    return v;
}
static std::vector<MonInfo> g_monList;
static HMONITOR g_curMon;
static HMONITOR PickMonitor() {
    if (g_set.followMouse) {
        POINT pt;
        GetCursorPos(&pt);
        return MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    if (!g_set.monitor.empty()) {
        for (auto& m : ListMonitors())
            if (m.device == g_set.monitor) {
                POINT c = {(m.rc.left + m.rc.right) / 2, (m.rc.top + m.rc.bottom) / 2};
                return MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
            }
    }
    POINT o = {0, 0};
    return MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY);
}

static void UpdateMetrics() {
    HMONITOR mon = PickMonitor();
    g_curMon = mon;
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    g_mon = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    float s = dx / 96.f * (g_set.sizePct / 100.f);
    s = std::min(s, (mi.rcMonitor.right - mi.rcMonitor.left) * 0.95f / 840.f);  // раскрытая панель должна влезать в экран
    if (std::fabs(s - g_scale) > 0.001f || !g_memDC) {
        g_scale = s;
        CreateDib();
        if (g_rt) g_rt->SetDpi(96.f * s, 96.f * s);
    }
    g_dirty = true;
}

// ----------------------------------------------------------------------------
// Режимы и размеры
// ----------------------------------------------------------------------------
static bool LyricsReady() { return g_set.lyrics && g_lyrState == LYR_FOUND && !g_lyr.lines.empty() && g_media.has; }
static bool CallActive() { return g_set.calls && g_inCall; }
static Mode Collapsed() {
    if (g_rec.Active()) return M_REC;
    if (CallActive()) return M_CALL;
    if (g_game.on) return M_GAME;
    if (g_tm.on) return M_TIMER;
    if (g_set.downloads && g_dl.active > 0) return M_DL;
    return g_media.has ? (LyricsReady() ? M_LYRICS : M_COMPACT) : M_IDLE;
}
static bool ShelfShown() { return g_set.shelf && !g_shelf.empty(); }

static int CmdRowCount();
static void DrawCmd(float ox, float oy, float tw, float th);
static void StyleShape(Mode m, float& w, float& h, float& r);
static float GameHudWidth();
static void TargetSize(Mode m, float& w, float& h, float& r) {
    switch (m) {
        case M_IDLE: {
            w = g_set.idleWidget > 0 ? 270.f : (g_set.widgets && g_wx.ok) ? 214.f : 128.f;
            h = 34;
            SYSTEMTIME ny;
            if (NyCountdown(ny)) w = 250;
            break;
        }
        case M_TIMER: w = 210; h = 36; break;
        case M_CALL: w = 260; h = 38; break;
        case M_NOTIF: w = 440; h = g_notifReply ? 124.f : 66.f; break;
        case M_DEV: w = g_dev.single ? 300.f : 420.f; h = 84; break;
        case M_COMPACT: w = 250; h = 36; break;
        case M_PEEK: w = 380; h = 58; break;
        case M_TOAST:
            h = 40;
            if (g_toast.value >= 0 && (g_toast.kind == TK_VOLUME || g_toast.kind == TK_BRIGHT)) w = 290;
            else w = Clamp(50 + g_toast.textW + (g_toast.right.empty() ? 0.f : g_toast.rightW + 18) + 20, 190.f, 440.f);
            break;
        case M_CLIP: w = 780; h = 452; break;
        case M_REC: w = 170; h = 34; break;
        case M_DL: w = 320; h = 36; break;
        case M_GAME: w = GameHudWidth(); h = 30; break;
        case M_DROP: w = 320; h = 64; break;
        case M_CMD: { int n = CmdRowCount(); w = 640; h = 64 + (n ? 10.f + n * 48.f : 0.f); break; }
        case M_LYRICS: w = g_llCur.lay ? Clamp(g_llCur.w + 42 + 48, 250.f, 540.f) : 260.f; h = 38; break;
        case M_EXPANDED: w = 780; h = 452; break;
    }
    r = m == M_CMD ? 30.f : (m == M_EXPANDED || m == M_CLIP) ? 34.f : (m == M_PEEK ? 29.f : m == M_DROP ? 32.f : h / 2);
    if (g_hover && (m == M_IDLE || m == M_COMPACT || m == M_LYRICS || m == M_REC || m == M_DL || m == M_TIMER || m == M_CALL)) {
        w += 14; h += 2; r = h / 2;
    }
    StyleShape(m, w, h, r);
}
static void Retarget() {
    float w, h, r;
    TargetSize(g_mode, w, h, r);
    sW.target = w; sH.target = h; sR.target = r;
    g_dirty = true;
}
static void PlayUi(int s);
static void UpdatePassThrough();
static void SetMode(Mode m) {
    if (m != g_mode) {
        if (m == M_EXPANDED) PlayUi(0 /*открытие*/);
        else if (g_mode == M_EXPANDED && (m == M_IDLE || m == M_COMPACT || m == M_LYRICS || m == M_TIMER || m == M_DL)) PlayUi(1 /*закрытие*/);
        if (g_ieFocus >= 0 && !((m == M_EXPANDED || m == M_CLIP) && g_ieFocus != 3) && !(m == M_NOTIF && g_ieFocus == 3)) IEBlur(true);
        if (m != M_EXPANDED) g_appPick = g_recPick = false;
        g_mode = m;
        g_modeT = NowSec();
        g_content = 0;
        g_auroraT += 0.8;
        if (g_set.passThrough) UpdatePassThrough();
        KillTimer(g_hwnd, T_TICK);
        if (m == M_EXPANDED) SetTimer(g_hwnd, T_TICK, 1000, nullptr);
        g_hotId = 0; g_hotArg = -1;
        if (m == M_EXPANDED) g_panelOpenedPending = true;
        if (m != M_PEEK) KillTimer(g_hwnd, T_PEEK);
        g_hoverBtn = g_pressBtn = -1;
        g_hoverProg = false;
        // бегущие строки начинают с начала
        g_mqTitle.t0 = g_mqArtist.t0 = g_mqPeekTitle.t0 = g_mqPeekArtist.t0 = g_mqSource.t0 = g_modeT;
    }
    Retarget();
}

static bool AudioActive() {
    return g_set.audioAnim && g_visible && g_media.has && g_media.status == 4 && (g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_PEEK || g_mode == M_EXPANDED);
}
static Spring sTop, sEar, sShake, sBub;
static float g_setScroll = 0, g_setScrollT = 0;
static double g_rollUntil = 0;
static bool SpringsMoving() {
    return !SettledPx(sW) || !SettledPx(sH) || !SettledPx(sR) || !sTab.Settled() || !SettledPx(sTop) || !SettledPx(sEar) ||
           !SettledPx(sShake) || !sBub.Settled() || NowSec() < g_rollUntil || NowSec() < g_blinkUntil || NowSec() < g_moodUntil || NowSec() < g_actUntil || std::fabs(g_setScrollT - g_setScroll) > 0.3f;
}
static float ArtFade() { return Smooth((NowSec() - g_artFadeT) / 0.45); }
static bool UiAnimating() {
    for (int i = 0; i < 3; i++)
        if (!g_btnScale[i].Settled() || !g_btnHov[i].Settled()) return true;
    if (LyricsReady() && (NowSec() - g_lyrLineT < 0.34 || !SettledPx(g_lyrScroll) || !SettledPx(g_lyrScrollX)))
        return true;
    if (g_mode == M_CLIP && !SettledPx(g_clipScroll)) return true;
    return !g_playMorph.Settled() || !g_progHov.Settled() || ArtFade() < 1.f;
}
static bool LyricsRunning() {
    return g_visible && LyricsReady() && g_media.status == 4 && (g_mode == M_LYRICS || g_mode == M_EXPANDED);
}
static bool MarqueeRunning() {
    return g_marqueeActive && (g_mode == M_EXPANDED || g_mode == M_PEEK) && g_content > 0.01f;
}
static bool NeedAnim() {
    if (!g_visible) return false;
    if (SpringsMoving() || !SettledPx(sWave) || !SettledPx(sPull) || !sHide.Settled() || g_content < 1.f || UiAnimating()) return true;
    if (MarqueeRunning() || LyricsRunning()) return true;
    if (g_mode == M_DL || g_mode == M_DROP) return true;  // крутилка загрузки / пульсация полки
    if (AudioActive() && (g_level > 0.01f || g_peak > 0.01f)) return true;
    return false;
}

static INT64 FileTimeNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((INT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static INT64 Duration() { return g_media.end - g_media.start; }
static INT64 CurElapsed() {
    INT64 pos = g_media.pos - g_media.start;
    if (g_media.status == 4 && g_media.lastUpd > 0) {
        INT64 d = FileTimeNow() - g_media.lastUpd;
        if (d > 0 && d < 3600LL * 10000000LL) pos += d;
    }
    return Clamp<INT64>(pos, 0, std::max<INT64>(0, Duration()));
}
static std::wstring FmtDur(INT64 t100ns) {
    int s = (int)(t100ns / 10000000LL);
    wchar_t b[24];
    if (s >= 3600) swprintf(b, 24, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 24, L"%d:%02d", s / 60, s % 60);
    return b;
}

// ----------------------------------------------------------------------------
// Геометрия
// ----------------------------------------------------------------------------
// Спектр вдоль нижнего края: бас в центре, верха к краям
static float SpecAt(float t) {
    float b = std::fabs(t - 0.5f) * 2.f;
    float fi = b * (Spectrum::kBands - 1);
    int i = std::min((int)fi, Spectrum::kBands - 1), j = std::min(i + 1, Spectrum::kBands - 1);
    float fr = fi - i;
    return g_spec.band[i] + (g_spec.band[j] - g_spec.band[i]) * fr;
}
static float WaveY(float xx, float t, float amp, float ph) {
    float env = std::pow(std::sin(3.14159265f * t), 0.6f);
    float y = 0;
    if (amp > 0.01f) {
        float shape = g_vizOk ? (0.2f + 1.15f * SpecAt(t)) : 1.f;
        y = amp * env * shape * (0.62f * std::sin(xx * 0.085f + ph) + 0.38f * std::sin(xx * 0.19f - ph * 1.37f));
    }
    // «жидкий» край тянется к курсору
    float pull = (float)sPull.x;
    if (pull > 0.02f) {
        float d = (xx - g_pullX) / 26.f;
        y += pull * env * std::exp(-d * d);
    }
    return y;
}
static void Arc(ID2D1GeometrySink* s, D2D1_POINT_2F p, float r) {
    D2D1_ARC_SEGMENT a = {p, {r, r}, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
    s->AddArc(&a);
}
static void ArcCCW(ID2D1GeometrySink* s, D2D1_POINT_2F p, float r) {
    D2D1_ARC_SEGMENT a = {p, {r, r}, 0.f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
    s->AddArc(&a);
}
// Пилюля со скруглениями и «живым» волнистым низом.
// notch: верх прямой и прирос к краю экрана, по бокам вогнутые «ушки» (как у чёлки MacBook).
static ID2D1PathGeometry* BuildPill(float x, float y, float w, float h, float r, float amp, float ph, bool notch = false) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return nullptr; }
    r = std::min(r, std::min(w, h) / 2);
    bool arcs = r > 0.1f;
    const float E = kEar;
    if (notch) {
        r = std::min(r, h - E);
        arcs = r > 0.1f;
        s->BeginFigure(Pt(x - E, y), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(Pt(x + w + E, y));
        ArcCCW(s, Pt(x + w, y + E), E);
    } else {
        s->BeginFigure(Pt(x + r, y), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(Pt(x + w - r, y));
        if (arcs) Arc(s, Pt(x + w, y + r), r);
    }
    s->AddLine(Pt(x + w, y + h - r));
    if (arcs) Arc(s, Pt(x + w - r, y + h), r);
    float x0 = x + w - r, x1 = x + r;
    if ((amp > 0.05f || sPull.x > 0.05) && x0 - x1 > 8) {
        int n = (int)((x0 - x1) / 3);
        for (int i = 1; i < n; i++) {
            float t = (float)i / n;
            float xx = x0 - (x0 - x1) * t;
            s->AddLine(Pt(xx, y + h + WaveY(xx - x, t, amp, ph)));
        }
    }
    s->AddLine(Pt(x1, y + h));
    if (arcs) Arc(s, Pt(x, y + h - r), r);
    if (notch) {
        s->AddLine(Pt(x, y + E));
        ArcCCW(s, Pt(x - E, y), E);
    } else {
        s->AddLine(Pt(x, y + r));
        if (arcs) Arc(s, Pt(x + r, y), r);
    }
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    return g;
}
static ID2D1PathGeometry* BuildWaveLine(float x, float y, float w, float h, float r, float amp, float ph) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return nullptr; }
    r = std::min(r, std::min(w, h) / 2);
    float x0 = x + w - r, x1 = x + r;
    int n = std::max(2, (int)((x0 - x1) / 3));
    s->BeginFigure(Pt(x0, y + h), D2D1_FIGURE_BEGIN_HOLLOW);
    for (int i = 1; i < n; i++) {
        float t = (float)i / n;
        float xx = x0 - (x0 - x1) * t;
        s->AddLine(Pt(xx, y + h + WaveY(xx - x, t, amp, ph)));
    }
    s->AddLine(Pt(x1, y + h));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    s->Release();
    return g;
}
static void FillPoly(const D2D1_POINT_2F* p, int n) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return; }
    s->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i < n; i++) s->AddLine(p[i]);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    g_rt->FillGeometry(g, g_brush, nullptr);
    g->Release();
}

// ----------------------------------------------------------------------------
// Рисование содержимого
// ----------------------------------------------------------------------------

static void SetColor(D2D1_COLOR_F c) { g_brush->SetColor(&c); }
static void Text(IDWriteTextFormat* f, const std::wstring& s, D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT a,
                 D2D1_COLOR_F c) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(a);
    SetColor(c);
    g_rt->DrawText(s.c_str(), (UINT32)s.size(), f, &r, g_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                   DWRITE_MEASURING_MODE_NATURAL);
}
static void RRect(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    SetColor(c);
    g_rt->FillRoundedRectangle(&rr, g_brush);
}
static void Circle(float cx, float cy, float r, D2D1_COLOR_F c) {
    D2D1_ELLIPSE e = {Pt(cx, cy), r, r};
    SetColor(c);
    g_rt->FillEllipse(&e, g_brush);
}

// Текст, который прокручивается, если не влезает. Края мягко растворяются.
static void MarqueeText(Marquee& m, IDWriteTextFormat* fmt, const std::wstring& s, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (!fmt || s.empty()) return;
    float boxW = r.right - r.left, boxH = r.bottom - r.top;
    if (m.text != s || m.fmt != fmt || !m.layout) {
        SafeRelease(m.layout);
        m.text = s;
        m.fmt = fmt;
        m.t0 = NowSec();
        m.width = 0;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (SUCCEEDED(g_dwf->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, 100000.f, boxH, &m.layout))) {
            DWRITE_TRIMMING none = {DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
            m.layout->SetTrimming(&none, nullptr);
            DWRITE_TEXT_METRICS tm = {};
            if (SUCCEEDED(m.layout->GetMetrics(&tm))) m.width = tm.widthIncludingTrailingWhitespace;
        }
    }
    if (!m.layout || m.width <= boxW + 0.5f) {
        Text(fmt, s, r, DWRITE_TEXT_ALIGNMENT_LEADING, c);
        return;
    }
    g_marqueeActive = true;
    const float gap = 40.f, speed = 30.f;
    const double pause = 1.8;
    double cycle = (m.width + gap) / speed;
    double t = std::fmod(std::max(0.0, NowSec() - m.t0), pause + cycle);
    float off = t < pause ? 0.f : (float)((t - pause) * speed);

    ID2D1LinearGradientBrush* mask = nullptr;
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {Pt(r.left, 0), Pt(r.right, 0)};
    g_rt->CreateLinearGradientBrush(&lg, nullptr, off > 0.5f ? g_gsBoth : g_gsRight, &mask);
    D2D1_LAYER_PARAMETERS lp = {};
    lp.contentBounds = r;
    lp.geometricMask = nullptr;
    lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lp.maskTransform = D2D1::Matrix3x2F::Identity();
    lp.opacity = 1.f;
    lp.opacityBrush = mask;
    lp.layerOptions = D2D1_LAYER_OPTIONS_NONE;
    g_rt->PushLayer(&lp, g_layer2);
    SetColor(c);
    g_rt->DrawTextLayout(Pt(r.left - off, r.top), m.layout, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    g_rt->DrawTextLayout(Pt(r.left - off + m.width + gap, r.top), m.layout, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    g_rt->PopLayer();
    SafeRelease(mask);
}

static void DrawPicRounded(const Pic& p, D2D1_RECT_F r, float rad, float alpha) {
    if (!p.brush || alpha <= 0.003f) return;
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    float s = (r.right - r.left) / p.size;
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(r.left, r.top);
    p.brush->SetTransform(&m);
    p.brush->SetOpacity(alpha);
    g_rt->FillRoundedRectangle(&rr, p.brush);
    p.brush->SetOpacity(1.f);
}
static void DrawPlaceholder(D2D1_RECT_F r, float rad, float alpha) {
    if (alpha <= 0.003f) return;
    RRect(r, rad, Col(0.16f, 0.16f, 0.19f, alpha));
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, sz = (r.right - r.left) * 0.16f;
    D2D1_ELLIPSE e = {Pt(cx - sz * 0.4f, cy + sz * 0.9f), sz, sz * 0.8f};
    SetColor(WithA(g_accent, 0.9f * alpha));
    g_rt->FillEllipse(&e, g_brush);
    RRect(RF(cx + sz * 0.45f, cy - sz * 2.2f, sz * 0.4f, sz * 3.1f), sz * 0.2f, WithA(g_accent, 0.9f * alpha));
}
// Обложка с плавным перетеканием старой в новую
static void DrawCover(D2D1_RECT_F r, float rad) {
    float f = ArtFade();
    if (f < 1.f) {
        if (g_artPrev.brush) DrawPicRounded(g_artPrev, r, rad, 1.f);
        else DrawPlaceholder(r, rad, 1.f);
        // новая слегка «вырастает» при появлении
        float k = 0.94f + 0.06f * f;
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        float hw = (r.right - r.left) / 2 * k, hh = (r.bottom - r.top) / 2 * k;
        D2D1_RECT_F rs = {cx - hw, cy - hh, cx + hw, cy + hh};
        if (g_art.brush) DrawPicRounded(g_art, rs, rad * k, f);
        else DrawPlaceholder(rs, rad * k, f);
    } else {
        if (g_art.brush) DrawPicRounded(g_art, r, rad, 1.f);
        else DrawPlaceholder(r, rad, 1.f);
    }
}
static void DrawBars(float x, float cy, float h) {
    bool play = g_media.status == 4;
    double t = NowSec();
    for (int i = 0; i < 4; i++) {
        float amp;
        if (play && g_set.audioAnim && g_vizOk) {
            static const int grp[5] = {0, 3, 7, 11, 16};
            float lv = 0;
            for (int b = grp[i]; b < grp[i + 1]; b++) lv += g_spec.band[b];
            lv /= (grp[i + 1] - grp[i]);
            amp = 0.15f + 0.85f * std::min(1.f, lv * (1.f + 0.35f * g_spec.beat));
        } else if (play && g_set.audioAnim) {
            float osc = 0.5f + 0.5f * std::sin((float)(t * (5.3 + i * 1.7)) + i * 1.9f);
            float lv = std::min(1.f, g_level * 1.35f * (0.45f + 0.55f * osc) * (i % 2 ? 1.f : 0.82f));
            amp = 0.2f + 0.8f * lv;
        } else if (play) {
            static const float st[4] = {0.55f, 0.85f, 0.45f, 0.7f};
            amp = st[i];
        } else {
            amp = 0.18f;
        }
        float bh = std::max(3.f, h * amp);
        RRect(RF(x + i * 5.5f, cy - bh / 2, 3.f, bh), 1.5f, play ? g_accent : WithA(kGray, 0.8f));
    }
}
// Плей/пауза с морфингом: m=0 — треугольник, m=1 — две палки
static void IconPlayPause(float cx, float cy, float s, float m) {
    float a = 1.f - m;
    if (a > 0.01f) {
        float k = s * (0.6f + 0.4f * a);
        SetColor(WithA(kWhite, a));
        D2D1_POINT_2F p[3] = {Pt(cx - k * 0.5f, cy - k * 0.85f), Pt(cx + k * 0.85f, cy), Pt(cx - k * 0.5f, cy + k * 0.85f)};
        FillPoly(p, 3);
    }
    if (m > 0.01f) {
        float k = s * (0.6f + 0.4f * m);
        RRect(RF(cx - k * 0.55f, cy - k * 0.8f, k * 0.4f, k * 1.6f), k * 0.12f, WithA(kWhite, m));
        RRect(RF(cx + k * 0.15f, cy - k * 0.8f, k * 0.4f, k * 1.6f), k * 0.12f, WithA(kWhite, m));
    }
}
static void IconSkip(float cx, float cy, float s, bool next, bool enabled) {
    D2D1_COLOR_F c = enabled ? kWhite : WithA(kWhite, 0.3f);
    SetColor(c);
    float d = next ? 1.f : -1.f;
    D2D1_POINT_2F p[3] = {Pt(cx - d * s * 0.65f, cy - s * 0.7f), Pt(cx + d * s * 0.45f, cy),
                          Pt(cx - d * s * 0.65f, cy + s * 0.7f)};
    FillPoly(p, 3);
    float bx = next ? cx + s * 0.45f : cx - s * 0.45f - s * 0.22f;
    RRect(RF(bx, cy - s * 0.7f, s * 0.22f, s * 1.4f), s * 0.08f, c);
}
static void IconSpeaker(float x, float cy, float s, bool muted) {
    D2D1_COLOR_F c = muted ? kGray : kWhite;
    RRect(RF(x, cy - s * 0.3f, s * 0.35f, s * 0.6f), s * 0.06f, c);
    SetColor(c);
    D2D1_POINT_2F p[4] = {Pt(x + s * 0.3f, cy - s * 0.3f), Pt(x + s * 0.75f, cy - s * 0.72f),
                          Pt(x + s * 0.75f, cy + s * 0.72f), Pt(x + s * 0.3f, cy + s * 0.3f)};
    FillPoly(p, 4);
}

static void AddBtn(Layout& L, float cx, float cy, float r) {
    if (L.btnCount < 3) L.btn[L.btnCount++] = D2D1_RECT_F{cx - r, cy - r, cx + r, cy + r};
}

static void DrawExpandedMedia(Layout& L, float ox, float oy, float tw, const std::wstring& timeStr) {
    DrawCover(RF(ox + 18, oy + 18, 68, 68), 14);

    float tx = ox + 100;
    MarqueeText(g_mqTitle, g_fTitle, g_media.title, RF(tx, oy + 17, tw - 100 - 84, 24), kWhite);
    MarqueeText(g_mqArtist, g_fArtist, g_media.artist, RF(tx, oy + 41, tw - 100 - 60, 20), kGray);

    // источник: иконка + название приложения
    if (!g_media.appName.empty()) {
        float sx = tx;
        if (g_appIcon.brush) {
            DrawPicRounded(g_appIcon, RF(sx, oy + 66, 14, 14), 3.5f, 1.f);
            sx += 19;
        }
        MarqueeText(g_mqSource, g_fSource, g_media.appName, RF(sx, oy + 64, ox + tw - 60 - sx, 18),
                    WithA(kGray, 0.85f));
    }

    std::wstring corner = timeStr;
    if (NowSec() < g_volShowUntil) {
        wchar_t b[16];
        swprintf(b, 16, L"\u266A %d%%", (int)std::lround(g_vol * 100));
        corner = b;
    }
    Text(g_fTime, corner, RF(ox + tw - 80, oy + 17, 62, 24), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
    if (g_media.status == 4) DrawBars(ox + tw - 40, oy + 58, 14);

    // прогресс + перемотка
    INT64 dur = Duration();
    float cyCtl = oy + 140;
    L.hasProg = false;
    if (dur > 10000000LL) {
        INT64 el = g_dragSeek ? (INT64)(g_dragFrac * (double)dur) : CurElapsed();
        float y = oy + 108, x0 = ox + 64, x1 = ox + tw - 64;
        float frac = g_dragSeek ? g_dragFrac : Clamp((float)((double)el / (double)dur), 0.f, 1.f);
        float hv = (float)Clamp(g_progHov.x, 0.0, 1.2);
        float th = 4.f + 3.f * hv;
        RRect(RF(x0, y - th / 2, x1 - x0, th), th / 2, Ov(0.16f + 0.06f * hv));
        D2D1_COLOR_F fillC = Lerp(g_accent, kWhite, std::min(1.f, hv) * 0.35f);
        if (frac > 0) RRect(RF(x0, y - th / 2, std::max(th, (x1 - x0) * frac), th), th / 2, fillC);
        if (hv > 0.02f) Circle(x0 + (x1 - x0) * frac, y, 6.5f * std::min(1.f, hv), WithA(kWhite, std::min(1.f, hv)));
        Text(g_fSmall, FmtDur(el), RF(ox + 18, y - 9, 42, 18), DWRITE_TEXT_ALIGNMENT_LEADING,
             g_dragSeek ? kWhite : kGray);
        Text(g_fSmall, L"-" + FmtDur(dur - el), RF(ox + tw - 62, y - 9, 44, 18), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        L.hasProg = true;
        L.progX0 = x0; L.progX1 = x1; L.progY = y;
    } else {
        cyCtl = oy + 126;
    }

    // кнопки: подсветка при наведении + «пружинное» нажатие
    float cx = ox + tw / 2;
    float bx[3] = {cx - 64, cx, cx + 64}, br[3] = {19, 23, 19};
    for (int i = 0; i < 3; i++) {
        AddBtn(L, bx[i], cyCtl, br[i]);
        float sc = (float)g_btnScale[i].x;
        float hv = (float)Clamp(g_btnHov[i].x, 0.0, 1.0);
        float pressBoost = g_pressBtn == i ? 0.08f : 0.f;
        if (hv > 0.01f || pressBoost > 0) Circle(bx[i], cyCtl, br[i] * sc, Ov(0.11f * hv + pressBoost));
    }
    IconSkip(bx[0], cyCtl, 10 * (float)g_btnScale[0].x, false, g_media.canPrev);
    IconPlayPause(bx[1], cyCtl, 11 * (float)g_btnScale[1].x, (float)Clamp(g_playMorph.x, 0.0, 1.0));
    IconSkip(bx[2], cyCtl, 10 * (float)g_btnScale[2].x, true, g_media.canNext);
}

// ----------------------------------------------------------------------------
// Текст песни в такт (караоке-подсветка по словам)
// ----------------------------------------------------------------------------
static double LyricTime() { return CurElapsed() / 1e7 + g_set.lyricOffsetMs / 1000.0 + 0.12; }
static int FindLine(double t) {
    int lo = 0, hi = (int)g_lyr.lines.size() - 1, r = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_lyr.lines[m].t <= t) { r = m; lo = m + 1; } else hi = m - 1;
    }
    return r;
}
static std::wstring LineText(int idx) {
    if (idx < 0 || idx >= (int)g_lyr.lines.size()) return g_media.title;
    const std::wstring& t = g_lyr.lines[idx].text;
    return t.empty() ? std::wstring(L"♪  ♪  ♪") : t;
}
enum { KS_KARAOKE, KS_PLAIN, KS_NOTE };
static int LineStyle(int idx) {
    if (idx < 0 || idx >= (int)g_lyr.lines.size()) return KS_PLAIN;
    return g_lyr.lines[idx].text.empty() ? KS_NOTE : KS_KARAOKE;
}
static void EnsureLine(LineLayout& L, int idx, IDWriteTextFormat* fmt, float h) {
    if (!fmt) return;
    std::wstring t = LineText(idx);
    if (L.lay && L.idx == idx && L.fmt == fmt && L.text == t) return;
    L.Release();
    L.idx = idx;
    L.text = t;
    L.fmt = fmt;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (SUCCEEDED(g_dwf->CreateTextLayout(t.c_str(), (UINT32)t.size(), fmt, 100000.f, h, &L.lay))) {
        DWRITE_TRIMMING none = {DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        L.lay->SetTrimming(&none, nullptr);
        DWRITE_TEXT_METRICS tm = {};
        if (SUCCEEDED(L.lay->GetMetrics(&tm))) L.w = tm.widthIncludingTrailingWhitespace;
    }
}
static float HitX(IDWriteTextLayout* lay, int pos) {
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m;
    if (FAILED(lay->HitTestTextPosition((UINT32)std::max(0, pos), FALSE, &x, &y, &m))) return 0;
    return x;
}
// Сколько строки уже «спето» — X в DIP от начала текста
static float SweepX(int idx, const LineLayout& L, double t) {
    if (idx < 0 || idx >= (int)g_lyr.lines.size() || !L.lay) return 0;
    const LyricLine& ln = g_lyr.lines[idx];
    int n = (int)ln.words.size();
    if (n == 0) return 0;
    double next = idx + 1 < (int)g_lyr.lines.size() ? g_lyr.lines[idx + 1].t : ln.t + 6;
    std::vector<double> st(n + 1);
    if (ln.wordTimes) {
        for (int i = 0; i < n; i++) st[i] = ln.words[i].t;
        st[n] = std::min(next, st[n - 1] + 0.9);
    } else {
        // время слов не известно — раскладываем строку по длине слов (~13 символов/с)
        int total = 0;
        for (auto& w : ln.words) total += w.len + 1;
        double dur = std::max(0.35, std::min(next - ln.t, 0.5 + 0.075 * total));
        int cum = 0;
        for (int i = 0; i < n; i++) { st[i] = ln.t + dur * cum / total; cum += ln.words[i].len + 1; }
        st[n] = ln.t + dur;
    }
    if (t <= st[0]) return 0;
    if (t >= st[n]) return L.w;
    int i = 0;
    while (i + 1 < n && t >= st[i + 1]) i++;
    double span = st[i + 1] - st[i];
    float f = span > 1e-3 ? (float)Clamp((t - st[i]) / span, 0.0, 1.0) : 1.f;
    float x0 = HitX(L.lay, ln.words[i].start), x1 = HitX(L.lay, ln.words[i].start + ln.words[i].len);
    return x0 + (x1 - x0) * f;
}
static void DrawLine(const LineLayout& L, D2D1_RECT_F box, float hx, float alpha, float yOff, float scroll, int style) {
    if (!L.lay || alpha <= 0.01f) return;
    float boxW = box.right - box.left, boxH = box.bottom - box.top;
    bool over = L.w > boxW + 0.5f;
    float x = over ? box.left - scroll : box.left + (boxW - L.w) / 2;
    float y = box.top + yOff;
    ID2D1LinearGradientBrush* mask = nullptr;
    if (over) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {Pt(box.left, 0), Pt(box.right, 0)};
        g_rt->CreateLinearGradientBrush(&lg, nullptr, scroll > 0.5f ? g_gsBoth : g_gsRight, &mask);
        D2D1_LAYER_PARAMETERS lp = {};
        lp.contentBounds = D2D1_RECT_F{box.left, box.top - 30, box.right, box.bottom + 30};
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        lp.maskTransform = D2D1::Matrix3x2F::Identity();
        lp.opacity = 1.f;
        lp.opacityBrush = mask;
        g_rt->PushLayer(&lp, g_layer2);
    }
    if (style == KS_KARAOKE) {
        SetColor(Ov(0.32f * alpha));
        g_rt->DrawTextLayout(Pt(x, y), L.lay, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
        if (hx > 0.5f) {
            D2D1_RECT_F c = {x - 4, y - 30, x + hx, y + boxH + 30};
            g_rt->PushAxisAlignedClip(&c, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            SetColor(WithA(kWhite, alpha));
            g_rt->DrawTextLayout(Pt(x, y), L.lay, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            g_rt->PopAxisAlignedClip();
        }
    } else if (style == KS_PLAIN) {
        SetColor(WithA(kGray, alpha));
        g_rt->DrawTextLayout(Pt(x, y), L.lay, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    } else {
        float pulse = 0.55f + 0.45f * (float)std::sin(NowSec() * 3.5);
        SetColor(WithA(g_accent, alpha * pulse));
        g_rt->DrawTextLayout(Pt(x, y), L.lay, g_brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    if (over) {
        g_rt->PopLayer();
        SafeRelease(mask);
    }
}
// Текущая + уходящая строка с анимацией «уезжает вверх / приезжает снизу»
static void DrawLyricPair(LineLayout& cur, LineLayout& prev, Spring& scroll, D2D1_RECT_F box, float shift) {
    float boxW = box.right - box.left;
    float p = Smooth((NowSec() - g_lyrLineT) / 0.32);
    if (p < 1.f && prev.lay) {
        float sc = std::max(0.f, prev.w - boxW);
        int ps = LineStyle(g_lyrPrevLine);
        DrawLine(prev, box, prev.w, 1.f - p, -shift * p, sc, ps);
    }
    int st = LineStyle(g_lyrLine);
    float hx = st == KS_KARAOKE ? SweepX(g_lyrLine, cur, LyricTime()) : 0.f;
    scroll.target = cur.w > boxW ? Clamp(hx - boxW * 0.45f, 0.f, cur.w - boxW) : 0.f;
    DrawLine(cur, box, hx, p, shift * (1.f - p), (float)scroll.x, st);
}
static void UpdateLyrics() {
    if (!LyricsReady()) return;
    int idx = FindLine(LyricTime());
    bool changed = idx != g_lyrLine;
    if (changed) {
        g_lyrPrevLine = g_lyrLine;
        g_lyrLine = idx;
        g_lyrLineT = g_lyrPrevLine == -100 ? NowSec() - 1 : NowSec();
        std::swap(g_llPrev, g_llCur);
        std::swap(g_llXPrev, g_llXCur);
        g_lyrScroll.Set(0);
        g_lyrScrollX.Set(0);
    }
    EnsureLine(g_llCur, idx, g_fLyric, 38);
    EnsureLine(g_llXCur, idx, g_fLyricBig, 26);
    if (changed && g_mode == M_LYRICS) Retarget();
}
static void ResetLyrics() {
    g_lyr.lines.clear();
    g_lyrState = LYR_NONE;
    g_lyrLine = g_lyrPrevLine = -100;
    g_llCur.Release(); g_llPrev.Release(); g_llXCur.Release(); g_llXPrev.Release();
    g_lyrReqKey.clear();
    g_lyrReqDur = -1;
    KillTimer(g_hwnd, T_LYR);
}
static void RequestLyrics(bool force) {
    if (!g_set.lyrics || !g_media.has) return;
    std::wstring key = g_media.title + L"\x1f" + g_media.artist;
    if (!force && key == g_lyrReqKey) return;
    g_lyrReqKey = key;
    INT64 d = Duration();
    g_lyrReqDur = d > 0 ? (int)std::lround(d / 1e7) : 0;
    g_lyrReq = g_lyrSvc.Request(g_media.title, g_media.artist, g_media.album, g_lyrReqDur);
}

// ----------------------------------------------------------------------------
// Иконки для системных тостов и буфера
// ----------------------------------------------------------------------------
static float MeasureW(IDWriteTextFormat* f, const std::wstring& s) {
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* l = nullptr;
    float w = 0;
    if (SUCCEEDED(g_dwf->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 100000.f, 100.f, &l))) {
        DWRITE_TEXT_METRICS m = {};
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}
// обрезать строку с «…», чтобы влезла в ширину w
static std::wstring FitTextW(IDWriteTextFormat* f, const std::wstring& s0, float w) {
    std::wstring s = s0;
    for (auto& c : s) if (c == L'\r' || c == L'\n') c = L' ';
    if (w <= 8 || MeasureW(f, s) <= w) return s;
    size_t lo = 0, hi = s.size();
    while (lo < hi) {  // двоичный поиск — длинный текст уведомления не тормозит
        size_t mid = (lo + hi + 1) / 2;
        if (MeasureW(f, s.substr(0, mid) + L"…") <= w) lo = mid; else hi = mid - 1;
    }
    std::wstring t = s.substr(0, lo);
    while (!t.empty() && t.back() == L' ') t.pop_back();
    return t + L"…";
}
static void Line(float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c) {
    SetColor(c);
    g_rt->DrawLine(Pt(x0, y0), Pt(x1, y1), g_brush, w, nullptr);
}
static void StrokeRRect(D2D1_RECT_F r, float rad, float w, D2D1_COLOR_F c) {
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    SetColor(c);
    g_rt->DrawRoundedRectangle(&rr, g_brush, w, nullptr);
}
// дуга от угла a0 до a1 (радианы, по часовой), только обводка
static void Ring(float cx, float cy, float r, float w, D2D1_COLOR_F c) {
    SetColor(c);
    g_rt->DrawEllipse(D2D1::Ellipse(Pt(cx, cy), r, r), g_brush, w);
}
static void StrokeArc(float cx, float cy, float r, float a0, float a1, float w, D2D1_COLOR_F c) {
    if (a1 - a0 >= 6.27f) { Ring(cx, cy, r, w, c); return; }  // полный круг дугой не рисуется (начало = конец)
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sk = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&sk))) { SafeRelease(g); return; }
    sk->BeginFigure(Pt(cx + r * std::cos(a0), cy + r * std::sin(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    D2D1_ARC_SEGMENT a = {Pt(cx + r * std::cos(a1), cy + r * std::sin(a1)), {r, r}, 0.f,
                          D2D1_SWEEP_DIRECTION_CLOCKWISE, (a1 - a0) > 3.14159f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL};
    sk->AddArc(&a);
    sk->EndFigure(D2D1_FIGURE_END_OPEN);
    sk->Close();
    sk->Release();
    SetColor(c);
    g_rt->DrawGeometry(g, g_brush, w, nullptr);
    g->Release();
}
static void IconSun(float cx, float cy, D2D1_COLOR_F c) {
    Circle(cx, cy, 4.2f, c);
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159f / 4;
        Line(cx + 6.8f * std::cos(a), cy + 6.8f * std::sin(a), cx + 9.2f * std::cos(a), cy + 9.2f * std::sin(a), 1.7f, c);
    }
}
static void IconBattery(float cx, float cy, float pct, bool charging, D2D1_COLOR_F c) {
    D2D1_RECT_F body = RF(cx - 10, cy - 5.5f, 18, 11);
    StrokeRRect(body, 3, 1.4f, WithA(kWhite, 0.75f));
    RRect(RF(cx + 8.6f, cy - 2.5f, 2, 5), 1, WithA(kWhite, 0.75f));
    float fw = std::max(1.5f, 14.f * Clamp(pct, 0.f, 1.f));
    RRect(RF(cx - 8, cy - 3.5f, fw, 7), 1.6f, c);
    if (charging) {
        SetColor(kWhite);
        D2D1_POINT_2F b[6] = {Pt(cx + 1.5f, cy - 7), Pt(cx - 3.5f, cy + 1), Pt(cx - 0.5f, cy + 1),
                              Pt(cx - 1.5f, cy + 7), Pt(cx + 3.5f, cy - 1), Pt(cx + 0.5f, cy - 1)};
        FillPoly(b, 6);
    }
}
static void IconHeadphones(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy + 1, 8, 3.14159f, 6.28318f, 2.f, c);
    RRect(RF(cx - 9.5f, cy, 4.5f, 8), 2, c);
    RRect(RF(cx + 5, cy, 4.5f, 8), 2, c);
}
static void IconMic(float cx, float cy, D2D1_COLOR_F c) {
    RRect(RF(cx - 3.2f, cy - 9, 6.4f, 11), 3.2f, c);
    StrokeArc(cx, cy - 1, 6, 0.f, 3.14159f, 1.7f, c);
    Line(cx, cy + 5, cx, cy + 8.5f, 1.7f, c);
}
static void IconCamera(float cx, float cy, D2D1_COLOR_F c) {
    RRect(RF(cx - 10, cy - 5.5f, 13, 11), 3, c);
    SetColor(c);
    D2D1_POINT_2F p[3] = {Pt(cx + 4, cy), Pt(cx + 10, cy - 5), Pt(cx + 10, cy + 5)};
    FillPoly(p, 3);
}
static void IconCaps(float cx, float cy, bool on, D2D1_COLOR_F c) {
    SetColor(on ? c : WithA(kWhite, 0.55f));
    D2D1_POINT_2F p[7] = {Pt(cx, cy - 9), Pt(cx + 8, cy - 1), Pt(cx + 3.5f, cy - 1), Pt(cx + 3.5f, cy + 4),
                          Pt(cx - 3.5f, cy + 4), Pt(cx - 3.5f, cy - 1), Pt(cx - 8, cy - 1)};
    FillPoly(p, 7);
    RRect(RF(cx - 3.5f, cy + 6, 7, 2.6f), 1, on ? c : WithA(kWhite, 0.55f));
}
static void IconPin(float cx, float cy, D2D1_COLOR_F c) {
    Circle(cx, cy - 3, 4, c);
    Line(cx, cy, cx, cy + 7, 1.6f, c);
}
static void IconCross(float cx, float cy, D2D1_COLOR_F c) {
    Line(cx - 4, cy - 4, cx + 4, cy + 4, 1.6f, c);
    Line(cx - 4, cy + 4, cx + 4, cy - 4, 1.6f, c);
}
static void IconSearch(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx - 1.5f, cy - 1.5f, 5, 0.f, 6.2f, 1.7f, c);
    Line(cx + 2.2f, cy + 2.2f, cx + 6, cy + 6, 1.8f, c);
}

// ----------------------------------------------------------------------------
// Иконки инструментов, полка, запись, загрузки
// ----------------------------------------------------------------------------
static void Corners(float cx, float cy, float s, float w, D2D1_COLOR_F c) {
    float k = s * 0.45f;
    Line(cx - s, cy - s, cx - s + k, cy - s, w, c); Line(cx - s, cy - s, cx - s, cy - s + k, w, c);
    Line(cx + s, cy - s, cx + s - k, cy - s, w, c); Line(cx + s, cy - s, cx + s, cy - s + k, w, c);
    Line(cx - s, cy + s, cx - s + k, cy + s, w, c); Line(cx - s, cy + s, cx - s, cy + s - k, w, c);
    Line(cx + s, cy + s, cx + s - k, cy + s, w, c); Line(cx + s, cy + s, cx + s, cy + s - k, w, c);
}
static void DrawToolIcon(int mode, float cx, float cy, D2D1_COLOR_F c) {
    switch (mode) {
        case TM_SHOT_AREA: Corners(cx, cy, 8, 1.8f, c); Circle(cx, cy, 2.2f, c); break;
        case TM_SHOT_FULL:
            StrokeRRect(RF(cx - 9, cy - 7, 18, 12), 2.5f, 1.7f, c);
            Line(cx, cy + 5, cx, cy + 8.5f, 1.7f, c);
            Line(cx - 4.5f, cy + 8.5f, cx + 4.5f, cy + 8.5f, 1.7f, c);
            break;
        case TM_OCR:
            Corners(cx, cy, 8, 1.6f, c);
            Line(cx - 4, cy - 4, cx + 4, cy - 4, 1.9f, c);
            Line(cx, cy - 4, cx, cy + 5, 1.9f, c);
            break;
        case TM_QR:
            StrokeRRect(RF(cx - 8, cy - 8, 6, 6), 1, 1.5f, c);
            StrokeRRect(RF(cx + 2, cy - 8, 6, 6), 1, 1.5f, c);
            StrokeRRect(RF(cx - 8, cy + 2, 6, 6), 1, 1.5f, c);
            RRect(RF(cx + 2, cy + 2, 2.5f, 2.5f), 0.5f, c);
            RRect(RF(cx + 5.5f, cy + 5.5f, 2.5f, 2.5f), 0.5f, c);
            RRect(RF(cx + 5.5f, cy + 2, 2.5f, 2.5f), 0.5f, c);
            break;
        case TM_PICK:
            Line(cx - 6, cy + 6, cx + 3, cy - 3, 3.2f, c);
            Circle(cx + 5, cy - 5, 3.4f, c);
            Line(cx - 8, cy + 8, cx - 6, cy + 6, 1.6f, c);
            break;
        case TM_REC:
            StrokeArc(cx, cy, 8, 0.f, 6.2f, 1.6f, c);
            Circle(cx, cy, 4.6f, Col(1.f, 0.27f, 0.27f));
            break;
    }
}
static void IconTray(float cx, float cy, D2D1_COLOR_F c) {
    Line(cx - 9, cy + 1, cx - 9, cy + 7, 1.8f, c);
    Line(cx + 9, cy + 1, cx + 9, cy + 7, 1.8f, c);
    Line(cx - 9, cy + 7, cx + 9, cy + 7, 1.8f, c);
    Line(cx, cy - 8, cx, cy + 2, 1.8f, c);
    Line(cx - 4, cy - 2, cx, cy + 2, 1.8f, c);
    Line(cx + 4, cy - 2, cx, cy + 2, 1.8f, c);
}
static void Spinner(float cx, float cy, float r, D2D1_COLOR_F c) {
    float a = (float)std::fmod(NowSec() * 5.0, 6.2831853);
    StrokeArc(cx, cy, r, 0.f, 6.2f, 2.f, WithA(c, 0.22f));
    StrokeArc(cx, cy, r, a, a + 1.9f, 2.f, c);
    Line(cx, cy - 3.5f, cx, cy + 2.5f, 1.5f, c);
    Line(cx - 2.6f, cy, cx, cy + 2.5f, 1.5f, c);
    Line(cx + 2.6f, cy, cx, cy + 2.5f, 1.5f, c);
}
static std::wstring FmtBytes(double b) {
    wchar_t s[32];
    if (b < 1024 * 1024) swprintf(s, 32, L"%.0f КБ", b / 1024);
    else if (b < 1024.0 * 1024 * 1024) swprintf(s, 32, L"%.1f МБ", b / 1024 / 1024);
    else swprintf(s, 32, L"%.2f ГБ", b / 1024 / 1024 / 1024);
    return s;
}

static const wchar_t* kToolNames[6] = {L"Область", L"Экран", L"Текст", L"QR-код", L"Пипетка", L"Запись"};
static const int kToolModes[6] = {TM_SHOT_AREA, TM_SHOT_FULL, TM_OCR, TM_QR, TM_PICK, TM_REC};

static void DrawToolsRow(Layout& L, float ox, float y, float tw) {
    RRect(RF(ox + 24, y, tw - 48, 1), 0.5f, Ov(0.07f));
    float cell = (tw - 24) / 6;
    L.toolCount = 6;
    for (int i = 0; i < 6; i++) {
        float cx = ox + 12 + cell * (i + 0.5f);
        D2D1_RECT_F r = RF(cx - cell / 2 + 3, y + 5, cell - 6, 52);
        L.tool[i] = r;
        if (g_hoverTool == i) RRect(r, 12, Ov(0.08f));
        bool recNow = kToolModes[i] == TM_REC && g_rec.Active();
        DrawToolIcon(kToolModes[i], cx, y + 23, recNow ? Col(1.f, 0.3f, 0.3f) : kWhite);
        Text(g_fSmall, recNow ? L"Стоп" : kToolNames[i], RF(cx - cell / 2, y + 36, cell, 16), DWRITE_TEXT_ALIGNMENT_CENTER,
             recNow ? Col(1.f, 0.45f, 0.45f) : kGray);
    }
}

static void DrawShelf(Layout& L, float ox, float y, float tw) {
    RRect(RF(ox + 24, y, tw - 48, 1), 0.5f, Ov(0.07f));
    wchar_t b[48];
    swprintf(b, 48, L"Полка · %d", (int)g_shelf.size());
    Text(g_fSmall, b, RF(ox + 24, y + 6, 160, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    L.shelfClear = RF(ox + tw - 104, y + 4, 80, 22);
    Text(g_fSmall, L"Очистить", L.shelfClear, DWRITE_TEXT_ALIGNMENT_TRAILING, g_hoverShelfClear ? kWhite : WithA(kGray, 0.8f));
    const float TW = 74;
    int maxTiles = std::min(8, (int)((tw - 32) / TW));
    int n = std::min((int)g_shelf.size(), maxTiles);
    L.shelfCount = n;
    for (int i = 0; i < n; i++) {
        float x = ox + 16 + i * TW;
        D2D1_RECT_F r = RF(x + 2, y + 26, TW - 4, 68);
        L.shelf[i] = r;
        L.shelfX[i] = RF(r.right - 20, r.top + 2, 18, 18);
        bool hov = g_hoverShelf == i;
        if (hov) RRect(r, 12, Ov(0.08f));
        bool more = i == maxTiles - 1 && (int)g_shelf.size() > maxTiles;
        if (more) {
            wchar_t m[16];
            swprintf(m, 16, L"+%d", (int)g_shelf.size() - maxTiles + 1);
            Text(g_fToast, m, RF(x, y + 30, TW, 40), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
            Text(g_fSmall, L"ещё", RF(x, y + 70, TW, 16), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
            continue;
        }
        ShelfItem& it = g_shelf[i];
        if (!it.icon.brush && !it.px.empty()) MakePic(it.icon, it.px, it.iw, it.ih);
        D2D1_RECT_F ir = RF(x + TW / 2 - 20, y + 30, 40, 40);
        if (it.icon.brush) DrawPicRounded(it.icon, ir, 6, 1.f);
        else RRect(ir, 8, Ov(0.1f));
        Text(g_fSmall, it.name, RF(x + 4, y + 72, TW - 8, 16), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        if (hov) {
            Circle(L.shelfX[i].left + 9, L.shelfX[i].top + 9, 9, g_hoverShelfX ? Col(1, 0.35f, 0.35f) : Col(0.25f, 0.25f, 0.3f));
            IconCross(L.shelfX[i].left + 9, L.shelfX[i].top + 9, kWhite);
        }
    }
}

static void DrawRecPill(float ox, float oy, float tw, float th) {
    float cy = oy + th / 2;
    if (g_rec.Paused()) {  // пауза — две жёлтые полоски
        RRect(RF(ox + 15, cy - 5.5f, 3.5f, 11), 1.5f, Col(1.f, 0.8f, 0.3f));
        RRect(RF(ox + 21.5f, cy - 5.5f, 3.5f, 11), 1.5f, Col(1.f, 0.8f, 0.3f));
    } else Circle(ox + 20, cy, 5.5f, g_recBlink ? Col(1.f, 0.25f, 0.25f) : Col(0.6f, 0.15f, 0.15f));
    if (g_rec.HasMic()) IconMic(ox + tw - 52, cy, g_rec.MicMuted() ? WithA(kGray, 0.5f) : kGray);
    int s = (int)g_rec.Elapsed();
    wchar_t b[32];
    swprintf(b, 32, L"%d:%02d", s / 60, s % 60);
    Text(g_fTime, b, RF(ox + 34, oy, 70, th), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    RRect(RF(ox + tw - 30, cy - 6, 12, 12), 2.5f, kWhite);  // «стоп»
}
static void DrawDlPill(float ox, float oy, float tw, float th) {
    float cy = oy + th / 2;
    Spinner(ox + 21, cy, 8, g_accent);
    std::wstring right = FmtBytes((double)g_dl.bytes);
    if (g_dl.speed > 0) right += L" · " + FmtBytes(g_dl.speed) + L"/с";
    float rw = MeasureW(g_fSmall, right);
    std::wstring name = g_dl.name;
    if (g_dl.active > 1) name = std::to_wstring(g_dl.active) + L" загрузки · " + name;
    Text(g_fToast, name, RF(ox + 38, oy, tw - 38 - rw - 26, th), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fSmall, right, RF(ox + tw - rw - 18, oy, rw + 4, th), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
}
static void DrawDropPill(float ox, float oy, float tw, float th) {
    float cy = oy + th / 2;
    float pulse = 0.75f + 0.25f * (float)std::sin(NowSec() * 5);
    Circle(ox + 34, cy, 20, WithA(g_accent, 0.18f * pulse + 0.1f));
    IconTray(ox + 34, cy, g_accent);
    Text(g_fToast, g_dropCount > 1 ? L"Отпусти — положу на полку" : L"Отпусти — положу на полку",
         RF(ox + 64, oy + 10, tw - 80, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fSmall, L"файл останется на своём месте", RF(ox + 64, oy + 32, tw - 80, 18), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
}

// ---- игровая пилюля: «143 FPS · CPU 39% · GPU 91%»
struct HudSeg { std::wstring t; D2D1_COLOR_F c; };
static std::vector<HudSeg> GameHudSegs() {
    std::vector<HudSeg> v;
    wchar_t b[48];
    if (g_game.fps >= 0) {
        swprintf(b, 48, L"%d FPS", (int)(g_game.fps + 0.5f));
        D2D1_COLOR_F c = g_game.fps >= 55 ? Col(0.45f, 0.9f, 0.5f) : g_game.fps >= 30 ? Col(1.f, 0.8f, 0.3f) : Col(1.f, 0.42f, 0.38f);
        v.push_back({b, c});
    }
    if (g_ssOk) {
        swprintf(b, 48, L"CPU %d%%", (int)(g_ss.cpu + 0.5f));
        v.push_back({b, kWhite});
        if (g_ss.gpu >= 0) { swprintf(b, 48, L"GPU %d%%", (int)(g_ss.gpu + 0.5f)); v.push_back({b, kWhite}); }
        if (g_game.fps < 0) { swprintf(b, 48, L"RAM %d%%", (int)(g_ss.ram + 0.5f)); v.push_back({b, kWhite}); }
    } else if (v.empty()) v.push_back({L"Игровой режим", kWhite});
    return v;
}
static float GameHudWidth() {
    float w = 44;
    auto v = GameHudSegs();
    for (size_t i = 0; i < v.size(); i++) w += MeasureW(g_fSmall, v[i].t) + (i ? 16.f : 0.f);
    return Clamp(std::ceil(w / 8) * 8, 140.f, 360.f);  // шаг 8 — чтобы ширина не дёргалась каждую секунду
}
static void IconGamepad(float cx, float cy, D2D1_COLOR_F c) {
    RRect(RF(cx - 8, cy - 4.5f, 16, 9.5f), 4.5f, c);
    D2D1_COLOR_F k = kBgSolid;
    Line(cx - 5.5f, cy, cx - 2.5f, cy, 1.3f, k);
    Line(cx - 4, cy - 1.5f, cx - 4, cy + 1.5f, 1.3f, k);
    Circle(cx + 3.f, cy + 1.f, 1.f, k);
    Circle(cx + 5.f, cy - 1.f, 1.f, k);
}
static void DrawGamePill(float ox, float oy, float tw, float th) {
    float cy = oy + th / 2;
    IconGamepad(ox + 22, cy, WithA(kGray, 0.9f));
    auto v = GameHudSegs();
    float x = ox + 38;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) { Circle(x + 6, cy, 1.3f, WithA(kGray, 0.6f)); x += 12; }
        float w = MeasureW(g_fSmall, v[i].t);
        Text(g_fSmall, v[i].t, RF(x, oy, w + 4, th), DWRITE_TEXT_ALIGNMENT_LEADING, v[i].c);
        x += w + 4;
    }
}

// ----------------------------------------------------------------------------
// Виджеты: погода, таймер, календарь; звонок; уведомление
// ----------------------------------------------------------------------------
static const D2D1_COLOR_F kCloud = {0.86f, 0.89f, 0.95f, 1.f};
static void Cloud(float cx, float cy, float k, D2D1_COLOR_F c) {
    Circle(cx - 3.5f * k, cy + 1.f * k, 4.f * k, c);
    Circle(cx + 1.f * k, cy - 1.5f * k, 5.2f * k, c);
    Circle(cx + 5.f * k, cy + 1.5f * k, 3.5f * k, c);
    RRect(RF(cx - 7.f * k, cy + 1.f * k, 15.f * k, 4.5f * k), 2.f * k, c);
}
static void SunK(float cx, float cy, float k) {
    D2D1_COLOR_F c = {1.f, 0.8f, 0.25f, 1.f};
    Circle(cx, cy, 4.2f * k, c);
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159f / 4;
        Line(cx + 6.6f * k * std::cos(a), cy + 6.6f * k * std::sin(a), cx + 9.f * k * std::cos(a), cy + 9.f * k * std::sin(a), 1.5f * k, c);
    }
}
static void MoonK(float cx, float cy, float k, D2D1_COLOR_F bg) {
    Circle(cx, cy, 6.f * k, Col(0.95f, 0.92f, 0.75f));
    Circle(cx + 2.8f * k, cy - 2.2f * k, 5.2f * k, bg);
}
static void DrawWx(int icon, bool day, float cx, float cy, float k, D2D1_COLOR_F bg) {
    D2D1_COLOR_F blue = {0.45f, 0.7f, 1.f, 1.f};
    switch (icon) {
        case WX_CLEAR: if (day) SunK(cx, cy, k); else MoonK(cx, cy, k, bg); break;
        case WX_PARTLY:
            if (day) SunK(cx - 3.f * k, cy - 3.f * k, 0.75f * k); else MoonK(cx - 3.f * k, cy - 3.f * k, 0.75f * k, bg);
            Cloud(cx + 1.5f * k, cy + 1.5f * k, 0.85f * k, kCloud);
            break;
        case WX_CLOUD: Cloud(cx, cy, k, kCloud); break;
        case WX_FOG:
            for (int i = 0; i < 3; i++) Line(cx - 8 * k + (i % 2) * 2 * k, cy - 4 * k + i * 4 * k, cx + 8 * k - (i % 2) * 2 * k, cy - 4 * k + i * 4 * k, 1.8f * k, kCloud);
            break;
        case WX_DRIZZLE:
            Cloud(cx, cy - 2 * k, k, kCloud);
            Circle(cx - 3 * k, cy + 7 * k, 1.1f * k, blue); Circle(cx + 2 * k, cy + 8 * k, 1.1f * k, blue);
            break;
        case WX_RAIN:
            Cloud(cx, cy - 2 * k, k, kCloud);
            for (int i = 0; i < 3; i++) Line(cx - 4 * k + i * 4 * k, cy + 5 * k, cx - 5.5f * k + i * 4 * k, cy + 9 * k, 1.5f * k, blue);
            break;
        case WX_SNOW:
            Cloud(cx, cy - 2 * k, k, kCloud);
            for (int i = 0; i < 3; i++) Circle(cx - 4 * k + i * 4 * k, cy + 7 * k + (i % 2) * 1.5f * k, 1.3f * k, kWhite);
            break;
        case WX_THUNDER: {
            Cloud(cx, cy - 2 * k, k, Col(0.7f, 0.72f, 0.8f));
            SetColor(Col(1.f, 0.85f, 0.2f));
            D2D1_POINT_2F b[4] = {Pt(cx + 1 * k, cy + 2 * k), Pt(cx - 3 * k, cy + 7 * k), Pt(cx, cy + 7 * k), Pt(cx - 2 * k, cy + 11 * k)};
            D2D1_POINT_2F b2[3] = {Pt(cx - 2 * k, cy + 11 * k), Pt(cx + 3 * k, cy + 5.5f * k), Pt(cx - 0.5f * k, cy + 5.5f * k)};
            FillPoly(b, 3); FillPoly(b2, 3); (void)b[3];
            break;
        }
    }
}
static std::wstring Temp(double t) {
    wchar_t b[16];
    int v = (int)std::lround(t);
    swprintf(b, 16, L"%d°", std::abs(v));
    return (v < 0 ? std::wstring(L"−") : std::wstring()) + b;
}

static double TimerLeft() {
    if (!g_tm.on) return 0;
    return g_tm.paused ? g_tm.left : std::max(0.0, g_tm.endAt - NowSec());
}
static std::wstring MMSS(double sec) {
    int s = (int)std::ceil(sec);
    wchar_t b[16];
    if (s >= 3600) swprintf(b, 16, L"%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 16, L"%d:%02d", s / 60, s % 60);
    return b;
}
static const wchar_t* TimerLabel() {
    return g_tm.kind == 1 ? L"Фокус" : g_tm.kind == 2 ? L"Перерыв" : L"Таймер";
}
static D2D1_COLOR_F TimerColor() {
    return g_tm.kind == 1 ? Col(1.f, 0.45f, 0.35f) : g_tm.kind == 2 ? Col(0.3f, 0.85f, 0.45f) : g_accent;
}
static void TimerRing(float cx, float cy, float r, float w) {
    double frac = g_tm.total > 0 ? TimerLeft() / g_tm.total : 0;
    StrokeArc(cx, cy, r, 0.f, 6.2f, w, Ov(0.15f));
    if (frac > 0.002) StrokeArc(cx, cy, r, -1.5708f, -1.5708f + (float)(6.2831853 * std::min(0.999, frac)), w, TimerColor());
}

static std::wstring EventWhen(const CalEvent& e) {
    FILETIME ft, lft;
    ft.dwLowDateTime = (DWORD)e.start;
    ft.dwHighDateTime = (DWORD)(e.start >> 32);
    SYSTEMTIME st, now;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    GetLocalTime(&now);
    auto dayNum = [](const SYSTEMTIME& t) {
        FILETIME f;
        SYSTEMTIME d = t;
        d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
        SystemTimeToFileTime(&d, &f);
        return (((long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / (86400LL * 10000000LL);
    };
    long long diff = dayNum(st) - dayNum(now);
    static const wchar_t* wd[] = {L"Вс", L"Пн", L"Вт", L"Ср", L"Чт", L"Пт", L"Сб"};
    wchar_t b[64];
    std::wstring day = diff == 0 ? L"Сегодня" : diff == 1 ? L"Завтра" : diff < 0 ? L"Сейчас" : L"";
    if (day.empty()) { swprintf(b, 64, L"%ls %02d.%02d", wd[st.wDayOfWeek % 7], st.wDay, st.wMonth); day = b; }
    if (e.allDay) return day + L" · весь день";
    swprintf(b, 64, L" %02d:%02d", st.wHour, st.wMinute);
    return day + b;
}
static const CalEvent* NextEvent() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long long now = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    for (auto& e : g_cal.events)
        if (e.end > now || (!e.allDay && e.start > now - 5 * 60 * 10000000LL)) return &e;
    return nullptr;
}

static void DrawWidgetsRow(Layout& L, float ox, float y, float tw) {
    RRect(RF(ox + 24, y, tw - 48, 1), 0.5f, Ov(0.07f));
    const float gap = 8, cw = (tw - 32 - 2 * gap) / 3, ch = 68, cy0 = y + 8;
    const D2D1_COLOR_F cardBg = {1, 1, 1, 0.06f}, cut = {0.075f, 0.075f, 0.085f, 1.f};
    L.hasWidgets = true;
    // погода
    float x0 = ox + 16;
    L.wxCard = RF(x0, cy0, cw, ch);
    RRect(L.wxCard, 14, cardBg);
    if (g_wx.ok) {
        DrawWx(WeatherIcon(g_wx.code), g_wx.isDay, x0 + 24, cy0 + 24, 1.15f, cut);
        Text(g_fWBig, Temp(g_wx.temp), RF(x0 + 44, cy0 + 8, cw - 50, 30), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        std::wstring mm = Temp(g_wx.tmin[0]) + L"…" + Temp(g_wx.tmax[0]);
        Text(g_fSmall, mm, RF(x0 + 8, cy0 + 9, cw - 16, 14), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
        std::wstring desc = WeatherText(g_wx.code);
        if (!g_wx.city.empty()) desc = g_wx.city + L" · " + desc;
        Text(g_fSmall, desc, RF(x0 + 10, cy0 + 46, cw - 20, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    } else {
        Text(g_fToast, L"Погода", RF(x0 + 12, cy0 + 10, cw - 24, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, g_wx.error.empty() ? L"загружаю…" : g_wx.error, RF(x0 + 12, cy0 + 36, cw - 24, 16),
             DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
    // таймер
    float x1 = x0 + cw + gap;
    RRect(RF(x1, cy0, cw, ch), 14, cardBg);
    L.timerOn = g_tm.on;
    if (g_tm.on) {
        TimerRing(x1 + 22, cy0 + 24, 11, 2.6f);
        Text(g_fWBig, MMSS(TimerLeft()), RF(x1 + 40, cy0 + 8, cw - 44, 30), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        std::wstring lab = TimerLabel();
        if (g_tm.kind == 1) lab += L" #" + std::to_wstring(g_tm.focusDone + 1);
        if (g_tm.paused) lab += L" · пауза";
        Text(g_fSmall, lab, RF(x1 + 10, cy0 + 46, cw - 70, 16), DWRITE_TEXT_ALIGNMENT_LEADING, TimerColor());
        L.tPause = RF(x1 + cw - 58, cy0 + 40, 24, 24);
        L.tStop = RF(x1 + cw - 30, cy0 + 40, 24, 24);
        float pcx = L.tPause.left + 12, pcy = L.tPause.top + 12;
        Circle(pcx, pcy, 12, Ov(g_hoverChip == 10 ? 0.18f : 0.1f));
        IconPlayPause(pcx, pcy, 6, g_tm.paused ? 0.f : 1.f);
        float scx = L.tStop.left + 12, scy = L.tStop.top + 12;
        Circle(scx, scy, 12, Ov(g_hoverChip == 11 ? 0.18f : 0.1f));
        RRect(RF(scx - 4.5f, scy - 4.5f, 9, 9), 2, kWhite);
    } else {
        Text(g_fToast, L"Таймер", RF(x1 + 12, cy0 + 8, cw - 24, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        static const wchar_t* chips[3] = {L"5 мин", L"15 мин", L"🍅 25"};
        float chw = (cw - 24 - 8) / 3;
        for (int i = 0; i < 3; i++) {
            L.chip[i] = RF(x1 + 12 + i * (chw + 4), cy0 + 36, chw, 24);
            RRect(L.chip[i], 9, Ov(g_hoverChip == i ? 0.18f : 0.09f));
            Text(g_fSmall, chips[i], L.chip[i], DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
        }
    }
    // календарь
    float x2 = x1 + cw + gap;
    L.calCard = RF(x2, cy0, cw, ch);
    RRect(L.calCard, 14, cardBg);
    const CalEvent* e = g_cal.ok ? NextEvent() : nullptr;
    if (e) {
        Text(g_fSmall, EventWhen(*e), RF(x2 + 12, cy0 + 9, cw - 24, 16), DWRITE_TEXT_ALIGNMENT_LEADING, g_accent);
        Text(g_fToast, e->title, RF(x2 + 12, cy0 + 26, cw - 20, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        if (!e->location.empty()) Text(g_fSmall, e->location, RF(x2 + 12, cy0 + 48, cw - 20, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    } else {
        SYSTEMTIME st;
        GetLocalTime(&st);
        static const wchar_t* days[] = {L"Воскресенье", L"Понедельник", L"Вторник", L"Среда", L"Четверг", L"Пятница", L"Суббота"};
        static const wchar_t* mon[] = {L"января", L"февраля", L"марта", L"апреля", L"мая", L"июня", L"июля",
                                       L"августа", L"сентября", L"октября", L"ноября", L"декабря"};
        Text(g_fSmall, days[st.wDayOfWeek % 7], RF(x2 + 12, cy0 + 9, cw - 24, 16), DWRITE_TEXT_ALIGNMENT_LEADING, g_accent);
        wchar_t b[32];
        swprintf(b, 32, L"%d %ls", st.wDay, mon[(st.wMonth + 11) % 12]);
        Text(g_fToast, b, RF(x2 + 12, cy0 + 26, cw - 20, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        std::wstring sub = g_set.icsUrl.empty() ? L"добавь календарь в настройках"
                         : g_cal.ok ? L"событий нет" : (g_cal.error.empty() ? L"загружаю…" : g_cal.error);
        Text(g_fSmall, sub, RF(x2 + 12, cy0 + 48, cw - 20, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
}

static void IconPhone(float cx, float cy, D2D1_COLOR_F c) {
    StrokeArc(cx, cy + 3, 6.5f, 3.4f, 6.02f, 3.f, c);
    RRect(RF(cx - 8.5f, cy + 1, 5, 4), 1.5f, c);
    RRect(RF(cx + 3.5f, cy + 1, 5, 4), 1.5f, c);
}
static void DrawTimerPill(float ox, float oy, float tw, float th) {
    float cy = oy + th / 2;
    TimerRing(ox + 21, cy, 8, 2.2f);
    RollText(3, g_fTime, MMSS(TimerLeft()), RF(ox + 38, oy, 70, th), DWRITE_TEXT_ALIGNMENT_LEADING, g_tm.paused ? kGray : kWhite);
    Text(g_fSmall, g_tm.paused ? L"пауза" : TimerLabel(), RF(ox + tw - 100, oy, 84, th), DWRITE_TEXT_ALIGNMENT_TRAILING, TimerColor());
}
static void DrawCallPill(Layout& L, float ox, float oy, float tw, float th) {
    float cy = oy + th / 2;
    Circle(ox + 20, cy, 11, Col(0.2f, 0.78f, 0.38f));
    IconPhone(ox + 20, cy - 1, kWhite);
    Text(g_fToast, g_callApp, RF(ox + 38, oy, tw - 38 - 100, th), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fSmall, MMSS(NowSec() - g_callStart), RF(ox + tw - 100, oy, 56, th), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
    float mx = ox + tw - 22;
    L.mic = RF(mx - 14, cy - 14, 28, 28);
    Circle(mx, cy, 13, g_micMuted ? Col(1.f, 0.3f, 0.3f) : Ov(g_hoverMic ? 0.22f : 0.12f));
    IconMic(mx, cy + 1.5f, kWhite);
    if (g_micMuted) Line(mx - 7, cy - 7, mx + 7, cy + 7, 2.f, kWhite);
}
static void DrawNotifPill(float ox, float oy, float tw, float th) {
    if (!g_notif) return;
    const NotifData& n = *g_notif;
    D2D1_RECT_F ir = RF(ox + 12, oy + (th - 42) / 2, 42, 42);
    if (g_notifPic.brush && n.avatar) DrawPicRounded(g_notifPic, ir, 21.f, 1.f);
    else if (g_notifPic.brush) {  // значок приложения — на плашке и с полями, как в Windows
        RRect(ir, 11, Ov(0.08f));
        DrawPicRounded(g_notifPic, RF(ir.left + 6, ir.top + 6, 30, 30), 4.f, 1.f);
    } else { RRect(ir, 10, Ov(0.1f)); Text(g_fToast, n.app.substr(0, 1), ir, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite); }
    if (n.call) {
        Circle(ir.right - 4, ir.bottom - 4, 9, Col(0.2f, 0.78f, 0.38f));
        IconPhone(ir.right - 4, ir.bottom - 5, kWhite);
    }
    std::wstring title = n.title.empty() ? n.app : n.title;
    std::wstring body = g_set.notifPrivate && !n.call ? std::wstring(L"Новое уведомление") : n.body;
    float tx = ox + 66;
    // справа «Приложение · сейчас», заголовок — в оставшееся место (раньше они налезали друг на друга)
    std::wstring meta = n.app + (n.call ? L" · открыть" : L" · сейчас");
    meta = FitTextW(g_fSmall, meta, 150);
    float mw = MeasureW(g_fSmall, meta) + 4;
    float titleW = ox + tw - 18 - mw - 10 - tx;
    Text(g_fToast, FitTextW(g_fToast, title, titleW), RF(tx, oy + 11, titleW, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    Text(g_fSmall, meta, RF(ox + tw - 18 - mw, oy + 13, mw, 18), DWRITE_TEXT_ALIGNMENT_TRAILING,
         n.call ? Col(0.3f, 0.85f, 0.45f) : WithA(kGray, 0.8f));
    float reserve = (g_notif->replyArgs.empty() || g_set.notifPrivate || g_notifReply) ? 0.f : (g_hover ? 200.f : 100.f);
    float bw = tw - tx + ox - 16 - reserve;
    Text(g_fClip, FitTextW(g_fClip, body, bw), RF(tx, oy + 34, bw, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
}

static void DrawToast(float ox, float oy, float tw, float th) {
    const Toast& t = g_toast;
    float cy = oy + th / 2, icx = ox + 25;
    switch (t.kind) {
        case TK_VOLUME: IconSpeaker(ox + 16, cy, 14, t.flag || t.value <= 0.001f); break;
        case TK_BRIGHT: IconSun(icx, cy, kWhite); break;
        case TK_BATTERY: IconBattery(icx, cy, t.value < 0 ? 1.f : t.value, t.flag, t.color); break;
        case TK_AUDIO: IconHeadphones(icx, cy, t.color); break;
        case TK_MIC: IconMic(icx, cy, t.color); break;
        case TK_CAM: IconCamera(icx, cy, t.color); break;
        case TK_CAPS: IconCaps(icx, cy, t.flag, t.color); break;
        case TK_SHOT: DrawToolIcon(TM_SHOT_AREA, icx, cy, t.color); break;
        case TK_TEXT: DrawToolIcon(TM_OCR, icx, cy, t.color); break;
        case TK_QR: DrawToolIcon(TM_QR, icx, cy, t.color); break;
        case TK_COLOR: Circle(icx, cy, 9, t.color); StrokeArc(icx, cy, 9, 0.f, 6.2f, 1.2f, Ov(0.5f)); break;
        case TK_REC: DrawToolIcon(TM_REC, icx, cy, kWhite); break;
        case TK_DOWNLOAD: IconTray(icx, cy, t.color); break;
        case TK_SHELF: IconTray(icx, cy, g_accent); break;
        case TK_REMIND: IconBell(icx, cy - 1, t.color); break;
        case TK_VPN: IconShield(icx, cy, t.color, t.flag); break;
        case TK_RAIN: DrawWx(WX_RAIN, true, icx, cy, 0.85f, kBgSolid); break;
        case TK_ERROR: Circle(icx, cy, 9, Col(1.f, 0.35f, 0.3f)); Line(icx, cy - 4.5f, icx, cy + 1, 2, kWhite); Circle(icx, cy + 4.2f, 1.2f, kWhite); break;
        case TK_LAYOUT: {
            float bw = std::max(26.f, MeasureW(g_fSmall, t.badge) + 10);
            RRect(RF(icx - bw / 2, cy - 9, bw, 18), 6, WithA(g_accent, 0.9f));
            Text(g_fSmall, t.badge, RF(icx - bw / 2, cy - 9, bw, 18), DWRITE_TEXT_ALIGNMENT_CENTER, Col(0.05f, 0.05f, 0.07f));
            break;
        }
    }
    if ((t.kind == TK_VOLUME || t.kind == TK_BRIGHT) && t.value >= 0) {
        float x0 = ox + 46, x1 = ox + tw - 58;
        RRect(RF(x0, cy - 3, x1 - x0, 6), 3, Ov(0.16f));
        float v = t.flag ? 0.f : t.value;
        if (v > 0) RRect(RF(x0, cy - 3, std::max(6.f, (x1 - x0) * v), 6), 3, kWhite);
        wchar_t b[16];
        swprintf(b, 16, L"%d%%", (int)std::lround(t.value * 100));
        Text(g_fTime, t.flag ? std::wstring(L"выкл") : std::wstring(b), RF(ox + tw - 54, oy, 40, th),
             DWRITE_TEXT_ALIGNMENT_TRAILING, t.flag ? kGray : kWhite);
        return;
    }
    float rightW = t.right.empty() ? 0.f : t.rightW + 18;
    Text(g_fToast, t.text, RF(ox + 46, oy, tw - 46 - 16 - rightW, th), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    if (!t.right.empty())
        Text(g_fTime, t.right, RF(ox + tw - 16 - t.rightW - 4, oy, t.rightW + 4, th), DWRITE_TEXT_ALIGNMENT_TRAILING,
             t.action != TA_NONE ? g_accent : (t.kind == TK_BATTERY || t.kind == TK_AUDIO) ? t.color : kGray);
}

// ----------------------------------------------------------------------------
// Панель буфера обмена
// ----------------------------------------------------------------------------
static const float kClipRowH = 52.f, kClipTop = 60.f;
static long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
}
static std::wstring Ago(long long ms) {
    long long d = (NowMs() - ms) / 1000;
    wchar_t b[48];
    if (d < 60) return L"только что";
    if (d < 3600) { swprintf(b, 48, L"%d мин назад", (int)(d / 60)); return b; }
    if (d < 86400) { swprintf(b, 48, L"%d ч назад", (int)(d / 3600)); return b; }
    if (d < 172800) return L"вчера";
    FILETIME ft;
    ULARGE_INTEGER u;
    u.QuadPart = (unsigned long long)(ms + 11644473600000LL) * 10000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    FILETIME lft;
    SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    swprintf(b, 48, L"%02d.%02d", st.wDay, st.wMonth);
    return b;
}
static std::wstring FirstLine(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && iswspace(s[a])) a++;
    size_t e = s.find_first_of(L"\r\n", a);
    std::wstring r = s.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
    for (auto& c : r) if (c == L'\t') c = L' ';
    bool more = e != std::wstring::npos && s.find_first_not_of(L" \t\r\n", e) != std::wstring::npos;
    return more ? r + L"  ↵" : r;
}
static std::wstring ClipTitle(const ClipItem& it) {
    if (it.kind == CK_IMAGE) {
        wchar_t b[64];
        swprintf(b, 64, L"Картинка %d×%d", it.w, it.h);
        return b;
    }
    if (it.kind == CK_FILES) {
        std::wstring s = it.preview;
        size_t nl = s.find(L'\n');
        std::wstring first = s.substr(0, nl);
        size_t sl = first.find_last_of(L"\\/");
        if (sl != std::wstring::npos) first = first.substr(sl + 1);
        int count = 1;
        for (wchar_t c : s) if (c == L'\n') count++;
        if (count > 1) { wchar_t b[32]; swprintf(b, 32, L"  +%d", count - 1); first += b; }
        return first;
    }
    return FirstLine(it.preview);
}
static std::wstring ClipSub(const ClipItem& it) {
    std::wstring k;
    switch (it.kind) {
        case CK_LINK: k = L"ссылка"; break;
        case CK_IMAGE: k = L"картинка"; break;
        case CK_FILES: k = L"файлы"; break;
        default: {
            wchar_t b[32];
            swprintf(b, 32, L"текст · %d симв.", it.textLen);
            k = b;
        }
    }
    return k + L" · " + Ago(it.created);
}
static void ClipRefilter() {
    g_clipView.clear();
    std::wstring q = LowerW(g_clipQuery);
    for (int i = 0; i < (int)g_clipItems.size(); i++) {
        if (q.empty() || LowerW(g_clipItems[i].preview).find(q) != std::wstring::npos ||
            LowerW(ClipTitle(g_clipItems[i])).find(q) != std::wstring::npos)
            g_clipView.push_back(i);
    }
    g_clipSel = Clamp(g_clipSel, 0, std::max(0, (int)g_clipView.size() - 1));
    g_dirty = true;
}
static void ClipEnsureVisible() {
    float listH = g_clipList.bottom - g_clipList.top;
    if (listH <= 0) listH = 440 - kClipTop - 10;
    float y = g_clipSel * kClipRowH;
    if (y < g_clipScroll.target) g_clipScroll.target = y;
    if (y + kClipRowH > g_clipScroll.target + listH) g_clipScroll.target = y + kClipRowH - listH;
    float maxS = std::max(0.f, g_clipView.size() * kClipRowH - listH);
    g_clipScroll.target = Clamp((float)g_clipScroll.target, 0.f, maxS);
    g_dirty = true;
}
static Pic* ClipThumb(const ClipItem& it) {
    if (it.thumb.empty()) return nullptr;
    auto f = g_clipThumbs.find(it.id);
    if (f != g_clipThumbs.end()) return f->second.brush ? &f->second : nullptr;
    Pic& p = g_clipThumbs[it.id];
    MakePic(p, it.thumb, it.tw, it.th);
    return p.brush ? &p : nullptr;
}
static void DrawClipPanel(float ox, float oy, float tw, float th) {
    // поиск
    D2D1_RECT_F sb = RF(ox + 14, oy + 14, tw - 28, 36);
    RRect(sb, 12, Ov(0.08f));
    IconSearch(ox + 32, oy + 32, kGray);
    D2D1_RECT_F tr = RF(ox + 46, oy + 14, tw - 46 - 24, 36);
    if (g_clipQuery.empty()) {
        Text(g_fSearch, L"Поиск в буфере обмена…", tr, DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.8f));
    } else {
        Text(g_fSearch, g_clipQuery, tr, DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    }
    if (g_caretOn && GetForegroundWindow() == g_hwnd) {
        float cx = tr.left + std::min(MeasureW(g_fSearch, g_clipQuery), tr.right - tr.left - 2) + 1;
        RRect(RF(cx, oy + 23, 1.6f, 18), 0.8f, g_accent);
    }
    Text(g_fSmall, L"Ctrl+Alt+V", RF(ox + tw - 110, oy + 14, 82, 36), DWRITE_TEXT_ALIGNMENT_TRAILING, WithA(kGray, 0.6f));

    g_clipList = RF(ox + 8, oy + kClipTop, tw - 16, th - kClipTop - 10);
    if (g_clipView.empty()) {
        Text(g_fClip, g_clipItems.empty() ? L"Тут появится всё, что ты копируешь" : L"Ничего не найдено",
             g_clipList, DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
        return;
    }
    g_rt->PushAxisAlignedClip(&g_clipList, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float scroll = (float)g_clipScroll.x;
    int first = std::max(0, (int)(scroll / kClipRowH));
    int last = std::min((int)g_clipView.size() - 1, (int)((scroll + (g_clipList.bottom - g_clipList.top)) / kClipRowH));
    for (int v = first; v <= last; v++) {
        const ClipItem& it = g_clipItems[g_clipView[v]];
        float y = g_clipList.top + v * kClipRowH - scroll;
        D2D1_RECT_F row = RF(g_clipList.left, y + 2, g_clipList.right - g_clipList.left, kClipRowH - 4);
        if (v == g_clipSel) RRect(row, 12, Ov(0.10f));
        else if (v == g_clipHover) RRect(row, 12, Ov(0.05f));
        // иконка/миниатюра
        D2D1_RECT_F ib = RF(row.left + 8, y + 8, 36, 36);
        Pic* th = it.kind == CK_IMAGE ? ClipThumb(it) : nullptr;
        if (th) DrawPicRounded(*th, ib, 8, 1.f);
        else {
            RRect(ib, 8, Ov(0.08f));
            float cx = ib.left + 18, cy = ib.top + 18;
            if (it.kind == CK_LINK) {
                StrokeArc(cx, cy, 8, 0.f, 6.2f, 1.6f, g_accent);
                Line(cx - 8, cy, cx + 8, cy, 1.2f, g_accent);
                StrokeArc(cx, cy, 3.5f, -1.5708f, 1.5708f, 1.2f, g_accent);
                StrokeArc(cx, cy, 3.5f, 1.5708f, 4.7124f, 1.2f, g_accent);
            } else if (it.kind == CK_FILES) {
                RRect(RF(cx - 9, cy - 6, 8, 4), 1.5f, WithA(g_accent, 0.9f));
                RRect(RF(cx - 9, cy - 4, 18, 12), 2.5f, g_accent);
            } else {
                for (int k = 0; k < 3; k++) RRect(RF(cx - 8, cy - 6 + k * 5.f, k == 2 ? 10.f : 16.f, 2), 1, kGray);
            }
        }
        bool hov = v == g_clipHover;
        float rightPad = hov || it.pinned ? 64.f : 14.f;
        float tx = row.left + 54;
        Text(g_fClip, ClipTitle(it), RF(tx, y + 7, row.right - tx - rightPad, 20), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        Text(g_fSmall, ClipSub(it), RF(tx, y + 27, row.right - tx - rightPad, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        // кнопки справа
        float pcx = row.right - 46, dcx = row.right - 20, bcy = y + kClipRowH / 2;
        if (it.pinned || hov) {
            if (hov && g_clipHoverBtn == 0) Circle(pcx, bcy, 12, Ov(0.10f));
            IconPin(pcx, bcy, it.pinned ? g_accent : kGray);
        }
        if (hov) {
            if (g_clipHoverBtn == 1) Circle(dcx, bcy, 12, Ov(0.10f));
            IconCross(dcx, bcy, kGray);
        }
    }
    g_rt->PopAxisAlignedClip();
}

#include "panel_draw.inc"

static void DrawContent(Layout& L) {
    float tw, th, tr;
    TargetSize(g_mode, tw, th, tr);
    float ox = L.px + L.pw / 2 - tw / 2, oy = L.py;
    L.btnCount = 0;
    L.hasProg = false;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t tbuf[16];
    swprintf(tbuf, 16, L"%02d:%02d", st.wHour, st.wMinute);
    std::wstring timeStr = tbuf;

    switch (g_mode) {
        case M_IDLE: {
            SYSTEMTIME ny;
            if (NyCountdown(ny)) {  // последний час года — обратный отсчёт
                wchar_t cd[16];
                swprintf(cd, 16, L"%02d:%02d", 59 - ny.wMinute, 59 - ny.wSecond);
                Text(g_fSmall, L"🎄 до Нового года", RF(ox + 16, oy, tw / 2 - 40, th), DWRITE_TEXT_ALIGNMENT_LEADING, Col(1.f, 0.85f, 0.35f));
                RollText(2, g_fTime, cd, RF(ox, oy, tw, th), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
                break;
            }
            if (Quiet()) IconMoon(ox + tw / 2 - MeasureW(g_fTime, timeStr) / 2 - 14, oy + th / 2, WithA(kGray, 0.9f), kBgSolid);
            if (g_set.idleWidget > 0) {  // мини-виджет: сеть или CPU/GPU
                RollText(1, g_fTime, timeStr, RF(ox, oy, tw, th), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
                std::wstring l, r;
                if (!g_ssOk) l = r = L"…";
                else if (g_set.idleWidget == 1) { l = L"↓ " + FmtSpeed(g_ss.down); r = L"↑ " + FmtSpeed(g_ss.up); }
                else {
                    wchar_t a[32], b[32];
                    swprintf(a, 32, L"CPU %.0f%%", g_ss.cpu);
                    if (g_ss.gpu < 0) swprintf(b, 32, L"ОЗУ %.0f%%", g_ss.ram);
                    else swprintf(b, 32, L"GPU %.0f%%", g_ss.gpu);
                    l = a; r = b;
                }
                Text(g_fSmall, l, RF(ox + 16, oy, tw / 2 - 50, th), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
                Text(g_fSmall, r, RF(ox + tw / 2 + 34, oy, tw / 2 - 50, th), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
            } else if (g_set.widgets && g_wx.ok) {  // погода слева, время по центру, температура справа
                if (g_set.critter > 0) DrawCritter(g_set.critter, ox + 24, oy + th / 2 + 1, 20);
                else DrawWx(WeatherIcon(g_wx.code), g_wx.isDay, ox + 24, oy + th / 2, 0.78f, kBgSolid);
                RollText(1, g_fTime, timeStr, RF(ox, oy, tw, th), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
                int dny = g_set.fxNY ? DaysToNy(ny) : -1;
                if (dny > 0) Text(g_fSmall, L"🎄 " + std::to_wstring(dny) + L" дн.", RF(ox + tw - 80, oy, 64, th), DWRITE_TEXT_ALIGNMENT_TRAILING, Col(1.f, 0.85f, 0.35f));
                else Text(g_fTime, Temp(g_wx.temp), RF(ox + tw - 70, oy, 54, th), DWRITE_TEXT_ALIGNMENT_TRAILING, kGray);
            } else {
                if (g_set.critter > 0) DrawCritter(g_set.critter, ox + 22, oy + th / 2 + 1, 19);
                RollText(1, g_fTime, timeStr, RF(ox, oy, tw, th), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
            }
            break;
        }

        case M_COMPACT: {
            float b = 1 + 0.07f * CoverBreath();
            D2D1_RECT_F cr = RF(ox + 7 + 12 - 12 * b, oy + th / 2 - 12 * b, 24 * b, 24 * b);
            CoverGlow(cr, 7);
            DrawCover(cr, 7);
            RollText(1, g_fTime, timeStr, RF(ox, oy, tw, th), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
            if (g_set.critter > 0) DrawCritter(g_set.critter, ox + tw - 24, oy + th / 2 + 1, 20);  // танцует вместо эквалайзера
            else DrawBars(ox + tw - 34, oy + th / 2, 14);
            break;
        }

        case M_LYRICS:
            if (LyricsReady()) {
                DrawCover(RF(ox + 7, oy + (th - 26) / 2, 26, 26), 7);
                DrawLyricPair(g_llCur, g_llPrev, g_lyrScroll, RF(ox + 42, oy, tw - 42 - 46, th), 11.f);
                if (g_set.critter > 0) DrawCritter(g_set.critter, ox + tw - 24, oy + th / 2 + 1, 20);
                else DrawBars(ox + tw - 34, oy + th / 2, 14);
            } else {
                DrawCover(RF(ox + 7, oy + (th - 24) / 2, 24, 24), 7);
                Text(g_fTime, timeStr, RF(ox, oy, tw, th), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
                DrawBars(ox + tw - 34, oy + th / 2, 14);
            }
            break;

        case M_PEEK:
            DrawCover(RF(ox + 9, oy + 9, 40, 40), 11);
            MarqueeText(g_mqPeekTitle, g_fPeekTitle, g_media.title, RF(ox + 60, oy + 9, tw - 112, 21), kWhite);
            MarqueeText(g_mqPeekArtist, g_fPeekArtist, g_media.artist, RF(ox + 60, oy + 29, tw - 112, 19), kGray);
            DrawBars(ox + tw - 38, oy + th / 2, 18);
            break;

        case M_TOAST:
            DrawToast(ox, oy, tw, th);
            break;
        case M_REC: DrawRecPill(ox, oy, tw, th); break;
        case M_TIMER: DrawTimerPill(ox, oy, tw, th); break;
        case M_CALL: DrawCallPill(L, ox, oy, tw, th); break;
        case M_NOTIF: DrawNotifPill(ox, oy, tw, th); DrawNotifActions(ox, oy, tw, th); break;
        case M_DEV: DrawDevCard(ox, oy, tw, th); break;
        case M_DL: DrawDlPill(ox, oy, tw, th); break;
        case M_GAME: DrawGamePill(ox, oy, tw, th); break;
        case M_DROP: DrawDropPill(ox, oy, tw, th); break;
        case M_CMD: DrawCmd(ox, oy, tw, th); break;

        case M_CLIP:
        case M_EXPANDED:
            DrawExpandedPanel(L, ox, oy, tw, th);
            break;

        case M_COUNT_UNUSED:
            L.toolCount = 0;
            L.shelfCount = 0;
            L.hasWidgets = false;
            if (g_media.has) {
                DrawExpandedMedia(L, ox, oy, tw, timeStr);
                if (LyricsReady()) {
                    float y0 = oy + 170;
                    RRect(RF(ox + 24, y0 - 6, tw - 48, 1), 0.5f, Ov(0.07f));
                    DrawLyricPair(g_llXCur, g_llXPrev, g_lyrScrollX, RF(ox + 24, y0, tw - 48, 26), 9.f);
                    int nx = g_lyrLine + 1;
                    if (nx >= 0 && nx < (int)g_lyr.lines.size())
                        Text(g_fLyricSmall, LineText(nx), RF(ox + 24, y0 + 27, tw - 48, 20), DWRITE_TEXT_ALIGNMENT_CENTER,
                             WithA(kGray, 0.75f));
                }
            } else {
                static const wchar_t* days[] = {L"воскресенье", L"понедельник", L"вторник", L"среда",
                                                L"четверг",     L"пятница",     L"суббота"};
                static const wchar_t* months[] = {L"января",   L"февраля", L"марта",  L"апреля",
                                                  L"мая",      L"июня",    L"июля",   L"августа",
                                                  L"сентября", L"октября", L"ноября", L"декабря"};
                wchar_t d[64];
                swprintf(d, 64, L"%ls, %d %ls", days[st.wDayOfWeek % 7], st.wDay, months[(st.wMonth + 11) % 12]);
                Text(g_fBig, timeStr, RF(ox, oy + 12, tw, 58), DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
                Text(g_fDate, d, RF(ox, oy + 72, tw, 22), DWRITE_TEXT_ALIGNMENT_CENTER, kGray);
            }
            {
                float y = oy + (g_media.has ? (LyricsReady() ? 228.f : 172.f) : 116.f);
                if (g_set.widgets) { DrawWidgetsRow(L, ox, y, tw); y += 84; }
                if (g_set.tools) { DrawToolsRow(L, ox, y, tw); y += 60; }
                if (ShelfShown()) DrawShelf(L, ox, y, tw);
            }
            break;
    }
}

// ----------------------------------------------------------------------------
// Кадр
// ----------------------------------------------------------------------------
static void ShowActionToast(int kind, const std::wstring& text, const std::wstring& right, int action,
                            const std::wstring& arg, D2D1_COLOR_F color, UINT ms);
#include "style.inc"
#include "fx.inc"
static float EarR() { return (float)std::max(0.0, sEar.x); }
// форма по стилю: скругления, «Минимал» — тонкая полоска в покое
static void StyleShape(Mode m, float& w, float& h, float& r) {
    bool big = m == M_EXPANDED || m == M_CLIP || m == M_CMD;
    if (big) r = std::min(r, g_sd.expR);
    else if (g_sd.maxR > 0 || g_sd.shape == SH_PIXEL) r = std::min(r, g_sd.maxR);
    if (g_sd.minimal && !g_hover && !g_pullDown && (m == M_IDLE || m == M_COMPACT || m == M_LYRICS || m == M_TIMER || m == M_DL)) {
        w = m == M_IDLE ? 110.f : 150.f;
        h = 6;
        r = 3;
    }
}
static void ReloadFonts();

static void Render() {
    if (!g_rt || !g_visible || !g_memDC) return;
    Layout& L = g_L;
    const bool dot = g_set.privacy && (g_priv.mic || g_priv.cam);
    const float E = EarR();
    const bool notch = E > 0.5f && g_sd.shape != SH_PIXEL;
    const float T = (float)std::max(0.0, sTop.x);
    float M = (dot ? 18.f : 4.f) + (notch ? E : 0.f) + (g_sd.shadow ? 14.f : 0.f) + BubbleSpace() + g_fxPad;
    float WB = 14 + (g_sd.shadow ? 10.f : 0.f) + (g_sd.season ? 46.f : 0.f) + g_fxPad;
    // не выходим за размер буфера
    M = std::min(M, std::max(4.f, ((float)g_dibW / g_scale - (float)sW.x) / 2 - 2));
    WB = std::min(WB, std::max(6.f, (float)g_dibH / g_scale - T - (float)sH.x - 2));
    L.pw = (float)std::max(12.0, sW.x);
    L.ph = (float)std::max(12.0, sH.x);
    L.pr = (float)Clamp(sR.x, 0.0, (double)L.ph / 2);
    L.bw = L.pw + 2 * M;
    L.bh = T + L.ph + WB;
    L.px = M;
    L.py = T - (float)Clamp(sHide.x, 0.0, 1.0) * (L.ph + 8);  // автоскрытие: уезжает за край

    int pxW = std::min(g_dibW, (int)std::ceil(L.bw * g_scale));
    int pxH = std::min(g_dibH, (int)std::ceil(L.bh * g_scale));
    RECT rc = {0, 0, pxW, pxH};
    if (FAILED(g_rt->BindDC(g_memDC, &rc))) return;

    g_marqueeActive = false;
    g_rt->BeginDraw();
    // тряска «нет» сдвигает весь остров
    const D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation((float)sShake.x, 0);
    g_rt->SetTransform(base);
    g_rt->Clear(Col(0, 0, 0, 0));

    float amp = (float)std::max(0.0, sWave.x);
    const bool thin = L.ph < 16;  // «Минимал»: полоска
    g_critterShown = false;
    if (thin) amp = 0;
    ID2D1PathGeometry* geo = g_sd.shape == SH_PIXEL ? BuildPixelRect(L.px, L.py, L.pw, L.ph, T < 1)
                                                   : BuildPill(L.px, L.py, L.pw, L.ph, L.pr, amp, (float)g_phase, notch);
    if (geo) {
        PaintIslandBg(geo, L);
        PaintIslandBorder(geo, L, notch);

        if (dot) {  // как на iPhone: зелёная — камера, оранжевая — микрофон
            D2D1_COLOR_F dc = g_priv.cam ? Col(0.20f, 0.84f, 0.36f) : Col(1.f, 0.62f, 0.12f);
            Circle(L.px + L.pw + (notch ? kEar + 4.f : 10.f), L.py + std::min(L.ph, 36.f) / 2 + (notch ? 4.f : 0.f), 4.2f, dc);
        }
        if (amp > 0.05f) {
            if (ID2D1PathGeometry* wl = BuildWaveLine(L.px, L.py, L.pw, L.ph, L.pr, amp, (float)g_phase)) {
                SetColor(WithA(g_accent, std::min(1.f, amp / 1.5f) * 0.9f));
                g_rt->DrawGeometry(wl, g_brush, 1.6f, nullptr);
                wl->Release();
            }
        }

        D2D1_LAYER_PARAMETERS lp = {};
        lp.contentBounds = D2D1_RECT_F{-1e6f, -1e6f, 1e6f, 1e6f};
        lp.geometricMask = geo;
        lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        lp.maskTransform = D2D1::Matrix3x2F::Identity();
        lp.opacity = g_content;
        lp.opacityBrush = nullptr;
        lp.layerOptions = D2D1_LAYER_OPTIONS_NONE;
        if (g_content > 0.003f) {
            g_rt->PushLayer(&lp, g_layer);
            // «живое» появление: содержимое чуть подрастает и всплывает
            if (g_set.anim > 0 && g_content < 0.999f) {
                float k = 0.93f + 0.07f * g_content, cx = L.px + L.pw / 2, cy = L.py + L.ph / 2;
                g_rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k, Pt(cx, cy)) * D2D1::Matrix3x2F::Translation(0, (1 - g_content) * 5) * base);
            }
            if (g_sd.shape == SH_PIXEL) g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
            if (!thin) DrawContent(L);
            else { L.btnCount = 0; L.hasProg = false; }
            g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
            g_rt->SetTransform(base);
            g_rt->PopLayer();
        } else {
            L.btnCount = 0;
            L.hasProg = false;
        }
    }
    if (geo) {
        // «Минимал»: полоска светится цветом происходящего
        if (thin) {
            float f = g_mode == M_TIMER ? 1.f - (float)(TimerLeft() / std::max(1.0, g_tm.total)) : -1.f;
            D2D1_COLOR_F c = g_mode == M_IDLE ? WithA(kGray, 0.6f) : g_mode == M_TIMER ? TimerColor() : g_accent;
            if (f >= 0) RRect(RF(L.px + 2, L.py + 1.5f, (L.pw - 4) * f, L.ph - 3), (L.ph - 3) / 2, c);
            else if (g_mode != M_IDLE) RRect(RF(L.px + 2, L.py + 1.5f, L.pw - 4, L.ph - 3), (L.ph - 3) / 2, WithA(c, 0.55f + 0.4f * CoverBreath()));
        }
        // прогресс бежит по контуру
        if (g_mode == M_TIMER && g_tm.total > 0) DrawOutlineProgress(L, 1.f - (float)(TimerLeft() / g_tm.total), TimerColor());
        if (g_mode == M_DL) DrawOutlineProgress(L, -1.f, g_accent);
        DrawBubble(L);
        DrawGarland(L);
        DrawParticles(L);
    }
    HRESULT hr = g_rt->EndDraw();
    SafeRelease(geo);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        DiscardTarget();
        CreateTarget();
        g_dirty = true;
        return;
    }

    POINT dst = {g_mon.left + ((g_mon.right - g_mon.left) - pxW) / 2, g_mon.top};  // остров всегда по центру
    SIZE sz = {pxW, pxH};
    POINT src = {0, 0};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hwnd, nullptr, &dst, &sz, g_memDC, &src, 0, &bf, ULW_ALPHA);
    g_winPos = dst;
    SyncEditors();
}

static void Frame() {
    if (g_panelOpenedPending) { g_panelOpenedPending = false; OnPanelOpened(); }
    double now = NowSec();
    double dt = Clamp(now - g_lastFrame, 0.0, 0.05);
    g_lastFrame = now;

    bool beat = false;
    if (AudioActive()) {
        g_vizLastActive = now;
        if (!g_viz.Running()) g_viz.Start();
        if (g_viz.Analyze(dt, g_spec)) {
            g_vizOk = true;
            g_level += (g_spec.level - g_level) * std::min(1.f, (float)(25 * dt));
            g_peak = g_level;
            beat = g_spec.beatNow;
        } else {
            // захват недоступен — запасной вариант: пиковый уровень
            g_vizOk = false;
            float p = g_audio.Peak();
            g_peak = p;
            float target = std::sqrt(std::min(1.f, p));
            float k = target > g_level ? 20.f : 5.f;
            g_level += (target - g_level) * std::min(1.f, (float)(k * dt));
        }
    } else {
        g_peak = 0;
        g_level += (0 - g_level) * std::min(1.f, (float)(6 * dt));
        if (g_level < 0.005f) g_level = 0;
        float d = std::exp(-(float)dt * 6.f);
        for (float& b : g_spec.band) b *= d;
        g_spec.beat *= d;
        if (g_viz.Running() && now - g_vizLastActive > 3) g_viz.Stop();
    }
    g_phase += dt * (2.2 + 7.0 * g_level);
    if (g_phase > 1e4) g_phase = 0;
    float bt = g_vizOk ? g_spec.beat : 0.f;
    sWave.target = AudioActive() ? (g_mode == M_EXPANDED ? 1.0 + 6.0 * g_level + 3.0 * bt : 0.5 + 2.4 * g_level + 1.3 * bt)
                                 : 0.0;
    if (beat) {  // лёгкий «пульс» острова в такт
        sWave.v += g_mode == M_EXPANDED ? 40 : 18;
        if (g_mode == M_COMPACT || g_mode == M_LYRICS) { sH.v += 30; sW.v += 60; }
    }
    UpdateLyrics();
    g_lyrScroll.Step(dt, 90, 1.0);
    g_lyrScrollX.Step(dt, 90, 1.0);

    {
        static const double K[3] = {300, 330, 290}, Z[3] = {0.92, 0.72, 0.56};
        int a = Clamp(g_set.anim, 0, 2);
        sW.Step(dt, K[a], Z[a]);
        sH.Step(dt, K[a] * 1.05, Z[a] + 0.04);
        sR.Step(dt, K[a], Z[a] + 0.1);
        sTab.Step(dt, 260, 0.82);
    }
    g_setScroll += (g_setScrollT - g_setScroll) * (float)std::min(1.0, dt * 16);
    if (std::fabs(g_setScrollT - g_setScroll) < 0.3f) g_setScroll = g_setScrollT;
    sTop.Step(dt, 220, 0.85);
    sEar.Step(dt, 220, 0.85);
    sShake.Step(dt, 900, 0.16);
    UpdateBubble();
    sBub.Step(dt, 200, g_set.anim == 2 ? 0.5 : 0.62);
    FxStep(dt);
    sWave.Step(dt, 260, 0.9);
    sPull.Step(dt, 200, 0.55);  // упругая «капля»
    sHide.Step(dt, 260, 0.9);

    // кнопки
    for (int i = 0; i < 3; i++) {
        g_btnHov[i].target = (g_hoverBtn == i) ? 1.0 : 0.0;
        g_btnScale[i].target = (g_pressBtn == i && g_hoverBtn == i) ? 0.82 : 1.0;
        g_btnScale[i].Step(dt, 520, 0.42);  // упругий отскок
        g_btnHov[i].Step(dt, 300, 1.0);
    }
    g_playMorph.target = g_media.status == 4 ? 1.0 : 0.0;
    g_playMorph.Step(dt, 360, 0.8);
    g_progHov.target = (g_hoverProg || g_dragSeek) ? 1.0 : 0.0;
    g_progHov.Step(dt, 380, 0.75);

    float af = ArtFade();
    g_accent = Lerp(g_accentFrom, g_accentTo, af);
    if (af >= 1.f && g_artPrev.bmp) g_artPrev.Release();

    g_content = Smooth((now - g_modeT - 0.05) / 0.22);

    // стекло: если остров вырос за пределы снятой подложки — переснимаем (не чаще ~7 раз в секунду)
    if (g_glassAllowed && now - g_glassT > 0.15) {
        RECT wr;
        GetWindowRect(g_hwnd, &wr);
        if (wr.left < g_glassRc.left || wr.right > g_glassRc.right || wr.bottom > g_glassRc.bottom) CaptureBackdrop();
    }
    Render();
    g_frames++;
    g_dirty = false;
}

// ----------------------------------------------------------------------------
// Ввод
// ----------------------------------------------------------------------------
static int HitBtn(int x, int y) {
    if (g_mode != M_EXPANDED || !g_media.has) return -1;
    float fx = x / g_scale, fy = y / g_scale;
    for (int i = 0; i < g_L.btnCount; i++) {
        const D2D1_RECT_F& r = g_L.btn[i];
        if (fx >= r.left && fx <= r.right && fy >= r.top && fy <= r.bottom) return i;
    }
    return -1;
}
static bool HitProg(int x, int y) {
    if (g_mode != M_EXPANDED || !g_L.hasProg) return false;
    float fx = x / g_scale, fy = y / g_scale;
    return fx >= g_L.progX0 - 8 && fx <= g_L.progX1 + 8 && std::fabs(fy - g_L.progY) <= 11;
}
static float ProgFrac(int x) {
    float fx = x / g_scale;
    if (g_L.progX1 <= g_L.progX0) return 0;
    return Clamp((fx - g_L.progX0) / (g_L.progX1 - g_L.progX0), 0.f, 1.f);
}

// объявлены ниже
static void RunTool(int mode);
static void ShelfRemove(int i);
static void ShelfOpen(int i);
static void ShelfClear();
static void ShelfDragOut(int i);
static void DoToastAction();
static void StartTimer(double seconds, int kind);
static void ToggleTimerPause();
static void StopTimer();
static void ToggleMicMute();
static void ActivateApp(const std::wstring& aumid, const std::wstring& exeName);
static void NotifClicked();
static void OpenSettings();
static void UpdateCallState();
static void CloseClip(bool restoreFocus);
static bool IsLink(const std::wstring& t);
static int ClipHit(int x, int y, int* btn);
static void ClipTogglePin(int viewIdx);
static void ClipDelete(int viewIdx);
static void ClipPaste(int viewIdx);

static bool InRect(const D2D1_RECT_F& r, int x, int y) {
    float fx = x / g_scale, fy = y / g_scale;
    return fx >= r.left && fx <= r.right && fy >= r.top && fy <= r.bottom;
}
static int HitTool(int x, int y) {
    if (g_mode != M_EXPANDED) return -1;
    for (int i = 0; i < g_L.toolCount; i++) if (InRect(g_L.tool[i], x, y)) return i;
    return -1;
}
static int HitShelf(int x, int y, bool* onX) {
    if (onX) *onX = false;
    if (g_mode != M_EXPANDED) return -1;
    for (int i = 0; i < g_L.shelfCount; i++)
        if (InRect(g_L.shelf[i], x, y)) {
            if (onX) *onX = InRect(g_L.shelfX[i], x, y);
            return i;
        }
    return -1;
}

#include "panel_logic.inc"
static bool IsElevated();
static void RestartElevated();
static void CheckFullscreen();
static std::wstring StampName(const wchar_t* prefix, const wchar_t* ext);
static void RecBegin(RECT rc, HWND win = nullptr);
static UpdateInfo g_upd;          // найденная новая версия
static std::wstring g_updStatus;  // строка для «О программе»
static bool g_updBusy = false;
static int g_welcomeStep = 0;
static void UpdateCheck(bool manual);
static void UpdateStartDownload();
static void SettingsBackup();
static void SettingsRestore();
static void WelcomeStep();
static void OpenLogFile() {
    std::wstring p = LogPath();
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) LogLine(L"Журнал открыт вручную — ошибок пока не было");
    ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}
#include "settings_panel.inc"
#include "rec.inc"

static void OnLButtonDown(int x, int y) {
    if (g_mode == M_CLIP || g_mode == M_CMD) return;
    if (ToastDragDown(x, y)) return;
    if (InRect(g_bubRect, x, y)) return;  // «пузырь» — по отпусканию
    PullStart(y);
    // «живой» отклик: остров слегка проминается под пальцем
    if (g_set.anim > 0 && g_mode != M_EXPANDED) { sW.v -= g_set.anim == 2 ? 170 : 110; sH.v -= g_set.anim == 2 ? 55 : 35; g_dirty = true; }
    if (g_mode == M_EXPANDED && PanelDown(x, y)) return;
    if (g_mode == M_EXPANDED) {
        bool onX = false;
        int si = HitShelf(x, y, &onX);
        if (si >= 0 && !onX && si < (int)g_shelf.size()) {
            g_shelfPress = si;
            g_shelfPressPt = {x, y};
            SetCapture(g_hwnd);
            return;
        }
    }
    if (HitProg(x, y)) {
        g_dragSeek = true;
        g_dragFrac = ProgFrac(x);
        SetCapture(g_hwnd);
        g_dirty = true;
        return;
    }
    int b = HitBtn(x, y);
    if (b >= 0) {
        g_pressBtn = b;
        SetCapture(g_hwnd);
        g_dirty = true;
    }
}

static void OnLButtonUp(int x, int y) {
    if (g_setDrag >= 0) { SettingsDragEnd(); return; }
    if (g_sbarDrag) { g_sbarDrag = false; ReleaseCapture(); g_dirty = true; return; }
    if (g_recDrag >= 0) { g_recDrag = -1; ReleaseCapture(); SaveSettings(); g_dirty = true; return; }
    if (PullEnd(y)) return;  // потянули — раскрылся (или пружинкой вернулся)
    if (InRect(g_bubRect, x, y) && g_bubShown != BUB_NONE && sBub.x > 0.5) {
        PlayUi(SN_POP);
        if (g_bubShown == BUB_DL) ShellExecuteW(nullptr, L"open", g_dlw.Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else { SetMode(M_EXPANDED); SwitchTab(g_bubShown == BUB_TIMER ? TAB_NOTE : TAB_HOME); }
        return;
    }
    if (g_toastPress) {  // просто клик по тосту скриншота
        g_toastPress = false;
        ReleaseCapture();
        DoToastAction();
        return;
    }
    if (g_mode == M_DEV) { SetMode(Collapsed()); return; }
    if (g_mode == M_NOTIF && NotifClick(x, y)) return;
    if (g_mode == M_CMD) {
        int i = CmdHit(x, y);
        if (i >= 0) RunCmd(i, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
        return;
    }
    if (g_volDrag) {
        g_volDrag = false;
        ReleaseCapture();
        g_volUi = -1;
        g_dirty = true;
        return;
    }
    if (g_mode == M_CLIP) {
        const Hit* h = HitAt(x, y);
        if (h && (h->id == H_TAB || h->id == H_PIN || h->id == H_SETTINGS)) { PanelClick(x, y); return; }
        int btn = -1;
        int v = ClipHit(x, y, &btn);
        if (v >= 0) {
            if (btn == 0) ClipTogglePin(v);
            else if (btn == 1) ClipDelete(v);
            else ClipPaste(v);
        }
        return;
    }
    if (g_dragSeek) {
        g_dragSeek = false;
        ReleaseCapture();
        INT64 dur = Duration();
        if (dur > 0) {
            g_seekPos = g_media.start + (INT64)(ProgFrac(x) * (double)dur);
            g_seekStamp = FileTimeNow();
            g_seekHoldUntil = NowSec() + 2.0;
            g_media.pos = g_seekPos;
            g_media.lastUpd = g_seekStamp;
            g_mw.Seek(g_seekPos);
        }
        g_dirty = true;
        return;
    }
    if (g_pressBtn >= 0) {
        int b = g_pressBtn;
        g_pressBtn = -1;
        ReleaseCapture();
        if (HitBtn(x, y) == b) {
            if (b == 0 && g_media.canPrev) g_mw.Command(CMD_PREV);
            if (b == 1) {
                g_mw.Command(CMD_TOGGLE);
                // мгновенный отклик, поток потом подтвердит
                if (g_media.status == 4) { g_media.pos = g_media.start + CurElapsed(); g_media.lastUpd = FileTimeNow(); g_media.status = 5; }
                else { g_media.lastUpd = FileTimeNow(); g_media.status = 4; }
            }
            if (b == 2 && g_media.canNext) g_mw.Command(CMD_NEXT);
        }
        g_dirty = true;
        return;
    }
    if (g_shelfPress != -1) {
        int i = g_shelfPress;
        g_shelfPress = -1;
        ReleaseCapture();
        const Hit* h = HitAt(x, y);
        if (i >= 0 && h && h->id == H_SHELFTILE && h->arg == i) ShelfOpen(i);
        return;
    }
    if (g_mode == M_EXPANDED && PanelClick(x, y)) return;
    if (g_mode == M_EXPANDED && g_L.hasWidgets) {
        if (g_L.timerOn) {
            if (InRect(g_L.tPause, x, y)) { ToggleTimerPause(); return; }
            if (InRect(g_L.tStop, x, y)) { StopTimer(); return; }
        } else {
            static const double secs[3] = {5 * 60, 15 * 60, 25 * 60};
            for (int i = 0; i < 3; i++)
                if (InRect(g_L.chip[i], x, y)) { StartTimer(secs[i], i == 2 ? 1 : 0); return; }
        }
        if (InRect(g_L.wxCard, x, y)) { g_wxs.Refresh(); return; }
        if (InRect(g_L.calCard, x, y)) { if (g_set.icsUrl.empty()) OpenSettings(); return; }
    }
    if (g_mode == M_EXPANDED) {
        int t = HitTool(x, y);
        if (t >= 0) { RunTool(kToolModes[t]); return; }
        bool onX = false;
        int si = HitShelf(x, y, &onX);
        if (si >= 0 && onX) { ShelfRemove(si); return; }
        if (ShelfShown() && InRect(g_L.shelfClear, x, y)) { ShelfClear(); return; }
        if (si >= 0) return;
    }
    if (g_mode == M_REC) { g_rec.Stop(); return; }
    if (g_mode == M_CALL) {
        if (InRect(g_L.mic, x, y)) ToggleMicMute();
        else ActivateApp(L"", g_callApp);
        return;
    }
    if (g_mode == M_NOTIF) { NotifClicked(); return; }
    if (g_mode == M_TOAST && g_toast.action != TA_NONE) { DoToastAction(); return; }
    if (g_mode == M_EXPANDED) SetMode(Collapsed());
    else SetMode(M_EXPANDED);
}

// ----------------------------------------------------------------------------
// Тосты системных событий
// ----------------------------------------------------------------------------
static bool Quiet();
static void Shake();
static void ShowToast(Toast t, UINT ms) {
    if (t.kind == TK_ERROR) Shake();
    if (Quiet() && (t.kind == TK_BRIGHT || t.kind == TK_BATTERY || t.kind == TK_AUDIO || t.kind == TK_MIC || t.kind == TK_CAM ||
                    t.kind == TK_CAPS || t.kind == TK_LAYOUT || t.kind == TK_DOWNLOAD))
        return;
    t.textW = MeasureW(g_fToast, t.text);
    t.rightW = MeasureW(g_fTime, t.right);
    bool sameKind = g_mode == M_TOAST && g_toast.kind == t.kind;
    if (g_mode == M_CLIP || g_mode == M_CMD) return;
    if (g_mode == M_EXPANDED && t.kind != TK_VOLUME) return;
    g_toast = t;
    if (g_mode == M_EXPANDED) { g_dirty = true; return; }
    if (sameKind) Retarget();
    else SetMode(M_TOAST);
    SetTimer(g_hwnd, T_TOAST, ms, nullptr);
}
static void ShowVolumeToast(float v, bool mute) {
    g_vol = v;
    g_muted = mute;
    g_volShowUntil = NowSec() + 1.6;  // в развёрнутом виде — в углу
    Toast t;
    t.kind = TK_VOLUME;
    t.value = v;
    t.flag = mute;
    ShowToast(t, 1500);
}
// dir: +n / -n шагов по 2%, 0 — вкл/выкл звук
static void ChangeVolume(float steps, bool toggleMute) {
    if (!g_audio.Ensure()) return;
    float v = 0;
    BOOL mute = FALSE;
    g_audio.vol->GetMasterVolumeLevelScalar(&v);
    g_audio.vol->GetMute(&mute);
    if (toggleMute) {
        mute = !mute;
        g_audio.vol->SetMute(mute, nullptr);
    } else {
        v = Clamp(std::round((v + 0.02f * steps) * 50.f) / 50.f, 0.f, 1.f);
        g_audio.vol->SetMasterVolumeLevelScalar(v, nullptr);
        if (mute && steps > 0) { g_audio.vol->SetMute(FALSE, nullptr); mute = FALSE; }
    }
    ShowVolumeToast(v, mute != FALSE);
}
static void OnWheel(int delta) {
    if (g_mode == M_CLIP) {
        g_clipScroll.target -= delta / (float)WHEEL_DELTA * kClipRowH * 1.5f;
        float listH = g_clipList.bottom - g_clipList.top;
        g_clipScroll.target = Clamp((float)g_clipScroll.target, 0.f, std::max(0.f, g_clipView.size() * kClipRowH - listH));
        g_dirty = true;
        return;
    }
    ChangeVolume((float)delta / WHEEL_DELTA, false);
}

static bool HasBattery() {
    SYSTEM_POWER_STATUS ps;
    return GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
}
static int BatteryPct() {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps) || ps.BatteryLifePercent > 100) return -1;
    return ps.BatteryLifePercent;
}
static D2D1_COLOR_F BatColor(int pct, bool charging) {
    if (charging) return Col(0.25f, 0.85f, 0.4f);
    if (pct >= 0 && pct <= 10) return Col(1.f, 0.3f, 0.28f);
    if (pct >= 0 && pct <= 20) return Col(1.f, 0.65f, 0.15f);
    return kWhite;
}
static void BatteryToast(const wchar_t* text, bool charging) {
    int pct = BatteryPct();
    Toast t;
    t.kind = TK_BATTERY;
    t.text = text;
    t.flag = charging;
    t.value = pct >= 0 ? pct / 100.f : 1.f;  // уровень для иконки батареи
    if (pct >= 0) t.right = std::to_wstring(pct) + L"%";
    t.color = BatColor(pct, charging);
    ShowToast(t, 2600);
}
static void OnPowerSetting(const POWERBROADCAST_SETTING* ps) {
    static const GUID kAcDc = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
    static const GUID kBatPct = {0xA7AD8041, 0xB45A, 0x4CAE, {0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1}};
    static const GUID kBright = {0x8FFEE2C6, 0x2D01, 0x46BE, {0xAD, 0xB9, 0x39, 0x8A, 0xDD, 0xC5, 0xB4, 0xFF}};
    if (!ps || ps->DataLength < sizeof(DWORD)) return;
    DWORD v = *(const DWORD*)ps->Data;
    int bit = IsEqualGUID(ps->PowerSetting, kAcDc) ? 1 : IsEqualGUID(ps->PowerSetting, kBatPct) ? 2 : IsEqualGUID(ps->PowerSetting, kBright) ? 4 : 0;
    if (!bit) return;
    bool first = !(g_pwrSeen & bit);
    g_pwrSeen |= bit;
    if (first || !g_set.sysToasts) return;  // первое уведомление — текущее состояние, не событие
    if (bit == 1 && HasBattery()) {
        if (v == 0) { g_batLowLevel = 100; BatteryToast(L"Зарядка", true); }
        else BatteryToast(L"От батареи", false);
    } else if (bit == 2 && HasBattery()) {
        SYSTEM_POWER_STATUS st;
        bool onAc = GetSystemPowerStatus(&st) && st.ACLineStatus == 1;
        if (onAc) { g_batLowLevel = 100; return; }
        int pct = (int)v;
        if (pct <= 10 && g_batLowLevel > 10) { g_batLowLevel = 10; BatteryToast(L"Батарея почти села", false); }
        else if (pct <= 20 && g_batLowLevel > 20) { g_batLowLevel = 20; BatteryToast(L"Низкий заряд", false); }
    } else if (bit == 4) {
        Toast t;
        t.kind = TK_BRIGHT;
        t.value = Clamp((int)v, 0, 100) / 100.f;
        ShowToast(t, 1500);
    }
}

static void CheckLayout(bool notify) {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return;
    HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(fg, nullptr));
    if (!hkl) return;
    if (notify && g_set.sysToasts && fg == g_layoutWnd && hkl != g_layoutHkl) {
        LCID lcid = MAKELCID(LOWORD((UINT_PTR)hkl), SORT_DEFAULT);
        wchar_t iso[16] = {}, name[64] = {};
        GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, iso, 16);
        GetLocaleInfoW(lcid, LOCALE_SNATIVELANGNAME, name, 64);
        Toast t;
        t.kind = TK_LAYOUT;
        t.badge = iso;
        if (!t.badge.empty()) CharUpperBuffW(&t.badge[0], (DWORD)t.badge.size());
        t.text = name;
        if (!t.text.empty()) CharUpperBuffW(&t.text[0], 1);
        ShowToast(t, 1300);
    }
    g_layoutWnd = fg;
    g_layoutHkl = hkl;
}

static void OnKeyEvent(UINT vk, bool up) {
    switch (vk) {
        case VK_VOLUME_UP: ChangeVolume(1, false); break;
        case VK_VOLUME_DOWN: ChangeVolume(-1, false); break;
        case VK_VOLUME_MUTE: ChangeVolume(0, true); break;
        case VK_MEDIA_PLAY_PAUSE:
        case VK_MEDIA_NEXT_TRACK:
        case VK_MEDIA_PREV_TRACK:
        case VK_MEDIA_STOP:
            // показываем трек, а состояние быстро перечитываем
            g_mw.Command(CMD_NONE);
            SetTimer(g_hwnd, T_MEDIAPOKE, 350, nullptr);
            if (g_set.sysToasts && g_media.has && (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_PEEK)) {
                SetMode(M_PEEK);
                SetTimer(g_hwnd, T_PEEK, 2200, nullptr);
            }
            break;
        case VK_CAPITAL:
            if (up && g_set.sysToasts) SetTimer(g_hwnd, T_CAPS, 40, nullptr);
            break;
        default:
            if (up) SetTimer(g_hwnd, T_LAYOUT, 160, nullptr);
    }
}

static void OnSysEvent(int kind, LPARAM lp) {
    if (kind == SE_VOLUME) {
        VolumeInfo* v = (VolumeInfo*)lp;
        if (g_set.sysToasts && (std::fabs(v->vol - g_vol) > 0.004f || v->mute != g_muted)) ShowVolumeToast(v->vol, v->mute);
        delete v;
    } else if (kind == SE_AUDIODEV) {
        AudioDevInfo* a = (AudioDevInfo*)lp;
        g_audio.Reset();  // пиковый метр — на новое устройство
        if (LooksLikePods(a->name)) PodsLookup(a->name, false);  // заряд AirPods — из BLE-рекламы
        if (g_set.sysToasts && !a->name.empty()) {
            Toast t;
            t.kind = TK_AUDIO;
            t.text = a->name;
            t.flag = a->bluetooth;
            t.color = a->bluetooth ? Col(0.35f, 0.62f, 1.f) : kWhite;
            if (a->battery >= 0) {
                t.right = std::to_wstring(a->battery) + L"%";
                t.color = a->battery <= 20 ? Col(1.f, 0.62f, 0.15f) : Col(0.25f, 0.85f, 0.4f);
            }
            ShowToast(t, 2600);
        }
        delete a;
    } else if (kind == SE_PRIVACY) {
        PrivacyInfo* p = (PrivacyInfo*)lp;
        bool micStarted = p->mic && !g_priv.mic, camStarted = p->cam && !g_priv.cam;
        g_priv = *p;
        delete p;
        UpdateCallState();
        if (g_inCall) micStarted = false;  // вместо тоста — пилюля звонка
        if (g_privInit && g_set.privacy) {
            if (camStarted) {
                Toast t;
                t.kind = TK_CAM;
                t.text = g_priv.camApp.empty() ? L"Камера включена" : L"Камера · " + g_priv.camApp;
                t.color = Col(0.20f, 0.84f, 0.36f);
                ShowToast(t, 2400);
            } else if (micStarted) {
                Toast t;
                t.kind = TK_MIC;
                t.text = g_priv.micApp.empty() ? L"Микрофон включён" : L"Микрофон · " + g_priv.micApp;
                t.color = Col(1.f, 0.62f, 0.12f);
                ShowToast(t, 2400);
            }
        }
        g_privInit = true;
        g_dirty = true;
    }
}


// ----------------------------------------------------------------------------
// Действие по клику на тост
// ----------------------------------------------------------------------------
static void SelectInExplorer(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    } else {
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
}
static void UpdateStartDownload();
static void DoToastAction() {
    int a = g_toast.action;
    std::wstring arg = g_toast.arg;
    g_toast.action = TA_NONE;
    KillTimer(g_hwnd, T_TOAST);
    SetMode(Collapsed());
    if (a == TA_OPEN || a == TA_URL) ShellExecuteW(nullptr, L"open", arg.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (a == TA_SELECT) SelectInExplorer(arg);
    else if (a == TA_UPDATE) UpdateStartDownload();
}
static void ShowActionToast(int kind, const std::wstring& text, const std::wstring& right, int action,
                            const std::wstring& arg, D2D1_COLOR_F color, UINT ms) {
    Toast t;
    t.kind = kind;
    t.text = text;
    t.right = right;
    t.action = action;
    t.arg = arg;
    t.color = color;
    if (g_mode == M_EXPANDED) SetMode(Collapsed());  // результат инструментов показываем всегда
    ShowToast(t, ms);
}

// ----------------------------------------------------------------------------
// Полка
// ----------------------------------------------------------------------------
static std::wstring ShelfFile() { return g_baseDir + L"\\data\\shelf.txt"; }
static void ShelfSave() {
    std::wstring all;
    for (auto& it : g_shelf) all += it.path + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(), &u[0], n, nullptr, nullptr);
    HANDLE f = CreateFileW(ShelfFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr;
    WriteFile(f, u.data(), (DWORD)u.size(), &wr, nullptr);
    CloseHandle(f);
}
static bool LoadShellIcon(const std::wstring& path, std::vector<BYTE>& px, int& w, int& h) {
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, (void**)&it)) || !it) return false;
    bool ok = false;
    IShellItemImageFactory* f = nullptr;
    if (SUCCEEDED(it->QueryInterface(IID_IShellItemImageFactory, (void**)&f)) && f) {
        SIZE sz = {64, 64};
        HBITMAP hb = nullptr;
        if (SUCCEEDED(f->GetImage(sz, SIIGBF_BIGGERSIZEOK, &hb)) && hb) {
            BITMAP bm = {};
            GetObjectW(hb, sizeof(bm), &bm);
            w = bm.bmWidth;
            h = bm.bmHeight;
            if (w > 0 && h > 0) {
                BITMAPINFO bi = {};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                px.resize((size_t)w * h * 4);
                HDC dc = GetDC(nullptr);
                ok = GetDIBits(dc, hb, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h;
                ReleaseDC(nullptr, dc);
                bool anyA = false;
                for (size_t i = 3; i < px.size(); i += 4) if (px[i]) { anyA = true; break; }
                if (!anyA) for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            }
            DeleteObject(hb);
        }
        f->Release();
    }
    it->Release();
    return ok;
}
static void ShelfAdd(const std::wstring& path) {
    for (auto& it : g_shelf) if (_wcsicmp(it.path.c_str(), path.c_str()) == 0) return;
    ShelfItem it;
    it.path = path;
    size_t sl = path.find_last_of(L"\\/");
    it.name = sl == std::wstring::npos ? path : path.substr(sl + 1);
    if (it.name.empty()) it.name = path;
    LoadShellIcon(path, it.px, it.iw, it.ih);
    g_shelf.insert(g_shelf.begin(), std::move(it));
}
static void ShelfLoad() {
    HANDLE f = CreateFileW(ShelfFile().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    std::string u;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart < 4 * 1024 * 1024) {
        u.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (!u.empty()) ReadFile(f, &u[0], (DWORD)u.size(), &rd, nullptr);
    }
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring all(n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), &all[0], n);
    std::vector<std::wstring> paths;
    size_t a = 0;
    while (a < all.size()) {
        size_t e = all.find(L'\n', a);
        std::wstring p = all.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
        if (!p.empty() && p.back() == L'\r') p.pop_back();
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) paths.push_back(p);
        if (e == std::wstring::npos) break;
        a = e + 1;
    }
    for (auto it = paths.rbegin(); it != paths.rend(); ++it) ShelfAdd(*it);
}
static void ShelfChanged() {
    ShelfSave();
    if (g_mode == M_EXPANDED) Retarget();
    g_hoverShelf = -1;
    g_dirty = true;
}
static void ShelfRemove(int i) {
    if (i < 0 || i >= (int)g_shelf.size()) return;
    g_shelf[i].icon.Release();
    g_shelf.erase(g_shelf.begin() + i);
    ShelfChanged();
}
static void ShelfClear() {
    for (auto& it : g_shelf) it.icon.Release();
    g_shelf.clear();
    ShelfChanged();
}
static void ShelfOpen(int i) {
    if (i < 0 || i >= (int)g_shelf.size()) return;
    ShellExecuteW(nullptr, L"open", g_shelf[i].path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
static void ShelfPrune() {
    bool changed = false;
    for (size_t i = 0; i < g_shelf.size();) {
        if (GetFileAttributesW(g_shelf[i].path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            g_shelf[i].icon.Release();
            g_shelf.erase(g_shelf.begin() + i);
            changed = true;
        } else i++;
    }
    if (changed) ShelfChanged();
}
// Утащить файл с полки в любое окно (проводник, Telegram, браузер…)
static void ShelfDragAll();
static void ShelfDragOut(int i) {
    if (i == -2) { ShelfDragAll(); return; }
    if (i < 0 || i >= (int)g_shelf.size()) return;
    static const GUID kBHID_DataObject = {0xB8C0BD9F, 0xED24, 0x455C, {0x83, 0xE6, 0xD5, 0x39, 0x0C, 0x4F, 0xE8, 0xC4}};
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(g_shelf[i].path.c_str(), nullptr, IID_IShellItem, (void**)&it)) || !it) {
        ShelfPrune();
        return;
    }
    IDataObject* dobj = nullptr;
    if (SUCCEEDED(it->BindToHandler(nullptr, kBHID_DataObject, IID_IDataObject, (void**)&dobj)) && dobj) {
        DWORD eff = 0;
        SHDoDragDrop(g_hwnd, dobj, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
        dobj->Release();
    }
    it->Release();
    SetTimer(g_hwnd, T_SHELFCHECK, 1500, nullptr);  // если файл перенесли — уберём его с полки
}

// Приём файлов, брошенных на остров
class DropTarget : public IDropTarget {
    LONG ref_ = 1;
    DWORD eff_ = DROPEFFECT_NONE;
    static DWORD Pick(DWORD allowed) {
        if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
        if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        return DROPEFFECT_NONE;
    }
public:
    virtual ~DropTarget() {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        bool ok = g_set.shelf && d && d->QueryGetData(&fe) == S_OK && g_mode != M_CLIP;
        eff_ = ok ? Pick(*eff) : DROPEFFECT_NONE;
        *eff = eff_;
        if (ok) {
            g_dropActive = true;
            g_preDropMode = g_mode;
            g_dropCount = 1;
            KillTimer(g_hwnd, T_COLLAPSE);
            SetMode(M_DROP);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* eff) override { *eff = eff_; return S_OK; }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (g_dropActive) { g_dropActive = false; SetMode(g_preDropMode == M_EXPANDED ? M_EXPANDED : Collapsed()); }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL, DWORD* eff) override {
        *eff = eff_;
        int added = 0;
        FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM m = {};
        if (eff_ != DROPEFFECT_NONE && d && SUCCEEDED(d->GetData(&fe, &m))) {
            HDROP hd = (HDROP)GlobalLock(m.hGlobal);
            UINT n = hd ? DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0) : 0;
            for (UINT i = 0; i < n && i < 64; i++) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(hd, i, path, MAX_PATH * 2)) { ShelfAdd(path); added++; }
            }
            if (hd) GlobalUnlock(m.hGlobal);
            ReleaseStgMedium(&m);
        }
        g_dropActive = false;
        if (added) {
            ShelfSave();
            Toast t;
            t.kind = TK_SHELF;
            t.text = added == 1 ? L"На полке: " + g_shelf[0].name : L"На полку добавлено: " + std::to_wstring(added);
            SetMode(Collapsed());
            ShowToast(t, 1800);
        } else {
            SetMode(Collapsed());
        }
        return S_OK;
    }
};
static DropTarget* g_dropTarget;

// ----------------------------------------------------------------------------
// Инструменты экрана
// ----------------------------------------------------------------------------
struct WorkResult { int mode; bool ok; std::wstring text, err; };
static void RecBegin(RECT rc, HWND win);  // rec.inc
static void RecScanSoon();

static std::wstring StampName(const wchar_t* prefix, const wchar_t* ext) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[128];
    swprintf(b, 128, L"%ls %04d-%02d-%02d %02d-%02d-%02d%ls", prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, ext);
    return b;
}
static void RestoreAfterTool() {
    if (!g_toolHidden) return;
    g_toolHidden = false;
    if (g_visible) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    g_dirty = true;
}
static void RunTool(int mode) {
    if (!g_set.tools || ToolActive() || g_toolHidden) return;
    if (mode == TM_REC && g_rec.Active()) { g_rec.Stop(); return; }
    if (g_clipOpen) CloseClip(false);
    KillTimer(g_hwnd, T_TOAST);
    SetMode(Collapsed());
    // прячем остров, чтобы он не попал на снимок
    g_toolHidden = true;
    ShowWindow(g_hwnd, SW_HIDE);
    g_toolPending = mode;
    SetTimer(g_hwnd, T_TOOLGO, 180, nullptr);
}
static void OnToolResult(ToolResult* r) {
    RestoreAfterTool();
    if (r->cancel) { delete r; return; }
    const D2D1_COLOR_F white = kWhite;
    switch (r->mode) {
        case TM_SHOT_AREA:
        case TM_SHOT_FULL: {
            if (r->mode == TM_SHOT_AREA && g_set.markup && OpenMarkupFor(r)) break;  // сначала разметка
            std::wstring dir = g_baseDir + L"\\screenshots";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring path = dir + L"\\" + StampName(L"Скриншот", L".png");
            bool saved = SavePng(path, r->px, r->w, r->h);
            CopyImageToClipboard(g_hwnd, r->px, r->w, r->h);
            ShowActionToast(TK_SHOT, L"Скриншот скопирован", saved ? L"Открыть" : L"", saved ? TA_OPEN : TA_NONE, path, white, 4000);
            break;
        }
        case TM_OCR:
        case TM_QR: {
            HWND hw = g_hwnd;
            int mode = r->mode;
            auto px = std::make_shared<std::vector<BYTE>>(std::move(r->px));
            int w = r->w, h = r->h;
            std::thread([hw, mode, px, w, h] {
                WorkResult* res = new WorkResult{mode, false, L"", L""};
                if (mode == TM_OCR) res->ok = RunOcr(*px, w, h, res->text, res->err);
                else res->ok = DecodeQr(*px, w, h, res->text);
                if (!PostMessageW(hw, WM_APP_WORK, 0, (LPARAM)res)) delete res;
            }).detach();
            if (mode == TM_OCR) {
                Toast t;
                t.kind = TK_TEXT;
                t.text = L"Распознаю текст…";
                ShowToast(t, 4000);
            }
            break;
        }
        case TM_PICK: {
            wchar_t hex[16], rgb[32];
            swprintf(hex, 16, L"#%02X%02X%02X", GetRValue(r->color), GetGValue(r->color), GetBValue(r->color));
            swprintf(rgb, 32, L"rgb(%d, %d, %d)", GetRValue(r->color), GetGValue(r->color), GetBValue(r->color));
            CopyTextToClipboard(g_hwnd, hex);
            ShowActionToast(TK_COLOR, std::wstring(hex) + L" скопирован", rgb, TA_NONE, L"",
                            Col(GetRValue(r->color) / 255.f, GetGValue(r->color) / 255.f, GetBValue(r->color) / 255.f), 3000);
            break;
        }
        case TM_REC: RecBegin(r->rc); break;
    }
    delete r;
}
static void OnWorkResult(WorkResult* w) {
    if (w->mode == TM_OCR) {
        if (w->ok) {
            CopyTextToClipboard(g_hwnd, w->text);
            ShowActionToast(TK_TEXT, L"Текст скопирован", std::to_wstring(w->text.size()) + L" симв.", TA_NONE, L"", kWhite, 2600);
        } else {
            ShowActionToast(TK_ERROR, w->err.empty() ? L"Текст не найден" : w->err, L"", TA_NONE, L"", kWhite, 3500);
        }
    } else {
        if (w->ok) {
            CopyTextToClipboard(g_hwnd, w->text);
            bool link = IsLink(w->text);
            std::wstring shown = w->text;
            for (auto& c : shown) if (c == L'\r' || c == L'\n') c = L' ';
            ShowActionToast(TK_QR, shown, link ? L"Открыть" : L"Скопировано", link ? TA_URL : TA_NONE, w->text, kWhite, 5000);
        } else {
            ShowActionToast(TK_ERROR, L"QR-код не найден", L"", TA_NONE, L"", kWhite, 2500);
        }
    }
    delete w;
}
static void OnRecDone(bool ok, std::wstring* msg) {
    if (!ok) LogLine(L"Запись экрана: %ls", msg->c_str());
    UpdateAffinity();
    KillTimer(g_hwnd, T_REC);
    if (g_mode == M_REC) SetMode(Collapsed());
    RecScanSoon();
    if (ok) ShowActionToast(TK_REC, g_rec.audioNote.empty() ? L"Запись сохранена" : L"Сохранено · " + g_rec.audioNote, L"Показать", TA_SELECT,
                            *msg, kWhite, 5000);
    else ShowActionToast(TK_ERROR, L"Запись: " + *msg, L"", TA_NONE, L"", kWhite, 4000);
    delete msg;
}


// ----------------------------------------------------------------------------
// Таймер и помодоро
// ----------------------------------------------------------------------------
static void StartTimer(double seconds, int kind) {
    g_tm.on = true;
    g_tm.paused = false;
    g_tm.kind = kind;
    g_tm.total = seconds;
    g_tm.endAt = NowSec() + seconds;
    if (kind == 0) g_tm.focusDone = 0;
    SetTimer(g_hwnd, T_TIMER, 500, nullptr);
    if (g_mode != M_EXPANDED) SetMode(Collapsed());
    g_dirty = true;
}
static void StopTimer() {
    g_tm = TimerState{};
    KillTimer(g_hwnd, T_TIMER);
    if (g_mode == M_TIMER) SetMode(Collapsed());
    g_dirty = true;
}
static void ToggleTimerPause() {
    if (!g_tm.on) return;
    if (g_tm.paused) { g_tm.endAt = NowSec() + g_tm.left; g_tm.paused = false; }
    else { g_tm.left = TimerLeft(); g_tm.paused = true; }
    g_dirty = true;
}
static void TimerTick() {
    if (!g_tm.on) { KillTimer(g_hwnd, T_TIMER); return; }
    if (g_mode == M_TIMER || g_mode == M_EXPANDED) g_dirty = true;
    if (g_tm.paused || TimerLeft() > 0) return;
    MessageBeep(MB_ICONASTERISK);
    Toast t;
    t.kind = TK_SHELF;
    t.color = TimerColor();
    if (g_tm.kind == 0) {
        g_tm = TimerState{};
        KillTimer(g_hwnd, T_TIMER);
        t.text = L"Таймер: время вышло";
        if (g_mode == M_TIMER) SetMode(Collapsed());
        ShowToast(t, 6000);
        Confetti();
        PlayUi(SN_DONE);
    } else if (g_tm.kind == 1) {
        int done = g_tm.focusDone + 1;
        bool longBreak = done % 4 == 0;
        StartTimer(longBreak ? 15 * 60 : 5 * 60, 2);
        g_tm.focusDone = done;
        t.text = longBreak ? L"Отлично! Длинный перерыв 15 мин" : L"Фокус закончен — перерыв 5 мин";
        ShowToast(t, 5000);
        Confetti(longBreak ? 110 : 60);
        PlayUi(SN_DONE);
    } else {
        int done = g_tm.focusDone;
        StartTimer(25 * 60, 1);
        g_tm.focusDone = done;
        t.text = L"Перерыв окончен — снова фокус";
        ShowToast(t, 5000);
    }
}

// ----------------------------------------------------------------------------
// Звонки: Discord/Telegram/… взяли микрофон → пилюля звонка с кнопкой mute
// ----------------------------------------------------------------------------
static bool IsCallApp(const std::wstring& app) {
    std::wstring l = LowerW(app);
    static const wchar_t* apps[] = {L"discord", L"telegram", L"skype", L"zoom", L"teams", L"whatsapp", L"viber", L"slack", L"element", L"signal"};
    for (auto* a : apps) if (l.find(a) != std::wstring::npos) return true;
    return false;
}
static IAudioEndpointVolume* MicVolume() {
    IMMDeviceEnumerator* en = nullptr;
    IAudioEndpointVolume* v = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_MMDevEnum, nullptr, CLSCTX_ALL, IID_MMDevEnum, (void**)&en)) && en) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eCommunications, &d)) && d) {
            d->Activate(IID_EndpointVol, CLSCTX_ALL, nullptr, (void**)&v);
            d->Release();
        }
        en->Release();
    }
    return v;
}
static void RefreshMicMute() {
    if (IAudioEndpointVolume* v = MicVolume()) {
        BOOL m = FALSE;
        v->GetMute(&m);
        g_micMuted = m != FALSE;
        v->Release();
    }
}
static void ToggleMicMute() {
    if (IAudioEndpointVolume* v = MicVolume()) {
        BOOL m = FALSE;
        v->GetMute(&m);
        v->SetMute(!m, nullptr);
        g_micMuted = !m;
        v->Release();
    }
    g_dirty = true;
}
static void UpdateCallState() {
    bool call = g_priv.mic && IsCallApp(g_priv.micApp);
    if (call && !g_inCall) {
        g_inCall = true;
        g_callApp = g_priv.micApp;
        g_callStart = NowSec();
        RefreshMicMute();
        SetTimer(g_hwnd, T_CALL, 1000, nullptr);
    } else if (!call && g_inCall) {
        g_inCall = false;
        KillTimer(g_hwnd, T_CALL);
    } else return;
    if (g_mode != M_EXPANDED && g_mode != M_CLIP && g_mode != M_NOTIF) SetMode(Collapsed());
}
// Вывести окно приложения на передний план: по AUMID (shell:AppsFolder) или по имени процесса
struct FindWnd { std::wstring exe; HWND found; };
static BOOL CALLBACK FindWndProc(HWND w, LPARAM lp) {
    FindWnd* f = (FindWnd*)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;
    wchar_t path[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    bool match = false;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) match = LowerW(path).find(f->exe) != std::wstring::npos;
    CloseHandle(p);
    if (match && GetWindowTextLengthW(w) > 0) { f->found = w; return FALSE; }
    return TRUE;
}
static void ActivateApp(const std::wstring& aumid, const std::wstring& exeName) {
    if (!exeName.empty()) {
        FindWnd f = {LowerW(exeName), nullptr};
        EnumWindows(FindWndProc, (LPARAM)&f);
        if (f.found) {
            if (IsIconic(f.found)) ShowWindow(f.found, SW_RESTORE);
            SetForegroundWindow(f.found);
            return;
        }
    }
    if (!aumid.empty()) ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ----------------------------------------------------------------------------
// Уведомления Windows
// ----------------------------------------------------------------------------
static void ShowNextNotif() {
    if (g_notif) { delete g_notif; g_notif = nullptr; }
    g_notifPic.Release();
    if (g_notifQ.empty()) {
        KillTimer(g_hwnd, T_NOTIF);
        if (g_mode == M_NOTIF) SetMode(Collapsed());
        return;
    }
    g_notif = g_notifQ.front();
    g_notifQ.erase(g_notifQ.begin());
    MakePic(g_notifPic, g_notif->icon, g_notif->iw, g_notif->ih);
    if (g_mode == M_NOTIF) { g_modeT = NowSec(); g_content = 0; Retarget(); }  // перелистывание
    else SetMode(M_NOTIF);
    SetTimer(g_hwnd, T_NOTIF, g_notif->call ? 12000 : 5000, nullptr);
}
static bool Quiet();
static void OnNotif(NotifData* d) {
    if (!d->error.empty() || !g_set.notifs) { delete d; return; }
    if (Quiet() && !d->call) { delete d; return; }  // «не беспокоить»
    // не перебиваем: буфер, инструменты, развёрнутый остров — уведомление подождёт
    if (g_notifQ.size() > 6) { delete g_notifQ.front(); g_notifQ.erase(g_notifQ.begin()); }
    g_notifQ.push_back(d);
    if (!g_notif && g_mode != M_CLIP && g_mode != M_CMD && g_mode != M_EXPANDED && g_mode != M_DROP && !g_toolHidden) ShowNextNotif();
}
static void NotifClicked() {
    if (g_notif) {
        std::wstring aumid = g_notif->aumid, app = g_notif->app;
        // у обычных (не магазинных) приложений AUMID часто совпадает с exe — пробуем найти окно
        std::wstring exe = aumid.find(L'!') == std::wstring::npos && aumid.find(L'_') == std::wstring::npos ? app : L"";
        ActivateApp(aumid, exe);
    }
    ShowNextNotif();
}

// ----------------------------------------------------------------------------
// Календарь: напоминания
// ----------------------------------------------------------------------------
static void CheckCalendarReminders() {
    if (!g_set.calRemind || !g_cal.ok) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    long long now = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    const long long MIN = 60LL * 10000000LL;
    for (auto& e : g_cal.events) {
        if (e.allDay) continue;
        long long until = e.start - now;
        const wchar_t* when = nullptr;
        if (until > 9 * MIN && until <= 10 * MIN + MIN / 2) when = L"Через 10 мин";
        else if (until > -MIN && until <= MIN / 2) when = L"Сейчас";
        if (!when) continue;
        std::wstring key = std::to_wstring(e.start) + when + e.title;
        if (std::find(g_calNotified.begin(), g_calNotified.end(), key) != g_calNotified.end()) continue;
        g_calNotified.push_back(key);
        if (g_calNotified.size() > 200) g_calNotified.erase(g_calNotified.begin());
        Toast t;
        t.kind = TK_SHELF;
        t.color = g_accent;
        t.text = std::wstring(when) + L" · " + e.title;
        if (!e.location.empty()) t.right = e.location.size() > 24 ? e.location.substr(0, 24) + L"…" : e.location;
        if (e.location.rfind(L"http", 0) == 0) { t.action = TA_URL; t.arg = e.location; t.right = L"Открыть"; }
        MessageBeep(MB_ICONASTERISK);
        ShowToast(t, 8000);
        break;
    }
}

// ----------------------------------------------------------------------------
// Загрузки
// ----------------------------------------------------------------------------
static void OnDownload(int kind, DlInfo* d) {
    if (!g_set.downloads) { delete d; return; }
    if (kind == DL_PROGRESS) {
        bool started = g_dl.active == 0 && d->active > 0;
        bool ended = g_dl.active > 0 && d->active == 0;
        double prevSpeed = started ? 0.0 : g_dl.speed;
        g_dl = *d;
        // скорость сглаживаем: замеры дёргаются (браузер пишет на диск рывками)
        g_dl.speed = d->speed < 0 ? prevSpeed : (prevSpeed > 0 ? prevSpeed * 0.7 + d->speed * 0.3 : d->speed);
        if ((started || ended) && (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_DL))
            SetMode(Collapsed());
        else if (g_mode == M_DL) g_dirty = true;
    } else if (kind == DL_DONE) {
        g_dl.active = d->active;
        if (g_dl.active == 0 && g_mode == M_DL) SetMode(Collapsed());
        ShowActionToast(TK_DOWNLOAD, L"Загружено · " + d->name, L"Открыть папку", TA_SELECT, d->path,
                        Col(0.3f, 0.85f, 0.45f), 6000);
    }
    delete d;
}

// ----------------------------------------------------------------------------
// Буфер обмена: захват, панель, вставка
// ----------------------------------------------------------------------------
static bool IsLink(const std::wstring& t) {
    size_t a = 0, b = t.size();
    while (a < b && iswspace(t[a])) a++;
    while (b > a && iswspace(t[b - 1])) b--;
    std::wstring s = LowerW(t.substr(a, b - a));
    if (s.empty() || s.size() > 2048) return false;
    for (wchar_t c : s) if (iswspace(c)) return false;
    return s.rfind(L"http://", 0) == 0 || s.rfind(L"https://", 0) == 0 || s.rfind(L"www.", 0) == 0;
}
// CF_DIB -> 32bpp BGRA сверху вниз
static bool DibToBgra(const BITMAPINFOHEADER* bih, size_t total, std::vector<BYTE>& out, int& w, int& h) {
    if (total < sizeof(BITMAPINFOHEADER) || bih->biSize < sizeof(BITMAPINFOHEADER)) return false;
    w = bih->biWidth;
    h = std::abs(bih->biHeight);
    if (w <= 0 || h <= 0 || (long long)w * h > 40000000LL) return false;
    bool bottomUp = bih->biHeight > 0;
    size_t hdr = bih->biSize;
    if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) hdr += 12;
    int colors = bih->biClrUsed ? (int)bih->biClrUsed : (bih->biBitCount <= 8 ? 1 << bih->biBitCount : 0);
    hdr += colors * 4;
    const BYTE* bits = (const BYTE*)bih + hdr;
    out.resize((size_t)w * h * 4);
    if (bih->biBitCount == 32 && (bih->biCompression == BI_RGB || bih->biCompression == BI_BITFIELDS)) {
        size_t stride = (size_t)w * 4;
        if (hdr + stride * h > total) return false;
        for (int y = 0; y < h; y++) {
            const BYTE* src = bits + stride * (bottomUp ? h - 1 - y : y);
            memcpy(&out[stride * y], src, stride);
        }
        bool anyA = false;
        for (size_t i = 3; i < out.size(); i += 4) if (out[i]) { anyA = true; break; }
        if (!anyA) for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
        return true;
    }
    // остальные форматы — через GDI
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    HBITMAP hb = CreateDIBitmap(dc, bih, CBM_INIT, bits, (const BITMAPINFO*)bih, DIB_RGB_COLORS);
    bool ok = hb && GetDIBits(dc, hb, 0, h, out.data(), &bi, DIB_RGB_COLORS) == h;
    if (hb) DeleteObject(hb);
    ReleaseDC(nullptr, dc);
    if (ok) for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return ok;
}
static void ReadClipboard() {
    if (!g_set.clip || !g_clipStoreOk) return;
    DWORD seq = GetClipboardSequenceNumber();
    if (seq == g_ownClipSeq || seq == g_lastClipSeq) return;
    static UINT fExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static UINT fIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    static UINT fCanInclude = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    if (IsClipboardFormatAvailable(fExclude) || IsClipboardFormatAvailable(fIgnore)) { g_lastClipSeq = seq; return; }
    bool open = false;
    for (int i = 0; i < 6 && !(open = OpenClipboard(g_hwnd)); i++) Sleep(12);
    if (!open) { SetTimer(g_hwnd, T_CLIPREAD, 250, nullptr); return; }  // кто-то держит буфер — попробуем позже
    g_lastClipSeq = seq;
    bool skip = false;
    if (IsClipboardFormatAvailable(fCanInclude)) {  // менеджеры паролей просят не сохранять
        HANDLE h = GetClipboardData(fCanInclude);
        if (DWORD* p = h ? (DWORD*)GlobalLock(h) : nullptr) { skip = *p == 0; GlobalUnlock(h); }
    }
    if (!skip) {
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
            UINT n = hd ? DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0) : 0;
            std::wstring all;
            for (UINT i = 0; i < n && i < 500; i++) {
                wchar_t path[MAX_PATH * 2];
                if (DragQueryFileW(hd, i, path, MAX_PATH * 2)) { if (!all.empty()) all += L"\n"; all += path; }
            }
            if (!all.empty()) g_clip.AddFiles(all);
        } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (const wchar_t* p = h ? (const wchar_t*)GlobalLock(h) : nullptr) {
                size_t maxN = GlobalSize(h) / sizeof(wchar_t);
                size_t n = wcsnlen(p, std::min<size_t>(maxN, 2000000));
                std::wstring text(p, n);
                GlobalUnlock(h);
                if (text.find_first_not_of(L" \t\r\n") != std::wstring::npos) g_clip.AddText(text, IsLink(text) ? CK_LINK : CK_TEXT);
            }
        } else if (IsClipboardFormatAvailable(CF_DIB)) {
            HANDLE h = GetClipboardData(CF_DIB);
            if (const BITMAPINFOHEADER* bih = h ? (const BITMAPINFOHEADER*)GlobalLock(h) : nullptr) {
                std::vector<BYTE> px;
                int w = 0, hh = 0;
                bool ok = DibToBgra(bih, GlobalSize(h), px, w, hh);
                GlobalUnlock(h);
                if (ok) g_clip.AddImage(std::move(px), w, hh);
            }
        }
    }
    CloseClipboard();
}

static HGLOBAL GlobalFrom(const void* data, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return nullptr;
    void* p = GlobalLock(g);
    memcpy(p, data, n);
    GlobalUnlock(g);
    return g;
}
static bool PutToClipboard(const ClipItem& it) {
    std::wstring text;
    std::vector<BYTE> px;
    int w = 0, h = 0;
    if (it.kind == CK_IMAGE) { if (!g_clip.GetImage(it.id, px, w, h)) return false; }
    else if (!g_clip.GetText(it.id, text)) return false;

    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(g_hwnd)); i++) Sleep(15);
    if (!open) return false;
    EmptyClipboard();
    if (it.kind == CK_IMAGE) {
        // CF_DIB (снизу вверх) — понимают все приложения
        BITMAPINFOHEADER bih = {};
        bih.biSize = sizeof(bih);
        bih.biWidth = w;
        bih.biHeight = h;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        size_t stride = (size_t)w * 4;
        std::vector<BYTE> dib(sizeof(bih) + stride * h);
        memcpy(dib.data(), &bih, sizeof(bih));
        for (int y = 0; y < h; y++) memcpy(&dib[sizeof(bih) + stride * (h - 1 - y)], &px[stride * y], stride);
        if (HGLOBAL g = GlobalFrom(dib.data(), dib.size())) SetClipboardData(CF_DIB, g);
    } else if (it.kind == CK_FILES) {
        std::wstring list;
        size_t a = 0;
        while (a <= text.size()) {
            size_t e = text.find(L'\n', a);
            std::wstring one = text.substr(a, e == std::wstring::npos ? std::wstring::npos : e - a);
            if (!one.empty()) { list += one; list.push_back(L'\0'); }
            if (e == std::wstring::npos) break;
            a = e + 1;
        }
        list.push_back(L'\0');
        std::vector<BYTE> buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
        DROPFILES* df = (DROPFILES*)buf.data();
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        if (HGLOBAL g = GlobalFrom(buf.data(), buf.size())) SetClipboardData(CF_HDROP, g);
        if (HGLOBAL g = GlobalFrom(text.c_str(), (text.size() + 1) * sizeof(wchar_t))) SetClipboardData(CF_UNICODETEXT, g);
    } else {
        if (HGLOBAL g = GlobalFrom(text.c_str(), (text.size() + 1) * sizeof(wchar_t))) SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
    g_ownClipSeq = GetClipboardSequenceNumber();
    if (it.kind != CK_IMAGE) g_clip.AddText(text, it.kind);  // поднять запись наверх
    return true;
}
static void SendCtrlV() {
    INPUT in[4] = {};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].ki.wVk = 'V';
    in[2].ki.wVk = 'V';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

static void OpenClip() {
    if (!g_set.clip || !g_clipStoreOk) return;
    if (g_clipOpen) { SetForegroundWindow(g_hwnd); return; }
    g_prevFg = GetForegroundWindow();
    if (g_prevFg == g_hwnd) g_prevFg = nullptr;
    if (!g_visible) { g_visible = true; ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); }  // даже поверх полноэкранного
    g_clipOpen = true;
    g_clipQuery.clear();
    g_clipSel = 0;
    g_clipHover = g_clipHoverBtn = -1;
    g_clipScroll.Set(0);
    ClipRefilter();
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
    KillTimer(g_hwnd, T_TOAST);
    SetMode(M_CLIP);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    g_caretOn = true;
    SetTimer(g_hwnd, T_CARET, 530, nullptr);
}
static void CloseClip(bool restoreFocus) {
    if (!g_clipOpen) return;
    g_clipOpen = false;
    KillTimer(g_hwnd, T_CARET);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE) | WS_EX_NOACTIVATE);
    if (g_mode == M_CLIP) SetMode(Collapsed());
    if (restoreFocus && g_prevFg && IsWindow(g_prevFg)) SetForegroundWindow(g_prevFg);
}
static void ClipPaste(int viewIdx) {
    if (viewIdx < 0 || viewIdx >= (int)g_clipView.size()) return;
    ClipItem it = g_clipItems[g_clipView[viewIdx]];
    bool ok = PutToClipboard(it);
    CloseClip(true);
    if (ok && g_prevFg) {
        g_pasteTries = 0;
        SetTimer(g_hwnd, T_PASTE, 70, nullptr);
    }
}
static void ClipTogglePin(int viewIdx) {
    if (viewIdx < 0 || viewIdx >= (int)g_clipView.size()) return;
    int i = g_clipView[viewIdx];
    ClipItem it = g_clipItems[i];
    it.pinned = !it.pinned;
    g_clip.SetPinned(it.id, it.pinned);
    g_clipItems.erase(g_clipItems.begin() + i);
    // закреплённые — сверху, внутри групп по времени
    auto pos = std::find_if(g_clipItems.begin(), g_clipItems.end(), [&](const ClipItem& x) {
        return x.pinned == it.pinned ? x.created < it.created : !x.pinned;
    });
    g_clipItems.insert(pos, it);
    ClipRefilter();
}
static void ClipDelete(int viewIdx) {
    if (viewIdx < 0 || viewIdx >= (int)g_clipView.size()) return;
    int i = g_clipView[viewIdx];
    long long id = g_clipItems[i].id;
    g_clip.Delete(id);
    auto f = g_clipThumbs.find(id);
    if (f != g_clipThumbs.end()) { f->second.Release(); g_clipThumbs.erase(f); }
    g_clipItems.erase(g_clipItems.begin() + i);
    ClipRefilter();
}
static void OnClipUpdate(ClipUpdate* u) {
    if (u->reload) {
        for (auto& kv : g_clipThumbs) kv.second.Release();
        g_clipThumbs.clear();
        g_clipItems = g_clip.LoadAll();
    }
    for (long long id : u->removed) {
        g_clipItems.erase(std::remove_if(g_clipItems.begin(), g_clipItems.end(), [&](const ClipItem& x) { return x.id == id; }),
                          g_clipItems.end());
        auto f = g_clipThumbs.find(id);
        if (f != g_clipThumbs.end()) { f->second.Release(); g_clipThumbs.erase(f); }
    }
    if (u->hasItem) {
        const ClipItem& it = u->item;
        g_clipItems.erase(std::remove_if(g_clipItems.begin(), g_clipItems.end(), [&](const ClipItem& x) { return x.id == it.id; }),
                          g_clipItems.end());
        auto pos = std::find_if(g_clipItems.begin(), g_clipItems.end(), [&](const ClipItem& x) {
            return x.pinned == it.pinned ? x.created < it.created : !x.pinned;
        });
        g_clipItems.insert(pos, it);
    }
    delete u;
    ClipRefilter();
}
// Что под мышью в панели: строка (индекс в g_clipView) и кнопка (0 — закрепить, 1 — удалить)
static int ClipHit(int x, int y, int* btn) {
    if (btn) *btn = -1;
    float fx = x / g_scale, fy = y / g_scale;
    const D2D1_RECT_F& r = g_clipList;
    if (fx < r.left || fx > r.right || fy < r.top || fy > r.bottom) return -1;
    int v = (int)((fy - r.top + g_clipScroll.x) / kClipRowH);
    if (v < 0 || v >= (int)g_clipView.size()) return -1;
    if (btn) {
        float pcx = r.right - 46, dcx = r.right - 20;
        if (std::fabs(fx - pcx) < 13) *btn = 0;
        else if (std::fabs(fx - dcx) < 13) *btn = 1;
    }
    return v;
}
static void OnClipKey(UINT vk) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int n = (int)g_clipView.size();
    switch (vk) {
        case VK_ESCAPE: CloseClip(true); return;
        case VK_UP: if (g_clipSel > 0) g_clipSel--; ClipEnsureVisible(); break;
        case VK_DOWN: if (g_clipSel + 1 < n) g_clipSel++; ClipEnsureVisible(); break;
        case VK_PRIOR: g_clipSel = std::max(0, g_clipSel - 7); ClipEnsureVisible(); break;
        case VK_NEXT: g_clipSel = std::max(0, std::min(n - 1, g_clipSel + 7)); ClipEnsureVisible(); break;
        case VK_HOME: g_clipSel = 0; ClipEnsureVisible(); break;
        case VK_END: g_clipSel = std::max(0, n - 1); ClipEnsureVisible(); break;
        case VK_RETURN: ClipPaste(g_clipSel); return;
        case VK_DELETE: ClipDelete(g_clipSel); break;
        case VK_BACK:
            if (ctrl) g_clipQuery.clear();
            else if (!g_clipQuery.empty()) g_clipQuery.pop_back();
            g_clipSel = 0;
            g_clipScroll.target = 0;
            ClipRefilter();
            break;
        case 'P': if (ctrl) ClipTogglePin(g_clipSel); break;
    }
    g_caretOn = true;
    g_dirty = true;
}

#include "cmd.inc"
#include "apppick.inc"
#include "extras.inc"

// ---- какие источники показывать в плеере ----
static bool IsBrowserApp(const std::wstring& id) {
    std::wstring l = LowerW(id);
    static const wchar_t* br[] = {L"chrome", L"msedge", L"firefox", L"opera", L"brave", L"vivaldi", L"yandex", L"browser.exe", L"arc.exe",
                                  L"thorium", L"librewolf", L"waterfox", L"zen.exe", L"floorp", L"chromium"};
    if (l.find(L"music") != std::wstring::npos || l.find(L"музык") != std::wstring::npos) return false;  // «Яндекс Музыка» — не браузер
    for (auto b : br) if (l.find(b) != std::wstring::npos) return true;
    if (l == L"zen" || l.find(L"zen browser") != std::wstring::npos || l.find(L"zen-browser") != std::wstring::npos ||
        l.find(L"\\zen\\") != std::wstring::npos || l.find(L"firefox") != std::wstring::npos)
        return true;
    // Firefox и его форки (Zen, LibreWolf, Floorp) называют себя хэшем из 16 hex-символов
    if (l.size() == 16 && std::all_of(l.begin(), l.end(), [](wchar_t c) { return iswxdigit(c) != 0; })) return true;
    return false;
}
static void RememberSource(const std::wstring& id, const std::wstring& name) {
    if (id.empty() || g_set.mediaSeen.find(id + L"=") != std::wstring::npos) return;
    g_set.mediaSeen += id + L"=" + (name.empty() ? id : name) + L";";
    if (g_set.mediaSeen.size() > 3000) g_set.mediaSeen.erase(0, g_set.mediaSeen.find(L';') + 1);
    SaveSettings();
}
static bool SourceBlocked(const std::wstring& id) { return !id.empty() && g_set.mediaBlock.find(L"|" + id + L"|") != std::wstring::npos; }
static double g_pausedSince = 0;
static std::wstring g_pausedKey;
static void MediaFilterReset() { g_pausedSince = 0; }
// видео в браузере (YouTube и т.п.) — не музыка: у роликов нет альбома, а у YouTube Music, Яндекс Музыки, Spotify Web он есть
static bool LooksLikeVideo(const MediaSnap& s) {
    if (!(IsBrowserApp(s.appId) || IsBrowserApp(s.appName))) return false;
    if (!s.album.empty()) return false;  // у треков обычно есть альбом
    // главный признак: у музыки (Яндекс Музыка, Spotify, SoundCloud, YouTube Music) обложка квадратная,
    // у роликов YouTube/Twitch — кадр 16:9 (а у Shorts — 9:16)
    if (s.artSrcW > 0 && s.artSrcH > 0) {
        float a = (float)s.artSrcW / s.artSrcH;
        return a > 1.25f || a < 0.8f;
    }
    return false;  // обложки ещё нет — не прячем, решим, когда придёт
}
static void FilterMedia(MediaSnap& s) {
    if (!s.has) { g_pausedSince = 0; return; }
    RememberSource(s.appId, s.appName);
    bool hide = SourceBlocked(s.appId) || (g_set.browserMusicOnly && LooksLikeVideo(s));
    // Пауза: считаем от момента, когда музыка последний раз реально играла. Вкладка с Яндекс Музыкой,
    // где ничего не включали, сразу отдаёт «пауза» с обложкой — такую не показываем вовсе.
    static double lastPlaying = 0;
    static std::wstring lastPlayingApp;
    if (s.status == 4) {
        lastPlaying = NowSec();
        lastPlayingApp = s.appId;
        g_pausedSince = 0;
        KillTimer(g_hwnd, T_MEDIAHIDE);
    } else if (!hide && g_set.pauseHide > 0) {
        static const double lim[5] = {0, 4, 60, 300, 900};
        double now = NowSec();
        bool played = lastPlaying > 0 && lastPlayingApp == s.appId;
        double left = played ? lim[g_set.pauseHide] - (now - lastPlaying) : -1;
        g_pausedSince = played ? lastPlaying : now;
        if (left <= 0) hide = true;
        else SetTimer(g_hwnd, T_MEDIAHIDE, (UINT)(left * 1000) + 300, nullptr);
    }
    if (hide) s = MediaSnap{};
}

static void OnMedia() {
    MediaSnap s = g_mw.Get();
    FilterMedia(s);
    std::wstring key = s.title + L"\x1f" + s.artist;
    bool trackChanged = s.has && key != g_lastKey;
    bool hadMedia = g_media.has;
    bool visualChange = s.has != g_media.has || s.title != g_media.title || s.artist != g_media.artist ||
                        s.status != g_media.status || s.artVer != g_media.artVer || s.appVer != g_media.appVer ||
                        s.canNext != g_media.canNext || s.canPrev != g_media.canPrev;
    g_media = std::move(s);

    // сразу после перемотки приложение может ещё прислать старую позицию — держим свою
    if (NowSec() < g_seekHoldUntil && !trackChanged) {
        INT64 diff = (g_media.pos - g_seekPos) - (g_media.lastUpd - g_seekStamp);
        if (diff > 20000000LL || diff < -20000000LL) { g_media.pos = g_seekPos; g_media.lastUpd = g_seekStamp; }
    }

    if (g_media.artVer != g_artVer) {
        g_artVer = g_media.artVer;
        // старую обложку оставляем для перетекания
        g_artPrev.Release();
        g_artPrev = g_art;
        g_art = Pic{};
        MakePic(g_art, g_media.art, g_media.artW, g_media.artH);
        g_accentFrom = g_accent;
        g_accentTo = AccentFor(g_media.accent);
        g_artFadeT = NowSec();
    }
    if (g_media.appVer != g_appVer) {
        g_appVer = g_media.appVer;
        MakePic(g_appIcon, g_media.appIcon, g_media.appIconW, g_media.appIconH);
    }
    if (trackChanged) {
        g_lastKey = key;
        ResetLyrics();
        if (g_mode == M_LYRICS) SetMode(M_COMPACT);
        SetTimer(g_hwnd, T_LYR, 350, nullptr);  // даём приложению прислать длительность
        if (g_set.peek && !g_firstMedia && !Quiet() && (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_PEEK)) {
            SetMode(M_PEEK);
            SetTimer(g_hwnd, T_PEEK, 3200, nullptr);
        }
    }
    if (!g_media.has) {
        g_lastKey.clear();
        if (g_lyrState != LYR_NONE || !g_lyrReqKey.empty()) ResetLyrics();
    } else if (!trackChanged && g_set.lyrics && g_lyrState != LYR_FOUND && g_lyrReqDur == 0 && Duration() > 0) {
        RequestLyrics(true);  // длительность пришла позже — уточняем запрос
    }
    g_firstMedia = false;

    if (hadMedia != g_media.has) {
        if (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_PEEK || g_mode == M_LYRICS) SetMode(Collapsed());
        else Retarget();
    }
    if (visualChange || g_mode == M_EXPANDED) g_dirty = true;
}

// ----------------------------------------------------------------------------
// Окно настроек (обычные Win32-контролы, изменения применяются сразу)
// ----------------------------------------------------------------------------
enum { CID_MON = 200, CID_SIZE, CID_SIZE_LBL, CID_TOP, CID_TOP_LBL, CID_ANIM, CID_PEEK, CID_AUTO, CID_RESET, CID_DONE,
       CID_LYR, CID_LOFF, CID_LOFF_LBL, CID_VOLHOOK, CID_SYSTOAST, CID_PRIV, CID_CLIP, CID_CLIPLIM, CID_CLIPLIM_LBL,
       CID_CLIPCLEAR, CID_TOOLS, CID_DL, CID_SHELF, CID_CITY, CID_ICS, CID_WIDGETS, CID_CALREM, CID_NOTIFS, CID_NOTIFPRIV,
       CID_CALLS, CID_HOVEROPEN };
static HFONT g_uiFont;

// Хуки, хоткей и слушатель буфера — по текущим настройкам
static void ApplySystemHooks() {
    SetVolumeIntercept(g_set.volHook);
    if (g_set.volHook || g_set.sysToasts) InstallKeyHook(g_hwnd);
    else RemoveKeyHook();
    static bool hk = false, listen = false;
    bool wantClip = g_set.clip && g_clipStoreOk;
    if (wantClip && !hk) hk = RegisterHotKey(g_hwnd, HK_CLIP, MOD_CONTROL | MOD_ALT | 0x4000 /*MOD_NOREPEAT*/, 'V') != 0;
    if (!wantClip && hk) { UnregisterHotKey(g_hwnd, HK_CLIP); hk = false; }
    if (wantClip && !listen) listen = AddClipboardFormatListener(g_hwnd) != 0;
    if (!wantClip && listen) { RemoveClipboardFormatListener(g_hwnd); listen = false; }
    if (!wantClip && g_clipOpen) CloseClip(true);
    static bool tk = false;
    if (g_set.tools && !tk) {
        const UINT m = MOD_CONTROL | MOD_ALT | 0x4000;
        RegisterHotKey(g_hwnd, HK_SHOT, m, 'S');
        RegisterHotKey(g_hwnd, HK_OCR, m, 'T');
        RegisterHotKey(g_hwnd, HK_PICK, m, 'C');
        tk = true;
        RecRegisterHotkey();
    } else if (!g_set.tools && tk) {
        for (int id : {HK_SHOT, HK_OCR, HK_REC, HK_PICK}) UnregisterHotKey(g_hwnd, id);
        tk = false;
    }
    static bool ck = false;
    if (g_set.cmd && !ck) {
        if (RegisterHotKey(g_hwnd, HK_CMD, MOD_ALT | 0x4000, VK_SPACE)) g_cmdHotkeyText = L"Alt+Space";
        else if (RegisterHotKey(g_hwnd, HK_CMD, MOD_CONTROL | MOD_ALT | 0x4000, VK_SPACE)) g_cmdHotkeyText = L"Ctrl+Alt+Space";
        else g_cmdHotkeyText.clear();
        ck = true;
    } else if (!g_set.cmd && ck) {
        UnregisterHotKey(g_hwnd, HK_CMD);
        if (g_cmdOpen) CloseCmd(true);
        ck = false;
    }
    static bool dl = false;
    if (g_set.downloads && !dl) { g_dlw.Start(g_hwnd, DownloadsFolder()); dl = true; }
    else if (!g_set.downloads && dl) { g_dlw.Stop(); dl = false; g_dl = DlInfo{}; }
    static bool wx = false;
    if (g_set.widgets && !wx) { g_wxs.Start(g_hwnd, g_set.city); g_cals.Start(g_hwnd, g_set.icsUrl); wx = true; }
    else if (wx) { g_wxs.SetCity(g_set.city); g_cals.SetUrl(g_set.icsUrl); }
    static bool nt = false;
    if (g_set.notifs && !nt) { g_nw.Start(g_hwnd); nt = true; }
    else if (!g_set.notifs && nt) { g_nw.Stop(); nt = false; }
    if (!g_set.calls && g_mode == M_CALL) SetMode(Collapsed());
}

// ---- тема и акцент ----
static bool SystemLight() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v != 0;
}
static D2D1_COLOR_F SystemAccent() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &sz) !=
        ERROR_SUCCESS)
        return DefaultAccent();
    return Col((v & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, ((v >> 16) & 0xFF) / 255.f);  // ABGR
}
static D2D1_COLOR_F AccentFor(D2D1_COLOR_F cover) {
    if (g_sd.palette == 1) return Col(0.3f, 1.f, 0.42f);  // терминал
    if (g_sd.bg == BG_MATERIAL) return g_wallColor;       // Material You — из обоев
    if (g_set.accentMode == 1) return SystemAccent();
    if (g_set.accentMode == 2) return kSwatch[Clamp(g_set.accentIdx, 0, 7)];
    return cover;
}
static void ReloadFonts() {
    std::wstring want = StyleFont();
    if (want == g_fontName && g_fTime) return;
    g_fontName = want;
    IDWriteTextFormat** fmts[] = {&g_fTime, &g_fTitle, &g_fArtist, &g_fSmall, &g_fBig, &g_fDate, &g_fPeekTitle, &g_fPeekArtist, &g_fSource,
                                  &g_fLyric, &g_fLyricBig, &g_fLyricSmall, &g_fToast, &g_fClip, &g_fSearch, &g_fWBig, &g_fPlayerTitle,
                                  &g_fCmd, &g_fQa};
    for (auto f : fmts) SafeRelease(*f);
    CreateFormats();
    g_llCur.Release(); g_llPrev.Release(); g_llXCur.Release(); g_llXPrev.Release();
    Marquee* mqs[] = {&g_mqTitle, &g_mqArtist, &g_mqPeekTitle, &g_mqPeekArtist, &g_mqSource};
    for (auto* m : mqs) m->Release();
}
static void ApplyTheme() {
    g_sd = Def(Sty());
    if (g_sd.bg == BG_MATERIAL) ReadWallpaperColor();
    ReloadFonts();
    StyleTargets(false);
    bool light = (g_set.theme == 1 || (g_set.theme == 2 && SystemLight())) && !ForceDark();
    SetThemeColors(light);
    if (g_sd.palette == 1) {  // терминал: зелёным по чёрному
        kWhite = Col(0.36f, 1.f, 0.46f);
        kGray = Col(0.2f, 0.62f, 0.3f);
        kBgSolid = Col(0, 0, 0);
        kEdCol = Col(0.02f, 0.06f, 0.03f);
    }
    if (g_sd.bg == BG_MATERIAL) {
        kBgSolid = light ? Lerp(Col(1, 1, 1), g_wallColor, 0.16f) : Lerp(Col(0.04f, 0.04f, 0.05f), g_wallColor, 0.2f);
        kEdCol = light ? Col(1, 1, 1) : Lerp(Col(0.08f, 0.08f, 0.09f), g_wallColor, 0.15f);
    }
    ApplyEditorTheme();
    g_accentFrom = g_accent;
    g_accentTo = AccentFor(g_media.has ? g_media.accent : DefaultAccent());
    g_artFadeT = NowSec();
    g_dirty = true;
    if (g_hwnd) {
        Retarget();
        UpdateAffinity();
        if (FxBusy()) FxEnsureTimer();
    }
}
static bool Quiet() { return g_set.dnd || (g_set.dndCalls && CallActive()); }

static void ApplySettings() {
    SaveSettings();
    UpdateAffinity();
    if (FxBusy()) FxEnsureTimer();
    ApplySystemHooks();
    UpdateMetrics();
    if (g_set.lyrics) RequestLyrics(false);
    if (g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_DL || g_mode == M_IDLE || g_mode == M_CALL || g_mode == M_TIMER)
        SetMode(Collapsed());
    Retarget();
    g_dirty = true;
}
static void SettingsSyncLabels(HWND w) {
    wchar_t b[64];
    swprintf(b, 64, L"Размер острова: %d%%", g_set.sizePct);
    SetDlgItemTextW(w, CID_SIZE_LBL, b);
    if (g_set.topOffset == 0) swprintf(b, 64, L"Отступ сверху: 0 px — «чёлка» у края экрана");
    else swprintf(b, 64, L"Отступ сверху: %d px", g_set.topOffset);
    SetDlgItemTextW(w, CID_TOP_LBL, b);
    swprintf(b, 64, L"Сдвиг текста песен: %+.1f с", g_set.lyricOffsetMs / 1000.0);
    SetDlgItemTextW(w, CID_LOFF_LBL, b);
    swprintf(b, 64, L"Хранить записей в буфере: %d", g_set.clipLimit);
    SetDlgItemTextW(w, CID_CLIPLIM_LBL, b);
}
static void SettingsFill(HWND w) {
    HWND cb = GetDlgItem(w, CID_MON);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    g_monList = ListMonitors();
    SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Основной монитор (авто)");
    int sel = 0;
    for (size_t i = 0; i < g_monList.size(); i++) {
        const MonInfo& m = g_monList[i];
        wchar_t b[128];
        swprintf(b, 128, L"Монитор %d — %d×%d%ls", (int)i + 1, (int)(m.rc.right - m.rc.left),
                 (int)(m.rc.bottom - m.rc.top), m.primary ? L" (основной)" : L"");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)b);
        if (!g_set.monitor.empty() && m.device == g_set.monitor) sel = (int)i + 1;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
    SendDlgItemMessageW(w, CID_SIZE, TBM_SETPOS, TRUE, g_set.sizePct);
    SendDlgItemMessageW(w, CID_TOP, TBM_SETPOS, TRUE, g_set.topOffset);
    SendDlgItemMessageW(w, CID_LOFF, TBM_SETPOS, TRUE, g_set.lyricOffsetMs / 100);
    CheckDlgButton(w, CID_LYR, g_set.lyrics ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_VOLHOOK, g_set.volHook ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_SYSTOAST, g_set.sysToasts ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_PRIV, g_set.privacy ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_CLIP, g_set.clip ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_TOOLS, g_set.tools ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_DL, g_set.downloads ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_SHELF, g_set.shelf ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_WIDGETS, g_set.widgets ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_CALREM, g_set.calRemind ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_NOTIFS, g_set.notifs ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_NOTIFPRIV, g_set.notifPrivate ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_CALLS, g_set.calls ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(w, CID_CITY, g_set.city.c_str());
    SetDlgItemTextW(w, CID_ICS, g_set.icsUrl.c_str());
    SendDlgItemMessageW(w, CID_CLIPLIM, TBM_SETPOS, TRUE, g_set.clipLimit / 50);
    CheckDlgButton(w, CID_ANIM, g_set.audioAnim ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_PEEK, g_set.peek ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_HOVEROPEN, g_set.hoverOpen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(w, CID_AUTO, IsAutostart() ? BST_CHECKED : BST_UNCHECKED);
    SettingsSyncLabels(w);
}

static LRESULT CALLBACK SettingsProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            UINT dpi = GetDpiForWindow(w);
            float s = dpi / 96.f;
            auto S = [s](int v) { return (int)std::lround(v * s); };
            NONCLIENTMETRICSW ncm = {};
            ncm.cbSize = sizeof(ncm);
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
            g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
            auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int cw, int ch, int id) {
                HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(cw), S(ch), w,
                                         (HMENU)(INT_PTR)id, g_inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
                return c;
            };
            mk(L"STATIC", L"Где показывать остров", 0, 20, 18, 360, 20, -1);
            mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 20, 40, 360, 220, CID_MON);
            mk(L"STATIC", L"", 0, 20, 84, 360, 20, CID_SIZE_LBL);
            HWND t1 = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 14, 104, 372, 30, CID_SIZE);
            SendMessageW(t1, TBM_SETRANGE, TRUE, MAKELONG(80, 140));
            SendMessageW(t1, TBM_SETTICFREQ, 10, 0);
            SendMessageW(t1, TBM_SETPAGESIZE, 0, 10);
            mk(L"STATIC", L"", 0, 20, 144, 360, 20, CID_TOP_LBL);
            HWND t2 = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 14, 164, 372, 30, CID_TOP);
            SendMessageW(t2, TBM_SETRANGE, TRUE, MAKELONG(0, 40));
            SendMessageW(t2, TBM_SETTICFREQ, 5, 0);
            SendMessageW(t2, TBM_SETPAGESIZE, 0, 5);
            mk(L"STATIC", L"", 0, 20, 204, 360, 20, CID_LOFF_LBL);
            HWND t3 = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 14, 224, 372, 30, CID_LOFF);
            SendMessageW(t3, TBM_SETRANGEMIN, FALSE, -20);
            SendMessageW(t3, TBM_SETRANGEMAX, TRUE, 20);
            SendMessageW(t3, TBM_SETTICFREQ, 5, 0);
            SendMessageW(t3, TBM_SETPAGESIZE, 0, 5);
            const DWORD cb = BS_AUTOCHECKBOX | WS_TABSTOP;
            int y = 264;
            mk(L"BUTTON", L"Анимация под музыку (спектр, волна, эквалайзер)", cb, 20, y, 370, 22, CID_ANIM); y += 25;
            mk(L"BUTTON", L"Показывать трек при смене песни", cb, 20, y, 370, 22, CID_PEEK); y += 25;
            mk(L"BUTTON", L"Текст песен в такт (LRCLIB)", cb, 20, y, 370, 22, CID_LYR); y += 25;
            mk(L"BUTTON", L"Свой индикатор громкости вместо системного", cb, 20, y, 370, 22, CID_VOLHOOK); y += 25;
            mk(L"BUTTON", L"События: наушники, зарядка, яркость, Caps, раскладка", cb, 20, y, 370, 22, CID_SYSTOAST); y += 25;
            mk(L"BUTTON", L"Точка «микрофон / камера используются»", cb, 20, y, 370, 22, CID_PRIV); y += 25;
            mk(L"BUTTON", L"История буфера обмена (Ctrl+Alt+V)", cb, 20, y, 370, 22, CID_CLIP); y += 25;
            mk(L"BUTTON", L"Инструменты экрана (Ctrl+Alt+S / T / R / C)", cb, 20, y, 370, 22, CID_TOOLS); y += 25;
            mk(L"BUTTON", L"Показывать загрузки из браузера", cb, 20, y, 370, 22, CID_DL); y += 25;
            mk(L"BUTTON", L"Полка для файлов (перетащи файл на остров)", cb, 20, y, 370, 22, CID_SHELF); y += 25;
            mk(L"BUTTON", L"Запускать вместе с Windows", cb, 20, y, 370, 22, CID_AUTO); y += 34;
            mk(L"STATIC", L"", 0, 20, y, 360, 20, CID_CLIPLIM_LBL); y += 20;
            HWND t4 = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, 14, y, 372, 30, CID_CLIPLIM);
            SendMessageW(t4, TBM_SETRANGE, TRUE, MAKELONG(1, 20));
            SendMessageW(t4, TBM_SETTICFREQ, 2, 0);
            SendMessageW(t4, TBM_SETPAGESIZE, 0, 2);
            y += 36;
            mk(L"BUTTON", L"Очистить историю буфера", BS_PUSHBUTTON | WS_TABSTOP, 20, y, 200, 28, CID_CLIPCLEAR); y += 46;
            // правая колонка: виджеты и уведомления
            const int X = 430;
            int ry = 18;
            mk(L"STATIC", L"Город для погоды (пусто — определить по IP)", 0, X, ry, 370, 20, -1); ry += 22;
            HWND ec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_set.city.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      S(X), S(ry), S(370), S(26), w, (HMENU)(INT_PTR)CID_CITY, g_inst, nullptr);
            SendMessageW(ec, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(ec, EM_SETCUEBANNER, TRUE, (LPARAM)L"например, Иркутск");
            ry += 40;
            mk(L"STATIC", L"Календарь: ICS-ссылка (Google, Яндекс, Outlook)", 0, X, ry, 370, 20, -1); ry += 22;
            HWND ei = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_set.icsUrl.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      S(X), S(ry), S(370), S(26), w, (HMENU)(INT_PTR)CID_ICS, g_inst, nullptr);
            SendMessageW(ei, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
            SendMessageW(ei, EM_SETCUEBANNER, TRUE, (LPARAM)L"https://calendar.google.com/…/basic.ics");
            ry += 32;
            mk(L"STATIC", L"Google: Настройки календаря → «Секретный адрес в формате iCal». Яндекс: Настройки → Экспорт.",
               0, X, ry, 370, 36, -1);
            ry += 46;
            mk(L"BUTTON", L"Виджеты: погода, таймер, календарь", cb, X, ry, 370, 22, CID_WIDGETS); ry += 25;
            mk(L"BUTTON", L"Напоминать о событиях за 10 минут", cb, X, ry, 370, 22, CID_CALREM); ry += 25;
            mk(L"BUTTON", L"Уведомления Windows в острове", cb, X, ry, 370, 22, CID_NOTIFS); ry += 25;
            mk(L"BUTTON", L"Скрывать текст уведомлений", cb, X, ry, 370, 22, CID_NOTIFPRIV); ry += 25;
            mk(L"BUTTON", L"Звонки (Discord, Telegram…) с кнопкой микрофона", cb, X, ry, 370, 22, CID_CALLS); ry += 25;
            mk(L"BUTTON", L"Раскрывать остров при наведении мыши", cb, X, ry, 370, 22, CID_HOVEROPEN); ry += 34;
            mk(L"STATIC",
               L"Чтобы уведомления не дублировались, выключи баннеры Windows: Параметры → Система → Уведомления → "
               L"у нужного приложения «Показывать баннеры уведомлений».",
               0, X, ry, 370, 54, -1);
            y = std::max(y, ry + 64);
            mk(L"BUTTON", L"Сбросить", BS_PUSHBUTTON | WS_TABSTOP, 20, y, 110, 30, CID_RESET);
            mk(L"BUTTON", L"Готово", BS_DEFPUSHBUTTON | WS_TABSTOP, 690, y, 110, 30, CID_DONE);
            SettingsFill(w);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_HSCROLL: {
            HWND tb = (HWND)lp;
            int pos = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
            if (GetDlgCtrlID(tb) == CID_SIZE) g_set.sizePct = pos;
            else if (GetDlgCtrlID(tb) == CID_TOP) g_set.topOffset = pos;
            else if (GetDlgCtrlID(tb) == CID_LOFF) g_set.lyricOffsetMs = pos * 100;
            else if (GetDlgCtrlID(tb) == CID_CLIPLIM) {
                g_set.clipLimit = std::max(1, pos) * 50;
                if (g_clipStoreOk && (LOWORD(wp) == TB_ENDTRACK || LOWORD(wp) == TB_THUMBPOSITION)) g_clip.SetLimit(g_set.clipLimit);
            }
            SettingsSyncLabels(w);
            ApplySettings();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case CID_MON:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        int sel = (int)SendDlgItemMessageW(w, CID_MON, CB_GETCURSEL, 0, 0);
                        g_set.monitor = (sel > 0 && sel - 1 < (int)g_monList.size()) ? g_monList[sel - 1].device : L"";
                        ApplySettings();
                    }
                    break;
                case CID_ANIM: g_set.audioAnim = IsDlgButtonChecked(w, CID_ANIM) == BST_CHECKED; ApplySettings(); break;
                case CID_PEEK: g_set.peek = IsDlgButtonChecked(w, CID_PEEK) == BST_CHECKED; ApplySettings(); break;
                case CID_HOVEROPEN: g_set.hoverOpen = IsDlgButtonChecked(w, CID_HOVEROPEN) == BST_CHECKED; ApplySettings(); break;
                case CID_LYR: g_set.lyrics = IsDlgButtonChecked(w, CID_LYR) == BST_CHECKED; ApplySettings(); break;
                case CID_VOLHOOK: g_set.volHook = IsDlgButtonChecked(w, CID_VOLHOOK) == BST_CHECKED; ApplySettings(); break;
                case CID_SYSTOAST: g_set.sysToasts = IsDlgButtonChecked(w, CID_SYSTOAST) == BST_CHECKED; ApplySettings(); break;
                case CID_PRIV: g_set.privacy = IsDlgButtonChecked(w, CID_PRIV) == BST_CHECKED; ApplySettings(); break;
                case CID_CLIP: g_set.clip = IsDlgButtonChecked(w, CID_CLIP) == BST_CHECKED; ApplySettings(); break;
                case CID_TOOLS: g_set.tools = IsDlgButtonChecked(w, CID_TOOLS) == BST_CHECKED; ApplySettings(); break;
                case CID_DL: g_set.downloads = IsDlgButtonChecked(w, CID_DL) == BST_CHECKED; ApplySettings(); break;
                case CID_SHELF: g_set.shelf = IsDlgButtonChecked(w, CID_SHELF) == BST_CHECKED; ApplySettings(); break;
                case CID_CLIPCLEAR:
                    if (g_clipStoreOk && MessageBoxW(w, L"Удалить всю историю буфера, кроме закреплённых записей?",
                                                     kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES)
                        g_clip.Clear();
                    break;
                case CID_AUTO: SetAutostart(IsDlgButtonChecked(w, CID_AUTO) == BST_CHECKED); break;
                case CID_RESET: {
                    bool keepAnim = g_set.audioAnim;
                    g_set = Settings{};
                    g_set.audioAnim = keepAnim;
                    SettingsFill(w);
                    ApplySettings();
                    break;
                }
                case CID_CITY:
                case CID_ICS:
                    if (HIWORD(wp) == EN_KILLFOCUS) {
                        wchar_t buf[2048] = {};
                        GetDlgItemTextW(w, LOWORD(wp), buf, 2048);
                        std::wstring v = buf;
                        while (!v.empty() && iswspace(v.back())) v.pop_back();
                        while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
                        std::wstring& dst = LOWORD(wp) == CID_CITY ? g_set.city : g_set.icsUrl;
                        if (v != dst) { dst = v; ApplySettings(); }
                    }
                    break;
                case CID_WIDGETS: g_set.widgets = IsDlgButtonChecked(w, CID_WIDGETS) == BST_CHECKED; ApplySettings(); break;
                case CID_CALREM: g_set.calRemind = IsDlgButtonChecked(w, CID_CALREM) == BST_CHECKED; ApplySettings(); break;
                case CID_NOTIFS: g_set.notifs = IsDlgButtonChecked(w, CID_NOTIFS) == BST_CHECKED; ApplySettings(); break;
                case CID_NOTIFPRIV: g_set.notifPrivate = IsDlgButtonChecked(w, CID_NOTIFPRIV) == BST_CHECKED; ApplySettings(); break;
                case CID_CALLS: g_set.calls = IsDlgButtonChecked(w, CID_CALLS) == BST_CHECKED; ApplySettings(); break;
                case CID_DONE:
                case IDCANCEL:
                    SetFocus(GetDlgItem(w, CID_DONE));  // EN_KILLFOCUS сохранит поля ввода
                    DestroyWindow(w);
                    break;
            }
            return 0;
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DESTROY:
            g_settingsWnd = nullptr;
            if (g_uiFont) { DeleteObject(g_uiFont); g_uiFont = nullptr; }
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

// Настройки теперь живут в самом острове (вкладка за шестерёнкой)
static void OpenSettings() {
    if (g_clipOpen) CloseClip(false);
    if (g_cmdOpen) CloseCmd(false);
    if (!g_visible) { g_visible = true; ShowWindow(g_hwnd, SW_SHOWNOACTIVATE); }
    if (g_mode != M_EXPANDED) SetMode(M_EXPANDED);
    SwitchTab(TAB_SETTINGS);
}
// старое отдельное окно (оставлено на всякий случай: --settings-window)
static void OpenSettingsWindow() {
    if (g_settingsWnd) {
        ShowWindow(g_settingsWnd, SW_SHOWNORMAL);
        SetForegroundWindow(g_settingsWnd);
        return;
    }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.hIcon = g_iconBig;
        wc.hIconSm = g_icon;
        wc.lpszClassName = L"DerkIslandSettings";
        RegisterClassExW(&wc);
        reg = true;
    }
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float s = dx / 96.f;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, (LONG)std::lround(820 * s), (LONG)std::lround(697 * s)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dx);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    g_settingsWnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"DerkIslandSettings", L"Zen Island — настройки",
                                    style, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!g_settingsWnd) return;
    ShowWindow(g_settingsWnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_settingsWnd);
}

// ----------------------------------------------------------------------------
// Трей
// ----------------------------------------------------------------------------
static void AddTray() {
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof(n);
    n.hWnd = g_hwnd;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    n.uCallbackMessage = WM_APP_TRAY;
    n.hIcon = g_icon;
    wcscpy(n.szTip, L"Zen Island");
    Shell_NotifyIconW(NIM_ADD, &n);
}
static void RemoveTray() {
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof(n);
    n.hWnd = g_hwnd;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (std::wstring(kAppName) + L" · " + kAppVer).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Настройки…");
    if (g_set.clip && g_clipStoreOk) AppendMenuW(m, MF_STRING, ID_CLIP, L"Буфер обмена\tCtrl+Alt+V");
    if (g_set.tools) {
        AppendMenuW(m, MF_STRING, ID_SHOT, L"Скриншот области\tCtrl+Alt+S");
        AppendMenuW(m, MF_STRING, ID_OCR, L"Текст с экрана\tCtrl+Alt+T");
        AppendMenuW(m, MF_STRING, ID_REC, ((g_rec.Active() ? L"Остановить запись\t" : L"Запись экрана\t") + HotkeyName(g_set.recHotkey)).c_str());
        AppendMenuW(m, MF_STRING, ID_SHOTDIR, L"Папка скриншотов и записей");
    }
    AppendMenuW(m, MF_STRING | (g_set.audioAnim ? MF_CHECKED : 0), ID_ANIM, L"Анимация под музыку");
    AppendMenuW(m, MF_STRING | (g_set.lyrics ? MF_CHECKED : 0), ID_LYRICS, L"Текст песен");
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTO, L"Запускать вместе с Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_set.passThrough ? MF_CHECKED : 0), ID_PASS, L"Сквозной режим (клики сквозь остров)");
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_SETTINGS: OpenSettings(); break;
        case ID_CLIP: OpenClip(); break;
        case ID_SHOT: RunTool(TM_SHOT_AREA); break;
        case ID_OCR: RunTool(TM_OCR); break;
        case ID_REC: RunTool(TM_REC); break;
        case ID_SHOTDIR: {
            std::wstring dir = g_baseDir + L"\\screenshots";
            CreateDirectoryW(dir.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_ANIM:
            g_set.audioAnim = !g_set.audioAnim;
            SaveSettings();
            if (g_settingsWnd) CheckDlgButton(g_settingsWnd, CID_ANIM, g_set.audioAnim ? BST_CHECKED : BST_UNCHECKED);
            g_dirty = true;
            break;
        case ID_LYRICS:
            g_set.lyrics = !g_set.lyrics;
            if (g_settingsWnd) CheckDlgButton(g_settingsWnd, CID_LYR, g_set.lyrics ? BST_CHECKED : BST_UNCHECKED);
            ApplySettings();
            break;
        case ID_AUTO:
            SetAutostart(!IsAutostart());
            if (g_settingsWnd) CheckDlgButton(g_settingsWnd, CID_AUTO, IsAutostart() ? BST_CHECKED : BST_UNCHECKED);
            break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        case ID_PASS: g_set.passThrough = !g_set.passThrough; SaveSettings(); UpdatePassThrough(); break;
    }
}

static void ScheduleClock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    UINT ms = (60 - st.wSecond) * 1000 - st.wMilliseconds + 30;
    SetTimer(g_hwnd, T_CLOCK, ms, nullptr);
}

// права администратора (нужны для FPS)
static bool IsElevated() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        HANDLE t = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
            TOKEN_ELEVATION e = {};
            DWORD n = 0;
            if (GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n)) cached = e.TokenIsElevated ? 1 : 0;
            CloseHandle(t);
        }
    }
    return cached == 1;
}
static void RestartElevated() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t args[64];
    swprintf(args, 64, L"--after %lu", GetCurrentProcessId());
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) DestroyWindow(g_hwnd);  // новый экземпляр дождётся, пока мы закроемся
}
// exe программы на переднем плане (в нижнем регистре)
static std::wstring ForegroundExe(DWORD* pidOut = nullptr) {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_hwnd) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pidOut) *pidOut = pid;
    static DWORD lastPid = 0;
    static std::wstring lastExe;
    if (pid != lastPid) {
        lastPid = pid;
        lastExe.clear();
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t buf[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
                std::wstring p(buf, n);
                lastExe = LowerW(p.substr(p.find_last_of(L"\\") + 1));
            }
            CloseHandle(h);
        }
    }
    return lastExe;
}
// программа на переднем плане есть в списке исключений?
static bool ExcludedForeground() {
    if (g_set.exclude.empty() || g_clipOpen || g_cmdOpen) return false;
    std::wstring lastExe = ForegroundExe();
    if (lastExe.empty()) return false;
    std::wstring list = LowerW(g_set.exclude);
    size_t a = 0;
    while (a < list.size()) {
        size_t b = list.find_first_of(L",; ", a);
        if (b == std::wstring::npos) b = list.size();
        std::wstring t = list.substr(a, b - a);
        a = b + 1;
        if (t.empty()) continue;
        if (t.size() < 4 || t.compare(t.size() - 4, 4, L".exe") != 0) t += L".exe";
        if (t == lastExe) return true;
    }
    return false;
}
// ---- игровой режим: проверка, включение, итоги
static void GameSummary() {
    GameSession s = g_sess;
    g_sess = GameSession{};
    double dur = s.last - s.start;
    if (dur < 120 || !g_set.gameSummary) return;  // короче двух минут — не интересно
    int m = (int)(dur / 60);
    wchar_t b[96];
    if (m >= 60) swprintf(b, 96, L"%d ч %d мин", m / 60, m % 60);
    else swprintf(b, 96, L"%d мин", m);
    std::wstring t = s.title.empty() ? L"Игра" : s.title;
    if (t.size() > 28) t = t.substr(0, 27) + L"…";
    std::wstring right = b;
    if (s.fpsN > 0) { wchar_t f[32]; swprintf(f, 32, L" · ~%d FPS", (int)(s.fpsSum / s.fpsN + 0.5)); right += f; }
    ShowActionToast(TK_TEXT, t, right, TA_NONE, L"", g_accent, 6000);
}
static void GameOn(DWORD pid) {
    g_game.on = true;
    g_game.pid = pid;
    g_game.fps = -1;
    g_game.fpsOk = g_set.gameFps && (FpsRunning() || FpsStart());
    if (!g_sess.active || g_sess.pid != pid) {
        if (g_sess.active) GameSummary();
        g_sess = GameSession{};
        g_sess.active = true;
        g_sess.pid = pid;
        g_sess.start = NowSec();
        wchar_t t[128] = {};
        if (HWND fg = GetForegroundWindow()) GetWindowTextW(fg, t, 128);
        g_sess.title = t;
    }
    g_sess.last = NowSec();
    if (g_mode != M_EXPANDED && g_mode != M_CLIP && g_mode != M_CMD && g_mode != M_NOTIF && g_mode != M_DROP) SetMode(Collapsed());
    UpdatePassThrough();
    EnsureStats();
    SetTimer(g_hwnd, T_GAMETOP, 400, nullptr);
}
static void GameOff() {
    g_game.on = false;
    g_game.fps = -1;
    FpsStop();
    KillTimer(g_hwnd, T_GAMETOP);
    if (g_mode == M_GAME) SetMode(Collapsed());
    UpdatePassThrough();
}
static void CheckFullscreen();
// вызывается раз в секунду из StatsTick, пока идёт проверка
static void GameProbeTick() {
    if (!g_probe.pid) return;
    float gpu = g_ss.pidGpu;
    float fps = FpsRunning() ? FpsTake(g_probe.pid) : -1;
    g_probe.tries++;
    if (gpu >= 12.f || fps >= 20.f) g_probe.hits++;
    else g_probe.hits = 0;
    if (g_probe.hits >= 2) {  // две секунды подряд грузит видеокарту — игра
        DWORD pid = g_probe.pid;
        g_probe.pid = 0;
        GameOn(pid);
        CheckFullscreen();
    } else if (g_probe.tries >= 7) {
        if (g_notGame.size() > 64) g_notGame.clear();
        g_notGame.insert(g_probe.pid);
        g_probe.pid = 0;
        if (!g_game.on) FpsStop();
    }
}
// «Сквозной режим»: в свёрнутом виде клики проходят сквозь остров
static void UpdatePassThrough() {
    bool collapsed = g_mode != M_EXPANDED && g_mode != M_CLIP && g_mode != M_CMD && g_mode != M_NOTIF && g_mode != M_DROP;
    bool want = (g_set.passThrough && collapsed) || g_mode == M_GAME;  // в игре остров никогда не ловит клики
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    LONG_PTR nx = want ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    if (nx != ex) SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, nx);
}
static void CheckFullscreen() {
    if (g_toolHidden || ToolActive()) return;
    bool fs = false;
    QUERY_USER_NOTIFICATION_STATE q;
    if (SUCCEEDED(SHQueryUserNotificationState(&q)))
        fs = q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE;
    if (!fs && ExcludedForeground()) fs = true;  // программа из списка «прятать остров»
    if (!fs) {
        HWND fg = GetForegroundWindow();
        if (fg && fg != g_hwnd && fg != g_settingsWnd && fg != GetShellWindow() && fg != GetDesktopWindow() && !g_clipOpen && !g_cmdOpen) {
            wchar_t cls[64] = {};
            GetClassNameW(fg, cls, 64);
            if (wcscmp(cls, L"WorkerW") && wcscmp(cls, L"Progman")) {
                // окно без рамки на весь экран: сравниваем с монитором самого окна (в одних и тех же единицах DPI)
                RECT r;
                HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
                MONITORINFO mi = {sizeof(mi)};
                if (hm && hm == MonitorFromRect(&g_mon, MONITOR_DEFAULTTONEAREST) && GetMonitorInfoW(hm, &mi) && GetWindowRect(fg, &r) &&
                    !IsIconic(fg) && r.left <= mi.rcMonitor.left + 2 && r.top <= mi.rcMonitor.top + 2 && r.right >= mi.rcMonitor.right - 2 &&
                    r.bottom >= mi.rcMonitor.bottom - 2)
                    fs = true;
            }
        }
    }
    // Игра на весь экран → пилюля с FPS. Но «на весь экран» бывает и фото в Telegram, поэтому сперва
    // несколько секунд смотрим, грузит ли процесс видеокарту (или выдаёт кадры), и только потом решаем.
    bool cand = false;
    DWORD pid = 0;
    if (fs && g_set.gameHud == 1 && !ExcludedForeground() && !g_rec.Active()) {
        QUERY_USER_NOTIFICATION_STATE q2;
        bool pres = SUCCEEDED(SHQueryUserNotificationState(&q2)) && q2 == QUNS_PRESENTATION_MODE;
        std::wstring exe = ForegroundExe(&pid);
        g_fsExe = exe.empty() ? L"(защищённый процесс)" : exe;
        g_fsVideo = !exe.empty() && IsVideoOrBrowserExe(exe);
        g_fsPid = pid;
        if (!pres && !g_fsVideo && !g_notGame.count(pid)) cand = true;
    }
    if (getenv("DI_TEST_GAME")) { cand = true; pid = GetCurrentProcessId(); g_probe.pid = 0; if (!g_game.on) GameOn(pid); }
    bool game = cand && g_game.on && g_game.pid == pid;
    if (g_game.on && !game) GameOff();
    if (cand && !game) {
        if (g_probe.pid != pid) {
            g_probe = GameProbe{};
            g_probe.pid = pid;
            g_stats.watchPid = pid;
            if (g_set.gameFps && !FpsRunning()) FpsStart();  // с правами администратора — заодно смотрим кадры
            EnsureStats();
        }
    } else if (g_probe.pid) {
        g_probe.pid = 0;
        if (!g_game.on) FpsStop();
    }
    if (game) fs = false;
    if (g_sess.active && !g_game.on) {  // игру закрыли (или давно не возвращались) — итоги
        bool alive = false;
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_sess.pid)) { alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT; CloseHandle(h); }
        else alive = GetLastError() != ERROR_INVALID_PARAMETER;  // античит не пускает — значит, процесс есть
        if (!alive || NowSec() - g_sess.last > 600) GameSummary();
    }
    if (fs == g_visible) {
        g_visible = !fs;
        if (fs) {
            ShowWindow(g_hwnd, SW_HIDE);
        } else {
            ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
            g_dirty = true;
        }
        g_mw.SetInterval(fs ? 3000 : 1000);
    }
    if (g_visible)
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

// ---- автообновление
static std::wstring UpdateRepo() { return !g_set.updateRepo.empty() ? g_set.updateRepo : std::wstring(ZEN_REPO); }
static void UpdateCheck(bool manual) {
    if (UpdateRepo().empty()) { if (manual) g_updStatus = L"Обновления заработают, когда программа появится на GitHub"; g_dirty = true; return; }
    if (manual) g_updStatus = L"Проверяю…";
    UpdateCheckAsync(g_hwnd, UpdateRepo(), manual);
    g_dirty = true;
}
static void UpdateStartDownload() {
    if (g_upd.url.empty() || g_updBusy) return;
    g_updBusy = true;
    g_updStatus = L"Скачиваю " + g_upd.version + L"…";
    ShowActionToast(TK_DOWNLOAD, L"Скачиваю обновление " + g_upd.version + L"…", L"", TA_NONE, L"", g_accent, 3000);
    UpdateDownloadAsync(g_hwnd, g_upd.url);
}
static void OnUpdateInfo(bool newer, UpdateInfo* u) {
    if (newer) {
        g_upd = *u;
        g_updStatus = L"Доступна версия " + u->version;
        ShowActionToast(TK_DOWNLOAD, L"Доступна версия " + u->version, L"Обновить", TA_UPDATE, L"", g_accent, 12000);
    } else if (u) {
        if (!u->error.empty()) LogLine(L"Проверка обновлений: %ls", u->error.c_str());
        g_updStatus = u->error.empty() ? L"У тебя последняя версия" : L"Не получилось проверить: " + u->error;
        if (u->error.empty()) ShowActionToast(TK_TEXT, L"У тебя последняя версия", ZEN_VERSION, TA_NONE, L"", g_accent, 2500);
    }
    delete u;
    g_dirty = true;
}
static void OnUpdateDl(int pct, std::wstring* path) {
    if (pct <= 100) {
        static int last = -10;
        if (pct - last >= 10 || pct < last) {
            last = pct;
            g_updStatus = L"Скачиваю " + g_upd.version + L"… " + std::to_wstring(pct) + L"%";
            ShowActionToast(TK_DOWNLOAD, L"Обновление · " + std::to_wstring(pct) + L"%", L"", TA_NONE, L"", g_accent, 4000);
        }
        return;
    }
    g_updBusy = false;
    if (pct == 102 || !path) {
        LogLine(L"Обновление не скачалось: %ls", g_upd.url.c_str());
        g_updStatus = L"Не скачалось — попробуй позже";
        ShowActionToast(TK_ERROR, L"Обновление не скачалось", L"", TA_NONE, L"", kWhite, 3500);
        return;
    }
    // ставим тихо в ту же папку; установщик сам закроет остров и запустит новую версию
    std::wstring args = L"/S /D=\"" + g_baseDir + L"\"";
    ShowActionToast(TK_DOWNLOAD, L"Ставлю обновление…", L"", TA_NONE, L"", g_accent, 4000);
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", path->c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) <= 32)
        ShowActionToast(TK_ERROR, L"Не удалось запустить установщик", L"", TA_NONE, L"", kWhite, 3500);
    delete path;
}
// ---- знакомство при первом запуске: несколько подсказок прямо на острове
static void WelcomeStep() {
    if (g_launchCrashed) {
        g_launchCrashed = false;
        ShowActionToast(TK_ERROR, L"Остров перезапустился после ошибки", L"Журнал", TA_OPEN, LogPath(), kWhite, 5000);
        if (g_set.welcomed && !g_launchUpdated) return;
        SetTimer(g_hwnd, T_WELCOME, 5200, nullptr);
        return;
    }
    if (g_launchUpdated) {  // только что обновились
        g_launchUpdated = false;
        ShowActionToast(TK_DOWNLOAD, std::wstring(L"Обновлено до ") + ZEN_VERSION, L"", TA_NONE, L"", g_accent, 3500);
        if (g_set.welcomed && !g_launchWelcome) return;
        SetTimer(g_hwnd, T_WELCOME, 3800, nullptr);
        return;
    }
    static const wchar_t* tips[5][2] = {
        {L"Привет! Это Zen Island", L"1/5"},
        {L"Кликни по острову — откроется панель", L"2/5"},
        {L"Alt+Space — поиск программ, файлов и расчёты", L"3/5"},
        {L"Ctrl+Alt+V — буфер, Ctrl+Alt+S — снимок экрана", L"4/5"},
        {L"Настройки и питомец — за шестерёнкой", L"5/5"},
    };
    if (g_welcomeStep >= 5) {
        g_welcomeStep = 0;
        g_set.welcomed = true;
        g_launchWelcome = false;
        SaveSettings();
        return;
    }
    ShowActionToast(TK_TEXT, tips[g_welcomeStep][0], tips[g_welcomeStep][1], TA_NONE, L"", g_accent, 3600);
    if (g_welcomeStep == 0 && g_set.critter > 0) SetMood(1, 2.5);  // питомец радуется знакомству
    g_welcomeStep++;
    SetTimer(g_hwnd, T_WELCOME, 3800, nullptr);
}
// ---- бэкап: все настройки одним файлом (обычный .reg — его можно и просто открыть двойным кликом)
static bool RunHiddenWait(const std::wstring& cmdline) {
    std::wstring c = cmdline;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
static std::wstring PickSettingsFile(bool save) {
    wchar_t buf[MAX_PATH * 2] = L"Zen Island — настройки.reg";
    OPENFILENAMEW of = {sizeof(of)};
    of.lpstrFilter = L"Настройки Zen Island (*.reg)\0*.reg\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"reg";
    of.lpstrTitle = save ? L"Сохранить настройки" : L"Загрузить настройки";
    of.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!save) buf[0] = 0;
    BOOL ok = save ? GetSaveFileNameW(&of) : GetOpenFileNameW(&of);
    return ok ? buf : L"";
}
static void SettingsBackup() {
    SaveSettings();
    std::wstring f = PickSettingsFile(true);
    if (f.empty()) return;
    bool ok = RunHiddenWait(L"reg.exe export \"HKCU\\Software\\DerkIsland\" \"" + f + L"\" /y");
    ShowActionToast(ok ? TK_TEXT : TK_ERROR, ok ? L"Настройки сохранены" : L"Не получилось сохранить", ok ? L"Показать" : L"", ok ? TA_SELECT : TA_NONE, f,
                    g_accent, 3500);
}
static void RestartSelf() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t args[64];
    swprintf(args, 64, L"--after %lu", GetCurrentProcessId());
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", self, args, nullptr, SW_SHOWNORMAL) > 32) DestroyWindow(g_hwnd);
}
static void SettingsRestore() {
    std::wstring f = PickSettingsFile(false);
    if (f.empty()) return;
    // только наш раздел: в чужом .reg могут быть любые ключи — такой не берём
    std::string body;
    if (HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr); h != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(h, nullptr), rd = 0;
        if (sz < 4 * 1024 * 1024) { body.resize(sz); ReadFile(h, &body[0], sz, &rd, nullptr); body.resize(rd); }
        CloseHandle(h);
    }
    std::wstring w;
    if (body.size() >= 2 && (unsigned char)body[0] == 0xFF && (unsigned char)body[1] == 0xFE) w.assign((const wchar_t*)(body.data() + 2), (body.size() - 2) / 2);
    size_t cnt = 0, ours = 0;
    for (size_t p = 0; (p = w.find(L"\n[", p)) != std::wstring::npos; p++) {
        cnt++;
        if (_wcsnicmp(w.c_str() + p + 2, L"HKEY_CURRENT_USER\\Software\\DerkIsland", 37) == 0) ours++;
    }
    if (!cnt || cnt != ours) { ShowActionToast(TK_ERROR, L"Это не файл настроек Zen Island", L"", TA_NONE, L"", kWhite, 3500); return; }
    if (!RunHiddenWait(L"reg.exe import \"" + f + L"\"")) { ShowActionToast(TK_ERROR, L"Не получилось загрузить", L"", TA_NONE, L"", kWhite, 3500); return; }
    RestartSelf();  // перечитать всё с нуля
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return (g_clipOpen || g_cmdOpen || g_ieFocus >= 0 || g_hkCap) ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && g_clipOpen) CloseClip(false);  // кликнули мимо — закрываем
            if (LOWORD(wp) == WA_INACTIVE && g_cmdOpen) SetTimer(hwnd, T_CMDBLUR, 120, nullptr);  // проверим, куда ушёл фокус
            if (LOWORD(wp) == WA_INACTIVE && g_ieFocus >= 0) IEBlur(true);
            if (LOWORD(wp) == WA_INACTIVE && g_hkCap) RecHkEnd();
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (g_hkCap) { RecHkKey((UINT)wp); return 0; }
            if (g_cmdOpen) { OnCmdKey((UINT)wp); return 0; }
            if (g_ieFocus >= 0) { IEKey((UINT)wp); return 0; }
            if (g_clipOpen) { OnClipKey((UINT)wp); return 0; }
            break;
        case WM_SYSCHAR:
            if (g_cmdOpen || g_ieFocus >= 0) return 0;  // Alt+Space не должен открывать системное меню
            break;
        case WM_CHAR:
            if (g_cmdOpen) { OnCmdChar((wchar_t)wp); return 0; }
            if (g_ieFocus >= 0) { IEChar((wchar_t)wp); return 0; }
            if (g_clipOpen && wp >= 32 && wp != 127 && !(GetKeyState(VK_CONTROL) & 0x8000)) {
                g_clipQuery += (wchar_t)wp;
                g_clipSel = 0;
                g_clipScroll.target = 0;
                g_caretOn = true;
                ClipRefilter();
            }
            return 0;
        case WM_HOTKEY:
            if (wp == HK_CMD) { if (g_cmdOpen) CloseCmd(true); else OpenCmd(); return 0; }
            if (wp == HK_CLIP) { if (g_clipOpen) CloseClip(true); else OpenClip(); }
            else if (wp == HK_SHOT) RunTool(TM_SHOT_AREA);
            else if (wp == HK_OCR) RunTool(TM_OCR);
            else if (wp == HK_REC) RecHotkeyPressed();
            else if (wp == HK_PICK) RunTool(TM_PICK);
            return 0;
        case WM_APP_TOOL: OnToolResult((ToolResult*)lp); return 0;
        case WM_APP_WORK: OnWorkResult((WorkResult*)lp); return 0;
        case WM_APP_REC: OnRecDone(wp != 0, (std::wstring*)lp); return 0;
        case WM_APP_DL: OnDownload((int)wp, (DlInfo*)lp); return 0;
        case WM_APP_WEATHER: {
            WeatherData* d = (WeatherData*)lp;
            if (d->ok || !g_wx.ok) g_wx = *d;  // при ошибке сети оставляем прошлую погоду
            delete d;
            CheckRain();
            if (g_mode == M_IDLE) Retarget();
            g_dirty = true;
            return 0;
        }
        case WM_APP_CAL: {
            CalData* d = (CalData*)lp;
            if (d->ok || g_set.icsUrl.empty() || !g_cal.ok) g_cal = *d;
            delete d;
            g_dirty = true;
            return 0;
        }
        case WM_APP_NOTIF: OnNotif((NotifData*)lp); return 0;
        case WM_CLIPBOARDUPDATE:
            if (g_set.clip) SetTimer(hwnd, T_CLIPREAD, 120, nullptr);  // приложения кладут форматы не сразу
            return 0;
        case WM_APP_CLIP:
            OnClipUpdate((ClipUpdate*)lp);
            return 0;
        case WM_APP_SYS:
            OnSysEvent((int)wp, lp);
            return 0;
        case WM_APP_KEY:
            OnKeyEvent((UINT)wp, lp != 0);
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_POWERSETTINGCHANGE) OnPowerSetting((const POWERBROADCAST_SETTING*)lp);
            if (wp == PBT_APMRESUMEAUTOMATIC) {  // проснулись: мониторы, сеть и погода могли поменяться
                LogLine(L"Выход из сна");
                UpdateMetrics();
                if (g_set.vpn) VpnCheckAsync(hwnd, true);
                g_wxs.Refresh();
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                g_dirty = true;
            }
            return TRUE;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, (g_hoverBtn >= 0 || ((g_mode == M_EXPANDED || g_mode == M_CLIP) && g_hotId != 0) || g_hoverProg || g_dragSeek || g_hoverTool >= 0 || g_hoverShelf >= 0 ||
                                           g_hoverShelfClear || (g_mode == M_CLIP && g_clipHover >= 0) ||
                                           (g_mode == M_TOAST && g_toast.action != TA_NONE))
                                              ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (!g_tracking) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                g_tracking = true;
            }
            KillTimer(hwnd, T_COLLAPSE);
            if (!g_hover) {
                g_hover = true;
                Retarget();
                if (g_set.hoverOpen && (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_TIMER ||
                                        g_mode == M_DL))
                    SetTimer(hwnd, T_HOVER, 320, nullptr);
            }
            if (ToastDragMove(x, y)) return 0;
            if (PullMove(y)) return 0;
            if (Sty() == ST_GLASS) {
                float lx = Clamp((x / g_scale - g_L.px) / std::max(1.f, g_L.pw), 0.f, 1.f);
                if (std::fabs(lx - g_lightX) > 0.02f) { g_lightX = lx; g_dirty = true; }
            }
            if (g_mode == M_NOTIF) NotifHover(x, y);
            if (g_mode == M_CMD) {
                int h = CmdHit(x, y);
                if (h != g_cmdHover) { g_cmdHover = h; g_dirty = true; }
                return 0;
            }
            if (g_mode == M_EXPANDED || g_mode == M_CLIP) PanelHover(x, y);
            if (g_volDrag) return 0;
            if (g_mode == M_CLIP) {
                int btn = -1;
                int v = ClipHit(x, y, &btn);
                if (v != g_clipHover || btn != g_clipHoverBtn) { g_clipHover = v; g_clipHoverBtn = btn; g_dirty = true; }
                return 0;
            }
            if (g_dragSeek) {
                g_dragFrac = ProgFrac(x);
                g_dirty = true;
                return 0;
            }
            if (g_shelfPress != -1) {  // тянем файл с полки наружу
                if (std::abs(x - g_shelfPressPt.x) > 6 || std::abs(y - g_shelfPressPt.y) > 6) {
                    int i = g_shelfPress;
                    g_shelfPress = -1;
                    ReleaseCapture();
                    ShelfDragOut(i);
                }
                return 0;
            }
            int hb = HitBtn(x, y);
            bool hp = hb < 0 && HitProg(x, y);
            int hc2 = -1;
            if (g_mode == M_EXPANDED && g_L.hasWidgets) {
                if (g_L.timerOn) { if (InRect(g_L.tPause, x, y)) hc2 = 10; else if (InRect(g_L.tStop, x, y)) hc2 = 11; }
                else for (int i = 0; i < 3; i++) if (InRect(g_L.chip[i], x, y)) hc2 = i;
            }
            bool hm = g_mode == M_CALL && InRect(g_L.mic, x, y);
            if (hc2 != g_hoverChip || hm != g_hoverMic) { g_hoverChip = hc2; g_hoverMic = hm; g_dirty = true; }
            int ht = HitTool(x, y);
            bool sx = false;
            int hs = HitShelf(x, y, &sx);
            bool hc = ShelfShown() && g_mode == M_EXPANDED && InRect(g_L.shelfClear, x, y);
            if (hb != g_hoverBtn || hp != g_hoverProg || ht != g_hoverTool || hs != g_hoverShelf || sx != g_hoverShelfX ||
                hc != g_hoverShelfClear) {
                g_hoverBtn = hb; g_hoverProg = hp; g_hoverTool = ht; g_hoverShelf = hs; g_hoverShelfX = sx; g_hoverShelfClear = hc;
                g_dirty = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_tracking = false;
            KillTimer(hwnd, T_HOVER);
            if (g_hotId) { g_hotId = 0; g_hotArg = -1; g_dirty = true; }
            if (g_dragSeek || g_pressBtn >= 0 || g_volDrag || g_setDrag >= 0) return 0;  // мышь захвачена — не сворачиваемся
            if (g_mode == M_CMD) { g_hover = false; g_cmdHover = -1; g_dirty = true; return 0; }
            if (g_mode == M_CLIP) { g_hover = false; g_clipHover = g_clipHoverBtn = -1; g_dirty = true; return 0; }
            if (g_shelfPress != -1) return 0;
            g_hoverTool = g_hoverShelf = -1;
            g_hoverShelfX = g_hoverShelfClear = false;
            g_hover = false;
            g_hoverBtn = -1;
            g_hoverProg = false;
            if (g_mode == M_EXPANDED) SetTimer(hwnd, T_COLLAPSE, 450, nullptr);
            Retarget();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:  // быстрый второй клик — тоже нажатие
            OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
            OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd && g_setDrag >= 0) SettingsDragEnd();
            if ((HWND)lp != hwnd && g_pullDown) { g_pullDown = false; Retarget(); }
            if ((HWND)lp != hwnd) { g_sbarDrag = false; g_recDrag = -1; }
            if ((HWND)lp != hwnd && (g_dragSeek || g_pressBtn >= 0)) {
                g_dragSeek = false;
                g_pressBtn = -1;
                g_dirty = true;
            }
            return 0;
        case WM_RBUTTONUP:
            if (g_mode == M_EXPANDED && PanelRightClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
            ShowMenu();
            return 0;
        case WM_MOUSEWHEEL: {
            if (g_mode == M_CMD) return 0;
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            if (g_mode == M_EXPANDED && PanelWheel(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp))) return 0;
            OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case WM_APP_CTRL: OnCtrlInfo((CtrlInfo*)lp); return 0;
        case WM_APP_INDEX: OnIndex((CmdIndexData*)lp); return 0;
        case WM_APP_MARKUP: OnMarkup((MarkupResult*)lp); return 0;
        case WM_APP_PODS: OnPods((PodsInfo*)lp); return 0;
        case WM_APP_REPLY: OnReplyResult((ReplyResult*)lp); return 0;
        case WM_APP_RATES: OnRates((std::map<std::string, double>*)lp); return 0;
        case WM_APP_NETCHG: if (g_set.vpn) SetTimer(hwnd, T_VPN, 2000, nullptr); return 0;
        case WM_APP_UPDATE: OnUpdateInfo(wp == 1, (UpdateInfo*)lp); return 0;
        case WM_APP_UPDDL: OnUpdateDl((int)wp, (std::wstring*)lp); return 0;
        case WM_APP_VPN: OnVpn((VpnInfo*)lp); return 0;
        case WM_TIMER:
            switch (wp) {
                case T_CLOCK: {
                    KillTimer(hwnd, T_CLOCK);
                    g_dirty = true;
                    ScheduleClock();
                    CheckCalendarReminders();
                    NyTick();
                    if (FxBusy()) FxEnsureTimer();
                    // раз в 5 минут простоя отдаём Windows неиспользуемую память (Диспетчер задач покажет меньше)
                    static int idleMin = 0;
                    bool idle = g_mode != M_EXPANDED && g_mode != M_CLIP && g_mode != M_CMD && !g_rec.Active() && !g_game.on;
                    if (idle && ++idleMin >= 5) {
                        idleMin = 0;
                        HeapCompact(GetProcessHeap(), 0);
                        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
                    } else if (!idle) idleMin = 0;
                    break;
                }
                case T_TIMER: TimerTick(); break;
                case T_NOTIF:
                    if (g_notifReply) break;                  // отвечают — не убираем
                    if (g_hover && g_mode == M_NOTIF) break;  // навели мышь — держим
                    ShowNextNotif();
                    break;
                case T_CALL:
                    if (g_mode == M_CALL) { RefreshMicMute(); g_dirty = true; }
                    break;
                case T_TICK: g_dirty = true; break;
                case T_FS:
                    if (getenv("DI_PERF")) {  // замер: кадров за последние 1,5 с
                        static unsigned lastF = 0;
                        LogLine(L"perf: кадров %u, режим %d, dirty-причины: moving=%d", g_frames - lastF, (int)g_mode, (int)SpringsMoving());
                        lastF = g_frames;
                    }
                    if (g_set.critter > 0) {
                        bool sl = g_set.petSleep && (UserAway() || (NightTime() && IdleMs() > 45000));  // ночью засыпает быстрее
                        if (sl != g_sleepy) {
                            if (!sl) SetMood(1, 2.2);  // вернулся — обрадовался
                            g_sleepy = sl;
                            g_dirty = true;
                        }
                    }
                    EnsureStats();
                    CheckFullscreen();
                    CheckLayout(true);
                    if (!g_notif && !g_notifQ.empty() && g_mode != M_CLIP && g_mode != M_CMD && g_mode != M_EXPANDED && g_mode != M_DROP && !g_toolHidden)
                        ShowNextNotif();
                    if (g_viz.Running() && !AudioActive() && NowSec() - g_vizLastActive > 3) g_viz.Stop();
                    break;
                case T_PEEK:
                    KillTimer(hwnd, T_PEEK);
                    if (g_mode == M_PEEK) {
                        if (g_hover) SetTimer(hwnd, T_PEEK, 800, nullptr);
                        else SetMode(Collapsed());
                    }
                    break;
                case T_COLLAPSE:
                    KillTimer(hwnd, T_COLLAPSE);
                    if (g_mode != M_EXPANDED || g_pinned || g_hover || g_dragSeek || g_volDrag || g_setDrag >= 0) break;
                    if (EditorFocused() || g_ieFocus >= 0) break;     // печатают — ждём, пока уйдут
                    if (CursorOverIsland()) { SetTimer(hwnd, T_COLLAPSE, 400, nullptr); break; }  // мышь над редактором
                    SetMode(Collapsed());
                    break;
                case T_HOVER:
                    KillTimer(hwnd, T_HOVER);
                    if (g_hover && g_set.hoverOpen && !g_dropActive &&
                        (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_TIMER || g_mode == M_DL))
                        SetMode(M_EXPANDED);
                    break;
                case T_CURSOR: PollCursorPull(); break;
                case T_REMIND: RemCheck(); break;
                case T_NOTESAVE: KillTimer(hwnd, T_NOTESAVE); NoteSave(); break;
                case T_SETTEXT: KillTimer(hwnd, T_SETTEXT); SettingsTextCommit(); break;
                case T_CMDBLUR: {
                    KillTimer(hwnd, T_CMDBLUR);
                    HWND fg = GetForegroundWindow();
                    if (g_cmdOpen && fg && fg != hwnd && !getenv("DI_TEST")) CloseCmd(false);  // ушли в другое окно — закрываемся
                    break;
                }
                case T_AUDIO: {
                    float p = g_audio.Peak();
                    if (p > 0.01f) { g_peak = p; g_level = std::max(g_level, 0.02f); }
                    break;
                }
                case T_TOAST:
                    KillTimer(hwnd, T_TOAST);
                    if (g_hover && (g_mode == M_DEV || (g_mode == M_TOAST && g_toast.kind == TK_SHOT))) { SetTimer(hwnd, T_TOAST, 1500, nullptr); break; }
                    if (g_mode == M_TOAST || g_mode == M_DEV) SetMode(Collapsed());
                    break;
                case T_STATS: StatsTick(); break;
                case T_GLASS: CaptureBackdrop(); break;
                case T_FX: FxTick(); break;
                case T_NY: NyTick(); break;
                case T_PODSMISS:
                    KillTimer(hwnd, T_PODSMISS);
                    if (g_podsManual && !g_podsGot)
                        ShowActionToast(TK_AUDIO, L"Не вижу AirPods рядом", L"открой кейс", TA_NONE, L"", kGray, 2500);
                    break;
                case T_CAPS: {
                    KillTimer(hwnd, T_CAPS);
                    bool on = (GetKeyState(VK_CAPITAL) & 1) != 0;
                    Toast t;
                    t.kind = TK_CAPS;
                    t.flag = on;
                    t.text = on ? L"Caps Lock включён" : L"Caps Lock выключен";
                    t.color = g_accent;
                    ShowToast(t, 1200);
                    break;
                }
                case T_LAYOUT: KillTimer(hwnd, T_LAYOUT); CheckLayout(true); break;
                case T_CLIPREAD: KillTimer(hwnd, T_CLIPREAD); ReadClipboard(); break;
                case T_CARET: g_caretOn = !g_caretOn; g_dirty = true; break;
                case T_VPN: KillTimer(hwnd, T_VPN); if (g_set.vpn) VpnCheckAsync(hwnd, true); break;
                case T_BLINK: CritterBlinkTick(); break;
                case T_MEDIAHIDE: KillTimer(hwnd, T_MEDIAHIDE); OnMedia(); break;
                case T_MEDIAPOKE: KillTimer(hwnd, T_MEDIAPOKE); g_mw.Command(CMD_NONE); break;
                case T_PASTE: {
                    // ждём, пока отпустят Ctrl/Alt/Shift от хоткея, иначе вставка превратится в другое сочетание
                    bool held = (GetAsyncKeyState(VK_MENU) | GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_LWIN) |
                                 GetAsyncKeyState(VK_RWIN) | GetAsyncKeyState(VK_CONTROL)) & 0x8000;
                    if (held && ++g_pasteTries < 30) break;
                    KillTimer(hwnd, T_PASTE);
                    SendCtrlV();
                    break;
                }
                case T_TOOLGO:
                    KillTimer(hwnd, T_TOOLGO);
                    if (!StartTool(hwnd, g_toolPending)) RestoreAfterTool();
                    break;
                case T_RECCD: RecCdTick(); break;
                case T_UPDATE:
                    SetTimer(hwnd, T_UPDATE, 6 * 3600 * 1000, nullptr);  // дальше — раз в 6 часов
                    if (g_set.autoUpdate) UpdateCheck(false);
                    break;
                case T_WELCOME: KillTimer(hwnd, T_WELCOME); WelcomeStep(); break;
                case T_GAMETOP:  // игра на весь экран любит вылезать наверх — почаще возвращаем остров поверх неё
                    if (g_game.on && g_visible)
                        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    break;
                case T_REC:
                    g_recBlink = !g_recBlink;
                    if (g_mode == M_REC) g_dirty = true;
                    break;
                case T_SHELFCHECK: KillTimer(hwnd, T_SHELFCHECK); ShelfPrune(); break;
                case T_LYR:
                    KillTimer(hwnd, T_LYR);
                    RequestLyrics(false);
                    break;
                case T_VOL:
                    KillTimer(hwnd, T_VOL);
                    if (g_mode == M_TOAST) SetMode(Collapsed());
                    else g_dirty = true;
                    break;
            }
            return 0;
        case WM_APP_MEDIA:
            OnMedia();
            return 0;
        case WM_APP_LYRICS: {
            if ((UINT)wp != g_lyrReq) return 0;
            Lyrics L;
            int st = LYR_NONE;
            if (g_lyrSvc.Take(g_lyrReq, L, st)) {
                g_lyrState = st;
                if (st == LYR_FOUND) {
                    g_lyr = std::move(L);
                    g_lyrLine = g_lyrPrevLine = -100;
                    UpdateLyrics();
                    if (g_mode == M_COMPACT) SetMode(M_LYRICS);
                    else Retarget();
                }
                g_dirty = true;
            }
            return 0;
        }
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: ShowMenu(); break;
                case WM_LBUTTONDBLCLK: OpenSettings(); break;
                case WM_LBUTTONUP: SetMode(g_mode == M_EXPANDED ? Collapsed() : M_EXPANDED); break;
            }
            return 0;
        case WM_DISPLAYCHANGE:
            LogLine(L"Сменились мониторы/разрешение");
            UpdateMetrics();
            return 0;
        case WM_DPICHANGED:
            UpdateMetrics();
            return 0;
        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) UpdateMetrics();
            if (wp == SPI_SETDESKWALLPAPER && g_sd.bg == BG_MATERIAL) ApplyTheme();
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
                if (g_set.theme == 2 || g_set.accentMode == 1) ApplyTheme();
                g_darkTheme = IsDarkTheme();
                g_dirty = true;
            }
            break;
        case WM_DESTROY:
            FpsStop();
            NoteSave();
            if (g_settingsWnd) DestroyWindow(g_settingsWnd);
            RevokeDragDrop(hwnd);
            for (int id : {HK_SHOT, HK_OCR, HK_REC, HK_PICK, HK_CMD}) UnregisterHotKey(hwnd, id);
            UnregisterHotKey(hwnd, HK_CLIP);
            RemoveClipboardFormatListener(hwnd);
            RemoveKeyHook();
            for (auto& h : g_pwrN) if (h) { UnregisterPowerSettingNotification(h); h = nullptr; }
            RemoveTray();
            PostQuitMessage(0);
            return 0;
        default:
            if (msg == g_taskbarCreated && g_taskbarCreated) { AddTray(); return 0; }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// Главный цикл: кадры только когда нужно
// ----------------------------------------------------------------------------
static void WaitFor(double seconds) {
    if (seconds <= 0) return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(seconds * 1e7);
    if (g_frameTimer && SetWaitableTimer(g_frameTimer, &due, 0, nullptr, nullptr, FALSE))
        MsgWaitForMultipleObjects(1, &g_frameTimer, FALSE, INFINITE, QS_ALLINPUT);
    else
        MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)(seconds * 1000) + 1, QS_ALLINPUT);
}
static void Dispatch(MSG& msg) {
    if (g_settingsWnd && IsDialogMessageW(g_settingsWnd, &msg)) return;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
}

static int RunLoop() {
    MSG msg;
    bool wasAnim = false;
    for (;;) {
        bool anim = NeedAnim();
        if (anim || (g_dirty && g_visible)) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return (int)msg.wParam;
                Dispatch(msg);
            }
            if (g_audioTimer) { KillTimer(g_hwnd, T_AUDIO); g_audioTimer = false; }
            double now = NowSec();
            if (!wasAnim) g_lastFrame = now - 1.0 / 60;
            anim = NeedAnim();
            wasAnim = anim;
            if (!anim) {  // одиночная перерисовка (часы, смена трека и т.п.)
                if (g_dirty && g_visible) Frame();
                continue;
            }
            bool smooth = SpringsMoving() || g_content < 1.f || UiAnimating() || g_dragSeek ||
                          (g_mode == M_EXPANDED && AudioActive());
            double fps = smooth ? 60.0 : 30.0;
            double next = g_lastFrame + 1.0 / fps;
            if (now >= next - 0.0007) {
                Frame();
                next = g_lastFrame + 1.0 / fps;
            }
            WaitFor(next - NowSec());
        } else {
            wasAnim = false;
            bool wantAudioPoll = AudioActive();
            if (wantAudioPoll && !g_audioTimer) { SetTimer(g_hwnd, T_AUDIO, 150, nullptr); g_audioTimer = true; }
            if (!wantAudioPoll && g_audioTimer) { KillTimer(g_hwnd, T_AUDIO); g_audioTimer = false; }
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) return r == 0 ? (int)msg.wParam : 1;
            Dispatch(msg);
        }
    }
}

// ---- удаление (так его вызывает «Установленные приложения»: ZenIsland.exe --uninstall)
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZenIsland";
static void SchedTaskRemove();
static int RunUninstall(bool quiet) {
    if (!quiet && MessageBoxW(nullptr, L"Удалить Zen Island с компьютера?", L"Zen Island", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    for (int i = 0; i < 50; i++) {  // закрываем работающий остров
        HWND w = FindWindowW(L"DerkDynamicIsland", nullptr);
        if (!w) break;
        if (i == 0) PostMessageW(w, WM_CLOSE, 0, 0);
        Sleep(100);
    }
    bool wipe = !quiet && MessageBoxW(nullptr, L"Удалить заодно историю буфера, скриншоты и настройки?\n\n«Нет» — оставить их на случай, если вернёшься.",
                                      L"Zen Island", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    wchar_t exeB[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exeB, MAX_PATH);
    std::wstring exe(exeB, n), dir = exe.substr(0, exe.find_last_of(L'\\'));
    wchar_t loc[MAX_PATH] = {};
    DWORD sz = sizeof(loc);
    RegGetValueW(HKEY_CURRENT_USER, kUninstKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, loc, &sz);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
    SchedTaskRemove();
    auto known = [](REFKNOWNFOLDERID id) {
        PWSTR p = nullptr;
        std::wstring r;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
        if (p) CoTaskMemFree(p);
        return r;
    };
    DeleteFileW((known(FOLDERID_Programs) + L"\\Zen Island.lnk").c_str());
    DeleteFileW((known(FOLDERID_Desktop) + L"\\Zen Island.lnk").c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstKey);
    if (wipe) RegDeleteTreeW(HKEY_CURRENT_USER, kRegKey);
    // папку целиком сносим, только если это наша папка установки «…\Zen Island» (не D:\ и не папка с исходниками)
    bool ours = _wcsicmp(loc, dir.c_str()) == 0 && dir.size() > 11 && _wcsicmp(dir.c_str() + dir.size() - 10, L"Zen Island") == 0;
    std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + exe + L"\" & ";
    cmd += (wipe && ours) ? L"rmdir /s /q \"" + dir + L"\"" : L"rmdir \"" + dir + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, L"C:\\", &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (!quiet) MessageBoxW(nullptr, L"Zen Island удалён. Спасибо, что попробовал!", L"Zen Island", MB_ICONINFORMATION);
    return 0;
}
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    g_inst = hInst;
    if (cmdLine && wcsstr(cmdLine, L"--uninstall")) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        return RunUninstall(wcsstr(cmdLine, L"--quiet") != nullptr);
    }
    g_launchWelcome = cmdLine && wcsstr(cmdLine, L"--welcome");
    g_launchUpdated = cmdLine && wcsstr(cmdLine, L"--updated");
    if (cmdLine && (g_launchWelcome || g_launchUpdated)) {  // после установки старый экземпляр мог ещё не выйти
        for (int i = 0; i < 40; i++) {
            HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\DerkDynamicIsland");
            if (!m) break;
            CloseHandle(m);
            Sleep(100);
        }
    }
    if (cmdLine && wcsstr(cmdLine, L"--after ")) {  // перезапуск с правами: ждём, пока старый экземпляр выйдет
        DWORD old = (DWORD)_wtoi(wcsstr(cmdLine, L"--after ") + 8);
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, old)) { WaitForSingleObject(h, 8000); CloseHandle(h); }
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DerkDynamicIsland");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    OleInitialize(nullptr);  // OLE нужен для drag&drop (полка)
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    LoadSettings();
    if (IsAutostart()) SetAutostart(true);  // путь мог смениться (DynamicIsland.exe → ZenIsland.exe)
    g_sd = Def(Sty());
    StyleTargets(true);
    g_fontName = StyleFont();
    SetThemeColors(g_set.theme == 1 && !g_sd.darkOnly);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                 (void**)&g_d2f)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dwf))) {
        MessageBoxW(nullptr, L"Не удалось инициализировать Direct2D/DirectWrite.", kAppName, MB_ICONERROR);
        return 1;
    }
    CreateFormats();

    g_icon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON), 0);
    g_iconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), 0);
    if (!g_icon) g_icon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!g_iconBig) g_iconBig = g_icon;

    for (int i = 0; i < 3; i++) { g_btnScale[i].Set(1); g_btnHov[i].Set(0); }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_icon;
    wc.lpszClassName = L"DerkDynamicIsland";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Zen Island", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst,
                             nullptr);
    if (!g_hwnd) return 1;

    UpdateMetrics();
    CreateTarget();

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();

    g_frameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_frameTimer) g_frameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // Появление: маленькая точка раскрывается в пилюлю
    sW.x = 36; sH.x = 18; sR.x = 9;
    g_mode = M_IDLE;
    g_modeT = NowSec();
    Retarget();

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    {
        // кэш текстов — рядом с exe (у тебя это диск D)
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe, n);
        size_t slash = dir.find_last_of(L"\\/");
        g_baseDir = slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash);
        dir = g_baseDir + L"\\cache\\lyrics";
        g_lyrSvc.Start(g_hwnd, dir);
        // история буфера — тоже рядом с exe
        std::wstring dataDir = dir.substr(0, dir.size() - wcslen(L"\\cache\\lyrics")) + L"\\data";
        CreateDirectoryW(dataDir.c_str(), nullptr);
        LogInit(dataDir);
        if (cmdLine && wcsstr(cmdLine, L"--crashed")) g_launchCrashed = true;
        g_clipStoreOk = g_clip.Open(dataDir + L"\\clipboard.db", g_hwnd, g_set.clipLimit);
        if (g_clipStoreOk) g_clipItems = g_clip.LoadAll();
    }
    g_audioWatch.Start(g_hwnd);
    g_privWatch.Start(g_hwnd);
    {
        static const GUID kAcDc = {0x5D3E9A59, 0xE9D5, 0x4B00, {0xA6, 0xBD, 0xFF, 0x34, 0xFF, 0x51, 0x65, 0x48}};
        static const GUID kBatPct = {0xA7AD8041, 0xB45A, 0x4CAE, {0x87, 0xA3, 0xEE, 0xCB, 0xB4, 0x68, 0xA9, 0xE1}};
        static const GUID kBright = {0x8FFEE2C6, 0x2D01, 0x46BE, {0xAD, 0xB9, 0x39, 0x8A, 0xDD, 0xC5, 0xB4, 0xFF}};
        g_pwrN[0] = RegisterPowerSettingNotification(g_hwnd, &kAcDc, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_pwrN[1] = RegisterPowerSettingNotification(g_hwnd, &kBatPct, DEVICE_NOTIFY_WINDOW_HANDLE);
        g_pwrN[2] = RegisterPowerSettingNotification(g_hwnd, &kBright, DEVICE_NOTIFY_WINDOW_HANDLE);
    }
    ShelfLoad();
    LauncherLoad();
    ApplyTheme();
    StyleTargets(true);
    UpdateAffinity();
    NyTick();
    if (g_set.vpn) { VpnWatchStart(g_hwnd); VpnCheckAsync(g_hwnd, true); }
    SetTimer(g_hwnd, T_BLINK, 3000, nullptr);
    SetTimer(g_hwnd, T_UPDATE, 30000, nullptr);  // первая проверка обновлений — через полминуты после старта
    if (!g_set.welcomed || g_launchWelcome || g_launchUpdated || g_launchCrashed) SetTimer(g_hwnd, T_WELCOME, 2500, nullptr);
    UpdatePassThrough();
    if (g_glassAllowed) CaptureBackdrop();
    CmdRequestIndex(true);  // список программ — заранее, в фоне
    g_rem = LoadReminders(DataPath(L"reminders.txt"));
    RemSort();
    g_dropTarget = new DropTarget();
    RegisterDragDrop(g_hwnd, g_dropTarget);
    ApplySystemHooks();
    CheckLayout(false);
    g_mw.Start(g_hwnd);
    SetTimer(g_hwnd, T_FS, 1500, nullptr);
    SetTimer(g_hwnd, T_REMIND, 15000, nullptr);
    SetTimer(g_hwnd, T_CURSOR, 90, nullptr);
    RemCheck();
    ScheduleClock();
    if (const wchar_t* cq = cmdLine ? wcsstr(cmdLine, L"--cmd=") : nullptr) {  // открыть командную строку с текстом
        OpenCmd();
        g_cmdQ = cq + 6;
        while (!g_cmdQ.empty() && (g_cmdQ.back() == L'"' || g_cmdQ.back() == L' ')) g_cmdQ.pop_back();
        CmdChanged();
    }
    if (cmdLine && wcsstr(cmdLine, L"--settings-window")) OpenSettingsWindow();
    else if (cmdLine && wcsstr(cmdLine, L"--settings")) OpenSettings();
    if (cmdLine) {  // запуск инструмента из ярлыка/скрипта: --tool=area|screen|ocr|qr|pick|rec
        struct { const wchar_t* k; int m; } tl[] = {{L"--tool=area", TM_SHOT_AREA}, {L"--tool=screen", TM_SHOT_FULL},
                                                    {L"--tool=ocr", TM_OCR},        {L"--tool=qr", TM_QR},
                                                    {L"--tool=pick", TM_PICK},      {L"--tool=rec", TM_REC}};
        for (auto& t : tl) if (wcsstr(cmdLine, t.k)) { RunTool(t.m); break; }
    }

    int rc = RunLoop();

    g_mw.Stop();
    if (g_rec.Active()) {  // корректно дописываем MP4 перед выходом
        g_rec.Stop();
        for (int i = 0; i < 100 && g_rec.Active(); i++) Sleep(50);
    }
    g_dlw.Stop();
    g_wxs.Stop();
    g_cals.Stop();
    g_nw.Stop();
    for (auto* n : g_notifQ) delete n;
    g_notifQ.clear();
    delete g_notif;
    g_notif = nullptr;
    g_notifPic.Release();
    for (auto& it : g_shelf) it.icon.Release();
    if (g_dropTarget) { g_dropTarget->Release(); g_dropTarget = nullptr; }
    g_audioWatch.Stop();
    g_privWatch.Stop();
    g_clip.Close();
    g_viz.Stop();
    g_lyrSvc.Stop();
    g_audio.Release();
    g_llCur.Release(); g_llPrev.Release(); g_llXCur.Release(); g_llXPrev.Release();
    Marquee* mqs[] = {&g_mqTitle, &g_mqArtist, &g_mqPeekTitle, &g_mqPeekArtist, &g_mqSource};
    for (auto* m : mqs) m->Release();
    DiscardTarget();
    IDWriteTextFormat* fmts[] = {g_fTime,       g_fTitle,       g_fArtist, g_fSmall,   g_fBig,      g_fDate,
                                 g_fPeekTitle,  g_fPeekArtist,  g_fSource, g_fLyric,   g_fLyricBig, g_fLyricSmall,
                                 g_fToast,      g_fClip,        g_fSearch, g_fWBig, g_fPlayerTitle, g_fCmd, g_fQa};
    for (auto* f : fmts) if (f) f->Release();
    SafeRelease(g_dwf);
    SafeRelease(g_d2f);
    if (g_frameTimer) CloseHandle(g_frameTimer);
    OleUninitialize();
    if (single) CloseHandle(single);
    return rc;
}
