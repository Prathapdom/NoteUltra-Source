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
        auto shown=[](HWND h){return (GetWindowLongPtrW(h,GWL_STYLE)&WS_VISIBLE)!=0;};
        command(DocumentMap);require(showMap&&shown(docMap)&&sci(mapView,SCI_GETDOCPOINTER)==sci(second->editor,SCI_GETDOCPOINTER),"document map shows the active document");
        {std::string lines;for(int i=0;i<2000;++i)lines+="line "+std::to_string(i)+"\n";set(second->editor,lines);sci(second->editor,SCI_SETFIRSTVISIBLELINE,1000);updateMap();
            require(sci(mapView,SCI_GETSELECTIONSTART)==sci(mapView,SCI_POSITIONFROMLINE,1000),"document map marks the visible lines");mapScrollTo(0);require(sci(second->editor,SCI_GETFIRSTVISIBLELINE)<1000,"clicking the document map scrolls the editor");
            set(second->editor,"alpha alpha\nBeta\n");}
        selectDocument(0);require(sci(mapView,SCI_GETDOCPOINTER)==sci(first->editor,SCI_GETDOCPOINTER),"document map follows the active tab");
        command(DocumentList);require(showList&&shown(docList),"document list shown");{RECT r=listRect(1);SendMessageW(docList,WM_LBUTTONDOWN,0,MAKELPARAM(r.left+px(20),(r.top+r.bottom)/2));}require(active==1,"document list click selects document");
        command(DocumentMap);command(DocumentList);require(!showMap&&!shown(docMap)&&!showList&&!shown(docList),"side panels hide");
        SetWindowTextW(control(FindField),L"alpha");require(findNext(),"find first match");require(sci(second->editor,SCI_GETSELECTIONSTART)==0,"first match position");require(findNext(),"find second match");require(sci(second->editor,SCI_GETSELECTIONSTART)==6,"second match position");
        SetWindowTextW(control(ReplaceField),L"omega");beginReplaceAll();pump([]{return !replacement;});require(contents(second->editor)=="omega omega\nBeta\n","replace all");command(Undo);require(contents(second->editor)=="alpha alpha\nBeta\n","replace all is one undo action");
        command(FindAllOpen);require(showResults&&shown(resultsPanel)&&hits.size()==3,"find all in open documents lists one line");require(sci(resultsView,SCI_GETLINECOUNT)>=3,"search results text");
        selectDocument(0);openResult(2);require(active==1&&sci(second->editor,SCI_GETSELECTIONSTART)==0&&sci(second->editor,SCI_GETSELECTIONEND)==5,"opening a result selects the match");command(SearchResults);require(!showResults&&!shown(resultsPanel),"search results hide");
        SendMessageW(control(Regex),BM_SETCHECK,BST_CHECKED,0);SetWindowTextW(control(FindField),L"^Beta$");require(findNext(),"regex search");SendMessageW(control(Regex),BM_SETCHECK,BST_UNCHECKED,0);
        sci(second->editor,SCI_GOTOPOS,0);toggleBookmark();require(sci(second->editor,SCI_MARKERGET,0)&1,"bookmark marker");
        command(EolBase);require(contents(second->editor)=="alpha alpha\r\nBeta\r\n","EOL conversion");command(Undo);require(contents(second->editor)=="alpha alpha\nBeta\n","EOL conversion undo");
        set(second->editor,utf8(L"Hello \u03a9 \u6771\u4eac \U0001f642\r\n"));auto text=contents(second->editor);
        for(int i=0;i<4;++i){auto path=directory+L"\\roundtrip-"+std::to_wstring(i)+L".txt";second->encoding=(Encoding)i;require(saveTo(second,path,stamp(path)),"start async save");require(sci(second->editor,SCI_GETREADONLY),"save pins read-only document");pump([&]{return second->busy==Busy::None;});require(!dirty(second),"successful save clears dirty state");require(second->path==path,"successful save updates path");
            std::atomic<bool> cancel{false};std::atomic<uint64_t> progress{0};std::string loaded;auto metadata=loadFile(path,[&](const char*p,size_t n){loaded.append(p,n);},cancel,progress);require(loaded==text,"UTF8 / UTF16 save roundtrip");require(metadata.encoding==(Encoding)i,"BOM and encoding preservation");}
        auto existing=second->path;auto before=raw(existing);set(second->editor,"changed text");require(saveTo(second,existing,second->original),"atomic replace begins");pump([&]{return second->busy==Busy::None;});require(raw(existing)!=(before)&&raw(existing).size()>0,"atomic replacement succeeds");
        second->encoding=Encoding::UTF8;auto path=directory+L"\\reload.cpp";set(second->editor,"constexpr int count = 7;\n");require(saveTo(second,path,stamp(path)),"save source file");pump([&]{return second->busy==Busy::None;});removeDocument(second);require(!recent.empty()&&recent.front()==path,"closed file joins recent files");command(RestoreClosed);auto opened=current();pump([&]{return opened->busy==Busy::None;});require(contents(opened->editor)=="constexpr int count = 7;\n","background loader roundtrip");require(opened->language==1&&!dirty(opened),"language detection and savepoint");
        auto oldTheme=dark;command(Dark);require(dark!=oldTheme,"theme toggle");command(Wrap);require(sci(opened->editor,SCI_GETWRAPMODE)==SC_WRAP_WORD,"word wrap");command(Wrap);
        auto savedStamp=opened->original;{Handle f(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,0,nullptr));require((bool)f,"external edit fixture");writeAll(f.value,"external changes",16);}set(opened->editor,"local edits");require(saveTo(opened,path,savedStamp),"start conflicting save");pump([&]{return opened->busy==Busy::None;});
        require(raw(path)=="external changes"&&dirty(opened),"external edit conflict preserves file and unsaved edits");command(Undo);require(contents(opened->editor)=="constexpr int count = 7;\n","undo works after background loading");
        {HDC probe=CreateDCW(L"WINSPOOL",L"Microsoft Print to PDF",nullptr,nullptr);if(probe){DeleteDC(probe);auto pdf=directory+L"\\export.pdf";require(exportPdf(pdf),"PDF export");
            pump([&]{try{return raw(pdf).rfind("%PDF",0)==0;}catch(const std::exception&){return false;}});}}
        newDocument();auto lines=current();HWND le=lines->editor;
        set(le,"pear\nApple\npear\n10\n9\n\n   \nbanana\n");sci(le,SCI_GOTOPOS,0);command(RemoveBlank);require(contents(le)=="pear\nApple\npear\n10\n9\nbanana\n","remove blank lines");
        command(RemoveDuplicates);require(contents(le)=="pear\nApple\n10\n9\nbanana\n","remove duplicate lines");command(SortBase);require(contents(le)=="10\n9\nApple\nbanana\npear\n","sort lexicographically");
        command(SortBase+6);require(contents(le)=="Apple\nbanana\npear\n9\n10\n","sort as integers");command(SortBase+7);require(contents(le)=="10\n9\nApple\nbanana\npear\n","sort as integers descending");
        command(ReverseLines);require(contents(le)=="pear\nbanana\nApple\n9\n10\n","reverse lines");command(SortBase+2);require(contents(le)=="10\n9\nApple\nbanana\npear\n","sort ignoring case");
        set(le,"a\na\nb\na\n");sci(le,SCI_GOTOPOS,0);command(RemoveConsecutive);require(contents(le)=="a\nb\na\n","remove consecutive duplicates");command(Undo);require(contents(le)=="a\na\nb\na\n","line operation is one undo action");
        set(le,"c\nb\na\nz\n");sci(le,SCI_SETSEL,0,4);command(SortBase);require(contents(le)=="b\nc\na\nz\n","sort selected lines");
        set(le,"x\ny");sci(le,SCI_GOTOPOS,0);command(InsertBelow);require(contents(le)=="x\n\ny"&&sci(le,SCI_GETCURRENTPOS)==2,"insert blank line below");
        set(le,"1,5\n1.000,25\n0,75\n");sci(le,SCI_GOTOPOS,0);command(SortBase+8);require(contents(le)=="0,75\n1,5\n1.000,25\n","sort decimals with comma");
        set(le,"a\nb\n");sci(le,SCI_GOTOPOS,0);command(JoinLines);require(contents(le)=="a b\n","join lines");
        int markdown=0;for(size_t i=0;i<languages().size();++i)if(strcmp(languages()[i].lexer,"markdown")==0)markdown=(int)i;
        command(LanguageBase+markdown);set(le,"# Title\n\nSome **bold** text and `code` at https://example.com and <https://example.org>.\n\n| A | B |\n|---|---|\n| 1 | 2 |\n");command(MarkdownPreview);renderPreview();
        {auto preview=getText(previewView);require(showPreview&&shown(previewPanel)&&preview.find(L"Title")!=preview.npos&&preview.find(L"bold")!=preview.npos&&preview.find(L"**")==preview.npos,"markdown preview renders");}
        command(MarkdownPreview);require(!shown(previewPanel),"markdown preview hides");sci(le,SCI_SETSAVEPOINT);removeDocument(lines);selectDocument((int)(std::find(documents.begin(),documents.end(),opened)-documents.begin()));
        std::wstring largePath=directory+L"\\large.txt";{Handle f(CreateFileW(largePath.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr));require((bool)f,"create large file");std::string block;for(int i=0;i<16384;++i)block+="background text line\n";for(int i=0;i<64;++i)writeAll(f.value,block.data(),block.size());}
        auto start=GetTickCount64();openPath(largePath);auto large=current();pump([&]{return large->busy==Busy::None;});require(large->large&&sci(large->editor,SCI_GETLENGTH)>16*1024*1024,"large-file mode");
        report="PASS: real Scintilla controls, tabs, syntax styling, split view, document map, document list, undo/redo, find, find all, regex, grouped replace-all, line operations and sorts, recent files and restore closed, Markdown preview, PDF export, bookmarks, EOL conversion, UTF8/UTF16 roundtrips, async loader/save, atomic replacement, external-change protection, themes, wrap, large-file mode.\r\n";
        report+="Large-file background load: "+std::to_string(sci(large->editor,SCI_GETLENGTH))+" bytes in "+std::to_string(GetTickCount64()-start)+" ms (synthetic warm-cache fixture).\r\n";
    }catch(const std::exception&e){report="FAIL: "+std::string(e.what())+"\r\n";code=1;}
    if(trace)fclose(trace);FILE*f=_wfopen((directory+L"\\ui-test-results.txt").c_str(),L"wb");if(f){fwrite(report.data(),1,report.size(),f);fclose(f);}else code=1;
    for(auto& j:jobs)j->cancel=true;while(!jobs.empty()){tick();Sleep(2);}if(replacement)finishReplace();closingAll=false;DestroyWindow(window);return code;
}
