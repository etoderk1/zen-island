// fps.h — счётчик кадров в играх через ETW (как PresentMon): события Present от DXGI, D3D9 и ядра DirectX.
// Нужны права администратора (или группа «Пользователи журналов производительности»).
#pragma once
#include <windows.h>

bool FpsStart();             // запустить сессию; false — нет прав или ошибка
void FpsStop();
bool FpsRunning();
// Кадров в секунду для процесса pid с прошлого вызова; -1 — нет данных. Сбрасывает счётчики.
float FpsTake(DWORD pid);
