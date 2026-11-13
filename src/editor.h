// editor.h — редактор разметки скриншота: стрелки, рамки, маркер, карандаш, текст, номера, размытие, обрезка
#pragma once
#include <windows.h>
#include <string>
#include <vector>

#define WM_APP_MARKUP (WM_APP + 23)  // lParam = MarkupResult*

enum MarkupAction { MK_CANCEL, MK_DONE, MK_COPY, MK_SAVED, MK_DRAGGED };
struct MarkupResult {
    int action = MK_CANCEL;
    std::vector<BYTE> px;  // BGRA итоговой картинки (с обрезкой)
    int w = 0, h = 0;
    std::wstring path;     // для MK_SAVED / MK_DRAGGED — куда сохранили
};

// Открывает редактор (немодальное окно). По закрытию шлёт WM_APP_MARKUP в notify.
// shotDir — куда класть файл при перетаскивании. accent — цвет кнопки «Готово».
bool OpenMarkupEditor(HWND notify, std::vector<BYTE>&& bgra, int w, int h, const std::wstring& shotDir, bool light, COLORREF accent);
bool MarkupEditorOpen();
