#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace nu {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle&operator=(const Handle&)=delete;
    explicit operator bool() const{return value!=INVALID_HANDLE_VALUE&&value;}
};
inline std::wstring widen(const std::string& s,UINT cp=CP_UTF8,DWORD flags=MB_ERR_INVALID_CHARS){
    if(s.empty())return {};int n=MultiByteToWideChar(cp,flags,s.data(),(int)s.size(),nullptr,0);if(!n)throw std::runtime_error("Invalid text encoding");
    std::wstring out(n,L' ');if(!MultiByteToWideChar(cp,flags,s.data(),(int)s.size(),&out[0],n))throw std::runtime_error("Text decoding failed");return out;
}
inline std::string utf8(const std::wstring& s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);if(!n)throw std::runtime_error("Invalid Unicode text");std::string out(n,' ');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),&out[0],n,nullptr,nullptr);return out;}
inline std::wstring errorMessage(DWORD code=GetLastError()){
    wchar_t* message=nullptr;FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,(LPWSTR)&message,0,nullptr);
    std::wstring s=message?message:L"Windows error "+std::to_wstring(code);if(message)LocalFree(message);return s;
}
inline std::wstring basename(const std::wstring& path){auto p=path.find_last_of(L"\\/");return p==path.npos?path:path.substr(p+1);}
inline std::wstring lower(std::wstring s){if(s.empty())return s;LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,s.data(),(int)s.size(),&s[0],(int)s.size(),nullptr,nullptr,0);return s;}
inline std::wstring absolute(const std::wstring& p){DWORD n=GetFullPathNameW(p.c_str(),0,nullptr,nullptr);if(!n)throw std::runtime_error("Invalid file path");std::wstring s(n,L' ');DWORD used=GetFullPathNameW(p.c_str(),n,&s[0],nullptr);if(!used||used>=n)throw std::runtime_error("Invalid file path");s.resize(used);return s;}
}
