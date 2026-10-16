#pragma once
#include "platform.hpp"
#include <cstring>
namespace nu {
constexpr uint64_t MaxFileBytes=128ull*1024*1024;
constexpr size_t ChunkBytes=256*1024;
enum class Encoding {UTF8,UTF8BOM,UTF16LE,UTF16BE,ANSI};
inline const wchar_t* encodingName(Encoding e){switch(e){case Encoding::UTF8:return L"UTF-8";case Encoding::UTF8BOM:return L"UTF-8 BOM";case Encoding::UTF16LE:return L"UTF-16 LE";case Encoding::UTF16BE:return L"UTF-16 BE";default:return L"Windows ANSI";}}
struct Stamp {uint64_t size=0;FILETIME time{};bool exists=false;};
inline Stamp stampHandle(HANDLE file){BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))throw std::runtime_error("Cannot read file metadata");return {(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,info.ftLastWriteTime,true};}
inline Stamp stamp(const std::wstring& path){Handle f(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));if(!f){DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return {};throw std::runtime_error("Cannot check destination file");}return stampHandle(f.value);}
inline bool sameStamp(const Stamp&a,const Stamp&b){return a.exists==b.exists && (!a.exists || (a.size==b.size && CompareFileTime(&a.time,&b.time)==0));}
class Utf8Check {
    unsigned need=0;uint32_t code=0,minimum=0;
public:
    bool add(const char* p,size_t n){for(size_t i=0;i<n;++i){unsigned c=(unsigned char)p[i];if(!need){if(c<0x80)continue;if(c>=0xc2&&c<=0xdf){need=1;code=c&31;minimum=0x80;}else if(c>=0xe0&&c<=0xef){need=2;code=c&15;minimum=0x800;}else if(c>=0xf0&&c<=0xf4){need=3;code=c&7;minimum=0x10000;}else return false;}else{if((c&0xc0)!=0x80)return false;code=(code<<6)|(c&63);if(--need==0 && (code<minimum||code>0x10ffff||(code>=0xd800&&code<=0xdfff)))return false;}}return true;}
    bool complete()const{return need==0;}
};
struct Loaded {Encoding encoding=Encoding::UTF8;int eol=0;Stamp original;uint64_t utf8Bytes=0;bool large=false;};
struct EolDetect {bool found=false,cr=false;int eol=0;void add(const std::string& s){if(found)return;for(char c:s){if(cr){eol=c=='\n'?0:1;found=true;return;}if(c=='\r')cr=true;else if(c=='\n'){eol=2;found=true;return;}}}int finish()const{return found?eol:(cr?1:0);}};
inline Loaded loadFile(const std::wstring& path,const std::function<void(const char*,size_t)>& append,std::atomic<bool>& cancel,std::atomic<uint64_t>& progress){
    Handle f(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));if(!f)throw std::runtime_error("Cannot open file: "+utf8(errorMessage()));
    Loaded result;result.original=stampHandle(f.value);result.large=result.original.size>16*1024*1024;
    if(result.original.size>MaxFileBytes)throw std::runtime_error("This version supports files up to 128 MiB");
    std::vector<char> buffer(ChunkBytes);DWORD count=0;size_t bom=0;bool first=true,valid=true,binary=false;Utf8Check validator;
    while(true){if(cancel)throw std::runtime_error("Cancelled");if(!ReadFile(f.value,buffer.data(),(DWORD)buffer.size(),&count,nullptr))throw std::runtime_error("File read failed");if(!count)break;
        size_t begin=0;if(first){first=false;if(count>=3 && memcmp(buffer.data(),"\xef\xbb\xbf",3)==0){result.encoding=Encoding::UTF8BOM;bom=3;}else if(count>=2&&(unsigned char)buffer[0]==0xff&&(unsigned char)buffer[1]==0xfe){result.encoding=Encoding::UTF16LE;bom=2;}else if(count>=2&&(unsigned char)buffer[0]==0xfe&&(unsigned char)buffer[1]==0xff){result.encoding=Encoding::UTF16BE;bom=2;}begin=bom;}
        if(result.encoding==Encoding::UTF16LE||result.encoding==Encoding::UTF16BE)break;
        if(memchr(buffer.data()+begin,0,count-begin))binary=true;
        if(valid)valid=validator.add(buffer.data()+begin,count-begin);
    }
    if(cancel)throw std::runtime_error("Cancelled");
    if(binary)throw std::runtime_error("This appears to be a binary file, not a text document");
    if(result.encoding==Encoding::UTF8 || result.encoding==Encoding::UTF8BOM){valid=valid&&validator.complete();if(!valid){if(bom)throw std::runtime_error("Invalid UTF-8 in a BOM-marked file");if(GetACP()==CP_UTF8)throw std::runtime_error("Invalid UTF-8; the Windows ANSI codepage is also UTF-8 on this machine");result.encoding=Encoding::ANSI;}}
    LARGE_INTEGER start{};start.QuadPart=bom;if(!SetFilePointerEx(f.value,start,nullptr,FILE_BEGIN))throw std::runtime_error("Cannot seek file");
    std::string pending;EolDetect eols;progress=0;
    while(true){if(cancel)throw std::runtime_error("Cancelled");if(!ReadFile(f.value,buffer.data(),(DWORD)buffer.size(),&count,nullptr))throw std::runtime_error("File read failed");bool end=count==0;if(end&&pending.empty())break;
        std::string raw=std::move(pending);raw.append(buffer.data(),count);pending.clear();std::string decoded;
        if(result.encoding==Encoding::UTF16LE||result.encoding==Encoding::UTF16BE){
            size_t use=raw.size()&~size_t(1);if(end&&use!=raw.size())throw std::runtime_error("Truncated UTF-16 file");
            auto unit=[&](size_t p){unsigned a=(unsigned char)raw[p],b=(unsigned char)raw[p+1];return (wchar_t)(result.encoding==Encoding::UTF16LE?(a|b<<8):(b|a<<8));};
            if(!end&&use>=2){auto last=unit(use-2);if(last>=0xd800&&last<=0xdbff)use-=2;}
            pending=raw.substr(use);std::wstring wide;wide.reserve(use/2);for(size_t i=0;i<use;i+=2)wide+=unit(i);decoded=utf8(wide);
        }else if(result.encoding==Encoding::ANSI){
            size_t use=raw.size();if(GetACP()!=CP_UTF8&&!end){for(size_t i=0;i<raw.size();){if(IsDBCSLeadByteEx(CP_ACP,(BYTE)raw[i])){if(i+1==raw.size()){use=i;break;}i+=2;}else ++i;}}
            pending=raw.substr(use);decoded=utf8(widen(raw.substr(0,use),CP_ACP,0));
        }else decoded=std::move(raw);
        if(end&&!pending.empty())throw std::runtime_error("Truncated text encoding");
        if(!decoded.empty()){append(decoded.data(),decoded.size());eols.add(decoded);result.utf8Bytes+=decoded.size();if(result.utf8Bytes>MaxFileBytes*3)throw std::runtime_error("Decoded document is too large");}
        progress+=count;if(end)break;
    }
    result.eol=eols.finish();return result;
}
inline void writeAll(HANDLE f,const char* data,size_t size){while(size){DWORD done=0;if(!WriteFile(f,data,(DWORD)std::min(size,size_t(1024*1024)),&done,nullptr)||!done)throw std::runtime_error("File write failed");data+=done;size-=done;}}
inline void encodeTo(HANDLE file,const char* data,size_t size,Encoding encoding,std::atomic<uint64_t>& progress){
    if(encoding==Encoding::UTF8BOM)writeAll(file,"\xef\xbb\xbf",3);else if(encoding==Encoding::UTF16LE)writeAll(file,"\xff\xfe",2);else if(encoding==Encoding::UTF16BE)writeAll(file,"\xfe\xff",2);
    size_t offset=0;
    while(offset<size){size_t n=std::min(ChunkBytes,size-offset);if(offset+n<size)while(n&&((unsigned char)data[offset+n]&0xc0)==0x80)--n;
        if(!n)throw std::runtime_error("Invalid UTF-8 boundary");
        if(encoding==Encoding::UTF8||encoding==Encoding::UTF8BOM)writeAll(file,data+offset,n);
        else {auto wide=widen(std::string(data+offset,n));if(encoding==Encoding::UTF16LE)writeAll(file,(const char*)wide.data(),wide.size()*2);
            else if(encoding==Encoding::UTF16BE){std::string out;out.reserve(wide.size()*2);for(wchar_t c:wide){out+=(char)(c>>8);out+=(char)c;}writeAll(file,out.data(),out.size());}
            else{BOOL lost=FALSE;DWORD flags=GetACP()==CP_UTF8?WC_ERR_INVALID_CHARS:WC_NO_BEST_FIT_CHARS;BOOL* loss=GetACP()==CP_UTF8?nullptr:&lost;int length=WideCharToMultiByte(CP_ACP,flags,wide.data(),(int)wide.size(),nullptr,0,nullptr,loss);if(!length||lost)throw std::runtime_error("Some characters cannot be saved as ANSI. Choose UTF-8.");std::string out(length,' ');WideCharToMultiByte(CP_ACP,flags,wide.data(),(int)wide.size(),&out[0],length,nullptr,loss);if(lost)throw std::runtime_error("ANSI conversion would lose characters");writeAll(file,out.data(),out.size());}}
        offset+=n;progress=offset;
    }
}
inline Stamp atomicSave(const std::wstring& path,const char* data,size_t size,Encoding encoding,const Stamp& expected,std::atomic<uint64_t>& progress){
    if(!sameStamp(stamp(path),expected))throw std::runtime_error("The file changed outside NoteUltra. Use Save As to preserve both versions.");
    static std::atomic<unsigned> counter{0};std::wstring temporary=path+L".noteultra-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++counter)+L".tmp";
    bool created=false;
    try{{Handle out(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(!out)throw std::runtime_error("Cannot create temporary save file: "+utf8(errorMessage()));created=true;encodeTo(out.value,data,size,encoding,progress);if(!FlushFileBuffers(out.value))throw std::runtime_error("Could not flush saved data");}
        if(!sameStamp(stamp(path),expected))throw std::runtime_error("Destination changed during save. Use Save As.");
        BOOL ok=expected.exists?ReplaceFileW(path.c_str(),temporary.c_str(),nullptr,0,nullptr,nullptr):MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH);
        if(!ok)throw std::runtime_error("Could not commit saved file: "+utf8(errorMessage()));
        return stamp(path);
    }catch(...){if(created)DeleteFileW(temporary.c_str());throw;}
}
}
