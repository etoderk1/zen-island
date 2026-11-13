// media.h — фоновый поток, который читает «что сейчас играет» из Windows
// (GlobalSystemMediaTransportControls: Spotify, Яндекс Музыка, браузеры и т.д.)
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

#define WM_APP_MEDIA (WM_APP + 1)

enum MediaCmd { CMD_NONE = 0, CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_SEEK };

// PlaybackStatus: 0 Closed, 1 Opened, 2 Changing, 3 Stopped, 4 Playing, 5 Paused
struct MediaSnap {
    bool has = false;
    std::wstring title, artist, album;
    int status = 0;
    INT64 start = 0, end = 0, pos = 0, lastUpd = 0;  // 100 нс единицы
    bool canNext = true, canPrev = true;
    std::vector<BYTE> art;                         // 32bpp PBGRA
    int artW = 0, artH = 0;
    int artSrcW = 0, artSrcH = 0;  // исходный размер обложки (по нему отличаем ролик 16:9 от квадратной обложки трека)
    UINT artVer = 0;
    D2D1_COLOR_F accent = {0.56f, 0.50f, 1.0f, 1.0f};
    // Приложение-источник
    std::wstring appId, appName;
    std::vector<BYTE> appIcon;  // 32bpp PBGRA
    int appIconW = 0, appIconH = 0;
    UINT appVer = 0;
};

class MediaWatcher {
public:
    void Start(HWND notify);
    void Stop();
    void Command(int c);
    void Seek(INT64 absolutePos);  // в единицах таймлайна (100 нс)
    void SetInterval(int ms);
    MediaSnap Get();

private:
    void Run();
    HWND notify_ = nullptr;
    HANDLE wake_ = nullptr;
    std::thread th_;
    std::mutex mx_;
    MediaSnap snap_;
    std::atomic<int> cmd_{0};
    std::atomic<long long> seekPos_{0};
    std::atomic<int> interval_{1000};
    std::atomic<bool> quit_{false};
};

D2D1_COLOR_F DefaultAccent();

// Имя и иконка приложения по AppUserModelId (Spotify.exe, Chrome, ...). Блокирующая.
void ResolveAppInfo(const std::wstring& id, std::wstring& name, std::vector<BYTE>& icon, int& w, int& h);
