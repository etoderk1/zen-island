// pods.h — заряд AirPods/Beats из Bluetooth LE рекламы (как это делает iPhone) и разбор пакета
#pragma once
#include <windows.h>
#include <string>
#include <vector>

#define WM_APP_PODS (WM_APP + 24)  // lParam = PodsInfo*

struct PodsInfo {
    int left = -1, right = -1, box = -1;     // 0..100, -1 — нет данных
    bool chL = false, chR = false, chBox = false;
    bool single = false;                      // AirPods Max / Beats — одна батарея (в left)
    std::wstring model;
    int rssi = -127;
};

// Разбор manufacturer data Apple (без company id 0x004C). false — не пакет наушников.
bool ParseApplePods(const BYTE* d, size_t n, PodsInfo& out);

// Слушает BLE-рекламу seconds секунд, присылает лучший (самый близкий) результат, не чаще раза в секунду.
void PodsScanStart(HWND notify, int seconds);
void PodsScanStop();
bool PodsScanning();
