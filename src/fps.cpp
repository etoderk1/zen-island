// fps.cpp — см. fps.h
#define INITGUID
#include "fps.h"
#include <evntrace.h>
#include <evntcons.h>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace {
// {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9} Microsoft-Windows-DXGI, Present_Start = 42
const GUID kDxgi = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
// {783ACA0A-790E-4D7F-8451-AA850511C6B9} Microsoft-Windows-D3D9, Present_Start = 1
const GUID kD3d9 = {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};
// {802EC45A-1E99-4B83-9920-87C98277BA9D} Microsoft-Windows-DxgKrnl, Present_Info = 184 (ловит OpenGL/Vulkan)
const GUID kDxgKrnl = {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};

const wchar_t kName[] = L"DynamicIslandFps";

struct Cnt { unsigned n[3] = {0, 0, 0}; };
CRITICAL_SECTION g_cs;
bool g_csInit = false;
std::unordered_map<DWORD, Cnt> g_cnt;
ULONGLONG g_lastTake = 0;
TRACEHANDLE g_session = 0, g_trace = INVALID_PROCESSTRACE_HANDLE;
HANDLE g_thread = nullptr;
volatile bool g_running = false;
std::vector<BYTE> g_propsBuf;

EVENT_TRACE_PROPERTIES* Props() {
    g_propsBuf.assign(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kName) + 16, 0);
    auto* p = (EVENT_TRACE_PROPERTIES*)g_propsBuf.data();
    p->Wnode.BufferSize = (ULONG)g_propsBuf.size();
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = 64;
    p->MinimumBuffers = 4;
    p->MaximumBuffers = 16;
    p->FlushTimer = 1;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return p;
}

void WINAPI OnEvent(PEVENT_RECORD e) {
    const GUID& g = e->EventHeader.ProviderId;
    USHORT id = e->EventHeader.EventDescriptor.Id;
    int src = -1;
    if (id == 42 && IsEqualGUID(g, kDxgi)) src = 0;
    else if (id == 1 && IsEqualGUID(g, kD3d9)) src = 1;
    else if (id == 184 && IsEqualGUID(g, kDxgKrnl)) src = 2;
    if (src < 0) return;
    EnterCriticalSection(&g_cs);
    if (g_cnt.size() < 512) g_cnt[e->EventHeader.ProcessId].n[src]++;
    LeaveCriticalSection(&g_cs);
}

DWORD WINAPI TraceThread(void*) {
    ProcessTrace(&g_trace, 1, nullptr, nullptr);  // блокирует до остановки сессии
    return 0;
}
}  // namespace

bool FpsRunning() { return g_running; }

bool FpsStart() {
    if (g_running) return true;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    ControlTraceW(0, kName, Props(), EVENT_TRACE_CONTROL_STOP);  // хвост от прошлого запуска
    ULONG st = StartTraceW(&g_session, kName, Props());
    if (st != ERROR_SUCCESS) { g_session = 0; return false; }
    EnableTraceEx2(g_session, &kDxgi, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0x2, 0, 0, nullptr);
    EnableTraceEx2(g_session, &kD3d9, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0x2, 0, 0, nullptr);
    EnableTraceEx2(g_session, &kDxgKrnl, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, 0x8000001, 0, 0, nullptr);
    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = (LPWSTR)kName;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = OnEvent;
    g_trace = OpenTraceW(&lf);
    if (g_trace == INVALID_PROCESSTRACE_HANDLE) { FpsStop(); return false; }
    g_thread = CreateThread(nullptr, 0, TraceThread, nullptr, 0, nullptr);
    if (!g_thread) { FpsStop(); return false; }
    SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    g_running = true;
    g_lastTake = GetTickCount64();
    return true;
}

void FpsStop() {
    if (g_session) {
        ControlTraceW(g_session, nullptr, Props(), EVENT_TRACE_CONTROL_STOP);
        g_session = 0;
    }
    if (g_trace != INVALID_PROCESSTRACE_HANDLE) { CloseTrace(g_trace); g_trace = INVALID_PROCESSTRACE_HANDLE; }
    if (g_thread) { WaitForSingleObject(g_thread, 1500); CloseHandle(g_thread); g_thread = nullptr; }
    g_running = false;
    if (g_csInit) { EnterCriticalSection(&g_cs); g_cnt.clear(); LeaveCriticalSection(&g_cs); }
}

float FpsTake(DWORD pid) {
    if (!g_running) return -1;
    ULONGLONG now = GetTickCount64();
    double dt = (now - g_lastTake) / 1000.0;
    g_lastTake = now;
    unsigned best = 0;
    EnterCriticalSection(&g_cs);
    auto it = g_cnt.find(pid);
    if (it != g_cnt.end()) best = std::max(std::max(it->second.n[0], it->second.n[1]), it->second.n[2]);
    g_cnt.clear();
    LeaveCriticalSection(&g_cs);
    if (dt < 0.2 || best == 0) return best == 0 ? -1.f : 0.f;
    return (float)(best / dt);
}
