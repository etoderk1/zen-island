// sysevents.h — системные события: громкость, аудиоустройства (Bluetooth + заряд),
// микрофон/камера, перехват клавиш (громкость, медиа, Caps Lock, раскладка)
#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>

#define WM_APP_SYS (WM_APP + 4)  // wParam = SysEvt, lParam = указатель на данные (освобождает UI)
#define WM_APP_KEY (WM_APP + 5)  // wParam = vk, lParam = 1 если отпускание

enum SysEvt { SE_VOLUME = 1, SE_AUDIODEV, SE_PRIVACY };

struct VolumeInfo { float vol = 0; bool mute = false; };
struct AudioDevInfo { std::wstring name; int battery = -1; bool bluetooth = false; };
struct PrivacyInfo { bool mic = false, cam = false; std::wstring micApp, camApp; };

// Громкость и смена устройства вывода (свой поток MTA)
class AudioWatcher {
public:
    void Start(HWND notify);
    void Stop();

private:
    void Run();
    HWND notify_ = nullptr;
    HANDLE stopEv_ = nullptr, changeEv_ = nullptr;
    std::thread th_;
};

// Кто использует микрофон/камеру (реестр CapabilityAccessManager)
class PrivacyWatcher {
public:
    void Start(HWND notify);
    void Stop();

private:
    void Run();
    HWND notify_ = nullptr;
    HANDLE stopEv_ = nullptr;
    std::thread th_;
};

// Низкоуровневый хук клавиатуры (на потоке UI)
bool InstallKeyHook(HWND notify);
void RemoveKeyHook();
void SetVolumeIntercept(bool on);  // глотать клавиши громкости (системный OSD не появится)

// Заряд Bluetooth-устройства по имени (SetupAPI), -1 если неизвестен
int BluetoothBattery(const std::wstring& name, bool* isBluetooth);
