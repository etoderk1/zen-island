// recorder.cpp — запись экрана в MP4 (H.264, аппаратный кодек видеокарты, если есть)
#include "recorder.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <algorithm>
#include <cmath>

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static LONGLONG Qpc() { LARGE_INTEGER x; QueryPerformanceCounter(&x); return x.QuadPart; }
static LONGLONG QpcFreq() { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x.QuadPart; }

bool Recorder::Start(const RECT& rc, const std::wstring& path, HWND notify, const RecOptions& o) {
    if (active_) return false;
    if (th_.joinable()) th_.join();
    rc_ = rc;
    path_ = path;
    notify_ = notify;
    opt_ = o;
    if (opt_.fps != 60) opt_.fps = 30;
    sysGain_ = o.sysGain;
    micGain_ = o.micGain;
    audioNote.clear();
    stop_ = false;
    paused_ = false;
    micMuted_ = false;
    pausedTotal_ = 0;
    active_ = true;
    startQpc_ = Qpc();
    th_ = std::thread([this] { Run(); });
    return true;
}
void Recorder::Stop() { stop_ = true; }
void Recorder::SetPaused(bool p) {
    if (!active_ || p == paused_) return;
    if (p) { pausedAt_ = Qpc(); paused_ = true; }
    else { pausedTotal_ = pausedTotal_ + (Qpc() - pausedAt_); paused_ = false; }
}
double Recorder::Elapsed() const {
    if (!active_) return 0.0;
    LONGLONG now = paused_ ? pausedAt_.load() : Qpc();
    return (double)(now - startQpc_ - pausedTotal_) / (double)QpcFreq();
}

// ---------------------------------------------------------------- звук: WASAPI (loopback и микрофон) → 48 кГц стерео
static const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const int kRate = 48000;
struct AudioCap {
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    bool flt = false;
    int ch = 2, rate = 48000, bits = 32;
    std::vector<float> fifo;   // стерео, 48 кГц, чередование L R
    long long fifoStart = 0;   // номер сэмпла (в общем времени записи) для fifo[0]
    double rpos = 0;
    float prev[2] = {0, 0};
    bool havePrev = false;
    bool Init(bool loopback, const std::wstring& id) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) &&
            ((!id.empty() && SUCCEEDED(en->GetDevice(id.c_str(), &dev))) ||
             SUCCEEDED(en->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &dev))) &&
            SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) {
            WAVEFORMATEX* wf = nullptr;
            if (SUCCEEDED(ac->GetMixFormat(&wf)) && wf) {
                ch = wf->nChannels;
                rate = (int)wf->nSamplesPerSec;
                bits = wf->wBitsPerSample;
                flt = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(((WAVEFORMATEXTENSIBLE*)wf)->SubFormat, kSubFloat));
                if (SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, 3000000, 0, wf, nullptr)) &&
                    SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)) && SUCCEEDED(ac->Start()))
                    ok = true;
                CoTaskMemFree(wf);
            }
        }
        if (dev) dev->Release();
        if (en) en->Release();
        if (!ok) Close();
        return ok;
    }
    void Close() {
        if (ac) ac->Stop();
        Rel(cc);
        Rel(ac);
    }
    float Smp(const BYTE* p, int c) const {
        if (flt && bits == 32) return ((const float*)p)[c];
        if (bits == 16) return ((const short*)p)[c] / 32768.f;
        if (bits == 24) { const BYTE* q = p + c * 3; int v = (q[0] << 8) | (q[1] << 16) | (q[2] << 24); return (v >> 8) / 8388608.f; }
        if (bits == 32) return ((const int*)p)[c] / 2147483648.f;
        return 0.f;
    }
    // забрать всё, что накопилось; idxOf(qpc100ns) переводит время пакета в номер сэмпла записи
    template <class F> void Pump(F idxOf, long long written, bool discard) {
        if (!cc) return;
        UINT32 pk = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&pk)) && pk > 0) {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD fl = 0;
            UINT64 devPos = 0, qpcPos = 0;
            if (FAILED(cc->GetBuffer(&data, &n, &fl, &devPos, &qpcPos))) break;
            if (!discard && n > 0) {
                // во входном формате → стерео float
                std::vector<float> in;
                in.reserve((n + 1) * 2);
                if (!havePrev) { prev[0] = prev[1] = 0; havePrev = true; }
                in.push_back(prev[0]);
                in.push_back(prev[1]);
                int fb = ch * bits / 8;
                for (UINT32 i = 0; i < n; i++) {
                    float l = 0, r = 0;
                    if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        const BYTE* f = data + (size_t)i * fb;
                        l = Smp(f, 0);
                        r = ch > 1 ? Smp(f, 1) : l;
                    }
                    in.push_back(l);
                    in.push_back(r);
                }
                // куда во времени записи ложится пакет
                long long at = idxOf((long long)qpcPos);
                long long end = fifoStart + (long long)fifo.size() / 2;
                if (fifo.empty()) fifoStart = std::max(at, written), end = fifoStart;
                else if (at > end + kRate / 50) fifo.insert(fifo.end(), (size_t)(at - end) * 2, 0.f);  // дыра > 20 мс — тишина
                // линейный ресемплинг к 48 кГц
                double step = (double)rate / kRate, p = rpos;
                size_t frames = in.size() / 2;
                while (p <= (double)(frames - 1) - 1e-9) {
                    size_t i0 = (size_t)p;
                    float f = (float)(p - i0);
                    size_t i1 = std::min(i0 + 1, frames - 1);
                    fifo.push_back(in[i0 * 2] * (1 - f) + in[i1 * 2] * f);
                    fifo.push_back(in[i0 * 2 + 1] * (1 - f) + in[i1 * 2 + 1] * f);
                    p += step;
                }
                rpos = p - (double)(frames - 1);
                prev[0] = in[(frames - 1) * 2];
                prev[1] = in[(frames - 1) * 2 + 1];
            }
            cc->ReleaseBuffer(n);
        }
        if (discard) { fifo.clear(); havePrev = false; rpos = 0; }
    }
    // сэмпл k (стерео) или тишина
    void Take(long long k, float& l, float& r) const {
        long long i = k - fifoStart;
        if (i < 0 || i * 2 + 1 >= (long long)fifo.size()) { l = r = 0; return; }
        l = fifo[i * 2];
        r = fifo[i * 2 + 1];
    }
    void Drop(long long upTo, long long nowIdx) {
        long long i = upTo - fifoStart;
        if (i > 0) {
            size_t cut = (size_t)std::min<long long>(i * 2, (long long)fifo.size());
            fifo.erase(fifo.begin(), fifo.begin() + cut);
            fifoStart += (long long)cut / 2;
        }
        // часы устройства спешат — не копим больше 0,3 с вперёд
        long long end = fifoStart + (long long)fifo.size() / 2;
        if (end > nowIdx + kRate * 3 / 10) fifo.resize((size_t)std::max<long long>(0, (nowIdx + kRate / 10 - fifoStart) * 2));
    }
};

// Media Foundation грузим динамически: в N-редакциях Windows её нет, и exe не должен из-за этого падать
struct MfApi {
    HMODULE plat = nullptr, rw = nullptr;
    HRESULT(WINAPI* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI* Shutdown)() = nullptr;
    HRESULT(WINAPI* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(WINAPI* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* CreateSample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* CreateSinkWriterFromURL)(LPCWSTR, IMFByteStream*, IMFAttributes*, IMFSinkWriter**) = nullptr;
    bool Load() {
        if (Startup) return true;
        plat = LoadLibraryW(L"mfplat.dll");
        rw = LoadLibraryW(L"mfreadwrite.dll");
        if (!plat || !rw) return false;
        Startup = (decltype(Startup))GetProcAddress(plat, "MFStartup");
        Shutdown = (decltype(Shutdown))GetProcAddress(plat, "MFShutdown");
        CreateAttributes = (decltype(CreateAttributes))GetProcAddress(plat, "MFCreateAttributes");
        CreateMediaType = (decltype(CreateMediaType))GetProcAddress(plat, "MFCreateMediaType");
        CreateMemoryBuffer = (decltype(CreateMemoryBuffer))GetProcAddress(plat, "MFCreateMemoryBuffer");
        CreateSample = (decltype(CreateSample))GetProcAddress(plat, "MFCreateSample");
        CreateSinkWriterFromURL = (decltype(CreateSinkWriterFromURL))GetProcAddress(rw, "MFCreateSinkWriterFromURL");
        if (!Shutdown || !CreateAttributes || !CreateMediaType || !CreateMemoryBuffer || !CreateSample || !CreateSinkWriterFromURL) {
            Startup = nullptr;
            return false;
        }
        return Startup != nullptr;
    }
};
static MfApi g_mf;

// видимые границы окна без невидимой рамки (DWM), dwmapi грузим сами
static bool WinRect(HWND w, RECT& r) {
    if (!w || !IsWindow(w)) return false;
    typedef HRESULT(WINAPI * DwmAttr)(HWND, DWORD, PVOID, DWORD);
    static DwmAttr fn = (DwmAttr)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute");
    if (fn && SUCCEEDED(fn(w, 9 /*DWMWA_EXTENDED_FRAME_BOUNDS*/, &r, sizeof(r)))) return true;
    return GetWindowRect(w, &r) != 0;
}
static HRESULT SetSize(IMFMediaType* t, const GUID& k, UINT32 a, UINT32 b) { return t->SetUINT64(k, ((UINT64)a << 32) | b); }

void Recorder::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::wstring err;
    bool mfOk = g_mf.Load();
    if (mfOk) g_mf.Startup(MF_VERSION, MFSTARTUP_LITE);
    bool wroteAny = false;

    IDXGIFactory1* fac = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    IDXGIOutput1* out1 = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* staging = nullptr;
    IMFSinkWriter* wr = nullptr;
    DWORD stream = 0, astream = 0;
    AudioCap sys, mic;
    bool haveAudio = false;
    long long aWritten = 0;  // сколько звуковых сэмплов записано
    HDC frameDC = nullptr, outDC = nullptr;
    HBITMAP frameBmp = nullptr, outBmp = nullptr;
    HGDIOBJ frameOld = nullptr, outOld = nullptr;
    BYTE *frameBits = nullptr, *outBits = nullptr;
    RECT desk = {};
    int w = 0, h = 0;
    LONG lx = 0, ly = 0;

    do {
        if (!mfOk) { err = L"нет Media Foundation (Windows N — поставь Media Feature Pack)"; break; }
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) { err = L"DXGI недоступен"; break; }
        if (opt_.win) {
            RECT wr2;
            if (!WinRect(opt_.win, wr2)) { err = L"Окно закрылось"; break; }
            rc_ = wr2;
        }
        POINT c = {(rc_.left + rc_.right) / 2, (rc_.top + rc_.bottom) / 2};
        for (UINT i = 0; !output && fac->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            IDXGIOutput* o = nullptr;
            for (UINT j = 0; adapter->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; j++) {
                DXGI_OUTPUT_DESC d;
                o->GetDesc(&d);
                if (PtInRect(&d.DesktopCoordinates, c)) { output = o; desk = d.DesktopCoordinates; break; }
                o->Release();
            }
            if (!output) Rel(adapter);
        }
        if (!output) { err = L"Не найден монитор для записи"; break; }
        RECT r;
        IntersectRect(&r, &rc_, &desk);
        lx = r.left - desk.left;
        ly = r.top - desk.top;
        w = (r.right - r.left) & ~1;
        h = (r.bottom - r.top) & ~1;
        if (w < 16 || h < 16) { err = L"Слишком маленькая область"; break; }

        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &dev, &fl, &ctx))) { err = L"Direct3D 11 недоступен"; break; }
        if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&out1)) ||
            FAILED(out1->DuplicateOutput(dev, &dup))) { err = L"Захват экрана недоступен"; break; }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) { err = L"Нет памяти видеокарты"; break; }

        // Media Foundation: H.264 в MP4
        IMFAttributes* attr = nullptr;
        g_mf.CreateAttributes(&attr, 1);
        if (attr) attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        HRESULT hr = g_mf.CreateSinkWriterFromURL(path_.c_str(), nullptr, attr, &wr);
        Rel(attr);
        if (FAILED(hr)) { err = L"Не удалось создать файл"; break; }
        IMFMediaType *outT = nullptr, *inT = nullptr;
        g_mf.CreateMediaType(&outT);
        outT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        const int fps = opt_.fps;
        static const double bpp[3] = {0.05, 0.1, 0.18};
        double br = bpp[std::max(0, std::min(2, opt_.quality))] * w * h * fps;
        outT->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::max(2000000.0, std::min(60000000.0, br)));
        outT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        SetSize(outT, MF_MT_FRAME_SIZE, w, h);
        SetSize(outT, MF_MT_FRAME_RATE, fps, 1);
        SetSize(outT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = wr->AddStream(outT, &stream);
        g_mf.CreateMediaType(&inT);
        inT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        inT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        inT->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));
        SetSize(inT, MF_MT_FRAME_SIZE, w, h);
        SetSize(inT, MF_MT_FRAME_RATE, fps, 1);
        SetSize(inT, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = wr->SetInputMediaType(stream, inT, nullptr);
        Rel(outT);
        Rel(inT);
        if (FAILED(hr)) { err = L"Кодек H.264 недоступен"; break; }
        // звук: захват + дорожка AAC
        if (opt_.sysAudio && !sys.Init(true, opt_.outId)) audioNote = L"звук компьютера не записывается";
        if (opt_.mic && !mic.Init(false, opt_.micId)) audioNote = L"микрофон недоступен";
        if (sys.cc || mic.cc) {
            IMFMediaType *aOut = nullptr, *aIn = nullptr;
            g_mf.CreateMediaType(&aOut);
            aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, opt_.quality >= 2 ? 24000 : 16000);
            g_mf.CreateMediaType(&aIn);
            aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
            HRESULT ha = wr->AddStream(aOut, &astream);
            if (SUCCEEDED(ha)) ha = wr->SetInputMediaType(astream, aIn, nullptr);
            Rel(aOut);
            Rel(aIn);
            if (SUCCEEDED(ha)) haveAudio = true;
            else { sys.Close(); mic.Close(); audioNote = L"кодек AAC недоступен — запись без звука"; }
        }
        hr = wr->BeginWriting();
        if (FAILED(hr)) { err = L"Не удалось начать запись"; break; }

        // два DIB: «чистый» кадр и кадр с курсором
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        frameDC = CreateCompatibleDC(nullptr);
        outDC = CreateCompatibleDC(nullptr);
        frameBmp = CreateDIBSection(frameDC, &bi, DIB_RGB_COLORS, (void**)&frameBits, nullptr, 0);
        outBmp = CreateDIBSection(outDC, &bi, DIB_RGB_COLORS, (void**)&outBits, nullptr, 0);
        if (!frameBmp || !outBmp) { err = L"Нет памяти"; break; }
        frameOld = SelectObject(frameDC, frameBmp);
        outOld = SelectObject(outDC, outBmp);
        const size_t frameBytes = (size_t)w * h * 4;

        const LONGLONG freq = QpcFreq();
        const LONGLONG dur = 10000000LL / fps;
        auto mediaQpc = [&]() -> LONGLONG { return Qpc() - startQpc_ - pausedTotal_; };
        // время пакета звука (в 100 нс по QPC) → номер сэмпла записи
        auto idxOf = [&](long long qpc100) -> long long {
            LONGLONG q = (LONGLONG)((double)qpc100 * freq / 1e7);
            return (long long)((double)(q - startQpc_ - pausedTotal_) * kRate / freq);
        };
        auto pumpAudio = [&](bool discard) {
            if (!haveAudio) return;
            sys.Pump(idxOf, aWritten, discard);
            mic.Pump(idxOf, aWritten, discard);
            if (discard) return;
            long long nowIdx = (long long)((double)mediaQpc() * kRate / freq);
            long long target = nowIdx - kRate / 10;  // отстаём на 100 мс, чтобы пакеты успели прийти
            if (target - aWritten < kRate / 50) return;  // пишем кусками от 20 мс
            long long n = target - aWritten;
            IMFMediaBuffer* ab = nullptr;
            if (SUCCEEDED(g_mf.CreateMemoryBuffer((DWORD)(n * 4), &ab))) {
                BYTE* p = nullptr;
                if (SUCCEEDED(ab->Lock(&p, nullptr, nullptr))) {
                    short* o = (short*)p;
                    float mg = micMuted_ ? 0.f : micGain_.load(), sg = sysGain_.load();
                    float sp = 0, mp = 0;
                    for (long long k = 0; k < n; k++) {
                        float sl, sr, ml, mr;
                        sys.Take(aWritten + k, sl, sr);
                        mic.Take(aWritten + k, ml, mr);
                        sl *= sg; sr *= sg; ml *= mg; mr *= mg;
                        sp = std::max(sp, std::max(std::fabs(sl), std::fabs(sr)));
                        mp = std::max(mp, std::max(std::fabs(ml), std::fabs(mr)));
                        float l = sl + ml, r = sr + mr;
                        o[k * 2] = (short)std::lround(std::max(-1.f, std::min(1.f, l)) * 32767.f);
                        o[k * 2 + 1] = (short)std::lround(std::max(-1.f, std::min(1.f, r)) * 32767.f);
                    }
                    if (sp > sysPeak_) sysPeak_ = sp;
                    if (mp > micPeak_) micPeak_ = mp;
                    ab->Unlock();
                    ab->SetCurrentLength((DWORD)(n * 4));
                    IMFSample* as = nullptr;
                    if (SUCCEEDED(g_mf.CreateSample(&as))) {
                        as->AddBuffer(ab);
                        as->SetSampleTime(aWritten * 10000000LL / kRate);
                        as->SetSampleDuration(n * 10000000LL / kRate);
                        wr->WriteSample(astream, as);
                        as->Release();
                    }
                }
                ab->Release();
            }
            aWritten += n;
            sys.Drop(aWritten, nowIdx);
            mic.Drop(aWritten, nowIdx);
        };
        LONGLONG idx = 0;
        bool haveFrame = false;
        while (!stop_) {
            if (paused_) {  // пауза: ничего не пишем, звук выбрасываем
                pumpAudio(true);
                IDXGIResource* res = nullptr;
                DXGI_OUTDUPL_FRAME_INFO fi = {};
                if (SUCCEEDED(dup->AcquireNextFrame(30, &fi, &res))) { Rel(res); dup->ReleaseFrame(); }
                continue;
            }
            LONGLONG target = idx * freq / fps;
            LONGLONG now = mediaQpc();
            UINT waitMs = now < target ? (UINT)std::min<LONGLONG>(15, (target - now) * 1000 / freq) : 0;
            DXGI_OUTDUPL_FRAME_INFO fi = {};
            IDXGIResource* res = nullptr;
            hr = dup->AcquireNextFrame(waitMs, &fi, &res);
            if (SUCCEEDED(hr)) {
                bool winOk = true;
                if (opt_.win) {  // окно могли подвинуть — едем за ним (размер кадра остаётся прежним)
                    RECT cr;
                    if (!WinRect(opt_.win, cr)) { stop_ = true; winOk = false; }
                    else if (IsIconic(opt_.win)) winOk = false;  // свернули — держим последний кадр
                    else {
                        LONG dw = desk.right - desk.left, dh = desk.bottom - desk.top;
                        lx = std::max(0L, std::min(cr.left - desk.left, dw - (LONG)w));
                        ly = std::max(0L, std::min(cr.top - desk.top, dh - (LONG)h));
                    }
                }
                if (winOk && (fi.LastPresentTime.QuadPart != 0 || !haveFrame || opt_.win)) {
                    ID3D11Texture2D* tex = nullptr;
                    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                        D3D11_BOX box = {(UINT)lx, (UINT)ly, 0, (UINT)(lx + w), (UINT)(ly + h), 1};
                        ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, &box);
                        D3D11_MAPPED_SUBRESOURCE m;
                        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                            for (int y = 0; y < h; y++) memcpy(frameBits + (size_t)y * w * 4, (BYTE*)m.pData + (size_t)y * m.RowPitch, (size_t)w * 4);
                            ctx->Unmap(staging, 0);
                            haveFrame = true;
                        }
                        tex->Release();
                    }
                }
                Rel(res);
                dup->ReleaseFrame();
            } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                // смена режима/UAC — пересоздаём дубликацию
                Rel(dup);
                Sleep(100);
                if (FAILED(out1->DuplicateOutput(dev, &dup))) { err = L"Захват экрана прерван"; break; }
                continue;
            } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
                err = L"Ошибка захвата экрана";
                break;
            }
            pumpAudio(false);
            if (mediaQpc() < target || !haveFrame) continue;

            memcpy(outBits, frameBits, frameBytes);
            CURSORINFO ci = {sizeof(ci)};
            if (opt_.cursor && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii = {};
                if (GetIconInfo(ci.hCursor, &ii)) {
                    int cx = ci.ptScreenPos.x - (desk.left + lx) - (int)ii.xHotspot;
                    int cy = ci.ptScreenPos.y - (desk.top + ly) - (int)ii.yHotspot;
                    DrawIconEx(outDC, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                }
            }
            GdiFlush();
            IMFMediaBuffer* buf = nullptr;
            IMFSample* smp = nullptr;
            if (SUCCEEDED(g_mf.CreateMemoryBuffer((DWORD)frameBytes, &buf))) {
                BYTE* p = nullptr;
                if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
                    memcpy(p, outBits, frameBytes);
                    buf->Unlock();
                    buf->SetCurrentLength((DWORD)frameBytes);
                    if (SUCCEEDED(g_mf.CreateSample(&smp))) {
                        smp->AddBuffer(buf);
                        smp->SetSampleTime(idx * dur);
                        smp->SetSampleDuration(dur);
                        if (SUCCEEDED(wr->WriteSample(stream, smp))) wroteAny = true;
                        smp->Release();
                    }
                }
                buf->Release();
            }
            idx++;
            // отстаём больше чем на 2 кадра — пропускаем, чтобы видео не «замедлялось»
            LONGLONG behind = mediaQpc() * fps / freq;
            if (behind > idx + 2) idx = behind;
        }
    } while (false);

    sys.Close();
    mic.Close();
    bool ok = false;
    if (wr) {
        ok = wroteAny && SUCCEEDED(wr->Finalize());
        wr->Release();
    }
    if (frameDC) { SelectObject(frameDC, frameOld); DeleteDC(frameDC); }
    if (outDC) { SelectObject(outDC, outOld); DeleteDC(outDC); }
    if (frameBmp) DeleteObject(frameBmp);
    if (outBmp) DeleteObject(outBmp);
    Rel(staging);
    Rel(dup);
    Rel(out1);
    Rel(ctx);
    Rel(dev);
    Rel(output);
    Rel(adapter);
    Rel(fac);
    if (mfOk) g_mf.Shutdown();
    CoUninitialize();
    if (!ok) {
        DeleteFileW(path_.c_str());
        if (err.empty()) err = L"Запись слишком короткая";
    }
    active_ = false;
    std::wstring* msg = new std::wstring(ok ? path_ : err);
    if (!PostMessageW(notify_, WM_APP_REC, ok ? 1 : 0, (LPARAM)msg)) delete msg;
}
