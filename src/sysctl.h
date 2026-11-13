// sysctl.h — вкладка «Управление»: Wi-Fi, Bluetooth, тёмная тема, устройства звука, питание
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Радио (Windows.Devices.Radios). state: -1 нет такого радио, 0 выкл, 1 вкл
int RadioState(int kind);            // kind: 1 Wi-Fi, 3 Bluetooth (блокирующий вызов, ~десятки мс)
bool SetRadio(int kind, bool on);    // блокирующий

bool IsDarkTheme();
void SetDarkTheme(bool dark);

struct AudioDev { std::wstring id, name; bool isDefault = false; };
std::vector<AudioDev> ListOutputDevices();
std::vector<AudioDev> ListAudioDevices(bool capture);  // capture — микрофоны
bool SetDefaultOutput(const std::wstring& id);

std::wstring WifiName();             // SSID текущей сети или ""
void PowerAction(int what);          // 0 блокировка, 1 сон, 2 перезагрузка, 3 выключение
