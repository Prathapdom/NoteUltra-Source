#include "file_io.hpp"
#include "languages.hpp"
#include "vendor/scintilla/include/ILoader.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <sstream>
#include <chrono>
#include <cstdio>

using namespace nu;
enum Command {
 New=1001,Open,Save,SaveAs,SaveAll,Close,CloseAll,Exit,CopyPath,RevealPath,
 Undo=1101,Redo,Cut,Copy,Paste,SelectAll,Duplicate,DeleteLine,Upper,Lower,Indent,Unindent,
 Find=1201,Replace,FindNext,FindPrevious,GoTo,Bookmark,NextBookmark,Complete,
 Wrap=1301,Whitespace,LineNumbers,Dark,Split,ZoomIn,ZoomOut,ZoomReset,FoldAll,ExpandAll,ShowToolbar,ShowStatus,ThemeSystem,ThemeLight,ThemeDark,
 AutoLanguage=2999,LanguageBase=3000,EncodingBase=4000,EolBase=4100,About=4200,
 Tabs=5001,Status,Toolbar,
 FindField=5101,ReplaceField,NextButton,PreviousButton,ReplaceButton,ReplaceAllButton,MatchCase,WholeWord,Regex,HideFind,GoToText,GoToButton,CancelWork
};
enum class Busy {None,Loading,Saving,Replacing};
enum class Theme {System,Light,Dark};
struct Document {
    HWND editor=nullptr;std::wstring path,title;Encoding encoding=Encoding::UTF8;Stamp original;int language=0;
    bool automatic=true,metaDirty=false,large=false,closeAfter=false;Busy busy=Busy::None;unsigned number=0;
};
struct Job {
    std::shared_ptr<Document> doc;std::function<void()> work;std::atomic<bool> done{false},cancel{false};std::atomic<uint64_t> progress{0};
    bool load=false;std::wstring path;std::string error;Loaded loaded;Stamp saved;void* loadedDocument=nullptr;
};
struct ReplaceTask {std::shared_ptr<Document> doc;std::string query,replacement;int flags=0;Sci_Position pos=0;size_t count=0;bool cancel=false;};
HWND window=nullptr,tabControl=nullptr,toolBar=nullptr,statusBar=nullptr,secondary=nullptr,tooltip=nullptr;
HINSTANCE instance=nullptr;HMODULE scintillaDLL=nullptr,lexillaDLL=nullptr;Lexilla::CreateLexerFn createLexer=nullptr;
std::vector<std::shared_ptr<Document>> documents;std::vector<std::unique_ptr<Job>> jobs;std::unique_ptr<ReplaceTask> replacement;
std::thread ioThread;std::mutex ioMutex;std::condition_variable ioReady;std::deque<Job*> ioQueue;bool ioShutdown=false;
int active=-1;unsigned nextNumber=0;bool dark=false,wrap=false,whitespace=false,lineNumbers=true,split=false,findVisible=false,showToolbar=true,showStatus=true;
bool closingAll=false,exitAfterClose=false,testMode=false,refreshNeeded=true;int fontSize=11,dpi=96;Theme theme=Theme::System;Palette pal=makePalette(false);
HFONT uiFont=nullptr,statusFont=nullptr,iconFont=nullptr,smallIcons=nullptr,tinyIcons=nullptr;HBRUSH fieldBrush=nullptr,focusBrush=nullptr;int fieldTextHeight=18;
std::wstring notice=L"Ready",settingsPath;
Sci_Position lastZero=-1;std::string lastQuery;
void startIO(){ioThread=std::thread([]{while(true){Job* job=nullptr;{std::unique_lock<std::mutex> lock(ioMutex);ioReady.wait(lock,[]{return ioShutdown||!ioQueue.empty();});if(ioShutdown&&ioQueue.empty())break;job=ioQueue.front();ioQueue.pop_front();}auto work=std::move(job->work);work();work=nullptr;job->done=true;}});}
void submit(std::unique_ptr<Job> job){Job* ptr=job.get();jobs.push_back(std::move(job));{std::lock_guard<std::mutex> lock(ioMutex);ioQueue.push_back(ptr);}ioReady.notify_one();}
void stopIO(){{std::lock_guard<std::mutex> lock(ioMutex);ioShutdown=true;}ioReady.notify_one();if(ioThread.joinable())ioThread.join();}
LRESULT CALLBACK windowProc(HWND,UINT,WPARAM,LPARAM);
void layout();void refresh();void applyAppearance();void closeAllStep();void requestClose(std::shared_ptr<Document>);void selectDocument(int);void newDocument();void tick();void command(int);void layoutTabs(bool reveal=true);
int px(int n){return MulDiv(n,dpi,96);}
HWND control(int id){return GetDlgItem(window,id);}
std::wstring getText(HWND h){int n=GetWindowTextLengthW(h);std::wstring s(n+1,L'\0');GetWindowTextW(h,&s[0],n+1);s.resize(n);return s;}
std::shared_ptr<Document> current(){return active>=0&&active<(int)documents.size()?documents[active]:nullptr;}
HWND editor(){auto d=current();return !d?nullptr:(split&&GetFocus()==secondary?secondary:d->editor);}
bool isEditor(HWND h){if(h&&h==secondary)return true;for(auto& d:documents)if(d->editor==h)return true;return false;}
void tell(std::wstring s){notice=std::move(s);refreshNeeded=true;}
void alert(const std::wstring& s){MessageBoxW(window,s.c_str(),L"NoteUltra",MB_OK|MB_ICONWARNING);}
bool dirty(const std::shared_ptr<Document>& d){return d->metaDirty||sci(d->editor,SCI_GETMODIFY);}
std::wstring tabTitle(const std::shared_ptr<Document>&d){auto name=d->path.empty()?d->title:basename(d->path);if(d->busy==Busy::Loading)name+=L" (opening)";if(d->busy==Busy::Saving)name+=L" (saving)";return name;}
HWND add(const wchar_t* cls,const wchar_t* title,DWORD style,int id,DWORD ex=0){HWND c=CreateWindowExW(ex,cls,title,WS_VISIBLE|WS_CHILD|style,0,0,10,10,window,(HMENU)(INT_PTR)id,instance,nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)uiFont,TRUE);return c;}
void place(int id,int x,int y,int w,int h){MoveWindow(control(id),x,y,std::max(w,1),std::max(h,1),TRUE);}
void checkMenu(int id,bool check){CheckMenuItem(GetMenu(window),id,MF_BYCOMMAND|(check?MF_CHECKED:MF_UNCHECKED));}
bool checked(int id){return SendMessageW(control(id),BM_GETCHECK,0,0)==BST_CHECKED;}
void configure(HWND h){
    sci(h,SCI_SETTECHNOLOGY,SC_TECHNOLOGY_DIRECTWRITE);sci(h,SCI_SETBUFFEREDDRAW,0);
    sci(h,SCI_SETCODEPAGE,SC_CP_UTF8);sci(h,SCI_SETLAYOUTCACHE,SC_CACHE_PAGE);sci(h,SCI_SETIDLESTYLING,SC_IDLESTYLING_ALL);
    sci(h,SCI_SETTABWIDTH,4);sci(h,SCI_SETINDENT,4);sci(h,SCI_SETUSETABS,0);sci(h,SCI_SETBACKSPACEUNINDENTS,1);sci(h,SCI_SETTABINDENTS,1);
    sci(h,SCI_SETMULTIPLESELECTION,1);sci(h,SCI_SETADDITIONALSELECTIONTYPING,1);sci(h,SCI_SETMULTIPASTE,SC_MULTIPASTE_EACH);sci(h,SCI_SETSCROLLWIDTHTRACKING,1);sci(h,SCI_SETSCROLLWIDTH,1);
    sci(h,SCI_SETMOUSEDWELLTIME,500);sci(h,SCI_SETAUTOMATICFOLD,SC_AUTOMATICFOLD_SHOW|SC_AUTOMATICFOLD_CLICK|SC_AUTOMATICFOLD_CHANGE);
    sci(h,SCI_AUTOCSETIGNORECASE,1);sci(h,SCI_AUTOCSETAUTOHIDE,1);sci(h,SCI_AUTOCSETORDER,SC_ORDER_PERFORMSORT);
    paintEditor(h,pal,lineNumbers,whitespace,wrap,fontSize,dpi);
}
// Scintilla uses standard window scrollbars, which the dark Explorer theme recolours.
HWND createEditor(){HWND h=CreateWindowExW(0,L"Scintilla",L"",WS_CHILD|WS_TABSTOP|WS_CLIPCHILDREN,0,0,100,100,window,nullptr,instance,nullptr);if(!h)throw std::runtime_error("Cannot create editor control");themeControl(h,dark);configure(h);return h;}
void syncSplit(){auto d=current();if(!d||!secondary)return;sci(secondary,SCI_SETDOCPOINTER,0,sci(d->editor,SCI_GETDOCPOINTER));sci(secondary,SCI_SETREADONLY,d->busy!=Busy::None);configure(secondary);colorLanguage(secondary,d->large?0:d->language,createLexer,dark,false);ShowWindow(secondary,split?SW_SHOW:SW_HIDE);}
void tabsChanged(){layoutTabs();refreshNeeded=true;}
void selectDocument(int index){if(index<0||index>=(int)documents.size())return;auto old=current();if(old&&old!=documents[index])ShowWindow(old->editor,SW_HIDE);active=index;ShowWindow(documents[index]->editor,SW_SHOW);syncSplit();layout();tabsChanged();SetFocus(documents[index]->editor);lastZero=-1;}
std::shared_ptr<Document> makeDocument(const std::wstring& path=L""){
    auto d=std::make_shared<Document>();d->number=++nextNumber;d->title=L"Untitled "+std::to_wstring(d->number);d->path=path;d->editor=createEditor();d->language=detectLanguage(path);colorLanguage(d->editor,d->language,createLexer,dark);
    documents.push_back(d);selectDocument((int)documents.size()-1);return d;
}
void newDocument(){makeDocument();tell(L"New document");}
void removeDocument(std::shared_ptr<Document> d){auto it=std::find(documents.begin(),documents.end(),d);if(it==documents.end())return;int index=(int)(it-documents.begin());bool wasActive=active==index;
    documents.erase(it);DestroyWindow(d->editor);d->editor=nullptr;
    if(documents.empty()){active=-1;tabsChanged();if(!closingAll)newDocument();}
    else{int next=wasActive?std::min(index,(int)documents.size()-1):(active>index?active-1:active);active=-1;selectDocument(next);}
    refreshNeeded=true;
}
bool blankUntitled(std::shared_ptr<Document> d){return d&&d->path.empty()&&d->busy==Busy::None&&!dirty(d)&&sci(d->editor,SCI_GETLENGTH)==0;}
void openPath(const std::wstring& input){
    std::wstring path;try{path=absolute(input);}catch(const std::exception&e){tell(widen(e.what()));return;}
    for(size_t i=0;i<documents.size();++i)if(!documents[i]->path.empty()&&lower(documents[i]->path)==lower(path)){selectDocument((int)i);return;}
    auto old=current();bool discardBlank=blankUntitled(old);auto d=makeDocument(path);d->busy=Busy::Loading;sci(d->editor,SCI_SETREADONLY,1);
    auto job=std::make_unique<Job>();job->doc=d;job->path=path;job->load=true;
    auto loader=(Scintilla::ILoader*)sci(d->editor,SCI_CREATELOADER,0,0);
    if(!loader){d->busy=Busy::None;d->path.clear();tell(L"Not enough memory to load file");return;}
    Job* j=job.get();j->work=[j,loader]{
        try{j->loaded=loadFile(j->path,[&](const char* data,size_t size){if(loader->AddData(data,(Sci_Position)size)!=SC_STATUS_OK)throw std::runtime_error("Not enough memory to load document");},j->cancel,j->progress);
            if(j->cancel)throw std::runtime_error("Cancelled");j->loadedDocument=loader->ConvertToDocument();if(!j->loadedDocument)throw std::runtime_error("Could not create loaded document");
        }catch(const std::exception&e){j->error=e.what();loader->Release();}
    };submit(std::move(job));if(discardBlank)removeDocument(old);tell(L"Opening "+basename(path)+L"…");
}
void openDialog(){wchar_t paths[65536]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrTitle=L"Open documents";dialog.lpstrFilter=L"Text and code files\0*.txt;*.log;*.md;*.json;*.xml;*.csv;*.cpp;*.h;*.js;*.ts;*.py;*.sql\0All files\0*.*\0\0";dialog.lpstrFile=paths;dialog.nMaxFile=65536;dialog.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetOpenFileNameW(&dialog))return;std::wstring first=paths;wchar_t* p=paths+first.size()+1;if(!*p)openPath(first);else while(*p){std::wstring name=p;openPath(first+L"\\"+name);p+=name.size()+1;}
}
bool saveTo(std::shared_ptr<Document> d,const std::wstring& path,const Stamp& expected){
    if(!d||d->busy!=Busy::None)return false;
    for(auto other:documents)if(other!=d&&!other->path.empty()&&lower(other->path)==lower(path)){alert(L"This file is already open in another tab. Save to a different name.");return false;}
    d->busy=Busy::Saving;sci(d->editor,SCI_SETREADONLY,1);if(current()==d&&split)sci(secondary,SCI_SETREADONLY,1);
    Sci_Position length=sci(d->editor,SCI_GETLENGTH);const char* data=(const char*)sci(d->editor,SCI_GETCHARACTERPOINTER);
    auto job=std::make_unique<Job>();job->doc=d;job->path=path;Job* j=job.get();Encoding encoding=d->encoding;
    j->work=[j,data,length,encoding,expected]{try{j->saved=atomicSave(j->path,data,(size_t)length,encoding,expected,j->progress);}catch(const std::exception&e){j->error=e.what();}};submit(std::move(job));tell(L"Saving "+basename(path)+L"…");return true;
}
bool saveDocument(std::shared_ptr<Document> d,bool saveAs=false){if(!d||d->busy!=Busy::None)return false;
    std::wstring destination=d->path;Stamp expected=d->original;
    if(saveAs||destination.empty()){wchar_t path[32768]{};auto initial=destination.empty()?L"Untitled.txt":destination;lstrcpynW(path,initial.c_str(),32768);OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrTitle=L"Save document as";dialog.lpstrFilter=L"All files\0*.*\0Text files\0*.txt\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"txt";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetSaveFileNameW(&dialog))return false;destination=absolute(path);try{expected=stamp(destination);}catch(const std::exception&e){alert(widen(e.what()));return false;}}
    return saveTo(d,destination,expected);
}
// Save / Don't save / Cancel as a task dialog, the standard Windows prompt for unsaved changes.
int askSave(const std::wstring& name){std::wstring text=L"Do you want to save changes to "+name+L"?";
    using Show=HRESULT(WINAPI*)(const TASKDIALOGCONFIG*,int*,int*,BOOL*);static auto show=(Show)GetProcAddress(GetModuleHandleW(L"comctl32.dll"),"TaskDialogIndirect");
    if(show){TASKDIALOG_BUTTON buttons[]={{IDYES,L"&Save"},{IDNO,L"Do&n't save"}};TASKDIALOGCONFIG c{};c.cbSize=sizeof(c);c.hwndParent=window;c.hInstance=instance;c.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;c.dwCommonButtons=TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle=L"NoteUltra";c.pszMainInstruction=text.c_str();c.cButtons=2;c.pButtons=buttons;c.nDefaultButton=IDYES;int result=IDCANCEL;if(SUCCEEDED(show(&c,&result,nullptr,nullptr)))return result;}
    return MessageBoxW(window,text.c_str(),L"NoteUltra",MB_YESNOCANCEL|MB_ICONQUESTION);}
void requestClose(std::shared_ptr<Document> d){if(!d)return;
    if(d->busy==Busy::Loading){for(auto& j:jobs)if(j->doc==d)j->cancel=true;d->closeAfter=true;return;}
    if(d->busy==Busy::Saving){d->closeAfter=true;return;}
    if(d->busy==Busy::Replacing){replacement->cancel=true;tell(L"Stopping replace. Close the tab after it finishes.");closingAll=false;exitAfterClose=false;return;}
    if(dirty(d)){int choice=askSave(d->path.empty()?d->title:basename(d->path));
        if(choice==IDCANCEL){closingAll=false;exitAfterClose=false;return;}if(choice==IDYES){if(saveDocument(d)){d->closeAfter=true;}else{closingAll=false;exitAfterClose=false;}return;}}
    removeDocument(d);
}
void closeAllStep(){if(!closingAll)return;
    while(!documents.empty()){auto d=current();if(!d){selectDocument(0);d=current();}size_t count=documents.size();requestClose(d);if(!closingAll||documents.size()==count)return;}
    bool quit=exitAfterClose;closingAll=false;exitAfterClose=false;if(quit)DestroyWindow(window);else newDocument();
}
void showFind(bool replace=false,bool go=false){findVisible=true;layout();HWND h=editor();if(h&&!go){auto a=sci(h,SCI_GETSELECTIONSTART),b=sci(h,SCI_GETSELECTIONEND);if(b>a&&b-a<2048){std::string s((size_t)(b-a)+1,'\0');sci(h,SCI_GETSELTEXT,0,(LPARAM)&s[0]);s.resize((size_t)(b-a));if(s.find_first_of("\r\n")==s.npos)SetWindowTextW(control(FindField),widen(s).c_str());}}
    SetFocus(control(go?GoToText:(replace?ReplaceField:FindField)));SendMessageW(GetFocus(),EM_SETSEL,0,-1);
}
int findFlags(){return (checked(MatchCase)?SCFIND_MATCHCASE:0)|(checked(WholeWord)?SCFIND_WHOLEWORD:0)|(checked(Regex)?SCFIND_REGEXP|SCFIND_POSIX:0);}
bool canSearch(){auto d=current();if(!d||d->busy!=Busy::None){tell(L"Wait for the current operation to finish.");return false;}if(checked(Regex)&&sci(d->editor,SCI_GETLENGTH)>16*1024*1024){tell(L"Regex search is limited to documents up to 16 MiB. Literal search remains available.");return false;}return true;}
bool findNext(bool backwards=false){if(!canSearch())return false;auto query=utf8(getText(control(FindField)));if(query.empty()){showFind();return false;}
    HWND h=editor();Sci_Position length=sci(h,SCI_GETLENGTH),start=sci(h,backwards?SCI_GETSELECTIONSTART:SCI_GETSELECTIONEND);
    if(lastZero==start&&lastQuery==query){if(backwards)start=sci(h,SCI_POSITIONBEFORE,start);else if(start<length)start=sci(h,SCI_POSITIONAFTER,start);else start=0;}
    sci(h,SCI_SETSTATUS,SC_STATUS_OK);sci(h,SCI_SETSEARCHFLAGS,findFlags());sci(h,SCI_SETTARGETSTART,start);sci(h,SCI_SETTARGETEND,backwards?0:length);
    auto found=sci(h,SCI_SEARCHINTARGET,query.size(),(LPARAM)query.data());bool wrapped=false;
    if(found<0&&sci(h,SCI_GETSTATUS)==SC_STATUS_OK){sci(h,SCI_SETTARGETSTART,backwards?length:0);sci(h,SCI_SETTARGETEND,start);found=sci(h,SCI_SEARCHINTARGET,query.size(),(LPARAM)query.data());wrapped=found>=0;}
    if(sci(h,SCI_GETSTATUS)!=SC_STATUS_OK){tell(L"Invalid or unsupported regular expression.");return false;}
    if(found<0){tell(L"No matching text found.");return false;}
    auto end=sci(h,SCI_GETTARGETEND);sci(h,SCI_SETSEL,found,end);sci(h,SCI_SCROLLCARET);sci(h,SCI_ENSUREVISIBLE,sci(h,SCI_LINEFROMPOSITION,found));lastZero=found==end?found:-1;lastQuery=query;tell(wrapped?L"Match found — wrapped around the document.":L"Match found.");return true;
}
void replaceOne(){if(!canSearch())return;HWND h=editor();auto query=utf8(getText(control(FindField))),value=utf8(getText(control(ReplaceField)));if(query.empty()){showFind(true);return;}
    auto a=sci(h,SCI_GETSELECTIONSTART),b=sci(h,SCI_GETSELECTIONEND);sci(h,SCI_SETSEARCHFLAGS,findFlags());sci(h,SCI_SETTARGETSTART,a);sci(h,SCI_SETTARGETEND,b);sci(h,SCI_SETSTATUS,0);
    auto found=sci(h,SCI_SEARCHINTARGET,query.size(),(LPARAM)query.data());
    if(found==a&&sci(h,SCI_GETTARGETEND)==b&&(a!=b||lastZero==a)){
        auto n=sci(h,checked(Regex)?SCI_REPLACETARGETRE:SCI_REPLACETARGET,value.size(),(LPARAM)value.data());if(n>=0)sci(h,SCI_SETSEL,a,a+n);lastZero=-1;
    }findNext();
}
void beginReplaceAll(){if(!canSearch()||replacement)return;auto query=utf8(getText(control(FindField)));if(query.empty()){showFind(true);return;}
    auto d=current();replacement=std::make_unique<ReplaceTask>();replacement->doc=d;replacement->query=std::move(query);replacement->replacement=utf8(getText(control(ReplaceField)));replacement->flags=findFlags();
    d->busy=Busy::Replacing;sci(d->editor,SCI_BEGINUNDOACTION);sci(d->editor,SCI_SETREADONLY,1);if(current()==d&&split)sci(secondary,SCI_SETREADONLY,1);tell(L"Replacing… Escape stops; Undo restores the operation.");
}
void finishReplace(const std::wstring& extra=L""){if(!replacement)return;auto d=replacement->doc;auto count=replacement->count;sci(d->editor,SCI_ENDUNDOACTION);sci(d->editor,SCI_SETREADONLY,0);d->busy=Busy::None;if(current()==d&&split)sci(secondary,SCI_SETREADONLY,0);replacement.reset();tell(std::to_wstring(count)+L" replacement(s). "+extra);}
void replaceTick(){if(!replacement)return;auto& task=*replacement;if(task.cancel){finishReplace(L"Stopped; use Undo to restore all changes.");return;}HWND h=task.doc->editor;auto start=GetTickCount64();sci(h,SCI_SETREADONLY,0);sci(h,SCI_SETSEARCHFLAGS,task.flags);
    for(int i=0;i<100&&GetTickCount64()-start<12;++i){auto length=sci(h,SCI_GETLENGTH);if(task.pos>length){finishReplace();return;}
        sci(h,SCI_SETSTATUS,0);sci(h,SCI_SETTARGETSTART,task.pos);sci(h,SCI_SETTARGETEND,length);auto found=sci(h,SCI_SEARCHINTARGET,task.query.size(),(LPARAM)task.query.data());
        if(sci(h,SCI_GETSTATUS)!=SC_STATUS_OK){finishReplace(L"Invalid or unsupported regular expression.");return;}if(found<0){finishReplace();return;}
        auto end=sci(h,SCI_GETTARGETEND);auto count=sci(h,(task.flags&SCFIND_REGEXP)?SCI_REPLACETARGETRE:SCI_REPLACETARGET,task.replacement.size(),(LPARAM)task.replacement.data());
        if(count<0){finishReplace(L"Replacement failed.");return;}++task.count;task.pos=found+count;
        if(found==end){if(task.pos>=sci(h,SCI_GETLENGTH)){finishReplace();return;}task.pos=sci(h,SCI_POSITIONAFTER,task.pos);}
        if(task.count>=1000000){finishReplace(L"One-million replacement limit reached.");return;}
    }
    sci(h,SCI_SETREADONLY,1);tell(L"Replacing… "+std::to_wstring(task.count)+L" done. Escape stops.");
}
void goToLine(){auto d=current();if(!d||d->busy==Busy::Loading)return;auto input=getText(control(GoToText));wchar_t* end=nullptr;unsigned long long value=wcstoull(input.c_str(),&end,10);auto count=sci(editor(),SCI_GETLINECOUNT);
    if(input.empty()||*end||value<1||value>(unsigned long long)count){tell(L"Enter a line number between 1 and "+std::to_wstring(count)+L".");return;}sci(editor(),SCI_GOTOLINE,(WPARAM)(value-1));sci(editor(),SCI_SCROLLCARET);SetFocus(editor());refreshNeeded=true;
}
void toggleBookmark(){HWND h=editor();if(!h)return;auto line=sci(h,SCI_LINEFROMPOSITION,sci(h,SCI_GETCURRENTPOS));if(sci(h,SCI_MARKERGET,line)&1)sci(h,SCI_MARKERDELETE,line,0);else sci(h,SCI_MARKERADD,line,0);}
void nextBookmark(){HWND h=editor();if(!h)return;auto line=sci(h,SCI_LINEFROMPOSITION,sci(h,SCI_GETCURRENTPOS));auto next=sci(h,SCI_MARKERNEXT,line+1,1);if(next<0)next=sci(h,SCI_MARKERNEXT,0,1);if(next>=0)sci(h,SCI_GOTOLINE,next);else tell(L"No bookmarks. Use Ctrl+F2 to add one.");}
void completeWord(){auto d=current();HWND h=editor();if(!d||!h||d->busy!=Busy::None)return;auto pos=sci(h,SCI_GETCURRENTPOS),start=sci(h,SCI_WORDSTARTPOSITION,pos,1);auto words=languages()[d->language].keywords;if(*words)sci(h,SCI_AUTOCSHOW,pos-start,(LPARAM)words);else tell(L"Keyword completion is available for code languages.");}
void cancelWork(){if(replacement){replacement->cancel=true;return;}auto d=current();if(d&&d->busy==Busy::Loading){for(auto& j:jobs)if(j->doc==d)j->cancel=true;tell(L"Cancelling open…");}else{findVisible=false;layout();if(editor())SetFocus(editor());}}
void completeJobs(){
    for(size_t i=0;i<jobs.size();){auto& job=jobs[i];if(!job->done){++i;continue;}auto d=job->doc;bool close=d->closeAfter;
        d->busy=Busy::None;sci(d->editor,SCI_SETREADONLY,0);
        if(job->load){
            if(job->loadedDocument){sci(d->editor,SCI_SETDOCPOINTER,0,(LPARAM)job->loadedDocument);sci(d->editor,SCI_RELEASEDOCUMENT,0,(LPARAM)job->loadedDocument);job->loadedDocument=nullptr;
                configure(d->editor);sci(d->editor,SCI_SETUNDOCOLLECTION,1);sci(d->editor,SCI_EMPTYUNDOBUFFER);sci(d->editor,SCI_SETSAVEPOINT);d->encoding=job->loaded.encoding;d->original=job->loaded.original;d->large=job->loaded.large;sci(d->editor,SCI_SETEOLMODE,job->loaded.eol);
                colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);if(current()==d)syncSplit();tell(d->large?L"Opened in large-file mode; syntax highlighting is disabled above 16 MiB.":L"Opened "+basename(d->path));
            }else if(job->cancel){tell(L"Open cancelled.");close=true;}
            else{d->path.clear();d->language=0;colorLanguage(d->editor,0,createLexer,dark);tell(L"Open failed: "+widen(job->error));closingAll=false;exitAfterClose=false;close=false;d->closeAfter=false;}
        }else if(job->error.empty()){
            d->path=job->path;d->original=job->saved;d->metaDirty=false;sci(d->editor,SCI_SETSAVEPOINT);if(d->automatic){d->language=detectLanguage(d->path);colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);}if(current()==d)syncSplit();tell(L"Saved "+basename(d->path));
        }else{tell(L"Save failed: "+widen(job->error));close=false;d->closeAfter=false;closingAll=false;exitAfterClose=false;if(current()==d&&split)sci(secondary,SCI_SETREADONLY,0);}
        jobs.erase(jobs.begin()+i);if(close)removeDocument(d);
    }
    if(closingAll)closeAllStep();
}
void tick(){replaceTick();completeJobs();if(!jobs.empty())refreshNeeded=true;if(refreshNeeded&&IsWindow(window))refresh();}

// ---- Window chrome ----------------------------------------------------------------
// The tab strip, command bar, find bar and status bar are painted here so every part
// follows the light/dark palette; the classic common controls cannot be fully recoloured.
void trackLeave(HWND h){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);}
POINT mousePoint(LPARAM lp){return {(short)LOWORD(lp),(short)HIWORD(lp)};}
void addTip(HWND owner,UINT_PTR id,const RECT& rc,const wchar_t* text){TOOLINFOW t{};t.cbSize=sizeof(t);t.uFlags=TTF_SUBCLASS;t.hwnd=owner;t.uId=id;t.rect=rc;t.lpszText=(LPWSTR)text;SendMessageW(tooltip,TTM_ADDTOOLW,0,(LPARAM)&t);}
void dropTip(HWND owner,UINT_PTR id){TOOLINFOW t{};t.cbSize=sizeof(t);t.hwnd=owner;t.uId=id;SendMessageW(tooltip,TTM_DELTOOLW,0,(LPARAM)&t);}
void hwndTip(HWND control,const wchar_t* text){TOOLINFOW t{};t.cbSize=sizeof(t);t.uFlags=TTF_IDISHWND|TTF_SUBCLASS;t.hwnd=window;t.uId=(UINT_PTR)control;t.lpszText=(LPWSTR)text;SendMessageW(tooltip,TTM_ADDTOOLW,0,(LPARAM)&t);}
HMENU encodingMenu(){HMENU m=CreatePopupMenu();for(int i=0;i<5;++i)AppendMenuW(m,MF_STRING,EncodingBase+i,encodingName((Encoding)i));return m;}
HMENU eolMenu(){HMENU m=CreatePopupMenu();AppendMenuW(m,MF_STRING,EolBase,L"Windows (CRLF)");AppendMenuW(m,MF_STRING,EolBase+2,L"Unix (LF)");AppendMenuW(m,MF_STRING,EolBase+1,L"Macintosh (CR)");return m;}
HMENU languageMenu(){HMENU m=CreatePopupMenu();AppendMenuW(m,MF_STRING,AutoLanguage,L"Detect from file name");AppendMenuW(m,MF_SEPARATOR,0,nullptr);for(size_t i=0;i<languages().size();++i)AppendMenuW(m,MF_STRING,LanguageBase+(int)i,languages()[i].title);return m;}
void markMenu(HMENU m){auto d=current();HWND h=editor();if(!d||!h||!m)return;
    CheckMenuRadioItem(m,EncodingBase,EncodingBase+4,EncodingBase+(int)d->encoding,MF_BYCOMMAND);CheckMenuRadioItem(m,EolBase,EolBase+2,EolBase+(int)sci(h,SCI_GETEOLMODE),MF_BYCOMMAND);
    CheckMenuRadioItem(m,LanguageBase,LanguageBase+(int)languages().size()-1,LanguageBase+d->language,MF_BYCOMMAND);CheckMenuItem(m,AutoLanguage,MF_BYCOMMAND|(d->automatic?MF_CHECKED:MF_UNCHECKED));}
// Shows a popup menu anchored to a rectangle of a chrome window and runs the chosen command.
void popup(HMENU m,HWND owner,const RECT& anchor,bool above){markMenu(m);POINT pt{anchor.left,above?anchor.top:anchor.bottom};ClientToScreen(owner,&pt);
    int id=TrackPopupMenu(m,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN|(above?TPM_BOTTOMALIGN:TPM_TOPALIGN),pt.x,pt.y,0,window,nullptr);DestroyMenu(m);if(id)command(id);}

// Tab strip: Windows 11 style tabs with a rounded active tab that flows into the content.
std::vector<RECT> tabRects;RECT plusRect{};int tabHot=-1,closeHot=-1,closeDown=-1,tabScroll=0,tabTips=0;bool plusHot=false,plusDown=false;std::wstring tabSignature;
int tabsLeft(){return px(8);}
int tabsRight(){RECT rc;GetClientRect(tabControl,&rc);return std::max(tabsLeft()+px(100),(int)rc.right-px(48));}
RECT closeBox(const RECT& tab){int s=px(24),cy=(tab.top+tab.bottom)/2;return {tab.right-px(8)-s,cy-s/2,tab.right-px(8),cy-s/2+s};}
void layoutTabs(bool reveal){if(!tabControl)return;RECT rc;GetClientRect(tabControl,&rc);HDC dc=GetDC(tabControl);std::vector<int> widths;int total=0;
    for(auto& d:documents){int w=std::clamp(textWidth(dc,tabTitle(d),uiFont)+px(56),px(110),px(240));widths.push_back(w);total+=w;}ReleaseDC(tabControl,dc);
    int left=tabsLeft(),room=tabsRight()-left;
    if(total>room){int shrunk=0;for(auto& w:widths){w=std::max(px(110),(int)((long long)w*room/total));shrunk+=w;}total=shrunk;}
    if(reveal&&active>=0&&active<(int)widths.size()){int start=0;for(int i=0;i<active;++i)start+=widths[i];if(start<tabScroll)tabScroll=start;if(start+widths[active]>tabScroll+room)tabScroll=start+widths[active]-room;}
    tabScroll=std::clamp(tabScroll,0,std::max(0,total-room));
    tabRects.clear();int x=left-tabScroll,top=px(6);for(int w:widths){tabRects.push_back({x,top,x+w,rc.bottom});x+=w;}
    int s=px(32),cy=(top+rc.bottom)/2,plusX=std::min(x,left+room)+px(4);plusRect={plusX,cy-s/2,plusX+s,cy-s/2+s};
    for(int i=0;i<tabTips;++i)dropTip(tabControl,100+i);dropTip(tabControl,99);
    tabTips=(int)tabRects.size();for(int i=0;i<tabTips;++i){RECT r=tabRects[i];r.left=std::max<LONG>(r.left,left);r.right=std::min<LONG>(r.right,left+room);if(r.right>r.left)addTip(tabControl,100+i,r,LPSTR_TEXTCALLBACKW);}
    addTip(tabControl,99,plusRect,L"New tab (Ctrl+N)");InvalidateRect(tabControl,nullptr,FALSE);}
int hitTab(POINT p){if(p.x<tabsLeft()||p.x>=tabsRight())return -1;for(int i=0;i<(int)tabRects.size();++i)if(PtInRect(&tabRects[i],p))return i;return -1;}
void paintTabs(HDC target){RECT rc;GetClientRect(tabControl,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.chrome);
    int left=tabsLeft(),right=tabsRight();HRGN clip=CreateRectRgn(left-px(8),0,right+px(8),rc.bottom);SelectClipRgn(b.dc,clip);
    {Canvas c(b.dc);float flare=(float)px(6);
        for(int i=0;i<(int)tabRects.size();++i){RECT r=tabRects[i];
            if(i==active){c.round(r,(float)px(8),pal.surface,true,false);c.flare((float)r.left,(float)r.bottom,flare,pal.surface,true);c.flare((float)r.right,(float)r.bottom,flare,pal.surface,false);}
            else if(i==tabHot){RECT h=r;InflateRect(&h,-px(2),0);h.top+=px(2);h.bottom-=px(5);c.round(h,(float)px(5),pal.hover(pal.chrome));}
            if(i==closeHot){RECT box=closeBox(r);c.round(box,(float)px(4),closeDown==i?pal.pressed(i==active?pal.surface:pal.chrome):pal.hover(i==active?pal.surface:pal.hover(pal.chrome)));}
            if(dirty(documents[i])&&closeHot!=i){RECT box=closeBox(r);c.dot((box.left+box.right)/2.0f,(box.top+box.bottom)/2.0f,(float)px(4),i==active?pal.text:pal.textSecondary);}
        }
        if(plusHot)c.round(plusRect,(float)px(5),plusDown?pal.pressed(pal.chrome):pal.hover(pal.chrome));
    }
    for(int i=0;i+1<(int)tabRects.size();++i)if(i!=active&&i+1!=active&&i!=tabHot&&i+1!=tabHot){int cy=(tabRects[i].top+tabRects[i].bottom)/2;fill(b.dc,{tabRects[i].right,cy-px(8),tabRects[i].right+1,cy+px(8)},pal.divider);}
    for(int i=0;i<(int)tabRects.size();++i){RECT r=tabRects[i],box=closeBox(r);auto& d=documents[i];
        drawText(b.dc,tabTitle(d),{r.left+px(12),r.top,box.left-px(4),r.bottom},i==active?pal.text:pal.textSecondary,uiFont);
        if(closeHot==i||((i==active||i==tabHot)&&!dirty(d)))drawGlyph(b.dc,0xE8BB,box,i==active||closeHot==i?pal.text:pal.textSecondary,tinyIcons);}
    SelectClipRgn(b.dc,nullptr);DeleteObject(clip);
    drawGlyph(b.dc,0xE710,plusRect,plusHot?pal.text:pal.textSecondary,smallIcons);}
void tabMenu(int index,POINT pt){selectDocument(index);auto d=current();HMENU m=CreatePopupMenu();
    AppendMenuW(m,MF_STRING,Close,L"Close\tCtrl+W");AppendMenuW(m,MF_STRING,CloseAll,L"Close all");AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(m,MF_STRING|(d&&!d->path.empty()?0:MF_GRAYED),CopyPath,L"Copy full path");AppendMenuW(m,MF_STRING|(d&&!d->path.empty()?0:MF_GRAYED),RevealPath,L"Open containing folder");
    popup(m,tabControl,{pt.x,pt.y,pt.x,pt.y},false);}
LRESULT CALLBACK tabsProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintTabs(dc);EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:layoutTabs();return 0;
case WM_MOUSEMOVE:{POINT p=mousePoint(lp);int tab=hitTab(p);int close=-1;if(tab>=0){RECT box=closeBox(tabRects[tab]);if(PtInRect(&box,p))close=tab;}bool plus=PtInRect(&plusRect,p)!=0;
    if(tab!=tabHot||close!=closeHot||plus!=plusHot){tabHot=tab;closeHot=close;plusHot=plus;InvalidateRect(h,nullptr,FALSE);}trackLeave(h);return 0;}
case WM_MOUSELEAVE:tabHot=closeHot=-1;plusHot=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_LBUTTONDOWN:{POINT p=mousePoint(lp);int tab=hitTab(p);if(tab>=0&&tab==closeHot){closeDown=tab;SetCapture(h);}else if(tab>=0)selectDocument(tab);else if(PtInRect(&plusRect,p)){plusDown=true;SetCapture(h);}InvalidateRect(h,nullptr,FALSE);return 0;}
case WM_LBUTTONUP:{int down=closeDown;bool plus=plusDown;closeDown=-1;plusDown=false;if(GetCapture()==h)ReleaseCapture();InvalidateRect(h,nullptr,FALSE);
    if(down>=0&&down==closeHot&&down<(int)documents.size())requestClose(documents[down]);else if(plus&&plusHot)newDocument();return 0;}
case WM_CAPTURECHANGED:closeDown=-1;plusDown=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_MBUTTONUP:{int tab=hitTab(mousePoint(lp));if(tab>=0)requestClose(documents[tab]);return 0;}
case WM_LBUTTONDBLCLK:if(hitTab(mousePoint(lp))<0&&!PtInRect(&plusRect,mousePoint(lp)))newDocument();return 0;
case WM_RBUTTONUP:{POINT p=mousePoint(lp);int tab=hitTab(p);if(tab>=0)tabMenu(tab,p);return 0;}
case WM_MOUSEWHEEL:tabScroll-=GET_WHEEL_DELTA_WPARAM(wp)*px(80)/WHEEL_DELTA;layoutTabs(false);return 0;
case WM_NOTIFY:{auto info=(NMTTDISPINFOW*)lp;if(info->hdr.code==TTN_GETDISPINFOW){static std::wstring text;int i=(int)info->hdr.idFrom-100;if(i>=0&&i<(int)documents.size()){text=documents[i]->path.empty()?documents[i]->title:documents[i]->path;info->lpszText=&text[0];}}return 0;}
case TCM_GETITEMCOUNT:return (LRESULT)documents.size();
case TCM_GETCURSEL:return active;
}return DefWindowProcW(h,msg,wp,lp);}

// Command bar: icon buttons using the Windows icon font (Segoe Fluent Icons on Windows 11).
struct Tool{int id;wchar_t glyph;const wchar_t* tip;};
const Tool tools[]={{New,0xE8A5,L"New (Ctrl+N)"},{Open,0xE8E5,L"Open (Ctrl+O)"},{Save,0xE74E,L"Save (Ctrl+S)"},{SaveAll,0xE78C,L"Save all"},{0,0,nullptr},
    {Cut,0xE8C6,L"Cut (Ctrl+X)"},{Copy,0xE8C8,L"Copy (Ctrl+C)"},{Paste,0xE77F,L"Paste (Ctrl+V)"},{0,0,nullptr},{Undo,0xE7A7,L"Undo (Ctrl+Z)"},{Redo,0xE7A6,L"Redo (Ctrl+Y)"},{0,0,nullptr},
    {Find,0xE721,L"Find (Ctrl+F)"},{Replace,0xE8AC,L"Replace (Ctrl+H)"},{0,0,nullptr},{Wrap,0xE751,L"Word wrap (Alt+Z)"},{Split,0xE89A,L"Split editor (Ctrl+Alt+S)"},{0,0,nullptr},
    {ZoomIn,0xE8A3,L"Zoom in (Ctrl++)"},{ZoomOut,0xE71F,L"Zoom out (Ctrl+-)"},{Dark,0,L"Switch light / dark theme"}};
constexpr int toolCount=sizeof(tools)/sizeof(tools[0]);
RECT toolRects[toolCount];int toolHot=-1,toolDown=-1;std::wstring toolSignature;bool toolTipsAdded=false;
bool toolEnabled(int id){auto d=current();HWND h=editor();if(!d||!h)return false;bool idle=d->busy==Busy::None;
    switch(id){case Save:return idle;case Undo:return idle&&sci(h,SCI_CANUNDO);case Redo:return idle&&sci(h,SCI_CANREDO);case Cut:return idle&&!sci(h,SCI_GETSELECTIONEMPTY);case Copy:return !sci(h,SCI_GETSELECTIONEMPTY);case Paste:return idle&&sci(h,SCI_CANPASTE);default:return true;}}
bool toolChecked(int id){return (id==Wrap&&wrap)||(id==Split&&split);}
void layoutTools(){if(!toolBar)return;RECT rc;GetClientRect(toolBar,&rc);int x=px(8),size=px(32),y=(rc.bottom-1-size)/2;
    for(int i=0;i<toolCount;++i){if(tools[i].id==Dark)toolRects[i]={rc.right-px(8)-size,y,rc.right-px(8),y+size};else if(!tools[i].id){toolRects[i]={x+px(4),y+px(8),x+px(5),y+size-px(8)};x+=px(9);}else{toolRects[i]={x,y,x+size,y+size};x+=size+px(2);}
        if(tools[i].id){if(!toolTipsAdded)addTip(toolBar,i,toolRects[i],tools[i].tip);else{TOOLINFOW t{};t.cbSize=sizeof(t);t.hwnd=toolBar;t.uId=i;t.rect=toolRects[i];SendMessageW(tooltip,TTM_NEWTOOLRECTW,0,(LPARAM)&t);}}}
    toolTipsAdded=true;InvalidateRect(toolBar,nullptr,FALSE);}
int hitTool(POINT p){for(int i=0;i<toolCount;++i)if(tools[i].id&&PtInRect(&toolRects[i],p))return i;return -1;}
void paintTools(HDC target){RECT rc;GetClientRect(toolBar,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.surface);
    {Canvas c(b.dc);for(int i=0;i<toolCount;++i){int id=tools[i].id;if(!id)continue;bool enabled=toolEnabled(id),on=toolChecked(id);RECT r=toolRects[i];
        if(enabled&&i==toolHot)c.round(r,(float)px(4),i==toolDown?pal.pressed(pal.surface):pal.hover(pal.surface));else if(on)c.round(r,(float)px(4),pal.hover(pal.surface));
        if(on){int cx=(r.left+r.right)/2;c.round({cx-px(7),r.bottom-px(3),cx+px(7),r.bottom},(float)px(1),pal.accent);}}}
    for(int i=0;i<toolCount;++i){int id=tools[i].id;if(!id){fill(b.dc,toolRects[i],pal.divider);continue;}
        wchar_t glyph=id==Dark?(dark?0xE706:0xE708):tools[i].glyph;drawGlyph(b.dc,glyph,toolRects[i],toolEnabled(id)?pal.text:pal.textDisabled,iconFont);}
    fill(b.dc,{0,rc.bottom-1,rc.right,rc.bottom},pal.divider);}
void toolsChanged(){std::wstring s;for(auto& t:tools)if(t.id)s+=toolEnabled(t.id)?(toolChecked(t.id)?L'2':L'1'):L'0';s+=dark?L'd':L'l';if(s!=toolSignature){toolSignature=s;InvalidateRect(toolBar,nullptr,FALSE);}}
LRESULT CALLBACK toolsProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintTools(dc);EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:layoutTools();return 0;
case WM_MOUSEMOVE:{int i=hitTool(mousePoint(lp));if(i!=toolHot){toolHot=i;InvalidateRect(h,nullptr,FALSE);}trackLeave(h);return 0;}
case WM_MOUSELEAVE:toolHot=-1;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:{toolDown=hitTool(mousePoint(lp));if(toolDown>=0)SetCapture(h);InvalidateRect(h,nullptr,FALSE);return 0;}
case WM_LBUTTONUP:{int down=toolDown;toolDown=-1;if(GetCapture()==h)ReleaseCapture();InvalidateRect(h,nullptr,FALSE);if(down>=0&&down==hitTool(mousePoint(lp))&&toolEnabled(tools[down].id))command(tools[down].id);return 0;}
case WM_CAPTURECHANGED:toolDown=-1;InvalidateRect(h,nullptr,FALSE);return 0;
}return DefWindowProcW(h,msg,wp,lp);}

// Status bar: text segments on the right open the matching menu when clicked, as in Notepad.
struct Segment{std::wstring text;int action;RECT rc;};
std::vector<Segment> segments;std::wstring statusMessage,statusSignature;int segmentHot=-1;
void layoutStatus(){if(!statusBar)return;RECT rc;GetClientRect(statusBar,&rc);HDC dc=GetDC(statusBar);int x=rc.right-px(8);
    for(int i=(int)segments.size()-1;i>=0;--i){int w=textWidth(dc,segments[i].text,statusFont)+px(20);segments[i].rc={x-w,px(3),x,rc.bottom-px(3)};x-=w+px(1);}ReleaseDC(statusBar,dc);InvalidateRect(statusBar,nullptr,FALSE);}
int hitSegment(POINT p){for(int i=0;i<(int)segments.size();++i)if(segments[i].action&&PtInRect(&segments[i].rc,p))return i;return -1;}
void paintStatus(HDC target){RECT rc;GetClientRect(statusBar,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.chrome);fill(b.dc,{0,0,rc.right,1},pal.divider);
    if(segmentHot>=0){Canvas c(b.dc);c.round(segments[segmentHot].rc,(float)px(4),pal.hover(pal.chrome));}
    int right=segments.empty()?rc.right:segments.front().rc.left;drawText(b.dc,statusMessage,{px(14),0,right-px(12),rc.bottom},pal.textSecondary,statusFont);
    for(int i=0;i<(int)segments.size();++i){auto& s=segments[i];drawText(b.dc,s.text,s.rc,i==segmentHot?pal.text:pal.textSecondary,statusFont,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
        if(i>0){int cy=rc.bottom/2;fill(b.dc,{s.rc.left-1,cy-px(7),s.rc.left,cy+px(7)},pal.divider);}}}
LRESULT CALLBACK statusProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintStatus(dc);EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:layoutStatus();return 0;
case WM_MOUSEMOVE:{int i=hitSegment(mousePoint(lp));if(i!=segmentHot){segmentHot=i;InvalidateRect(h,nullptr,FALSE);}trackLeave(h);return 0;}
case WM_MOUSELEAVE:segmentHot=-1;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_LBUTTONUP:{int i=hitSegment(mousePoint(lp));if(i<0)return 0;int action=segments[i].action;RECT anchor=segments[i].rc;
    if(action==AutoLanguage)popup(languageMenu(),h,anchor,true);else if(action==EolBase)popup(eolMenu(),h,anchor,true);else if(action==EncodingBase)popup(encodingMenu(),h,anchor,true);else command(action);return 0;}
}return DefWindowProcW(h,msg,wp,lp);}

// Find bar: borderless edit controls inside painted Fluent text boxes, and owner-drawn buttons.
struct Box{int id;RECT rc;};std::vector<Box> boxes;int findTop=0;RECT splitter{};
bool isToggle(int id){return id==MatchCase||id==WholeWord||id==Regex;}
void invalidateBox(int id){for(auto& b:boxes)if(b.id==id){RECT r=b.rc;InflateRect(&r,2,2);InvalidateRect(window,&r,FALSE);}}
// Tracks hover for owner-drawn buttons and stores the toggle state that BM_GETCHECK reports.
LRESULT CALLBACK buttonProc(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){switch(msg){
case WM_MOUSEMOVE:if(!GetPropW(h,L"NU.hot")){SetPropW(h,L"NU.hot",(HANDLE)1);trackLeave(h);InvalidateRect(h,nullptr,FALSE);}break;
case WM_MOUSELEAVE:RemovePropW(h,L"NU.hot");InvalidateRect(h,nullptr,FALSE);break;
case WM_LBUTTONDBLCLK:return SendMessageW(h,WM_LBUTTONDOWN,wp,lp);
case BM_GETCHECK:return GetPropW(h,L"NU.check")?BST_CHECKED:BST_UNCHECKED;
case BM_SETCHECK:if(wp==BST_CHECKED)SetPropW(h,L"NU.check",(HANDLE)1);else RemovePropW(h,L"NU.check");InvalidateRect(h,nullptr,FALSE);return 0;
case WM_NCDESTROY:RemovePropW(h,L"NU.hot");RemovePropW(h,L"NU.check");RemoveWindowSubclass(h,buttonProc,1);break;
}return DefSubclassProc(h,msg,wp,lp);}
void drawButton(const DRAWITEMSTRUCT* item){HWND h=item->hwndItem;int id=(int)item->CtlID;RECT rc=item->rcItem;Buffer b(item->hDC,rc);
    bool hot=GetPropW(h,L"NU.hot")!=nullptr,down=(item->itemState&ODS_SELECTED)!=0,disabled=(item->itemState&ODS_DISABLED)!=0,focus=(item->itemState&ODS_FOCUS)&&!(item->itemState&ODS_NOFOCUSRECT);
    bool on=isToggle(id)&&GetPropW(h,L"NU.check"),subtle=isToggle(id)||id==NextButton||id==PreviousButton||id==HideFind;float r=(float)px(4);
    fill(b.dc,rc,pal.surface);
    {Canvas c(b.dc);
        if(on)c.round(rc,r,down?blend(pal.accent,pal.surface,0.25):hot?blend(pal.accent,pal.surface,0.12):pal.accent);
        else if(subtle){if(down)c.round(rc,r,pal.pressed(pal.surface));else if(hot)c.round(rc,r,pal.hover(pal.surface));}
        else{c.round(rc,r,down?pal.controlPressed:hot?pal.controlHover:pal.control);c.outline(rc,r,pal.controlBorder);}
        if(focus)c.outline(rc,r,pal.text,(float)std::max(1,px(2)));}
    COLORREF color=disabled?pal.textDisabled:on?pal.onAccent:(down&&!subtle)?pal.textSecondary:pal.text;
    switch(id){
    case PreviousButton:drawGlyph(b.dc,0xE74A,rc,color,smallIcons);break;
    case NextButton:drawGlyph(b.dc,0xE74B,rc,color,smallIcons);break;
    case HideFind:drawGlyph(b.dc,0xE8BB,rc,color,tinyIcons);break;
    case MatchCase:drawText(b.dc,L"Aa",rc,color,uiFont,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);break;
    case Regex:drawText(b.dc,L".*",rc,color,uiFont,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);break;
    case WholeWord:{RECT t=rc;t.bottom-=px(3);drawText(b.dc,L"ab",t,color,uiFont,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);int cx=(rc.left+rc.right)/2,y=(rc.top+rc.bottom)/2+px(7);fill(b.dc,{cx-px(8),y,cx+px(8),y+1},color);fill(b.dc,{cx-px(8),y-px(3),cx-px(7),y+1},color);fill(b.dc,{cx+px(7),y-px(3),cx+px(8),y+1},color);break;}
    default:drawText(b.dc,getText(h),rc,color,uiFont,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);}}
void box(int id,int x,int y,int w,int h){RECT rc{x,y,x+w,y+h};boxes.push_back({id,rc});place(id,x+px(10),y+(h-fieldTextHeight)/2,w-px(20),fieldTextHeight);}
void layoutFind(int top,int width){boxes.clear();findTop=top;
    for(int id:{FindField,ReplaceField,NextButton,PreviousButton,ReplaceButton,ReplaceAllButton,MatchCase,WholeWord,Regex,HideFind,GoToText,GoToButton})ShowWindow(control(id),findVisible?SW_SHOW:SW_HIDE);
    bool busy=replacement||(current()&&current()->busy==Busy::Loading);ShowWindow(control(CancelWork),findVisible&&busy?SW_SHOW:SW_HIDE);
    if(!findVisible)return;int s=px(32),gap=px(4),y1=top+px(10),y2=y1+s+px(6),field=std::clamp(width*2/5,px(220),px(460)),x=px(12);
    box(FindField,x,y1,field,s);x+=field+px(8);for(int id:{MatchCase,WholeWord,Regex}){place(id,x,y1,s,s);x+=s+gap;}x+=px(8);place(PreviousButton,x,y1,s,s);x+=s+gap;place(NextButton,x,y1,s,s);
    int right=width-px(12)-s;place(HideFind,right,y1,s,s);place(CancelWork,right-px(8)-px(72),y1,px(72),s);
    x=px(12);box(ReplaceField,x,y2,field,s);x+=field+px(8);place(ReplaceButton,x,y2,px(88),s);x+=px(88)+px(6);place(ReplaceAllButton,x,y2,px(100),s);
    right=width-px(12);place(GoToButton,right-px(56),y2,px(56),s);box(GoToText,right-px(56)-px(6)-px(132),y2,px(132),s);}
void paintMain(HDC target){RECT rc;GetClientRect(window,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.surface);
    if(split&&current())fill(b.dc,splitter,pal.divider);
    if(findVisible){fill(b.dc,{0,findTop,rc.right,findTop+1},pal.divider);Canvas c(b.dc);float r=(float)px(4);
        for(auto& bx:boxes){bool focused=GetFocus()==control(bx.id);c.round(bx.rc,r,focused?pal.fieldFocus:pal.field);c.outline(bx.rc,r,pal.fieldBorder);c.bottomEdge(bx.rc,r,focused?pal.accent:pal.fieldBottom,focused?px(2):1);}}}

void refresh(){refreshNeeded=false;auto d=current();if(!d)return;
    std::wstring tabs;for(auto& doc:documents)tabs+=tabTitle(doc)+(dirty(doc)?L"*":L"")+L"\n";tabs+=std::to_wstring(active);if(tabs!=tabSignature){tabSignature=tabs;layoutTabs();}
    auto title=(dirty(d)?L"● ":L"")+(d->path.empty()?d->title:d->path)+L" - NoteUltra";SetWindowTextW(window,title.c_str());
    HWND h=editor();auto pos=sci(h,SCI_GETCURRENTPOS),line=sci(h,SCI_LINEFROMPOSITION,pos),column=sci(h,SCI_GETCOLUMN,pos);auto selection=sci(h,SCI_GETSELECTIONEND)-sci(h,SCI_GETSELECTIONSTART);
    auto position=L"Ln "+std::to_wstring(line+1)+L", Col "+std::to_wstring(column+1);if(selection)position+=L" ("+std::to_wstring(selection)+L" selected)";
    auto eol=sci(h,SCI_GETEOLMODE);std::wstring lineEnd=eol==SC_EOL_CRLF?L"Windows (CRLF)":(eol==SC_EOL_LF?L"Unix (LF)":L"Macintosh (CR)");
    auto lang=std::wstring(languages()[d->language].title);if(d->large)lang+=L" · large file";
    auto message=notice;if(d->busy==Busy::Loading||d->busy==Busy::Saving)for(auto& j:jobs)if(j->doc==d){message=(d->busy==Busy::Loading?L"Opening… ":L"Saving… ")+std::to_wstring(j->progress.load()/1024)+L" KiB";break;}
    std::vector<Segment> next={{position,GoTo,{}},{lang,AutoLanguage,{}},{lineEnd,EolBase,{}},{encodingName(d->encoding),EncodingBase,{}}};
    std::wstring status=message;for(auto& s:next)status+=L"\n"+s.text;if(status!=statusSignature){statusSignature=status;statusMessage=message;segments=std::move(next);layoutStatus();}
    toolsChanged();fitLineNumbers(d->editor,lineNumbers,dpi);if(split)fitLineNumbers(secondary,lineNumbers,dpi);
    bool busy=replacement||d->busy==Busy::Loading;if(findVisible&&(IsWindowVisible(control(CancelWork))!=0)!=busy)ShowWindow(control(CancelWork),busy?SW_SHOW:SW_HIDE);
    for(auto pair:{std::pair<int,bool>{Wrap,wrap},{Whitespace,whitespace},{LineNumbers,lineNumbers},{Split,split},{ShowToolbar,showToolbar},{ShowStatus,showStatus}})checkMenu(pair.first,pair.second);
    CheckMenuRadioItem(GetMenu(window),ThemeSystem,ThemeDark,ThemeSystem+(int)theme,MF_BYCOMMAND);markMenu(GetMenu(window));
    EnableMenuItem(GetMenu(window),Save,MF_BYCOMMAND|(d->busy==Busy::None?MF_ENABLED:MF_GRAYED));EnableMenuItem(GetMenu(window),Undo,MF_BYCOMMAND|(d->busy==Busy::None&&sci(h,SCI_CANUNDO)?MF_ENABLED:MF_GRAYED));EnableMenuItem(GetMenu(window),Redo,MF_BYCOMMAND|(d->busy==Busy::None&&sci(h,SCI_CANREDO)?MF_ENABLED:MF_GRAYED));
    EnableWindow(control(ReplaceButton),d->busy==Busy::None);EnableWindow(control(ReplaceAllButton),d->busy==Busy::None);
}
HMENU menu(){HMENU bar=CreateMenu();auto popup=[&](const wchar_t* name,HMENU m=nullptr){if(!m)m=CreatePopupMenu();AppendMenuW(bar,MF_POPUP,(UINT_PTR)m,name);return m;};auto item=[](HMENU m,int id,const wchar_t* name){AppendMenuW(m,MF_STRING,id,name);};auto separator=[](HMENU m){AppendMenuW(m,MF_SEPARATOR,0,nullptr);};
    HMENU f=popup(L"&File");item(f,New,L"&New\tCtrl+N");item(f,Open,L"&Open…\tCtrl+O");separator(f);item(f,Save,L"&Save\tCtrl+S");item(f,SaveAs,L"Save &as…\tCtrl+Shift+S");item(f,SaveAll,L"Save a&ll");separator(f);item(f,Close,L"&Close tab\tCtrl+W");item(f,CloseAll,L"Close all ta&bs");separator(f);item(f,Exit,L"E&xit\tAlt+F4");
    HMENU e=popup(L"&Edit");item(e,Undo,L"&Undo\tCtrl+Z");item(e,Redo,L"&Redo\tCtrl+Y");separator(e);item(e,Cut,L"Cu&t\tCtrl+X");item(e,Copy,L"&Copy\tCtrl+C");item(e,Paste,L"&Paste\tCtrl+V");item(e,SelectAll,L"Select &all\tCtrl+A");separator(e);item(e,Duplicate,L"&Duplicate line / selection\tCtrl+D");item(e,DeleteLine,L"Delete &line\tCtrl+Shift+L");item(e,Upper,L"UPPERCASE selection\tCtrl+Shift+U");item(e,Lower,L"lowercase selection\tCtrl+U");item(e,Indent,L"&Indent\tTab");item(e,Unindent,L"U&nindent\tShift+Tab");separator(e);AppendMenuW(e,MF_POPUP,(UINT_PTR)eolMenu(),L"Line &endings");
    HMENU s=popup(L"&Search");item(s,Find,L"&Find…\tCtrl+F");item(s,Replace,L"&Replace…\tCtrl+H");item(s,FindNext,L"Find &next\tF3");item(s,FindPrevious,L"Find &previous\tShift+F3");item(s,GoTo,L"&Go to line…\tCtrl+G");separator(s);item(s,Bookmark,L"Toggle &bookmark\tCtrl+F2");item(s,NextBookmark,L"Next boo&kmark\tF2");item(s,Complete,L"&Complete keyword\tCtrl+Space");
    HMENU v=popup(L"&View");HMENU themes=CreatePopupMenu();item(themes,ThemeSystem,L"Use &system setting");item(themes,ThemeLight,L"&Light");item(themes,ThemeDark,L"&Dark");AppendMenuW(v,MF_POPUP,(UINT_PTR)themes,L"&Theme");
    item(v,ShowToolbar,L"T&oolbar");item(v,ShowStatus,L"Status &bar");separator(v);item(v,Wrap,L"&Word wrap\tAlt+Z");item(v,Whitespace,L"Show w&hitespace");item(v,LineNumbers,L"&Line numbers");item(v,Split,L"S&plit editor\tCtrl+Alt+S");separator(v);item(v,ZoomIn,L"Zoom &in\tCtrl++");item(v,ZoomOut,L"Zoom &out\tCtrl+-");item(v,ZoomReset,L"&Reset zoom\tCtrl+0");separator(v);item(v,FoldAll,L"&Fold all");item(v,ExpandAll,L"&Expand all");
    popup(L"E&ncoding",encodingMenu());popup(L"&Language",languageMenu());
    HMENU help=popup(L"&Help");item(help,About,L"&About NoteUltra");return bar;
}
void layout(){if(!window||!tabControl)return;RECT client;GetClientRect(window,&client);int w=client.right,h=client.bottom,y=0;
    MoveWindow(tabControl,0,y,w,px(40),TRUE);y+=px(40);
    ShowWindow(toolBar,showToolbar?SW_SHOW:SW_HIDE);if(showToolbar){MoveWindow(toolBar,0,y,w,px(40)+1,TRUE);y+=px(40)+1;}
    int statusHeight=showStatus?px(30):0;ShowWindow(statusBar,showStatus?SW_SHOW:SW_HIDE);if(showStatus)MoveWindow(statusBar,0,h-statusHeight,w,statusHeight,TRUE);
    int panel=findVisible?px(90):0,bottom=std::max(y+1,h-statusHeight-panel);
    auto d=current();if(d){if(split){int half=(w-1)/2;MoveWindow(d->editor,0,y,half,bottom-y,TRUE);splitter={half,y,half+1,bottom};MoveWindow(secondary,half+1,y,w-half-1,bottom-y,TRUE);}else MoveWindow(d->editor,0,y,w,bottom-y,TRUE);}
    layoutFind(bottom,w);InvalidateRect(window,nullptr,FALSE);
}
void createFonts(){for(HFONT f:{uiFont,statusFont,iconFont,smallIcons,tinyIcons})if(f)DeleteObject(f);
    static const wchar_t* face=fontExists(L"Segoe UI Variable Text")?L"Segoe UI Variable Text":L"Segoe UI";static const wchar_t* icons=fontExists(L"Segoe Fluent Icons")?L"Segoe Fluent Icons":L"Segoe MDL2 Assets";
    uiFont=makeFont(face,px(14));statusFont=makeFont(face,px(12));iconFont=makeFont(icons,px(16));smallIcons=makeFont(icons,px(12));tinyIcons=makeFont(icons,px(10));
    HDC dc=GetDC(window);HGDIOBJ old=SelectObject(dc,uiFont);TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);SelectObject(dc,old);ReleaseDC(window,dc);fieldTextHeight=tm.tmHeight;
    for(int id:{FindField,ReplaceField,GoToText,NextButton,PreviousButton,ReplaceButton,ReplaceAllButton,MatchCase,WholeWord,Regex,HideFind,GoToButton,CancelWork})if(control(id))SendMessageW(control(id),WM_SETFONT,(WPARAM)uiFont,TRUE);
    if(tooltip)SendMessageW(tooltip,WM_SETFONT,(WPARAM)statusFont,TRUE);}
void applyAppearance(){dark=theme==Theme::Dark||(theme==Theme::System&&systemUsesDark());pal=makePalette(dark);
    setAppDark(dark);themeFrame(window,pal);themeControl(tooltip,dark);
    for(HBRUSH b:{fieldBrush,focusBrush})if(b)DeleteObject(b);fieldBrush=CreateSolidBrush(pal.field);focusBrush=CreateSolidBrush(pal.fieldFocus);
    for(int id:{FindField,ReplaceField,GoToText})themeControl(control(id),dark,L"DarkMode_CFD");
    for(auto d:documents){themeControl(d->editor,dark);paintEditor(d->editor,pal,lineNumbers,whitespace,wrap,fontSize,dpi);colorLanguage(d->editor,d->large?0:d->language,createLexer,dark,false);}
    if(secondary){themeControl(secondary,dark);paintEditor(secondary,pal,lineNumbers,whitespace,wrap,fontSize,dpi);if(current())colorLanguage(secondary,current()->large?0:current()->language,createLexer,dark,false);}
    toolSignature.clear();statusSignature.clear();tabSignature.clear();layoutTools();
    DrawMenuBar(window);RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);refreshNeeded=true;
}
void copyPath(){auto d=current();if(!d||d->path.empty()||!OpenClipboard(window))return;EmptyClipboard();size_t bytes=(d->path.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(memory){memcpy(GlobalLock(memory),d->path.c_str(),bytes);GlobalUnlock(memory);if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}CloseClipboard();tell(L"Path copied.");}
void command(int id){auto d=current();HWND h=editor();
    if(id>=LanguageBase&&id<LanguageBase+(int)languages().size()){if(d&&d->busy==Busy::None){d->language=id-LanguageBase;d->automatic=false;colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);if(split)colorLanguage(secondary,d->large?0:d->language,createLexer,dark,false);refreshNeeded=true;}return;}
    if(id>=EncodingBase&&id<EncodingBase+5){if(d&&d->busy==Busy::None&&d->encoding!=(Encoding)(id-EncodingBase)){d->encoding=(Encoding)(id-EncodingBase);d->metaDirty=true;tell(L"Encoding will be applied when you save.");}return;}
    if(id>=EolBase&&id<EolBase+3){if(d&&d->busy==Busy::None){sci(h,SCI_BEGINUNDOACTION);sci(h,SCI_CONVERTEOLS,id-EolBase);sci(h,SCI_SETEOLMODE,id-EolBase);sci(h,SCI_ENDUNDOACTION);tell(L"Line endings converted.");}return;}
    switch(id){
    case New:newDocument();break;case Open:openDialog();break;case Save:saveDocument(d);break;case SaveAs:saveDocument(d,true);break;
    case SaveAll:{auto copy=documents;for(auto doc:copy)if(doc->busy==Busy::None&&dirty(doc))saveDocument(doc);break;}
    case Close:requestClose(d);break;case CloseAll:closingAll=true;exitAfterClose=false;closeAllStep();break;case Exit:SendMessageW(window,WM_CLOSE,0,0);break;
    case CopyPath:copyPath();break;case RevealPath:if(d&&!d->path.empty())ShellExecuteW(window,nullptr,L"explorer.exe",(L"/select,\""+d->path+L"\"").c_str(),nullptr,SW_SHOWNORMAL);break;
    case Find:showFind();break;case Replace:showFind(true);break;case FindNext:case NextButton:findNext();break;case FindPrevious:case PreviousButton:findNext(true);break;case ReplaceButton:replaceOne();break;case ReplaceAllButton:beginReplaceAll();break;
    case MatchCase:case WholeWord:case Regex:SendMessageW(control(id),BM_SETCHECK,checked(id)?BST_UNCHECKED:BST_CHECKED,0);lastZero=-1;break;
    case GoTo:showFind(false,true);break;case GoToButton:goToLine();break;case HideFind:findVisible=false;layout();if(h)SetFocus(h);break;case CancelWork:cancelWork();break;
    case Bookmark:toggleBookmark();break;case NextBookmark:nextBookmark();break;case Complete:completeWord();break;
    case Dark:theme=dark?Theme::Light:Theme::Dark;applyAppearance();break;case ThemeSystem:case ThemeLight:case ThemeDark:theme=(Theme)(id-ThemeSystem);applyAppearance();break;
    case Wrap:wrap=!wrap;applyAppearance();break;case Whitespace:whitespace=!whitespace;applyAppearance();break;case LineNumbers:lineNumbers=!lineNumbers;applyAppearance();break;
    case ShowToolbar:showToolbar=!showToolbar;layout();break;case ShowStatus:showStatus=!showStatus;layout();break;
    case Split:split=!split;syncSplit();layout();tell(split?L"Split view: both panes edit the same document.":L"Single editor view.");break;
    case ZoomIn:case ZoomOut:case ZoomReset:fontSize=id==ZoomReset?11:std::max(8,std::min(32,fontSize+(id==ZoomIn?1:-1)));applyAppearance();break;
    case FoldAll:if(h)sci(h,SCI_FOLDALL,SC_FOLDACTION_CONTRACT);break;case ExpandAll:if(h)sci(h,SCI_FOLDALL,SC_FOLDACTION_EXPAND);break;
    case AutoLanguage:if(d){d->automatic=true;d->language=detectLanguage(d->path);colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);syncSplit();refreshNeeded=true;}break;
    case About:MessageBoxW(window,L"NoteUltra 1.0\n\nA native Windows text and code editor.\nBuilt with Scintilla 5.6.7 and Lexilla 5.5.4.\n\nIndependent software; not affiliated with Notepad++.\nThird-party licenses are included beside the executable.",L"About NoteUltra",MB_OK|MB_ICONINFORMATION);break;
    default:{if(!d||!h||d->busy!=Busy::None)return;UINT message=0;switch(id){case Undo:message=SCI_UNDO;break;case Redo:message=SCI_REDO;break;case Cut:message=SCI_CUT;break;case Copy:message=SCI_COPY;break;case Paste:message=SCI_PASTE;break;case SelectAll:message=SCI_SELECTALL;break;case Duplicate:message=SCI_SELECTIONDUPLICATE;break;case DeleteLine:message=SCI_LINEDELETE;break;case Upper:message=SCI_UPPERCASE;break;case Lower:message=SCI_LOWERCASE;break;case Indent:message=SCI_TAB;break;case Unindent:message=SCI_BACKTAB;break;}if(message){sci(h,message);SetFocus(h);}break;}
    }refreshNeeded=true;
}
void loadSettings(){if(testMode)return;PWSTR local=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local))){settingsPath=std::wstring(local)+L"\\NoteUltra";CoTaskMemFree(local);CreateDirectoryW(settingsPath.c_str(),nullptr);settingsPath+=L"\\settings.ini";
        auto get=[](const wchar_t* key,int fallback){return (int)GetPrivateProfileIntW(L"View",key,fallback,settingsPath.c_str());};
        int legacy=get(L"Dark",-1),saved=get(L"Theme",legacy<0?0:(legacy?2:1));theme=(Theme)std::max(0,std::min(2,saved));wrap=get(L"Wrap",0)!=0;showToolbar=get(L"Toolbar",1)!=0;showStatus=get(L"StatusBar",1)!=0;fontSize=std::max(8,std::min(32,get(L"FontSize",11)));}}
void saveSettings(){if(testMode||settingsPath.empty())return;auto set=[](const wchar_t* key,int value){WritePrivateProfileStringW(L"View",key,std::to_wstring(value).c_str(),settingsPath.c_str());};
    set(L"Theme",(int)theme);set(L"Wrap",wrap);set(L"Toolbar",showToolbar);set(L"StatusBar",showStatus);set(L"FontSize",fontSize);}
void buildControls(){dpi=dpiOf(window);dark=theme==Theme::Dark||(theme==Theme::System&&systemUsesDark());pal=makePalette(dark);createFonts();
    tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,instance,nullptr);SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,px(600));SendMessageW(tooltip,WM_SETFONT,(WPARAM)statusFont,TRUE);
    tabControl=add(L"NoteUltra.Tabs",L"Documents",0,Tabs);toolBar=add(L"NoteUltra.Toolbar",L"Commands",0,Toolbar);statusBar=add(L"NoteUltra.Status",L"Status",0,Status);
    auto field=[](int id,const wchar_t* cue,DWORD style){HWND e=add(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|style,id);SendMessageW(e,EM_SETCUEBANNER,TRUE,(LPARAM)cue);SendMessageW(e,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);return e;};
    field(FindField,L"Find",0);field(ReplaceField,L"Replace",0);field(GoToText,L"Go to line",ES_NUMBER);
    SendMessageW(control(FindField),EM_SETLIMITTEXT,8192,0);SendMessageW(control(ReplaceField),EM_SETLIMITTEXT,8192,0);SendMessageW(control(GoToText),EM_SETLIMITTEXT,12,0);
    struct Named{int id;const wchar_t* name;const wchar_t* tip;};
    for(auto b:{Named{MatchCase,L"Match case",L"Match case (Alt+C)"},{WholeWord,L"Whole word",L"Match whole word (Alt+W)"},{Regex,L"Regular expression",L"Use regular expression (Alt+R)"},{PreviousButton,L"Previous match",L"Previous match (Shift+F3)"},{NextButton,L"Next match",L"Next match (F3)"},
        {HideFind,L"Close",L"Close (Escape)"},{ReplaceButton,L"Replace",nullptr},{ReplaceAllButton,L"Replace all",nullptr},{GoToButton,L"Go",nullptr},{CancelWork,L"Stop",L"Stop the current operation (Escape)"}}){
        HWND button=add(L"BUTTON",b.name,WS_TABSTOP|BS_OWNERDRAW,b.id);SetWindowSubclass(button,buttonProc,1,0);if(b.tip)hwndTip(button,b.tip);}
    secondary=createEditor();newDocument();applyAppearance();layout();DragAcceptFiles(window,TRUE);SetTimer(window,1,40,nullptr);
}
void editorNotification(SCNotification* n){
    HWND source=(HWND)n->nmhdr.hwndFrom;auto d=current();if(!d)return;
    if(n->nmhdr.code==SCN_UPDATEUI){refreshNeeded=true;if(source==editor()){
        auto pos=sci(source,SCI_GETCURRENTPOS);Sci_Position brace=-1;
        for(auto candidate:{pos-1,pos})if(candidate>=0){int c=(int)sci(source,SCI_GETCHARAT,candidate);if(c&&strchr("[]{}()",c)){brace=candidate;break;}}
        if(brace>=0){auto other=sci(source,SCI_BRACEMATCH,brace,0);if(other>=0)sci(source,SCI_BRACEHIGHLIGHT,brace,other);else sci(source,SCI_BRACEBADLIGHT,brace);}else sci(source,SCI_BRACEHIGHLIGHT,(WPARAM)-1,-1);
    }}
    if(n->nmhdr.code==SCN_SAVEPOINTLEFT||n->nmhdr.code==SCN_SAVEPOINTREACHED||n->nmhdr.code==SCN_MODIFIED)refreshNeeded=true;
    if(n->nmhdr.code==SCN_MARGINCLICK){auto line=sci(source,SCI_LINEFROMPOSITION,n->position);if(n->margin==2)sci(source,SCI_TOGGLEFOLD,line);else if(n->margin==1){if(sci(source,SCI_MARKERGET,line)&1)sci(source,SCI_MARKERDELETE,line,0);else sci(source,SCI_MARKERADD,line,0);}}
    if(n->nmhdr.code==SCN_CHARADDED&&n->ch=='\n'&&d->busy==Busy::None){auto line=sci(source,SCI_LINEFROMPOSITION,sci(source,SCI_GETCURRENTPOS));if(line>0){auto indent=sci(source,SCI_GETLINEINDENTATION,line-1);sci(source,SCI_SETLINEINDENTATION,line,indent);sci(source,SCI_GOTOPOS,sci(source,SCI_GETLINEINDENTPOSITION,line));}}
}
LRESULT CALLBACK windowProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_CREATE:window=h;buildControls();return 0;
case WM_SIZE:if(tabControl)layout();return 0;
case WM_GETMINMAXINFO:{auto info=(MINMAXINFO*)lp;info->ptMinTrackSize.x=px(720);info->ptMinTrackSize.y=px(420);return 0;}
case WM_ERASEBKGND:return 1;
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintMain(dc);EndPaint(h,&ps);return 0;}
case WM_DRAWITEM:drawButton((DRAWITEMSTRUCT*)lp);return TRUE;
case WM_CTLCOLOREDIT:{HDC dc=(HDC)wp;bool focused=GetFocus()==(HWND)lp;SetTextColor(dc,pal.text);SetBkColor(dc,focused?pal.fieldFocus:pal.field);return (LRESULT)(focused?focusBrush:fieldBrush);}
case WM_UAHDRAWMENU:paintMenuBar(h,(UAHMENU*)lp,pal);return TRUE;
case WM_UAHDRAWMENUITEM:paintMenuItem(h,(UAHDRAWMENUITEM*)lp,pal);return TRUE;
case WM_NCPAINT:case WM_NCACTIVATE:{LRESULT result=DefWindowProcW(h,msg,wp,lp);paintMenuLine(h,pal);return result;}
case WM_SETTINGCHANGE:if(lp&&lstrcmpW((LPCWSTR)lp,L"ImmersiveColorSet")==0)applyAppearance();break;
case WM_DWMCOLORIZATIONCOLORCHANGED:applyAppearance();break;
case WM_DPICHANGED:{dpi=HIWORD(wp);createFonts();auto r=(RECT*)lp;SetWindowPos(h,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);applyAppearance();layout();layoutTabs();layoutStatus();return 0;}
case WM_COMMAND:{int code=HIWORD(wp),id=LOWORD(wp);if(code==EN_SETFOCUS||code==EN_KILLFOCUS){invalidateBox(id);return 0;}
    if(code==BN_CLICKED||code==1)try{command(id);}catch(const std::exception&e){tell(L"Operation failed: "+widen(e.what()));}return 0;}
case WM_NOTIFY:{auto header=(NMHDR*)lp;if(isEditor(header->hwndFrom))editorNotification((SCNotification*)lp);return 0;}
case WM_DROPFILES:{HDROP drop=(HDROP)wp;UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);for(UINT i=0;i<count;++i){UINT length=DragQueryFileW(drop,i,nullptr,0);std::wstring path(length+1,L' ');DragQueryFileW(drop,i,&path[0],length+1);path.resize(length);openPath(path);}DragFinish(drop);return 0;}
case WM_TIMER:tick();return 0;
case WM_SETFOCUS:if(editor())SetFocus(editor());return 0;
case WM_CLOSE:closingAll=true;exitAfterClose=true;closeAllStep();return 0;
case WM_DESTROY:saveSettings();KillTimer(h,1);PostQuitMessage(0);return 0;
}return DefWindowProcW(h,msg,wp,lp);}
bool shortcut(MSG& msg){if(msg.message!=WM_KEYDOWN&&msg.message!=WM_SYSKEYDOWN)return false;bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,alt=(GetKeyState(VK_MENU)&0x8000)!=0;int key=(int)msg.wParam,cmd=0;
    if(key==VK_ESCAPE){cancelWork();return true;}
    if(ctrl){if(key==VK_TAB){if(!documents.empty())selectDocument((active+(shift?(int)documents.size()-1:1))%(int)documents.size());return true;}
        if(key=='N')cmd=New;else if(key=='O')cmd=Open;else if(key=='S')cmd=alt?Split:(shift?SaveAs:Save);else if(key=='W')cmd=Close;else if(key=='F')cmd=Find;else if(key=='H')cmd=Replace;else if(key=='G')cmd=GoTo;
        else if(key==VK_F2)cmd=Bookmark;else if(key==VK_SPACE)cmd=Complete;else if(key==VK_OEM_PLUS||key==VK_ADD)cmd=ZoomIn;else if(key==VK_OEM_MINUS||key==VK_SUBTRACT)cmd=ZoomOut;else if(key=='0')cmd=ZoomReset;
        else if(GetFocus()==editor()){if(key=='D')cmd=Duplicate;else if(key=='L'&&shift)cmd=DeleteLine;else if(key=='U')cmd=shift?Upper:Lower;}
    }
    if(key==VK_F3)cmd=shift?FindPrevious:FindNext;if(key==VK_F2&&!ctrl)cmd=NextBookmark;if(alt&&!ctrl&&key=='Z')cmd=Wrap;
    if(findVisible){HWND focus=GetFocus();bool inFind=focus==control(FindField)||focus==control(ReplaceField);
        if(alt&&!ctrl&&inFind){if(key=='C')cmd=MatchCase;else if(key=='W')cmd=WholeWord;else if(key=='R')cmd=Regex;}
        if(key==VK_RETURN){if(focus==control(FindField))cmd=shift?PreviousButton:NextButton;else if(focus==control(ReplaceField))cmd=ReplaceButton;else if(focus==control(GoToText))cmd=GoToButton;}}
    if(cmd){command(cmd);return true;}return false;
}
#ifdef NOTEULTRA_TESTING
#include "self_test.hpp"
#endif
ATOM registerClass(const wchar_t* name,WNDPROC proc,UINT style=0){WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.style=style;cls.hInstance=instance;cls.lpfnWndProc=proc;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=name;return RegisterClassExW(&cls);}
int WINAPI wWinMain(HINSTANCE appInstance,HINSTANCE,PWSTR,int show){instance=appInstance;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
#ifdef NOTEULTRA_TESTING
    testMode=argc==3&&wcscmp(argv[1],L"--self-test")==0;
#endif
    wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);std::wstring base=exe;auto slash=base.find_last_of(L"\\/");base=base.substr(0,slash+1);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    scintillaDLL=LoadLibraryExW((base+L"Scintilla.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    lexillaDLL=LoadLibraryExW((base+L"Lexilla.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(lexillaDLL)createLexer=(Lexilla::CreateLexerFn)GetProcAddress(lexillaDLL,"CreateLexer");
    if(!scintillaDLL||!lexillaDLL||!createLexer){MessageBoxW(nullptr,L"Keep NoteUltra.exe, Scintilla.dll, and Lexilla.dll together. An editor library could not be loaded.",L"NoteUltra",MB_OK|MB_ICONERROR);LocalFree(argv);return 1;}
    Gdiplus::GdiplusStartupInput gdiplusInput;ULONG_PTR gdiplusToken=0;Gdiplus::GdiplusStartup(&gdiplusToken,&gdiplusInput,nullptr);
    startIO();loadSettings();INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TAB_CLASSES|ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    setAppDark(theme==Theme::Dark||(theme==Theme::System&&systemUsesDark()));
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.hInstance=instance;cls.lpfnWndProc=windowProc;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));cls.lpszClassName=L"NoteUltra.Main";RegisterClassExW(&cls);
    registerClass(L"NoteUltra.Tabs",tabsProc,CS_DBLCLKS);registerClass(L"NoteUltra.Toolbar",toolsProc,CS_DBLCLKS);registerClass(L"NoteUltra.Status",statusProc);
    dpi=dpiOf(nullptr);
    HWND h=CreateWindowExW(WS_EX_ACCEPTFILES,cls.lpszClassName,L"NoteUltra",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,px(1280),px(850),nullptr,menu(),instance,nullptr);if(!h){stopIO();LocalFree(argv);return 1;}
    int code=0;
#ifdef NOTEULTRA_TESTING
    if(testMode){code=selfTests(argv[2]);}
    else
#endif
    {for(int i=1;i<argc;++i)openPath(argv[i]);ShowWindow(h,show);UpdateWindow(h);MSG message;
        while(GetMessageW(&message,nullptr,0,0)>0){try{if(shortcut(message))continue;}catch(const std::exception&e){tell(widen(e.what()));}
            // Scintilla owns Tab/Enter and its edit shortcuts; only other controls
            // use dialog navigation.
            HWND focus=GetFocus();bool inEditor=focus==secondary;for(auto d:documents)if(focus==d->editor){inEditor=true;break;}
            if(inEditor||!IsDialogMessageW(h,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }code=(int)message.wParam;}
    stopIO();LocalFree(argv);FreeLibrary(lexillaDLL);FreeLibrary(scintillaDLL);for(HFONT f:{uiFont,statusFont,iconFont,smallIcons,tinyIcons})DeleteObject(f);for(HBRUSH b:{fieldBrush,focusBrush})DeleteObject(b);Gdiplus::GdiplusShutdown(gdiplusToken);CoUninitialize();return code;
}
