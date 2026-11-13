// tools.cpp — захват экрана, оверлей выделения, пипетка, PNG, OCR (Windows.Media.Ocr), QR (quirc)
#include "tools.h"
#include <windowsx.h>
#include <wincodec.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
extern "C" {
#include "third_party/quirc/quirc.h"
}

template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// Оверлей выделения
// ---------------------------------------------------------------------------
struct Overlay {
    HWND wnd = nullptr, notify = nullptr;
    int mode = 0;
    int vx = 0, vy = 0, vw = 0, vh = 0;
    HDC shotDC = nullptr, dimDC = nullptr, backDC = nullptr;
    HBITMAP shotBmp = nullptr, dimBmp = nullptr, backBmp = nullptr;
    HGDIOBJ shotOld = nullptr, dimOld = nullptr, backOld = nullptr;
    BYTE* shotBits = nullptr;
    bool dragging = false;
    POINT a = {}, b = {}, cur = {};
    HFONT font = nullptr, fontSmall = nullptr;
};
static Overlay g_ov;

static HBITMAP MakeDib(HDC dc, int w, int h, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void FreeOverlay() {
    Overlay& o = g_ov;
    auto freeDC = [](HDC& dc, HBITMAP& bmp, HGDIOBJ& old) {
        if (dc) { SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
    };
    freeDC(o.shotDC, o.shotBmp, o.shotOld);
    freeDC(o.dimDC, o.dimBmp, o.dimOld);
    freeDC(o.backDC, o.backBmp, o.backOld);
    if (o.font) { DeleteObject(o.font); o.font = nullptr; }
    if (o.fontSmall) { DeleteObject(o.fontSmall); o.fontSmall = nullptr; }
    o.shotBits = nullptr;
}

static bool CaptureVirtual() {
    Overlay& o = g_ov;
    o.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    o.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    o.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    o.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (o.vw <= 0 || o.vh <= 0) return false;
    HDC scr = GetDC(nullptr);
    void* bits = nullptr;
    o.shotDC = CreateCompatibleDC(scr);
    o.shotBmp = MakeDib(scr, o.vw, o.vh, &bits);
    if (!o.shotBmp) { ReleaseDC(nullptr, scr); return false; }
    o.shotOld = SelectObject(o.shotDC, o.shotBmp);
    BitBlt(o.shotDC, 0, 0, o.vw, o.vh, scr, o.vx, o.vy, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    o.shotBits = (BYTE*)bits;
    ReleaseDC(nullptr, scr);
    return true;
}

static RECT MonitorRectAt(POINT screenPt) {
    HMONITOR m = MonitorFromPoint(screenPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    return mi.rcMonitor;
}

static void Crop(const RECT& vr, ToolResult* r) {
    Overlay& o = g_ov;
    RECT c = {std::max(vr.left - o.vx, 0L), std::max(vr.top - o.vy, 0L), std::min(vr.right - o.vx, (LONG)o.vw),
              std::min(vr.bottom - o.vy, (LONG)o.vh)};
    r->w = std::max(0L, c.right - c.left);
    r->h = std::max(0L, c.bottom - c.top);
    r->rc = {c.left + o.vx, c.top + o.vy, c.right + o.vx, c.bottom + o.vy};
    r->px.resize((size_t)r->w * r->h * 4);
    for (int y = 0; y < r->h; y++)
        memcpy(&r->px[(size_t)y * r->w * 4], o.shotBits + ((size_t)(c.top + y) * o.vw + c.left) * 4, (size_t)r->w * 4);
    for (size_t i = 3; i < r->px.size(); i += 4) r->px[i] = 255;
}

static void Finish(ToolResult* r) {
    HWND n = g_ov.notify;
    if (g_ov.wnd) { HWND w = g_ov.wnd; g_ov.wnd = nullptr; DestroyWindow(w); }
    FreeOverlay();
    if (!PostMessageW(n, WM_APP_TOOL, 0, (LPARAM)r)) delete r;
}
static void Cancel() {
    ToolResult* r = new ToolResult;
    r->mode = g_ov.mode;
    r->cancel = true;
    Finish(r);
}

static RECT SelRect() {
    RECT r = {std::min(g_ov.a.x, g_ov.b.x), std::min(g_ov.a.y, g_ov.b.y), std::max(g_ov.a.x, g_ov.b.x),
              std::max(g_ov.a.y, g_ov.b.y)};
    return r;
}

static void DrawLabel(HDC dc, const wchar_t* text, int x, int y, HFONT f, COLORREF bg, COLORREF fg) {
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    RECT r = {x, y, x + sz.cx + 16, y + sz.cy + 8};
    HBRUSH b = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 12, 12);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    TextOutW(dc, x + 8, y + 4, text, (int)wcslen(text));
    SelectObject(dc, old);
}

static const wchar_t* ModeTitle(int m) {
    switch (m) {
        case TM_SHOT_AREA: return L"Скриншот: выдели область · клик — весь экран · Esc — отмена";
        case TM_OCR: return L"Текст с экрана: выдели область с текстом · Esc — отмена";
        case TM_QR: return L"QR-код: выдели код · клик — искать на всём экране · Esc — отмена";
        case TM_PICK: return L"Пипетка: кликни по цвету · Esc — отмена";
        case TM_REC: return L"Запись: выдели область · клик — весь экран · Esc — отмена";
    }
    return L"";
}

static void Paint(HDC dc) {
    Overlay& o = g_ov;
    HDC bd = o.backDC;
    if (o.mode == TM_PICK) {
        BitBlt(bd, 0, 0, o.vw, o.vh, o.shotDC, 0, 0, SRCCOPY);
    } else {
        BitBlt(bd, 0, 0, o.vw, o.vh, o.dimDC, 0, 0, SRCCOPY);
        if (o.dragging) {
            RECT r = SelRect();
            BitBlt(bd, r.left, r.top, r.right - r.left, r.bottom - r.top, o.shotDC, r.left, r.top, SRCCOPY);
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(140, 128, 255));
            HGDIOBJ op = SelectObject(bd, pen), ob = SelectObject(bd, GetStockObject(NULL_BRUSH));
            Rectangle(bd, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
            SelectObject(bd, op);
            SelectObject(bd, ob);
            DeleteObject(pen);
            wchar_t b[64];
            swprintf(b, 64, L"%d × %d", (int)(r.right - r.left), (int)(r.bottom - r.top));
            int ly = r.top - 34 > 0 ? r.top - 34 : r.bottom + 6;
            DrawLabel(bd, b, r.left, ly, o.fontSmall, RGB(20, 20, 26), RGB(240, 240, 245));
        }
    }
    // подсказка сверху по центру монитора с курсором
    POINT sp = {o.cur.x + o.vx, o.cur.y + o.vy};
    RECT mr = MonitorRectAt(sp);
    const wchar_t* t = ModeTitle(o.mode);
    HGDIOBJ of = SelectObject(bd, o.font);
    SIZE sz;
    GetTextExtentPoint32W(bd, t, (int)wcslen(t), &sz);
    SelectObject(bd, of);
    DrawLabel(bd, t, (mr.left + mr.right) / 2 - o.vx - (sz.cx + 16) / 2, mr.top - o.vy + 24, o.font, RGB(14, 14, 18),
              RGB(245, 245, 250));

    if (o.mode == TM_PICK) {
        // лупа: 15×15 пикселей вокруг курсора, увеличено в 9 раз
        const int N = 15, Z = 9, S = N * Z;
        int x = o.cur.x + 24, y = o.cur.y + 24;
        if (x + S + 10 > o.vw) x = o.cur.x - S - 24;
        if (y + S + 50 > o.vh) y = o.cur.y - S - 60;
        SetStretchBltMode(bd, COLORONCOLOR);
        HBRUSH frame = CreateSolidBrush(RGB(20, 20, 26));
        RECT fr = {x - 4, y - 4, x + S + 4, y + S + 4};
        FillRect(bd, &fr, frame);
        DeleteObject(frame);
        StretchBlt(bd, x, y, S, S, o.shotDC, o.cur.x - N / 2, o.cur.y - N / 2, N, N, SRCCOPY);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(bd, pen), ob = SelectObject(bd, GetStockObject(NULL_BRUSH));
        Rectangle(bd, x + (N / 2) * Z, y + (N / 2) * Z, x + (N / 2 + 1) * Z + 1, y + (N / 2 + 1) * Z + 1);
        SelectObject(bd, op);
        SelectObject(bd, ob);
        DeleteObject(pen);
        COLORREF c = GetPixel(o.shotDC, o.cur.x, o.cur.y);
        wchar_t b[48];
        swprintf(b, 48, L"#%02X%02X%02X   %d, %d, %d", GetRValue(c), GetGValue(c), GetBValue(c), GetRValue(c),
                 GetGValue(c), GetBValue(c));
        DrawLabel(bd, b, x - 4, y + S + 10, o.fontSmall, RGB(20, 20, 26), RGB(240, 240, 245));
        HBRUSH sw = CreateSolidBrush(c);
        RECT swr = {x + S - 26, y + S + 14, x + S, y + S + 34};
        FillRect(bd, &swr, sw);
        DeleteObject(sw);
    }
    BitBlt(dc, 0, 0, o.vw, o.vh, bd, 0, 0, SRCCOPY);
}

static LRESULT CALLBACK OverlayProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay& o = g_ov;
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(w, &ps);
            if (o.backDC) Paint(dc);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_SETCURSOR: SetCursor(LoadCursor(nullptr, IDC_CROSS)); return TRUE;
        case WM_MOUSEMOVE:
            o.cur = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (o.dragging) o.b = o.cur;
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            o.cur = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (o.mode == TM_PICK) {
                ToolResult* r = new ToolResult;
                r->mode = TM_PICK;
                r->color = GetPixel(o.shotDC, o.cur.x, o.cur.y);
                Finish(r);
                return 0;
            }
            o.dragging = true;
            o.a = o.b = o.cur;
            SetCapture(w);
            return 0;
        case WM_LBUTTONUP: {
            if (!o.dragging) return 0;
            ReleaseCapture();
            o.dragging = false;
            o.b = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT r = SelRect();
            ToolResult* res = new ToolResult;
            res->mode = o.mode;
            RECT vr;
            if (r.right - r.left < 6 && r.bottom - r.top < 6) {
                POINT sp = {o.b.x + o.vx, o.b.y + o.vy};
                vr = MonitorRectAt(sp);  // просто клик — весь монитор
            } else {
                vr = {r.left + o.vx, r.top + o.vy, r.right + o.vx, r.bottom + o.vy};
            }
            if (o.mode == TM_REC) res->rc = vr;
            else Crop(vr, res);
            Finish(res);
            return 0;
        }
        case WM_RBUTTONUP: Cancel(); return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Cancel();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && o.wnd) Cancel();
            return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

bool ToolActive() { return g_ov.wnd != nullptr; }

bool StartTool(HWND notify, int mode) {
    if (g_ov.wnd) return false;
    g_ov = Overlay{};
    g_ov.notify = notify;
    g_ov.mode = mode;
    if (!CaptureVirtual()) { FreeOverlay(); return false; }
    if (mode == TM_SHOT_FULL) {
        POINT p;
        GetCursorPos(&p);
        ToolResult* r = new ToolResult;
        r->mode = TM_SHOT_FULL;
        Crop(MonitorRectAt(p), r);
        Finish(r);
        return true;
    }
    // затемнённая копия
    HDC scr = GetDC(nullptr);
    void* bits = nullptr;
    g_ov.dimDC = CreateCompatibleDC(scr);
    g_ov.dimBmp = MakeDib(scr, g_ov.vw, g_ov.vh, &bits);
    g_ov.dimOld = SelectObject(g_ov.dimDC, g_ov.dimBmp);
    if (bits) {
        BYTE* d = (BYTE*)bits;
        size_t n = (size_t)g_ov.vw * g_ov.vh * 4;
        for (size_t i = 0; i < n; i++) d[i] = (BYTE)((g_ov.shotBits[i] * 115) >> 8);
    }
    void* bb = nullptr;
    g_ov.backDC = CreateCompatibleDC(scr);
    g_ov.backBmp = MakeDib(scr, g_ov.vw, g_ov.vh, &bb);
    g_ov.backOld = SelectObject(g_ov.backDC, g_ov.backBmp);
    ReleaseDC(nullptr, scr);

    POINT p;
    GetCursorPos(&p);
    UINT dpi = 96, dy = 96;
    HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    typedef HRESULT(WINAPI * GetDpiFn)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll")) {
        if (auto f = (GetDpiFn)GetProcAddress(sh, "GetDpiForMonitor")) f(hm, 0, &dpi, &dy);
    }
    int fh = -MulDiv(15, dpi, 72) * 3 / 4, fs = -MulDiv(13, dpi, 72) * 3 / 4;
    g_ov.font = CreateFontW(fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_ov.fontSmall = CreateFontW(fs, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_ov.cur = {p.x - g_ov.vx, p.y - g_ov.vy};

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DerkIslandOverlay";
        RegisterClassExW(&wc);
        reg = true;
    }
    g_ov.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"DerkIslandOverlay", L"", WS_POPUP, g_ov.vx, g_ov.vy,
                               g_ov.vw, g_ov.vh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_ov.wnd) { FreeOverlay(); return false; }
    ShowWindow(g_ov.wnd, SW_SHOW);
    SetForegroundWindow(g_ov.wnd);
    SetFocus(g_ov.wnd);
    return true;
}

// ---------------------------------------------------------------------------
// PNG и буфер обмена
// ---------------------------------------------------------------------------
bool SavePng(const std::wstring& path, const std::vector<BYTE>& bgra, int w, int h) {
    // кодируем в память, а файл пишем сами — так надёжно с любыми именами (кириллица, пробелы)
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&wic);
    if (!wic) return false;
    IStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    do {
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) break;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr))) break;
        fr->SetSize(w, h);
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&pf);
        if (FAILED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), (BYTE*)bgra.data()))) break;
        if (FAILED(fr->Commit()) || FAILED(enc->Commit())) break;
        HGLOBAL hg = nullptr;
        if (FAILED(GetHGlobalFromStream(st, &hg))) break;
        STATSTG ss = {};
        st->Stat(&ss, STATFLAG_NONAME);
        void* p = GlobalLock(hg);
        if (!p) break;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            ok = WriteFile(f, p, (DWORD)ss.cbSize.QuadPart, &wr, nullptr) && wr == (DWORD)ss.cbSize.QuadPart;
            CloseHandle(f);
        }
        GlobalUnlock(hg);
    } while (false);
    Rel(fr);
    Rel(enc);
    Rel(st);
    Rel(wic);
    return ok;
}

static bool OpenCb(HWND owner) {
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(owner)) return true;
        Sleep(15);
    }
    return false;
}
bool CopyImageToClipboard(HWND owner, const std::vector<BYTE>& px, int w, int h) {
    if (!OpenCb(owner)) return false;
    EmptyClipboard();
    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = h;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    size_t stride = (size_t)w * 4;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(bih) + stride * h);
    if (g) {
        BYTE* p = (BYTE*)GlobalLock(g);
        memcpy(p, &bih, sizeof(bih));
        for (int y = 0; y < h; y++) memcpy(p + sizeof(bih) + stride * (h - 1 - y), &px[stride * y], stride);
        GlobalUnlock(g);
        SetClipboardData(CF_DIB, g);
    }
    CloseClipboard();
    return g != nullptr;
}
bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenCb(owner)) return false;
    EmptyClipboard();
    size_t n = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (g) {
        memcpy(GlobalLock(g), text.c_str(), n);
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
    return g != nullptr;
}

// ---------------------------------------------------------------------------
// QR (quirc)
// ---------------------------------------------------------------------------
static std::wstring FromU8(const char* s, size_t n) {
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) { cp = 1251; flags = 0; len = MultiByteToWideChar(cp, 0, s, (int)n, nullptr, 0); }  // старые QR в cp1251
    std::wstring w(std::max(0, len), L'\0');
    if (len > 0) MultiByteToWideChar(cp, flags, s, (int)n, &w[0], len);
    return w;
}
static bool QrPass(const std::vector<BYTE>& gray, int w, int h, std::wstring& out) {
    struct quirc* q = quirc_new();
    if (!q) return false;
    bool found = false;
    if (quirc_resize(q, w, h) >= 0) {
        uint8_t* buf = quirc_begin(q, nullptr, nullptr);
        memcpy(buf, gray.data(), (size_t)w * h);
        quirc_end(q);
        int n = quirc_count(q);
        for (int i = 0; i < n && !found; i++) {
            struct quirc_code code;
            struct quirc_data data;
            quirc_extract(q, i, &code);
            if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
                quirc_flip(&code);  // зеркальные коды
                if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
            }
            out = FromU8((const char*)data.payload, data.payload_len);
            found = true;
        }
    }
    quirc_destroy(q);
    return found;
}
bool DecodeQr(const std::vector<BYTE>& px, int w, int h, std::wstring& out) {
    if (w < 21 || h < 21) return false;
    std::vector<BYTE> g((size_t)w * h);
    for (size_t i = 0; i < g.size(); i++) g[i] = (BYTE)((px[i * 4] * 29 + px[i * 4 + 1] * 150 + px[i * 4 + 2] * 77) >> 8);
    if (QrPass(g, w, h, out)) return true;
    // светлый код на тёмном фоне
    std::vector<BYTE> inv(g.size());
    for (size_t i = 0; i < g.size(); i++) inv[i] = 255 - g[i];
    if (QrPass(inv, w, h, out)) return true;
    // мелкий код — увеличиваем в 2 раза
    if ((long long)w * h < 1500000) {
        int W = w * 2, H = h * 2;
        std::vector<BYTE> up((size_t)W * H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) up[(size_t)y * W + x] = g[(size_t)(y / 2) * w + x / 2];
        if (QrPass(up, W, H, out)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// OCR: Windows.Media.Ocr через «сырой» WinRT ABI
// ---------------------------------------------------------------------------
struct IAsyncInfoR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};
struct IAsyncOpR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(void*) = 0;
};
struct IOcrEngineStaticsR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MaxImageDimension(UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_AvailableRecognizerLanguages(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLanguageSupported(IInspectable* lang, unsigned char* r) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromLanguage(IInspectable* lang, IInspectable** engine) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryCreateFromUserProfileLanguages(IInspectable** engine) = 0;
};
struct IOcrEngineR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RecognizeAsync(IInspectable* bitmap, IInspectable** op) = 0;
};
struct IOcrResultR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Lines(IInspectable** lines) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_TextAngle(IInspectable** a) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING* t) = 0;
};
struct IOcrLineR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Words(IInspectable** w) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Text(HSTRING* t) = 0;
};
struct IVectorViewR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 i, IInspectable** item) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* n) = 0;
};
struct ILanguageFactoryR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateLanguage(HSTRING tag, IInspectable** lang) = 0;
};
struct ISoftwareBitmapFactoryR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Create(int format, int w, int h, IInspectable** bmp) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateWithAlpha(int format, int w, int h, int alpha, IInspectable** bmp) = 0;
};
struct ISoftwareBitmapR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_BitmapPixelFormat(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapAlphaMode(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PixelWidth(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PixelHeight(int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsReadOnly(unsigned char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DpiX(double) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DpiX(double*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DpiY(double) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DpiY(double*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LockBuffer(int mode, IInspectable** buffer) = 0;
};
struct PlaneDesc { INT32 start, width, height, stride; };
struct IBitmapBufferR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPlaneCount(INT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPlaneDescription(INT32 i, PlaneDesc* d) = 0;
};
struct IMemoryBufferR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateReference(IInspectable** ref) = 0;
};
struct IMemoryBufferByteAccessR : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** value, UINT32* capacity) = 0;
};
struct IClosableR : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

static const IID kIID_AsyncInfo = {0x00000036, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const IID kIID_OcrStatics = {0x5bffa85a, 0x3384, 0x3540, {0x99, 0x40, 0x69, 0x91, 0x20, 0xd4, 0x28, 0xa8}};
static const IID kIID_OcrEngine = {0x5a14bc41, 0x5b76, 0x3140, {0xb6, 0x80, 0x88, 0x25, 0x56, 0x26, 0x83, 0xac}};
static const IID kIID_OcrResult = {0x9bd235b2, 0x175b, 0x3d6a, {0x92, 0xe2, 0x38, 0x8c, 0x20, 0x6e, 0x2f, 0x63}};
static const IID kIID_OcrLine = {0x0043a16f, 0xe31f, 0x3a24, {0x89, 0x9c, 0xd4, 0x44, 0xbd, 0x08, 0x81, 0x24}};
static const IID kIID_LangFactory = {0x9b0252ac, 0x0c27, 0x44f8, {0xb7, 0x92, 0x97, 0x93, 0xfb, 0x66, 0xc6, 0x3e}};
static const IID kIID_SbFactory = {0xc99feb69, 0x2d62, 0x4d47, {0xa6, 0xb3, 0x4f, 0xdb, 0x6a, 0x07, 0xfd, 0xf8}};
static const IID kIID_Sb = {0x689e0708, 0x7eef, 0x483f, {0x96, 0x3f, 0xda, 0x93, 0x88, 0x18, 0xe0, 0x73}};
static const IID kIID_BitmapBuffer = {0xa53e04c4, 0x399c, 0x438c, {0xb2, 0x8f, 0xa6, 0x3a, 0x6b, 0x83, 0xd1, 0xa1}};
static const IID kIID_MemoryBuffer = {0xfbc4dd2a, 0x245b, 0x11e4, {0xaf, 0x98, 0x68, 0x94, 0x23, 0x26, 0x0c, 0xf8}};
static const IID kIID_ByteAccess = {0x5b0d3235, 0x4dba, 0x4d44, {0x86, 0x5e, 0x8f, 0x1d, 0x0e, 0x4f, 0xd0, 0x4d}};
static const IID kIID_Closable = {0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};

template <class T> static T* QIr(IUnknown* p, const IID& iid) {
    T* r = nullptr;
    if (p) p->QueryInterface(iid, (void**)&r);
    return r;
}
static HSTRING HStr(const wchar_t* s) {
    HSTRING h = nullptr;
    WindowsCreateString(s, (UINT32)wcslen(s), &h);
    return h;
}
static std::wstring TakeH(HSTRING h) {
    if (!h) return L"";
    UINT32 n = 0;
    const wchar_t* s = WindowsGetStringRawBuffer(h, &n);
    std::wstring r(s ? s : L"", s ? n : 0);
    WindowsDeleteString(h);
    return r;
}
template <class T> static bool AwaitOp(IInspectable* op, T* out, DWORD timeoutMs) {
    IAsyncInfoR* info = QIr<IAsyncInfoR>(op, kIID_AsyncInfo);
    if (!info) return false;
    int st = 0;
    DWORD t0 = GetTickCount();
    while (SUCCEEDED(info->get_Status(&st)) && st == 0) {
        if (GetTickCount() - t0 > timeoutMs) { info->Cancel(); break; }
        Sleep(5);
    }
    info->Release();
    return st == 1 && SUCCEEDED(static_cast<IAsyncOpR*>(op)->GetResults(out));
}
template <class T> static T* Factory(const wchar_t* cls, const IID& iid) {
    HSTRING h = HStr(cls);
    T* f = nullptr;
    RoGetActivationFactory(h, iid, (void**)&f);
    WindowsDeleteString(h);
    return f;
}

static IOcrEngineR* EngineFor(IOcrEngineStaticsR* st, ILanguageFactoryR* lf, const wchar_t* tag) {
    if (!st || !lf) return nullptr;
    IInspectable* lang = nullptr;
    HSTRING h = HStr(tag);
    lf->CreateLanguage(h, &lang);
    WindowsDeleteString(h);
    if (!lang) return nullptr;
    unsigned char ok = 0;
    IOcrEngineR* eng = nullptr;
    if (SUCCEEDED(st->IsLanguageSupported(lang, &ok)) && ok) {
        IInspectable* e = nullptr;
        if (SUCCEEDED(st->TryCreateFromLanguage(lang, &e)) && e) { eng = QIr<IOcrEngineR>(e, kIID_OcrEngine); e->Release(); }
    }
    lang->Release();
    return eng;
}

static IInspectable* MakeSoftwareBitmap(const std::vector<BYTE>& px, int w, int h) {
    ISoftwareBitmapFactoryR* f = Factory<ISoftwareBitmapFactoryR>(L"Windows.Graphics.Imaging.SoftwareBitmap", kIID_SbFactory);
    if (!f) return nullptr;
    IInspectable* sbI = nullptr;
    f->CreateWithAlpha(87 /*Bgra8*/, w, h, 0 /*Premultiplied*/, &sbI);
    f->Release();
    ISoftwareBitmapR* sb = QIr<ISoftwareBitmapR>(sbI, kIID_Sb);
    bool ok = false;
    if (sb) {
        IInspectable* bufI = nullptr;
        if (SUCCEEDED(sb->LockBuffer(2 /*Write*/, &bufI)) && bufI) {
            IBitmapBufferR* bb = QIr<IBitmapBufferR>(bufI, kIID_BitmapBuffer);
            IMemoryBufferR* mb = QIr<IMemoryBufferR>(bufI, kIID_MemoryBuffer);
            PlaneDesc pd = {};
            if (bb) bb->GetPlaneDescription(0, &pd);
            IInspectable* ref = nullptr;
            if (mb && SUCCEEDED(mb->CreateReference(&ref)) && ref) {
                IMemoryBufferByteAccessR* ba = QIr<IMemoryBufferByteAccessR>(ref, kIID_ByteAccess);
                BYTE* dst = nullptr;
                UINT32 cap = 0;
                if (ba && SUCCEEDED(ba->GetBuffer(&dst, &cap)) && dst) {
                    int stride = pd.stride > 0 ? pd.stride : w * 4;
                    for (int y = 0; y < h; y++) {
                        size_t off = (size_t)pd.start + (size_t)y * stride;
                        if (off + (size_t)w * 4 > cap) break;
                        memcpy(dst + off, &px[(size_t)y * w * 4], (size_t)w * 4);
                    }
                    ok = true;
                }
                Rel(ba);
                if (IClosableR* c = QIr<IClosableR>(ref, kIID_Closable)) { c->Close(); c->Release(); }
                ref->Release();
            }
            Rel(mb);
            Rel(bb);
            if (IClosableR* c = QIr<IClosableR>(bufI, kIID_Closable)) { c->Close(); c->Release(); }
            bufI->Release();
        }
        sb->Release();
    }
    if (!ok) Rel(sbI);
    return sbI;
}

static std::wstring Recognize(IOcrEngineR* eng, IInspectable* bmp) {
    std::wstring out;
    IInspectable* op = nullptr;
    if (FAILED(eng->RecognizeAsync(bmp, &op)) || !op) return out;
    IInspectable* resI = nullptr;
    if (AwaitOp(op, &resI, 20000) && resI) {
        if (IOcrResultR* res = QIr<IOcrResultR>(resI, kIID_OcrResult)) {
            IInspectable* linesI = nullptr;
            if (SUCCEEDED(res->get_Lines(&linesI)) && linesI) {
                IVectorViewR* v = (IVectorViewR*)linesI;  // IVectorView<OcrLine> — берём по vtable
                UINT32 n = 0;
                v->get_Size(&n);
                for (UINT32 i = 0; i < n; i++) {
                    IInspectable* li = nullptr;
                    if (FAILED(v->GetAt(i, &li)) || !li) continue;
                    if (IOcrLineR* l = QIr<IOcrLineR>(li, kIID_OcrLine)) {
                        HSTRING h = nullptr;
                        if (SUCCEEDED(l->get_Text(&h))) {
                            if (!out.empty()) out += L"\r\n";
                            out += TakeH(h);
                        }
                        l->Release();
                    }
                    li->Release();
                }
                linesI->Release();
            }
            res->Release();
        }
        resI->Release();
    }
    op->Release();
    return out;
}
static int CountCyr(const std::wstring& s, int* latin) {
    int c = 0, l = 0;
    for (wchar_t ch : s) {
        if (ch >= 0x400 && ch <= 0x4FF) c++;
        else if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) l++;
    }
    if (latin) *latin = l;
    return c;
}

bool RunOcr(const std::vector<BYTE>& px0, int w0, int h0, std::wstring& out, std::wstring& err) {
    RoInitialize(RO_INIT_MULTITHREADED);
    bool ok = false;
    IOcrEngineStaticsR* st = Factory<IOcrEngineStaticsR>(L"Windows.Media.Ocr.OcrEngine", kIID_OcrStatics);
    ILanguageFactoryR* lf = Factory<ILanguageFactoryR>(L"Windows.Globalization.Language", kIID_LangFactory);
    IOcrEngineR* ru = EngineFor(st, lf, L"ru");
    IOcrEngineR* en = EngineFor(st, lf, L"en-US");
    if (!en) en = EngineFor(st, lf, L"en");
    IOcrEngineR* any = nullptr;
    if (!ru && !en && st) {
        IInspectable* e = nullptr;
        if (SUCCEEDED(st->TryCreateFromUserProfileLanguages(&e)) && e) { any = QIr<IOcrEngineR>(e, kIID_OcrEngine); e->Release(); }
    }
    if (!ru && !en && !any) {
        err = L"Нет языков OCR — добавь русский/английский в «Язык и регион»";
    } else {
        // мелкий текст распознаётся лучше, если увеличить картинку
        std::vector<BYTE> up;
        const std::vector<BYTE>* px = &px0;
        int w = w0, h = h0;
        UINT32 maxDim = 4096;
        if (st) st->get_MaxImageDimension(&maxDim);
        if (h0 < 120 && w0 * 3 <= (int)maxDim && h0 * 3 <= (int)maxDim) {
            int k = 3;
            w = w0 * k; h = h0 * k;
            up.resize((size_t)w * h * 4);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) memcpy(&up[((size_t)y * w + x) * 4], &px0[((size_t)(y / k) * w0 + x / k) * 4], 4);
            px = &up;
        }
        if ((UINT32)w > maxDim || (UINT32)h > maxDim) {
            err = L"Область слишком большая для OCR";
        } else if (IInspectable* bmp = MakeSoftwareBitmap(*px, w, h)) {
            std::wstring a = ru ? Recognize(ru, bmp) : L"", b = en ? Recognize(en, bmp) : L"";
            if (any) a = Recognize(any, bmp);
            // выбираем вариант: если в русском распознавании много кириллицы — он, иначе английский
            int la = 0, lb = 0;
            int ca = CountCyr(a, &la), cb = CountCyr(b, &lb);
            (void)cb;
            if (!a.empty() && (b.empty() || ca * 2 >= lb)) out = a;
            else out = b;
            ok = !out.empty();
            if (!ok) err = L"Текст не найден";
            bmp->Release();
        } else {
            err = L"Не удалось подготовить картинку";
        }
    }
    Rel(ru);
    Rel(en);
    Rel(any);
    Rel(lf);
    Rel(st);
    RoUninitialize();
    return ok;
}
