// log.cpp — см. log.h
#include "log.h"
#include "version.h"
#include <cstdarg>
#include <cstdio>

static std::wstring g_logPath, g_dataDir;
static CRITICAL_SECTION g_logCs;
static bool g_logOn = false;

std::wstring LogPath() { return g_logPath; }

void LogLine(const wchar_t* fmt, ...) {
    if (!g_logOn) return;
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf(line, 1200, L"%04d-%02d-%02d %02d:%02d:%02d  %ls\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    int u8 = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
    std::string out(u8, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, n, &out[0], u8, nullptr, nullptr);
    EnterCriticalSection(&g_logCs);
    // не больше ~512 КБ: старое уходит в .old
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024)
        MoveFileExW(g_logPath.c_str(), (g_logPath + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// --- падение: строка в журнал, мини-дамп и перезапуск (но не чаще раза в минуту — без бесконечного цикла)
typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
struct MDEI { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };

static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    // в каком модуле и по какому смещению — по этому видно, где ошибка
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE hm = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &hm))
        GetModuleFileNameW(hm, mod, MAX_PATH);
    const wchar_t* modName = wcsrchr(mod, L'\\') ? wcsrchr(mod, L'\\') + 1 : mod;
    LogLine(L"ПАДЕНИЕ: код 0x%08lX в %ls+0x%llX (версия %ls)", code, modName, (unsigned long long)((char*)addr - (char*)hm), ZEN_VERSION);
    if (HMODULE dh = LoadLibraryW(L"dbghelp.dll")) {
        if (auto fn = (MiniDumpWriteDump_t)GetProcAddress(dh, "MiniDumpWriteDump")) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t name[MAX_PATH];
            swprintf(name, MAX_PATH, L"%ls\\crash-%04d%02d%02d-%02d%02d%02d.dmp", g_dataDir.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond);
            HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MDEI ei = {GetCurrentThreadId(), ep, FALSE};
                fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0 /*MiniDumpNormal*/, &ei, nullptr, nullptr);
                CloseHandle(f);
            }
        }
    }
    // автоперезапуск
    ULONGLONG now = GetTickCount64() / 1000;
    DWORD last = 0, sz = sizeof(last);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\DerkIsland", L"LastCrash", RRF_RT_REG_DWORD, nullptr, &last, &sz);
    DWORD nowD = (DWORD)now;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\DerkIsland", L"LastCrash", REG_DWORD, &nowD, sizeof(nowD));
    if (last == 0 || nowD < last || nowD - last > 60) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        wchar_t cmd[MAX_PATH + 64];
        swprintf(cmd, MAX_PATH + 64, L"\"%ls\" --after %lu --crashed", self, GetCurrentProcessId());
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LogLine(L"Перезапускаюсь после падения");
        }
    } else LogLine(L"Второе падение за минуту — не перезапускаюсь");
    return EXCEPTION_EXECUTE_HANDLER;
}

void LogInit(const std::wstring& dataDir) {
    InitializeCriticalSection(&g_logCs);
    g_dataDir = dataDir;
    g_logPath = dataDir + L"\\zenisland.log";
    g_logOn = true;
    SetUnhandledExceptionFilter(OnCrash);
    // версия Windows — без устаревшего GetVersionEx: берём из реестра
    wchar_t build[64] = L"?", disp[64] = L"";
    DWORD sz = sizeof(build);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", RRF_RT_REG_SZ, nullptr, build, &sz);
    sz = sizeof(disp);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", RRF_RT_REG_SZ, nullptr, disp, &sz);
    LogLine(L"Запуск Zen Island %ls · Windows сборка %ls %ls", ZEN_VERSION, build, disp);
}
