// extras.inc — этапы 14–15: разметка скриншотов, карточка наушников, CPU/GPU/сеть, быстрый ответ на уведомления.
// Подключается в island.cpp после cmd.inc. Состояние объявлено выше (рядом с g_hotId).
// ----------------------------------------------------------------------------

// ================================================================ разметка скриншота
static void DragFileOut(const std::wstring& path) {
    static const GUID kBHID_DataObject = {0xB8C0BD9F, 0xED24, 0x455C, {0x83, 0xE6, 0xD5, 0x39, 0x0C, 0x4F, 0xE8, 0xC4}};
    IShellItem* it = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, (void**)&it)) || !it) return;
    IDataObject* dobj = nullptr;
    if (SUCCEEDED(it->BindToHandler(nullptr, kBHID_DataObject, IID_IDataObject, (void**)&dobj)) && dobj) {
        DWORD eff = 0;
        SHDoDragDrop(g_hwnd, dobj, nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &eff);
        dobj->Release();
    }
    it->Release();
}
static bool OpenMarkupFor(ToolResult* r) {
    COLORREF acc = RGB((int)(g_accent.r * 255), (int)(g_accent.g * 255), (int)(g_accent.b * 255));
    return OpenMarkupEditor(g_hwnd, std::move(r->px), r->w, r->h, g_baseDir + L"\\screenshots", g_light, acc);
}
static void OnMarkup(MarkupResult* r) {
    std::unique_ptr<MarkupResult> keep(r);
    switch (r->action) {
        case MK_CANCEL: return;
        case MK_COPY:
            CopyImageToClipboard(g_hwnd, r->px, r->w, r->h);
            ShowActionToast(TK_SHOT, L"Скриншот скопирован", L"", TA_NONE, L"", kWhite, 2200);
            return;
        case MK_SAVED:
            ShowActionToast(TK_SHOT, L"Скриншот сохранён", L"Показать", TA_SELECT, r->path, kWhite, 3500);
            return;
        case MK_DRAGGED:
            Sparks();
            PlayUi(SN_SEND);
            ShowActionToast(TK_SHOT, L"Скриншот отправлен", L"Показать", TA_SELECT, r->path, kWhite, 2500);
            return;
        case MK_DONE: {
            std::wstring dir = g_baseDir + L"\\screenshots";
            CreateDirectoryW(dir.c_str(), nullptr);
            std::wstring path = dir + L"\\" + StampName(L"Скриншот", L".png");
            bool saved = SavePng(path, r->px, r->w, r->h);
            CopyImageToClipboard(g_hwnd, r->px, r->w, r->h);
            if (saved && g_set.shelf) ShelfAdd(path);
            PlayUi(SN_POP);
            // тост можно утащить мышью прямо в Telegram
            ShowActionToast(TK_SHOT, saved ? L"В буфере · тащи в чат" : L"Скриншот скопирован", saved ? L"Открыть" : L"", saved ? TA_OPEN : TA_NONE,
                            path, kWhite, 6000);
            return;
        }
    }
}
// тост скриншота — перетаскиваемый
static bool ToastDragDown(int x, int y) {
    if (g_mode != M_TOAST || g_toast.kind != TK_SHOT || g_toast.arg.empty() || g_toast.action != TA_OPEN) return false;
    g_toastPress = true;
    g_toastPressPt = {x, y};
    SetCapture(g_hwnd);
    return true;
}
static bool ToastDragMove(int x, int y) {
    if (!g_toastPress) return false;
    if (std::abs(x - g_toastPressPt.x) > 6 || std::abs(y - g_toastPressPt.y) > 6) {
        g_toastPress = false;
        ReleaseCapture();
        std::wstring p = g_toast.arg;
        DragFileOut(p);
        if (g_mode == M_TOAST) SetMode(Collapsed());
    }
    return true;
}

// ================================================================ наушники
static bool LooksLikePods(const std::wstring& name) {
    std::wstring n = LowerW(name);
    return n.find(L"airpods") != std::wstring::npos || n.find(L"beats") != std::wstring::npos || n.find(L"powerbeats") != std::wstring::npos;
}
static void ShowDevCard() {
    if (g_mode == M_CLIP || g_mode == M_CMD || g_mode == M_EXPANDED || g_toolHidden) return;
    if (Quiet()) return;
    SetMode(M_DEV);
    SetTimer(g_hwnd, T_TOAST, 7000, nullptr);
}
static void PodsLookup(const std::wstring& name, bool manual) {
    g_podsWant = true;
    g_podsManual = manual;
    g_podsName = name;
    g_podsGot = false;
    PodsScanStart(g_hwnd, manual ? 10 : 14);
    if (manual) SetTimer(g_hwnd, T_PODSMISS, 10500, nullptr);
}
static void OnPods(PodsInfo* p) {
    std::unique_ptr<PodsInfo> keep(p);
    if (!g_podsWant) return;
    g_dev = DevCard{};
    g_dev.name = p->model;
    g_dev.pods = true;
    g_dev.single = p->single;
    g_dev.bat[0] = p->left; g_dev.bat[1] = p->right; g_dev.bat[2] = p->box;
    g_dev.chg[0] = p->chL; g_dev.chg[1] = p->chR; g_dev.chg[2] = p->chBox;
    bool first = !g_podsGot;
    g_podsGot = true;
    KillTimer(g_hwnd, T_PODSMISS);
    if (g_mode == M_DEV) g_dirty = true;
    else if (first) ShowDevCard();
}
static void DrawPodsGlyph(float cx, float cy, float k, D2D1_COLOR_F c) {
    // два «стебелька» AirPods
    for (int s = -1; s <= 1; s += 2) {
        float x = cx + s * 7 * k;
        Circle(x, cy - 6 * k, 6.5f * k, c);
        RRect(RF(x - (s < 0 ? 0.f : 3.f * k) - 1.5f * k, cy - 6 * k, 4.5f * k, 18 * k), 2.2f * k, c);
    }
}
static void DrawDevCard(float ox, float oy, float tw, float th) {
    (void)th;
    D2D1_RECT_F ic = RF(ox + 14, oy + 14, 56, 56);
    RRect(ic, 16, Ov(0.08f));
    if (g_dev.pods && !g_dev.single) DrawPodsGlyph(ic.left + 28, ic.top + 28, 1.1f, kWhite);
    else IconHeadphones(ic.left + 28, ic.top + 28, kWhite);
    float tx = ox + 84;
    Text(g_fToast, g_dev.name, RF(tx, oy + 12, tw - 100, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
    static const wchar_t* lab[3] = {L"Левый", L"Правый", L"Кейс"};
    int n = g_dev.single ? 1 : 3;
    float x = tx;
    for (int i = 0; i < n; i++) {
        int b = g_dev.bat[i];
        if (!g_dev.single && b < 0 && i < 2) { /* наушник в кейсе/далеко */ }
        D2D1_COLOR_F col = b < 0 ? kGray : b <= 20 ? Col(1.f, 0.42f, 0.32f) : g_dev.chg[i] ? Col(0.3f, 0.85f, 0.45f) : kWhite;
        Text(g_fSmall, g_dev.single ? L"Заряд" : lab[i], RF(x, oy + 38, 100, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        IconBattery(x + 13, oy + 66, b < 0 ? 0.f : b / 100.f, g_dev.chg[i], col);
        Text(g_fToast, b < 0 ? std::wstring(L"—") : std::to_wstring(b) + L"%", RF(x + 32, oy + 55, 70, 22), DWRITE_TEXT_ALIGNMENT_LEADING, col);
        x += 104;
    }
}

// ================================================================ CPU / GPU / сеть
static bool NeedStats() {
    if (!g_visible || g_toolHidden) return false;
    if (g_mode == M_EXPANDED && g_tab == TAB_CTRL) return true;
    if (g_game.on || g_probe.pid) return true;
    return g_set.idleWidget > 0 && g_mode == M_IDLE && sHide.x < 0.5;
}
static void StatsTick() {
    if (!NeedStats()) {
        KillTimer(g_hwnd, T_STATS);
        g_statsOn = false;
        g_stats.Close();
        g_ssOk = false;
        return;
    }
    if (g_stats.Sample(g_ss)) { g_ssOk = true; g_dirty = true; }
    if (g_probe.pid) GameProbeTick();
    if (g_game.on) {
        if (g_game.fpsOk) g_game.fps = FpsTake(g_game.pid);
        if (g_sess.active) {  // для итогов
            g_sess.last = NowSec();
            if (g_game.fps > 0) { g_sess.fpsSum += g_game.fps; g_sess.fpsN++; }
            if (g_ss.gpu > g_sess.gpuMax) g_sess.gpuMax = g_ss.gpu;
        }
        if (g_mode == M_GAME) { Retarget(); g_dirty = true; }
    }
}
static void EnsureStats() {
    if (g_statsOn || !NeedStats()) return;
    g_statsOn = true;
    SetTimer(g_hwnd, T_STATS, 1000, nullptr);
    StatsTick();
}
static void Spark(D2D1_RECT_F r, const float* h, float maxV, D2D1_COLOR_F c) {
    int n = g_stats.count;
    if (n < 2) return;
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_d2f->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return; }
    float w = r.right - r.left, hh = r.bottom - r.top;
    auto P = [&](int i) {
        int idx = (g_stats.pos - n + i + SysStats::N * 2) % SysStats::N;
        float v = std::min(1.f, h[idx] / std::max(1e-6f, maxV));
        return Pt(r.left + w * i / (n - 1), r.bottom - v * hh * 0.9f);  // растягиваем на всю ширину, даже если истории мало
    };
    s->BeginFigure(Pt(P(0).x, r.bottom), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 0; i < n; i++) s->AddLine(P(i));
    s->AddLine(Pt(P(n - 1).x, r.bottom));
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    s->Release();
    SetColor(WithA(c, 0.18f));
    g_rt->FillGeometry(g, g_brush);
    SafeRelease(g);
    for (int i = 1; i < n; i++) {
        D2D1_POINT_2F a = P(i - 1), b = P(i);
        Line(a.x, a.y, b.x, b.y, 1.5f, c);
    }
}
static void DrawStatsRow(D2D1_RECT_F c) {
    EnsureStats();
    float gap = 12, w = (c.right - c.left - 3 * gap) / 4;
    struct Card { const wchar_t* t; std::wstring v, sub; const float* h; float mx; D2D1_COLOR_F col; } cards[4];
    wchar_t b[64];
    swprintf(b, 64, L"%.0f%%", g_ss.cpu);
    cards[0] = {L"Процессор", g_ssOk ? b : L"…", L"", g_stats.hcpu, 100, Col(0.35f, 0.75f, 1.f)};
    swprintf(b, 64, L"%.0f%%", g_ss.gpu);
    cards[1] = {L"Видеокарта", g_ssOk ? (g_ss.gpu < 0 ? L"нет данных" : b) : L"…", L"", g_stats.hgpu, 100, Col(0.55f, 0.85f, 0.35f)};
    swprintf(b, 64, L"%.0f%%", g_ss.ram);
    wchar_t b2[64];
    swprintf(b2, 64, L"%.1f из %.0f ГБ", g_ss.ramUsedGb, g_ss.ramTotalGb);
    cards[2] = {L"Память", b, b2, nullptr, 100, Col(1.f, 0.7f, 0.3f)};
    float mx = 64 * 1024;
    for (int i = 0; i < SysStats::N; i++) mx = std::max(mx, std::max(g_stats.hdown[i], g_stats.hup[i]));
    cards[3] = {L"Сеть", L"↓ " + FmtSpeed(g_ss.down), L"↑ " + FmtSpeed(g_ss.up), g_stats.hdown, mx, Col(0.75f, 0.55f, 1.f)};
    for (int i = 0; i < 4; i++) {
        D2D1_RECT_F r = RF(c.left + i * (w + gap), c.top, w, c.bottom - c.top);
        RRect(r, 16, kCard);
        g_rt->PushAxisAlignedClip(&r, D2D1_ANTIALIAS_MODE_ALIASED);
        if (cards[i].h) Spark(RF(r.left, r.top + 30, w, r.bottom - r.top - 30), cards[i].h, cards[i].mx, cards[i].col);
        if (i == 2) {  // память — полоской
            RRect(RF(r.left + 14, r.bottom - 18, w - 28, 6), 3, Ov(0.12f));
            RRect(RF(r.left + 14, r.bottom - 18, std::max(6.f, (w - 28) * g_ss.ram / 100.f), 6), 3, cards[i].col);
        }
        g_rt->PopAxisAlignedClip();
        Text(g_fSmall, cards[i].t, RF(r.left + 14, r.top + 8, w - 20, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
        Text(g_fToast, cards[i].v, RF(r.left + 14, r.top + 26, w - 20, 22), DWRITE_TEXT_ALIGNMENT_LEADING, kWhite);
        if (!cards[i].sub.empty()) Text(g_fSmall, cards[i].sub, RF(r.left + 14, r.top + 48, w - 20, 16), DWRITE_TEXT_ALIGNMENT_LEADING, kGray);
    }
}

// ================================================================ быстрый ответ на уведомления
static bool NotifCanReply() { return g_notif && !g_notif->replyArgs.empty() && !g_set.notifPrivate; }
static void NotifReplyStart() {
    if (!NotifCanReply()) return;
    g_notifReply = true;
    KillTimer(g_hwnd, T_NOTIF);
    g_ie[IE_REPLY] = IEdit{};
    IEFocus(IE_REPLY);
    Retarget();
    g_dirty = true;
}
static void NotifReplyEnd(bool next) {
    g_notifReply = false;
    if (g_ieFocus == IE_REPLY) IEBlur(false);
    Retarget();
    g_dirty = true;
    if (next) ShowNextNotif();
    else SetTimer(g_hwnd, T_NOTIF, 4000, nullptr);
}
static void NotifInvokeAsync(const std::wstring& args, const std::wstring& input, const std::wstring& text, bool isReply) {
    if (!g_notif) return;
    std::wstring aumid = g_notif->aumid;
    HWND hw = g_hwnd;
    g_replySending = true;
    std::thread([hw, aumid, args, input, text, isReply] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ReplyResult* r = new ReplyResult;
        r->reply = isReply;
        r->ok = ToastInvoke(aumid, args, input, text, r->err);
        CoUninitialize();
        if (!PostMessageW(hw, WM_APP_REPLY, 0, (LPARAM)r)) delete r;
    }).detach();
}
static void NotifReplySend() {
    if (!g_notif || g_replySending) return;
    std::wstring t = g_ie[IE_REPLY].text;
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    if (t.empty()) return;
    NotifInvokeAsync(g_notif->replyArgs, g_notif->replyInput, t, true);
}
static void OnReplyResult(ReplyResult* r) {
    std::unique_ptr<ReplyResult> keep(r);
    g_replySending = false;
    if (r->ok) {
        g_notifReply = false;
        if (g_ieFocus == IE_REPLY) IEBlur(false);
        // показываем подтверждение и идём к следующему
        if (g_notif) { delete g_notif; g_notif = nullptr; }
        g_notifPic.Release();
        Sparks();
        PlayUi(SN_SEND);
        ShowActionToast(TK_TEXT, r->reply ? L"Ответ отправлен" : L"Отмечено прочитанным", L"", TA_NONE, L"", Col(0.3f, 0.85f, 0.45f), 1600);
    } else {
        Shake();
        g_replyErr = r->err;
        g_replyErrUntil = NowSec() + 4;
        g_dirty = true;
    }
}
// клик по карточке уведомления; true — обработали кнопку
static bool NotifClick(int x, int y) {
    if (!g_notif) return false;
    if (g_notifReply) {
        if (InRect(g_nrSend, x, y)) { NotifReplySend(); return true; }
        if (InRect(g_nrCancel, x, y)) { NotifReplyEnd(false); return true; }
        return true;  // мимо кнопок — ничего (печатают)
    }
    if (NotifCanReply() && InRect(g_nrReply, x, y)) { NotifReplyStart(); return true; }
    if (g_notif && !g_notif->readArgs.empty() && InRect(g_nrRead, x, y)) {
        NotifInvokeAsync(g_notif->readArgs, L"", L"", false);
        return true;
    }
    return false;
}
static void NotifHover(int x, int y) {
    int h = 0;
    if (InRect(g_nrReply, x, y)) h = 1;
    else if (InRect(g_nrRead, x, y)) h = 2;
    else if (InRect(g_nrSend, x, y)) h = 3;
    else if (InRect(g_nrCancel, x, y)) h = 4;
    if (h != g_notifHot) { g_notifHot = h; g_dirty = true; }
}
// кнопки и поле ответа — дорисовываются в DrawNotifPill
static void DrawNotifActions(float ox, float oy, float tw, float th) {
    (void)th;
    g_nrReply = g_nrRead = g_nrSend = g_nrCancel = g_rcReply = D2D1_RECT_F{};
    if (!g_notif) return;
    bool canReply = NotifCanReply(), canRead = !g_notif->readArgs.empty() && !g_set.notifPrivate;
    if (g_notifReply) {
        D2D1_RECT_F box = RF(ox + 14, oy + 66, tw - 28 - 96, 38);
        DrawIEdit(IE_REPLY, box, L"Ответ… (Enter — отправить)", 12);
        g_nrSend = RF(box.right + 8, oy + 66, 46, 38);
        g_nrCancel = RF(box.right + 58, oy + 66, 38, 38);
        RRect(g_nrSend, 19, g_replySending ? WithA(g_accent, 0.5f) : (g_notifHot == 3 ? Lerp(g_accent, Col(1, 1, 1), 0.12f) : g_accent));
        // стрелка «отправить»
        float cx = g_nrSend.left + 23, cy = g_nrSend.top + 19;
        Line(cx, cy + 7, cx, cy - 7, 2.f, Col(1, 1, 1));
        Line(cx - 6, cy - 1, cx, cy - 7, 2.f, Col(1, 1, 1));
        Line(cx + 6, cy - 1, cx, cy - 7, 2.f, Col(1, 1, 1));
        if (g_notifHot == 4) Circle(g_nrCancel.left + 19, g_nrCancel.top + 19, 17, Ov(0.1f));
        IconCross(g_nrCancel.left + 19, g_nrCancel.top + 19, kGray);
        if (!g_replyErr.empty() && NowSec() < g_replyErrUntil)
            Text(g_fSmall, g_replyErr, RF(ox + 16, oy + 106, tw - 32, 16), DWRITE_TEXT_ALIGNMENT_LEADING, Col(1.f, 0.5f, 0.4f));
        return;
    }
    if (!canReply && !canRead) return;
    // кнопки справа, показываем при наведении (а для Telegram — всегда, чтобы было видно, что можно ответить)
    float x = ox + tw - 14;
    if (canReply) {
        float w = MeasureW(g_fSmall, L"Ответить") + 26;
        g_nrReply = RF(x - w, oy + 32, w, 26);
        RRect(g_nrReply, 13, g_notifHot == 1 ? WithA(g_accent, 0.95f) : WithA(g_accent, 0.75f));
        Text(g_fSmall, L"Ответить", g_nrReply, DWRITE_TEXT_ALIGNMENT_CENTER, Col(1, 1, 1));
        x -= w + 6;
    }
    if (canRead && g_hover) {
        float w = MeasureW(g_fSmall, L"Прочитано") + 24;
        g_nrRead = RF(x - w, oy + 32, w, 26);
        RRect(g_nrRead, 13, Ov(g_notifHot == 2 ? 0.16f : 0.09f));
        Text(g_fSmall, L"Прочитано", g_nrRead, DWRITE_TEXT_ALIGNMENT_CENTER, kWhite);
    }
}

// ================================================================ VPN
static void VpnToast() {
    Toast t;
    t.kind = TK_VPN;
    t.flag = g_vpn.on;
    t.color = g_vpn.on ? Col(0.25f, 0.82f, 0.45f) : kGray;
    std::wstring where = g_vpn.country.empty() ? std::wstring() : g_vpn.country + (g_vpn.cc.empty() ? L"" : L" (" + g_vpn.cc + L")");
    if (g_vpn.on) t.text = L"VPN включён" + (where.empty() ? L"" : L" · " + where);
    else t.text = L"VPN выключен" + (where.empty() ? L"" : L" · " + where);
    t.right = g_vpn.ip;
    ShowToast(t, 3500);
}
static void OnVpn(VpnInfo* v) {
    std::unique_ptr<VpnInfo> keep(v);
    bool changed = g_vpnInit && (v->on != g_vpn.on || (v->ipOk && g_vpn.ipOk && v->ip != g_vpn.ip));
    if (!v->ipOk && g_vpn.ipOk && v->on == g_vpn.on) { v->ip = g_vpn.ip; v->country = g_vpn.country; v->cc = g_vpn.cc; v->ipOk = true; }
    g_vpn = *v;
    g_vpnInit = true;
    if (changed || g_vpnForceToast) VpnToast();
    g_vpnForceToast = false;
    g_dirty = true;
}

// ================================================================ «скоро дождь»
static std::wstring g_rainKey;
static void CheckRain() {
    if (!g_set.rainAlert || !g_wx.ok || g_wx.hours < 3 || Quiet()) return;
    auto wet = [](int code) { int ic = WeatherIcon(code); return ic == WX_RAIN || ic == WX_DRIZZLE || ic == WX_THUNDER || ic == WX_SNOW; };
    if (wet(g_wx.code)) return;  // уже идёт
    for (int i = 1; i <= 2 && i < g_wx.hours; i++) {
        if (wet(g_wx.hcode[i]) && g_wx.hprob[i] >= 50) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            std::wstring key = std::to_wstring(st.wDay) + L"-" + std::to_wstring(g_wx.hhour[i]);
            if (key == g_rainKey) return;
            g_rainKey = key;
            bool snow = WeatherIcon(g_wx.hcode[i]) == WX_SNOW;
            wchar_t b[96];
            swprintf(b, 96, L"%s около %d:00 — %s", snow ? L"Снег" : L"Дождь", g_wx.hhour[i], snow ? L"одевайся теплее" : L"возьми зонт");
            Toast t;
            t.kind = TK_RAIN;
            t.text = b;
            t.right = std::to_wstring(g_wx.hprob[i]) + L"%";
            t.color = Col(0.45f, 0.7f, 1.f);
            ShowToast(t, 6000);
            return;
        }
    }
}
