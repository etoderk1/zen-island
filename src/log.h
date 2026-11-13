// log.h — журнал ошибок (data\zenisland.log) и обработка падений: дамп + автоперезапуск
#pragma once
#include <windows.h>
#include <string>

void LogInit(const std::wstring& dataDir);  // включить журнал и ловушку падений
void LogLine(const wchar_t* fmt, ...);      // строка с временем; безопасно из любого потока
std::wstring LogPath();
