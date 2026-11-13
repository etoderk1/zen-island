// style.inc — готовые стили острова (пресеты), конструктор своего стиля и отрисовка: фон, рамка,
// «жидкое стекло», Mica, Material You, неон, аврора, пиксели, терминал. Подключается перед Render().
// ----------------------------------------------------------------------------

enum Style { ST_NOTCH, ST_MAC, ST_IPHONE, ST_GLASS, ST_NEON, ST_AURORA, ST_MINIMAL, ST_WIN11, ST_MATERIAL, ST_PIXEL, ST_TERM,
             ST_SEASON, ST_CUSTOM, ST_COUNT };
static const wchar_t* kStyleNames[ST_COUNT] = {L"Чёлка", L"MacBook", L"iPhone", L"iOS 26 · стекло", L"Неон", L"Аврора", L"Минимал",
                                               L"Windows 11", L"Material You", L"Пиксель", L"Терминал", L"Сезонный", L"Свой стиль"};
static const wchar_t* kStyleSubs[ST_COUNT] = {L"Как сейчас", L"Строгая чёрная", L"Капсула", L"Liquid Glass", L"Светящийся край",
                                              L"Живой градиент", L"Тонкая полоска", L"Mica и Fluent", L"Цвета из обоев", L"8 бит",
                                              L"Зелёная консоль", L"Снег, листья, гирлянда", L"Конструктор"};
enum ShapeKind { SH_NOTCH, SH_CAPSULE, SH_PIXEL };
enum BgKind { BG_SOLID, BG_GLASS, BG_AURORA, BG_MATERIAL, BG_MICA, BG_BLACK };
enum BorderKind { BD_NONE, BD_HAIR, BD_NEON, BD_SPEC, BD_PIXEL, BD_TERM };
struct StyleDef {
    int shape = SH_NOTCH, bg = BG_SOLID, border = BD_NONE, top = 0;
    float maxR = 0, expR = 34, ear = 12, borderW = 1;
    bool shadow = false, darkOnly = false, minimal = false, season = false;
    int font = -1;     // -1 — шрифт из настроек, иначе номер из kFonts
    int palette = 0;   // 0 — тема, 1 — терминал (зелёный)
};
static const wchar_t* kFonts[4] = {L"Segoe UI", L"Segoe UI Variable Display", L"Bahnschrift", L"Consolas"};

static int Sty() { return Clamp(g_set.style, 0, ST_COUNT - 1); }
static StyleDef Def(int st) {
    StyleDef d;
    switch (st) {
        case ST_NOTCH: break;
        case ST_MAC: d.bg = BG_BLACK; d.maxR = 11; d.expR = 22; d.ear = 8; d.darkOnly = true; break;
        case ST_IPHONE: d.shape = SH_CAPSULE; d.border = BD_HAIR; d.top = 10; d.shadow = true; break;
        case ST_GLASS: d.shape = SH_CAPSULE; d.bg = BG_GLASS; d.border = BD_SPEC; d.top = 10; d.shadow = true; break;
        case ST_NEON: d.shape = SH_CAPSULE; d.border = BD_NEON; d.top = 10; break;
        case ST_AURORA: d.bg = BG_AURORA; d.border = BD_HAIR; break;
        case ST_MINIMAL: d.minimal = true; d.ear = 8; break;
        case ST_WIN11: d.shape = SH_CAPSULE; d.bg = BG_MICA; d.border = BD_HAIR; d.top = 8; d.maxR = 8; d.expR = 12; d.shadow = true; d.font = 1; break;
        case ST_MATERIAL: d.shape = SH_CAPSULE; d.bg = BG_MATERIAL; d.top = 8; d.expR = 28; d.shadow = true; break;
        case ST_PIXEL: d.shape = SH_PIXEL; d.border = BD_PIXEL; d.top = 8; d.maxR = 0; d.expR = 0; d.borderW = 2; d.font = 3; break;
        case ST_TERM: d.shape = SH_CAPSULE; d.bg = BG_BLACK; d.border = BD_TERM; d.top = 6; d.maxR = 6; d.expR = 8; d.font = 3; d.palette = 1; d.darkOnly = true; break;
        case ST_SEASON: d.season = true; break;
        case ST_CUSTOM:
            d.shape = Clamp(g_set.cShape, 0, 2);
            d.bg = Clamp(g_set.cBg, 0, 5);
            d.border = Clamp(g_set.cBorder, 0, 5);
            d.top = g_set.topOffset;
            d.maxR = g_set.cRadius >= 100 ? 0.f : std::max(0.f, g_set.cRadius * 0.3f);
            d.expR = std::max(0.f, g_set.cRadius * 0.34f);
            d.borderW = (float)Clamp(g_set.cBorderW, 1, 4);
            d.shadow = g_set.cShadow;
            d.season = g_set.cSeason;
            d.palette = g_set.cBorder == BD_TERM ? 1 : 0;
            break;
    }
    return d;
}
static StyleDef g_sd;  // текущий стиль (обновляется в ApplyTheme)
static bool ForceDark() { return Def(Sty()).darkOnly; }
static std::wstring StyleFont() {
    int f = g_sd.font >= 0 ? g_sd.font : Clamp(g_set.font, 0, 3);
    return kFonts[f];
}

// ---- цвет ----
static D2D1_COLOR_F HueShift(D2D1_COLOR_F c, float dh) {
    float mx = std::max(c.r, std::max(c.g, c.b)), mn = std::min(c.r, std::min(c.g, c.b)), d = mx - mn;
    float h = 0, s = mx > 0 ? d / mx : 0, v = mx;
    if (d > 1e-5f) {
        if (mx == c.r) h = std::fmod((c.g - c.b) / d, 6.f);
        else if (mx == c.g) h = (c.b - c.r) / d + 2;
        else h = (c.r - c.g) / d + 4;
        h /= 6;
    }
    h = std::fmod(h + dh + 10.f, 1.f);
    s = std::max(s, 0.55f);
    v = std::max(v, 0.85f);
    float i = std::floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch ((int)i % 6) {
        case 0: return Col(v, t, p, c.a);
        case 1: return Col(q, v, p, c.a);
        case 2: return Col(p, v, t, c.a);
        case 3: return Col(p, q, v, c.a);
        case 4: return Col(t, p, v, c.a);
        default: return Col(v, p, q, c.a);
    }
}

// ---- применение пресета ----
static void ApplySettings();
static void ApplyTheme();
static void StyleMorphKick();
static void ApplyStyle(int st) {
    g_set.style = st;
    StyleDef d = Def(st);
    if (st != ST_CUSTOM) g_set.topOffset = d.top;
    g_set.opacity = st == ST_NOTCH ? 97 : st == ST_NEON ? 96 : 100;
    ApplySettings();
    ApplyTheme();
    StyleMorphKick();
}

// ---- «жидкое стекло»: размытая подложка того, что под островом ----
// Окно исключено из захвата экрана (WDA_EXCLUDEFROMCAPTURE), поэтому BitBlt видит то, что ПОД островом.
static bool g_glassAllowed = false;       // Windows 10 2004+ смог исключить окно из захвата
static std::vector<BYTE> g_glassPx;       // уменьшенная и размытая подложка
static int g_glassW = 0, g_glassH = 0;
static RECT g_glassRc = {};               // какой кусок экрана в ней (физические пиксели)
static bool g_glassNew = false;
static double g_glassT = -1;
static unsigned g_glassHash = 0;
static float g_lightX = 0.3f;             // «блик» следует за мышью

static void UpdateAffinity() {
    bool rec = g_rec.Active();
    bool glass = g_sd.bg == BG_GLASS || g_sd.bg == BG_MICA;
    bool want = rec || glass;
    BOOL ok = SetWindowDisplayAffinity(g_hwnd, want ? 0x11 /*WDA_EXCLUDEFROMCAPTURE*/ : 0);
    g_glassAllowed = glass && ok;
    if (glass && g_visible) SetTimer(g_hwnd, T_GLASS, 450, nullptr);
    else KillTimer(g_hwnd, T_GLASS);
}
static void BoxBlur(std::vector<BYTE>& px, int w, int h, int r) {
    std::vector<BYTE> tmp(px.size());
    for (int pass = 0; pass < 2; pass++) {
        // по горизонтали
        for (int y = 0; y < h; y++)
            for (int c = 0; c < 3; c++) {
                int sum = 0;
                for (int k = -r; k <= r; k++) sum += px[((size_t)y * w + Clamp(k, 0, w - 1)) * 4 + c];
                for (int x = 0; x < w; x++) {
                    tmp[((size_t)y * w + x) * 4 + c] = (BYTE)(sum / (2 * r + 1));
                    sum += px[((size_t)y * w + Clamp(x + r + 1, 0, w - 1)) * 4 + c] - px[((size_t)y * w + Clamp(x - r, 0, w - 1)) * 4 + c];
                }
            }
        // по вертикали
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++) {
                int sum = 0;
                for (int k = -r; k <= r; k++) sum += tmp[((size_t)Clamp(k, 0, h - 1) * w + x) * 4 + c];
                for (int y = 0; y < h; y++) {
                    px[((size_t)y * w + x) * 4 + c] = (BYTE)(sum / (2 * r + 1));
                    sum += tmp[((size_t)Clamp(y + r + 1, 0, h - 1) * w + x) * 4 + c] - tmp[((size_t)Clamp(y - r, 0, h - 1) * w + x) * 4 + c];
                }
            }
    }
}
static void CaptureBackdrop() {
    if (!g_glassAllowed || !g_visible || g_toolHidden) return;
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    // с запасом, чтобы раскрытие не упиралось в край подложки
    int padX = (int)(90 * g_scale), padY = (int)(40 * g_scale);
    RECT rc = {std::max(g_mon.left, wr.left - padX), g_mon.top, std::min(g_mon.right, wr.right + padX),
               std::min(g_mon.bottom, wr.bottom + padY)};
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W < 8 || H < 8) return;
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        ok = BitBlt(mem, 0, 0, W, H, scr, rc.left, rc.top, SRCCOPY) != 0;
        SelectObject(mem, old);
    }
    if (ok) {
        const int F = 4;  // уменьшаем в 4 раза — и быстрее, и мягче
        int w = std::max(1, W / F), h = std::max(1, H / F);
        std::vector<BYTE> small((size_t)w * h * 4);
        const BYTE* src = (const BYTE*)bits;
        unsigned hash = 2166136261u;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                unsigned s[3] = {0, 0, 0};
                for (int yy = 0; yy < F; yy++)
                    for (int xx = 0; xx < F; xx++) {
                        const BYTE* p = src + ((size_t)(y * F + yy) * W + x * F + xx) * 4;
                        s[0] += p[0]; s[1] += p[1]; s[2] += p[2];
                    }
                BYTE* d = &small[((size_t)y * w + x) * 4];
                // чуть насыщеннее и светлее — как у стекла iOS
                float b = s[0] / 16.f, g = s[1] / 16.f, r = s[2] / 16.f, l = 0.299f * r + 0.587f * g + 0.114f * b;
                const float sat = 1.35f;
                d[0] = (BYTE)Clamp(l + (b - l) * sat + 6, 0.f, 255.f);
                d[1] = (BYTE)Clamp(l + (g - l) * sat + 6, 0.f, 255.f);
                d[2] = (BYTE)Clamp(l + (r - l) * sat + 6, 0.f, 255.f);
                d[3] = 255;
                if (((x ^ y) & 3) == 0) hash = (hash ^ d[0] ^ (d[1] << 8) ^ (d[2] << 16)) * 16777619u;
            }
        BoxBlur(small, w, h, 3);
        bool changed = hash != g_glassHash || rc.left != g_glassRc.left || rc.right != g_glassRc.right || rc.bottom != g_glassRc.bottom;
        if (changed) {
            g_glassPx = std::move(small);
            g_glassW = w;
            g_glassH = h;
            g_glassRc = rc;
            g_glassHash = hash;
            g_glassNew = true;
            g_dirty = true;
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    g_glassT = NowSec();
}

// ---- фон и рамка ----
static void FillGeoGradient(ID2D1Geometry* geo, D2D1_POINT_2F a, D2D1_POINT_2F b, const D2D1_GRADIENT_STOP* st, UINT n, bool stroke, float w) {
    ID2D1GradientStopCollection* col = nullptr;
    ID2D1LinearGradientBrush* gb = nullptr;
    g_rt->CreateGradientStopCollection(st, n, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {a, b};
    if (col) g_rt->CreateLinearGradientBrush(&lg, nullptr, col, &gb);
    if (gb) {
        if (stroke) g_rt->DrawGeometry(geo, gb, w, nullptr);
        else g_rt->FillGeometry(geo, gb, nullptr);
    }
    SafeRelease(gb);
    SafeRelease(col);
}
static void Blob(float cx, float cy, float r, D2D1_COLOR_F c) {
    ID2D1GradientStopCollection* col = nullptr;
    ID2D1RadialGradientBrush* rb = nullptr;
    D2D1_GRADIENT_STOP st[2] = {{0.f, c}, {1.f, WithA(c, 0.f)}};
    g_rt->CreateGradientStopCollection(st, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp = {Pt(cx, cy), Pt(0, 0), r, r};
    if (col) g_rt->CreateRadialGradientBrush(&rp, nullptr, col, &rb);
    if (rb) g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy), r, r), rb);
    SafeRelease(rb);
    SafeRelease(col);
}
// ---- Material You: цвет из обоев ----
static D2D1_COLOR_F g_wallColor = {0.45f, 0.5f, 0.9f, 1};
static void ReadWallpaperColor() {
    wchar_t path[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) || !path[0]) return;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) || !wic) return;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && fr && SUCCEEDED(wic->CreateBitmapScaler(&sc)) && sc &&
        SUCCEEDED(sc->Initialize(fr, 32, 32, WICBitmapInterpolationModeFant)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) && cv &&
        SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        std::vector<BYTE> px(32 * 32 * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, 32 * 4, (UINT)px.size(), px.data()))) {
            // корзины по оттенку, вес = насыщенность × яркость: берём самый «сочный» частый цвет
            float wsum[12] = {}, rs[12] = {}, gs[12] = {}, bs[12] = {};
            for (size_t i = 0; i < px.size(); i += 4) {
                float b = px[i] / 255.f, g = px[i + 1] / 255.f, r = px[i + 2] / 255.f;
                float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
                if (mx < 0.15f || d < 0.08f) continue;
                float h;
                if (mx == r) h = std::fmod((g - b) / d, 6.f);
                else if (mx == g) h = (b - r) / d + 2;
                else h = (r - g) / d + 4;
                int k = ((int)std::floor((h < 0 ? h + 6 : h) * 2)) % 12;
                float w = (d / mx) * mx;
                wsum[k] += w; rs[k] += r * w; gs[k] += g * w; bs[k] += b * w;
            }
            int best = -1;
            for (int k = 0; k < 12; k++) if (wsum[k] > 0.5f && (best < 0 || wsum[k] > wsum[best])) best = k;
            if (best >= 0) g_wallColor = HueShift(Col(rs[best] / wsum[best], gs[best] / wsum[best], bs[best] / wsum[best]), 0);
        }
    }
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    wic->Release();
}

// ---- форма «пиксели»: углы лесенкой ----
static ID2D1PathGeometry* BuildPixelRect(float x, float y, float w, float h, bool attached) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return nullptr; }
    const float P = 4;  // «пиксель»
    auto stairs = [&](float cx, float cy, int sx, int sy, bool rev) {
        // три ступеньки от (cx, cy+sy*3P) к (cx+sx*3P, cy)
        D2D1_POINT_2F pts[6] = {Pt(cx, cy + sy * 3 * P), Pt(cx, cy + sy * 2 * P), Pt(cx + sx * P, cy + sy * 2 * P), Pt(cx + sx * P, cy + sy * P),
                                Pt(cx + sx * 2 * P, cy + sy * P), Pt(cx + sx * 2 * P, cy)};
        if (!rev) for (int i = 0; i < 6; i++) s->AddLine(pts[i]);
        else for (int i = 5; i >= 0; i--) s->AddLine(pts[i]);
        s->AddLine(rev ? Pt(cx, cy + sy * 3 * P) : Pt(cx + sx * 3 * P, cy));
    };
    if (attached) {
        s->BeginFigure(Pt(x, y), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(Pt(x + w, y));
    } else {
        s->BeginFigure(Pt(x + 3 * P, y), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(Pt(x + w - 3 * P, y));
        stairs(x + w, y, -1, 1, true);
    }
    s->AddLine(Pt(x + w, y + h - 3 * P));
    stairs(x + w, y + h, -1, -1, false);
    s->AddLine(Pt(x + 3 * P, y + h));
    stairs(x, y + h, 1, -1, true);
    if (!attached) {
        s->AddLine(Pt(x, y + 3 * P));
        stairs(x, y, 1, 1, false);
    }
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    return g;
}

static void PushGeoLayer(ID2D1Geometry* geo, float opacity) {
    D2D1_LAYER_PARAMETERS lp = {};
    lp.contentBounds = D2D1_RECT_F{-1e6f, -1e6f, 1e6f, 1e6f};
    lp.geometricMask = geo;
    lp.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    lp.maskTransform = D2D1::Matrix3x2F::Identity();
    lp.opacity = opacity;
    g_rt->PushLayer(&lp, g_layer);
}
static void DrawBackdrop() {
    if (!g_glassAllowed || g_glassPx.empty()) return;
    if (g_glassNew || !g_glassBmp) {
        SafeRelease(g_glassBmp);
        D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96, 96};
        g_rt->CreateBitmap(D2D1::SizeU(g_glassW, g_glassH), g_glassPx.data(), g_glassW * 4, &bp, &g_glassBmp);
        g_glassNew = false;
    }
    if (!g_glassBmp) return;
    float x0 = (g_glassRc.left - g_winPos.x) / g_scale, y0 = (g_glassRc.top - g_winPos.y) / g_scale;
    float x1 = (g_glassRc.right - g_winPos.x) / g_scale, y1 = (g_glassRc.bottom - g_winPos.y) / g_scale;
    g_rt->DrawBitmap(g_glassBmp, D2D1::RectF(x0, y0, x1, y1), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}
// мягкая тень под «парящими» стилями
static void PaintShadow(ID2D1PathGeometry* geo) {
    if (!g_sd.shadow) return;
    D2D1_MATRIX_3X2_F t0;
    g_rt->GetTransform(&t0);
    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, 4) * t0);
    float a = g_light ? 0.10f : 0.22f;
    const float wds[4] = {16, 10, 5, 2};
    const float als[4] = {0.15f, 0.25f, 0.4f, 0.6f};
    for (int i = 0; i < 4; i++) {
        SetColor(Col(0, 0, 0, a * als[i]));
        g_rt->DrawGeometry(geo, g_brush, wds[i], nullptr);
    }
    g_rt->SetTransform(t0);
}
static void PaintIslandBg(ID2D1PathGeometry* geo, const Layout& L) {
    float op = g_set.opacity / 100.f;
    PaintShadow(geo);
    switch (g_sd.bg) {
        case BG_BLACK: SetColor(Col(0, 0, 0, 1)); g_rt->FillGeometry(geo, g_brush); return;
        case BG_GLASS:
        case BG_MICA: {
            bool mica = g_sd.bg == BG_MICA;
            PushGeoLayer(geo, 1.f);
            bool drawn = g_glassAllowed && !g_glassPx.empty();
            DrawBackdrop();
            float tint = drawn ? (mica ? 0.80f : 0.42f) : (mica ? 0.92f : 0.85f);
            SetColor(g_light ? Col(0.97f, 0.97f, 0.98f, tint) : Col(0.03f, 0.03f, 0.05f, tint));
            g_rt->FillRectangle(D2D1::RectF(L.px - 40, L.py - 40, L.px + L.pw + 40, L.py + L.ph + 40), g_brush);
            if (!mica) {
                D2D1_GRADIENT_STOP gs[2] = {{0.f, Col(1, 1, 1, g_light ? 0.35f : 0.10f)}, {1.f, Col(1, 1, 1, 0.f)}};
                FillGeoGradient(geo, Pt(0, L.py), Pt(0, L.py + std::min(L.ph, 60.f)), gs, 2, false, 0);
            }
            g_rt->PopLayer();
            return;
        }
        case BG_MATERIAL: {
            D2D1_COLOR_F base = g_light ? Lerp(Col(1, 1, 1), g_wallColor, 0.16f) : Lerp(Col(0.04f, 0.04f, 0.05f), g_wallColor, 0.20f);
            SetColor(WithA(base, op));
            g_rt->FillGeometry(geo, g_brush);
            return;
        }
        default: break;
    }
    SetColor(WithA(kBgSolid, op));
    g_rt->FillGeometry(geo, g_brush);
    if (g_sd.bg == BG_AURORA) {
        PushGeoLayer(geo, g_light ? 0.5f : 0.62f);
        float t = (float)g_phase * 0.06f + (float)g_auroraT;
        float R = std::max(L.pw, L.ph) * 0.55f + 30;
        for (int i = 0; i < 3; i++) {
            float cx = L.px + L.pw * (0.5f + 0.42f * std::sin(t + i * 2.1f));
            float cy = L.py + L.ph * (0.5f + 0.55f * std::cos(t * 0.8f + i * 1.7f));
            Blob(cx, cy, R, WithA(HueShift(g_accent, i * 0.18f - 0.18f), 0.75f));
        }
        g_rt->PopLayer();
        SetColor(WithA(kBgSolid, 0.28f));
        g_rt->FillGeometry(geo, g_brush);
    }
}
static void PaintIslandBorder(ID2D1PathGeometry* geo, const Layout& L, bool notch) {
    float bw = g_sd.borderW;
    switch (g_sd.border) {
        case BD_SPEC: {
            float lx = L.px + L.pw * g_lightX;
            D2D1_GRADIENT_STOP gs[3] = {{0.f, Col(1, 1, 1, g_light ? 0.95f : 0.60f)}, {0.5f, Col(1, 1, 1, 0.10f)}, {1.f, Col(1, 1, 1, g_light ? 0.55f : 0.28f)}};
            FillGeoGradient(geo, Pt(lx, L.py), Pt(L.px + L.pw - (lx - L.px), L.py + L.ph), gs, 3, true, 1.3f * bw);
            return;
        }
        case BD_NEON: {
            float a = (float)g_phase * 0.12f + (float)g_auroraT;
            float cx = L.px + L.pw / 2, cy = L.py + L.ph / 2, rr = std::max(L.pw, L.ph) / 2;
            D2D1_POINT_2F p0 = Pt(cx + std::cos(a) * rr, cy + std::sin(a) * rr), p1 = Pt(cx - std::cos(a) * rr, cy - std::sin(a) * rr);
            D2D1_COLOR_F c0 = HueShift(g_accent, 0), c1 = HueShift(g_accent, 0.33f), c2 = HueShift(g_accent, 0.66f);
            float glow[3] = {9.f, 5.f, 2.f * bw}, al[3] = {0.10f, 0.22f, 1.f};
            for (int i = 0; i < 3; i++) {
                D2D1_GRADIENT_STOP gs[3] = {{0.f, WithA(c0, al[i])}, {0.5f, WithA(c1, al[i])}, {1.f, WithA(c2, al[i])}};
                FillGeoGradient(geo, p0, p1, gs, 3, true, glow[i]);
            }
            return;
        }
        case BD_HAIR:
            SetColor(Ov(g_sd.bg == BG_MICA ? 0.12f : 0.07f));
            g_rt->DrawGeometry(geo, g_brush, bw, nullptr);
            return;
        case BD_PIXEL:
            g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            SetColor(Ov(0.85f));
            g_rt->DrawGeometry(geo, g_brush, bw * 2, nullptr);
            g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            return;
        case BD_TERM:
            SetColor(WithA(kWhite, 0.18f));
            g_rt->DrawGeometry(geo, g_brush, 4.f, nullptr);
            SetColor(WithA(kWhite, 0.75f));
            g_rt->DrawGeometry(geo, g_brush, bw, nullptr);
            return;
        default:
            if (!notch && g_sd.bg != BG_BLACK) {
                SetColor(Ov(0.07f));
                g_rt->DrawGeometry(geo, g_brush, 1.f, nullptr);
            }
    }
}
static bool StyleAnimates() { return g_sd.border == BD_NEON || g_sd.bg == BG_AURORA; }

// ---- превью в настройках ----
static void DrawStylePreview(int st, D2D1_RECT_F r, bool sel, bool hot) {
    StyleDef d = Def(st);
    D2D1_GRADIENT_STOP wp[3] = {{0.f, Col(0.35f, 0.25f, 0.75f)}, {0.55f, Col(0.15f, 0.45f, 0.85f)}, {1.f, Col(0.95f, 0.55f, 0.45f)}};
    ID2D1GradientStopCollection* col = nullptr;
    ID2D1LinearGradientBrush* gb = nullptr;
    g_rt->CreateGradientStopCollection(wp, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg = {Pt(r.left, r.top), Pt(r.right, r.bottom)};
    if (col) g_rt->CreateLinearGradientBrush(&lg, nullptr, col, &gb);
    D2D1_ROUNDED_RECT rr = {r, 14, 14};
    if (gb) g_rt->FillRoundedRectangle(&rr, gb);
    SafeRelease(gb);
    SafeRelease(col);
    g_rt->PushAxisAlignedClip(&r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float cx = (r.left + r.right) / 2, w = st == ST_MAC ? 56.f : 72.f, h = d.minimal ? 5.f : 18.f;
    if (d.minimal) w = 50;
    bool attached = d.shape == SH_NOTCH || d.top == 0;
    float y = attached ? r.top : r.top + 8;
    D2D1_RECT_F ir = RF(cx - w / 2, y, w, h);
    float rad = d.maxR > 0 ? std::min(9.f, d.maxR * 0.6f) : 9.f;
    if (d.minimal) rad = 2.5f;
    ID2D1PathGeometry* g = nullptr;
    if (d.shape == SH_PIXEL) g = BuildPixelRect(ir.left, ir.top, w, h, false);
    else {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g_d2f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            float E = d.shape == SH_NOTCH ? (st == ST_MAC || d.minimal ? 4.f : 6.f) : 0.f;
            if (E > 0) {
                s->BeginFigure(Pt(ir.left - E, ir.top), D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(Pt(ir.right + E, ir.top));
                ArcCCW(s, Pt(ir.right, ir.top + E), E);
                s->AddLine(Pt(ir.right, std::max(ir.top + E, ir.bottom - rad)));
            } else {
                s->BeginFigure(Pt(ir.left + rad, ir.top), D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine(Pt(ir.right - rad, ir.top));
                Arc(s, Pt(ir.right, ir.top + rad), rad);
                s->AddLine(Pt(ir.right, ir.bottom - rad));
            }
            Arc(s, Pt(ir.right - rad, ir.bottom), rad);
            s->AddLine(Pt(ir.left + rad, ir.bottom));
            Arc(s, Pt(ir.left, ir.bottom - rad), rad);
            if (E > 0) {
                s->AddLine(Pt(ir.left, ir.top + E));
                ArcCCW(s, Pt(ir.left - E, ir.top), E);
            } else {
                s->AddLine(Pt(ir.left, ir.top + rad));
                Arc(s, Pt(ir.left + rad, ir.top), rad);
            }
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            s->Release();
        }
    }
    if (g) {
        D2D1_COLOR_F fill = Col(0.02f, 0.02f, 0.03f);
        if (d.bg == BG_GLASS) fill = Col(1, 1, 1, 0.28f);
        if (d.bg == BG_MICA) fill = Col(0.16f, 0.16f, 0.2f, 0.82f);
        if (d.bg == BG_MATERIAL) fill = Lerp(Col(0.04f, 0.04f, 0.05f), g_wallColor, 0.3f);
        if (d.bg == BG_BLACK) fill = Col(0, 0, 0);
        if (d.shadow) { SetColor(Col(0, 0, 0, 0.18f)); g_rt->DrawGeometry(g, g_brush, 5.f); }
        SetColor(fill);
        g_rt->FillGeometry(g, g_brush);
        if (d.bg == BG_AURORA) {
            g_rt->PushAxisAlignedClip(&ir, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            Blob(ir.left + 18, ir.top + 6, 26, WithA(HueShift(g_accent, -0.15f), 0.7f));
            Blob(ir.right - 16, ir.bottom - 4, 26, WithA(HueShift(g_accent, 0.2f), 0.7f));
            g_rt->PopAxisAlignedClip();
        }
        D2D1_COLOR_F ink = d.palette == 1 ? Col(0.35f, 1.f, 0.45f) : Col(1, 1, 1, 0.85f);
        switch (d.border) {
            case BD_SPEC: {
                D2D1_GRADIENT_STOP gs[2] = {{0.f, Col(1, 1, 1, 0.9f)}, {1.f, Col(1, 1, 1, 0.2f)}};
                FillGeoGradient(g, Pt(ir.left, ir.top), Pt(ir.right, ir.bottom), gs, 2, true, 1.f);
                break;
            }
            case BD_NEON: {
                D2D1_GRADIENT_STOP gs[3] = {{0.f, HueShift(g_accent, 0)}, {0.5f, HueShift(g_accent, 0.33f)}, {1.f, HueShift(g_accent, 0.66f)}};
                FillGeoGradient(g, Pt(ir.left, ir.top), Pt(ir.right, ir.bottom), gs, 3, true, 1.6f);
                break;
            }
            case BD_HAIR: SetColor(Col(1, 1, 1, 0.18f)); g_rt->DrawGeometry(g, g_brush, 1.f); break;
            case BD_PIXEL: SetColor(Col(1, 1, 1, 0.9f)); g_rt->DrawGeometry(g, g_brush, 1.5f); break;
            case BD_TERM: SetColor(ink); g_rt->DrawGeometry(g, g_brush, 1.f); break;
        }
        if (!d.minimal) {
            if (d.palette == 1) Text(g_fSmall, L">_", RF(ir.left + 4, ir.top, 20, h), DWRITE_TEXT_ALIGNMENT_LEADING, ink);
            else Circle(ir.left + 10, (ir.top + ir.bottom) / 2, 4.5f, WithA(d.bg == BG_MATERIAL ? g_wallColor : g_accent, 0.95f));
            RRect(RF(ir.left + 20, ir.top + 7, w - 40, 4), d.shape == SH_PIXEL ? 0.f : 2.f, ink);
        }
        if (d.season) {
            for (int i = 0; i < 9; i++) {
                float sx = r.left + 10 + std::fmod(i * 37.f, r.right - r.left - 20), sy = ir.bottom + 6 + std::fmod(i * 23.f, 36.f);
                Circle(sx, sy, 1.4f + (i % 3) * 0.5f, Col(1, 1, 1, 0.9f));
            }
        }
    }
    SafeRelease(g);
    g_rt->PopAxisAlignedClip();
    if (sel) StrokeRRect(r, 14, 2.2f, g_accent);
    else if (hot) StrokeRRect(r, 14, 1.2f, Ov(0.35f));
}
