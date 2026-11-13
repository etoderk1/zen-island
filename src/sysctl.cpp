// sysctl.cpp
#include "sysctl.h"
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <mmdeviceapi.h>
#include <wlanapi.h>
#include <powrprof.h>

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Windows.Devices.Radios — сырой ABI
// ---------------------------------------------------------------------------
struct IAsyncInfoX : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncOpX : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void*) = 0;
};
struct IRadioStaticsX : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetRadiosAsync(IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceSelector(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FromIdAsync(HSTRING, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE RequestAccessAsync(IInspectable** op) = 0;
};
struct IRadioX : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE SetStateAsync(int state, IInspectable** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_StateChanged(void*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_StateChanged(INT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_State(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Name(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Kind(int*) = 0;
};
struct IVectorViewX : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32, IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32*) = 0;
};
static const IID kAsyncInfo = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const IID kRadioStatics = {0x5fb6a12e, 0x67cb, 0x46ae, {0xaa, 0xe9, 0x65, 0x91, 0x9f, 0x86, 0xef, 0xf4}};
static const IID kRadio = {0x252118df, 0xb33e, 0x416a, {0x87, 0x5f, 0x1c, 0xf3, 0x8a, 0xe2, 0xd8, 0x3e}};

template <class T> static bool Await(IInspectable* op, T* out, DWORD ms) {
    IAsyncInfoX* info = nullptr;
    if (!op || FAILED(op->QueryInterface(kAsyncInfo, (void**)&info))) return false;
    int st = 0;
    DWORD t0 = GetTickCount();
    while (SUCCEEDED(info->get_Status(&st)) && st == 0) {
        if (GetTickCount() - t0 > ms) { info->Cancel(); break; }
        Sleep(5);
    }
    info->Release();
    return st == 1 && (!out || SUCCEEDED(((IAsyncOpX*)op)->GetResults(out)));
}

// Выполнить fn(радио нужного типа) в отдельном MTA-потоке
template <class F> static bool WithRadio(int kind, F fn) {
    struct Ctx { int kind; F* fn; bool found; } ctx = {kind, &fn, false};
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        Ctx* c = (Ctx*)p;
        RoInitialize(RO_INIT_MULTITHREADED);
        HSTRING cls = nullptr;
        const wchar_t* name = L"Windows.Devices.Radios.Radio";
        WindowsCreateString(name, (UINT32)wcslen(name), &cls);
        IRadioStaticsX* st = nullptr;
        if (SUCCEEDED(RoGetActivationFactory(cls, kRadioStatics, (void**)&st)) && st) {
            IInspectable* op = nullptr;
            if (SUCCEEDED(st->RequestAccessAsync(&op)) && op) { int acc = 0; Await(op, &acc, 3000); op->Release(); }
            op = nullptr;
            IInspectable* list = nullptr;
            if (SUCCEEDED(st->GetRadiosAsync(&op)) && op && Await(op, &list, 4000) && list) {
                IVectorViewX* v = (IVectorViewX*)list;
                UINT32 n = 0;
                v->get_Size(&n);
                for (UINT32 i = 0; i < n && !c->found; i++) {
                    IInspectable* ri = nullptr;
                    if (FAILED(v->GetAt(i, &ri)) || !ri) continue;
                    IRadioX* r = nullptr;
                    if (SUCCEEDED(ri->QueryInterface(kRadio, (void**)&r)) && r) {
                        int k = -1;
                        r->get_Kind(&k);
                        if (k == c->kind) { c->found = true; (*c->fn)(r); }
                        r->Release();
                    }
                    ri->Release();
                }
                list->Release();
            }
            if (op) op->Release();
            st->Release();
        }
        WindowsDeleteString(cls);
        RoUninitialize();
        return 0;
    }, &ctx, 0, nullptr);
    if (th) { WaitForSingleObject(th, 8000); CloseHandle(th); }
    return ctx.found;
}

int RadioState(int kind) {
    int state = -1;
    WithRadio(kind, [&](IRadioX* r) { int s = 0; r->get_State(&s); state = s == 1 ? 1 : 0; });
    return state;
}
bool SetRadio(int kind, bool on) {
    bool ok = false;
    WithRadio(kind, [&](IRadioX* r) {
        IInspectable* op = nullptr;
        if (SUCCEEDED(r->SetStateAsync(on ? 1 : 2, &op)) && op) {
            int res = 0;
            ok = Await(op, &res, 5000) && res == 1;  // RadioAccessStatus::Allowed
            op->Release();
        }
    });
    return ok;
}

// ---------------------------------------------------------------------------
// Тема
// ---------------------------------------------------------------------------
static const wchar_t* kPers = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
bool IsDarkTheme() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kPers, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}
void SetDarkTheme(bool dark) {
    DWORD v = dark ? 0 : 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPers, L"AppsUseLightTheme", REG_DWORD, &v, sizeof(v));
    RegSetKeyValueW(HKEY_CURRENT_USER, kPers, L"SystemUsesLightTheme", REG_DWORD, &v, sizeof(v));
    DWORD_PTR res;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200, &res);
}

// ---------------------------------------------------------------------------
// Устройства вывода звука + смена устройства по умолчанию (IPolicyConfig — так делает и сама Windows)
// ---------------------------------------------------------------------------
static const CLSID kMMDevEnum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIMMDevEnum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const PROPERTYKEY kPK_Friendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
static const CLSID kPolicyClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID kIPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
struct IPolicyConfigX : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

std::vector<AudioDev> ListOutputDevices() { return ListAudioDevices(false); }
std::vector<AudioDev> ListAudioDevices(bool capture) {
    std::vector<AudioDev> v;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(kMMDevEnum, nullptr, CLSCTX_ALL, kIMMDevEnum, (void**)&en)) || !en) return v;
    std::wstring defId;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, &d)) && d) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
        d->Release();
    }
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev)) || !dev) continue;
            AudioDev a;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) { a.id = id; CoTaskMemFree(id); }
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(kPK_Friendly, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) a.name = pv.pwszVal;
                PropVariantClear(&pv);
                ps->Release();
            }
            a.isDefault = a.id == defId;
            if (!a.id.empty()) v.push_back(a);
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    return v;
}
bool SetDefaultOutput(const std::wstring& id) {
    IPolicyConfigX* pc = nullptr;
    if (FAILED(CoCreateInstance(kPolicyClient, nullptr, CLSCTX_ALL, kIPolicyConfig, (void**)&pc)) || !pc) return false;
    bool ok = SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), eConsole));
    pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    pc->SetDefaultEndpoint(id.c_str(), eCommunications);
    pc->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Wi-Fi: имя сети (wlanapi грузим динамически — на ПК без Wi-Fi службы может не быть)
// ---------------------------------------------------------------------------
std::wstring WifiName() {
    std::wstring r;
    HMODULE m = LoadLibraryW(L"wlanapi.dll");
    if (!m) return r;
    auto Open = (DWORD(WINAPI*)(DWORD, PVOID, PDWORD, PHANDLE))GetProcAddress(m, "WlanOpenHandle");
    auto Close = (DWORD(WINAPI*)(HANDLE, PVOID))GetProcAddress(m, "WlanCloseHandle");
    auto Enum = (DWORD(WINAPI*)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*))GetProcAddress(m, "WlanEnumInterfaces");
    auto Query = (DWORD(WINAPI*)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE))GetProcAddress(m, "WlanQueryInterface");
    auto Free = (VOID(WINAPI*)(PVOID))GetProcAddress(m, "WlanFreeMemory");
    HANDLE h = nullptr;
    DWORD ver;
    if (Open && Close && Enum && Query && Free && Open(2, nullptr, &ver, &h) == 0) {
        PWLAN_INTERFACE_INFO_LIST list = nullptr;
        if (Enum(h, nullptr, &list) == 0 && list) {
            for (DWORD i = 0; i < list->dwNumberOfItems && r.empty(); i++) {
                if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
                PWLAN_CONNECTION_ATTRIBUTES ca = nullptr;
                DWORD sz = 0;
                if (Query(h, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, nullptr) == 0 && ca) {
                    DOT11_SSID& s = ca->wlanAssociationAttributes.dot11Ssid;
                    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, nullptr, 0);
                    r.assign(n, L'\0');
                    if (n) MultiByteToWideChar(CP_UTF8, 0, (const char*)s.ucSSID, (int)s.uSSIDLength, &r[0], n);
                    Free(ca);
                }
            }
            Free(list);
        }
        Close(h, nullptr);
    }
    FreeLibrary(m);
    return r;
}

// ---------------------------------------------------------------------------
// Питание
// ---------------------------------------------------------------------------
static void EnableShutdownPrivilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp = {};
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
}
void PowerAction(int what) {
    switch (what) {
        case 0: LockWorkStation(); break;
        case 1: {
            typedef BOOLEAN(WINAPI * SuspendFn)(BOOLEAN, BOOLEAN, BOOLEAN);
            if (HMODULE m = LoadLibraryW(L"powrprof.dll")) {
                if (auto f = (SuspendFn)GetProcAddress(m, "SetSuspendState")) f(FALSE, FALSE, FALSE);
                FreeLibrary(m);
            }
            break;
        }
        case 2: EnableShutdownPrivilege(); ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_FLAG_PLANNED); break;
        case 3: EnableShutdownPrivilege(); ExitWindowsEx(EWX_POWEROFF | EWX_HYBRID_SHUTDOWN, SHTDN_REASON_FLAG_PLANNED); break;
    }
}
