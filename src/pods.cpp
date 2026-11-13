// pods.cpp — BluetoothLEAdvertisementWatcher через «сырой» WinRT ABI (без C++/WinRT)
#include "pods.h"
#include <roapi.h>
#include <winstring.h>
#include <atomic>
#include <thread>
#include <mutex>

// ---------------------------------------------------------------- разбор пакета
// Пакет «proximity pairing» (тип 0x07, длина 0x19). Раскладка по OpenPods / AirPodsDesktop.
bool ParseApplePods(const BYTE* d, size_t n, PodsInfo& o) {
    if (n < 9 || d[0] != 0x07) return false;
    if (d[1] < 0x0B) return false;
    int model = (d[3] << 8) | d[4];
    struct { int id; const wchar_t* name; bool single; } models[] = {
        {0x0220, L"AirPods", false},          {0x0F20, L"AirPods 2", false},       {0x1320, L"AirPods 3", false},
        {0x1920, L"AirPods 4", false},        {0x1B20, L"AirPods 4", false},       {0x0E20, L"AirPods Pro", false},
        {0x1420, L"AirPods Pro 2", false},    {0x2420, L"AirPods Pro 2", false},   {0x0A20, L"AirPods Max", true},
        {0x1F20, L"AirPods Max", true},       {0x0320, L"Powerbeats 3", true},     {0x0B20, L"Powerbeats Pro", false},
        {0x0C20, L"Beats Solo Pro", true},    {0x1120, L"Beats Studio Buds", false}, {0x1020, L"Beats Flex", true},
        {0x0520, L"BeatsX", true},            {0x0620, L"Beats Solo 3", true},     {0x0920, L"Beats Studio 3", true},
        {0x1720, L"Beats Studio Pro", true},  {0x1220, L"Beats Fit Pro", false},   {0x1620, L"Beats Studio Buds+", false},
    };
    o.model = L"AirPods";
    o.single = false;
    bool known = false;
    for (auto& m : models)
        if (m.id == model) { o.model = m.name; o.single = m.single; known = true; break; }
    if (!known && (d[3] & 0xF0) != 0 && d[4] != 0x20) return false;  // похоже, не наушники
    int status = d[5];
    bool flip = (((status >> 4) >> 1) & 1) == 0;  // какой наушник «главный»
    int hi6 = d[6] >> 4, lo6 = d[6] & 0x0F, hi7 = d[7] >> 4, lo7 = d[7] & 0x0F;
    auto pct = [](int v) { return v == 15 ? -1 : (v > 10 ? -1 : std::min(100, v * 10)); };
    if (o.single) {
        o.left = pct(lo6);
        o.chL = (hi7 & 1) != 0;
    } else {
        o.left = pct(flip ? hi6 : lo6);
        o.right = pct(flip ? lo6 : hi6);
        o.box = pct(lo7);
        o.chL = (hi7 & (flip ? 0b10 : 0b01)) != 0;
        o.chR = (hi7 & (flip ? 0b01 : 0b10)) != 0;
        o.chBox = (hi7 & 0b100) != 0;
    }
    return o.left >= 0 || o.right >= 0 || o.box >= 0;
}

// ---------------------------------------------------------------- WinRT ABI
namespace {
struct EventToken { INT64 value; };
struct IInspectableRaw : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTrustLevel(int*) = 0;
};
struct IWatcher : IInspectableRaw {
    virtual HRESULT STDMETHODCALLTYPE get_MinSamplingInterval(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxSamplingInterval(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MinOutOfRangeTimeout(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_MaxOutOfRangeTimeout(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ScanningMode(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ScanningMode(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SignalStrengthFilter(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SignalStrengthFilter(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AdvertisementFilter(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_AdvertisementFilter(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Received(IUnknown*, EventToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Received(EventToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Stopped(IUnknown*, EventToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Stopped(EventToken) = 0;
};
struct IRecvArgs : IInspectableRaw {
    virtual HRESULT STDMETHODCALLTYPE get_RawSignalStrengthInDBm(INT16*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BluetoothAddress(UINT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AdvertisementType(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Timestamp(INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Advertisement(IInspectableRaw**) = 0;
};
struct IAdvertisement : IInspectableRaw {
    virtual HRESULT STDMETHODCALLTYPE get_Flags(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Flags(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_LocalName(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_LocalName(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ServiceUuids(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ManufacturerData(IInspectableRaw**) = 0;
};
struct IVectorRaw : IInspectableRaw {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectableRaw**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};
struct IManufData : IInspectableRaw {
    virtual HRESULT STDMETHODCALLTYPE get_CompanyId(UINT16*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_CompanyId(UINT16) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Data(IInspectableRaw**) = 0;
};
struct IBufferRaw : IInspectableRaw {
    virtual HRESULT STDMETHODCALLTYPE get_Capacity(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Length(UINT32*) = 0;
};
struct IBufferByteAccessRaw : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Buffer(BYTE**) = 0;
};
const IID IID_Watcher = {0xa6ac336f, 0xf3d3, 0x4297, {0x8d, 0x6c, 0xc8, 0x1e, 0xa6, 0x62, 0x3f, 0x40}};
const IID IID_RecvArgs = {0x27987ddf, 0xe596, 0x41be, {0x8d, 0x43, 0x9e, 0x67, 0x31, 0xd4, 0xa9, 0x13}};
const IID IID_Adv = {0x066fb2b7, 0x33d1, 0x4e7d, {0x83, 0x67, 0xcf, 0x81, 0xd0, 0xf7, 0x96, 0x53}};
const IID IID_Manuf = {0x912dba18, 0x6963, 0x4533, {0xb0, 0x61, 0x46, 0x94, 0xda, 0xfb, 0x34, 0xe5}};
const IID IID_ByteAccess = {0x905a0fef, 0xbc53, 0x11df, {0x8c, 0x49, 0x00, 0x1e, 0x4f, 0xc6, 0x86, 0xda}};
// TypedEventHandler<BluetoothLEAdvertisementWatcher, BluetoothLEAdvertisementReceivedEventArgs>
const IID IID_RecvHandler = {0x90eb4eca, 0xd465, 0x5ea0, {0xa6, 0x1c, 0x03, 0x3c, 0x8c, 0x5e, 0xce, 0xf2}};
const IID IID_Agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

std::atomic<bool> g_scan{false}, g_stopReq{false};
HANDLE g_stopEv = nullptr;
std::mutex g_mx;
PodsInfo g_best;
bool g_have = false;

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Handler : IUnknown {
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == IID_IUnknown || r == IID_RecvHandler || r == IID_Agile) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(IUnknown* sender, IUnknown* argsU) {
        (void)sender;
        IRecvArgs* args = nullptr;
        if (!argsU || FAILED(argsU->QueryInterface(IID_RecvArgs, (void**)&args)) || !args) return S_OK;
        INT16 rssi = -127;
        args->get_RawSignalStrengthInDBm(&rssi);
        IInspectableRaw* advI = nullptr;
        IAdvertisement* adv = nullptr;
        if (rssi >= -72 && SUCCEEDED(args->get_Advertisement(&advI)) && advI &&
            SUCCEEDED(advI->QueryInterface(IID_Adv, (void**)&adv)) && adv) {
            IInspectableRaw* vecI = nullptr;
            if (SUCCEEDED(adv->get_ManufacturerData(&vecI)) && vecI) {
                IVectorRaw* vec = (IVectorRaw*)vecI;  // IVector<T>: GetAt и Size — первые методы
                UINT32 n = 0;
                vec->get_Size(&n);
                for (UINT32 i = 0; i < n && i < 8; i++) {
                    IInspectableRaw* mI = nullptr;
                    IManufData* m = nullptr;
                    if (FAILED(vec->GetAt(i, &mI)) || !mI) continue;
                    if (SUCCEEDED(mI->QueryInterface(IID_Manuf, (void**)&m)) && m) {
                        UINT16 cid = 0;
                        m->get_CompanyId(&cid);
                        IInspectableRaw* bufI = nullptr;
                        if (cid == 0x004C && SUCCEEDED(m->get_Data(&bufI)) && bufI) {
                            UINT32 len = 0;
                            ((IBufferRaw*)bufI)->get_Length(&len);
                            IBufferByteAccessRaw* ba = nullptr;
                            BYTE* bytes = nullptr;
                            if (SUCCEEDED(bufI->QueryInterface(IID_ByteAccess, (void**)&ba)) && ba && SUCCEEDED(ba->Buffer(&bytes)) && bytes) {
                                PodsInfo info;
                                if (ParseApplePods(bytes, len, info)) {
                                    info.rssi = rssi;
                                    std::lock_guard<std::mutex> lk(g_mx);
                                    if (!g_have || info.rssi >= g_best.rssi - 4) { g_best = info; g_have = true; }
                                }
                            }
                            Rel(ba);
                            Rel(bufI);
                        }
                        Rel(m);
                    }
                    Rel(mI);
                }
                Rel(vecI);
            }
        }
        Rel(adv);
        Rel(advI);
        Rel(args);
        return S_OK;
    }
};

void Run(HWND notify, int seconds) {
    RoInitialize(RO_INIT_MULTITHREADED);
    HSTRING cls = nullptr;
    const wchar_t* name = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
    WindowsCreateString(name, (UINT32)wcslen(name), &cls);
    IInspectable* insp = nullptr;
    IWatcher* w = nullptr;
    Handler* h = nullptr;
    EventToken tok = {};
    if (SUCCEEDED(RoActivateInstance(cls, &insp)) && insp && SUCCEEDED(insp->QueryInterface(IID_Watcher, (void**)&w)) && w) {
        w->put_ScanningMode(1);  // Active — так приходят ответы на сканирование
        h = new Handler;
        if (SUCCEEDED(w->add_Received(h, &tok)) && SUCCEEDED(w->Start())) {
            DWORD until = GetTickCount() + seconds * 1000;
            while (!g_stopReq) {
                DWORD left = until > GetTickCount() ? until - GetTickCount() : 0;
                if (!left) break;
                WaitForSingleObject(g_stopEv, std::min<DWORD>(left, 1000));
                PodsInfo* out = nullptr;
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    if (g_have) { out = new PodsInfo(g_best); g_have = false; g_best = PodsInfo{}; }
                }
                if (out && !PostMessageW(notify, WM_APP_PODS, 0, (LPARAM)out)) delete out;
            }
            w->Stop();
        }
        w->remove_Received(tok);
    }
    Rel(h);
    Rel(w);
    if (insp) insp->Release();
    if (cls) WindowsDeleteString(cls);
    RoUninitialize();
}
}  // namespace

void PodsScanStart(HWND notify, int seconds) {
    if (g_scan.exchange(true)) return;
    if (!g_stopEv) g_stopEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopReq = false;
    ResetEvent(g_stopEv);
    std::thread([notify, seconds] {
        Run(notify, seconds);
        g_scan = false;
    }).detach();
}
void PodsScanStop() {
    g_stopReq = true;
    if (g_stopEv) SetEvent(g_stopEv);
}
bool PodsScanning() { return g_scan; }
