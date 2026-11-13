// downloads.h — слежение за папкой «Загрузки»: прогресс (.crdownload/.part) и завершение
#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <vector>

#define WM_APP_DL (WM_APP + 9)  // wParam = DL_PROGRESS / DL_DONE, lParam = DlInfo*

enum { DL_PROGRESS = 1, DL_DONE = 2 };

struct DlInfo {
    int active = 0;            // сколько загрузок идёт
    long long bytes = 0;       // скачано (по всем)
    double speed = 0;          // байт/с
    std::wstring name;         // имя файла (для DL_DONE — итоговое)
    std::wstring path;         // полный путь (DL_DONE)
};

class DownloadWatcher {
public:
    void Start(HWND notify, const std::wstring& dir);  // dir — главная папка; ещё смотрим папки браузеров и TEMP
    void Stop();
    const std::wstring& Dir() const { return dir_; }

private:
    void Run();
    HWND notify_ = nullptr;
    std::wstring dir_;
    std::vector<std::wstring> dirs_;
    HANDLE stopEv_ = nullptr;
    std::thread th_;
};

std::wstring DownloadsFolder();
