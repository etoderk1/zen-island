// cmdindex.h — фоновый список программ (меню «Пуск», включая магазинные) и файлов для командной строки
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

#define WM_APP_INDEX (WM_APP + 21)  // lParam = CmdIndexData*
#define WM_APP_RATES (WM_APP + 22)  // lParam = std::map<std::string,double>*

enum CmdKind { CE_APP, CE_FILE, CE_FOLDER };
struct CmdEntry {
    std::wstring name, lower;  // как показывать / для поиска
    std::wstring target;       // shell:AppsFolder\… или путь
    std::wstring sub;          // папка (для файлов)
    int kind = CE_APP;
    long long mtime = 0;       // для файлов: свежие выше
};
struct CmdIndexData { std::vector<CmdEntry> apps, files; };

// Строит индекс в отдельном потоке и присылает WM_APP_INDEX. Безопасно звать часто: лишние вызовы игнорируются.
void CmdIndexBuildAsync(HWND notify, const std::vector<std::wstring>& extraDirs);
// Курсы валют к доллару: кэш на диске (cacheFile), обновление раз в 6 часов. Присылает WM_APP_RATES.
void RatesFetchAsync(HWND notify, const std::wstring& cacheFile);
