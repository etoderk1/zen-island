// sysstats.h — загрузка CPU/GPU, память и скорость сети. Замер раз в секунду и только когда это видно.
#pragma once
#include <windows.h>
#include <string>

struct SysSample {
    float cpu = 0, gpu = -1, ram = 0;  // %; gpu -1 — не удалось узнать
    double down = 0, up = 0;           // байт/с
    double ramUsedGb = 0, ramTotalGb = 0;
    float pidGpu = -1;                 // 3D-загрузка видеокарты процессом watchPid, %
};

class SysStats {
public:
    // Возвращает true, если в s свежие данные (первый замер — только «прогрев»).
    bool Sample(SysSample& s);
    void Close();  // освободить PDH (когда виджет скрыт)
    static const int N = 60;
    float hcpu[N] = {}, hgpu[N] = {}, hdown[N] = {}, hup[N] = {};  // история для графиков
    int pos = 0, count = 0;
    DWORD watchPid = 0;  // чью загрузку GPU считать отдельно (проверка «это игра?»)

private:
    ULONGLONG idle_ = 0, kern_ = 0, user_ = 0, inB_ = 0, outB_ = 0, tick_ = 0;
    bool warm_ = false;
    void* pdhQuery_ = nullptr;
    void* pdhCounter_ = nullptr;
    bool pdhTried_ = false;
};

std::wstring FmtSpeed(double bytesPerSec);
