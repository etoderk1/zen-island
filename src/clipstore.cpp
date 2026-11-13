// clipstore.cpp — SQLite-хранилище истории буфера обмена
#include "clipstore.h"
#include "third_party/sqlite3.h"
#include <wincodec.h>
#include <objidl.h>
#include <algorithm>
#include <memory>

static long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000ULL) - 11644473600000LL;
}
static long long Fnv(const void* data, size_t n, unsigned long long seed = 1469598103934665603ULL, size_t step = 1) {
    const BYTE* p = (const BYTE*)data;
    unsigned long long h = seed;
    for (size_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ULL; }
    return (long long)(h & 0x7FFFFFFFFFFFFFFFULL);
}

// ---------------------------------------------------------------------------
// WIC: PNG и миниатюры
// ---------------------------------------------------------------------------
static IWICImagingFactory* Wic() {
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&f);
    return f;
}
static bool EncodePng(IWICImagingFactory* wic, const std::vector<BYTE>& bgra, int w, int h, std::vector<BYTE>& out) {
    if (!wic) return false;
    IStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    do {
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &st))) break;
        if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&fr, nullptr))) break;
        if (FAILED(fr->Initialize(nullptr))) break;
        fr->SetSize(w, h);
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&pf);
        if (FAILED(fr->WritePixels(h, w * 4, (UINT)bgra.size(), (BYTE*)bgra.data()))) break;
        if (FAILED(fr->Commit()) || FAILED(enc->Commit())) break;
        HGLOBAL hg = nullptr;
        if (FAILED(GetHGlobalFromStream(st, &hg))) break;
        STATSTG ss = {};
        st->Stat(&ss, STATFLAG_NONAME);
        size_t n = (size_t)ss.cbSize.QuadPart;
        void* p = GlobalLock(hg);
        if (!p) break;
        out.assign((BYTE*)p, (BYTE*)p + n);
        GlobalUnlock(hg);
        ok = true;
    } while (false);
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    return ok;
}
static bool DecodeImage(IWICImagingFactory* wic, const BYTE* data, size_t n, std::vector<BYTE>& bgra, int& w, int& h) {
    if (!wic || !n) return false;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    do {
        if (FAILED(wic->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)data, (DWORD)n))) break;
        if (FAILED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        UINT uw = 0, uh = 0;
        fr->GetSize(&uw, &uh);
        if (FAILED(wic->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            break;
        bgra.resize((size_t)uw * uh * 4);
        if (FAILED(cv->CopyPixels(nullptr, uw * 4, (UINT)bgra.size(), bgra.data()))) break;
        w = (int)uw;
        h = (int)uh;
        ok = true;
    } while (false);
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    return ok;
}
// Квадратная миниатюра 64x64 (центр картинки), PBGRA
static bool MakeThumb(IWICImagingFactory* wic, const std::vector<BYTE>& bgra, int w, int h, std::vector<BYTE>& out) {
    if (!wic) return false;
    IWICBitmap* bmp = nullptr;
    IWICBitmapClipper* cl = nullptr;
    IWICBitmapScaler* sc = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    const UINT S = 64;
    do {
        if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, (UINT)bgra.size(),
                                               (BYTE*)bgra.data(), &bmp)))
            break;
        int side = std::min(w, h);
        WICRect rc = {(w - side) / 2, (h - side) / 2, side, side};
        if (FAILED(wic->CreateBitmapClipper(&cl)) || FAILED(cl->Initialize(bmp, &rc))) break;
        if (FAILED(wic->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(cl, S, S, WICBitmapInterpolationModeFant))) break;
        if (FAILED(wic->CreateFormatConverter(&cv)) ||
            FAILED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            break;
        out.resize(S * S * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, S * 4, (UINT)out.size(), out.data()));
    } while (false);
    if (cv) cv->Release();
    if (sc) sc->Release();
    if (cl) cl->Release();
    if (bmp) bmp->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// SQLite
// ---------------------------------------------------------------------------
static sqlite3* DB(void* p) { return (sqlite3*)p; }
static void Exec(sqlite3* db, const char* sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }
static sqlite3* OpenDb(const std::wstring& path) {
    sqlite3* db = nullptr;
    if (sqlite3_open16(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 3000);
    Exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    return db;
}

static const char* kSelectItem =
    "SELECT id,kind,substr(text,1,400),length(text),w,h,thumb,pinned,created FROM clips";

static ClipItem ReadItem(sqlite3_stmt* s) {
    ClipItem it;
    it.id = sqlite3_column_int64(s, 0);
    it.kind = sqlite3_column_int(s, 1);
    const void* t = sqlite3_column_text16(s, 2);
    if (t) it.preview = (const wchar_t*)t;
    it.textLen = sqlite3_column_int(s, 3);
    it.w = sqlite3_column_int(s, 4);
    it.h = sqlite3_column_int(s, 5);
    const void* b = sqlite3_column_blob(s, 6);
    int bn = sqlite3_column_bytes(s, 6);
    if (b && bn == 64 * 64 * 4) {
        it.thumb.assign((const BYTE*)b, (const BYTE*)b + bn);
        it.tw = it.th = 64;
    }
    it.pinned = sqlite3_column_int(s, 7) != 0;
    it.created = sqlite3_column_int64(s, 8);
    return it;
}
static bool LoadOne(sqlite3* db, long long id, ClipItem& out) {
    std::string q = std::string(kSelectItem) + " WHERE id=?";
    sqlite3_stmt* s = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db, q.c_str(), -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW) { out = ReadItem(s); ok = true; }
    }
    sqlite3_finalize(s);
    return ok;
}
static std::vector<long long> Prune(sqlite3* db, int limit) {
    std::vector<long long> ids;
    sqlite3_stmt* s = nullptr;
    const char* q =
        "DELETE FROM clips WHERE pinned=0 AND id NOT IN "
        "(SELECT id FROM clips WHERE pinned=0 ORDER BY created DESC LIMIT ?) RETURNING id";
    if (sqlite3_prepare_v2(db, q, -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, limit);
        while (sqlite3_step(s) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(s, 0));
    }
    sqlite3_finalize(s);
    return ids;
}
// Уже есть такая запись? Тогда просто поднимаем её наверх
static long long Bump(sqlite3* db, long long hash, int kind) {
    sqlite3_stmt* s = nullptr;
    long long id = 0;
    if (sqlite3_prepare_v2(db, "SELECT id FROM clips WHERE hash=? AND kind=? LIMIT 1", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, hash);
        sqlite3_bind_int(s, 2, kind);
        if (sqlite3_step(s) == SQLITE_ROW) id = sqlite3_column_int64(s, 0);
    }
    sqlite3_finalize(s);
    if (id) {
        if (sqlite3_prepare_v2(db, "UPDATE clips SET created=? WHERE id=?", -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, NowMs());
            sqlite3_bind_int64(s, 2, id);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
    }
    return id;
}

// ---------------------------------------------------------------------------
bool ClipStore::Open(const std::wstring& dbPath, HWND notify, int limit) {
    path_ = dbPath;
    notify_ = notify;
    limit_ = limit;
    sqlite3* db = OpenDb(dbPath);
    if (!db) return false;
    Exec(db,
         "CREATE TABLE IF NOT EXISTS clips(id INTEGER PRIMARY KEY AUTOINCREMENT, kind INTEGER, text TEXT, "
         "hash INTEGER, w INTEGER DEFAULT 0, h INTEGER DEFAULT 0, png BLOB, thumb BLOB, pinned INTEGER DEFAULT 0, "
         "created INTEGER);"
         "CREATE INDEX IF NOT EXISTS ix_clips_hash ON clips(hash);"
         "CREATE INDEX IF NOT EXISTS ix_clips_created ON clips(pinned, created);");
    ui_ = db;
    th_ = std::thread([this] { Run(); });
    return true;
}
void ClipStore::Close() {
    {
        std::lock_guard<std::mutex> g(mx_);
        quit_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    if (ui_) { sqlite3_close(DB(ui_)); ui_ = nullptr; }
}
void ClipStore::Post(std::function<void(void*)> job) {
    if (!ui_) return;
    {
        std::lock_guard<std::mutex> g(mx_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void ClipStore::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    sqlite3* db = OpenDb(path_);
    for (;;) {
        std::function<void(void*)> job;
        {
            std::unique_lock<std::mutex> lk(mx_);
            cv_.wait(lk, [this] { return quit_ || !jobs_.empty(); });
            if (jobs_.empty() && quit_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (db) job(db);
    }
    if (db) sqlite3_close(db);
    CoUninitialize();
}

static void Notify(HWND n, ClipUpdate* u) {
    if (!PostMessageW(n, WM_APP_CLIP, 0, (LPARAM)u)) delete u;
}

void ClipStore::AddText(const std::wstring& text, int kind) {
    HWND n = notify_;
    int* lim = &limit_;
    Post([text, kind, n, lim](void* p) {
        sqlite3* db = DB(p);
        long long hash = Fnv(text.data(), text.size() * sizeof(wchar_t), 1469598103934665603ULL ^ (unsigned)kind);
        ClipUpdate* u = new ClipUpdate;
        long long id = Bump(db, hash, kind);
        if (!id) {
            sqlite3_stmt* s = nullptr;
            if (sqlite3_prepare_v2(db, "INSERT INTO clips(kind,text,hash,created) VALUES(?,?,?,?)", -1, &s, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(s, 1, kind);
                sqlite3_bind_text16(s, 2, text.c_str(), (int)(text.size() * sizeof(wchar_t)), SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 3, hash);
                sqlite3_bind_int64(s, 4, NowMs());
                if (sqlite3_step(s) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db);
            }
            sqlite3_finalize(s);
            u->removed = Prune(db, *lim);
        }
        if (id && LoadOne(db, id, u->item)) u->hasItem = true;
        Notify(n, u);
    });
}
void ClipStore::AddFiles(const std::wstring& paths) { AddText(paths, CK_FILES); }

void ClipStore::AddImage(std::vector<BYTE>&& bgra, int w, int h) {
    HWND n = notify_;
    int* lim = &limit_;
    auto px = std::make_shared<std::vector<BYTE>>(std::move(bgra));
    Post([px, w, h, n, lim](void* p) {
        sqlite3* db = DB(p);
        long long hash = Fnv(px->data(), px->size(), 1469598103934665603ULL ^ (unsigned)(w * 31 + h), 7);
        ClipUpdate* u = new ClipUpdate;
        long long id = Bump(db, hash, CK_IMAGE);
        if (!id) {
            IWICImagingFactory* wic = Wic();
            std::vector<BYTE> png, thumb;
            bool okPng = EncodePng(wic, *px, w, h, png);
            MakeThumb(wic, *px, w, h, thumb);
            if (wic) wic->Release();
            if (okPng) {
                sqlite3_stmt* s = nullptr;
                const char* q = "INSERT INTO clips(kind,text,hash,w,h,png,thumb,created) VALUES(?,?,?,?,?,?,?,?)";
                if (sqlite3_prepare_v2(db, q, -1, &s, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int(s, 1, CK_IMAGE);
                    sqlite3_bind_text16(s, 2, L"", 0, SQLITE_STATIC);
                    sqlite3_bind_int64(s, 3, hash);
                    sqlite3_bind_int(s, 4, w);
                    sqlite3_bind_int(s, 5, h);
                    sqlite3_bind_blob(s, 6, png.data(), (int)png.size(), SQLITE_TRANSIENT);
                    if (!thumb.empty()) sqlite3_bind_blob(s, 7, thumb.data(), (int)thumb.size(), SQLITE_TRANSIENT);
                    else sqlite3_bind_null(s, 7);
                    sqlite3_bind_int64(s, 8, NowMs());
                    if (sqlite3_step(s) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db);
                }
                sqlite3_finalize(s);
                u->removed = Prune(db, *lim);
            }
        }
        if (id && LoadOne(db, id, u->item)) u->hasItem = true;
        Notify(n, u);
    });
}
void ClipStore::SetPinned(long long id, bool pinned) {
    Post([id, pinned](void* p) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(DB(p), "UPDATE clips SET pinned=? WHERE id=?", -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, pinned ? 1 : 0);
            sqlite3_bind_int64(s, 2, id);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
    });
}
void ClipStore::Delete(long long id) {
    Post([id](void* p) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(DB(p), "DELETE FROM clips WHERE id=?", -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, id);
            sqlite3_step(s);
        }
        sqlite3_finalize(s);
    });
}
void ClipStore::Clear() {
    HWND n = notify_;
    Post([n](void* p) {
        Exec(DB(p), "DELETE FROM clips WHERE pinned=0; PRAGMA wal_checkpoint(TRUNCATE);");
        Exec(DB(p), "VACUUM;");
        ClipUpdate* u = new ClipUpdate;
        u->reload = true;
        Notify(n, u);
    });
}
void ClipStore::SetLimit(int limit) {
    HWND n = notify_;
    Post([this, limit, n](void* p) {
        limit_ = limit;
        ClipUpdate* u = new ClipUpdate;
        u->removed = Prune(DB(p), limit);
        Notify(n, u);
    });
}

std::vector<ClipItem> ClipStore::LoadAll() {
    std::vector<ClipItem> v;
    if (!ui_) return v;
    std::string q = std::string(kSelectItem) + " ORDER BY pinned DESC, created DESC LIMIT 2000";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(DB(ui_), q.c_str(), -1, &s, nullptr) == SQLITE_OK)
        while (sqlite3_step(s) == SQLITE_ROW) v.push_back(ReadItem(s));
    sqlite3_finalize(s);
    return v;
}
bool ClipStore::GetText(long long id, std::wstring& out) {
    if (!ui_) return false;
    sqlite3_stmt* s = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(DB(ui_), "SELECT text FROM clips WHERE id=?", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const void* t = sqlite3_column_text16(s, 0);
            int n = sqlite3_column_bytes16(s, 0);
            if (t) out.assign((const wchar_t*)t, n / sizeof(wchar_t));
            ok = true;
        }
    }
    sqlite3_finalize(s);
    return ok;
}
bool ClipStore::GetImage(long long id, std::vector<BYTE>& bgra, int& w, int& h) {
    if (!ui_) return false;
    sqlite3_stmt* s = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(DB(ui_), "SELECT png FROM clips WHERE id=?", -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const void* b = sqlite3_column_blob(s, 0);
            int n = sqlite3_column_bytes(s, 0);
            IWICImagingFactory* wic = Wic();
            ok = b && DecodeImage(wic, (const BYTE*)b, (size_t)n, bgra, w, h);
            if (wic) wic->Release();
        }
    }
    sqlite3_finalize(s);
    return ok;
}
