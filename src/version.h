// version.h — одна версия на всё: остров, установщик, проверка обновлений
#pragma once
#define ZEN_VERSION L"1.8.0"
#define ZEN_VERSION_A "1.8.0"
#define ZEN_VERSION_RC 1,8,0,0
// откуда брать обновления: «владелец/репозиторий» на GitHub (пусто — пока не выложено).
// Можно переопределить без пересборки: реестр HKCU\Software\DerkIsland, строка UpdateRepo.
#define ZEN_REPO L"etoderk1/zen-island"
