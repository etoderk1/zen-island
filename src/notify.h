// notify.h — уведомления Windows без MSIX: читаем базу Центра уведомлений
// (%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db, это обычный SQLite)
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <map>

#define WM_APP_NOTIF (WM_APP + 13)  // lParam = NotifData*

struct NotifData {
    std::wstring aumid, app, title, body, error;
    std::vector<BYTE> icon;   // аватар (appLogoOverride) или иконка приложения, PBGRA
    int iw = 0, ih = 0;
    bool avatar = false;      // круглый аватар отправителя
    bool call = false;        // похоже на входящий звонок
    // быстрый ответ (Telegram, AyuGram и любые приложения с полем ответа в уведомлении)
    std::wstring replyArgs, replyInput, readArgs, readLabel;
};
struct ToastActions { std::wstring replyArgs, replyInput, readArgs, readLabel; };

// Разбор XML уведомления: заголовок, текст, путь к картинке-аватару
void ParseToastXml(const std::string& xml, std::wstring& title, std::wstring& body, std::wstring& image, ToastActions* acts = nullptr);
// Нажать кнопку уведомления за пользователя — так же, как это делает Windows: через COM-активатор приложения.
// text/inputId — что «ввели» в поле ответа (можно пусто). Блокирующий вызов: звать из фонового потока.
bool ToastInvoke(const std::wstring& aumid, const std::wstring& args, const std::wstring& inputId, const std::wstring& text, std::wstring& err);

class NotificationWatcher {
public:
    void Start(HWND notify);
    void Stop();

private:
    void Run();
    HWND notify_ = nullptr;
    HANDLE stopEv_ = nullptr;
    std::thread th_;
};
