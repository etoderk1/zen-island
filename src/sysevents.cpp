// sysevents.cpp — системные события для острова
#include "sysevents.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <cwctype>
#include <algorithm>
#include <vector>

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static const CLSID kCLSID_MMDevEnum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_MMDevEnum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_EndpointVol = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};
static const IID kIID_VolCb = {0x657804FA, 0xD6AD, 0x4496, {0x8A, 0x60, 0x35, 0x27, 0x52, 0xAF, 0x4F, 0x89}};
static const IID kIID_NotifyClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
static const PROPERTYKEY kPK_IfaceFriendly = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
static const PROPERTYKEY kPK_DevFriendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// Уровень заряда Bluetooth-устройства (то же, что показывает «Параметры → Bluetooth»)
static const DEVPROPKEY kDK_BtBattery = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());  // понимает кириллицу, в отличие от towlower
    return s;
}

// ---------------------------------------------------------------------------
// COM-обработчики
// ---------------------------------------------------------------------------
class VolCb : public IAudioEndpointVolumeCallback {
    LONG ref_ = 1;
    HWND n_;
public:
    explicit VolCb(HWND n) : n_(n) {}
    virtual ~VolCb() {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, kIID_VolCb)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (d) {
            VolumeInfo* v = new VolumeInfo{d->fMasterVolume, d->bMuted != FALSE};
            if (!PostMessageW(n_, WM_APP_SYS, SE_VOLUME, (LPARAM)v)) delete v;
        }
        return S_OK;
    }
};

class DevNotify : public IMMNotificationClient {
    LONG ref_ = 1;
    HANDLE ev_;
public:
    explicit DevNotify(HANDLE ev) : ev_(ev) {}
    virtual ~DevNotify() {}
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** pp) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, kIID_NotifyClient)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) SetEvent(ev_);  // тут ничего тяжёлого — только будим поток
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

// ---------------------------------------------------------------------------
// Bluetooth: заряд по имени устройства
// ---------------------------------------------------------------------------
int BluetoothBattery(const std::wstring& name, bool* isBt) {
    if (isBt) *isBt = false;
    if (name.empty()) return -1;
    std::wstring want = Lower(name);
    int best = -1;
    HDEVINFO h = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return -1;
    SP_DEVINFO_DATA d;
    d.cbSize = sizeof(d);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(h, i, &d); i++) {
        wchar_t en[64] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_ENUMERATOR_NAME, nullptr, (PBYTE)en, sizeof(en) - 2, nullptr))
            continue;
        if (_wcsnicmp(en, L"BTH", 3) != 0) continue;
        wchar_t fn[256] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_FRIENDLYNAME, nullptr, (PBYTE)fn, sizeof(fn) - 2, nullptr) &&
            !SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_DEVICEDESC, nullptr, (PBYTE)fn, sizeof(fn) - 2, nullptr))
            continue;
        std::wstring f = Lower(fn);
        if (f.find(want) == std::wstring::npos && want.find(f) == std::wstring::npos) continue;
        if (isBt) *isBt = true;
        DEVPROPTYPE type = 0;
        BYTE val = 0;
        DWORD req = 0;
        if (SetupDiGetDevicePropertyW(h, &d, &kDK_BtBattery, &type, &val, sizeof(val), &req, 0) && type == DEVPROP_TYPE_BYTE)
            best = std::max(best, (int)val);
    }
    SetupDiDestroyDeviceInfoList(h);
    return best;
}

// ---------------------------------------------------------------------------
// AudioWatcher
// ---------------------------------------------------------------------------
void AudioWatcher::Start(HWND notify) {
    notify_ = notify;
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    changeEv_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void AudioWatcher::Stop() {
    if (!stopEv_) return;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    CloseHandle(stopEv_);
    CloseHandle(changeEv_);
    stopEv_ = changeEv_ = nullptr;
}

static std::wstring PropStr(IPropertyStore* ps, const PROPERTYKEY& k) {
    std::wstring r;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(k, &v)) && v.vt == VT_LPWSTR && v.pwszVal) r = v.pwszVal;
    PropVariantClear(&v);
    return r;
}

void AudioWatcher::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(kCLSID_MMDevEnum, nullptr, CLSCTX_ALL, kIID_MMDevEnum, (void**)&en);
    DevNotify* dn = new DevNotify(changeEv_);
    if (en) en->RegisterEndpointNotificationCallback(dn);
    VolCb* vc = new VolCb(notify_);
    IAudioEndpointVolume* vol = nullptr;

    auto bind = [&](bool announce) {
        if (vol) { vol->UnregisterControlChangeNotify(vc); Rel(vol); }
        if (!en) return;
        IMMDevice* d = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)) || !d) return;
        if (SUCCEEDED(d->Activate(kIID_EndpointVol, CLSCTX_ALL, nullptr, (void**)&vol)) && vol)
            vol->RegisterControlChangeNotify(vc);
        if (announce) {
            AudioDevInfo* info = new AudioDevInfo;
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                info->name = PropStr(ps, kPK_IfaceFriendly);
                if (info->name.empty()) info->name = PropStr(ps, kPK_DevFriendly);
                ps->Release();
            }
            info->battery = BluetoothBattery(info->name, &info->bluetooth);
            if (!PostMessageW(notify_, WM_APP_SYS, SE_AUDIODEV, (LPARAM)info)) delete info;
        }
        d->Release();
    };
    bind(false);

    HANDLE waits[2] = {stopEv_, changeEv_};
    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0 + 1) break;
        // Windows шлёт несколько событий подряд — дожидаемся, пока устаканится
        if (WaitForSingleObject(stopEv_, 400) == WAIT_OBJECT_0) break;
        ResetEvent(changeEv_);
        bind(true);
    }
    if (vol) { vol->UnregisterControlChangeNotify(vc); Rel(vol); }
    if (en) en->UnregisterEndpointNotificationCallback(dn);
    vc->Release();
    dn->Release();
    Rel(en);
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// PrivacyWatcher: HKCU\...\CapabilityAccessManager\ConsentStore\{microphone,webcam}
// Приложение использует устройство, если LastUsedTimeStart != 0 и LastUsedTimeStop == 0
// ---------------------------------------------------------------------------
void PrivacyWatcher::Start(HWND notify) {
    notify_ = notify;
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    th_ = std::thread([this] { Run(); });
}
void PrivacyWatcher::Stop() {
    if (!stopEv_) return;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    CloseHandle(stopEv_);
    stopEv_ = nullptr;
}

static bool InUse(HKEY k) {
    ULONGLONG start = 0, stop = 1;
    DWORD sz = sizeof(start);
    if (RegGetValueW(k, nullptr, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &sz) != ERROR_SUCCESS) return false;
    sz = sizeof(stop);
    if (RegGetValueW(k, nullptr, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &sz) != ERROR_SUCCESS) return false;
    return start != 0 && stop == 0;
}
static std::wstring PrettyApp(const std::wstring& key, bool nonPackaged) {
    std::wstring n = key;
    if (nonPackaged) {
        size_t p = n.find_last_of(L"#\\/");
        if (p != std::wstring::npos) n = n.substr(p + 1);
        if (n.size() > 4 && Lower(n.substr(n.size() - 4)) == L".exe") n.resize(n.size() - 4);
    } else {
        size_t u = n.find(L'_');
        if (u != std::wstring::npos) n = n.substr(0, u);
        size_t d = n.find_last_of(L'.');
        if (d != std::wstring::npos && d + 1 < n.size()) n = n.substr(d + 1);
        if (Lower(n) == L"windowscamera") n = L"Камера";
    }
    if (!n.empty()) CharUpperBuffW(&n[0], 1);
    return n;
}
static bool ScanStore(HKEY root, std::wstring& app) {
    wchar_t name[512];
    for (DWORD i = 0;; i++) {
        DWORD len = 512;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        if (_wcsicmp(name, L"NonPackaged") == 0) {
            wchar_t n2[512];
            for (DWORD j = 0;; j++) {
                DWORD l2 = 512;
                if (RegEnumKeyExW(sub, j, n2, &l2, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                HKEY s2 = nullptr;
                if (RegOpenKeyExW(sub, n2, 0, KEY_READ, &s2) != ERROR_SUCCESS) continue;
                bool u = InUse(s2);
                RegCloseKey(s2);
                if (u) { app = PrettyApp(n2, true); RegCloseKey(sub); return true; }
            }
        } else if (InUse(sub)) {
            app = PrettyApp(name, false);
            RegCloseKey(sub);
            return true;
        }
        RegCloseKey(sub);
    }
    return false;
}

void PrivacyWatcher::Run() {
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    HKEY km = nullptr, kc = nullptr;
    RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(base) + L"microphone").c_str(), 0, KEY_READ | KEY_NOTIFY, &km);
    RegOpenKeyExW(HKEY_CURRENT_USER, (std::wstring(base) + L"webcam").c_str(), 0, KEY_READ | KEY_NOTIFY, &kc);
    HANDLE em = CreateEventW(nullptr, FALSE, FALSE, nullptr), ec = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const DWORD filter = REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET;
    PrivacyInfo last;
    bool first = true;
    for (;;) {
        if (km) RegNotifyChangeKeyValue(km, TRUE, filter, em, TRUE);
        if (kc) RegNotifyChangeKeyValue(kc, TRUE, filter, ec, TRUE);
        PrivacyInfo cur;
        if (km) cur.mic = ScanStore(km, cur.micApp);
        if (kc) cur.cam = ScanStore(kc, cur.camApp);
        if (first || cur.mic != last.mic || cur.cam != last.cam || cur.micApp != last.micApp || cur.camApp != last.camApp) {
            PrivacyInfo* p = new PrivacyInfo(cur);
            if (!PostMessageW(notify_, WM_APP_SYS, SE_PRIVACY, (LPARAM)p)) delete p;
            last = cur;
            first = false;
        }
        HANDLE waits[3] = {stopEv_, em, ec};
        DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
        if (WaitForSingleObject(stopEv_, 150) == WAIT_OBJECT_0) break;  // дребезг записей в реестр
    }
    if (km) RegCloseKey(km);
    if (kc) RegCloseKey(kc);
    CloseHandle(em);
    CloseHandle(ec);
}

// ---------------------------------------------------------------------------
// Хук клавиатуры
// ---------------------------------------------------------------------------
static HHOOK g_hook;
static HWND g_hookWnd;
static std::atomic<bool> g_interceptVol{true};

static LRESULT CALLBACK LLKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lp;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool injected = (k->flags & LLKHF_INJECTED) != 0;
        switch (k->vkCode) {
            case VK_VOLUME_UP:
            case VK_VOLUME_DOWN:
            case VK_VOLUME_MUTE:
                if (g_interceptVol && !injected) {
                    if (down) PostMessageW(g_hookWnd, WM_APP_KEY, k->vkCode, 0);
                    return 1;  // глотаем — системный индикатор не появится, громкость меняем сами
                }
                break;
            case VK_MEDIA_PLAY_PAUSE:
            case VK_MEDIA_NEXT_TRACK:
            case VK_MEDIA_PREV_TRACK:
            case VK_MEDIA_STOP:
                if (down) PostMessageW(g_hookWnd, WM_APP_KEY, k->vkCode, 0);
                break;
            case VK_CAPITAL:
            case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT:
            case VK_LMENU: case VK_RMENU: case VK_MENU:
            case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL:
            case VK_LWIN: case VK_RWIN: case VK_SPACE:
                if (!down && !injected) PostMessageW(g_hookWnd, WM_APP_KEY, k->vkCode, 1);
                break;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}
bool InstallKeyHook(HWND notify) {
    if (g_hook) return true;
    g_hookWnd = notify;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyProc, GetModuleHandleW(nullptr), 0);
    return g_hook != nullptr;
}
void RemoveKeyHook() {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
}
void SetVolumeIntercept(bool on) { g_interceptVol = on; }
