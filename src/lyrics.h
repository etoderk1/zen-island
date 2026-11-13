// lyrics.h — синхронизированные тексты песен из LRCLIB с кэшем на диске
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>

#define WM_APP_LYRICS (WM_APP + 3)

struct LyricWord {
    double t = -1;  // время начала слова (если в LRC есть пословные метки), иначе -1
    int start = 0, len = 0;  // диапазон символов в строке
};
struct LyricLine {
    double t = 0;  // секунды
    std::wstring text;
    std::vector<LyricWord> words;
    bool wordTimes = false;
};
struct Lyrics {
    std::vector<LyricLine> lines;
};

// Разбор LRC (обычного и «расширенного» с <mm:ss.xx> перед словами)
bool ParseLrc(const std::wstring& lrc, Lyrics& out);

enum { LYR_NONE = 0, LYR_FOUND = 1, LYR_NOTFOUND = 2 };

class LyricsService {
public:
    void Start(HWND notify, const std::wstring& cacheDir);
    void Stop();
    // Новый запрос отменяет предыдущий (берётся только самый свежий). Возвращает id.
    UINT Request(const std::wstring& title, const std::wstring& artist, const std::wstring& album, int durationSec);
    // Забрать результат для id (true, если готов)
    bool Take(UINT id, Lyrics& out, int& state);

private:
    struct Req {
        UINT id = 0;
        std::wstring title, artist, album;
        int dur = 0;
    };
    void Run();
    int Fetch(const Req& r, Lyrics& out);

    HWND notify_ = nullptr;
    std::wstring dir_;
    std::thread th_;
    std::mutex mx_;
    std::condition_variable cv_;
    bool quit_ = false, hasReq_ = false;
    Req req_;
    UINT nextId_ = 0, resId_ = 0;
    int resState_ = LYR_NONE;
    Lyrics res_;
};
