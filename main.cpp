#include "file_io.hpp"
#include "languages.hpp"
#include "markdown.hpp"
#include "vendor/scintilla/include/ILoader.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <richedit.h>
#include <random>
#include <numeric>
#include <unordered_set>
#include <climits>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <sstream>
#include <chrono>
#include <cstdio>

using namespace nu;
enum Command {
 New=1001,Open,Save,SaveAs,SaveAll,Close,CloseAll,Exit,CopyPath,RevealPath,Print,PrintNow,ExportPdf,RestoreClosed,OpenAllRecent,ClearRecent,
 Undo=1101,Redo,Cut,Copy,Paste,SelectAll,Duplicate,DeleteLine,Upper,Lower,Indent,Unindent,
 LineDuplicate=1150,RemoveDuplicates,RemoveConsecutive,SplitLines,JoinLines,MoveLineUp,MoveLineDown,RemoveEmpty,RemoveBlank,InsertAbove,InsertBelow,ReverseLines,ShuffleLines,
 SortBase=1170,// SortBase+kind*2+descending; kinds: lexicographic, ignoring case, locale, integer, decimal comma, decimal dot, length
 Find=1201,Replace,FindNext,FindPrevious,GoTo,Bookmark,NextBookmark,Complete,FindAllCurrent,FindAllOpen,
 Wrap=1301,Whitespace,LineNumbers,Dark,Split,ZoomIn,ZoomOut,ZoomReset,FoldAll,ExpandAll,ShowToolbar,ShowStatus,ThemeSystem,ThemeLight,ThemeDark,DocumentMap,DocumentList,MarkdownPreview,SearchResults,
 AutoLanguage=2999,LanguageBase=3000,EncodingBase=4000,EolBase=4100,About=4200,RecentBase=4300,
 Tabs=5001,Status,Toolbar,ListPanel,MapPanel,ResultsPanel,PreviewPanel,
 FindField=5101,ReplaceField,NextButton,PreviousButton,ReplaceButton,ReplaceAllButton,MatchCase,WholeWord,Regex,HideFind,GoToText,GoToButton,CancelWork,FindAllButton,FindAllDocsButton
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
bool showMap=false,showList=false,showPreview=false,showResults=false,closingAll=false,exitAfterClose=false,testMode=false,refreshNeeded=true;int fontSize=11,dpi=96;Theme theme=Theme::System;Palette pal=makePalette(false);
HFONT uiFont=nullptr,statusFont=nullptr,iconFont=nullptr,smallIcons=nullptr,tinyIcons=nullptr;HBRUSH fieldBrush=nullptr,focusBrush=nullptr;int fieldTextHeight=18;
std::wstring notice=L"Ready",settingsPath;std::vector<std::wstring> recent,closedFiles;
Sci_Position lastZero=-1;std::string lastQuery;
void startIO(){ioThread=std::thread([]{while(true){Job* job=nullptr;{std::unique_lock<std::mutex> lock(ioMutex);ioReady.wait(lock,[]{return ioShutdown||!ioQueue.empty();});if(ioShutdown&&ioQueue.empty())break;job=ioQueue.front();ioQueue.pop_front();}auto work=std::move(job->work);work();work=nullptr;job->done=true;}});}
void submit(std::unique_ptr<Job> job){Job* ptr=job.get();jobs.push_back(std::move(job));{std::lock_guard<std::mutex> lock(ioMutex);ioQueue.push_back(ptr);}ioReady.notify_one();}
void stopIO(){{std::lock_guard<std::mutex> lock(ioMutex);ioShutdown=true;}ioReady.notify_one();if(ioThread.joinable())ioThread.join();}
LRESULT CALLBACK windowProc(HWND,UINT,WPARAM,LPARAM);
void layout();void refresh();void schedulePreview(int delay=0);void addRecent(const std::wstring&);void applyAppearance();void closeAllStep();void requestClose(std::shared_ptr<Document>);void selectDocument(int);void newDocument();void tick();void command(int);void layoutTabs(bool reveal=true);void layoutList(bool reveal=true);void syncMap();void updateMap();void previewTick();
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
void syncSplit(){auto d=current();if(!d||!secondary)return;sci(secondary,SCI_SETDOCPOINTER,0,sci(d->editor,SCI_GETDOCPOINTER));sci(secondary,SCI_SETREADONLY,d->busy!=Busy::None);configure(secondary);colorLanguage(secondary,d->large?0:d->language,createLexer,dark,false);ShowWindow(secondary,split?SW_SHOW:SW_HIDE);syncMap();}
void tabsChanged(){layoutTabs();refreshNeeded=true;}
void selectDocument(int index){if(index<0||index>=(int)documents.size())return;auto old=current();if(old&&old!=documents[index])ShowWindow(old->editor,SW_HIDE);active=index;ShowWindow(documents[index]->editor,SW_SHOW);syncSplit();layout();tabsChanged();SetFocus(documents[index]->editor);lastZero=-1;schedulePreview();}
std::shared_ptr<Document> makeDocument(const std::wstring& path=L""){
    auto d=std::make_shared<Document>();d->number=++nextNumber;d->title=L"Untitled "+std::to_wstring(d->number);d->path=path;d->editor=createEditor();d->language=detectLanguage(path);colorLanguage(d->editor,d->language,createLexer,dark);
    documents.push_back(d);selectDocument((int)documents.size()-1);return d;
}
void newDocument(){makeDocument();tell(L"New document");}
void removeDocument(std::shared_ptr<Document> d){auto it=std::find(documents.begin(),documents.end(),d);if(it==documents.end())return;int index=(int)(it-documents.begin());bool wasActive=active==index;
    if(!d->path.empty()){addRecent(d->path);closedFiles.push_back(d->path);if(closedFiles.size()>30)closedFiles.erase(closedFiles.begin());}
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
// The save prompt and Save As dialog pump messages, so the timer can call back in here; a nested call would stack another prompt.
void closeAllStep(){static bool running=false;if(!closingAll||running)return;running=true;
    while(!documents.empty()){auto d=current();if(!d){selectDocument(0);d=current();}size_t count=documents.size();requestClose(d);if(!closingAll||documents.size()==count){running=false;return;}}
    running=false;bool quit=exitAfterClose;closingAll=false;exitAfterClose=false;if(quit)DestroyWindow(window);else newDocument();
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
// ---- Line operations (Edit > Line operations), as in Notepad++ ----
// They act on the lines the selection touches, or on the whole document when the selection is within one line.
bool editable(){auto d=current();if(!d||!editor()||d->busy!=Busy::None){tell(L"Wait for the current operation to finish.");return false;}return true;}
std::string lineEnd(HWND h,Sci_Position line){auto a=sci(h,SCI_GETLINEENDPOSITION,line),b=sci(h,SCI_POSITIONFROMLINE,line+1);if(b>a){const char* p=(const char*)sci(h,SCI_GETRANGEPOINTER,a,b-a);return std::string(p,(size_t)(b-a));}
    int m=(int)sci(h,SCI_GETEOLMODE);return m==SC_EOL_CRLF?"\r\n":m==SC_EOL_CR?"\r":"\n";}
void transformLines(const std::function<void(std::vector<std::string>&)>& change){if(!editable())return;HWND h=editor();
    auto a=sci(h,SCI_GETSELECTIONSTART),b=sci(h,SCI_GETSELECTIONEND);auto first=sci(h,SCI_LINEFROMPOSITION,a),last=sci(h,SCI_LINEFROMPOSITION,b);bool whole=first==last;
    if(whole){first=0;last=sci(h,SCI_GETLINECOUNT)-1;if(last>0&&sci(h,SCI_POSITIONFROMLINE,last)==sci(h,SCI_GETLENGTH))--last;}else if(b==sci(h,SCI_POSITIONFROMLINE,last))--last;
    auto start=sci(h,SCI_POSITIONFROMLINE,first),end=sci(h,SCI_GETLINEENDPOSITION,last);const char* p=(const char*)sci(h,SCI_GETRANGEPOINTER,start,end-start);std::string text(p?p:"",(size_t)(end-start));
    std::string eol=text.find("\r\n")!=text.npos?"\r\n":text.find('\n')!=text.npos?"\n":text.find('\r')!=text.npos?"\r":lineEnd(h,last);
    std::vector<std::string> lines;size_t from=0;for(size_t i=0;i<=text.size();++i){if(i==text.size()||text[i]=='\n'||text[i]=='\r'){lines.push_back(text.substr(from,i-from));if(i+1<text.size()&&text[i]=='\r'&&text[i+1]=='\n')++i;from=i+1;}}
    size_t before=lines.size();change(lines);std::string result;for(size_t i=0;i<lines.size();++i){if(i)result+=eol;result+=lines[i];}
    if(result==text){tell(L"No lines changed.");return;}
    // Removing every line of a range that ends before the last line also removes its line ending.
    if(lines.empty()&&last+1<sci(h,SCI_GETLINECOUNT))end=sci(h,SCI_POSITIONFROMLINE,last+1);
    sci(h,SCI_BEGINUNDOACTION);sci(h,SCI_SETTARGETRANGE,start,end);sci(h,SCI_REPLACETARGET,result.size(),(LPARAM)result.data());sci(h,SCI_ENDUNDOACTION);
    if(whole)sci(h,SCI_GOTOPOS,std::min(a,sci(h,SCI_GETLENGTH)));else sci(h,SCI_SETSEL,start,start+(Sci_Position)result.size());
    tell(before==lines.size()?std::to_wstring(lines.size())+L" line(s) rearranged.":std::to_wstring(before-lines.size())+L" line(s) removed.");}
std::wstring wideLine(const std::string& s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0);std::wstring w(n,L' ');MultiByteToWideChar(CP_UTF8,0,s.data(),(int)s.size(),&w[0],n);return w;}
// Numeric sorts read the number at the start of the line; lines without one sort first.
bool lineNumber(std::string s,int kind,double& value){size_t a=s.find_first_not_of(" \t");if(a==s.npos)return false;s=s.substr(a);
    if(kind==3){char* end=nullptr;long long v=strtoll(s.c_str(),&end,10);if(end==s.c_str())return false;value=(double)v;return true;}
    std::string t;for(char c:s){if(kind==4){if(c=='.')continue;if(c==','){t+='.';continue;}}else if(c==',')continue;t+=c;}
    char* end=nullptr;value=strtod(t.c_str(),&end);return end!=t.c_str();}
void sortLines(int kind,bool descending){transformLines([&](std::vector<std::string>& lines){std::vector<size_t> order(lines.size());std::iota(order.begin(),order.end(),0);
    std::vector<std::wstring> wide;std::vector<double> value(lines.size(),0);std::vector<char> numeric(lines.size(),0);std::vector<size_t> length(lines.size(),0);
    if(kind==1||kind==2)for(auto& l:lines)wide.push_back(wideLine(l));
    if(kind>=3&&kind<=5)for(size_t i=0;i<lines.size();++i)numeric[i]=lineNumber(lines[i],kind,value[i]);
    if(kind==6)for(size_t i=0;i<lines.size();++i)for(char c:lines[i])if(((unsigned char)c&0xC0)!=0x80)++length[i];
    auto less=[&](size_t a,size_t b){switch(kind){
        case 0:return lines[a]<lines[b];
        case 1:return CompareStringOrdinal(wide[a].data(),(int)wide[a].size(),wide[b].data(),(int)wide[b].size(),TRUE)==CSTR_LESS_THAN;
        case 2:return CompareStringEx(LOCALE_NAME_USER_DEFAULT,0,wide[a].data(),(int)wide[a].size(),wide[b].data(),(int)wide[b].size(),nullptr,nullptr,0)==CSTR_LESS_THAN;
        case 6:return length[a]<length[b];
        default:if(numeric[a]!=numeric[b])return numeric[b]!=0;return value[a]<value[b];}};
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return descending?less(b,a):less(a,b);});
    std::vector<std::string> sorted;sorted.reserve(lines.size());for(size_t i:order)sorted.push_back(std::move(lines[i]));lines=std::move(sorted);});}
void insertBlankLine(bool below){if(!editable())return;HWND h=editor();auto line=sci(h,SCI_LINEFROMPOSITION,sci(h,SCI_GETCURRENTPOS));auto eol=lineEnd(h,line);
    auto pos=below?sci(h,SCI_GETLINEENDPOSITION,line):sci(h,SCI_POSITIONFROMLINE,line);sci(h,SCI_INSERTTEXT,pos,(LPARAM)eol.c_str());sci(h,SCI_GOTOPOS,below?pos+(Sci_Position)eol.size():pos);}
void lineCommand(int id){if(id>=SortBase&&id<SortBase+14){sortLines((id-SortBase)/2,(id-SortBase)%2==1);return;}if(!editable())return;HWND h=editor();
    auto blankLine=[](const std::string& l){return l.find_first_not_of(" \t\f\v")==l.npos;};
    switch(id){
    case LineDuplicate:sci(h,SCI_LINEDUPLICATE);break;case MoveLineUp:sci(h,SCI_MOVESELECTEDLINESUP);break;case MoveLineDown:sci(h,SCI_MOVESELECTEDLINESDOWN);break;
    case InsertAbove:insertBlankLine(false);break;case InsertBelow:insertBlankLine(true);break;
    case RemoveDuplicates:transformLines([](std::vector<std::string>& lines){std::unordered_set<std::string> seen;std::vector<std::string> kept;for(auto& l:lines)if(seen.insert(l).second)kept.push_back(l);lines=std::move(kept);});break;
    case RemoveConsecutive:transformLines([](std::vector<std::string>& lines){lines.erase(std::unique(lines.begin(),lines.end()),lines.end());});break;
    case RemoveEmpty:transformLines([](std::vector<std::string>& lines){lines.erase(std::remove_if(lines.begin(),lines.end(),[](const std::string& l){return l.empty();}),lines.end());});break;
    case RemoveBlank:transformLines([&](std::vector<std::string>& lines){lines.erase(std::remove_if(lines.begin(),lines.end(),blankLine),lines.end());});break;
    case ReverseLines:transformLines([](std::vector<std::string>& lines){std::reverse(lines.begin(),lines.end());});break;
    case ShuffleLines:transformLines([](std::vector<std::string>& lines){std::mt19937 random((unsigned)GetTickCount64()^std::random_device{}());std::shuffle(lines.begin(),lines.end(),random);});break;
    case SplitLines:case JoinLines:{auto a=sci(h,SCI_GETSELECTIONSTART),b=sci(h,SCI_GETSELECTIONEND);auto first=sci(h,SCI_LINEFROMPOSITION,a),last=sci(h,SCI_LINEFROMPOSITION,b);
        if(id==JoinLines&&first==last)last=std::min(first+1,sci(h,SCI_GETLINECOUNT)-1);else if(last>first&&b==sci(h,SCI_POSITIONFROMLINE,last))--last;
        sci(h,SCI_SETTARGETRANGE,sci(h,SCI_POSITIONFROMLINE,first),sci(h,SCI_GETLINEENDPOSITION,last));sci(h,SCI_BEGINUNDOACTION);sci(h,id==JoinLines?SCI_LINESJOIN:SCI_LINESSPLIT,0);sci(h,SCI_ENDUNDOACTION);break;}
    }SetFocus(h);}

// ---- Recent files (File menu) ----
HMENU fileMenu=nullptr;
void saveRecent(){if(testMode||settingsPath.empty())return;WritePrivateProfileStringW(L"Recent",nullptr,nullptr,settingsPath.c_str());
    for(size_t i=0;i<recent.size();++i)WritePrivateProfileStringW(L"Recent",(L"File"+std::to_wstring(i+1)).c_str(),recent[i].c_str(),settingsPath.c_str());}
void addRecent(const std::wstring& path){if(path.empty())return;auto key=lower(path);recent.erase(std::remove_if(recent.begin(),recent.end(),[&](const std::wstring& p){return lower(p)==key;}),recent.end());
    recent.insert(recent.begin(),path);if(recent.size()>10)recent.resize(10);saveRecent();}
bool isOpen(const std::wstring& path){for(auto& d:documents)if(!d->path.empty()&&lower(d->path)==lower(path))return true;return false;}
bool exists(const std::wstring& path){return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;}
void openRecent(size_t i){if(i>=recent.size())return;auto path=recent[i];if(exists(path)){openPath(path);return;}
    if(MessageBoxW(window,(path+L"\n\nThis file no longer exists. Remove it from the recent files list?").c_str(),L"NoteUltra",MB_YESNO|MB_ICONQUESTION)==IDYES){recent.erase(recent.begin()+(long)i);saveRecent();}}
void restoreClosed(){while(!closedFiles.empty()){auto path=closedFiles.back();closedFiles.pop_back();if(!isOpen(path)&&exists(path)){openPath(path);return;}}tell(L"No closed files to restore.");}
std::wstring menuPath(std::wstring p){if(p.size()>90)p=p.substr(0,36)+L"…"+p.substr(p.size()-53);std::wstring s;for(wchar_t c:p){if(c==L'&')s+=L'&';s+=c;}return s;}
// The File menu is rebuilt each time it opens so that the recent list is current.
void fillFileMenu(){HMENU f=fileMenu;if(!f)return;while(GetMenuItemCount(f)>0)DeleteMenu(f,0,MF_BYPOSITION);auto d=current();bool idle=d&&d->busy==Busy::None,ready=d&&d->busy!=Busy::Loading;
    auto item=[f](int id,const std::wstring& name,bool on=true){AppendMenuW(f,MF_STRING|(on?0:MF_GRAYED),id,name.c_str());};auto separator=[f]{AppendMenuW(f,MF_SEPARATOR,0,nullptr);};
    item(New,L"&New\tCtrl+N");item(Open,L"&Open…\tCtrl+O");separator();item(Save,L"&Save\tCtrl+S",idle);item(SaveAs,L"Save &as…\tCtrl+Shift+S",idle);item(SaveAll,L"Save a&ll");separator();
    item(Close,L"&Close tab\tCtrl+W");item(CloseAll,L"Close all ta&bs");separator();item(Print,L"&Print…\tCtrl+P",ready);item(PrintNow,L"Print no&w",ready);item(ExportPdf,L"Export as P&DF…",ready);separator();
    for(size_t i=0;i<recent.size();++i)item(RecentBase+(int)i,(i<9?L"&"+std::to_wstring(i+1):std::wstring(L"1&0"))+L": "+menuPath(recent[i]));if(!recent.empty())separator();
    item(RestoreClosed,L"&Restore recent closed file\tCtrl+Shift+T",!closedFiles.empty());item(OpenAllRecent,L"Open all recent &files",!recent.empty());item(ClearRecent,L"&Empty recent files list",!recent.empty());separator();item(Exit,L"E&xit\tAlt+F4");}

// ---- Printing and PDF export ----
// Scintilla draws the pages through a hidden view that shares the document and uses the light
// palette, so dark mode still prints dark text on white paper.
HGLOBAL printMode=nullptr,printNames=nullptr;
bool printTo(HDC dc,const std::wstring& output,Sci_Position start,Sci_Position end,int from,int to){auto d=current();if(!d||!dc)return false;
    HWND view=CreateWindowExW(0,L"Scintilla",L"",WS_CHILD,0,0,100,100,window,nullptr,instance,nullptr);if(!view)return false;Palette light=makePalette(false);
    sci(view,SCI_SETDOCPOINTER,0,sci(d->editor,SCI_GETDOCPOINTER));sci(view,SCI_SETCODEPAGE,SC_CP_UTF8);paintEditor(view,light,lineNumbers,false,false,std::min(fontSize,11),dpi);colorLanguage(view,d->large?0:d->language,createLexer,false,false);
    for(int m=1;m<5;++m)sci(view,SCI_SETMARGINWIDTHN,m,0);sci(view,SCI_SETPRINTCOLOURMODE,SC_PRINT_COLOURONWHITE);sci(view,SCI_SETPRINTWRAPMODE,SC_WRAP_WORD);sci(view,SCI_SETPRINTMAGNIFICATION,0);if(!d->large)sci(view,SCI_COLOURISE,0,-1);
    int pw=GetDeviceCaps(dc,PHYSICALWIDTH),ph=GetDeviceCaps(dc,PHYSICALHEIGHT),ox=GetDeviceCaps(dc,PHYSICALOFFSETX),oy=GetDeviceCaps(dc,PHYSICALOFFSETY),hr=GetDeviceCaps(dc,HORZRES),vr=GetDeviceCaps(dc,VERTRES),dx=GetDeviceCaps(dc,LOGPIXELSX),dy=GetDeviceCaps(dc,LOGPIXELSY);
    if(pw<=0||ph<=0){pw=hr;ph=vr;}RECT body{std::max(0,dx*3/5-ox),std::max(0,dy*3/5-oy),std::min(hr,pw-dx*3/5-ox),std::min(vr,ph-dy*3/5-oy)};
    HFONT font=CreateFontW(-MulDiv(9,dy,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HGDIOBJ oldFont=SelectObject(dc,font);TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);SelectObject(dc,oldFont);int lh=tm.tmHeight;
    Sci_RangeToFormatFull range{};range.hdc=range.hdcTarget=dc;range.rc={(int)body.left,(int)body.top+lh*2,(int)body.right,(int)body.bottom-lh*2};range.rcPage={0,0,hr,vr};
    std::wstring name=d->path.empty()?d->title:d->path;wchar_t date[128]{};GetDateFormatEx(LOCALE_NAME_USER_DEFAULT,DATE_SHORTDATE,nullptr,nullptr,date,128,nullptr);
    DOCINFOW info{};info.cbSize=sizeof(info);info.lpszDocName=name.c_str();info.lpszOutput=output.empty()?nullptr:output.c_str();
    bool ok=StartDocW(dc,&info)>0;int page=1;Sci_Position pos=start;
    while(ok){bool draw=page>=from&&page<=to;
        if(draw){if(StartPage(dc)<=0){ok=false;break;}RECT head{body.left,body.top,body.right,body.top+lh};RECT foot{body.left,body.bottom-lh,body.right,body.bottom};
            drawText(dc,name,{head.left,head.top,head.right-lh*8,head.bottom},light.textSecondary,font,DT_SINGLELINE|DT_LEFT|DT_PATH_ELLIPSIS|DT_NOPREFIX);drawText(dc,date,head,light.textSecondary,font,DT_SINGLELINE|DT_RIGHT|DT_NOPREFIX);
            fill(dc,{body.left,head.bottom+lh/3,body.right,head.bottom+lh/3+std::max(1,dy/150)},light.divider);drawText(dc,L"Page "+std::to_wstring(page),foot,light.textSecondary,font,DT_SINGLELINE|DT_CENTER|DT_NOPREFIX);}
        range.chrg.cpMin=pos;range.chrg.cpMax=end;Sci_Position next=sci(view,SCI_FORMATRANGEFULL,draw,(LPARAM)&range);
        if(draw&&EndPage(dc)<=0){ok=false;break;}
        if(next<=pos||next>=end||page>=to)break;pos=next;++page;}
    sci(view,SCI_FORMATRANGEFULL,FALSE,0);if(ok)ok=EndDoc(dc)>0;else AbortDoc(dc);DestroyWindow(view);DeleteObject(font);return ok;}
std::pair<Sci_Position,Sci_Position> printRange(bool selection){HWND h=editor();if(selection&&!sci(h,SCI_GETSELECTIONEMPTY))return {sci(h,SCI_GETSELECTIONSTART),sci(h,SCI_GETSELECTIONEND)};return {0,sci(h,SCI_GETLENGTH)};}
void printDialog(bool now){auto d=current();if(!d||d->busy==Busy::Loading)return;bool hasSelection=!sci(editor(),SCI_GETSELECTIONEMPTY);
    PRINTDLGW p{};p.lStructSize=sizeof(p);p.hwndOwner=window;
    if(now)p.Flags=PD_RETURNDEFAULT|PD_RETURNDC;else{p.hDevMode=printMode;p.hDevNames=printNames;p.Flags=PD_RETURNDC|PD_USEDEVMODECOPIESANDCOLLATE|PD_NOCURRENTPAGE|PD_ALLPAGES|(hasSelection?0:PD_NOSELECTION);p.nMinPage=1;p.nMaxPage=0xFFFF;p.nFromPage=1;p.nToPage=0xFFFF;}
    if(!PrintDlgW(&p)){if(CommDlgExtendedError())alert(L"No printer is available. Add one in Windows Settings, or use File > Export as PDF.");return;}
    if(now){if(p.hDevMode)GlobalFree(p.hDevMode);if(p.hDevNames)GlobalFree(p.hDevNames);}else{printMode=p.hDevMode;printNames=p.hDevNames;}
    if(!p.hDC)return;bool selection=!now&&(p.Flags&PD_SELECTION);int from=1,to=INT_MAX;if(!now&&(p.Flags&PD_PAGENUMS)){from=p.nFromPage;to=p.nToPage;}
    auto range=printRange(selection);tell(L"Printing…");bool ok=printTo(p.hDC,L"",range.first,range.second,from,to);DeleteDC(p.hDC);tell(ok?L"Sent to the printer.":L"Printing failed or was cancelled.");}
// PDF export prints to the "Microsoft Print to PDF" printer that ships with Windows, writing straight to the chosen file.
bool exportPdf(const std::wstring& path){HDC dc=CreateDCW(L"WINSPOOL",L"Microsoft Print to PDF",nullptr,nullptr);
    if(!dc){alert(L"PDF export uses the \"Microsoft Print to PDF\" printer, which is not installed.\n\nTurn it on under Windows Features, or add it in Settings > Bluetooth & devices > Printers & scanners.");return false;}
    auto range=printRange(false);DeleteFileW(path.c_str());bool ok=printTo(dc,path,range.first,range.second,1,INT_MAX);DeleteDC(dc);return ok;}
void exportPdfDialog(){auto d=current();if(!d||d->busy==Busy::Loading)return;std::wstring name=d->path.empty()?d->title:d->path;auto dot=name.find_last_of(L'.');auto slash=name.find_last_of(L"\\/");if(dot!=name.npos&&(slash==name.npos||dot>slash+1))name.resize(dot);name+=L".pdf";
    wchar_t path[32768]{};lstrcpynW(path,name.c_str(),32768);OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrTitle=L"Export as PDF";dialog.lpstrFilter=L"PDF document\0*.pdf\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"pdf";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(!GetSaveFileNameW(&dialog))return;std::wstring out=absolute(path);tell(L"Exporting PDF…");tell(exportPdf(out)?L"Exported "+basename(out):L"PDF export failed.");}
void cancelWork(){if(replacement){replacement->cancel=true;return;}auto d=current();if(d&&d->busy==Busy::Loading){for(auto& j:jobs)if(j->doc==d)j->cancel=true;tell(L"Cancelling open…");}else{findVisible=false;layout();if(editor())SetFocus(editor());}}
void completeJobs(){
    for(size_t i=0;i<jobs.size();){auto& job=jobs[i];if(!job->done){++i;continue;}auto d=job->doc;bool close=d->closeAfter;
        d->busy=Busy::None;sci(d->editor,SCI_SETREADONLY,0);
        if(job->load){
            if(job->loadedDocument){sci(d->editor,SCI_SETDOCPOINTER,0,(LPARAM)job->loadedDocument);sci(d->editor,SCI_RELEASEDOCUMENT,0,(LPARAM)job->loadedDocument);job->loadedDocument=nullptr;
                configure(d->editor);sci(d->editor,SCI_SETUNDOCOLLECTION,1);sci(d->editor,SCI_EMPTYUNDOBUFFER);sci(d->editor,SCI_SETSAVEPOINT);d->encoding=job->loaded.encoding;d->original=job->loaded.original;d->large=job->loaded.large;sci(d->editor,SCI_SETEOLMODE,job->loaded.eol);
                colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);if(current()==d){syncSplit();schedulePreview();}addRecent(d->path);tell(d->large?L"Opened in large-file mode; syntax highlighting is disabled above 16 MiB.":L"Opened "+basename(d->path));
            }else if(job->cancel){tell(L"Open cancelled.");d->path.clear();close=true;}
            else{d->path.clear();d->language=0;colorLanguage(d->editor,0,createLexer,dark);tell(L"Open failed: "+widen(job->error));closingAll=false;exitAfterClose=false;close=false;d->closeAfter=false;}
        }else if(job->error.empty()){
            d->path=job->path;d->original=job->saved;d->metaDirty=false;sci(d->editor,SCI_SETSAVEPOINT);if(d->automatic){d->language=detectLanguage(d->path);colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);}if(current()==d)syncSplit();addRecent(d->path);tell(L"Saved "+basename(d->path));
        }else{tell(L"Save failed: "+widen(job->error));close=false;d->closeAfter=false;closingAll=false;exitAfterClose=false;if(current()==d&&split)sci(secondary,SCI_SETREADONLY,0);}
        jobs.erase(jobs.begin()+i);if(close)removeDocument(d);
    }
    if(closingAll)closeAllStep();
}
void tick(){replaceTick();completeJobs();previewTick();if(!jobs.empty())refreshNeeded=true;if(refreshNeeded&&IsWindow(window))refresh();}

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
    addTip(tabControl,99,plusRect,L"New tab (Ctrl+N)");InvalidateRect(tabControl,nullptr,FALSE);layoutList(reveal);}
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
void tabMenu(int index,POINT pt,HWND owner=tabControl){selectDocument(index);auto d=current();HMENU m=CreatePopupMenu();
    AppendMenuW(m,MF_STRING,Close,L"Close\tCtrl+W");AppendMenuW(m,MF_STRING,CloseAll,L"Close all");AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(m,MF_STRING|(d&&!d->path.empty()?0:MF_GRAYED),CopyPath,L"Copy full path");AppendMenuW(m,MF_STRING|(d&&!d->path.empty()?0:MF_GRAYED),RevealPath,L"Open containing folder");
    popup(m,owner,{pt.x,pt.y,pt.x,pt.y},false);}
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
const Tool tools[]={{New,0xE8A5,L"New (Ctrl+N)"},{Open,0xE8E5,L"Open (Ctrl+O)"},{Save,0xE74E,L"Save (Ctrl+S)"},{SaveAll,0xE78C,L"Save all"},{Print,0xE749,L"Print (Ctrl+P)"},{0,0,nullptr},
    {Cut,0xE8C6,L"Cut (Ctrl+X)"},{Copy,0xE8C8,L"Copy (Ctrl+C)"},{Paste,0xE77F,L"Paste (Ctrl+V)"},{0,0,nullptr},{Undo,0xE7A7,L"Undo (Ctrl+Z)"},{Redo,0xE7A6,L"Redo (Ctrl+Y)"},{0,0,nullptr},
    {Find,0xE721,L"Find (Ctrl+F)"},{Replace,0xE8AC,L"Replace (Ctrl+H)"},{0,0,nullptr},{Wrap,0xE751,L"Word wrap (Alt+Z)"},{Split,0xE89A,L"Split editor (Ctrl+Alt+S)"},{DocumentList,0xE90C,L"Document list"},{DocumentMap,0xE90D,L"Document map"},{MarkdownPreview,0xE8A1,L"Markdown preview"},{0,0,nullptr},
    {ZoomIn,0xE8A3,L"Zoom in (Ctrl++)"},{ZoomOut,0xE71F,L"Zoom out (Ctrl+-)"},{Dark,0,L"Switch light / dark theme"}};
constexpr int toolCount=sizeof(tools)/sizeof(tools[0]);
RECT toolRects[toolCount];int toolHot=-1,toolDown=-1;std::wstring toolSignature;bool toolTipsAdded=false;
bool toolEnabled(int id){auto d=current();HWND h=editor();if(!d||!h)return false;bool idle=d->busy==Busy::None;
    switch(id){case Save:return idle;case Print:return d->busy!=Busy::Loading;case Undo:return idle&&sci(h,SCI_CANUNDO);case Redo:return idle&&sci(h,SCI_CANREDO);case Cut:return idle&&!sci(h,SCI_GETSELECTIONEMPTY);case Copy:return !sci(h,SCI_GETSELECTIONEMPTY);case Paste:return idle&&sci(h,SCI_CANPASTE);default:return true;}}
bool toolChecked(int id){return (id==Wrap&&wrap)||(id==Split&&split)||(id==DocumentList&&showList)||(id==DocumentMap&&showMap)||(id==MarkdownPreview&&showPreview);}
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

// Side panels, as in Notepad++: the Document List on the left and the Document Map on the right.
// Each has a title bar with a close button.
HWND docList=nullptr,docMap=nullptr,mapView=nullptr;struct PanelHead{bool hot=false,down=false;}listHead,mapHead;
int listScroll=0,listHot=-1,listCloseHot=-1,listCloseDown=-1;
int panelHeader(){return px(36);}
RECT panelClose(HWND h){RECT rc;GetClientRect(h,&rc);int s=px(28),cy=panelHeader()/2,right=h==docList?rc.right-1:rc.right;return {right-px(6)-s,cy-s/2,right-px(6),cy-s/2+s};}
void paintHeader(HDC dc,HWND h,const wchar_t* title,const PanelHead& head){RECT box=panelClose(h);drawText(dc,title,{px(14),0,box.left-px(4),panelHeader()},pal.text,uiFont);
    if(head.hot){Canvas c(dc);c.round(box,(float)px(4),head.down?pal.pressed(pal.chrome):pal.hover(pal.chrome));}drawGlyph(dc,0xE8BB,box,head.hot?pal.text:pal.textSecondary,tinyIcons);}
// Title-bar close button; returns true when the message belongs to it.
bool headerMouse(HWND h,UINT msg,LPARAM lp,PanelHead& head,int toggle){POINT p=mousePoint(lp);RECT box=panelClose(h);bool in=PtInRect(&box,p)!=0;
    switch(msg){case WM_MOUSEMOVE:if(in!=head.hot){head.hot=in;InvalidateRect(h,&box,FALSE);}return in;
    case WM_LBUTTONDOWN:if(in){head.down=true;SetCapture(h);InvalidateRect(h,&box,FALSE);}return in;
    case WM_LBUTTONUP:if(head.down){head.down=false;if(GetCapture()==h)ReleaseCapture();InvalidateRect(h,&box,FALSE);if(in)command(toggle);return true;}return false;}return false;}

// Document List: one row per tab, in tab order, with the folder under the name.
int listRow(){return px(44);}
int listTop(){return panelHeader()+px(2);}
RECT listRect(int i){RECT rc;GetClientRect(docList,&rc);int y=listTop()+i*listRow()-listScroll;return {px(6),y,rc.right-1-px(6),y+listRow()};}
RECT listCloseBox(const RECT& r){int s=px(24),cy=(r.top+r.bottom)/2;return {r.right-px(6)-s,cy-s/2,r.right-px(6),cy-s/2+s};}
int hitList(POINT p){if(p.y<listTop())return -1;for(int i=0;i<(int)documents.size();++i){RECT r=listRect(i);if(PtInRect(&r,p))return i;}return -1;}
void layoutList(bool reveal){if(!docList)return;RECT rc;GetClientRect(docList,&rc);int room=std::max(0,(int)rc.bottom-listTop()-px(4)),row=listRow();
    if(reveal&&active>=0){int top=active*row;if(top<listScroll)listScroll=top;if(top+row>listScroll+room)listScroll=top+row-room;}
    listScroll=std::clamp(listScroll,0,std::max(0,(int)documents.size()*row-room));InvalidateRect(docList,nullptr,FALSE);}
std::wstring folderOf(const std::shared_ptr<Document>& d){if(d->path.empty())return L"Not saved";auto p=d->path.find_last_of(L"\\/");return p==d->path.npos?d->path:d->path.substr(0,p);}
void paintList(HDC target){RECT rc;GetClientRect(docList,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.chrome);fill(b.dc,{rc.right-1,0,rc.right,rc.bottom},pal.divider);paintHeader(b.dc,docList,L"Document List",listHead);
    HRGN clip=CreateRectRgn(0,listTop(),rc.right-1,rc.bottom);SelectClipRgn(b.dc,clip);
    {Canvas c(b.dc);c.g.SetClip(Gdiplus::Rect(0,listTop(),rc.right-1,rc.bottom-listTop()));
        for(int i=0;i<(int)documents.size();++i){RECT r=listRect(i);if(r.bottom<listTop()||r.top>rc.bottom)continue;
        if(i==active||i==listHot)c.round(r,(float)px(4),i==active&&i==listHot?pal.hover(pal.hover(pal.chrome)):pal.hover(pal.chrome));
        if(i==active){int cy=(r.top+r.bottom)/2;c.round({r.left,cy-px(8),r.left+px(3),cy+px(8)},(float)px(1),pal.accent);}
        RECT box=listCloseBox(r);if(i==listCloseHot)c.round(box,(float)px(4),listCloseDown==i?pal.pressed(pal.hover(pal.chrome)):pal.hover(pal.hover(pal.chrome)));
        else if(dirty(documents[i])&&i!=listHot)c.dot((box.left+box.right)/2.0f,(box.top+box.bottom)/2.0f,(float)px(4),i==active?pal.text:pal.textSecondary);}}
    for(int i=0;i<(int)documents.size();++i){RECT r=listRect(i);if(r.bottom<listTop()||r.top>rc.bottom)continue;auto& d=documents[i];RECT box=listCloseBox(r);int mid=(r.top+r.bottom)/2;
        drawText(b.dc,tabTitle(d),{r.left+px(14),r.top+px(3),box.left-px(4),mid+px(1)},i==active?pal.text:pal.textSecondary,uiFont);
        drawText(b.dc,folderOf(d),{r.left+px(14),mid+px(1),box.left-px(4),r.bottom-px(4)},pal.textSecondary,statusFont,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_PATH_ELLIPSIS|DT_NOPREFIX);
        if(i==listHot)drawGlyph(b.dc,0xE8BB,box,i==listCloseHot?pal.text:pal.textSecondary,tinyIcons);}
    SelectClipRgn(b.dc,nullptr);DeleteObject(clip);}
LRESULT CALLBACK listProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintList(dc);EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:layoutList(false);return 0;
case WM_MOUSEMOVE:{POINT p=mousePoint(lp);bool head=headerMouse(h,msg,lp,listHead,DocumentList);int row=head?-1:hitList(p),close=-1;if(row>=0){RECT box=listCloseBox(listRect(row));if(PtInRect(&box,p))close=row;}
    if(row!=listHot||close!=listCloseHot){listHot=row;listCloseHot=close;InvalidateRect(h,nullptr,FALSE);}trackLeave(h);return 0;}
case WM_MOUSELEAVE:listHot=listCloseHot=-1;listHead.hot=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:{if(headerMouse(h,WM_LBUTTONDOWN,lp,listHead,DocumentList))return 0;int row=hitList(mousePoint(lp));if(row>=0&&row==listCloseHot){listCloseDown=row;SetCapture(h);InvalidateRect(h,nullptr,FALSE);}else if(row>=0)selectDocument(row);return 0;}
case WM_LBUTTONUP:{if(headerMouse(h,msg,lp,listHead,DocumentList))return 0;int down=listCloseDown;listCloseDown=-1;if(GetCapture()==h)ReleaseCapture();InvalidateRect(h,nullptr,FALSE);if(down>=0&&down==listCloseHot&&down<(int)documents.size())requestClose(documents[down]);return 0;}
case WM_CAPTURECHANGED:listCloseDown=-1;listHead.down=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_MBUTTONUP:{int row=hitList(mousePoint(lp));if(row>=0)requestClose(documents[row]);return 0;}
case WM_RBUTTONUP:{POINT p=mousePoint(lp);int row=hitList(p);if(row>=0)tabMenu(row,p,h);return 0;}
case WM_MOUSEWHEEL:listScroll-=GET_WHEEL_DELTA_WPARAM(wp)*listRow()*3/WHEEL_DELTA;layoutList(false);return 0;
}return DefWindowProcW(h,msg,wp,lp);}

// Document Map: a zoomed-out Scintilla view sharing the active document. Instead of the
// separate overlay window Notepad++ uses, the map's own selection, drawn translucently under
// the text, marks the lines the editor shows; the map never has focus, so it is the
// inactive selection colour.
void configureMap(){if(!mapView)return;auto d=current();paintEditor(mapView,pal,false,false,false,fontSize,dpi);if(d)colorLanguage(mapView,d->large?0:d->language,createLexer,dark,false);
    for(int m=0;m<5;++m)sci(mapView,SCI_SETMARGINWIDTHN,m,0);sci(mapView,SCI_SETMARGINLEFT,0,px(4));sci(mapView,SCI_SETMARGINRIGHT,0,0);
    sci(mapView,SCI_SETZOOM,-10);sci(mapView,SCI_SETEXTRAASCENT,0);sci(mapView,SCI_SETEXTRADESCENT,0);sci(mapView,SCI_SETCARETLINEVISIBLE,0);sci(mapView,SCI_SETCARETLINEVISIBLEALWAYS,0);sci(mapView,SCI_SETCARETSTYLE,CARETSTYLE_INVISIBLE);
    sci(mapView,SCI_SETHSCROLLBAR,0);sci(mapView,SCI_SETVSCROLLBAR,0);sci(mapView,SCI_SETINDENTATIONGUIDES,SC_IV_NONE);sci(mapView,SCI_SETSELEOLFILLED,1);sci(mapView,SCI_SETSELECTIONLAYER,SC_LAYER_UNDER_TEXT);
    for(int e:{SC_ELEMENT_SELECTION_BACK,SC_ELEMENT_SELECTION_INACTIVE_BACK})element(mapView,e,pal.text,pal.dark?0x30:0x22);
    for(int e:{SC_ELEMENT_SELECTION_TEXT,SC_ELEMENT_SELECTION_INACTIVE_TEXT})sci(mapView,SCI_RESETELEMENTCOLOUR,e);}
void syncMap(){if(!mapView)return;auto d=current();sptr_t doc=showMap&&d?sci(d->editor,SCI_GETDOCPOINTER):0;
    // A hidden map lets go of the document so that closed tabs are freed.
    if(!doc){if(GetPropW(mapView,L"NU.doc")){sci(mapView,SCI_SETDOCPOINTER,0,0);RemovePropW(mapView,L"NU.doc");}return;}
    if(sci(mapView,SCI_GETDOCPOINTER)!=doc)sci(mapView,SCI_SETDOCPOINTER,0,doc);SetPropW(mapView,L"NU.doc",(HANDLE)1);configureMap();updateMap();}
struct MapGeometry{Sci_Position total,mapLines,first,shown;double k;};
MapGeometry mapGeometry(HWND h){MapGeometry v{};v.total=sci(mapView,SCI_GETLINECOUNT);v.mapLines=sci(mapView,SCI_LINESONSCREEN);auto top=sci(h,SCI_GETFIRSTVISIBLELINE);
    v.first=sci(h,SCI_DOCLINEFROMVISIBLE,top);v.shown=std::max<Sci_Position>(1,sci(h,SCI_DOCLINEFROMVISIBLE,top+std::max<Sci_Position>(1,sci(h,SCI_LINESONSCREEN))-1)-v.first+1);
    // The map scrolls in proportion to the editor so that both reach the end together.
    v.k=v.total>v.mapLines?(double)(v.total-v.mapLines)/std::max<Sci_Position>(1,v.total-v.shown):0;return v;}
void updateMap(){HWND h=editor();if(!mapView||!showMap||!h||!GetPropW(mapView,L"NU.doc"))return;auto v=mapGeometry(h);
    auto last=v.first+v.shown;sci(mapView,SCI_SETSEL,sci(mapView,SCI_POSITIONFROMLINE,v.first),last<v.total?sci(mapView,SCI_POSITIONFROMLINE,last):sci(mapView,SCI_GETLENGTH));
    sci(mapView,SCI_SETFIRSTVISIBLELINE,std::clamp<Sci_Position>((Sci_Position)(v.first*std::min(v.k,1.0)+0.5),0,std::max<Sci_Position>(0,v.total-v.mapLines)));sci(mapView,SCI_SETXOFFSET,0);}
// Clicking or dragging on the map centres the editor on that point.
void mapScrollTo(int y){HWND h=editor();if(!h||!GetPropW(mapView,L"NU.doc"))return;auto v=mapGeometry(h);int lh=std::max(1,(int)sci(mapView,SCI_TEXTHEIGHT,0));RECT rc;GetClientRect(mapView,&rc);
    double target=v.k<0.999?((double)y/lh-v.shown/2.0)/(1-v.k):(double)y*v.total/std::max<LONG>(1,rc.bottom)-v.shown/2.0;
    auto line=std::clamp<Sci_Position>((Sci_Position)(target+0.5),0,std::max<Sci_Position>(0,v.total-1));sci(h,SCI_SETFIRSTVISIBLELINE,sci(h,SCI_VISIBLEFROMDOCLINE,line));updateMap();}
// The map never takes focus or edits; mouse input scrolls the editor instead.
LRESULT CALLBACK mapProc(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){switch(msg){
case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:SetCapture(h);mapScrollTo(mousePoint(lp).y);return 0;
case WM_MOUSEMOVE:if(GetCapture()==h)mapScrollTo(mousePoint(lp).y);return 0;
case WM_LBUTTONUP:if(GetCapture()==h)ReleaseCapture();return 0;
case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_RBUTTONDBLCLK:case WM_MBUTTONDOWN:case WM_MBUTTONUP:case WM_MBUTTONDBLCLK:case WM_CONTEXTMENU:case WM_KEYDOWN:case WM_KEYUP:case WM_CHAR:case WM_SYSCHAR:return 0;
case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:if(editor())SendMessageW(editor(),msg,wp,lp);return 0;
case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_ARROW));return TRUE;
case WM_SETFOCUS:if(editor())SetFocus(editor());return 0;
case WM_NCDESTROY:RemovePropW(h,L"NU.doc");RemoveWindowSubclass(h,mapProc,1);break;
}return DefSubclassProc(h,msg,wp,lp);}
void paintMapPanel(HDC target){RECT rc;GetClientRect(docMap,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.chrome);fill(b.dc,{0,0,1,rc.bottom},pal.divider);paintHeader(b.dc,docMap,L"Document Map",mapHead);}
LRESULT CALLBACK mapPanelProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paintMapPanel(dc);EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:{RECT rc;GetClientRect(h,&rc);if(mapView)MoveWindow(mapView,1,panelHeader(),std::max<int>(1,rc.right-1),std::max<int>(1,rc.bottom-panelHeader()),TRUE);updateMap();InvalidateRect(h,nullptr,FALSE);return 0;}
case WM_MOUSEMOVE:headerMouse(h,msg,lp,mapHead,DocumentMap);trackLeave(h);return 0;
case WM_LBUTTONDOWN:case WM_LBUTTONUP:headerMouse(h,msg,lp,mapHead,DocumentMap);return 0;
case WM_MOUSELEAVE:if(mapHead.hot){mapHead.hot=false;InvalidateRect(h,nullptr,FALSE);}return 0;
case WM_CAPTURECHANGED:mapHead.down=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_NOTIFY:return 0;
}return DefWindowProcW(h,msg,wp,lp);}

// Search Results: Find All lists the matching lines of one or all tabs in a read-only Scintilla
// view, as in Notepad++. Double-click or Enter on a line opens that match.
HWND resultsPanel=nullptr,resultsView=nullptr,previewPanel=nullptr,previewView=nullptr;PanelHead resultsHead,previewHead;
struct Hit{std::weak_ptr<Document> doc;Sci_Position start=0,end=0;};std::vector<Hit> hits;
const wchar_t* uiFace(){static const wchar_t* face=fontExists(L"Segoe UI Variable Text")?L"Segoe UI Variable Text":L"Segoe UI";return face;}
void configureResults(){if(!resultsView)return;HWND r=resultsView;paintEditor(r,pal,false,false,false,std::max(8,fontSize-1),dpi);for(int m=0;m<5;++m)sci(r,SCI_SETMARGINWIDTHN,m,0);sci(r,SCI_SETMARGINLEFT,0,px(10));
    sci(r,SCI_SETINDENTATIONGUIDES,SC_IV_NONE);sci(r,SCI_SETCARETLINEVISIBLEALWAYS,1);style(r,1,pal.text,true);style(r,2,pal.accent,true);style(r,3,pal.textSecondary);
    sci(r,SCI_INDICSETSTYLE,8,INDIC_FULLBOX);sci(r,SCI_INDICSETFORE,8,pal.accent);sci(r,SCI_INDICSETALPHA,8,pal.dark?90:60);sci(r,SCI_INDICSETOUTLINEALPHA,8,pal.dark?150:120);sci(r,SCI_INDICSETUNDER,8,1);sci(r,SCI_SETREADONLY,1);}
void findAll(bool all){auto query=utf8(getText(control(FindField)));if(query.empty()){showFind();return;}if(!resultsView)return;int flags=findFlags();bool regex=(flags&SCFIND_REGEXP)!=0;
    std::string out;std::vector<Hit> found(1);struct Run{size_t at,length;int style;};std::vector<Run> runs;std::vector<std::pair<size_t,size_t>> marks;
    size_t total=0,files=0,searched=0;bool capped=false,invalid=false;std::vector<std::shared_ptr<Document>> list;if(all)list=documents;else if(current())list.push_back(current());
    for(auto& d:list){if(d->busy==Busy::Loading)continue;HWND h=d->editor;auto length=sci(h,SCI_GETLENGTH);if(regex&&length>16*1024*1024)continue;++searched;
        std::string block;std::vector<Hit> lines;std::vector<Run> blockRuns;std::vector<std::pair<size_t,size_t>> blockMarks;size_t count=0;Sci_Position pos=0,lastLine=-1,lineAt=0,cut=0;size_t textAt=0,textLength=0;
        sci(h,SCI_SETSEARCHFLAGS,flags);
        while(pos<=length&&total+count<100000){sci(h,SCI_SETSTATUS,SC_STATUS_OK);sci(h,SCI_SETTARGETRANGE,pos,length);auto at=sci(h,SCI_SEARCHINTARGET,query.size(),(LPARAM)query.data());
            if(sci(h,SCI_GETSTATUS)!=SC_STATUS_OK){invalid=true;break;}if(at<0)break;auto end=sci(h,SCI_GETTARGETEND);++count;
            auto line=sci(h,SCI_LINEFROMPOSITION,at);
            if(line!=lastLine){lastLine=line;lineAt=sci(h,SCI_POSITIONFROMLINE,line);auto lineEnd=sci(h,SCI_GETLINEENDPOSITION,line);const char* p=(const char*)sci(h,SCI_GETRANGEPOINTER,lineAt,lineEnd-lineAt);std::string text(p?p:"",(size_t)(lineEnd-lineAt));
                size_t lead=text.find_first_not_of(" \t");if(lead==text.npos)lead=text.size();cut=(Sci_Position)lead;text=text.substr(lead);if(text.size()>1000){size_t n=1000;while(n>0&&((unsigned char)text[n]&0xC0)==0x80)--n;text.resize(n);}
                for(auto& c:text)if(c=='\0')c=' ';std::string label="    Line "+std::to_string(line+1)+": ";block+=label;blockRuns.push_back({block.size()-label.size(),label.size(),3});textAt=block.size();textLength=text.size();block+=text+"\n";lines.push_back({d,at,end});}
            Sci_Position a=at-lineAt-cut,b=std::min<Sci_Position>(end,sci(h,SCI_GETLINEENDPOSITION,line))-lineAt-cut;a=std::max<Sci_Position>(a,0);b=std::min<Sci_Position>(b,(Sci_Position)textLength);if(b>a)blockMarks.push_back({textAt+(size_t)a,(size_t)(b-a)});
            pos=end>at?end:sci(h,SCI_POSITIONAFTER,end);if(end==length)break;}
        if(invalid)break;if(total+count>=100000)capped=true;if(!count)continue;++files;total+=count;
        std::string name="  "+utf8(d->path.empty()?d->title:d->path)+" ("+std::to_string(count)+(count==1?" hit)\n":" hits)\n");size_t base=out.size();
        runs.push_back({base,name.size()-1,2});found.push_back({d,-1,-1});out+=name;base=out.size();out+=block;
        for(auto& r:blockRuns)runs.push_back({base+r.at,r.length,r.style});for(auto& m:blockMarks)marks.push_back({base+m.first,m.second});for(auto& l:lines)found.push_back(l);if(capped)break;}
    if(invalid){tell(L"Invalid or unsupported regular expression.");return;}
    std::string head="Search \""+query+"\" ("+std::to_string(total)+(total==1?" hit":" hits")+" in "+std::to_string(files)+(files==1?" file":" files")+" of "+std::to_string(searched)+" searched)"+(capped?" - stopped at 100,000 hits":"")+"\n";
    out=head+out;HWND r=resultsView;sci(r,SCI_SETREADONLY,0);sci(r,SCI_CLEARALL);sci(r,SCI_APPENDTEXT,out.size(),(LPARAM)out.data());
    sci(r,SCI_STARTSTYLING,0);sci(r,SCI_SETSTYLING,head.size()-1,1);for(auto& run:runs){sci(r,SCI_STARTSTYLING,head.size()+run.at);sci(r,SCI_SETSTYLING,run.length,run.style);}
    sci(r,SCI_SETINDICATORCURRENT,8);for(auto& m:marks)sci(r,SCI_INDICATORFILLRANGE,head.size()+m.first,m.second);
    sci(r,SCI_SETREADONLY,1);sci(r,SCI_EMPTYUNDOBUFFER);sci(r,SCI_GOTOPOS,0);hits=std::move(found);
    if(!showResults){showResults=true;layout();}tell(std::to_wstring(total)+(total==1?L" match":L" matches")+L" in "+std::to_wstring(files)+(files==1?L" document.":L" documents."));}
void openResult(Sci_Position line){if(line<=0||line>=(Sci_Position)hits.size())return;auto hit=hits[(size_t)line];auto d=hit.doc.lock();if(!d){tell(L"That document has been closed.");return;}
    auto it=std::find(documents.begin(),documents.end(),d);if(it==documents.end())return;selectDocument((int)(it-documents.begin()));if(hit.start<0)return;HWND h=d->editor;auto length=sci(h,SCI_GETLENGTH);
    sci(h,SCI_SETSEL,std::min(hit.start,length),std::min(hit.end,length));sci(h,SCI_ENSUREVISIBLE,sci(h,SCI_LINEFROMPOSITION,std::min(hit.start,length)));sci(h,SCI_SCROLLCARET);SetFocus(h);}
LRESULT CALLBACK resultsKeys(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR){
    if(msg==WM_KEYDOWN&&wp==VK_RETURN){PostMessageW(resultsPanel,WM_APP+1,(WPARAM)sci(h,SCI_LINEFROMPOSITION,sci(h,SCI_GETCURRENTPOS)),0);return 0;}if(msg==WM_CHAR&&wp=='\r')return 0;
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,resultsKeys,1);return DefSubclassProc(h,msg,wp,lp);}
LRESULT CALLBACK resultsProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);{RECT rc;GetClientRect(h,&rc);Buffer b(dc,rc);fill(b.dc,rc,pal.chrome);fill(b.dc,{0,0,rc.right,1},pal.divider);paintHeader(b.dc,h,L"Search Results",resultsHead);}EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:{RECT rc;GetClientRect(h,&rc);if(resultsView)MoveWindow(resultsView,0,panelHeader(),std::max<int>(1,rc.right),std::max<int>(1,rc.bottom-panelHeader()),TRUE);InvalidateRect(h,nullptr,FALSE);return 0;}
case WM_MOUSEMOVE:headerMouse(h,msg,lp,resultsHead,SearchResults);trackLeave(h);return 0;
case WM_LBUTTONDOWN:case WM_LBUTTONUP:headerMouse(h,msg,lp,resultsHead,SearchResults);return 0;
case WM_MOUSELEAVE:if(resultsHead.hot){resultsHead.hot=false;InvalidateRect(h,nullptr,FALSE);}return 0;
case WM_CAPTURECHANGED:resultsHead.down=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_NOTIFY:{auto n=(SCNotification*)lp;if(n->nmhdr.hwndFrom==resultsView&&n->nmhdr.code==SCN_DOUBLECLICK)PostMessageW(h,WM_APP+1,(WPARAM)n->line,0);return 0;}
case WM_APP+1:openResult((Sci_Position)wp);return 0;
}return DefWindowProcW(h,msg,wp,lp);}

// Markdown Preview: a RichEdit beside the editor shows the active Markdown document, converted
// to RTF by markdown.hpp. It re-renders shortly after each edit and follows the editor's scrolling.
std::vector<std::pair<std::string,std::string>> previewLinks;bool previewPending=false;ULONGLONG previewDue=0;
bool isMarkdown(const std::shared_ptr<Document>& d){return d&&!d->large&&strcmp(languages()[d->language].lexer,"markdown")==0;}
bool previewShown(){return showPreview&&previewView&&isMarkdown(current());}
void schedulePreview(int delay){previewPending=true;previewDue=GetTickCount64()+(ULONGLONG)std::max(0,delay);}
void syncPreview(){if(!previewShown())return;HWND h=current()->editor;auto total=sci(h,SCI_VISIBLEFROMDOCLINE,sci(h,SCI_GETLINECOUNT)),shown=sci(h,SCI_LINESONSCREEN),first=sci(h,SCI_GETFIRSTVISIBLELINE);
    double f=total>shown?std::clamp((double)first/(double)(total-shown),0.0,1.0):0;SCROLLINFO si{};si.cbSize=sizeof(si);si.fMask=SIF_RANGE|SIF_PAGE;if(!GetScrollInfo(previewView,SB_VERT,&si))return;
    int range=std::max(0,si.nMax-si.nMin+1-(int)si.nPage);POINT p{0,(int)(f*range+0.5)};SendMessageW(previewView,EM_SETSCROLLPOS,0,(LPARAM)&p);}
void renderPreview(){previewPending=false;if(!previewShown())return;auto d=current();if(d->busy==Busy::Loading)return;HWND h=d->editor;auto length=sci(h,SCI_GETLENGTH);std::string text;
    if(length>4*1024*1024)text="*This document is too large to preview (over 4 MiB).*";else{const char* p=(const char*)sci(h,SCI_GETCHARACTERPOINTER);text.assign(p?p:"",(size_t)length);}
    HDC dc=GetDC(previewView);int lx=std::max(1,GetDeviceCaps(dc,LOGPIXELSX));ReleaseDC(previewView,dc);RECT rc{};SendMessageW(previewView,EM_GETRECT,0,(LPARAM)&rc);
    MarkdownStyle st{pal.text,pal.textSecondary,pal.accent,pal.editorText,pal.chrome,pal.divider,pal.chrome,uiFace(),widen(editorFont()),std::max(1440,MulDiv(rc.right-rc.left-px(4),1440,lx))};
    auto rtf=markdownToRtf(text,st,&previewLinks);POINT scroll{};SendMessageW(previewView,EM_GETSCROLLPOS,0,(LPARAM)&scroll);SendMessageW(previewView,WM_SETREDRAW,FALSE,0);
    SETTEXTEX set{ST_DEFAULT,CP_ACP};SendMessageW(previewView,EM_SETTEXTEX,(WPARAM)&set,(LPARAM)rtf.c_str());SendMessageW(previewView,EM_SETSCROLLPOS,0,(LPARAM)&scroll);
    SendMessageW(previewView,WM_SETREDRAW,TRUE,0);InvalidateRect(previewView,nullptr,TRUE);}
void previewTick(){if(previewPending&&GetTickCount64()>=previewDue)renderPreview();}
// Web and mail links open in the browser; relative links to files open in a tab.
void previewLink(const ENLINK* link){if(link->chrg.cpMax<=link->chrg.cpMin)return;std::wstring text((size_t)(link->chrg.cpMax-link->chrg.cpMin)+1,L'\0');TEXTRANGEW range{link->chrg,&text[0]};
    SendMessageW(previewView,EM_GETTEXTRANGE,0,(LPARAM)&range);text.resize(wcslen(text.c_str()));std::string url=utf8(text);auto q=url.find("HYPERLINK \"");
    if(q!=url.npos){auto e=url.find('"',q+11);url=url.substr(q+11,e==url.npos?url.npos:e-q-11);}else for(auto& l:previewLinks)if(l.first==url||l.second==url){url=l.second;break;}
    std::wstring target=widen(url,CP_UTF8,0),key=lower(target);
    if(key.rfind(L"http://",0)==0||key.rfind(L"https://",0)==0||key.rfind(L"mailto:",0)==0){ShellExecuteW(window,L"open",target.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return;}
    auto hash=target.find(L'#');if(hash!=target.npos)target.resize(hash);if(target.empty())return;std::wstring decoded;
    for(size_t i=0;i<target.size();++i){if(target[i]==L'%'&&i+2<target.size()&&iswxdigit(target[i+1])&&iswxdigit(target[i+2])){decoded+=(wchar_t)wcstol(target.substr(i+1,2).c_str(),nullptr,16);i+=2;}else decoded+=target[i]==L'/'?L'\\':target[i];}
    auto d=current();std::wstring base=d&&!d->path.empty()?d->path.substr(0,d->path.find_last_of(L"\\/")+1):L"";
    std::wstring path=decoded.size()>1&&(decoded[1]==L':'||decoded[0]==L'\\')?decoded:base+decoded;if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES&&!(GetFileAttributesW(path.c_str())&FILE_ATTRIBUTE_DIRECTORY))openPath(path);else tell(L"Link target not found: "+decoded);}
LRESULT CALLBACK previewProc(HWND h,UINT msg,WPARAM wp,LPARAM lp){switch(msg){
case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);{RECT rc;GetClientRect(h,&rc);Buffer b(dc,rc);fill(b.dc,rc,pal.chrome);fill(b.dc,{0,0,1,rc.bottom},pal.divider);paintHeader(b.dc,h,L"Markdown Preview",previewHead);}EndPaint(h,&ps);return 0;}
case WM_ERASEBKGND:return 1;
case WM_SIZE:{RECT rc;GetClientRect(h,&rc);if(previewView){int w=std::max<int>(1,rc.right-1),hh=std::max<int>(1,rc.bottom-panelHeader());MoveWindow(previewView,1,panelHeader(),w,hh,TRUE);
    RECT pad{px(18),px(10),std::max(px(19),w-px(18)),std::max(px(11),hh-px(6))};SendMessageW(previewView,EM_SETRECT,0,(LPARAM)&pad);schedulePreview(60);}InvalidateRect(h,nullptr,FALSE);return 0;}
case WM_MOUSEMOVE:headerMouse(h,msg,lp,previewHead,MarkdownPreview);trackLeave(h);return 0;
case WM_LBUTTONDOWN:case WM_LBUTTONUP:headerMouse(h,msg,lp,previewHead,MarkdownPreview);return 0;
case WM_MOUSELEAVE:if(previewHead.hot){previewHead.hot=false;InvalidateRect(h,nullptr,FALSE);}return 0;
case WM_CAPTURECHANGED:previewHead.down=false;InvalidateRect(h,nullptr,FALSE);return 0;
case WM_NOTIFY:{auto link=(ENLINK*)lp;if(link->nmhdr.hwndFrom==previewView&&link->nmhdr.code==EN_LINK&&link->msg==WM_LBUTTONUP){previewLink(link);return 1;}return 0;}
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
    for(int id:{FindField,ReplaceField,NextButton,PreviousButton,ReplaceButton,ReplaceAllButton,MatchCase,WholeWord,Regex,HideFind,GoToText,GoToButton,FindAllButton,FindAllDocsButton})ShowWindow(control(id),findVisible?SW_SHOW:SW_HIDE);
    bool busy=replacement||(current()&&current()->busy==Busy::Loading);ShowWindow(control(CancelWork),findVisible&&busy?SW_SHOW:SW_HIDE);
    if(!findVisible)return;int s=px(32),gap=px(4),y1=top+px(10),y2=y1+s+px(6),field=std::clamp(width*2/5,px(220),px(460)),x=px(12);
    box(FindField,x,y1,field,s);x+=field+px(8);for(int id:{MatchCase,WholeWord,Regex}){place(id,x,y1,s,s);x+=s+gap;}x+=px(8);place(PreviousButton,x,y1,s,s);x+=s+gap;place(NextButton,x,y1,s,s);x+=s+px(8);place(FindAllButton,x,y1,px(76),s);x+=px(76)+gap;place(FindAllDocsButton,x,y1,px(116),s);
    int right=width-px(12)-s;place(HideFind,right,y1,s,s);place(CancelWork,right-px(8)-px(72),y1,px(72),s);
    x=px(12);box(ReplaceField,x,y2,field,s);x+=field+px(8);place(ReplaceButton,x,y2,px(88),s);x+=px(88)+px(6);place(ReplaceAllButton,x,y2,px(100),s);
    right=width-px(12);place(GoToButton,right-px(56),y2,px(56),s);box(GoToText,right-px(56)-px(6)-px(132),y2,px(132),s);}
void paintMain(HDC target){RECT rc;GetClientRect(window,&rc);Buffer b(target,rc);fill(b.dc,rc,pal.surface);
    if(split&&current())fill(b.dc,splitter,pal.divider);
    if(findVisible){fill(b.dc,{0,findTop,rc.right,findTop+1},pal.divider);Canvas c(b.dc);float r=(float)px(4);
        for(auto& bx:boxes){bool focused=GetFocus()==control(bx.id);c.round(bx.rc,r,focused?pal.fieldFocus:pal.field);c.outline(bx.rc,r,pal.fieldBorder);c.bottomEdge(bx.rc,r,focused?pal.accent:pal.fieldBottom,focused?px(2):1);}}}

void refresh(){refreshNeeded=false;auto d=current();if(!d)return;
    static bool previewWas=false;if(previewShown()!=previewWas){previewWas=!previewWas;layout();if(previewWas)schedulePreview();}
    std::wstring tabs;for(auto& doc:documents)tabs+=tabTitle(doc)+(dirty(doc)?L"*":L"")+doc->path+L"\n";tabs+=std::to_wstring(active);if(tabs!=tabSignature){tabSignature=tabs;layoutTabs();}
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
    for(auto pair:{std::pair<int,bool>{Wrap,wrap},{Whitespace,whitespace},{LineNumbers,lineNumbers},{Split,split},{ShowToolbar,showToolbar},{ShowStatus,showStatus},{DocumentMap,showMap},{DocumentList,showList},{MarkdownPreview,showPreview},{SearchResults,showResults}})checkMenu(pair.first,pair.second);
    CheckMenuRadioItem(GetMenu(window),ThemeSystem,ThemeDark,ThemeSystem+(int)theme,MF_BYCOMMAND);markMenu(GetMenu(window));
    EnableMenuItem(GetMenu(window),Save,MF_BYCOMMAND|(d->busy==Busy::None?MF_ENABLED:MF_GRAYED));EnableMenuItem(GetMenu(window),Undo,MF_BYCOMMAND|(d->busy==Busy::None&&sci(h,SCI_CANUNDO)?MF_ENABLED:MF_GRAYED));EnableMenuItem(GetMenu(window),Redo,MF_BYCOMMAND|(d->busy==Busy::None&&sci(h,SCI_CANREDO)?MF_ENABLED:MF_GRAYED));
    EnableWindow(control(ReplaceButton),d->busy==Busy::None);EnableWindow(control(ReplaceAllButton),d->busy==Busy::None);
}
HMENU menu(){HMENU bar=CreateMenu();auto popup=[&](const wchar_t* name,HMENU m=nullptr){if(!m)m=CreatePopupMenu();AppendMenuW(bar,MF_POPUP,(UINT_PTR)m,name);return m;};auto item=[](HMENU m,int id,const wchar_t* name){AppendMenuW(m,MF_STRING,id,name);};auto separator=[](HMENU m){AppendMenuW(m,MF_SEPARATOR,0,nullptr);};
    fileMenu=popup(L"&File");fillFileMenu();
    HMENU e=popup(L"&Edit");item(e,Undo,L"&Undo\tCtrl+Z");item(e,Redo,L"&Redo\tCtrl+Y");separator(e);item(e,Cut,L"Cu&t\tCtrl+X");item(e,Copy,L"&Copy\tCtrl+C");item(e,Paste,L"&Paste\tCtrl+V");item(e,SelectAll,L"Select &all\tCtrl+A");separator(e);item(e,Duplicate,L"&Duplicate line / selection\tCtrl+D");item(e,DeleteLine,L"Delete &line\tCtrl+Shift+L");item(e,Upper,L"UPPERCASE selection\tCtrl+Shift+U");item(e,Lower,L"lowercase selection\tCtrl+U");item(e,Indent,L"&Indent\tTab");item(e,Unindent,L"U&nindent\tShift+Tab");separator(e);AppendMenuW(e,MF_POPUP,(UINT_PTR)eolMenu(),L"Line &endings");
    HMENU lines=CreatePopupMenu();AppendMenuW(e,MF_POPUP,(UINT_PTR)lines,L"Line &operations");
    item(lines,LineDuplicate,L"D&uplicate current line");item(lines,RemoveDuplicates,L"Remove duplicate lines");item(lines,RemoveConsecutive,L"Remove consecutive duplicate lines");item(lines,SplitLines,L"Split lines\tCtrl+I");item(lines,JoinLines,L"Join lines\tCtrl+J");
    item(lines,MoveLineUp,L"Move up current line\tCtrl+Shift+Up");item(lines,MoveLineDown,L"Move down current line\tCtrl+Shift+Down");item(lines,RemoveEmpty,L"Remove empty lines");item(lines,RemoveBlank,L"Remove empty lines (containing blank characters)");
    item(lines,InsertAbove,L"Insert blank line above current\tCtrl+Alt+Enter");item(lines,InsertBelow,L"Insert blank line below current\tCtrl+Alt+Shift+Enter");separator(lines);item(lines,ReverseLines,L"Reverse line order");item(lines,ShuffleLines,L"Randomize line order");
    static const wchar_t* sorts[]={L"lexicographically",L"lexicographically ignoring case",L"in locale order",L"as integers",L"as decimals (comma)",L"as decimals (dot)",L"by length"};
    for(int descending=0;descending<2;++descending){separator(lines);for(int k=0;k<7;++k)item(lines,SortBase+k*2+descending,(std::wstring(L"Sort lines ")+sorts[k]+(descending?L" descending":L" ascending")).c_str());}
    HMENU s=popup(L"&Search");item(s,Find,L"&Find…\tCtrl+F");item(s,Replace,L"&Replace…\tCtrl+H");item(s,FindNext,L"Find &next\tF3");item(s,FindPrevious,L"Find &previous\tShift+F3");item(s,GoTo,L"&Go to line…\tCtrl+G");separator(s);item(s,FindAllCurrent,L"Find &all in current document");item(s,FindAllOpen,L"Find all in all &open documents");separator(s);item(s,Bookmark,L"Toggle &bookmark\tCtrl+F2");item(s,NextBookmark,L"Next boo&kmark\tF2");item(s,Complete,L"&Complete keyword\tCtrl+Space");
    HMENU v=popup(L"&View");HMENU themes=CreatePopupMenu();item(themes,ThemeSystem,L"Use &system setting");item(themes,ThemeLight,L"&Light");item(themes,ThemeDark,L"&Dark");AppendMenuW(v,MF_POPUP,(UINT_PTR)themes,L"&Theme");
    item(v,ShowToolbar,L"T&oolbar");item(v,ShowStatus,L"Status &bar");separator(v);item(v,Wrap,L"&Word wrap\tAlt+Z");item(v,Whitespace,L"Show w&hitespace");item(v,LineNumbers,L"&Line numbers");item(v,Split,L"S&plit editor\tCtrl+Alt+S");separator(v);item(v,DocumentList,L"Document &list");item(v,DocumentMap,L"Document &map");item(v,MarkdownPreview,L"Mar&kdown preview");item(v,SearchResults,L"Search res&ults\tF7");separator(v);item(v,ZoomIn,L"Zoom &in\tCtrl++");item(v,ZoomOut,L"Zoom &out\tCtrl+-");item(v,ZoomReset,L"&Reset zoom\tCtrl+0");separator(v);item(v,FoldAll,L"&Fold all");item(v,ExpandAll,L"&Expand all");
    popup(L"E&ncoding",encodingMenu());popup(L"&Language",languageMenu());
    HMENU help=popup(L"&Help");item(help,About,L"&About NoteUltra");return bar;
}
void layout(){if(!window||!tabControl)return;RECT client;GetClientRect(window,&client);int w=client.right,h=client.bottom,y=0;
    MoveWindow(tabControl,0,y,w,px(40),TRUE);y+=px(40);
    ShowWindow(toolBar,showToolbar?SW_SHOW:SW_HIDE);if(showToolbar){MoveWindow(toolBar,0,y,w,px(40)+1,TRUE);y+=px(40)+1;}
    int statusHeight=showStatus?px(30):0;ShowWindow(statusBar,showStatus?SW_SHOW:SW_HIDE);if(showStatus)MoveWindow(statusBar,0,h-statusHeight,w,statusHeight,TRUE);
    int results=showResults?std::min(px(220),std::max(px(100),(h-statusHeight-y)/3)):0;ShowWindow(resultsPanel,showResults?SW_SHOW:SW_HIDE);if(showResults)MoveWindow(resultsPanel,0,h-statusHeight-results,w,results,TRUE);
    int panel=findVisible?px(90):0,bottom=std::max(y+1,h-statusHeight-results-panel);
    int left=showList?std::min(px(240),w/3):0,right=showMap?std::min(px(170),w/4):0,x=left,width=std::max(1,w-left-right);
    ShowWindow(docList,showList?SW_SHOW:SW_HIDE);if(showList)MoveWindow(docList,0,y,left,bottom-y,TRUE);ShowWindow(docMap,showMap?SW_SHOW:SW_HIDE);if(showMap)MoveWindow(docMap,w-right,y,right,bottom-y,TRUE);
    bool preview=previewShown();int previewWidth=preview?std::max(px(160),width/2):0;if(preview&&width-previewWidth<px(160))previewWidth=std::max(1,width/2);ShowWindow(previewPanel,preview?SW_SHOW:SW_HIDE);if(preview){MoveWindow(previewPanel,x+width-previewWidth,y,previewWidth,bottom-y,TRUE);width=std::max(1,width-previewWidth);}
    auto d=current();if(d){if(split){int half=(width-1)/2;MoveWindow(d->editor,x,y,half,bottom-y,TRUE);splitter={x+half,y,x+half+1,bottom};MoveWindow(secondary,x+half+1,y,width-half-1,bottom-y,TRUE);}else MoveWindow(d->editor,x,y,width,bottom-y,TRUE);}
    layoutFind(bottom,w);updateMap();InvalidateRect(window,nullptr,FALSE);
}
void createFonts(){for(HFONT f:{uiFont,statusFont,iconFont,smallIcons,tinyIcons})if(f)DeleteObject(f);
    const wchar_t* face=uiFace();static const wchar_t* icons=fontExists(L"Segoe Fluent Icons")?L"Segoe Fluent Icons":L"Segoe MDL2 Assets";
    uiFont=makeFont(face,px(14));statusFont=makeFont(face,px(12));iconFont=makeFont(icons,px(16));smallIcons=makeFont(icons,px(12));tinyIcons=makeFont(icons,px(10));
    HDC dc=GetDC(window);HGDIOBJ old=SelectObject(dc,uiFont);TEXTMETRICW tm{};GetTextMetricsW(dc,&tm);SelectObject(dc,old);ReleaseDC(window,dc);fieldTextHeight=tm.tmHeight;
    for(int id:{FindField,ReplaceField,GoToText,NextButton,PreviousButton,ReplaceButton,ReplaceAllButton,MatchCase,WholeWord,Regex,HideFind,GoToButton,CancelWork,FindAllButton,FindAllDocsButton})if(control(id))SendMessageW(control(id),WM_SETFONT,(WPARAM)uiFont,TRUE);
    if(tooltip)SendMessageW(tooltip,WM_SETFONT,(WPARAM)statusFont,TRUE);}
void applyAppearance(){dark=theme==Theme::Dark||(theme==Theme::System&&systemUsesDark());pal=makePalette(dark);
    setAppDark(dark);themeFrame(window,pal);themeControl(tooltip,dark);
    for(HBRUSH b:{fieldBrush,focusBrush})if(b)DeleteObject(b);fieldBrush=CreateSolidBrush(pal.field);focusBrush=CreateSolidBrush(pal.fieldFocus);
    for(int id:{FindField,ReplaceField,GoToText})themeControl(control(id),dark,L"DarkMode_CFD");
    for(auto d:documents){themeControl(d->editor,dark);paintEditor(d->editor,pal,lineNumbers,whitespace,wrap,fontSize,dpi);colorLanguage(d->editor,d->large?0:d->language,createLexer,dark,false);}
    configureMap();if(secondary){themeControl(secondary,dark);paintEditor(secondary,pal,lineNumbers,whitespace,wrap,fontSize,dpi);if(current())colorLanguage(secondary,current()->large?0:current()->language,createLexer,dark,false);}
    configureResults();themeControl(resultsView,dark);if(previewView){themeControl(previewView,dark);SendMessageW(previewView,EM_SETBKGNDCOLOR,0,pal.surface);schedulePreview();}
    toolSignature.clear();statusSignature.clear();tabSignature.clear();layoutTools();
    DrawMenuBar(window);RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);refreshNeeded=true;
}
void copyPath(){auto d=current();if(!d||d->path.empty()||!OpenClipboard(window))return;EmptyClipboard();size_t bytes=(d->path.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(memory){memcpy(GlobalLock(memory),d->path.c_str(),bytes);GlobalUnlock(memory);if(!SetClipboardData(CF_UNICODETEXT,memory))GlobalFree(memory);}CloseClipboard();tell(L"Path copied.");}
void command(int id){auto d=current();HWND h=editor();
    if(id>=LanguageBase&&id<LanguageBase+(int)languages().size()){if(d&&d->busy==Busy::None){d->language=id-LanguageBase;d->automatic=false;colorLanguage(d->editor,d->large?0:d->language,createLexer,dark);if(split)colorLanguage(secondary,d->large?0:d->language,createLexer,dark,false);refreshNeeded=true;}return;}
    if(id>=EncodingBase&&id<EncodingBase+5){if(d&&d->busy==Busy::None&&d->encoding!=(Encoding)(id-EncodingBase)){d->encoding=(Encoding)(id-EncodingBase);d->metaDirty=true;tell(L"Encoding will be applied when you save.");}return;}
    if(id>=EolBase&&id<EolBase+3){if(d&&d->busy==Busy::None){sci(h,SCI_BEGINUNDOACTION);sci(h,SCI_CONVERTEOLS,id-EolBase);sci(h,SCI_SETEOLMODE,id-EolBase);sci(h,SCI_ENDUNDOACTION);tell(L"Line endings converted.");}return;}
    if(id>=RecentBase&&id<RecentBase+10){openRecent((size_t)(id-RecentBase));refreshNeeded=true;return;}
    if(id>=LineDuplicate&&id<SortBase+14){lineCommand(id);refreshNeeded=true;return;}
    switch(id){
    case New:newDocument();break;case Open:openDialog();break;case Save:saveDocument(d);break;case SaveAs:saveDocument(d,true);break;
    case SaveAll:{auto copy=documents;for(auto doc:copy)if(doc->busy==Busy::None&&dirty(doc))saveDocument(doc);break;}
    case Close:requestClose(d);break;case CloseAll:closingAll=true;exitAfterClose=false;closeAllStep();break;case Exit:SendMessageW(window,WM_CLOSE,0,0);break;
    case Print:printDialog(false);break;case PrintNow:printDialog(true);break;case ExportPdf:exportPdfDialog();break;
    case RestoreClosed:restoreClosed();break;case OpenAllRecent:{auto copy=recent;for(auto it=copy.rbegin();it!=copy.rend();++it)if(exists(*it))openPath(*it);break;}case ClearRecent:recent.clear();saveRecent();tell(L"Recent files list emptied.");break;
    case FindAllCurrent:case FindAllButton:findAll(false);break;case FindAllOpen:case FindAllDocsButton:findAll(true);break;
    case SearchResults:showResults=!showResults;layout();if(showResults&&resultsView)SetFocus(resultsView);else if(h)SetFocus(h);break;
    case MarkdownPreview:showPreview=!showPreview;layout();schedulePreview();if(showPreview&&!isMarkdown(d))tell(L"The preview appears beside Markdown documents.");break;
    case CopyPath:copyPath();break;case RevealPath:if(d&&!d->path.empty())ShellExecuteW(window,nullptr,L"explorer.exe",(L"/select,\""+d->path+L"\"").c_str(),nullptr,SW_SHOWNORMAL);break;
    case Find:showFind();break;case Replace:showFind(true);break;case FindNext:case NextButton:findNext();break;case FindPrevious:case PreviousButton:findNext(true);break;case ReplaceButton:replaceOne();break;case ReplaceAllButton:beginReplaceAll();break;
    case MatchCase:case WholeWord:case Regex:SendMessageW(control(id),BM_SETCHECK,checked(id)?BST_UNCHECKED:BST_CHECKED,0);lastZero=-1;break;
    case GoTo:showFind(false,true);break;case GoToButton:goToLine();break;case HideFind:findVisible=false;layout();if(h)SetFocus(h);break;case CancelWork:cancelWork();break;
    case Bookmark:toggleBookmark();break;case NextBookmark:nextBookmark();break;case Complete:completeWord();break;
    case Dark:theme=dark?Theme::Light:Theme::Dark;applyAppearance();break;case ThemeSystem:case ThemeLight:case ThemeDark:theme=(Theme)(id-ThemeSystem);applyAppearance();break;
    case Wrap:wrap=!wrap;applyAppearance();break;case Whitespace:whitespace=!whitespace;applyAppearance();break;case LineNumbers:lineNumbers=!lineNumbers;applyAppearance();break;
    case ShowToolbar:showToolbar=!showToolbar;layout();break;case ShowStatus:showStatus=!showStatus;layout();break;
    case DocumentMap:showMap=!showMap;syncMap();layout();break;case DocumentList:showList=!showList;layout();layoutList();break;
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
        int legacy=get(L"Dark",-1),saved=get(L"Theme",legacy<0?0:(legacy?2:1));theme=(Theme)std::max(0,std::min(2,saved));wrap=get(L"Wrap",0)!=0;showToolbar=get(L"Toolbar",1)!=0;showStatus=get(L"StatusBar",1)!=0;fontSize=std::max(8,std::min(32,get(L"FontSize",11)));showMap=get(L"DocumentMap",0)!=0;showList=get(L"DocumentList",0)!=0;showPreview=get(L"MarkdownPreview",0)!=0;
        for(int i=1;i<=10;++i){wchar_t path[32768]{};GetPrivateProfileStringW(L"Recent",(L"File"+std::to_wstring(i)).c_str(),L"",path,32768,settingsPath.c_str());if(*path)recent.push_back(path);}}}
void saveSettings(){if(testMode||settingsPath.empty())return;auto set=[](const wchar_t* key,int value){WritePrivateProfileStringW(L"View",key,std::to_wstring(value).c_str(),settingsPath.c_str());};
    set(L"Theme",(int)theme);set(L"Wrap",wrap);set(L"Toolbar",showToolbar);set(L"StatusBar",showStatus);set(L"FontSize",fontSize);set(L"DocumentMap",showMap);set(L"DocumentList",showList);set(L"MarkdownPreview",showPreview);}
void buildControls(){dpi=dpiOf(window);dark=theme==Theme::Dark||(theme==Theme::System&&systemUsesDark());pal=makePalette(dark);createFonts();
    tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,0,0,0,0,window,nullptr,instance,nullptr);SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,px(600));SendMessageW(tooltip,WM_SETFONT,(WPARAM)statusFont,TRUE);
    tabControl=add(L"NoteUltra.Tabs",L"Documents",0,Tabs);toolBar=add(L"NoteUltra.Toolbar",L"Commands",0,Toolbar);statusBar=add(L"NoteUltra.Status",L"Status",0,Status);
    docList=add(L"NoteUltra.DocList",L"Document List",0,ListPanel);docMap=add(L"NoteUltra.Map",L"Document Map",WS_CLIPCHILDREN,MapPanel);
    mapView=CreateWindowExW(0,L"Scintilla",L"",WS_CHILD|WS_VISIBLE,0,0,10,10,docMap,nullptr,instance,nullptr);if(mapView){RevokeDragDrop(mapView);SetWindowSubclass(mapView,mapProc,1,0);sci(mapView,SCI_SETTECHNOLOGY,SC_TECHNOLOGY_DIRECTWRITE);sci(mapView,SCI_SETCODEPAGE,SC_CP_UTF8);}
    resultsPanel=add(L"NoteUltra.Results",L"Search Results",WS_CLIPCHILDREN,ResultsPanel);previewPanel=add(L"NoteUltra.Preview",L"Markdown Preview",WS_CLIPCHILDREN,PreviewPanel);
    resultsView=CreateWindowExW(0,L"Scintilla",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,10,10,resultsPanel,nullptr,instance,nullptr);
    if(resultsView){RevokeDragDrop(resultsView);SetWindowSubclass(resultsView,resultsKeys,1,0);sci(resultsView,SCI_SETTECHNOLOGY,SC_TECHNOLOGY_DIRECTWRITE);sci(resultsView,SCI_SETCODEPAGE,SC_CP_UTF8);sci(resultsView,SCI_SETUNDOCOLLECTION,0);sci(resultsView,SCI_SETSCROLLWIDTHTRACKING,1);sci(resultsView,SCI_SETSCROLLWIDTH,1);}
    if(LoadLibraryExW(L"msftedit.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32))previewView=CreateWindowExW(0,L"RICHEDIT50W",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_NOHIDESEL,0,0,10,10,previewPanel,nullptr,instance,nullptr);
    if(previewView)SendMessageW(previewView,EM_SETEVENTMASK,0,ENM_LINK);
    auto field=[](int id,const wchar_t* cue,DWORD style){HWND e=add(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|style,id);SendMessageW(e,EM_SETCUEBANNER,TRUE,(LPARAM)cue);SendMessageW(e,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);return e;};
    field(FindField,L"Find",0);field(ReplaceField,L"Replace",0);field(GoToText,L"Go to line",ES_NUMBER);
    SendMessageW(control(FindField),EM_SETLIMITTEXT,8192,0);SendMessageW(control(ReplaceField),EM_SETLIMITTEXT,8192,0);SendMessageW(control(GoToText),EM_SETLIMITTEXT,12,0);
    struct Named{int id;const wchar_t* name;const wchar_t* tip;};
    for(auto b:{Named{MatchCase,L"Match case",L"Match case (Alt+C)"},{WholeWord,L"Whole word",L"Match whole word (Alt+W)"},{Regex,L"Regular expression",L"Use regular expression (Alt+R)"},{PreviousButton,L"Previous match",L"Previous match (Shift+F3)"},{NextButton,L"Next match",L"Next match (F3)"},
        {HideFind,L"Close",L"Close (Escape)"},{ReplaceButton,L"Replace",nullptr},{ReplaceAllButton,L"Replace all",nullptr},{GoToButton,L"Go",nullptr},{CancelWork,L"Stop",L"Stop the current operation (Escape)"},
        {FindAllButton,L"Find all",L"List every match in this document"},{FindAllDocsButton,L"Find in all tabs",L"List every match in all open documents"}}){
        HWND button=add(L"BUTTON",b.name,WS_TABSTOP|BS_OWNERDRAW,b.id);SetWindowSubclass(button,buttonProc,1,0);if(b.tip)hwndTip(button,b.tip);}
    secondary=createEditor();newDocument();applyAppearance();layout();DragAcceptFiles(window,TRUE);SetTimer(window,1,40,nullptr);
}
void editorNotification(SCNotification* n){
    HWND source=(HWND)n->nmhdr.hwndFrom;auto d=current();if(!d)return;
    if(n->nmhdr.code==SCN_FOCUSIN)updateMap();
    if(n->nmhdr.code==SCN_UPDATEUI){refreshNeeded=true;if(source==editor()){if(n->updated&(SC_UPDATE_V_SCROLL|SC_UPDATE_CONTENT))updateMap();if(n->updated&SC_UPDATE_V_SCROLL)syncPreview();
        auto pos=sci(source,SCI_GETCURRENTPOS);Sci_Position brace=-1;
        for(auto candidate:{pos-1,pos})if(candidate>=0){int c=(int)sci(source,SCI_GETCHARAT,candidate);if(c&&strchr("[]{}()",c)){brace=candidate;break;}}
        if(brace>=0){auto other=sci(source,SCI_BRACEMATCH,brace,0);if(other>=0)sci(source,SCI_BRACEHIGHLIGHT,brace,other);else sci(source,SCI_BRACEBADLIGHT,brace);}else sci(source,SCI_BRACEHIGHLIGHT,(WPARAM)-1,-1);
    }}
    if(n->nmhdr.code==SCN_SAVEPOINTLEFT||n->nmhdr.code==SCN_SAVEPOINTREACHED||n->nmhdr.code==SCN_MODIFIED)refreshNeeded=true;
    if(n->nmhdr.code==SCN_MODIFIED&&(n->modificationType&(SC_MOD_INSERTTEXT|SC_MOD_DELETETEXT))&&(source==d->editor||source==secondary)&&showPreview&&isMarkdown(d))schedulePreview(200);
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
case WM_INITMENUPOPUP:if((HMENU)wp==fileMenu)fillFileMenu();break;
case WM_SETFOCUS:if(editor())SetFocus(editor());return 0;
case WM_CLOSE:closingAll=true;exitAfterClose=true;closeAllStep();return 0;
case WM_DESTROY:saveSettings();KillTimer(h,1);PostQuitMessage(0);return 0;
}return DefWindowProcW(h,msg,wp,lp);}
bool shortcut(MSG& msg){if(msg.message!=WM_KEYDOWN&&msg.message!=WM_SYSKEYDOWN)return false;bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0,alt=(GetKeyState(VK_MENU)&0x8000)!=0;int key=(int)msg.wParam,cmd=0;
    if(key==VK_ESCAPE){cancelWork();return true;}
    if(ctrl){if(key==VK_TAB){if(!documents.empty())selectDocument((active+(shift?(int)documents.size()-1:1))%(int)documents.size());return true;}
        if(key=='N')cmd=New;else if(key=='O')cmd=Open;else if(key=='S')cmd=alt?Split:(shift?SaveAs:Save);else if(key=='W')cmd=Close;else if(key=='F')cmd=Find;else if(key=='H')cmd=Replace;else if(key=='G')cmd=GoTo;
        else if(key=='P'&&!shift&&!alt)cmd=Print;else if(key=='T'&&shift&&!alt)cmd=RestoreClosed;
        else if(key==VK_F2)cmd=Bookmark;else if(key==VK_SPACE)cmd=Complete;else if(key==VK_OEM_PLUS||key==VK_ADD)cmd=ZoomIn;else if(key==VK_OEM_MINUS||key==VK_SUBTRACT)cmd=ZoomOut;else if(key=='0')cmd=ZoomReset;
        else if(GetFocus()==editor()){if(key=='D')cmd=Duplicate;else if(key=='L'&&shift)cmd=DeleteLine;else if(key=='U')cmd=shift?Upper:Lower;else if(key=='I'&&!shift&&!alt)cmd=SplitLines;else if(key=='J'&&!shift&&!alt)cmd=JoinLines;
            else if(shift&&!alt&&key==VK_UP)cmd=MoveLineUp;else if(shift&&!alt&&key==VK_DOWN)cmd=MoveLineDown;else if(alt&&key==VK_RETURN)cmd=shift?InsertBelow:InsertAbove;}
    }
    if(key==VK_F3)cmd=shift?FindPrevious:FindNext;if(key==VK_F2&&!ctrl)cmd=NextBookmark;if(alt&&!ctrl&&key=='Z')cmd=Wrap;if(key==VK_F7&&!ctrl&&!alt)cmd=SearchResults;
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
    registerClass(L"NoteUltra.Tabs",tabsProc,CS_DBLCLKS);registerClass(L"NoteUltra.Toolbar",toolsProc,CS_DBLCLKS);registerClass(L"NoteUltra.Status",statusProc);registerClass(L"NoteUltra.DocList",listProc,CS_DBLCLKS);registerClass(L"NoteUltra.Map",mapPanelProc);registerClass(L"NoteUltra.Results",resultsProc);registerClass(L"NoteUltra.Preview",previewProc);
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
            HWND focus=GetFocus();bool inEditor=focus==secondary||focus==resultsView;for(auto d:documents)if(focus==d->editor){inEditor=true;break;}
            if(inEditor||!IsDialogMessageW(h,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
        }code=(int)message.wParam;}
    stopIO();LocalFree(argv);FreeLibrary(lexillaDLL);FreeLibrary(scintillaDLL);for(HFONT f:{uiFont,statusFont,iconFont,smallIcons,tinyIcons})DeleteObject(f);for(HBRUSH b:{fieldBrush,focusBrush})DeleteObject(b);for(HGLOBAL g:{printMode,printNames})if(g)GlobalFree(g);Gdiplus::GdiplusShutdown(gdiplusToken);CoUninitialize();return code;
}
