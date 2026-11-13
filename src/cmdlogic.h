// cmdlogic.h — «мозги» командной строки без окон: калькулятор, единицы, валюты, нечёткий поиск
#pragma once
#include <string>
#include <map>

// Калькулятор: 2^10, (3+4)*2, 15% от 3400, 3400+15%, sqrt(2), 2pi, sin(30)… (углы в градусах).
// false — строка не похожа на выражение.
bool CalcEval(const std::wstring& expr, double& out);

// Перевод единиц и валют: «5 км в мили», «100 usd в rub», «30 c в f», «2 гб в мб».
// rates: курсы к доллару (USD=1), может быть nullptr. needRates=true — это валюта, а курсов ещё нет.
bool UnitConvert(const std::wstring& q, const std::map<std::string, double>* rates, std::wstring& text, double& value,
                 bool& needRates);

std::wstring FmtNum(double v);          // 1 234 567,89 — для показа
std::wstring FmtNumPlain(double v);     // 1234567,89 — для копирования
std::wstring LowerW(std::wstring s);    // с кириллицей
// 0 — не подходит; больше — лучше. q и name уже в нижнем регистре.
int FuzzyScore(const std::wstring& name, const std::wstring& q);
