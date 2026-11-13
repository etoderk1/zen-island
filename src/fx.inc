// fx.inc — «фишки»: частицы (конфетти, искры, салют, снег/листья/лепестки/светлячки, гирлянда),
// звуки интерфейса, перекатывающиеся цифры, прогресс по контуру, тряска, раздвоение острова,
// «потянуть вниз», дыхание обложки, новогодний отсчёт. Подключается перед Render().
// ----------------------------------------------------------------------------

// ================================================================ звуки (синтезируем WAV в памяти)
enum Snd { SN_OPEN, SN_CLOSE, SN_SEND, SN_ERROR, SN_DONE, SN_POP, SN_COUNT };
static std::vector<char> g_snd[SN_COUNT];
static void MakeWav(std::vector<char>& out, const std::vector<std::pair<float, float>>& notes, float amp, float glide = 0) {
    const int SR = 22050;
    std::vector<short> pcm;
    for (auto& n : notes) {
        int len = (int)(n.second * SR);
        double ph = 0;
        for (int i = 0; i < len; i++) {
            float t = (float)i / len;
            float f = n.first * (1 + glide * t);
            ph += 2 * 3.14159265 * f / SR;
            float env = std::min(1.f, i / (SR * 0.006f)) * std::pow(1 - t, 2.2f);
            float v = (float)(std::sin(ph) + 0.25 * std::sin(ph * 2)) * env * amp;
            pcm.push_back((short)(v * 32767));
        }
    }
    int dataBytes = (int)pcm.size() * 2;
    out.resize(44 + dataBytes);
    char* p = out.data();
    auto w32 = [&](int off, int v) { memcpy(p + off, &v, 4); };
    auto w16 = [&](int off, short v) { memcpy(p + off, &v, 2); };
    memcpy(p, "RIFF", 4); w32(4, 36 + dataBytes); memcpy(p + 8, "WAVEfmt ", 8);
    w32(16, 16); w16(20, 1); w16(22, 1); w32(24, SR); w32(28, SR * 2); w16(32, 2); w16(34, 16);
    memcpy(p + 36, "data", 4); w32(40, dataBytes);
    memcpy(p + 44, pcm.data(), dataBytes);
}
static void InitSounds() {
    MakeWav(g_snd[SN_OPEN], {{520, 0.07f}, {780, 0.09f}}, 0.10f, 0.15f);
    MakeWav(g_snd[SN_CLOSE], {{700, 0.06f}, {470, 0.08f}}, 0.08f, -0.1f);
    MakeWav(g_snd[SN_SEND], {{600, 0.12f}}, 0.10f, 1.2f);
    MakeWav(g_snd[SN_ERROR], {{220, 0.08f}, {0, 0.03f}, {196, 0.11f}}, 0.12f);
    MakeWav(g_snd[SN_DONE], {{784, 0.12f}, {988, 0.12f}, {1319, 0.22f}}, 0.11f);
    MakeWav(g_snd[SN_POP], {{900, 0.05f}}, 0.07f, -0.5f);
}
static double g_sndLast = 0;
static void PlayUi(int s) {
    if (!g_set.fxSounds || Quiet() || s < 0 || s >= SN_COUNT) return;
    if (g_snd[0].empty()) InitSounds();
    double now = NowSec();
    if (now - g_sndLast < 0.08) return;  // не тарахтим
    g_sndLast = now;
    PlaySoundW((LPCWSTR)g_snd[s].data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ================================================================ частицы
enum PK { PK_CONFETTI, PK_SPARK, PK_SNOW, PK_LEAF, PK_PETAL, PK_FIREFLY, PK_ROCKET };
struct Particle {
    int kind;
    float x, y, vx, vy, life, maxLife, size, rot, vr;
    D2D1_COLOR_F c;
};
static std::vector<Particle> g_parts;
static double g_fireworksUntil = 0;
static float g_fxPad = 0;          // доп. поле вокруг острова, пока летят частицы
static double g_fxPadUntil = 0;
static float Rnd() { return (float)rand() / RAND_MAX; }
static D2D1_COLOR_F Rainbow() { return HueShift(Col(1, 0.3f, 0.3f), Rnd()); }
static void FxEnsureTimer() { SetTimer(g_hwnd, T_FX, 33, nullptr); }
static void FxPad(float pad, double sec) {
    g_fxPad = std::max(g_fxPad, pad);
    g_fxPadUntil = std::max(g_fxPadUntil, NowSec() + sec);
    FxEnsureTimer();
}
// координаты — относительно центра низа острова (пересчитываются при рисовании)
static void Confetti(int n = 70) {
    if (!g_set.fxParticles) return;
    FxPad(110, 3.2);
    for (int i = 0; i < n; i++) {
        Particle p;
        p.kind = PK_CONFETTI;
        p.x = (Rnd() - 0.5f) * 120;
        p.y = -10;
        float a = 3.14159f * (0.15f + 0.7f * Rnd()), sp = 90 + 160 * Rnd();
        p.vx = std::cos(a) * sp * (Rnd() < 0.5f ? -1 : 1) * 0.9f;
        p.vy = std::sin(a) * sp * 0.6f - 40;
        p.life = p.maxLife = 1.8f + Rnd() * 1.2f;
        p.size = 3 + Rnd() * 3;
        p.rot = Rnd() * 6.28f;
        p.vr = (Rnd() - 0.5f) * 14;
        p.c = Rainbow();
        g_parts.push_back(p);
    }
}
static void Sparks(int n = 26) {
    if (!g_set.fxParticles) return;
    FxPad(70, 1.4);
    for (int i = 0; i < n; i++) {
        Particle p;
        p.kind = PK_SPARK;
        p.x = (Rnd() - 0.5f) * 40;
        p.y = -6;
        float a = Rnd() * 3.14159f, sp = 60 + 140 * Rnd();
        p.vx = std::cos(a) * sp;
        p.vy = std::sin(a) * sp * 0.7f;
        p.life = p.maxLife = 0.5f + Rnd() * 0.6f;
        p.size = 1.2f + Rnd() * 1.6f;
        p.rot = 0; p.vr = 0;
        p.c = Lerp(g_accent, Col(1, 1, 1), 0.4f * Rnd());
        g_parts.push_back(p);
    }
}
static void Fireworks(double sec = 8) {
    if (!g_set.fxParticles) return;
    FxPad(120, sec + 2);
    g_fireworksUntil = NowSec() + sec;
}
static void SpawnRocketBurst() {
    float bx = (Rnd() - 0.5f) * 220, by = 30 + Rnd() * 60;
    D2D1_COLOR_F c = Rainbow();
    for (int i = 0; i < 34; i++) {
        Particle p;
        p.kind = PK_SPARK;
        float a = Rnd() * 6.2832f, sp = 50 + 70 * Rnd();
        p.x = bx; p.y = by;
        p.vx = std::cos(a) * sp; p.vy = std::sin(a) * sp;
        p.life = p.maxLife = 0.9f + Rnd() * 0.6f;
        p.size = 1.4f + Rnd() * 1.4f;
        p.rot = 0; p.vr = 0;
        p.c = Lerp(c, Col(1, 1, 1), 0.25f * Rnd());
        g_parts.push_back(p);
    }
    PlayUi(SN_POP);
}
// сезонные: что сейчас за окном
static int SeasonKind() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (st.wMonth == 12 || st.wMonth <= 2) return PK_SNOW;
    if (st.wMonth <= 5) return PK_PETAL;
    if (st.wMonth <= 8) return PK_FIREFLY;
    return PK_LEAF;
}
static bool GarlandTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return (st.wMonth == 12 && st.wDay >= 15) || (st.wMonth == 1 && st.wDay <= 10);
}
static bool SeasonActive() {
    return g_sd.season && g_set.fxParticles && g_visible && !g_toolHidden && sHide.x < 0.5 &&
           (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_TIMER);
}
static float g_seasonAcc = 0;
static void FxStep(double dt) {
    double now = NowSec();
    // салют
    if (now < g_fireworksUntil && Rnd() < dt * 2.6) SpawnRocketBurst();
    // сезонные частицы — сыплются из-под острова
    if (SeasonActive()) {
        int k = SeasonKind();
        float rate = k == PK_SNOW ? 7.f : k == PK_FIREFLY ? 2.5f : 3.f;
        g_seasonAcc += (float)dt * rate;
        while (g_seasonAcc >= 1) {
            g_seasonAcc -= 1;
            if (g_parts.size() > 80) break;
            Particle p;
            p.kind = k;
            p.x = (Rnd() - 0.5f) * (g_L.pw + 40);
            p.y = -4 - Rnd() * 6;
            p.vx = (Rnd() - 0.5f) * (k == PK_FIREFLY ? 18 : 10);
            p.vy = k == PK_SNOW ? 10 + Rnd() * 10 : k == PK_FIREFLY ? 3 + Rnd() * 4 : 14 + Rnd() * 10;
            p.life = p.maxLife = k == PK_FIREFLY ? 2.5f + Rnd() : 2.6f + Rnd() * 1.2f;
            p.size = k == PK_SNOW ? 1.1f + Rnd() * 1.4f : k == PK_FIREFLY ? 1.5f : 2.6f + Rnd() * 1.6f;
            p.rot = Rnd() * 6.28f;
            p.vr = (Rnd() - 0.5f) * 3;
            p.c = k == PK_SNOW ? Col(1, 1, 1) : k == PK_PETAL ? Col(1.f, 0.72f, 0.82f) : k == PK_FIREFLY ? Col(1.f, 0.92f, 0.45f)
                                                                                              : HueShift(Col(1.f, 0.5f, 0.1f), Rnd() * 0.08f - 0.04f);
            g_parts.push_back(p);
        }
    }
    float fdt = (float)dt;
    for (auto& p : g_parts) {
        p.life -= fdt;
        switch (p.kind) {
            case PK_CONFETTI: p.vy += 260 * fdt; p.vx *= 1 - 1.2f * fdt; p.vy *= 1 - 0.6f * fdt; break;
            case PK_SPARK: p.vy += 90 * fdt; p.vx *= 1 - 2.2f * fdt; p.vy *= 1 - 1.5f * fdt; break;
            case PK_SNOW: p.vx += std::sin((float)now * 1.7f + p.rot) * 6 * fdt; break;
            case PK_LEAF:
            case PK_PETAL: p.vx = std::sin((float)now * 2.1f + p.rot) * 14; break;
            case PK_FIREFLY: p.vx += (Rnd() - 0.5f) * 30 * fdt; p.vy += (Rnd() - 0.5f) * 30 * fdt; break;
        }
        p.x += p.vx * fdt;
        p.y += p.vy * fdt;
        p.rot += p.vr * fdt;
    }
    g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(), [](const Particle& p) { return p.life <= 0; }), g_parts.end());
    if (g_parts.empty() && now > g_fxPadUntil && now > g_fireworksUntil) g_fxPad = 0;
}
static bool FxBusy() { return !g_parts.empty() || NowSec() < g_fireworksUntil || SeasonActive() || (g_sd.season && GarlandTime()); }
static void FxTick() {
    if (!FxBusy()) { KillTimer(g_hwnd, T_FX); return; }
    g_dirty = true;
}
static void DrawParticles(const Layout& L) {
    if (g_parts.empty()) return;
    float ox = L.px + L.pw / 2, oy = L.py + L.ph;
    for (auto& p : g_parts) {
        float a = std::min(1.f, p.life / std::min(0.6f, p.maxLife * 0.5f));
        float x = ox + p.x, y = oy + p.y;
        switch (p.kind) {
            case PK_CONFETTI: {
                D2D1_MATRIX_3X2_F t0;
                g_rt->GetTransform(&t0);
                g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(p.rot * 57.3f, Pt(x, y)) * t0);
                RRect(RF(x - p.size, y - p.size * 0.45f, p.size * 2, p.size * 0.9f), 0.8f, WithA(p.c, a));
                g_rt->SetTransform(t0);
                break;
            }
            case PK_SPARK:
                Circle(x, y, p.size * 2.2f, WithA(p.c, a * 0.18f));
                Circle(x, y, p.size, WithA(p.c, a));
                break;
            case PK_SNOW: Circle(x, y, p.size, WithA(p.c, a * 0.9f)); break;
            case PK_FIREFLY: {
                float tw = 0.5f + 0.5f * std::sin((float)NowSec() * 5 + p.rot * 3);
                Circle(x, y, 4.5f, WithA(p.c, a * 0.15f * tw));
                Circle(x, y, p.size, WithA(p.c, a * (0.4f + 0.6f * tw)));
                break;
            }
            case PK_LEAF:
            case PK_PETAL: {
                D2D1_MATRIX_3X2_F t0;
                g_rt->GetTransform(&t0);
                g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(p.rot * 57.3f, Pt(x, y)) * t0);
                SetColor(WithA(p.c, a * 0.9f));
                g_rt->FillEllipse(D2D1::Ellipse(Pt(x, y), p.size, p.size * 0.55f), g_brush);
                g_rt->SetTransform(t0);
                break;
            }
        }
    }
}
// новогодняя гирлянда по нижнему краю
static void DrawGarland(const Layout& L) {
    if (!g_sd.season || !GarlandTime() || L.ph < 16) return;
    static const D2D1_COLOR_F cols[4] = {{1.f, 0.3f, 0.3f, 1}, {1.f, 0.85f, 0.3f, 1}, {0.3f, 0.9f, 0.45f, 1}, {0.35f, 0.6f, 1.f, 1}};
    float y0 = L.py + L.ph + 1;
    int n = std::max(4, (int)((L.pw - 2 * L.pr) / 16));
    float x0 = L.px + L.pr, step = (L.pw - 2 * L.pr) / n;
    double t = NowSec();
    for (int i = 0; i <= n; i++) {
        float x = x0 + i * step, sag = 3.5f * std::sin(3.14159f * ((i % 2) ? 1 : 0.0f));
        float y = y0 + 2 + sag;
        float on = 0.35f + 0.65f * (0.5f + 0.5f * std::sin((float)t * 3 + i * 1.3f));
        D2D1_COLOR_F c = cols[i % 4];
        Circle(x, y, 4.5f, WithA(c, 0.18f * on));
        Circle(x, y, 2.1f, WithA(c, on));
    }
}

// ================================================================ перекатывающиеся цифры
struct RollState { std::wstring cur, prev; double t = -1; };
static std::map<int, RollState> g_rolls;
static void RollText(int id, IDWriteTextFormat* f, const std::wstring& s, D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT al, D2D1_COLOR_F c) {
    RollState& st = g_rolls[id];
    double now = NowSec();
    if (st.cur != s) {
        st.prev = st.cur;
        st.cur = s;
        st.t = now;
        if (!st.prev.empty() && g_set.fxRoll) g_rollUntil = std::max(g_rollUntil, now + 0.42);
    }
    float p = st.t < 0 ? 1.f : (float)Clamp((now - st.t) / 0.38, 0.0, 1.0);
    if (!g_set.fxRoll || p >= 1.f || st.prev.size() != st.cur.size()) {
        Text(f, s, r, al, c);
        return;
    }
    p = 1 - (1 - p) * (1 - p) * (1 - p);  // плавная остановка
    // раскладка по символам
    std::vector<float> w(s.size());
    float total = 0;
    for (size_t i = 0; i < s.size(); i++) { w[i] = MeasureW(f, s.substr(i, 1)); total += w[i]; }
    float x = al == DWRITE_TEXT_ALIGNMENT_CENTER ? (r.left + r.right - total) / 2 : al == DWRITE_TEXT_ALIGNMENT_TRAILING ? r.right - total : r.left;
    float h = r.bottom - r.top, d = std::min(h * 0.55f, 14.f);
    g_rt->PushAxisAlignedClip(&r, D2D1_ANTIALIAS_MODE_ALIASED);
    for (size_t i = 0; i < s.size(); i++) {
        D2D1_RECT_F cr = RF(x - 1, r.top, w[i] + 4, h);
        if (st.prev[i] == s[i]) Text(f, s.substr(i, 1), cr, DWRITE_TEXT_ALIGNMENT_LEADING, c);
        else {
            D2D1_RECT_F up = RF(cr.left, cr.top - d * p, cr.right - cr.left, h), dn = RF(cr.left, cr.top + d * (1 - p), cr.right - cr.left, h);
            Text(f, st.prev.substr(i, 1), up, DWRITE_TEXT_ALIGNMENT_LEADING, WithA(c, c.a * (1 - p)));
            Text(f, s.substr(i, 1), dn, DWRITE_TEXT_ALIGNMENT_LEADING, WithA(c, c.a * p));
        }
        x += w[i];
    }
    g_rt->PopAxisAlignedClip();
}

// ================================================================ прогресс по контуру
// frac < 0 — «бегущий» отрезок (когда размер загрузки неизвестен)
static void DrawOutlineProgress(const Layout& L, float frac, D2D1_COLOR_F c) {
    if (!g_set.fxOutline || (frac >= 0 && frac <= 0.001f) || L.ph < 16) return;
    bool spin = frac < 0;
    frac = spin ? 0.16f : std::min(frac, 1.f);
    float in = 1.6f, x = L.px + in, y = L.py + in, w = L.pw - 2 * in, h = L.ph - 2 * in, r = std::max(0.f, std::min(L.pr - in, h / 2));
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return; }
    float cx = x + w / 2;
    s->BeginFigure(Pt(cx, y), D2D1_FIGURE_BEGIN_HOLLOW);
    s->AddLine(Pt(x + w - r, y));
    if (r > 0.1f) Arc(s, Pt(x + w, y + r), r);
    s->AddLine(Pt(x + w, y + h - r));
    if (r > 0.1f) Arc(s, Pt(x + w - r, y + h), r);
    s->AddLine(Pt(x + r, y + h));
    if (r > 0.1f) Arc(s, Pt(x, y + h - r), r);
    s->AddLine(Pt(x, y + r));
    if (r > 0.1f) Arc(s, Pt(x + r, y), r);
    s->AddLine(Pt(cx, y));
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    s->Release();
    float len = 0;
    g->ComputeLength(nullptr, &len);
    if (len > 1) {
        const float W[2] = {5.f, 2.2f}, A[2] = {0.22f, 1.f};
        for (int k = 0; k < 2; k++) {
            float dash[2] = {len * frac / W[k], len / W[k] + 10};
            float off = spin ? -(float)std::fmod(NowSec() * 0.45, 1.0) * len / W[k] : 0.f;
            D2D1_STROKE_STYLE_PROPERTIES sp = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10,
                                               D2D1_DASH_STYLE_CUSTOM, off};
            ID2D1StrokeStyle* ss = nullptr;
            g_d2f->CreateStrokeStyle(&sp, dash, 2, &ss);
            SetColor(WithA(c, A[k]));
            if (ss) g_rt->DrawGeometry(g, g_brush, W[k], ss);
            SafeRelease(ss);
        }
        D2D1_POINT_2F head;
        if (!spin && SUCCEEDED(g->ComputePointAtLength(len * frac, nullptr, &head, nullptr))) {
            Circle(head.x, head.y, 4.5f, WithA(c, 0.3f));
            Circle(head.x, head.y, 2.2f, Col(1, 1, 1, 0.95f));
        }
    }
    SafeRelease(g);
}

// ================================================================ тряска «нет»
static void Shake() {
    sShake.v += 520;
    g_dirty = true;
    PlayUi(SN_ERROR);
}

// ================================================================ раздвоение острова (вторая активность «пузырём»)
enum { BUB_NONE, BUB_MEDIA, BUB_TIMER, BUB_DL };
static int g_bubKind = BUB_NONE, g_bubShown = BUB_NONE;
static D2D1_RECT_F g_bubRect = {};
static int SecondaryKind() {
    if (!g_set.fxSplit) return BUB_NONE;
    bool music = g_media.has && g_media.status == 4, dl = g_set.downloads && g_dl.active > 0;
    switch (g_mode) {
        case M_TIMER: return music ? BUB_MEDIA : dl ? BUB_DL : BUB_NONE;
        case M_DL: return g_tm.on ? BUB_TIMER : music ? BUB_MEDIA : BUB_NONE;
        case M_CALL:
        case M_REC: return g_tm.on ? BUB_TIMER : dl ? BUB_DL : music ? BUB_MEDIA : BUB_NONE;
        default: return BUB_NONE;
    }
}
static float BubbleSpace() { return (sBub.target > 0 || sBub.x > 0.01) ? 46.f : 0.f; }
static void UpdateBubble() {
    int k = SecondaryKind();
    if (k != BUB_NONE) g_bubShown = k;
    double t = k != BUB_NONE ? 1.0 : 0.0;
    if (t != sBub.target) { sBub.target = t; if (t > 0) PlayUi(SN_POP); g_dirty = true; }
}
static void DrawBubble(const Layout& L) {
    g_bubRect = D2D1_RECT_F{};
    float b = (float)Clamp(sBub.x, 0.0, 1.2);
    if (b < 0.01f || g_bubShown == BUB_NONE) return;
    float d = std::min(L.ph, 40.f), r = d / 2, cy = L.py + d / 2;
    float edge = L.px + L.pw;
    float cx = edge - r + (r + 8 + r) * b;  // из-под острова — наружу
    // «перемычка» как у ртути, пока капли близко
    if (b > 0.05f && b < 0.92f) {
        float neck = r * 0.75f * (1 - b);
        float x0 = edge - 6, x1 = cx - r * 0.6f;
        ID2D1PathGeometry* g = nullptr;
        ID2D1GeometrySink* s = nullptr;
        if (x1 > x0 && SUCCEEDED(g_d2f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
            float mx = (x0 + x1) / 2;
            s->BeginFigure(Pt(x0, cy - r * 0.8f), D2D1_FIGURE_BEGIN_FILLED);
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(mx, cy - neck), Pt(x1, cy - r * 0.8f)));
            s->AddLine(Pt(x1, cy + r * 0.8f));
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(mx, cy + neck), Pt(x0, cy + r * 0.8f)));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            s->Release();
            SetColor(g_sd.bg == BG_BLACK ? Col(0, 0, 0) : WithA(kBgSolid, g_set.opacity / 100.f));
            g_rt->FillGeometry(g, g_brush);
        }
        SafeRelease(g);
    }
    D2D1_COLOR_F bg = g_sd.bg == BG_BLACK ? Col(0, 0, 0) : g_sd.bg == BG_MATERIAL ? Lerp(Col(0.04f, 0.04f, 0.05f), g_wallColor, 0.2f)
                                                                                   : WithA(kBgSolid, std::max(0.9f, g_set.opacity / 100.f));
    if (g_sd.shadow) Circle(cx, cy + 3, r + 3, Col(0, 0, 0, g_light ? 0.08f : 0.2f));
    Circle(cx, cy, r, bg);
    if (g_sd.border == BD_NEON || g_sd.border == BD_TERM || g_sd.border == BD_HAIR || g_sd.border == BD_SPEC)
        StrokeArc(cx, cy, r - 0.5f, 0.f, 6.2832f, 1.f, g_sd.border == BD_NEON ? g_accent : Ov(0.18f));
    float a = std::min(1.f, b);
    if (b > 0.5f) {
        switch (g_bubShown) {
            case BUB_MEDIA: {
                float cs = d - 12;
                DrawCover(RF(cx - cs / 2, cy - cs / 2, cs, cs), cs / 2);
                break;
            }
            case BUB_TIMER: TimerRing(cx, cy, r - 7, 2.4f); break;
            case BUB_DL: {
                StrokeArc(cx, cy, r - 7, 0.f, 6.2832f, 2.4f, Ov(0.15f));
                float a0 = (float)std::fmod(NowSec() * 5, 6.2832);
                StrokeArc(cx, cy, r - 7, a0, a0 + 1.6f, 2.4f, g_accent);
                Line(cx, cy - 4, cx, cy + 4, 1.6f, kWhite);
                Line(cx - 3.5f, cy + 1, cx, cy + 4.5f, 1.6f, kWhite);
                Line(cx + 3.5f, cy + 1, cx, cy + 4.5f, 1.6f, kWhite);
                break;
            }
        }
    }
    (void)a;
    g_bubRect = RF(cx - r, cy - r, d, d);
}

// ================================================================ «потянуть вниз»
static bool g_pullDown = false, g_pullMoved = false;
static int g_pullY0 = 0;
static float g_pullBaseH = 0, g_pullBaseW = 0;
static bool PullCan() {
    return g_set.fxPull && (g_mode == M_IDLE || g_mode == M_COMPACT || g_mode == M_LYRICS || g_mode == M_TIMER || g_mode == M_DL);
}
static void PullStart(int y) {
    if (!PullCan()) return;
    g_pullDown = true;
    g_pullMoved = false;
    g_pullY0 = y;
    float w, h, r;
    TargetSize(g_mode, w, h, r);
    g_pullBaseH = h;
    g_pullBaseW = w;
    SetCapture(g_hwnd);
}
static bool PullMove(int y) {
    if (!g_pullDown) return false;
    float dy = (y - g_pullY0) / g_scale;
    if (std::fabs(dy) > 4) g_pullMoved = true;
    if (dy < 0) dy *= 0.25f;
    float st = 56.f * std::tanh(dy / 70.f);  // резинка: чем дальше, тем туже
    sH.target = g_pullBaseH + st;
    sW.target = g_pullBaseW + st * 0.8f;
    g_dirty = true;
    return true;
}
// true — жест забрал клик
static bool PullEnd(int y) {
    if (!g_pullDown) return false;
    g_pullDown = false;
    ReleaseCapture();
    float dy = (y - g_pullY0) / g_scale;
    if (!g_pullMoved) { Retarget(); return false; }
    if (dy > 34) SetMode(M_EXPANDED);
    else Retarget();
    return true;
}

// ================================================================ «дыхание» обложки
static float CoverBreath() {
    if (!g_set.audioAnim || g_media.status != 4 || !g_vizOk) return 0;
    return std::min(1.f, g_spec.beat * 0.8f + g_level * 0.6f);
}
static void CoverGlow(D2D1_RECT_F r, float rad) {
    float b = CoverBreath();
    if (b < 0.02f) return;
    for (int i = 3; i >= 1; i--) {
        float e = i * 2.2f * b;
        RRect(RF(r.left - e, r.top - e, r.right - r.left + 2 * e, r.bottom - r.top + 2 * e), rad + e, WithA(g_accent, 0.10f * b));
    }
}

// ================================================================ Новый год
static double g_nyLastCheck = 0;
static bool NyCountdown(SYSTEMTIME& st) {
    GetLocalTime(&st);
    return g_set.fxNY && st.wMonth == 12 && st.wDay == 31 && st.wHour == 23;
}
static int DaysToNy(const SYSTEMTIME& st) { return st.wMonth == 12 && st.wDay >= 20 ? 32 - st.wDay : -1; }
static void NyTick() {
    SYSTEMTIME st;
    bool cd = NyCountdown(st);
    static bool wasCd = false;
    if (cd) { g_dirty = true; SetTimer(g_hwnd, T_NY, 1000, nullptr); }
    if (wasCd && !cd && st.wMonth == 1 && st.wDay == 1) {
        Fireworks(12);
        PlayUi(SN_DONE);
        wchar_t b[64];
        swprintf(b, 64, L"С Новым %d годом!", st.wYear);
        ShowActionToast(TK_SHELF, b, L"🎉", TA_NONE, L"", Col(1.f, 0.85f, 0.3f), 8000);
    }
    wasCd = cd;
    if (!cd) KillTimer(g_hwnd, T_NY);
}

// ================================================================ плавная смена стиля
static void StyleTargets(bool snap) {
    double top = g_set.topOffset;
    double ear = (g_sd.shape == SH_NOTCH && g_set.topOffset == 0) ? g_sd.ear : 0;
    if (snap) { sTop.Set(top); sEar.Set(ear); }
    else { sTop.target = top; sEar.target = ear; }
}
static void StyleMorphKick() {
    StyleTargets(false);
    g_modeT = NowSec();
    g_content = 0;
    sW.v -= 160;
    sH.v -= 40;
    g_auroraT += 1.3;
    Retarget();
    PlayUi(SN_POP);
}

// ================================================================ персонажи (свои, нарисованы кодом)
// Танцуют под музыку, следят глазами за курсором, моргают и засыпают, когда тебя нет.
static const int kCritterCount = 9;  // вместе с «Нет»
static const wchar_t* kCritters[kCritterCount] = {L"Нет", L"Капля", L"Котик", L"Робот", L"Призрак", L"Совёнок", L"Лягушонок", L"Пингвин", L"Лисёнок"};
static const wchar_t* kCritterSubs[kCritterCount] = {L"Без питомца", L"Цвета акцента", L"Мурчит под музыку", L"Светящиеся глаза", L"Парит и машет хвостом",
                                                     L"Не спит по ночам", L"Квакает от басов", L"Шлёпает лапками", L"Хитрый и рыжий"};
// «бытовые» движения в покое: подпрыгнуть, покачаться, оглядеться, сплющиться
enum { ACT_NONE, ACT_HOP, ACT_WIGGLE, ACT_LOOK, ACT_SQUISH };
static int g_act = 0;
static double g_actT0 = 0;
static float g_lookX = 0, g_lookY = 0;
static D2D1_POINT_2F g_critterPos = {-1000, -1000};
static bool g_sleepy = false;
static void Tri(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_COLOR_F col);
static DWORD IdleMs() {
    LASTINPUTINFO li = {sizeof(li)};
    if (!GetLastInputInfo(&li)) return 0;
    return GetTickCount() - li.dwTime;
}
static bool UserAway() { return IdleMs() > 120000; }
static bool NightTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wHour >= 23 || st.wHour < 5;
}
static void SetMood(int m, double sec) {
    if (g_set.critter <= 0 || !g_set.petReact) return;
    g_mood = m;
    g_moodUntil = NowSec() + sec;
    g_dirty = true;
}
static int Mood() { return NowSec() < g_moodUntil ? g_mood : 0; }
static void CritterBlinkTick() {
    SetTimer(g_hwnd, T_BLINK, 2200 + rand() % 3800, nullptr);
    if (!g_critterShown || g_sleepy || !g_visible) return;
    if (NightTime() && Mood() == 0 && rand() % 6 == 0) { SetMood(3, 1.8); return; }  // ночью позёвывает
    if (Mood() == 0 && g_set.petReact && rand() % 9 == 0) {  // примерно раз в полминуты чем-нибудь занят (дёшево по CPU)
        g_act = 1 + rand() % 4;
        g_actT0 = NowSec();
        g_actUntil = g_actT0 + (g_act == ACT_LOOK ? 1.4 : 0.9);
        g_dirty = true;
        return;
    }
    g_blinkUntil = NowSec() + 0.13;
    g_dirty = true;
}
static void Heart(float cx, float cy, float s, D2D1_COLOR_F c) {
    Circle(cx - s * 0.5f, cy - s * 0.2f, s * 0.55f, c);
    Circle(cx + s * 0.5f, cy - s * 0.2f, s * 0.55f, c);
    Tri(Pt(cx - s * 1.02f, cy), Pt(cx + s * 1.02f, cy), Pt(cx, cy + s * 1.05f), c);
}
static void CritterLook(POINT cp) {
    if (!g_critterShown) return;
    float dx = (cp.x - g_winPos.x) / g_scale - g_critterPos.x, dy = (cp.y - g_winPos.y) / g_scale - g_critterPos.y;
    static double petT = 0;  // курсор «гладит» персонажа: держим мышь прямо на нём
    if (dx * dx + dy * dy < 15.f * 15.f) {
        if (petT == 0) petT = NowSec();
        else if (NowSec() - petT > 0.9 && Mood() != 1) SetMood(1, 3.0);
    } else petT = 0;
    float d = std::sqrt(dx * dx + dy * dy) + 60;
    float lx = Clamp(dx / d * 1.4f, -1.f, 1.f), ly = Clamp(dy / d * 1.4f, -1.f, 1.f);
    if (std::fabs(lx - g_lookX) > 0.08f || std::fabs(ly - g_lookY) > 0.08f) { g_lookX = lx; g_lookY = ly; g_dirty = true; }
}
static void Eye(float x, float y, float r, bool closed, bool led) {
    int md = Mood();
    if (md == 1 && !closed) { Heart(x, y, r * 0.95f, Col(1.f, 0.3f, 0.45f)); return; }
    if (md == 3) closed = true;
    if (md == 2 && !closed) {  // обиделся: прищур и брови домиком наоборот
        float side = x < g_critterPos.x ? -1.f : 1.f;
        Circle(x, y + r * 0.15f, r * 0.85f, Col(1, 1, 1));
        Circle(x - r * 0.45f * side * -1.f, y + r * 0.3f, r * 0.45f, Col(0.06f, 0.06f, 0.08f));  // смотрит в сторону
        Line(x - r * 1.1f, y - r * (side < 0 ? 0.9f : 0.35f), x + r * 1.1f, y - r * (side < 0 ? 0.35f : 0.9f), std::max(1.2f, r * 0.42f),
             Col(0.08f, 0.08f, 0.1f));
        return;
    }
    if (closed) { Line(x - r, y, x + r, y, std::max(1.2f, r * 0.45f), Col(0.08f, 0.08f, 0.1f)); return; }
    if (led) {
        RRect(RF(x - r, y - r * 0.7f, 2 * r, 1.4f * r), r * 0.4f, g_accent);
        Circle(x + g_lookX * r * 0.35f, y + g_lookY * r * 0.25f, r * 0.35f, Col(1, 1, 1, 0.9f));
        return;
    }
    Circle(x, y, r, Col(1, 1, 1));
    Circle(x + g_lookX * r * 0.45f, y + g_lookY * r * 0.45f, r * 0.55f, Col(0.06f, 0.06f, 0.08f));
    Circle(x + g_lookX * r * 0.45f - r * 0.18f, y + g_lookY * r * 0.45f - r * 0.2f, r * 0.17f, Col(1, 1, 1, 0.9f));
}
static void Tri(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_COLOR_F col) {
    SetColor(col);
    D2D1_POINT_2F p[3] = {a, b, c};
    FillPoly(p, 3);
}
static void DrawCritter(int kind, float cx, float cy, float s) {
    if (kind <= 0) return;
    g_critterShown = true;
    g_critterPos = Pt(cx, cy);
    {   // реакции на музыку: включили — радуется, поставили на паузу — обижается, листают треки — злится
        static int lastSt = -1;
        static std::wstring lastTitle;
        static double lastSkip = 0;
        int st = g_media.has ? g_media.status : 0;
        if (lastSt >= 0 && st != lastSt) {
            if (st == 4) SetMood(1, 2.2);
            else if (lastSt == 4 && st == 5) SetMood(2, 2.5);
        }
        if (lastSt == 4 && st == 4 && !lastTitle.empty() && g_media.title != lastTitle) {
            if (NowSec() - lastSkip < 6) SetMood(2, 2.0);
            lastSkip = NowSec();
        }
        lastSt = st;
        lastTitle = g_media.title;
    }
    int mood = Mood();
    bool sleep = g_sleepy && mood == 0, blink = sleep || NowSec() < g_blinkUntil;
    float b = (g_media.status == 4 && g_set.audioAnim) ? CoverBreath() : 0.f;
    float bob = sleep ? std::sin((float)NowSec() * 1.2f) * 0.6f : -b * s * 0.14f;
    float sx = 1 + 0.10f * b, sy = 1 - 0.08f * b, rot = 0;
    float saveLook = g_lookX;
    if (NowSec() < g_actUntil && !sleep) {
        float t = (float)Clamp((NowSec() - g_actT0) / (g_actUntil - g_actT0), 0.0, 1.0), k = std::sin(t * 3.14159f);
        switch (g_act) {
            case ACT_HOP: bob -= k * s * 0.3f; sy *= 1 + 0.06f * k; sx *= 1 - 0.04f * k; break;          // прыжок с вытягиванием
            case ACT_WIGGLE: rot = std::sin(t * 12.566f) * (1 - t) * 14.f; break;                      // покачался
            case ACT_LOOK: g_lookX = std::sin(t * 6.2832f) * 0.95f; break;                             // огляделся по сторонам
            case ACT_SQUISH: sx *= 1 + 0.16f * k; sy *= 1 - 0.13f * k; break;                          // сплющился
        }
    }
    D2D1_MATRIX_3X2_F t0;
    g_rt->GetTransform(&t0);
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy, Pt(cx, cy + s * 0.45f)) * D2D1::Matrix3x2F::Rotation(rot, Pt(cx, cy + s * 0.4f)) *
                       D2D1::Matrix3x2F::Translation(0, bob) * t0);
    float r = s * 0.46f, er = s * 0.13f;
    switch (kind) {
        case 1: {  // Капля
            D2D1_COLOR_F c = Lerp(g_accent, Col(1, 1, 1), 0.15f);
            Tri(Pt(cx, cy - r * 1.45f), Pt(cx - r * 0.72f, cy - r * 0.35f), Pt(cx + r * 0.72f, cy - r * 0.35f), c);
            Circle(cx, cy, r, c);
            Circle(cx - r * 0.4f, cy - r * 0.35f, r * 0.18f, Col(1, 1, 1, 0.45f));
            Eye(cx - r * 0.33f, cy + r * 0.05f, er, blink, false);
            Eye(cx + r * 0.33f, cy + r * 0.05f, er, blink, false);
            break;
        }
        case 2: {  // Котик
            D2D1_COLOR_F c = Col(1.f, 0.66f, 0.32f);
            Tri(Pt(cx - r * 0.95f, cy - r * 0.2f), Pt(cx - r * 0.75f, cy - r * 1.3f), Pt(cx - r * 0.15f, cy - r * 0.8f), c);
            Tri(Pt(cx + r * 0.95f, cy - r * 0.2f), Pt(cx + r * 0.75f, cy - r * 1.3f), Pt(cx + r * 0.15f, cy - r * 0.8f), c);
            Circle(cx, cy, r, c);
            Eye(cx - r * 0.38f, cy - r * 0.05f, er, blink, false);
            Eye(cx + r * 0.38f, cy - r * 0.05f, er, blink, false);
            Tri(Pt(cx - r * 0.12f, cy + r * 0.28f), Pt(cx + r * 0.12f, cy + r * 0.28f), Pt(cx, cy + r * 0.42f), Col(1.f, 0.45f, 0.55f));
            Line(cx - r * 0.95f, cy + r * 0.3f, cx - r * 0.45f, cy + r * 0.35f, 0.8f, Col(1, 1, 1, 0.6f));
            Line(cx + r * 0.95f, cy + r * 0.3f, cx + r * 0.45f, cy + r * 0.35f, 0.8f, Col(1, 1, 1, 0.6f));
            break;
        }
        case 3: {  // Робот
            D2D1_COLOR_F c = Col(0.74f, 0.79f, 0.88f);
            Line(cx, cy - r * 0.8f, cx, cy - r * 1.3f, 1.2f, c);
            Circle(cx, cy - r * 1.35f, r * 0.17f, g_accent);
            RRect(RF(cx - r, cy - r * 0.8f, 2 * r, 1.7f * r), r * 0.35f, c);
            RRect(RF(cx - r * 0.78f, cy - r * 0.55f, 1.56f * r, 1.05f * r), r * 0.25f, Col(0.12f, 0.13f, 0.17f));
            Eye(cx - r * 0.35f, cy - r * 0.08f, er * 1.05f, blink, true);
            Eye(cx + r * 0.35f, cy - r * 0.08f, er * 1.05f, blink, true);
            break;
        }
        case 4: {  // Призрак
            D2D1_COLOR_F c = Col(0.93f, 0.95f, 1.f, 0.95f);
            ID2D1PathGeometry* g = nullptr;
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_d2f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                float wv = (float)std::sin(NowSec() * 3) * r * 0.08f * (b > 0 ? 1.f : 0.f);
                sk->BeginFigure(Pt(cx - r, cy + r * 0.9f), D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine(Pt(cx - r, cy - r * 0.1f));
                D2D1_ARC_SEGMENT arc = {Pt(cx + r, cy - r * 0.1f), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
                sk->AddArc(&arc);
                sk->AddLine(Pt(cx + r, cy + r * 0.9f));
                for (int i = 0; i < 3; i++) {
                    float x0 = cx + r - (i + 0.5f) * (2 * r / 3), x1 = cx + r - (i + 1) * (2 * r / 3);
                    sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(x0, cy + r * 0.55f + wv), Pt(x1, cy + r * 0.9f)));
                }
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();
                sk->Release();
                SetColor(c);
                g_rt->FillGeometry(g, g_brush);
            }
            SafeRelease(g);
            if (blink) {
                Line(cx - r * 0.5f, cy - r * 0.05f, cx - r * 0.15f, cy - r * 0.05f, 1.4f, Col(0.1f, 0.1f, 0.15f));
                Line(cx + r * 0.15f, cy - r * 0.05f, cx + r * 0.5f, cy - r * 0.05f, 1.4f, Col(0.1f, 0.1f, 0.15f));
            } else {
                SetColor(Col(0.1f, 0.1f, 0.15f));
                g_rt->FillEllipse(D2D1::Ellipse(Pt(cx - r * 0.33f + g_lookX * r * 0.1f, cy - r * 0.05f + g_lookY * r * 0.1f), er * 0.75f, er * 1.1f), g_brush);
                g_rt->FillEllipse(D2D1::Ellipse(Pt(cx + r * 0.33f + g_lookX * r * 0.1f, cy - r * 0.05f + g_lookY * r * 0.1f), er * 0.75f, er * 1.1f), g_brush);
            }
            Circle(cx - r * 0.55f, cy + r * 0.25f, r * 0.13f, Col(1.f, 0.5f, 0.6f, 0.45f));
            Circle(cx + r * 0.55f, cy + r * 0.25f, r * 0.13f, Col(1.f, 0.5f, 0.6f, 0.45f));
            break;
        }
        case 5: {  // Совёнок
            D2D1_COLOR_F c = Col(0.62f, 0.46f, 0.33f);
            Tri(Pt(cx - r * 0.85f, cy - r * 0.5f), Pt(cx - r * 0.75f, cy - r * 1.25f), Pt(cx - r * 0.3f, cy - r * 0.85f), c);
            Tri(Pt(cx + r * 0.85f, cy - r * 0.5f), Pt(cx + r * 0.75f, cy - r * 1.25f), Pt(cx + r * 0.3f, cy - r * 0.85f), c);
            SetColor(c);
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy), r, r * 1.05f), g_brush);
            SetColor(Col(0.86f, 0.74f, 0.58f));
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy + r * 0.45f), r * 0.55f, r * 0.45f), g_brush);
            Circle(cx - r * 0.4f, cy - r * 0.2f, er * 1.55f, Col(0.95f, 0.85f, 0.65f));
            Circle(cx + r * 0.4f, cy - r * 0.2f, er * 1.55f, Col(0.95f, 0.85f, 0.65f));
            Eye(cx - r * 0.4f, cy - r * 0.2f, er * 1.1f, blink, false);
            Eye(cx + r * 0.4f, cy - r * 0.2f, er * 1.1f, blink, false);
            Tri(Pt(cx - r * 0.12f, cy + r * 0.02f), Pt(cx + r * 0.12f, cy + r * 0.02f), Pt(cx, cy + r * 0.25f), Col(1.f, 0.7f, 0.2f));
            break;
        }
        case 6: {  // Лягушонок
            D2D1_COLOR_F c = Col(0.45f, 0.82f, 0.38f), d = Col(0.32f, 0.66f, 0.28f);
            SetColor(c);
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy + r * 0.1f), r * 1.05f, r * 0.85f), g_brush);
            Circle(cx - r * 0.48f, cy - r * 0.62f, r * 0.4f, c);
            Circle(cx + r * 0.48f, cy - r * 0.62f, r * 0.4f, c);
            Eye(cx - r * 0.48f, cy - r * 0.64f, er * 1.15f, blink, false);
            Eye(cx + r * 0.48f, cy - r * 0.64f, er * 1.15f, blink, false);
            // улыбка
            ID2D1PathGeometry* g = nullptr;
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g_d2f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                sk->BeginFigure(Pt(cx - r * 0.45f, cy + r * 0.2f), D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(cx, cy + r * 0.55f), Pt(cx + r * 0.45f, cy + r * 0.2f)));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                sk->Release();
                SetColor(Col(0.12f, 0.3f, 0.12f));
                g_rt->DrawGeometry(g, g_brush, std::max(1.f, r * 0.1f));
            }
            SafeRelease(g);
            Circle(cx - r * 0.72f, cy + r * 0.22f, r * 0.12f, Col(1.f, 0.55f, 0.6f, 0.55f));
            Circle(cx + r * 0.72f, cy + r * 0.22f, r * 0.12f, Col(1.f, 0.55f, 0.6f, 0.55f));
            (void)d;
            break;
        }
        case 7: {  // Пингвин
            D2D1_COLOR_F body = Col(0.17f, 0.2f, 0.3f), belly = Col(0.96f, 0.96f, 0.98f), orange = Col(1.f, 0.62f, 0.2f);
            SetColor(orange);
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx - r * 0.35f, cy + r * 1.0f), r * 0.28f, r * 0.12f), g_brush);
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx + r * 0.35f, cy + r * 1.0f), r * 0.28f, r * 0.12f), g_brush);
            SetColor(body);
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy), r * 0.95f, r * 1.08f), g_brush);
            SetColor(belly);
            g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy + r * 0.18f), r * 0.66f, r * 0.82f), g_brush);
            Eye(cx - r * 0.3f, cy - r * 0.28f, er, blink, false);
            Eye(cx + r * 0.3f, cy - r * 0.28f, er, blink, false);
            Tri(Pt(cx - r * 0.16f, cy - r * 0.05f), Pt(cx + r * 0.16f, cy - r * 0.05f), Pt(cx, cy + r * 0.18f), orange);
            Circle(cx - r * 0.55f, cy + r * 0.05f, r * 0.1f, Col(1.f, 0.55f, 0.6f, 0.5f));
            Circle(cx + r * 0.55f, cy + r * 0.05f, r * 0.1f, Col(1.f, 0.55f, 0.6f, 0.5f));
            break;
        }
        case 8: {  // Лисёнок
            D2D1_COLOR_F c = Col(1.f, 0.55f, 0.2f), w = Col(1.f, 0.96f, 0.92f), k = Col(0.15f, 0.1f, 0.1f);
            Tri(Pt(cx - r * 1.0f, cy - r * 0.1f), Pt(cx - r * 0.8f, cy - r * 1.35f), Pt(cx - r * 0.15f, cy - r * 0.75f), c);
            Tri(Pt(cx + r * 1.0f, cy - r * 0.1f), Pt(cx + r * 0.8f, cy - r * 1.35f), Pt(cx + r * 0.15f, cy - r * 0.75f), c);
            Tri(Pt(cx - r * 0.86f, cy - r * 0.95f), Pt(cx - r * 0.8f, cy - r * 1.35f), Pt(cx - r * 0.6f, cy - r * 1.07f), k);
            Tri(Pt(cx + r * 0.86f, cy - r * 0.95f), Pt(cx + r * 0.8f, cy - r * 1.35f), Pt(cx + r * 0.6f, cy - r * 1.07f), k);
            Circle(cx, cy, r, c);
            Tri(Pt(cx - r * 0.98f, cy + r * 0.05f), Pt(cx, cy + r * 0.12f), Pt(cx - r * 0.2f, cy + r * 0.95f), w);
            Tri(Pt(cx + r * 0.98f, cy + r * 0.05f), Pt(cx, cy + r * 0.12f), Pt(cx + r * 0.2f, cy + r * 0.95f), w);
            Circle(cx, cy + r * 0.62f, r * 0.33f, w);
            Eye(cx - r * 0.38f, cy - r * 0.15f, er, blink, false);
            Eye(cx + r * 0.38f, cy - r * 0.15f, er, blink, false);
            Circle(cx, cy + r * 0.32f, r * 0.12f, k);
            break;
        }
    }
    if (mood == 3) {  // зевок: рот открывается и закрывается
        float k = (float)std::sin(Clamp((float)(1.0 - (g_moodUntil - NowSec()) / 1.8), 0.f, 1.f) * 3.14159f);
        SetColor(Col(0.12f, 0.05f, 0.07f));
        g_rt->FillEllipse(D2D1::Ellipse(Pt(cx, cy + r * 0.42f), r * 0.2f * (0.4f + k), r * 0.26f * k + 0.5f), g_brush);
    }
    if (mood == 2) {  // «злюка» сбоку
        float x = cx + r * 0.95f, y = cy - r * 0.95f, a = r * 0.22f;
        D2D1_COLOR_F rc = Col(1.f, 0.3f, 0.3f);
        Line(x - a, y - a, x - a * 0.3f, y - a * 0.3f, 1.2f, rc); Line(x + a, y - a, x + a * 0.3f, y - a * 0.3f, 1.2f, rc);
        Line(x - a, y + a, x - a * 0.3f, y + a * 0.3f, 1.2f, rc); Line(x + a, y + a, x + a * 0.3f, y + a * 0.3f, 1.2f, rc);
    }
    g_rt->SetTransform(t0);
    g_lookX = saveLook;
    if (mood == 1) {  // сердечки поднимаются вверх
        for (int i = 0; i < 2; i++) {
            float t = (float)std::fmod(NowSec() * 0.7 + i * 0.5, 1.0);
            Heart(cx + (i ? r * 0.9f : -r * 0.8f) + std::sin(t * 6.f + i) * 1.5f, cy - r * 0.9f - t * s * 0.6f, s * 0.09f * (1.f - t * 0.4f),
                  Col(1.f, 0.35f, 0.5f, 1.f - t));
        }
    }
    if (sleep) {
        float t = (float)std::fmod(NowSec() * 0.4, 1.0);
        Text(g_fSmall, L"z", RF(cx + r * 0.7f, cy - r * 1.2f - t * 4, 10, 12), DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.9f));
        Text(g_fSmall, L"z", RF(cx + r * 1.1f, cy - r * 1.7f - t * 4, 10, 12), DWRITE_TEXT_ALIGNMENT_LEADING, WithA(kGray, 0.6f));
    }
}

// ================================================================ щит VPN
static void IconShield(float cx, float cy, D2D1_COLOR_F c, bool on) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (SUCCEEDED(g_d2f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&s))) {
        s->BeginFigure(Pt(cx, cy - 8), D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(Pt(cx + 7, cy - 5.5f));
        s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(cx + 7, cy + 5), Pt(cx, cy + 8.5f)));
        s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt(cx - 7, cy + 5), Pt(cx - 7, cy - 5.5f)));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        SetColor(c);
        if (on) g_rt->FillGeometry(g, g_brush);
        else g_rt->DrawGeometry(g, g_brush, 1.6f);
    }
    SafeRelease(g);
    if (on) { Line(cx - 3, cy, cx - 0.5f, cy + 2.5f, 1.7f, Col(1, 1, 1)); Line(cx - 0.5f, cy + 2.5f, cx + 3.5f, cy - 2.5f, 1.7f, Col(1, 1, 1)); }
}
