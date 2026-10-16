#pragma once
#include "platform.hpp"
#include <uxtheme.h>
// MinGW's GDI+ headers expect min/max to be visible; NOMINMAX hides the macros.
namespace Gdiplus{using std::min;using std::max;}
#include <gdiplus.h>
namespace nu {
// Windows 11 style colours. Chrome is the tab strip / menu / status area, surface is
// the active tab, toolbar and editor that sit on it.
constexpr COLORREF hex(uint32_t v){return RGB((v>>16)&255,(v>>8)&255,v&255);}
inline COLORREF blend(COLORREF a,COLORREF b,double t){auto mix=[t](int x,int y){return (BYTE)(x+(y-x)*t+0.5);};return RGB(mix(GetRValue(a),GetRValue(b)),mix(GetGValue(a),GetGValue(b)),mix(GetBValue(a),GetBValue(b)));}
struct Palette{
    bool dark=false;
    COLORREF chrome,surface,divider,text,textSecondary,textDisabled,accent,onAccent;
    COLORREF control,controlHover,controlPressed,controlBorder,controlBottom,field,fieldFocus,fieldBorder,fieldBottom;
    COLORREF editorText,gutterText,caretLine,selection,selectionInactive,guide,whitespace,braceBack;
    COLORREF hover(COLORREF base) const{return blend(base,dark?RGB(255,255,255):RGB(0,0,0),dark?0.07:0.05);}
    COLORREF pressed(COLORREF base) const{return blend(base,dark?RGB(255,255,255):RGB(0,0,0),dark?0.04:0.08);}
};
// The user's accent colour from Settings > Personalization > Colors. Dark mode uses the
// lighter shade and light mode the darker one, as the Windows shell does.
inline COLORREF systemAccent(bool dark){BYTE p[32];DWORD size=sizeof(p);
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",L"AccentPalette",RRF_RT_REG_BINARY,nullptr,p,&size)==ERROR_SUCCESS&&size>=32){int i=dark?1:4;return RGB(p[i*4],p[i*4+1],p[i*4+2]);}
    return dark?hex(0x60CDFF):hex(0x005FB8);}
inline bool systemUsesDark(){DWORD light=1,size=sizeof(light);
    return RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size)==ERROR_SUCCESS&&light==0;}
inline Palette makePalette(bool dark){Palette p;p.dark=dark;p.accent=systemAccent(dark);p.onAccent=dark?RGB(0,0,0):RGB(255,255,255);
    if(dark){
        p.chrome=hex(0x202020);p.surface=hex(0x282828);p.divider=hex(0x383838);p.text=hex(0xFFFFFF);p.textSecondary=hex(0xC8C8C8);p.textDisabled=hex(0x6E6E6E);
        p.control=hex(0x353535);p.controlHover=hex(0x3B3B3B);p.controlPressed=hex(0x303030);p.controlBorder=hex(0x404040);p.controlBottom=hex(0x3A3A3A);
        p.field=hex(0x323232);p.fieldFocus=hex(0x1F1F1F);p.fieldBorder=hex(0x3C3C3C);p.fieldBottom=hex(0x9A9A9A);
        p.editorText=hex(0xD6D6D6);p.gutterText=hex(0x7E7E7E);p.caretLine=hex(0x303030);p.selection=hex(0x264F78);p.selectionInactive=hex(0x3A3D41);p.guide=hex(0x3E3E3E);p.whitespace=hex(0x505050);p.braceBack=hex(0x3B4D5C);
    }else{
        p.chrome=hex(0xF3F3F3);p.surface=hex(0xFFFFFF);p.divider=hex(0xE5E5E5);p.text=hex(0x1B1B1B);p.textSecondary=hex(0x5F5F5F);p.textDisabled=hex(0xA6A6A6);
        p.control=hex(0xFBFBFB);p.controlHover=hex(0xF5F5F5);p.controlPressed=hex(0xEFEFEF);p.controlBorder=hex(0xE0E0E0);p.controlBottom=hex(0xC8C8C8);
        p.field=hex(0xFAFAFA);p.fieldFocus=hex(0xFFFFFF);p.fieldBorder=hex(0xE3E3E3);p.fieldBottom=hex(0x8A8A8A);
        p.editorText=hex(0x1F1F1F);p.gutterText=hex(0x8F8F8F);p.caretLine=hex(0xF4F6F8);p.selection=hex(0xADD6FF);p.selectionInactive=hex(0xE5EBF1);p.guide=hex(0xDDDDDD);p.whitespace=hex(0xC4C4C4);p.braceBack=hex(0xD5E6F7);
    }return p;}

// Dark menus, scrollbars and tooltips use the uxtheme entry points that Explorer and
// Notepad++ rely on. They exist from Windows 10 1809; earlier builds keep light menus.
inline DWORD windowsBuild(){static DWORD build=[]{using Get=LONG(WINAPI*)(OSVERSIONINFOW*);OSVERSIONINFOW v{};v.dwOSVersionInfoSize=sizeof(v);auto get=(Get)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion");return get&&get(&v)==0?v.dwBuildNumber:0;}();return build;}
inline HMODULE uxthemeModule(){static HMODULE m=windowsBuild()>=17763?LoadLibraryExW(L"uxtheme.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32):nullptr;return m;}
inline void setAppDark(bool dark){HMODULE m=uxthemeModule();if(!m)return;
    using SetMode=int(WINAPI*)(int);using Flush=void(WINAPI*)();auto set=(SetMode)GetProcAddress(m,MAKEINTRESOURCEA(135));auto flush=(Flush)GetProcAddress(m,MAKEINTRESOURCEA(136));
    if(set)set(dark?2:3);if(flush)flush();}
inline void allowDark(HWND h,bool dark){HMODULE m=uxthemeModule();if(!m||!h)return;using Allow=BOOL(WINAPI*)(HWND,BOOL);auto allow=(Allow)GetProcAddress(m,MAKEINTRESOURCEA(133));if(allow)allow(h,dark);}
inline void themeControl(HWND h,bool dark,const wchar_t* darkClass=L"DarkMode_Explorer"){if(!h)return;allowDark(h,dark);SetWindowTheme(h,dark?darkClass:L"Explorer",nullptr);}
inline void themeFrame(HWND h,const Palette& p){HMODULE dwm=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dwm)return;
    using SetAttr=HRESULT(WINAPI*)(HWND,DWORD,LPCVOID,DWORD);auto set=(SetAttr)GetProcAddress(dwm,"DwmSetWindowAttribute");
    if(set){BOOL dark=p.dark;if(FAILED(set(h,20,&dark,sizeof(dark))))set(h,19,&dark,sizeof(dark));
        COLORREF caption=p.chrome,text=p.text;set(h,35,&caption,sizeof(caption));set(h,36,&text,sizeof(text));}
    FreeLibrary(dwm);allowDark(h,p.dark);}

inline int dpiOf(HWND h){using Get=UINT(WINAPI*)(HWND);static auto get=(Get)GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow");
    if(get&&h){UINT d=get(h);if(d)return (int)d;}HDC dc=GetDC(nullptr);int d=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);return d;}
inline bool fontExists(const wchar_t* face){HDC dc=GetDC(nullptr);LOGFONTW lf{};lf.lfCharSet=DEFAULT_CHARSET;lstrcpynW(lf.lfFaceName,face,LF_FACESIZE);bool found=false;
    EnumFontFamiliesExW(dc,&lf,[](const LOGFONTW*,const TEXTMETRICW*,DWORD,LPARAM p)->int{*(bool*)p=true;return 0;},(LPARAM)&found,0);ReleaseDC(nullptr,dc);return found;}
inline HFONT makeFont(const wchar_t* face,int pixels,int weight=FW_NORMAL){return CreateFontW(-pixels,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);}

// Painting helpers: an off-screen buffer to avoid flicker, GDI+ for anti-aliased
// rounded shapes, and GDI for ClearType text.
struct Buffer{
    HDC target,dc;HBITMAP bitmap,old;int w,h;
    Buffer(HDC t,const RECT& rc):target(t),w(std::max<int>(1,rc.right)),h(std::max<int>(1,rc.bottom)){dc=CreateCompatibleDC(t);bitmap=CreateCompatibleBitmap(t,w,h);old=(HBITMAP)SelectObject(dc,bitmap);SetBkMode(dc,TRANSPARENT);}
    ~Buffer(){BitBlt(target,0,0,w,h,dc,0,0,SRCCOPY);SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);}
};
inline void fill(HDC dc,const RECT& rc,COLORREF c){SetDCBrushColor(dc,c);FillRect(dc,&rc,(HBRUSH)GetStockObject(DC_BRUSH));}
inline Gdiplus::Color gp(COLORREF c,BYTE a=255){return Gdiplus::Color(a,GetRValue(c),GetGValue(c),GetBValue(c));}
inline void roundPath(Gdiplus::GraphicsPath& path,float x,float y,float w,float h,float r,bool top=true,bool bottom=true){
    float d=r*2;path.StartFigure();
    if(top){path.AddArc(x,y,d,d,180,90);path.AddArc(x+w-d,y,d,d,270,90);}else{path.AddLine(x,y,x+w,y);}
    if(bottom){path.AddArc(x+w-d,y+h-d,d,d,0,90);path.AddArc(x,y+h-d,d,d,90,90);}else{path.AddLine(x+w,y+h,x,y+h);}
    path.CloseFigure();}
struct Canvas{
    Gdiplus::Graphics g;
    explicit Canvas(HDC dc):g(dc){g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);}
    void round(const RECT& rc,float r,COLORREF c,bool top=true,bool bottom=true){Gdiplus::GraphicsPath path;roundPath(path,(float)rc.left,(float)rc.top,(float)(rc.right-rc.left),(float)(rc.bottom-rc.top),r,top,bottom);Gdiplus::SolidBrush b(gp(c));g.FillPath(&b,&path);}
    void outline(const RECT& rc,float r,COLORREF c,float width=1){float i=width/2;Gdiplus::GraphicsPath path;roundPath(path,rc.left+i,rc.top+i,rc.right-rc.left-width,rc.bottom-rc.top-width,r,true,true);Gdiplus::Pen pen(gp(c),width);g.DrawPath(&pen,&path);}
    void dot(float cx,float cy,float r,COLORREF c){Gdiplus::SolidBrush b(gp(c));g.FillEllipse(&b,cx-r,cy-r,r*2,r*2);}
    // Concave corner where an active tab flares into the content below it.
    void flare(float x,float y,float r,COLORREF c,bool left){Gdiplus::Region region(Gdiplus::RectF(left?x-r:x,y-r,r,r));Gdiplus::GraphicsPath circle;circle.AddEllipse(left?x-2*r:x,y-2*r,2*r,2*r);region.Exclude(&circle);Gdiplus::SolidBrush b(gp(c));g.FillRegion(&b,&region);}
    // Text-box bottom stroke, clipped to the box's rounded outline.
    void bottomEdge(const RECT& rc,float r,COLORREF c,int thickness){Gdiplus::GraphicsPath path;roundPath(path,(float)rc.left,(float)rc.top,(float)(rc.right-rc.left),(float)(rc.bottom-rc.top),r);g.SetClip(&path);Gdiplus::SolidBrush b(gp(c));g.FillRectangle(&b,(float)rc.left,(float)(rc.bottom-thickness),(float)(rc.right-rc.left),(float)thickness);g.ResetClip();}
};
inline void drawText(HDC dc,const std::wstring& s,RECT rc,COLORREF color,HFONT font,UINT flags=DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS|DT_NOPREFIX){
    HGDIOBJ old=SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);DrawTextW(dc,s.c_str(),(int)s.size(),&rc,flags);SelectObject(dc,old);}
inline void drawGlyph(HDC dc,wchar_t glyph,const RECT& rc,COLORREF color,HFONT font){drawText(dc,std::wstring(1,glyph),rc,color,font,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);}
inline int textWidth(HDC dc,const std::wstring& s,HFONT font){HGDIOBJ old=SelectObject(dc,font);SIZE size{};GetTextExtentPoint32W(dc,s.c_str(),(int)s.size(),&size);SelectObject(dc,old);return size.cx;}

// Undocumented messages Windows sends to draw the menu bar. Handling them is the only way
// to colour the menu bar itself; the popup menus follow the app's dark-mode preference.
constexpr UINT WM_UAHDRAWMENU=0x0091,WM_UAHDRAWMENUITEM=0x0092;
struct UAHMENU{HMENU hmenu;HDC hdc;DWORD dwFlags;};
union UAHMENUITEMMETRICS{struct{DWORD cx,cy;}rgsizeBar[2];struct{DWORD cx,cy;}rgsizePopup[4];};
struct UAHMENUPOPUPMETRICS{DWORD rgcx[4];DWORD fUpdateMaxWidths:2;};
struct UAHMENUITEM{int iPosition;UAHMENUITEMMETRICS umim;UAHMENUPOPUPMETRICS umpm;};
struct UAHDRAWMENUITEM{DRAWITEMSTRUCT dis;UAHMENU um;UAHMENUITEM umi;};
inline void paintMenuBar(HWND h,const UAHMENU* menu,const Palette& p){MENUBARINFO info{};info.cbSize=sizeof(info);if(!GetMenuBarInfo(h,OBJID_MENU,0,&info))return;RECT win;GetWindowRect(h,&win);RECT rc=info.rcBar;OffsetRect(&rc,-win.left,-win.top);fill(menu->hdc,rc,p.chrome);}
inline void paintMenuItem(HWND,const UAHDRAWMENUITEM* item,const Palette& p){wchar_t name[256]{};MENUITEMINFOW info{};info.cbSize=sizeof(info);info.fMask=MIIM_STRING;info.dwTypeData=name;info.cch=255;GetMenuItemInfoW(item->um.hmenu,item->umi.iPosition,TRUE,&info);
    UINT state=item->dis.itemState;COLORREF back=p.chrome,color=p.text;
    if(state&(ODS_INACTIVE|ODS_GRAYED|ODS_DISABLED))color=p.textDisabled;
    if(state&ODS_SELECTED)back=p.pressed(p.chrome);else if(state&ODS_HOTLIGHT)back=p.hover(p.chrome);
    RECT rc=item->dis.rcItem;fill(item->um.hdc,rc,p.chrome);
    if(back!=p.chrome){RECT pill=rc;InflateRect(&pill,0,-2);Canvas c(item->um.hdc);c.round(pill,4,back);}
    SetBkMode(item->um.hdc,TRANSPARENT);SetTextColor(item->um.hdc,color);DrawTextW(item->um.hdc,name,-1,&rc,DT_CENTER|DT_SINGLELINE|DT_VCENTER|((state&ODS_NOACCEL)?DT_HIDEPREFIX:0));}
// Covers the one-pixel light line Windows draws between the menu bar and client area.
inline void paintMenuLine(HWND h,const Palette& p){MENUBARINFO info{};info.cbSize=sizeof(info);if(!GetMenuBarInfo(h,OBJID_MENU,0,&info))return;RECT client;GetClientRect(h,&client);MapWindowPoints(h,nullptr,(POINT*)&client,2);RECT win;GetWindowRect(h,&win);OffsetRect(&client,-win.left,-win.top);
    RECT line=client;line.bottom=line.top;line.top--;HDC dc=GetWindowDC(h);fill(dc,line,p.chrome);ReleaseDC(h,dc);}
}
