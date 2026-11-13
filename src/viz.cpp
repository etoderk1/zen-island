// viz.cpp — WASAPI loopback (то, что сейчас играет в системе) + FFT
// Поток захвата работает ТОЛЬКО пока остров видим и что-то играет.
#include "viz.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>

static const CLSID kCLSID_MMDevEnum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID kIID_MMDevEnum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID kIID_AudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID kIID_CaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static const float kPi = 3.14159265358979f;

Visualizer::Visualizer() {
    ring_.assign(RING, 0.f);
    win_.resize(N);
    buf_.resize(N);
    fft_.resize(N);
    for (int i = 0; i < N; i++) win_[i] = 0.5f - 0.5f * std::cos(2 * kPi * i / (N - 1));  // Ханн
    tw_.resize(N / 2);
    for (int i = 0; i < N / 2; i++) tw_[i] = std::polar(1.f, -2 * kPi * i / N);
    rev_.resize(N);
    int bits = 0;
    while ((1 << bits) < N) bits++;
    for (int i = 0; i < N; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev_[i] = r;
    }
    stopEv_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
Visualizer::~Visualizer() {
    Stop();
    if (stopEv_) CloseHandle(stopEv_);
}

void Visualizer::Start() {
    if (running_) return;
    quit_ = false;
    ResetEvent(stopEv_);
    running_ = true;
    th_ = std::thread([this] { Run(); });
}
void Visualizer::Stop() {
    if (!running_) return;
    quit_ = true;
    SetEvent(stopEv_);
    if (th_.joinable()) th_.join();
    running_ = false;
    healthy_ = false;
}

void Visualizer::Push(const BYTE* data, UINT32 frames, bool silent) {
    std::lock_guard<std::mutex> g(mx_);
    for (UINT32 f = 0; f < frames; f++) {
        float v = 0;
        if (!silent && data) {
            for (int c = 0; c < channels_; c++) {
                size_t idx = (size_t)f * channels_ + c;
                if (isFloat_ && bits_ == 32) v += ((const float*)data)[idx];
                else if (bits_ == 16) v += ((const int16_t*)data)[idx] / 32768.f;
                else if (bits_ == 32) v += ((const int32_t*)data)[idx] / 2147483648.f;
                else if (bits_ == 24) {
                    const BYTE* p = data + idx * 3;
                    int32_t s = (int32_t)((p[0] << 8) | (p[1] << 16) | (p[2] << 24)) >> 8;
                    v += s / 8388608.f;
                }
            }
            v /= (float)std::max(1, channels_);
        }
        ring_[written_ % RING] = v;
        written_++;
    }
}

void Visualizer::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    while (!quit_) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* ac = nullptr;
        IAudioCaptureClient* cc = nullptr;
        WAVEFORMATEX* wf = nullptr;
        LPWSTR devId = nullptr;
        bool eventMode = true, ok = false;
        do {
            if (FAILED(CoCreateInstance(kCLSID_MMDevEnum, nullptr, CLSCTX_ALL, kIID_MMDevEnum, (void**)&en))) break;
            if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
            dev->GetId(&devId);
            if (FAILED(dev->Activate(kIID_AudioClient, CLSCTX_ALL, nullptr, (void**)&ac))) break;
            if (FAILED(ac->GetMixFormat(&wf)) || !wf) break;
            HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0,
                                        wf, nullptr);
            if (FAILED(hr)) {
                // старые сборки Windows: loopback без событий — опрос каждые 10 мс
                Rel(ac);
                if (FAILED(dev->Activate(kIID_AudioClient, CLSCTX_ALL, nullptr, (void**)&ac))) break;
                if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, nullptr))) break;
                eventMode = false;
            } else if (FAILED(ac->SetEventHandle(ev))) {
                break;
            }
            if (FAILED(ac->GetService(kIID_CaptureClient, (void**)&cc))) break;
            {
                std::lock_guard<std::mutex> g(mx_);
                sampleRate_ = (int)wf->nSamplesPerSec;
                channels_ = wf->nChannels;
                bits_ = wf->wBitsPerSample;
                isFloat_ = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
                if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
                    const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)wf;
                    isFloat_ = IsEqualGUID(x->SubFormat, kSubFloat) != 0;
                }
            }
            if (FAILED(ac->Start())) break;
            ok = true;
        } while (false);

        healthy_ = ok;
        if (ok) {
            DWORD lastCheck = GetTickCount();
            HANDLE waits[2] = {stopEv_, ev};
            while (!quit_) {
                if (eventMode) WaitForMultipleObjects(2, waits, FALSE, 200);
                else WaitForSingleObject(stopEv_, 10);
                if (quit_) break;
                UINT32 pk = 0;
                HRESULT hr;
                while (SUCCEEDED(hr = cc->GetNextPacketSize(&pk)) && pk > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    hr = cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (FAILED(hr)) break;
                    Push(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                    cc->ReleaseBuffer(frames);
                }
                if (FAILED(hr)) break;  // устройство пропало — переподключаемся
                // сменилось устройство по умолчанию (наушники и т.п.)
                if (GetTickCount() - lastCheck > 2500) {
                    lastCheck = GetTickCount();
                    IMMDevice* d2 = nullptr;
                    LPWSTR id2 = nullptr;
                    bool changed = false;
                    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d2)) && d2) {
                        if (SUCCEEDED(d2->GetId(&id2)) && id2 && devId) changed = wcscmp(id2, devId) != 0;
                        if (id2) CoTaskMemFree(id2);
                        d2->Release();
                    }
                    if (changed) break;
                }
            }
            ac->Stop();
        }
        healthy_ = false;
        Rel(cc);
        Rel(ac);
        if (wf) CoTaskMemFree(wf);
        if (devId) CoTaskMemFree(devId);
        Rel(dev);
        Rel(en);
        if (!quit_) WaitForSingleObject(stopEv_, ok ? 200 : 1500);
    }
    CloseHandle(ev);
    CoUninitialize();
}

bool Visualizer::Analyze(double dt, Spectrum& s) {
    if (!running_ || !healthy_) return false;
    int sr;
    unsigned long long w;
    {
        std::lock_guard<std::mutex> g(mx_);
        w = written_;
        sr = sampleRate_;
        if (w < (unsigned long long)N) return false;
        for (int i = 0; i < N; i++) buf_[i] = ring_[(w - N + i) % RING];
    }
    // нет новых сэмплов (тишина — WASAPI ничего не шлёт) → плавно гасим
    if (w == lastWritten_) sinceData_ += dt; else sinceData_ = 0;
    lastWritten_ = w;
    bool stale = sinceData_ > 0.25;

    float target[Spectrum::kBands] = {};
    float bassEnergy = 0;
    if (!stale) {
        for (int i = 0; i < N; i++) fft_[rev_[i]] = std::complex<float>(buf_[i] * win_[i], 0.f);
        for (int len = 2; len <= N; len <<= 1) {
            int half = len >> 1, step = N / len;
            for (int i = 0; i < N; i += len)
                for (int j = 0; j < half; j++) {
                    std::complex<float> t = tw_[j * step] * fft_[i + j + half];
                    fft_[i + j + half] = fft_[i + j] - t;
                    fft_[i + j] += t;
                }
        }
        const float norm = 4.f / N;  // окно Ханна (×2) и односторонний спектр (×2)
        const float fLo = 35.f, fHi = 16000.f;
        for (int b = 0; b < Spectrum::kBands; b++) {
            float f0 = fLo * std::pow(fHi / fLo, (float)b / Spectrum::kBands);
            float f1 = fLo * std::pow(fHi / fLo, (float)(b + 1) / Spectrum::kBands);
            int k0 = std::max(1, (int)(f0 * N / sr)), k1 = std::max(k0 + 1, (int)std::ceil(f1 * N / sr));
            k1 = std::min(k1, N / 2);
            float mx = 0;
            for (int k = k0; k < k1; k++) mx = std::max(mx, std::abs(fft_[k]) * norm);
            float db = 20.f * std::log10(mx + 1e-7f) + b * 1.3f;  // наклон: верхам чуть плюс
            target[b] = std::min(1.f, std::max(0.f, (db + 62.f) / 50.f));
        }
        int kb0 = std::max(1, 35 * N / sr), kb1 = std::max(kb0 + 1, 150 * N / sr);
        for (int k = kb0; k < kb1; k++) bassEnergy += std::norm(fft_[k]);
        bassEnergy = std::sqrt(bassEnergy) * norm;
    }

    float rel = 1.f - std::exp(-(float)dt * 7.f);
    for (int b = 0; b < Spectrum::kBands; b++) {
        float& v = s.band[b];
        if (target[b] > v) v += (target[b] - v) * 0.65f;  // быстрая атака
        else v += (target[b] - v) * rel;                  // мягкий спад
    }
    auto avg = [&](int a, int b) { float t = 0; for (int i = a; i <= b; i++) t += s.band[i]; return t / (b - a + 1); };
    s.bass = avg(0, 3);
    s.mid = avg(4, 10);
    s.high = avg(11, 15);
    s.level = std::min(1.f, 0.5f * s.bass + 0.32f * s.mid + 0.25f * s.high);

    // детектор ударов по энергии баса относительно скользящего среднего
    s.beatNow = false;
    beatCool_ = std::max(0.f, beatCool_ - (float)dt);
    if (!stale && bassEnergy > 0.004f && bassEnergy > bassAvg_ * 1.45f && beatCool_ <= 0) {
        s.beatNow = true;
        s.beat = 1.f;
        beatCool_ = 0.2f;
    }
    bassAvg_ += (bassEnergy - bassAvg_) * std::min(1.f, (float)dt * 3.f);
    s.beat *= std::exp(-(float)dt * 7.f);
    return true;
}
