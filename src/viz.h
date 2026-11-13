// viz.h — захват системного звука (WASAPI loopback) + FFT для визуализации
#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <complex>

struct Spectrum {
    static const int kBands = 16;
    float band[kBands] = {};  // 0..1, сглаженные, от баса к верхам
    float bass = 0, mid = 0, high = 0;
    float level = 0;   // общий уровень 0..1
    float beat = 0;    // 1 в момент удара, затем затухает
    bool beatNow = false;  // удар случился в этом кадре
};

class Visualizer {
public:
    Visualizer();
    ~Visualizer();
    void Start();
    void Stop();
    bool Running() const { return running_; }
    bool Healthy() const { return healthy_; }
    // Вызывать из UI-потока раз в кадр. false — данных нет (захват не работает)
    bool Analyze(double dt, Spectrum& s);

private:
    void Run();
    void Push(const BYTE* data, UINT32 frames, bool silent);

    static const int N = 2048;
    static const int RING = 8192;
    std::thread th_;
    std::atomic<bool> quit_{false}, running_{false}, healthy_{false};
    HANDLE stopEv_ = nullptr;

    std::mutex mx_;
    std::vector<float> ring_;
    unsigned long long written_ = 0;
    int sampleRate_ = 48000;
    int channels_ = 2, bits_ = 32;
    bool isFloat_ = true;

    // анализ (UI-поток)
    std::vector<float> win_, buf_;
    std::vector<std::complex<float>> fft_, tw_;
    std::vector<int> rev_;
    unsigned long long lastWritten_ = 0;
    double sinceData_ = 0;
    float bassAvg_ = 0, beatCool_ = 0;
};
