// reminders.h — напоминания одной фразой: «позвонить маме завтра в 9», «через 20 минут плита»,
// «таблетки каждый день в 23:00»
#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct Reminder {
    long long id = 0;
    long long due = 0;     // FILETIME UTC
    std::wstring text;
    bool daily = false;    // повторять каждый день
};

// now — FILETIME UTC; false + err, если время не понято
bool ParseReminder(const std::wstring& phrase, long long now, Reminder& out, std::wstring& err);
std::wstring ReminderWhen(long long due, long long now);  // «через 25 мин», «завтра 9:00», «пт 14.11 18:30»
void SaveReminders(const std::wstring& path, const std::vector<Reminder>& v);
std::vector<Reminder> LoadReminders(const std::wstring& path);
