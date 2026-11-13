// vpn.h — включён ли VPN (TUN-адаптер или системный прокси) и через какую страну/IP мы выходим
#pragma once
#include <windows.h>
#include <string>

#define WM_APP_VPN (WM_APP + 26)     // lParam = VpnInfo*
#define WM_APP_NETCHG (WM_APP + 27)  // поменялись сетевые интерфейсы

struct VpnInfo {
    bool on = false;
    bool proxy = false;          // не TUN, а системный прокси (v2rayN и т.п. в режиме прокси)
    std::wstring adapter;        // имя адаптера / клиента
    std::wstring ip, country, cc, city;
    bool ipOk = false;
};

void VpnWatchStart(HWND notify);  // подписка на смену сетевых интерфейсов
void VpnWatchStop();
void VpnCheckAsync(HWND notify, bool lookupIp);

bool EthernetUp();  // подключение по кабелю есть?
