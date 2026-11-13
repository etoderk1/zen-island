// update.h — автообновление: последний релиз на GitHub → скачать ZenIsland-Setup.exe → тихо поставить
#pragma once
#include <windows.h>
#include <string>

#define WM_APP_UPDATE (WM_APP + 28)  // wParam: 1 — есть новая версия (lParam = UpdateInfo*), 0 — нет/ошибка (lParam = UpdateInfo* или 0)
#define WM_APP_UPDDL (WM_APP + 29)   // wParam: процент 0..100; 101 — скачано (lParam = std::wstring* путь); 102 — ошибка

struct UpdateInfo {
    std::wstring version;  // «1.9.0»
    std::wstring url;      // ссылка на ZenIsland-Setup.exe
    std::wstring notes;    // первые строки описания релиза
    std::wstring error;
};

// repo — «владелец/репозиторий». manual — проверка по кнопке (сообщить и «нет обновлений»).
void UpdateCheckAsync(HWND notify, const std::wstring& repo, bool manual);
void UpdateDownloadAsync(HWND notify, const std::wstring& url);
bool VersionNewer(const std::wstring& a, const std::wstring& b);  // a > b?
