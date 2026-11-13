// vpn.cpp — определение VPN и внешнего IP (ipwho.is), без лишних зависимостей
#include <winsock2.h>
#include <ws2ipdef.h>
#include "vpn.h"
#include "net.h"
#include <iphlpapi.h>
#include <netioapi.h>
#include <winhttp.h>
#include <thread>
#include <atomic>
#include <vector>

static HANDLE g_notifyHandle = nullptr;
static HWND g_vpnWnd = nullptr;
static std::atomic<bool> g_busy{false};

static VOID NETIOAPI_API_ OnIfChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE) {
    if (g_vpnWnd) PostMessageW(g_vpnWnd, WM_APP_NETCHG, 0, 0);
}
void VpnWatchStart(HWND notify) {
    g_vpnWnd = notify;
    if (!g_notifyHandle) NotifyIpInterfaceChange(AF_UNSPEC, OnIfChange, nullptr, FALSE, &g_notifyHandle);
}
void VpnWatchStop() {
    if (g_notifyHandle) { CancelMibChangeNotify2(g_notifyHandle); g_notifyHandle = nullptr; }
}

static std::wstring Low(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}
// TUN/TAP-адаптеры популярных клиентов
static bool FindVpnAdapter(std::wstring& name) {
    static const wchar_t* yes[] = {L"wireguard", L"wintun", L"tap-windows", L"tap0901", L"openvpn", L"amnezia", L"outline", L"hiddify",
                                   L"sing-box", L"singbox", L"v2ray", L"xray", L"nekoray", L"nekobox", L"clash", L"mihomo", L"happ",
                                   L"throne", L"proton", L"nordlynx", L"mullvad", L"windscribe", L"warp", L"cloudflare", L"zerotier",
                                   L"tailscale", L"vpn", L"tun", L"karing", L"invisible", L"radmin"};
    static const wchar_t* no[] = {L"virtualbox", L"vmware", L"hyper-v", L"vethernet", L"loopback", L"bluetooth", L"wi-fi direct", L"teredo",
                                  L"isatap", L"6to4", L"npcap", L"wan miniport", L"kernel debug"};
    ULONG sz = 32 * 1024;
    std::vector<BYTE> buf(sz);
    ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz);
    if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); r = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz); }
    if (r != NO_ERROR) return false;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstUnicastAddress) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring d = Low(std::wstring(a->Description ? a->Description : L"") + L" " + (a->FriendlyName ? a->FriendlyName : L""));
        bool bad = false;
        for (auto n : no) if (d.find(n) != std::wstring::npos) { bad = true; break; }
        if (bad) continue;
        bool hit = false;
        for (auto y : yes) if (d.find(y) != std::wstring::npos) { hit = true; break; }
        if (!hit && a->IfType == 53 /*PROP_VIRTUAL*/ && d.find(L"tun") != std::wstring::npos) hit = true;
        if (hit) { name = a->FriendlyName ? a->FriendlyName : L"VPN"; return true; }
    }
    return false;
}
// системный прокси на localhost — так работают клиенты в режиме «прокси»
static bool LocalProxy() {
    DWORD en = 0, sz = sizeof(en);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", L"ProxyEnable", RRF_RT_REG_DWORD,
                     nullptr, &en, &sz) != ERROR_SUCCESS || !en)
        return false;
    wchar_t srv[256] = {};
    sz = sizeof(srv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", L"ProxyServer", RRF_RT_REG_SZ,
                     nullptr, srv, &sz) != ERROR_SUCCESS)
        return false;
    std::wstring s = Low(srv);
    return s.find(L"127.0.0.1") != std::wstring::npos || s.find(L"localhost") != std::wstring::npos;
}
// запрос через системные настройки прокси (чтобы увидеть IP именно VPN)
static bool GetViaSystemProxy(const wchar_t* host, const wchar_t* path, std::string& body) {
    HINTERNET s = WinHttpOpen(L"DerkIsland/1.2", 4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"DerkIsland/1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    // если клиент работает как локальный прокси, а WinHTTP его не видит — подставим вручную
    if (LocalProxy()) {
        wchar_t srv[256] = {};
        DWORD sz = sizeof(srv);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings", L"ProxyServer", RRF_RT_REG_SZ,
                         nullptr, srv, &sz) == ERROR_SUCCESS) {
            WINHTTP_PROXY_INFO pi = {WINHTTP_ACCESS_TYPE_NAMED_PROXY, srv, nullptr};
            WinHttpSetOption(s, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
        }
    }
    DWORD to = 6000;
    WinHttpSetTimeouts(s, to, to, to, to);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            std::string chunk(avail, '\0');
            DWORD rd = 0;
            if (!WinHttpReadData(r, &chunk[0], avail, &rd) || !rd) break;
            body.append(chunk.data(), rd);
            if (body.size() > 64 * 1024) break;
        }
        ok = !body.empty();
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

void VpnCheckAsync(HWND notify, bool lookupIp) {
    if (g_busy.exchange(true)) return;
    std::thread([notify, lookupIp] {
        VpnInfo* v = new VpnInfo;
        std::wstring name;
        if (FindVpnAdapter(name)) { v->on = true; v->adapter = name; }
        else if (LocalProxy()) { v->on = true; v->proxy = true; v->adapter = L"Прокси"; }
        if (lookupIp) {
            std::string body;
            if (GetViaSystemProxy(L"ipwho.is", L"/?lang=ru&fields=success,ip,country,country_code,city", body)) {
                net::JVal j;
                if (net::ParseJson(body, j)) {
                    const net::JVal* ok = j.Get("success");
                    if (!ok || ok->t != net::JVal::Bool || ok->b) {
                        if (auto* p = j.Get("ip")) v->ip = net::FromU8(p->s);
                        if (auto* p = j.Get("country")) v->country = net::FromU8(p->s);
                        if (auto* p = j.Get("country_code")) v->cc = net::FromU8(p->s);
                        if (auto* p = j.Get("city")) v->city = net::FromU8(p->s);
                        v->ipOk = !v->ip.empty();
                    }
                }
            }
        }
        g_busy = false;
        if (!PostMessageW(notify, WM_APP_VPN, 0, (LPARAM)v)) delete v;
    }).detach();
}

// есть ли подключение по кабелю (Ethernet с адресом и шлюзом)
bool EthernetUp() {
    ULONG sz = 16 * 1024;
    std::vector<BYTE> buf(sz);
    ULONG fl = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz);
    if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(sz); r = GetAdaptersAddresses(AF_UNSPEC, fl, nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &sz); }
    if (r != NO_ERROR) return false;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next)
        if (a->IfType == IF_TYPE_ETHERNET_CSMACD && a->OperStatus == IfOperStatusUp && a->FirstGatewayAddress) return true;
    return false;
}
