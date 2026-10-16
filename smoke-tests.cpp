#include "platform.hpp"
#include "vendor/scintilla/include/Scintilla.h"
#include <commctrl.h>
#include <iostream>
DWORD target=0;HWND app=nullptr,view=nullptr;
BOOL CALLBACK mainWindow(HWND h,LPARAM){DWORD pid=0;GetWindowThreadProcessId(h,&pid);wchar_t cls[100]{};GetClassNameW(h,cls,100);if(pid==target&&wcscmp(cls,L"NoteUltra.Main")==0){app=h;return FALSE;}return TRUE;}
BOOL CALLBACK editorWindow(HWND h,LPARAM){wchar_t cls[100]{};GetClassNameW(h,cls,100);if(wcscmp(cls,L"Scintilla")==0&&(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)){view=h;return FALSE;}return TRUE;}
void require(bool ok,const char* s){if(!ok)throw std::runtime_error(s);}
void command(int id){SendMessageW(app,WM_COMMAND,id,0);}
LRESULT call(UINT msg,WPARAM w=0,LPARAM l=0){return SendMessageW(view,msg,w,l);}
bool waitFor(const std::function<bool()>& done){auto start=GetTickCount64();while(GetTickCount64()-start<10000){if(done())return true;Sleep(20);}return false;}
int largeTest(const wchar_t* executable,const wchar_t* file){PROCESS_INFORMATION pi{};try{
    nu::Handle input(CreateFileW(file,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));require((bool)input,"open large test fixture");LARGE_INTEGER size{};require(GetFileSizeEx(input.value,&size),"large fixture size");
    std::wstring args=L"\""+std::wstring(executable)+L"\" \""+file+L"\"";STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;auto began=GetTickCount64();
    require(CreateProcessW(executable,&args[0],nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&pi),"launch release app with large file");CloseHandle(pi.hThread);target=pi.dwProcessId;
    require(waitFor([]{EnumWindows(mainWindow,0);return app!=nullptr;}),"large file window appears");
    require(waitFor([&]{view=nullptr;EnumChildWindows(app,editorWindow,0);return view&&call(SCI_GETLENGTH)==size.QuadPart&&call(SCI_GETREADONLY)==0;}),"large document background load");
    auto elapsed=GetTickCount64()-began;DWORD_PTR response=0;require(SendMessageTimeoutW(app,WM_NULL,0,0,SMTO_ABORTIFHUNG,500,&response)!=0,"large-file UI responds");require(call(SCI_GETUNDOCOLLECTION)==1,"large-file undo enabled");
    PostMessageW(app,WM_CLOSE,0,0);require(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0,"large-file clean shutdown");DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);require(code==0,"large-file release exit code");CloseHandle(pi.hProcess);pi.hProcess=nullptr;
    std::cout<<"PASS: release large-file load, "<<size.QuadPart<<" bytes in "<<elapsed<<" ms including startup; UI responds, undo enabled, clean shutdown. Synthetic warm-cache fixture.\n";return 0;
}catch(const std::exception&e){if(pi.hProcess){TerminateProcess(pi.hProcess,1);CloseHandle(pi.hProcess);}std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
int wmain(int argc,wchar_t**argv){if(argc==3)return largeTest(argv[1],argv[2]);PROCESS_INFORMATION pi{};std::wstring fixture;try{require(argc==2,"provide release executable path");wchar_t temp[32768];GetTempPathW(32768,temp);fixture=std::wstring(temp)+L"NoteUltra-smoke-"+std::to_wstring(GetCurrentProcessId())+L".cpp";
    {nu::Handle f(CreateFileW(fixture.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));require((bool)f,"create smoke fixture");const char*s="int value = 42;\n";DWORD n=0;require(WriteFile(f.value,s,16,&n,nullptr)&&n==16,"write smoke fixture");}
    std::wstring arguments=L"\""+std::wstring(argv[1])+L"\" \""+fixture+L"\"";STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;require(CreateProcessW(argv[1],&arguments[0],nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&pi),"launch release app");CloseHandle(pi.hThread);target=pi.dwProcessId;
    require(waitFor([]{EnumWindows(mainWindow,0);return app!=nullptr;}),"release window appears");
    require(waitFor([]{view=nullptr;EnumChildWindows(app,editorWindow,0);return view&&call(SCI_GETLENGTH)==16&&call(SCI_GETREADONLY)==0;}),"command-line file loads asynchronously");
    require(call(SCI_GETUNDOCOLLECTION)==1,"loaded documents record undo");
    command(1201);SendMessageW(GetDlgItem(app,5101),WM_SETTEXT,0,(LPARAM)L"value");command(5103);require(call(SCI_GETSELECTIONSTART)==4&&call(SCI_GETSELECTIONEND)==9,"release find selects text");command(5110);
    call(SCI_GOTOPOS,0);SendMessageW(view,WM_CHAR,'x',0);require(call(SCI_GETMODIFY)!=0,"editing marks loaded document dirty");call(SCI_UNDO);require(call(SCI_GETLENGTH)==16&&call(SCI_GETMODIFY)==0,"undo returns loaded savepoint");call(SCI_REDO);require(call(SCI_GETMODIFY)!=0,"redo restores edit");
    command(1003);require(waitFor([]{return call(SCI_GETMODIFY)==0&&call(SCI_GETREADONLY)==0;}),"release async save succeeds");
    {nu::Handle f(CreateFileW(fixture.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));require((bool)f,"read saved fixture");char bytes[32]{};DWORD n=0;require(ReadFile(f.value,bytes,32,&n,nullptr)&&std::string(bytes,n)=="xint value = 42;\n","release save wrote expected bytes");}
    command(1305);int visible=0;EnumChildWindows(app,[](HWND h,LPARAM p)->BOOL{wchar_t cls[100]{};GetClassNameW(h,cls,100);if(wcscmp(cls,L"Scintilla")==0&&(GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE))++*(int*)p;return TRUE;},(LPARAM)&visible);require(visible==2,"split view shows both editors");command(1305);
    command(1001);require(SendMessageW(GetDlgItem(app,5001),TCM_GETITEMCOUNT,0,0)==2,"new tab");command(1006);require(SendMessageW(GetDlgItem(app,5001),TCM_GETITEMCOUNT,0,0)==1,"close clean tab");
    DWORD_PTR response=0;require(SendMessageTimeoutW(app,WM_NULL,0,0,SMTO_ABORTIFHUNG,500,&response)!=0,"UI responds");PostMessageW(app,WM_CLOSE,0,0);require(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0,"clean shutdown");DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);require(code==0,"release process exit code");CloseHandle(pi.hProcess);pi.hProcess=nullptr;DeleteFileW(fixture.c_str());
    std::cout<<"PASS: release executable launch, command-line async load, undo tracking, find, edit/undo/redo, async save with exact bytes, split view, tabs, responsiveness, clean shutdown\n";return 0;
}catch(const std::exception&e){if(pi.hProcess){TerminateProcess(pi.hProcess,1);CloseHandle(pi.hProcess);}std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
