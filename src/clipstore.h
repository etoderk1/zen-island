// clipstore.h — история буфера обмена в SQLite (запись в фоновом потоке)
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>

#define WM_APP_CLIP (WM_APP + 6)  // lParam = ClipUpdate* (освобождает UI)

enum ClipKind { CK_TEXT = 0, CK_LINK = 1, CK_IMAGE = 2, CK_FILES = 3 };

struct ClipItem {
    long long id = 0;
    int kind = CK_TEXT;
    std::wstring preview;        // первые ~400 символов (для списка и поиска)
    int textLen = 0;
    int w = 0, h = 0;            // размер картинки
    std::vector<BYTE> thumb;     // 64x64 PBGRA (для картинок)
    int tw = 0, th = 0;
    bool pinned = false;
    long long created = 0;       // unix ms
};

struct ClipUpdate {
    bool reload = false;               // перечитать всё (после очистки)
    bool hasItem = false;
    ClipItem item;                     // добавленный/поднятый наверх
    std::vector<long long> removed;    // удалённые id (лимит)
};

class ClipStore {
public:
    bool Open(const std::wstring& dbPath, HWND notify, int limit);
    void Close();
    std::vector<ClipItem> LoadAll();   // UI-поток

    void AddText(const std::wstring& text, int kind);
    void AddFiles(const std::wstring& paths);
    void AddImage(std::vector<BYTE>&& bgra, int w, int h);  // 32bpp top-down
    void SetPinned(long long id, bool pinned);
    void Delete(long long id);
    void Clear();                      // удаляет всё, кроме закреплённых
    void SetLimit(int limit);

    // Полные данные для вставки (UI-поток)
    bool GetText(long long id, std::wstring& out);
    bool GetImage(long long id, std::vector<BYTE>& bgra, int& w, int& h);

private:
    void Run();
    void Post(std::function<void(void*)> job);
    void* ui_ = nullptr;   // sqlite3* для UI
    HWND notify_ = nullptr;
    int limit_ = 200;
    std::thread th_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<std::function<void(void*)>> jobs_;
    bool quit_ = false;
    std::wstring path_;
};
