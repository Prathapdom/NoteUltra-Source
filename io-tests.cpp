#include "file_io.hpp"
#include <iostream>
using namespace nu;
void require(bool condition,const char* text){if(!condition)throw std::runtime_error(text);}
std::string readRaw(const std::wstring&p){Handle f(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));require((bool)f,"read fixture");std::string out((size_t)stampHandle(f.value).size,' ');DWORD n=0;if(!out.empty())require(ReadFile(f.value,&out[0],(DWORD)out.size(),&n,nullptr)&&n==out.size(),"read fixture contents");return out;}
void putRaw(const std::wstring&p,const std::string&s){Handle f(CreateFileW(p.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr));require((bool)f,"create fixture");writeAll(f.value,s.data(),s.size());}
int main(){wchar_t temporary[32768];GetTempPathW(32768,temporary);std::wstring root=std::wstring(temporary)+L"noteultra-io-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());CreateDirectoryW(root.c_str(),nullptr);
    std::vector<std::wstring> files;try{std::atomic<uint64_t> progress{0};std::atomic<bool> cancel{false};
        std::string text(ChunkBytes-2,'x');text+=utf8(L"\U0001f642\u03a9\u6771\u4eac\r\n");text+=std::string(ChunkBytes,'y')+"\nlast line";
        for(int i=0;i<4;++i){auto path=root+L"\\unicode-"+std::to_wstring(i)+L".txt";files.push_back(path);atomicSave(path,text.data(),text.size(),(Encoding)i,{},progress);std::string loaded;auto meta=loadFile(path,[&](const char*p,size_t n){loaded.append(p,n);},cancel,progress);require(loaded==text,"chunk-boundary Unicode roundtrip");require(meta.encoding==(Encoding)i,"encoding detection");require(meta.eol==0,"CRLF detection");
            auto old=meta.original;std::string next="edited\n";auto now=atomicSave(path,next.data(),next.size(),(Encoding)i,old,progress);require(now.exists,"existing atomic replacement");std::string again;loadFile(path,[&](const char*p,size_t n){again.append(p,n);},cancel,progress);require(again==next,"existing file content");
            auto before=readRaw(path);bool rejected=false;try{atomicSave(path,"oops",4,Encoding::UTF8,old,progress);}catch(...){rejected=true;}require(rejected&&readRaw(path)==before,"stale metadata prevents overwrite");}
        auto bad=root+L"\\bad.txt";files.push_back(bad);putRaw(bad,"\xff\xfe\x00"); // C literal excludes trailing NUL, resulting in a bare BOM.
        putRaw(bad,std::string("\xff\xfe\x00",3));bool rejected=false;try{loadFile(bad,[](const char*,size_t){},cancel,progress);}catch(...){rejected=true;}require(rejected,"odd UTF16 rejected");
        putRaw(bad,std::string("\xef\xbb\xbf\xc0\xaf",5));rejected=false;try{loadFile(bad,[](const char*,size_t){},cancel,progress);}catch(...){rejected=true;}require(rejected,"overlong UTF8 rejected");
        putRaw(bad,std::string("text\0binary",11));rejected=false;try{loadFile(bad,[](const char*,size_t){},cancel,progress);}catch(...){rejected=true;}require(rejected,"binary text rejected");
        cancel=true;rejected=false;try{loadFile(bad,[](const char*,size_t){},cancel,progress);}catch(...){rejected=true;}require(rejected,"load cancellation");cancel=false;
        putRaw(bad,"");std::string empty;auto meta=loadFile(bad,[&](const char*p,size_t n){empty.append(p,n);},cancel,progress);require(empty.empty()&&meta.original.exists,"empty file");
        std::string unicode=utf8(L"\u6771\u4eac");if(GetACP()!=CP_UTF8&&GetACP()!=932&&GetACP()!=936&&GetACP()!=949&&GetACP()!=950){auto before=readRaw(bad);rejected=false;try{atomicSave(bad,unicode.data(),unicode.size(),Encoding::ANSI,stamp(bad),progress);}catch(...){rejected=true;}require(rejected&&readRaw(bad)==before,"lossy ANSI save preserves original");}
        require(Utf8Check{}.add("ASCII",5),"UTF8 ASCII validation");for(auto&p:files)DeleteFileW(p.c_str());RemoveDirectoryW(root.c_str());
        std::cout<<"PASS: streaming UTF8/UTF16 LE/BE roundtrips, Unicode at read/write chunk boundaries, CRLF detection, atomic new/existing saves, external-edit conflict, malformed input, binary rejection, cancellation, empty files, ANSI data-loss protection\n";return 0;
    }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
