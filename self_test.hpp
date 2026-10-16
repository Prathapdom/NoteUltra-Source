#pragma once
// Included by main.cpp so tests exercise the same commands, real editor controls,
// asynchronous loader, and save pipeline as the application.
int selfTests(const std::wstring& directory){
    FILE* trace=_wfopen((directory+L"\\test-trace.txt").c_str(),L"wb");
    auto require=[&](bool value,const char* message){if(trace){fprintf(trace,"%s: %s\n",value?"OK":"FAIL",message);fflush(trace);}if(!value)throw std::runtime_error(message);};
    auto pump=[&](const std::function<bool()>& complete,DWORD timeout=15000){auto start=GetTickCount64();while(!complete()&&GetTickCount64()-start<timeout){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}}tick();Sleep(2);}require(complete(),"Timed out waiting for background operation");};
    auto contents=[](HWND h){size_t length=(size_t)sci(h,SCI_GETLENGTH);std::string s(length+1,'\0');sci(h,SCI_GETTEXT,s.size(),(LPARAM)&s[0]);s.resize(length);return s;};
    auto set=[](HWND h,const std::string& s){sci(h,SCI_SETTEXT,0,(LPARAM)s.c_str());};
    auto raw=[](const std::wstring& path){Handle f(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));if(!f)throw std::runtime_error("Read test output");auto size=stampHandle(f.value).size;std::string s((size_t)size,' ');DWORD n=0;if(size&&!ReadFile(f.value,&s[0],(DWORD)size,&n,nullptr))throw std::runtime_error("Read test output");return s;};
    std::string report;int code=0;
    try{
        require(current()!=nullptr,"initial tab exists");auto first=current();set(first->editor,"int main() {\n    return 42;\n}\n");require(dirty(first),"editing marks document dirty");command(Undo);require(contents(first->editor).empty(),"undo");command(Redo);require(contents(first->editor).find("return")!=std::string::npos,"redo");
        command(LanguageBase+1);sci(first->editor,SCI_COLOURISE,0,-1);require(sci(first->editor,SCI_GETSTYLEAT,0)==SCE_C_WORD,"C++ syntax lexer styles keywords");
        command(Split);require(split&&sci(secondary,SCI_GETDOCPOINTER)==sci(first->editor,SCI_GETDOCPOINTER),"split panes share document");command(Split);
        newDocument();auto second=current();set(second->editor,"alpha alpha\nBeta\n");selectDocument(0);require(contents(first->editor).find("42")!=std::string::npos,"tab preserves contents");selectDocument(1);
        SetWindowTextW(control(FindField),L"alpha");require(findNext(),"find first match");require(sci(second->editor,SCI_GETSELECTIONSTART)==0,"first match position");require(findNext(),"find second match");require(sci(second->editor,SCI_GETSELECTIONSTART)==6,"second match position");
        SetWindowTextW(control(ReplaceField),L"omega");beginReplaceAll();pump([]{return !replacement;});require(contents(second->editor)=="omega omega\nBeta\n","replace all");command(Undo);require(contents(second->editor)=="alpha alpha\nBeta\n","replace all is one undo action");
        SendMessageW(control(Regex),BM_SETCHECK,BST_CHECKED,0);SetWindowTextW(control(FindField),L"^Beta$");require(findNext(),"regex search");SendMessageW(control(Regex),BM_SETCHECK,BST_UNCHECKED,0);
        sci(second->editor,SCI_GOTOPOS,0);toggleBookmark();require(sci(second->editor,SCI_MARKERGET,0)&1,"bookmark marker");
        command(EolBase);require(contents(second->editor)=="alpha alpha\r\nBeta\r\n","EOL conversion");command(Undo);require(contents(second->editor)=="alpha alpha\nBeta\n","EOL conversion undo");
        set(second->editor,utf8(L"Hello \u03a9 \u6771\u4eac \U0001f642\r\n"));auto text=contents(second->editor);
        for(int i=0;i<4;++i){auto path=directory+L"\\roundtrip-"+std::to_wstring(i)+L".txt";second->encoding=(Encoding)i;require(saveTo(second,path,stamp(path)),"start async save");require(sci(second->editor,SCI_GETREADONLY),"save pins read-only document");pump([&]{return second->busy==Busy::None;});require(!dirty(second),"successful save clears dirty state");require(second->path==path,"successful save updates path");
            std::atomic<bool> cancel{false};std::atomic<uint64_t> progress{0};std::string loaded;auto metadata=loadFile(path,[&](const char*p,size_t n){loaded.append(p,n);},cancel,progress);require(loaded==text,"UTF8 / UTF16 save roundtrip");require(metadata.encoding==(Encoding)i,"BOM and encoding preservation");}
        auto existing=second->path;auto before=raw(existing);set(second->editor,"changed text");require(saveTo(second,existing,second->original),"atomic replace begins");pump([&]{return second->busy==Busy::None;});require(raw(existing)!=(before)&&raw(existing).size()>0,"atomic replacement succeeds");
        second->encoding=Encoding::UTF8;auto path=directory+L"\\reload.cpp";set(second->editor,"constexpr int count = 7;\n");require(saveTo(second,path,stamp(path)),"save source file");pump([&]{return second->busy==Busy::None;});removeDocument(second);openPath(path);auto opened=current();pump([&]{return opened->busy==Busy::None;});require(contents(opened->editor)=="constexpr int count = 7;\n","background loader roundtrip");require(opened->language==1&&!dirty(opened),"language detection and savepoint");
        auto oldTheme=dark;command(Dark);require(dark!=oldTheme,"theme toggle");command(Wrap);require(sci(opened->editor,SCI_GETWRAPMODE)==SC_WRAP_WORD,"word wrap");command(Wrap);
        auto savedStamp=opened->original;{Handle f(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,0,nullptr));require((bool)f,"external edit fixture");writeAll(f.value,"external changes",16);}set(opened->editor,"local edits");require(saveTo(opened,path,savedStamp),"start conflicting save");pump([&]{return opened->busy==Busy::None;});
        require(raw(path)=="external changes"&&dirty(opened),"external edit conflict preserves file and unsaved edits");command(Undo);require(contents(opened->editor)=="constexpr int count = 7;\n","undo works after background loading");
        std::wstring largePath=directory+L"\\large.txt";{Handle f(CreateFileW(largePath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));require((bool)f,"create large file");std::string block;for(int i=0;i<16384;++i)block+="background text line\n";for(int i=0;i<64;++i)writeAll(f.value,block.data(),block.size());}
        auto start=GetTickCount64();openPath(largePath);auto large=current();pump([&]{return large->busy==Busy::None;});require(large->large&&sci(large->editor,SCI_GETLENGTH)>16*1024*1024,"large-file mode");
        report="PASS: real Scintilla controls, tabs, syntax styling, split view, undo/redo, find, regex, grouped replace-all, bookmarks, EOL conversion, UTF8/UTF16 roundtrips, async loader/save, atomic replacement, external-change protection, themes, wrap, large-file mode.\r\n";
        report+="Large-file background load: "+std::to_string(sci(large->editor,SCI_GETLENGTH))+" bytes in "+std::to_string(GetTickCount64()-start)+" ms (synthetic warm-cache fixture).\r\n";
    }catch(const std::exception&e){report="FAIL: "+std::string(e.what())+"\r\n";code=1;}
    if(trace)fclose(trace);FILE*f=_wfopen((directory+L"\\ui-test-results.txt").c_str(),L"wb");if(f){fwrite(report.data(),1,report.size(),f);fclose(f);}else code=1;
    for(auto& j:jobs)j->cancel=true;while(!jobs.empty()){tick();Sleep(2);}if(replacement)finishReplace();closingAll=false;DestroyWindow(window);return code;
}
