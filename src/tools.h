// tools.h — инструменты экрана: скриншоты, OCR, QR, пипетка, выбор области для записи
#pragma once
#include <windows.h>
#include <string>
#include <vector>

#define WM_APP_TOOL (WM_APP + 7)  // lParam = ToolResult* (освобождает UI)

enum ToolMode { TM_SHOT_AREA = 1, TM_SHOT_FULL, TM_OCR, TM_QR, TM_PICK, TM_REC };

struct ToolResult {
    int mode = 0;
    bool cancel = false;
    RECT rc = {};                 // область в координатах виртуального экрана (физические пиксели)
    std::vector<BYTE> px;         // BGRA сверху вниз
    int w = 0, h = 0;
    COLORREF color = 0;
};

// Снимает экран и показывает оверлей выделения (для TM_SHOT_FULL — сразу результат)
bool StartTool(HWND notify, int mode);
bool ToolActive();

bool SavePng(const std::wstring& path, const std::vector<BYTE>& bgra, int w, int h);
bool CopyImageToClipboard(HWND owner, const std::vector<BYTE>& bgra, int w, int h);
bool CopyTextToClipboard(HWND owner, const std::wstring& text);

// Блокирующие — вызывать в фоновом потоке
bool RunOcr(const std::vector<BYTE>& bgra, int w, int h, std::wstring& out, std::wstring& err);
bool DecodeQr(const std::vector<BYTE>& bgra, int w, int h, std::wstring& out);
