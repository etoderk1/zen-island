// widgets.h — погода (Open-Meteo) и календарь (ICS-ссылка Google/Яндекс/Outlook)
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

#define WM_APP_WEATHER (WM_APP + 11)  // lParam = WeatherData*
#define WM_APP_CAL (WM_APP + 12)      // lParam = CalData*

enum WxIcon { WX_CLEAR, WX_PARTLY, WX_CLOUD, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_THUNDER };

struct WeatherData {
    bool ok = false;
    std::wstring error;
    std::wstring city;
    double temp = 0, feels = 0, wind = 0;
    int code = 0;
    bool isDay = true;
    double tmin[7] = {}, tmax[7] = {}, dprecip[7] = {};
    int dcode[7] = {}, dweekday[7] = {};
    int days = 0;
    double humidity = 0, precip = 0;
    std::wstring sunrise, sunset;
    // по часам на сутки вперёд
    int hours = 0;
    int hhour[24] = {}, hcode[24] = {}, hprob[24] = {};
    double htemp[24] = {};
    bool hday[24] = {};
};
const wchar_t* WeatherText(int wmoCode);
int WeatherIcon(int wmoCode);

class WeatherService {
public:
    void Start(HWND notify, const std::wstring& city);
    void SetCity(const std::wstring& city);  // пустая строка — определить по IP
    void Refresh();
    void Stop();

private:
    void Run();
    HWND notify_ = nullptr;
    std::wstring city_;
    std::mutex mx_;
    HANDLE stopEv_ = nullptr, wakeEv_ = nullptr;
    std::thread th_;
};

struct CalEvent {
    long long start = 0, end = 0;  // FILETIME (UTC), 100 нс
    bool allDay = false;
    std::wstring title, location;
};
struct CalData {
    bool ok = false;
    std::wstring error;
    std::vector<CalEvent> events;  // ближайшие, по возрастанию
};
// Разбор ICS и разворачивание повторов в окне [from, to) (FILETIME UTC)
bool ParseIcs(const std::string& ics, long long from, long long to, std::vector<CalEvent>& out);

class CalendarService {
public:
    void Start(HWND notify, const std::wstring& url);
    void SetUrl(const std::wstring& url);
    void Stop();

private:
    void Run();
    HWND notify_ = nullptr;
    std::wstring url_;
    std::mutex mx_;
    HANDLE stopEv_ = nullptr, wakeEv_ = nullptr;
    std::thread th_;
};
