// sysstats.cpp — CPU (GetSystemTimes), память, сеть (GetIfTable2), GPU (счётчики PDH «GPU Engine»)
#include "sysstats.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <string>
#include <vector>
#include <cwchar>

namespace {
// PDH грузим динамически — без лишних зависимостей
typedef LONG(WINAPI* PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, void**);
typedef LONG(WINAPI* PdhAddEnglishCounterW_t)(void*, LPCWSTR, DWORD_PTR, void**);
typedef LONG(WINAPI* PdhCollectQueryData_t)(void*);
typedef LONG(WINAPI* PdhCloseQuery_t)(void*);
struct PDH_FMT_COUNTERVALUE_ { DWORD CStatus; DWORD pad; union { LONG longValue; double doubleValue; LONGLONG largeValue; }; };
struct PDH_FMT_COUNTERVALUE_ITEM_W_ { LPWSTR szName; PDH_FMT_COUNTERVALUE_ FmtValue; };
typedef LONG(WINAPI* PdhGetFormattedCounterArrayW_t)(void*, DWORD, DWORD*, DWORD*, PDH_FMT_COUNTERVALUE_ITEM_W_*);
struct Pdh {
    HMODULE m = nullptr;
    PdhOpenQueryW_t open = nullptr;
    PdhAddEnglishCounterW_t add = nullptr;
    PdhCollectQueryData_t collect = nullptr;
    PdhCloseQuery_t close = nullptr;
    PdhGetFormattedCounterArrayW_t arr = nullptr;
    bool Load() {
        if (m) return open != nullptr;
        m = LoadLibraryW(L"pdh.dll");
        if (!m) return false;
        open = (PdhOpenQueryW_t)GetProcAddress(m, "PdhOpenQueryW");
        add = (PdhAddEnglishCounterW_t)GetProcAddress(m, "PdhAddEnglishCounterW");
        collect = (PdhCollectQueryData_t)GetProcAddress(m, "PdhCollectQueryData");
        close = (PdhCloseQuery_t)GetProcAddress(m, "PdhCloseQuery");
        arr = (PdhGetFormattedCounterArrayW_t)GetProcAddress(m, "PdhGetFormattedCounterArrayW");
        if (!open || !add || !collect || !close || !arr) { open = nullptr; return false; }
        return true;
    }
} g_pdh;
ULONGLONG U(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }
}  // namespace

bool SysStats::Sample(SysSample& s) {
    FILETIME fi, fk, fu;
    GetSystemTimes(&fi, &fk, &fu);
    ULONGLONG idle = U(fi), kern = U(fk), user = U(fu);
    MEMORYSTATUSEX ms = {sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    s.ram = (float)ms.dwMemoryLoad;
    s.ramTotalGb = ms.ullTotalPhys / 1073741824.0;
    s.ramUsedGb = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
    // сеть: сумма по физическим интерфейсам в состоянии «работает»
    ULONGLONG inB = 0, outB = 0;
    MIB_IF_TABLE2* t = nullptr;
    if (GetIfTable2(&t) == NO_ERROR && t) {
        for (ULONG i = 0; i < t->NumEntries; i++) {
            const MIB_IF_ROW2& r = t->Table[i];
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp) continue;
            if (!r.InterfaceAndOperStatusFlags.HardwareInterface) continue;
            if (r.InterfaceAndOperStatusFlags.FilterInterface) continue;
            inB += r.InOctets;
            outB += r.OutOctets;
        }
        FreeMibTable(t);
    }
    ULONGLONG now = GetTickCount64();
    // GPU
    if (!pdhTried_) {
        pdhTried_ = true;
        if (g_pdh.Load() && g_pdh.open(nullptr, 0, &pdhQuery_) == 0) {
            if (g_pdh.add(pdhQuery_, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &pdhCounter_) != 0) {
                g_pdh.close(pdhQuery_);
                pdhQuery_ = pdhCounter_ = nullptr;
            } else g_pdh.collect(pdhQuery_);
        }
    }
    s.gpu = -1;
    s.pidGpu = -1;
    if (pdhQuery_ && g_pdh.collect(pdhQuery_) == 0) {
        DWORD size = 0, count = 0;
        const DWORD FMT_DOUBLE = 0x00000200;
        LONG st = g_pdh.arr(pdhCounter_, FMT_DOUBLE, &size, &count, nullptr);
        if ((st == (LONG)0x800007D2 /*PDH_MORE_DATA*/ || st == 0) && size) {
            std::vector<BYTE> buf(size);
            if (g_pdh.arr(pdhCounter_, FMT_DOUBLE, &size, &count, (PDH_FMT_COUNTERVALUE_ITEM_W_*)buf.data()) == 0) {
                auto* items = (PDH_FMT_COUNTERVALUE_ITEM_W_*)buf.data();
                double sum = 0, mine = 0;
                wchar_t pref[32] = {};
                if (watchPid) swprintf(pref, 32, L"pid_%lu_", watchPid);
                size_t pl = wcslen(pref);
                for (DWORD i = 0; i < count; i++)
                    if (items[i].FmtValue.CStatus == 0) {
                        sum += items[i].FmtValue.doubleValue;
                        if (pl && items[i].szName && wcsncmp(items[i].szName, pref, pl) == 0) mine += items[i].FmtValue.doubleValue;
                    }
                s.gpu = (float)std::min(100.0, sum);
                if (watchPid) s.pidGpu = (float)std::min(100.0, mine);
            }
        }
    }
    bool ok = false;
    if (warm_) {
        ULONGLONG di = idle - idle_, dt = (kern - kern_) + (user - user_);
        s.cpu = dt ? (float)(100.0 * (double)(dt - di) / (double)dt) : 0.f;
        if (s.cpu < 0) s.cpu = 0;
        double sec = (now - tick_) / 1000.0;
        if (sec <= 0) sec = 1;
        s.down = inB >= inB_ ? (inB - inB_) / sec : 0;
        s.up = outB >= outB_ ? (outB - outB_) / sec : 0;
        hcpu[pos] = s.cpu;
        hgpu[pos] = s.gpu < 0 ? 0 : s.gpu;
        hdown[pos] = (float)s.down;
        hup[pos] = (float)s.up;
        pos = (pos + 1) % N;
        if (count < N) count++;
        ok = true;
    }
    idle_ = idle; kern_ = kern; user_ = user; inB_ = inB; outB_ = outB; tick_ = now;
    warm_ = true;
    return ok;
}
void SysStats::Close() {
    if (pdhQuery_ && g_pdh.close) g_pdh.close(pdhQuery_);
    pdhQuery_ = pdhCounter_ = nullptr;
    pdhTried_ = false;
    warm_ = false;
}

std::wstring FmtSpeed(double b) {
    wchar_t s[32];
    if (b < 1024) swprintf(s, 32, L"%.0f Б/с", b);
    else if (b < 1024 * 1024) swprintf(s, 32, L"%.0f КБ/с", b / 1024);
    else if (b < 100.0 * 1024 * 1024) swprintf(s, 32, L"%.1f МБ/с", b / 1048576);
    else swprintf(s, 32, L"%.0f МБ/с", b / 1048576);
    std::wstring r = s;
    for (auto& c : r) if (c == L'.') c = L',';
    return r;
}
