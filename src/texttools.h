// texttools.h — преобразования текста для вкладки «Текст»
#pragma once
#include <string>

enum TextOp {
    TT_LAYOUT,      // ghbdtn -> привет и обратно
    TT_UPPER, TT_LOWER, TT_TITLE, TT_SENTENCE,
    TT_TRANSLIT,    // кириллица -> латиница
    TT_JOINLINES,   // убрать переносы
    TT_SPACES,      // лишние пробелы
    TT_JSON_PRETTY, TT_JSON_MIN,
    TT_B64_ENC, TT_B64_DEC,
    TT_URL_ENC, TT_URL_DEC,
    TT_SORT_LINES, TT_UNIQUE_LINES,
    TT_COUNT
};
const wchar_t* TextOpName(int op);
// false — преобразование не удалось (например, неверный JSON/Base64); err — почему
bool ApplyTextOp(int op, const std::wstring& in, std::wstring& out, std::wstring& err);
