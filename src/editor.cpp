// editor.cpp — редактор разметки скриншота на Direct2D.
// Окно обычное (можно таскать, сворачивать), рисует только по событиям — в покое ничего не тратит.
#include "editor.h"
#include "tools.h"
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <commdlg.h>
#include <cmath>
#include <algorithm>
#include <memory>

namespace {

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

enum Tool { TL_ARROW, TL_RECT, TL_MARKER, TL_PEN, TL_TEXT, TL_NUM, TL_BLUR, TL_CROP, TL_COUNT };
const wchar_t* kToolTips[TL_COUNT] = {L"Стрелка (A)", L"Рамка (R)", L"Маркер (M)", L"Карандаш (P)", L"Текст (T)",
                                      L"Номер (N)", L"Размытие (B)", L"Обрезка (C)"};
const wchar_t kToolKeys[TL_COUNT] = {L'A', L'R', L'M', L'P', L'T', L'N', L'B', L'C'};
const D2D1_COLOR_F kPal[6] = {{1.f, 0.23f, 0.19f, 1}, {1.f, 0.80f, 0.f, 1}, {0.20f, 0.78f, 0.35f, 1},
                              {0.04f, 0.52f, 1.f, 1},  {1.f, 1.f, 1.f, 1},   {0.08f, 0.08f, 0.09f, 1}};

struct Shape {
    int type = TL_ARROW;
    std::vector<D2D1_POINT_2F> pts;  // в пикселях картинки
    D2D1_COLOR_F color = {1, 0, 0, 1};
    float width = 4;
    std::wstring text;
    int number = 0;
    // размытие: готовые «пиксели» области
    std::vector<BYTE> blurPx;
    int bx = 0, by = 0, bw = 0, bh = 0;
    ID2D1Bitmap* blurBmp = nullptr;
};

struct Editor {
    HWND hwnd = nullptr, notify = nullptr, edit = nullptr;
    std::vector<BYTE> px;
    int w = 0, h = 0;
    std::wstring shotDir;
    bool light = false;
    D2D1_COLOR_F accent = {0.45f, 0.4f, 1.f, 1};
    ID2D1Factory* f = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* br = nullptr;
    ID2D1Bitmap* base = nullptr;
    IDWriteTextFormat* fUi = nullptr;
    std::vector<std::unique_ptr<Shape>> shapes, redo;
    std::unique_ptr<Shape> cur;
    int tool = TL_ARROW, color = 0, size = 1;
    float dpi = 96;
    // вид
    float zoom = 1, offX = 0, offY = 0;  // DIP
    int hot = -1, pressBtn = -1;
    bool dragging = false;
    POINT dragStart = {};
    D2D1_POINT_2F textAt = {};
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    struct Btn { D2D1_RECT_F r; int id; };
    std::vector<Btn> btns;
    bool sent = false;
};
Editor* g_ed = nullptr;

enum BtnId { B_TOOL = 0, B_COLOR = 100, B_SIZE = 200, B_UNDO = 300, B_REDO, B_DRAG, B_COPY, B_SAVE, B_DONE, B_CANCEL };
const float kTop = 56, kBottom = 60;

D2D1_COLOR_F C(float r, float g, float b, float a = 1) { return {r, g, b, a}; }
D2D1_RECT_F R(float x, float y, float w, float h) { return {x, y, x + w, y + h}; }
D2D1_COLOR_F Fg() { return g_ed->light ? C(0.08f, 0.08f, 0.1f) : C(0.96f, 0.96f, 0.97f); }
D2D1_COLOR_F Gray() { return g_ed->light ? C(0.4f, 0.4f, 0.45f) : C(0.62f, 0.62f, 0.67f); }
D2D1_COLOR_F Ov(float a) { return g_ed->light ? C(0, 0, 0, a * 0.8f) : C(1, 1, 1, a); }
D2D1_COLOR_F Bg() { return g_ed->light ? C(0.93f, 0.93f, 0.95f) : C(0.07f, 0.07f, 0.08f); }

float ImgUnit() { return std::max(1.f, std::min(g_ed->w, g_ed->h) / 420.f); }
float StrokeW() { static const float s[3] = {2.f, 3.5f, 6.f}; return s[g_ed->size] * ImgUnit(); }

void Brush(ID2D1RenderTarget* t, D2D1_COLOR_F c) {
    if (t == g_ed->rt) { g_ed->br->SetColor(c); return; }
}

// --------------------------------------------------------------- размытие (пикселизация)
void MakeBlur(Shape& s) {
    float x0 = std::min(s.pts[0].x, s.pts[1].x), y0 = std::min(s.pts[0].y, s.pts[1].y);
    float x1 = std::max(s.pts[0].x, s.pts[1].x), y1 = std::max(s.pts[0].y, s.pts[1].y);
    int bx = std::max(0, (int)x0), by = std::max(0, (int)y0);
    int ex = std::min(g_ed->w, (int)std::ceil(x1)), ey = std::min(g_ed->h, (int)std::ceil(y1));
    s.bx = bx; s.by = by; s.bw = std::max(0, ex - bx); s.bh = std::max(0, ey - by);
    if (s.bw < 2 || s.bh < 2) { s.bw = s.bh = 0; return; }
    int blk = std::max(8, (int)(std::min(g_ed->w, g_ed->h) / 55));
    s.blurPx.assign((size_t)s.bw * s.bh * 4, 0);
    for (int y = 0; y < s.bh; y += blk)
        for (int x = 0; x < s.bw; x += blk) {
            int bw2 = std::min(blk, s.bw - x), bh2 = std::min(blk, s.bh - y);
            unsigned sum[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = 0; yy < bh2; yy++)
                for (int xx = 0; xx < bw2; xx++) {
                    const BYTE* p = &g_ed->px[((size_t)(by + y + yy) * g_ed->w + bx + x + xx) * 4];
                    for (int k = 0; k < 4; k++) sum[k] += p[k];
                    n++;
                }
            for (int yy = 0; yy < bh2; yy++)
                for (int xx = 0; xx < bw2; xx++) {
                    BYTE* q = &s.blurPx[((size_t)(y + yy) * s.bw + x + xx) * 4];
                    for (int k = 0; k < 3; k++) q[k] = (BYTE)(sum[k] / n);
                    q[3] = 255;
                }
        }
}
ID2D1Bitmap* BmpFrom(ID2D1RenderTarget* t, const std::vector<BYTE>& p, int w, int h) {
    ID2D1Bitmap* b = nullptr;
    D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
    t->CreateBitmap(D2D1::SizeU(w, h), p.data(), w * 4, &bp, &b);
    return b;
}

// --------------------------------------------------------------- рисование фигур (в координатах картинки)
ID2D1PathGeometry* Poly(const std::vector<D2D1_POINT_2F>& pts) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_ed->f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { Rel(g); return nullptr; }
    s->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < pts.size(); i++) s->AddLine(pts[i]);
    if (pts.size() == 1) s->AddLine(D2D1::Point2F(pts[0].x + 0.1f, pts[0].y));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    s->Release();
    return g;
}
void DrawShape(ID2D1RenderTarget* t, ID2D1SolidColorBrush* b, Shape& s, bool forExport) {
    if (s.pts.empty()) return;
    ID2D1StrokeStyle* round = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES sp = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
    g_ed->f->CreateStrokeStyle(&sp, nullptr, 0, &round);
    D2D1_POINT_2F a = s.pts[0], z = s.pts.back();
    switch (s.type) {
        case TL_ARROW: {
            float dx = z.x - a.x, dy = z.y - a.y, len = std::sqrt(dx * dx + dy * dy);
            if (len < 2) break;
            float ux = dx / len, uy = dy / len, hl = std::min(len * 0.6f, s.width * 4.5f + 6), hw = hl * 0.55f;
            D2D1_POINT_2F base = {z.x - ux * hl * 0.8f, z.y - uy * hl * 0.8f};
            b->SetColor(s.color);
            t->DrawLine(a, base, b, s.width, round);
            ID2D1PathGeometry* g = nullptr;
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_ed->f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                sk->BeginFigure(z, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine(D2D1::Point2F(z.x - ux * hl - uy * hw, z.y - uy * hl + ux * hw));
                sk->AddLine(D2D1::Point2F(z.x - ux * hl + uy * hw, z.y - uy * hl - ux * hw));
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();
                sk->Release();
                t->FillGeometry(g, b);
                t->DrawGeometry(g, b, std::max(1.f, s.width * 0.4f), round);
            }
            Rel(g);
            break;
        }
        case TL_RECT: {
            D2D1_RECT_F r = {std::min(a.x, z.x), std::min(a.y, z.y), std::max(a.x, z.x), std::max(a.y, z.y)};
            D2D1_ROUNDED_RECT rr = {r, s.width * 1.5f, s.width * 1.5f};
            b->SetColor(s.color);
            t->DrawRoundedRectangle(&rr, b, s.width, round);
            break;
        }
        case TL_MARKER:
        case TL_PEN: {
            ID2D1PathGeometry* g = Poly(s.pts);
            if (g) {
                D2D1_COLOR_F c = s.color;
                if (s.type == TL_MARKER) c.a = 0.38f;
                b->SetColor(c);
                t->DrawGeometry(g, b, s.type == TL_MARKER ? s.width * 4.5f : s.width, round);
            }
            Rel(g);
            break;
        }
        case TL_NUM: {
            float r = s.width * 2.6f + 9;
            b->SetColor(s.color);
            t->FillEllipse(D2D1::Ellipse(a, r, r), b);
            bool darkText = s.color.r + s.color.g + s.color.b > 2.2f;
            IDWriteTextFormat* tf = nullptr;
            g_ed->dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, r * 1.2f,
                                       L"ru-ru", &tf);
            if (tf) {
                tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                std::wstring n = std::to_wstring(s.number);
                b->SetColor(darkText ? C(0.05f, 0.05f, 0.06f) : C(1, 1, 1));
                t->DrawText(n.c_str(), (UINT32)n.size(), tf, D2D1::RectF(a.x - r, a.y - r, a.x + r, a.y + r), b);
                tf->Release();
            }
            break;
        }
        case TL_TEXT: {
            if (s.text.empty()) break;
            float fs = s.width * 4.5f + 12;
            IDWriteTextFormat* tf = nullptr;
            g_ed->dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fs,
                                       L"ru-ru", &tf);
            if (tf) {
                D2D1_RECT_F r = D2D1::RectF(a.x, a.y, a.x + 4000, a.y + 4000);
                bool lightCol = s.color.r + s.color.g + s.color.b > 1.6f;
                D2D1_COLOR_F sh = lightCol ? C(0, 0, 0, 0.65f) : C(1, 1, 1, 0.75f);
                float o = std::max(1.f, fs / 16);
                b->SetColor(sh);
                for (int k = 0; k < 4; k++) {
                    float ox = (k & 1 ? o : -o), oy = (k & 2 ? o : -o);
                    D2D1_RECT_F rs = {r.left + ox, r.top + oy, r.right + ox, r.bottom + oy};
                    t->DrawText(s.text.c_str(), (UINT32)s.text.size(), tf, rs, b);
                }
                b->SetColor(s.color);
                t->DrawText(s.text.c_str(), (UINT32)s.text.size(), tf, r, b);
                tf->Release();
            }
            break;
        }
        case TL_BLUR: {
            if (!s.bw) break;
            ID2D1Bitmap* bm = nullptr;
            if (forExport) bm = BmpFrom(t, s.blurPx, s.bw, s.bh);
            else {
                if (!s.blurBmp) s.blurBmp = BmpFrom(t, s.blurPx, s.bw, s.bh);
                bm = s.blurBmp;
                if (bm) bm->AddRef();
            }
            if (bm) {
                t->DrawBitmap(bm, D2D1::RectF((float)s.bx, (float)s.by, (float)(s.bx + s.bw), (float)(s.by + s.bh)), 1.f,
                              D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                bm->Release();
            }
            break;
        }
    }
    Rel(round);
}
// активная обрезка = последняя фигура-обрезка
bool CropRect(D2D1_RECT_F& r) {
    const Shape* c = nullptr;
    for (auto& s : g_ed->shapes) if (s->type == TL_CROP) c = s.get();
    if (g_ed->cur && g_ed->cur->type == TL_CROP) c = g_ed->cur.get();
    if (!c || c->pts.size() < 2) return false;
    r = {std::max(0.f, std::min(c->pts[0].x, c->pts[1].x)), std::max(0.f, std::min(c->pts[0].y, c->pts[1].y)),
         std::min((float)g_ed->w, std::max(c->pts[0].x, c->pts[1].x)), std::min((float)g_ed->h, std::max(c->pts[0].y, c->pts[1].y))};
    return r.right - r.left >= 4 && r.bottom - r.top >= 4;
}

// --------------------------------------------------------------- экспорт
bool Export(std::vector<BYTE>& out, int& ow, int& oh) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmap* wb = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateBitmap(g_ed->w, g_ed->h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb))) {
        ID2D1RenderTarget* t = nullptr;
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (SUCCEEDED(g_ed->f->CreateWicBitmapRenderTarget(wb, &rp, &t))) {
            ID2D1SolidColorBrush* b = nullptr;
            t->CreateSolidColorBrush(C(1, 1, 1), &b);
            ID2D1Bitmap* bm = BmpFrom(t, g_ed->px, g_ed->w, g_ed->h);
            t->BeginDraw();
            t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            t->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            if (bm) t->DrawBitmap(bm, D2D1::RectF(0, 0, (float)g_ed->w, (float)g_ed->h), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            for (auto& s : g_ed->shapes) if (s->type != TL_CROP) DrawShape(t, b, *s, true);
            ok = SUCCEEDED(t->EndDraw());
            Rel(bm);
            Rel(b);
            Rel(t);
        }
        if (ok) {
            D2D1_RECT_F cr;
            WICRect rc = {0, 0, g_ed->w, g_ed->h};
            if (CropRect(cr)) rc = {(INT)cr.left, (INT)cr.top, (INT)(cr.right - cr.left), (INT)(cr.bottom - cr.top)};
            out.resize((size_t)rc.Width * rc.Height * 4);
            ok = SUCCEEDED(wb->CopyPixels(&rc, rc.Width * 4, (UINT)out.size(), out.data()));
            ow = rc.Width;
            oh = rc.Height;
            for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;  // скриншот непрозрачный
        }
    }
    Rel(wb);
    Rel(wic);
    return ok;
}

// --------------------------------------------------------------- вид
float Px2Dip() { return 96.f / g_ed->dpi; }
void Layout() {
    RECT rc;
    GetClientRect(g_ed->hwnd, &rc);
    float W = rc.right * Px2Dip(), H = rc.bottom * Px2Dip();
    float aw = W - 40, ah = H - kTop - kBottom - 24;
    float iw = g_ed->w * Px2Dip(), ih = g_ed->h * Px2Dip();  // 1:1 с экраном
    g_ed->zoom = std::min(1.f, std::min(aw / iw, ah / ih));
    if (g_ed->zoom <= 0) g_ed->zoom = 0.1f;
    g_ed->offX = (W - iw * g_ed->zoom) / 2;
    g_ed->offY = kTop + 12 + (ah - ih * g_ed->zoom) / 2;
}
D2D1_POINT_2F ToImg(int x, int y) {
    float s = Px2Dip() * g_ed->zoom;
    return {(x * Px2Dip() - g_ed->offX) / s, (y * Px2Dip() - g_ed->offY) / s};
}
D2D1_POINT_2F ImgToDip(D2D1_POINT_2F p) {
    float s = Px2Dip() * g_ed->zoom;
    return {p.x * s + g_ed->offX, p.y * s + g_ed->offY};
}

void Text(const std::wstring& s, D2D1_RECT_F r, D2D1_COLOR_F c, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_CENTER) {
    g_ed->fUi->SetTextAlignment(al);
    g_ed->br->SetColor(c);
    g_ed->rt->DrawText(s.c_str(), (UINT32)s.size(), g_ed->fUi, r, g_ed->br);
}
void Line(float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c) {
    g_ed->br->SetColor(c);
    g_ed->rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), g_ed->br, w);
}
void RRect(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
    g_ed->br->SetColor(c);
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    g_ed->rt->FillRoundedRectangle(&rr, g_ed->br);
}
void SRect(D2D1_RECT_F r, float rad, float w, D2D1_COLOR_F c) {
    g_ed->br->SetColor(c);
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    g_ed->rt->DrawRoundedRectangle(&rr, g_ed->br, w);
}
void Circ(float x, float y, float r, D2D1_COLOR_F c) {
    g_ed->br->SetColor(c);
    g_ed->rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), g_ed->br);
}
void ToolIcon(int t, float cx, float cy, D2D1_COLOR_F c) {
    switch (t) {
        case TL_ARROW:
            Line(cx - 7, cy + 7, cx + 6, cy - 6, 1.8f, c);
            Line(cx + 7, cy - 7, cx, cy - 7, 1.8f, c);
            Line(cx + 7, cy - 7, cx + 7, cy, 1.8f, c);
            break;
        case TL_RECT: SRect(R(cx - 8, cy - 6, 16, 12), 2.5f, 1.7f, c); break;
        case TL_MARKER:
            g_ed->br->SetColor(C(c.r, c.g, c.b, 0.45f));
            g_ed->rt->DrawLine(D2D1::Point2F(cx - 7, cy + 2), D2D1::Point2F(cx + 7, cy + 2), g_ed->br, 6);
            Line(cx - 7, cy - 5, cx + 4, cy - 5, 1.5f, c);
            break;
        case TL_PEN: {
            ID2D1PathGeometry* g = nullptr;
            ID2D1GeometrySink* s = nullptr;
            if (SUCCEEDED(g_ed->f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
                s->BeginFigure(D2D1::Point2F(cx - 8, cy + 4), D2D1_FIGURE_BEGIN_HOLLOW);
                D2D1_BEZIER_SEGMENT bz = {D2D1::Point2F(cx - 3, cy - 10), D2D1::Point2F(cx + 2, cy + 10), D2D1::Point2F(cx + 8, cy - 4)};
                s->AddBezier(&bz);
                s->EndFigure(D2D1_FIGURE_END_OPEN);
                s->Close();
                s->Release();
                g_ed->br->SetColor(c);
                g_ed->rt->DrawGeometry(g, g_ed->br, 1.8f);
            }
            Rel(g);
            break;
        }
        case TL_TEXT:
            Line(cx - 6, cy - 7, cx + 6, cy - 7, 2, c);
            Line(cx, cy - 7, cx, cy + 7, 2, c);
            break;
        case TL_NUM: {
            g_ed->br->SetColor(c);
            g_ed->rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8, 8), g_ed->br, 1.6f);
            Text(L"1", R(cx - 8, cy - 9, 16, 18), c);
            break;
        }
        case TL_BLUR:
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++)
                    RRect(R(cx - 8 + i * 5.5f, cy - 8 + j * 5.5f, 4.5f, 4.5f), 1, C(c.r, c.g, c.b, (i + j) % 2 ? 0.45f : 1.f));
            break;
        case TL_CROP:
            Line(cx - 5, cy - 9, cx - 5, cy + 5, 1.7f, c);
            Line(cx - 9, cy + 5, cx + 5, cy + 5, 1.7f, c);
            Line(cx - 9, cy - 5, cx + 5, cy - 5, 1.7f, c);
            Line(cx + 5, cy - 5, cx + 5, cy + 9, 1.7f, c);
            break;
    }
}
void AddBtn(D2D1_RECT_F r, int id) { g_ed->btns.push_back({r, id}); }

void Paint() {
    Editor* e = g_ed;
    if (!e->rt) {
        RECT rc;
        GetClientRect(e->hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties();
        rp.dpiX = rp.dpiY = e->dpi;
        if (FAILED(e->f->CreateHwndRenderTarget(rp, D2D1::HwndRenderTargetProperties(e->hwnd, D2D1::SizeU(rc.right, rc.bottom)), &e->rt))) return;
        e->rt->CreateSolidColorBrush(C(1, 1, 1), &e->br);
        e->base = BmpFrom(e->rt, e->px, e->w, e->h);
        for (auto& s : e->shapes) Rel(s->blurBmp);
    }
    Layout();
    e->btns.clear();
    ID2D1HwndRenderTarget* t = e->rt;
    t->BeginDraw();
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    t->Clear(Bg());
    D2D1_SIZE_F sz = t->GetSize();
    // картинка + фигуры
    float s = Px2Dip() * e->zoom;
    D2D1_RECT_F imgR = R(e->offX, e->offY, e->w * s, e->h * s);
    RRect(R(imgR.left - 1, imgR.top - 1, imgR.right - imgR.left + 2, imgR.bottom - imgR.top + 2), 2, Ov(0.12f));
    if (e->base) t->DrawBitmap(e->base, imgR, 1.f, e->zoom * Px2Dip() >= 0.999f && e->dpi == 96 ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    t->PushAxisAlignedClip(imgR, D2D1_ANTIALIAS_MODE_ALIASED);
    t->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(e->offX, e->offY));
    for (auto& sh : e->shapes) if (sh->type != TL_CROP) DrawShape(t, e->br, *sh, false);
    if (e->cur && e->cur->type != TL_CROP) DrawShape(t, e->br, *e->cur, false);
    t->SetTransform(D2D1::Matrix3x2F::Identity());
    t->PopAxisAlignedClip();
    // обрезка: затемняем снаружи
    D2D1_RECT_F cr;
    if (CropRect(cr)) {
        D2D1_POINT_2F a = ImgToDip({cr.left, cr.top}), b = ImgToDip({cr.right, cr.bottom});
        D2D1_COLOR_F dim = C(0, 0, 0, 0.55f);
        e->br->SetColor(dim);
        t->FillRectangle(D2D1::RectF(imgR.left, imgR.top, imgR.right, a.y), e->br);
        t->FillRectangle(D2D1::RectF(imgR.left, b.y, imgR.right, imgR.bottom), e->br);
        t->FillRectangle(D2D1::RectF(imgR.left, a.y, a.x, b.y), e->br);
        t->FillRectangle(D2D1::RectF(b.x, a.y, imgR.right, b.y), e->br);
        e->br->SetColor(C(1, 1, 1, 0.9f));
        t->DrawRectangle(D2D1::RectF(a.x, a.y, b.x, b.y), e->br, 1.2f);
    }
    // верхняя панель — плавающая «пилюля»
    float tbW = TL_COUNT * 40 + 14 + 6 * 30 + 14 + 3 * 30 + 14 + 2 * 38 + 24;
    float tx = (sz.width - tbW) / 2, ty = 8;
    RRect(R(tx, ty, tbW, 44), 22, e->light ? C(1, 1, 1) : C(0.13f, 0.13f, 0.145f));
    SRect(R(tx, ty, tbW, 44), 22, 1, Ov(0.08f));
    float x = tx + 12;
    for (int i = 0; i < TL_COUNT; i++) {
        D2D1_RECT_F r = R(x, ty + 4, 36, 36);
        AddBtn(r, B_TOOL + i);
        if (e->tool == i) RRect(r, 12, e->accent);
        else if (e->hot == B_TOOL + i) RRect(r, 12, Ov(0.09f));
        ToolIcon(i, x + 18, ty + 22, e->tool == i ? C(1, 1, 1) : Fg());
        x += 40;
    }
    x += 6;
    Line(x, ty + 12, x, ty + 32, 1, Ov(0.15f));
    x += 10;
    for (int i = 0; i < 6; i++) {
        float cx = x + 13, cy = ty + 22;
        AddBtn(R(x, ty + 7, 28, 30), B_COLOR + i);
        if (e->color == i) {
            e->br->SetColor(Fg());
            t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 11.5f, 11.5f), e->br, 1.8f);
        }
        Circ(cx, cy, e->hot == B_COLOR + i ? 9.f : 8.f, kPal[i]);
        if (i >= 4) { e->br->SetColor(Ov(0.3f)); t->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 8, 8), e->br, 1); }
        x += 30;
    }
    x += 4;
    Line(x, ty + 12, x, ty + 32, 1, Ov(0.15f));
    x += 10;
    for (int i = 0; i < 3; i++) {
        D2D1_RECT_F r = R(x, ty + 7, 28, 30);
        AddBtn(r, B_SIZE + i);
        if (e->size == i) RRect(r, 10, Ov(0.12f));
        else if (e->hot == B_SIZE + i) RRect(r, 10, Ov(0.06f));
        Circ(x + 14, ty + 22, 2.f + i * 2.2f, Fg());
        x += 30;
    }
    x += 4;
    Line(x, ty + 12, x, ty + 32, 1, Ov(0.15f));
    x += 10;
    for (int i = 0; i < 2; i++) {
        D2D1_RECT_F r = R(x, ty + 4, 36, 36);
        bool en = i == 0 ? !e->shapes.empty() : !e->redo.empty();
        AddBtn(r, i == 0 ? B_UNDO : B_REDO);
        if (en && e->hot == (i == 0 ? B_UNDO : B_REDO)) RRect(r, 12, Ov(0.09f));
        D2D1_COLOR_F c = en ? Fg() : Ov(0.25f);
        float cx = x + 18, cy = ty + 22, d = i == 0 ? 1.f : -1.f;
        e->br->SetColor(c);
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(e->f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
            sk->BeginFigure(D2D1::Point2F(cx - 6 * d, cy - 3), D2D1_FIGURE_BEGIN_HOLLOW);
            D2D1_ARC_SEGMENT arc = {D2D1::Point2F(cx + 2 * d, cy + 7), D2D1::SizeF(6, 5), 0,
                                    i == 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_LARGE};
            sk->AddArc(&arc);
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
            sk->Close();
            sk->Release();
            t->DrawGeometry(g, e->br, 1.7f);
        }
        Rel(g);
        Line(cx - 6 * d, cy - 3, cx - 6 * d, cy - 8, 1.7f, c);
        Line(cx - 6 * d, cy - 3, cx - 1 * d, cy - 3, 1.7f, c);
        x += 38;
    }
    // подсказка инструмента
    if (e->hot >= B_TOOL && e->hot < B_TOOL + TL_COUNT) Text(kToolTips[e->hot - B_TOOL], R(tx, ty + 46, 240, 18), Gray(), DWRITE_TEXT_ALIGNMENT_LEADING);
    // нижняя панель
    float by = sz.height - kBottom + 8;
    D2D1_RECT_F cr2;
    int ow = e->w, oh = e->h;
    if (CropRect(cr2)) { ow = (int)(cr2.right - cr2.left); oh = (int)(cr2.bottom - cr2.top); }
    wchar_t info[96];
    swprintf(info, 96, L"%d × %d", ow, oh);
    Text(info, R(20, by, 200, 40), Gray(), DWRITE_TEXT_ALIGNMENT_LEADING);
    struct { int id; const wchar_t* t; } acts[] = {{B_CANCEL, L"Отмена"}, {B_DRAG, L"Перетащить"}, {B_COPY, L"Копировать"}, {B_SAVE, L"Сохранить как…"}, {B_DONE, L"Готово"}};
    float rx = sz.width - 20;
    for (int i = 4; i >= 0; i--) {
        float bw = acts[i].id == B_DONE ? 110.f : acts[i].id == B_SAVE ? 140.f : acts[i].id == B_DRAG ? 136.f : 112.f;
        D2D1_RECT_F r = R(rx - bw, by, bw, 40);
        AddBtn(r, acts[i].id);
        bool hot = e->hot == acts[i].id;
        if (acts[i].id == B_DONE) RRect(r, 20, hot ? C(std::min(1.f, e->accent.r + 0.08f), std::min(1.f, e->accent.g + 0.08f), std::min(1.f, e->accent.b + 0.08f)) : e->accent);
        else RRect(r, 20, Ov(hot ? 0.14f : 0.08f));
        Text(acts[i].t, r, acts[i].id == B_DONE ? C(1, 1, 1) : Fg());
        rx -= bw + 8;
    }
    if (t->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        for (auto& sh : e->shapes) Rel(sh->blurBmp);
        Rel(e->base);
        Rel(e->br);
        Rel(e->rt);
    }
}
void Redraw() { if (g_ed && g_ed->hwnd) InvalidateRect(g_ed->hwnd, nullptr, FALSE); }

int BtnAt(int x, int y) {
    float fx = x * Px2Dip(), fy = y * Px2Dip();
    for (auto& b : g_ed->btns)
        if (fx >= b.r.left && fx <= b.r.right && fy >= b.r.top && fy <= b.r.bottom) return b.id;
    return -1;
}

void Finish(int action, const std::wstring& path = L"") {
    Editor* e = g_ed;
    if (e->sent) return;
    MarkupResult* r = new MarkupResult;
    r->action = action;
    r->path = path;
    if (action != MK_CANCEL && !Export(r->px, r->w, r->h)) r->action = MK_CANCEL;
    e->sent = true;
    if (!PostMessageW(e->notify, WM_APP_MARKUP, 0, (LPARAM)r)) delete r;
    DestroyWindow(e->hwnd);
}

void CommitText() {
    Editor* e = g_ed;
    if (!e->edit) return;
    int n = GetWindowTextLengthW(e->edit);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(e->edit, &s[0], n + 1);
    s.resize(n);
    HWND ed = e->edit;
    e->edit = nullptr;
    DestroyWindow(ed);
    if (!s.empty()) {
        auto sh = std::make_unique<Shape>();
        sh->type = TL_TEXT;
        sh->pts.push_back(e->textAt);
        sh->color = kPal[e->color];
        sh->width = StrokeW();
        sh->text = s;
        e->shapes.push_back(std::move(sh));
        e->redo.clear();
    }
    SetFocus(e->hwnd);
    Redraw();
}
WNDPROC g_editProc = nullptr;
LRESULT CALLBACK EditSub(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_KEYDOWN && wp == VK_RETURN && !(GetKeyState(VK_SHIFT) & 0x8000)) { CommitText(); return 0; }
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { SetWindowTextW(w, L""); CommitText(); return 0; }
    if (m == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;
    if (m == WM_KILLFOCUS) { LRESULT r = CallWindowProcW(g_editProc, w, m, wp, lp); PostMessageW(g_ed->hwnd, WM_APP + 1, 0, 0); return r; }
    return CallWindowProcW(g_editProc, w, m, wp, lp);
}
void StartText(D2D1_POINT_2F at) {
    Editor* e = g_ed;
    if (e->edit) CommitText();
    e->textAt = at;
    D2D1_POINT_2F d = ImgToDip(at);
    float fsImg = StrokeW() * 4.5f + 12;
    int fsPx = (int)std::lround(fsImg * Px2Dip() * e->zoom * e->dpi / 96.f);
    if (e->editFont) DeleteObject(e->editFont);
    e->editFont = CreateFontW(-std::max(10, fsPx), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int x = (int)(d.x * e->dpi / 96.f), y = (int)(d.y * e->dpi / 96.f);
    e->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, std::max(200, fsPx * 12), fsPx + 10, e->hwnd,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(e->edit, WM_SETFONT, (WPARAM)e->editFont, TRUE);
    g_editProc = (WNDPROC)SetWindowLongPtrW(e->edit, GWLP_WNDPROC, (LONG_PTR)EditSub);
    SetFocus(e->edit);
}

void Undo() {
    Editor* e = g_ed;
    if (e->shapes.empty()) return;
    e->redo.push_back(std::move(e->shapes.back()));
    e->shapes.pop_back();
    Redraw();
}
void RedoOp() {
    Editor* e = g_ed;
    if (e->redo.empty()) return;
    e->shapes.push_back(std::move(e->redo.back()));
    e->redo.pop_back();
    Redraw();
}
int NextNumber() {
    int n = 0;
    for (auto& s : g_ed->shapes) if (s->type == TL_NUM) n = std::max(n, s->number);
    return n + 1;
}
void DoCopy() { Finish(MK_COPY); }
void DoSaveAs() {
    wchar_t buf[MAX_PATH] = L"Скриншот.png";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_ed->hwnd;
    of.lpstrFilter = L"PNG\0*.png\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrDefExt = L"png";
    of.lpstrTitle = L"Сохранить скриншот";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&of)) return;
    std::vector<BYTE> out;
    int ow, oh;
    if (Export(out, ow, oh) && SavePng(buf, out, ow, oh)) Finish(MK_SAVED, buf);
}
void DoDrag() {
    Editor* e = g_ed;
    std::vector<BYTE> out;
    int ow, oh;
    if (!Export(out, ow, oh)) return;
    CreateDirectoryW(e->shotDir.c_str(), nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t nm[96];
    swprintf(nm, 96, L"\\Скриншот %04d-%02d-%02d %02d-%02d-%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring path = e->shotDir + nm;
    if (!SavePng(path, out, ow, oh)) return;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) return;
    IShellItem* si = nullptr;
    IDataObject* dobj = nullptr;
    static const GUID kBHID_DataObject = {0xB8C0BD9F, 0xED24, 0x455C, {0x83, 0xE6, 0xD5, 0x39, 0x0C, 0x4F, 0xE8, 0xC4}};
    if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&si))) && si &&
        SUCCEEDED(si->BindToHandler(nullptr, kBHID_DataObject, IID_IDataObject, (void**)&dobj)) && dobj) {
        DWORD eff = 0;
        HRESULT hr = SHDoDragDrop(e->hwnd, dobj, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &eff);
        dobj->Release();
        if (hr == DRAGDROP_S_DROP) { si->Release(); CoTaskMemFree(pidl); Finish(MK_DRAGGED, path); return; }
    }
    if (si) si->Release();
    CoTaskMemFree(pidl);
}

void Press(int id) {
    Editor* e = g_ed;
    if (id >= B_TOOL && id < B_TOOL + TL_COUNT) { if (e->edit) CommitText(); e->tool = id - B_TOOL; }
    else if (id >= B_COLOR && id < B_COLOR + 6) e->color = id - B_COLOR;
    else if (id >= B_SIZE && id < B_SIZE + 3) e->size = id - B_SIZE;
    else if (id == B_UNDO) Undo();
    else if (id == B_REDO) RedoOp();
    else if (id == B_COPY) { DoCopy(); return; }
    else if (id == B_SAVE) { DoSaveAs(); return; }
    else if (id == B_DONE) { Finish(MK_DONE); return; }
    else if (id == B_CANCEL) { Finish(MK_CANCEL); return; }
    Redraw();
}

LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    Editor* e = g_ed;
    if (!e) return DefWindowProcW(w, m, wp, lp);
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(w, &ps);
            Paint();
            EndPaint(w, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (e->rt) e->rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (e->edit) CommitText();
            Redraw();
            return 0;
        case WM_DPICHANGED: {
            e->dpi = (float)HIWORD(wp);
            if (e->rt) e->rt->SetDpi(e->dpi, e->dpi);
            RECT* r = (RECT*)lp;
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize = {(LONG)(940 * e->dpi / 96), (LONG)(420 * e->dpi / 96)};
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(w, &pt);
                int b = BtnAt(pt.x, pt.y);
                LPCWSTR cur = b == B_DRAG ? IDC_SIZEALL : b >= 0 ? IDC_HAND : e->tool == TL_TEXT ? IDC_IBEAM : IDC_CROSS;
                D2D1_POINT_2F ip = ToImg(pt.x, pt.y);
                if (b < 0 && (ip.x < 0 || ip.y < 0 || ip.x > e->w || ip.y > e->h)) cur = IDC_ARROW;
                SetCursor(LoadCursorW(nullptr, cur));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (e->pressBtn == B_DRAG && (wp & MK_LBUTTON) && (abs(x - e->dragStart.x) > 4 || abs(y - e->dragStart.y) > 4)) {
                e->pressBtn = -1;
                ReleaseCapture();
                DoDrag();
                return 0;
            }
            if (e->dragging && e->cur) {
                D2D1_POINT_2F p = ToImg(x, y);
                p.x = std::max(0.f, std::min((float)e->w, p.x));
                p.y = std::max(0.f, std::min((float)e->h, p.y));
                if (e->cur->type == TL_MARKER || e->cur->type == TL_PEN) {
                    D2D1_POINT_2F l = e->cur->pts.back();
                    if (std::fabs(l.x - p.x) + std::fabs(l.y - p.y) >= 1.5f) e->cur->pts.push_back(p);
                } else if (e->cur->type == TL_NUM) e->cur->pts[0] = p;
                else {
                    if (e->cur->pts.size() < 2) e->cur->pts.push_back(p);
                    else e->cur->pts[1] = p;
                    if (GetKeyState(VK_SHIFT) & 0x8000) {  // Shift: ровно
                        D2D1_POINT_2F a = e->cur->pts[0];
                        float dx = p.x - a.x, dy = p.y - a.y;
                        if (e->cur->type == TL_ARROW) {
                            float ang = std::atan2(dy, dx), st = 3.14159265f / 4, len = std::sqrt(dx * dx + dy * dy);
                            ang = std::round(ang / st) * st;
                            e->cur->pts[1] = {a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
                        } else {
                            float m2 = std::max(std::fabs(dx), std::fabs(dy));
                            e->cur->pts[1] = {a.x + (dx < 0 ? -m2 : m2), a.y + (dy < 0 ? -m2 : m2)};
                        }
                    }
                }
                Redraw();
                return 0;
            }
            int b = BtnAt(x, y);
            if (b != e->hot) { e->hot = b; Redraw(); }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, w, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (e->hot != -1) { e->hot = -1; Redraw(); }
            return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (e->edit) { CommitText(); }
            int b = BtnAt(x, y);
            if (b >= 0) {
                e->pressBtn = b;
                e->dragStart = {x, y};
                SetCapture(w);
                return 0;
            }
            D2D1_POINT_2F p = ToImg(x, y);
            if (p.x < 0 || p.y < 0 || p.x > e->w || p.y > e->h) return 0;
            if (e->tool == TL_TEXT) { StartText(p); return 0; }
            e->cur = std::make_unique<Shape>();
            e->cur->type = e->tool;
            e->cur->color = kPal[e->color];
            e->cur->width = StrokeW();
            e->cur->pts.push_back(p);
            if (e->tool == TL_NUM) e->cur->number = NextNumber();
            e->dragging = true;
            SetCapture(w);
            Redraw();
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (e->pressBtn >= 0) {
                int b = e->pressBtn;
                e->pressBtn = -1;
                ReleaseCapture();
                if (BtnAt(x, y) == b && b != B_DRAG) Press(b);
                return 0;
            }
            if (e->dragging) {
                std::unique_ptr<Shape> s = std::move(e->cur);  // забираем до ReleaseCapture (он шлёт WM_CAPTURECHANGED)
                e->dragging = false;
                ReleaseCapture();
                if (!s) return 0;
                bool keep = true;
                if (s->type == TL_ARROW || s->type == TL_RECT || s->type == TL_BLUR || s->type == TL_CROP) {
                    if (s->pts.size() < 2) keep = false;
                    else {
                        float dx = s->pts[1].x - s->pts[0].x, dy = s->pts[1].y - s->pts[0].y;
                        if (std::fabs(dx) + std::fabs(dy) < 4) keep = false;
                    }
                }
                if (keep && s->type == TL_BLUR) { MakeBlur(*s); if (!s->bw) keep = false; }
                if (keep) { e->shapes.push_back(std::move(s)); e->redo.clear(); }
                // после обрезки удобнее сразу рисовать
                if (keep && e->tool == TL_CROP) e->tool = TL_ARROW;
                Redraw();
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if ((HWND)lp != w) { e->dragging = false; e->cur.reset(); e->pressBtn = -1; Redraw(); }
            return 0;
        case WM_APP + 1:  // текстовое поле потеряло фокус
            if (e->edit && GetFocus() != e->edit) CommitText();
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            COLORREF c = RGB((int)(kPal[e->color].r * 255), (int)(kPal[e->color].g * 255), (int)(kPal[e->color].b * 255));
            SetTextColor(dc, c);
            SetBkColor(dc, e->light ? RGB(255, 255, 255) : RGB(30, 30, 34));
            if (!e->editBrush) e->editBrush = CreateSolidBrush(e->light ? RGB(255, 255, 255) : RGB(30, 30, 34));
            return (LRESULT)e->editBrush;
        }
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl && wp == 'Z') { if (shift) RedoOp(); else Undo(); return 0; }
            if (ctrl && wp == 'Y') { RedoOp(); return 0; }
            if (ctrl && wp == 'C') { DoCopy(); return 0; }
            if (ctrl && wp == 'S') { DoSaveAs(); return 0; }
            if (wp == VK_RETURN) { Finish(MK_DONE); return 0; }
            if (wp == VK_ESCAPE) { Finish(MK_CANCEL); return 0; }
            if (!ctrl)
                for (int i = 0; i < TL_COUNT; i++)
                    if ((WPARAM)kToolKeys[i] == wp) { e->tool = i; Redraw(); return 0; }
            if (!ctrl && wp >= '1' && wp <= '6') { e->color = (int)(wp - '1'); Redraw(); return 0; }
            break;
        }
        case WM_CLOSE: Finish(MK_CANCEL); return 0;
        case WM_DESTROY: {
            if (!e->sent) {
                MarkupResult* r = new MarkupResult;
                if (!PostMessageW(e->notify, WM_APP_MARKUP, 0, (LPARAM)r)) delete r;
            }
            for (auto& s : e->shapes) Rel(s->blurBmp);
            for (auto& s : e->redo) Rel(s->blurBmp);
            Rel(e->base);
            Rel(e->br);
            Rel(e->rt);
            Rel(e->fUi);
            Rel(e->dw);
            Rel(e->f);
            if (e->editFont) DeleteObject(e->editFont);
            if (e->editBrush) DeleteObject(e->editBrush);
            delete e;
            g_ed = nullptr;
            return 0;
        }
    }
    return DefWindowProcW(w, m, wp, lp);
}

}  // namespace

bool MarkupEditorOpen() { return g_ed != nullptr; }

bool OpenMarkupEditor(HWND notify, std::vector<BYTE>&& bgra, int w, int h, const std::wstring& shotDir, bool light, COLORREF accent) {
    if (g_ed) { SetForegroundWindow(g_ed->hwnd); return false; }
    if (w <= 0 || h <= 0 || bgra.size() < (size_t)w * h * 4) return false;
    std::unique_ptr<Editor> e(new Editor);
    e->notify = notify;
    e->px = std::move(bgra);
    e->w = w;
    e->h = h;
    e->shotDir = shotDir;
    e->light = light;
    e->accent = C(GetRValue(accent) / 255.f, GetGValue(accent) / 255.f, GetBValue(accent) / 255.f);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&e->f)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&e->dw)))
        return false;
    e->dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f,
                            L"ru-ru", &e->fUi);
    if (e->fUi) e->fUi->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"DerkIslandMarkup";
        RegisterClassExW(&wc);
        reg = true;
    }
    // монитор с курсором, размер под картинку
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    e->dpi = (float)dx;
    float k = dx / 96.f;
    int waW = mi.rcWork.right - mi.rcWork.left, waH = mi.rcWork.bottom - mi.rcWork.top;
    int ww = std::max((int)(940 * k), std::min((int)(waW * 0.92f), w + (int)(80 * k)));
    int wh = std::max((int)(480 * k), std::min((int)(waH * 0.92f), h + (int)((kTop + kBottom + 70) * k)));
    RECT wr = {0, 0, ww, wh};
    AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int fw = wr.right - wr.left, fh = wr.bottom - wr.top;
    int x = mi.rcWork.left + (waW - fw) / 2, y = mi.rcWork.top + (waH - fh) / 2;
    g_ed = e.release();
    HWND hw = CreateWindowExW(0, L"DerkIslandMarkup", L"Разметка скриншота", WS_OVERLAPPEDWINDOW, x, y, fw, fh, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (!hw) { delete g_ed; g_ed = nullptr; return false; }
    g_ed->hwnd = hw;
    // тёмная рамка окна (Windows 11) — через dwmapi динамически
    if (!light)
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
            typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
            if (auto fn = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
                BOOL on = TRUE;
                fn(hw, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &on, sizeof(on));
            }
        }
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
    UpdateWindow(hw);
    return true;
}
