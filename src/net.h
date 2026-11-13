// net.h — HTTP GET (WinHTTP, системный прокси) и мини-JSON
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <utility>

namespace net {

struct JVal {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* Get(const char* k) const {
        for (auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    double GetNum(const char* k, double def = 0) const { const JVal* v = Get(k); return v && v->t == JVal::Num ? v->n : def; }
    std::string GetStr(const char* k) const { const JVal* v = Get(k); return v && v->t == JVal::Str ? v->s : std::string(); }
};

bool ParseJson(const std::string& s, JVal& v);
// true — сервер ответил (status заполнен); body — тело ответа
bool HttpGet(const std::wstring& url, DWORD& status, std::string& body, DWORD timeoutMs = 10000);
std::wstring UrlEnc(const std::wstring& w);
std::string ToU8(const std::wstring& w);
std::wstring FromU8(const std::string& s);

}  // namespace net
