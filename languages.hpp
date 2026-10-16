#pragma once
#include "theme.hpp"
#include "vendor/scintilla/include/Scintilla.h"
#include "vendor/scintilla/include/ILexer.h"
#include "vendor/lexilla/include/Lexilla.h"
#include "vendor/lexilla/include/SciLexer.h"
namespace nu {
struct Language {const wchar_t* title;const char* lexer;const wchar_t* extensions;const char* keywords;};
inline const std::vector<Language>& languages(){static const std::vector<Language> all={
 {L"Plain text","null",L".txt;.log;.csv;", ""},
 {L"C / C++","cpp",L".c;.cc;.cpp;.cxx;.h;.hh;.hpp;", "alignas alignof asm auto bool break case catch char class concept const constexpr consteval constinit continue co_await co_return co_yield decltype default delete do double else enum explicit export extern false float for friend if inline int long namespace new noexcept nullptr operator private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while"},
 {L"JavaScript / TypeScript","cpp",L".js;.jsx;.mjs;.cjs;.ts;.tsx;", "abstract as async await boolean break case catch class const constructor continue debugger declare default delete do else enum export extends false finally for from function get if implements import in instanceof interface let module namespace new null number of private protected public readonly return set static string super switch symbol this throw true try type typeof undefined var void while with yield"},
 {L"Python","python",L".py;.pyw;", "False None True and as assert async await break class continue def del elif else except finally for from global if import in is lambda nonlocal not or pass raise return try while with yield"},
 {L"JSON","json",L".json;.jsonc;.ipynb;", "false null true"},
 {L"HTML","hypertext",L".html;.htm;.xhtml;.vue;.svelte;", "a article aside audio body br button canvas code div em footer form h1 h2 h3 head header html iframe img input label li link main meta nav ol option p pre script section select small source span strong style table tbody td textarea th thead title tr ul video"},
 {L"XML","xml",L".xml;.xaml;.svg;.csproj;.config;.manifest;", ""},
 {L"CSS","css",L".css;.scss;.less;", "align-items background border box-sizing color display flex font font-family font-size gap grid height justify-content margin padding position width"},
 {L"SQL","sql",L".sql;", "add all alter and as asc between by case check column constraint create database default delete desc distinct drop else end exists foreign from full group having in index inner insert into is join key left like limit not null offset on or order outer primary references right select set table then union unique update values view when where with"},
 {L"Shell","bash",L".sh;.bash;.zsh;", "alias bg break case cd command continue declare do done echo elif else esac eval exec exit export false fi for function if in jobs kill local printf pwd read readonly return set shift source test then time trap true type ulimit umask unset until wait while"},
 {L"Markdown","markdown",L".md;.markdown;", ""},
 {L"YAML","yaml",L".yaml;.yml;", "false null true yes no on off"},
 {L"Rust","rust",L".rs;", "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while"},
 {L"Java / C#","cpp",L".java;.cs;", "abstract as async await base bool boolean break byte case catch char checked class const continue decimal default delegate do double else enum event explicit extends extern false final finally fixed float for foreach goto if implements implicit import in int interface internal is lock long namespace native new null object operator out override package params private protected public readonly ref return sbyte sealed short sizeof stackalloc static string struct super switch synchronized this throw throws transient true try typeof uint ulong unchecked unsafe ushort using var virtual void volatile while yield"},
 {L"PowerShell","powershell",L".ps1;.psm1;.psd1;", "begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally for foreach from function if in inlinescript parallel param process return sequence switch throw trap try until using var while workflow"},
 {L"INI / Properties","props",L".ini;.properties;.cfg;.conf;.toml;", ""}
 };return all;}
inline int detectLanguage(const std::wstring& path){auto name=lower(basename(path));auto p=name.find_last_of(L'.');if(p==name.npos)return 0;auto ext=name.substr(p)+L";";for(size_t i=0;i<languages().size();++i)if(std::wstring(languages()[i].extensions).find(ext)!=std::wstring::npos)return (int)i;return 0;}
inline LRESULT sci(HWND h,UINT msg,WPARAM w=0,LPARAM l=0){return SendMessageW(h,msg,w,l);}
inline void style(HWND h,int n,COLORREF color,bool bold=false){sci(h,SCI_STYLESETFORE,n,color);sci(h,SCI_STYLESETBOLD,n,bold);}
inline void element(HWND h,int id,COLORREF color,BYTE alpha=255){sci(h,SCI_SETELEMENTCOLOUR,id,(LPARAM)(color|((DWORD)alpha<<24)));}
inline const char* editorFont(){static const char* face=fontExists(L"Cascadia Mono")?"Cascadia Mono":"Consolas";return face;}
// Sizes the line-number margin to the document's digit count so it neither clips nor wastes space.
inline void fitLineNumbers(HWND h,bool lines,int dpi){if(!lines){if(sci(h,SCI_GETMARGINWIDTHN,0))sci(h,SCI_SETMARGINWIDTHN,0,0);return;}
    int digits=std::max(3,(int)std::to_string(sci(h,SCI_GETLINECOUNT)).size());std::string sample(digits,'9');
    LRESULT width=sci(h,SCI_TEXTWIDTH,STYLE_LINENUMBER,(LPARAM)sample.c_str())+MulDiv(18,dpi,96);if(sci(h,SCI_GETMARGINWIDTHN,0)!=width)sci(h,SCI_SETMARGINWIDTHN,0,width);}
inline void paintEditor(HWND h,const Palette& p,bool lines,bool whitespace,bool wrap,int fontSize,int dpi){
    COLORREF bg=p.surface,fg=p.editorText;auto px=[dpi](int n){return MulDiv(n,dpi,96);};
    sci(h,SCI_STYLESETFONT,STYLE_DEFAULT,(LPARAM)editorFont());sci(h,SCI_STYLESETSIZE,STYLE_DEFAULT,fontSize);sci(h,SCI_STYLESETFORE,STYLE_DEFAULT,fg);sci(h,SCI_STYLESETBACK,STYLE_DEFAULT,bg);sci(h,SCI_STYLECLEARALL);
    style(h,STYLE_LINENUMBER,p.gutterText);sci(h,SCI_STYLESETBACK,STYLE_LINENUMBER,bg);style(h,STYLE_INDENTGUIDE,p.guide);sci(h,SCI_STYLESETBACK,STYLE_INDENTGUIDE,bg);
    sci(h,SCI_SETCARETWIDTH,dpi>=144?3:2);sci(h,SCI_SETCARETLINEVISIBLE,1);sci(h,SCI_SETCARETLINEVISIBLEALWAYS,1);
    element(h,SC_ELEMENT_CARET,p.dark?hex(0xAEAFAD):hex(0x000000));element(h,SC_ELEMENT_CARET_LINE_BACK,p.caretLine);
    element(h,SC_ELEMENT_SELECTION_BACK,p.selection);element(h,SC_ELEMENT_SELECTION_ADDITIONAL_BACK,p.selection,200);element(h,SC_ELEMENT_SELECTION_INACTIVE_BACK,p.selectionInactive);element(h,SC_ELEMENT_WHITE_SPACE,p.whitespace);
    element(h,SC_ELEMENT_LIST,p.text);element(h,SC_ELEMENT_LIST_BACK,p.dark?hex(0x2C2C2C):hex(0xFFFFFF));element(h,SC_ELEMENT_LIST_SELECTED,p.dark?hex(0xFFFFFF):hex(0x000000));element(h,SC_ELEMENT_LIST_SELECTED_BACK,p.dark?hex(0x04395E):hex(0xCCE8FF));
    sci(h,SCI_SETFOLDMARGINCOLOUR,1,bg);sci(h,SCI_SETFOLDMARGINHICOLOUR,1,bg);sci(h,SCI_SETMARGINLEFT,0,px(6));
    sci(h,SCI_SETMARGINTYPEN,0,SC_MARGIN_NUMBER);fitLineNumbers(h,lines,dpi);
    sci(h,SCI_SETMARGINTYPEN,1,SC_MARGIN_SYMBOL);sci(h,SCI_SETMARGINMASKN,1,1);sci(h,SCI_SETMARGINWIDTHN,1,px(16));sci(h,SCI_SETMARGINSENSITIVEN,1,1);sci(h,SCI_SETMARGINCURSORN,1,SC_CURSORARROW);
    sci(h,SCI_MARKERDEFINE,0,SC_MARK_BOOKMARK);sci(h,SCI_MARKERSETBACK,0,p.accent);sci(h,SCI_MARKERSETFORE,0,p.accent);
    sci(h,SCI_SETMARGINTYPEN,2,SC_MARGIN_SYMBOL);sci(h,SCI_SETMARGINMASKN,2,SC_MASK_FOLDERS);sci(h,SCI_SETMARGINWIDTHN,2,px(16));sci(h,SCI_SETMARGINSENSITIVEN,2,1);sci(h,SCI_SETMARGINCURSORN,2,SC_CURSORARROW);
    // Chevron-style folding markers, as in current code editors.
    for(int n=SC_MARKNUM_FOLDEREND;n<=SC_MARKNUM_FOLDEROPEN;++n){sci(h,SCI_MARKERDEFINE,n,SC_MARK_EMPTY);sci(h,SCI_MARKERSETFORE,n,p.gutterText);sci(h,SCI_MARKERSETBACK,n,p.gutterText);}
    sci(h,SCI_MARKERDEFINE,SC_MARKNUM_FOLDER,SC_MARK_ARROW);sci(h,SCI_MARKERDEFINE,SC_MARKNUM_FOLDEREND,SC_MARK_ARROW);sci(h,SCI_MARKERDEFINE,SC_MARKNUM_FOLDEROPEN,SC_MARK_ARROWDOWN);sci(h,SCI_MARKERDEFINE,SC_MARKNUM_FOLDEROPENMID,SC_MARK_ARROWDOWN);
    sci(h,SCI_SETWRAPMODE,wrap?SC_WRAP_WORD:SC_WRAP_NONE);sci(h,SCI_SETVIEWWS,whitespace?SCWS_VISIBLEALWAYS:SCWS_INVISIBLE);sci(h,SCI_SETINDENTATIONGUIDES,SC_IV_LOOKBOTH);
    style(h,STYLE_BRACELIGHT,fg,true);sci(h,SCI_STYLESETBACK,STYLE_BRACELIGHT,p.braceBack);style(h,STYLE_BRACEBAD,hex(0xF14C4C),true);
    sci(h,SCI_SETEXTRAASCENT,px(2));sci(h,SCI_SETEXTRADESCENT,px(2));
}
inline void colorLanguage(HWND h,int language,Lexilla::CreateLexerFn create,bool dark,bool setLexer=true){
    auto& lang=languages().at(language);if(setLexer){auto lexer=create(lang.lexer);sci(h,SCI_SETILEXER,0,(LPARAM)lexer);sci(h,SCI_SETPROPERTY,(WPARAM)"fold",(LPARAM)"1");sci(h,SCI_SETPROPERTY,(WPARAM)"fold.compact",(LPARAM)"0");sci(h,SCI_SETKEYWORDS,0,(LPARAM)lang.keywords);}
    COLORREF keyword=dark?hex(0x569CD6):hex(0x0000FF),comment=dark?hex(0x6A9955):hex(0x008000),str=dark?hex(0xCE9178):hex(0xA31515),number=dark?hex(0xB5CEA8):hex(0x098658),
        type=dark?hex(0x4EC9B0):hex(0x267F99),preprocessor=dark?hex(0xC586C0):hex(0xAF00DB),property=dark?hex(0x9CDCFE):hex(0x0451A5),tag=dark?hex(0x569CD6):hex(0x800000),error=hex(0xF14C4C);
    std::string lexer=lang.lexer;
    if(lexer=="cpp"){for(int n:{SCE_C_COMMENT,SCE_C_COMMENTLINE,SCE_C_COMMENTDOC,SCE_C_COMMENTLINEDOC})style(h,n,comment);for(int n:{SCE_C_STRING,SCE_C_CHARACTER,SCE_C_STRINGRAW,SCE_C_VERBATIM})style(h,n,str);style(h,SCE_C_WORD,keyword);style(h,SCE_C_WORD2,type);style(h,SCE_C_NUMBER,number);style(h,SCE_C_PREPROCESSOR,preprocessor);style(h,SCE_C_GLOBALCLASS,type);}
    else if(lexer=="python"){for(int n:{SCE_P_COMMENTLINE,SCE_P_COMMENTBLOCK})style(h,n,comment);for(int n:{SCE_P_STRING,SCE_P_CHARACTER,SCE_P_TRIPLE,SCE_P_TRIPLEDOUBLE,SCE_P_FSTRING,SCE_P_FCHARACTER})style(h,n,str);style(h,SCE_P_WORD,keyword);style(h,SCE_P_NUMBER,number);style(h,SCE_P_CLASSNAME,type);style(h,SCE_P_DEFNAME,dark?hex(0xDCDCAA):hex(0x795E26));style(h,SCE_P_DECORATOR,preprocessor);}
    else if(lexer=="json"){style(h,SCE_JSON_STRING,str);style(h,SCE_JSON_PROPERTYNAME,property);style(h,SCE_JSON_NUMBER,number);style(h,SCE_JSON_KEYWORD,keyword);style(h,SCE_JSON_LINECOMMENT,comment);style(h,SCE_JSON_BLOCKCOMMENT,comment);style(h,SCE_JSON_ERROR,error);}
    else if(lexer=="hypertext"||lexer=="xml"){style(h,SCE_H_TAG,tag);style(h,SCE_H_TAGEND,tag);style(h,SCE_H_ATTRIBUTE,property);style(h,SCE_H_DOUBLESTRING,str);style(h,SCE_H_SINGLESTRING,str);style(h,SCE_H_COMMENT,comment);style(h,SCE_H_NUMBER,number);}
    else if(lexer=="css"){style(h,SCE_CSS_COMMENT,comment);style(h,SCE_CSS_TAG,tag);style(h,SCE_CSS_CLASS,type);style(h,SCE_CSS_IDENTIFIER,property);style(h,SCE_CSS_VALUE,number);style(h,SCE_CSS_DOUBLESTRING,str);style(h,SCE_CSS_SINGLESTRING,str);}
    else if(lexer=="sql"){style(h,SCE_SQL_WORD,keyword);style(h,SCE_SQL_NUMBER,number);style(h,SCE_SQL_STRING,str);style(h,SCE_SQL_CHARACTER,str);style(h,SCE_SQL_COMMENT,comment);style(h,SCE_SQL_COMMENTLINE,comment);}
    else if(lexer=="bash"){style(h,SCE_SH_WORD,keyword);style(h,SCE_SH_COMMENTLINE,comment);style(h,SCE_SH_STRING,str);style(h,SCE_SH_CHARACTER,str);style(h,SCE_SH_NUMBER,number);style(h,SCE_SH_SCALAR,property);}
    else if(lexer=="markdown"){for(int n:{SCE_MARKDOWN_HEADER1,SCE_MARKDOWN_HEADER2,SCE_MARKDOWN_HEADER3,SCE_MARKDOWN_HEADER4,SCE_MARKDOWN_STRONG1,SCE_MARKDOWN_STRONG2})style(h,n,keyword,true);style(h,SCE_MARKDOWN_LINK,type);style(h,SCE_MARKDOWN_CODE,str);style(h,SCE_MARKDOWN_CODEBK,str);}
    else if(lexer=="yaml"){style(h,SCE_YAML_COMMENT,comment);style(h,SCE_YAML_IDENTIFIER,property);style(h,SCE_YAML_KEYWORD,keyword);style(h,SCE_YAML_NUMBER,number);style(h,SCE_YAML_TEXT,str);}
    else if(lexer=="rust"){style(h,SCE_RUST_COMMENTLINE,comment);style(h,SCE_RUST_COMMENTBLOCK,comment);style(h,SCE_RUST_WORD,keyword);style(h,SCE_RUST_STRING,str);style(h,SCE_RUST_NUMBER,number);style(h,SCE_RUST_LIFETIME,type);}
    else if(lexer=="powershell"){style(h,SCE_POWERSHELL_COMMENT,comment);style(h,SCE_POWERSHELL_STRING,str);style(h,SCE_POWERSHELL_CHARACTER,str);style(h,SCE_POWERSHELL_KEYWORD,keyword);style(h,SCE_POWERSHELL_NUMBER,number);style(h,SCE_POWERSHELL_VARIABLE,property);}
    else if(lexer=="props"){style(h,SCE_PROPS_COMMENT,comment);style(h,SCE_PROPS_SECTION,keyword,true);style(h,SCE_PROPS_KEY,property);style(h,SCE_PROPS_DEFVAL,str);}
}
}
