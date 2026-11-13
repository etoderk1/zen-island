// recorder.h — запись экрана: Desktop Duplication (GPU) + Media Foundation H.264 → MP4
#pragma once
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>

#define WM_APP_REC (WM_APP + 8)  // wParam = 1 успех / 0 ошибка, lParam = std::wstring* (путь или текст ошибки)

struct RecOptions {
    int fps = 30;          // 30 или 60
    int quality = 1;       // 0 эконом, 1 обычное, 2 высокое
    bool cursor = true;    // рисовать курсор
    bool sysAudio = true;  // звук компьютера (WASAPI loopback)
    bool mic = false;      // микрофон
    HWND win = nullptr;
    std::wstring micId, outId;  // устройства (пусто — по умолчанию в Windows)    // записывать это окно (следим за ним, если двигают); nullptr — прямоугольник
    float sysGain = 1.f, micGain = 1.f;
};

class Recorder {
public:
    bool Start(const RECT& rcVirtual, const std::wstring& path, HWND notify, const RecOptions& o = RecOptions{});
    void Stop();          // асинхронно: результат придёт сообщением WM_APP_REC
    bool Active() const { return active_; }
    double Elapsed() const;  // без пауз
    void SetPaused(bool p);
    bool Paused() const { return paused_; }
    void SetMicMuted(bool m) { micMuted_ = m; }  // на лету (если микрофон включён при старте)
    bool MicMuted() const { return micMuted_; }
    bool HasMic() const { return opt_.mic; }
    const RecOptions& Options() const { return opt_; }
    void SetGains(float sys, float mic) { sysGain_ = sys; micGain_ = mic; }
    float SysPeak() { return sysPeak_.exchange(0.f); }  // пик с прошлого вызова (для индикатора)
    float MicPeak() { return micPeak_.exchange(0.f); }
    std::wstring audioNote;  // что не получилось со звуком (для подсказки)

private:
    void Run();
    RECT rc_ = {};
    std::wstring path_;
    HWND notify_ = nullptr;
    RecOptions opt_;
    std::thread th_;
    std::atomic<bool> stop_{false}, active_{false}, paused_{false}, micMuted_{false};
    std::atomic<LONGLONG> startQpc_{0}, pausedAt_{0}, pausedTotal_{0};
    std::atomic<float> sysGain_{1.f}, micGain_{1.f}, sysPeak_{0.f}, micPeak_{0.f};
};
