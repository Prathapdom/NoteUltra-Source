#pragma once
#include "platform.hpp"
#include <cstring>
#include <cstdio>
// Markdown to RTF for the preview pane, which is a RichEdit control. Covers CommonMark's
// common blocks (headings, paragraphs, lists, task lists, quotes, fenced and indented code,
// rules) plus GitHub tables, strikethrough and autolinks. Raw HTML tags are dropped.
namespace nu {
struct MarkdownStyle{COLORREF text,muted,link,codeText,codeBack,rule,tableHead;std::wstring body,mono;int width=9000;};
class MarkdownRtf{
    const MarkdownStyle& st;std::string out;
    static std::string trim(const std::string& s){size_t a=s.find_first_not_of(" \t"),b=s.find_last_not_of(" \t");return a==s.npos?std::string():s.substr(a,b-a+1);}
    static int indentOf(const std::string& s){int n=0;while(n<(int)s.size()&&s[n]==' ')++n;return n;}
    static bool blank(const std::string& s){return s.find_first_not_of(" \t")==s.npos;}
    static std::string num(long long v){return std::to_string(v);}
    // Appends one Unicode code point, escaped for RTF.
    void put(uint32_t c){
        if(c=='\\'||c=='{'||c=='}'){out+='\\';out+=(char)c;}else if(c=='\t')out+="\\tab ";else if(c<0x20)return;else if(c<0x80)out+=(char)c;
        else if(c<0x10000)out+="\\u"+num((int16_t)c)+"?";else{c-=0x10000;out+="\\u"+num((int16_t)(0xD800+(c>>10)))+"?\\u"+num((int16_t)(0xDC00+(c&0x3FF)))+"?";}}
    void plain(const std::string& s){for(size_t i=0;i<s.size();){unsigned char c=(unsigned char)s[i];uint32_t cp=c;int n=c<0x80?1:c<0xE0?2:c<0xF0?3:4;if(n>1){cp=c&(0x7F>>n);for(int k=1;k<n&&i+k<s.size();++k)cp=(cp<<6)|(s[i+k]&0x3F);}i+=n;put(cp);}}
    static bool punct(char c){return c&&strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~",c);}
    static bool word(char c){return isalnum((unsigned char)c)||(unsigned char)c>=0x80;}
    // Finds the closing delimiter of an emphasis run, skipping code spans.
    static size_t closing(const std::string& s,size_t from,const std::string& d){for(size_t i=from;i+d.size()<=s.size();++i){if(s[i]=='\\'){++i;continue;}if(s[i]=='`'){size_t e=s.find('`',i+1);if(e==s.npos)return s.npos;i=e;continue;}
            if(s.compare(i,d.size(),d)==0&&i>from&&s[i-1]!=' '&&(d.size()>1||i+1>=s.size()||s[i+1]!=d[0])&&(d[0]!='_'||i+d.size()>=s.size()||!word(s[i+d.size()])))return i;}return s.npos;}
    static size_t bracket(const std::string& s,size_t open){int depth=0;for(size_t i=open;i<s.size();++i){if(s[i]=='\\'){++i;continue;}if(s[i]=='[')++depth;else if(s[i]==']'&&--depth==0)return i;}return s.npos;}
    // Link text is parsed for emphasis but not for further links, so a URL that is its own text cannot recurse.
    bool inLink=false;
    void link(const std::string& url,const std::string& text){out+="{\\field{\\*\\fldinst{HYPERLINK \"";for(char c:url)if(c!='"'&&c!='\\'&&c!='{'&&c!='}')out+=c;out+="\"}}{\\fldrslt{\\ul\\cf3 ";bool was=inLink;inLink=true;inl(text);inLink=was;out+="}}}";}
public:
    std::vector<std::pair<std::string,std::string>> links;
    explicit MarkdownRtf(const MarkdownStyle& s):st(s){}
    void inl(const std::string& s){
        for(size_t i=0;i<s.size();){char c=s[i];
            if(c=='\\'&&i+1<s.size()&&punct(s[i+1])){put((unsigned char)s[i+1]);i+=2;continue;}
            if(c=='`'){size_t n=0;while(i+n<s.size()&&s[i+n]=='`')++n;size_t e=s.find(std::string(n,'`'),i+n);if(e!=s.npos){auto code=s.substr(i+n,e-i-n);if(code.size()>1&&code.front()==' '&&code.back()==' ')code=code.substr(1,code.size()-2);out+="{\\f1\\fs19\\cf4\\highlight5 ";plain(code);out+="}";i=e+n;continue;}plain(std::string(n,'`'));i+=n;continue;}
            if((c=='*'||c=='_'||c=='~')&&!(c=='_'&&i>0&&word(s[i-1]))){
                std::string d=s.compare(i,3,std::string(3,c))==0&&c!='~'?std::string(3,c):s.compare(i,2,std::string(2,c))==0?std::string(2,c):std::string(1,c);
                if(!(c=='~'&&d.size()!=2)&&i+d.size()<s.size()&&s[i+d.size()]!=' '){size_t e=closing(s,i+d.size(),d);
                    if(e!=s.npos){out+=c=='~'?"{\\strike ":d.size()==3?"{\\b\\i ":d.size()==2?"{\\b ":"{\\i ";inl(s.substr(i+d.size(),e-i-d.size()));out+="}";i=e+d.size();continue;}}}
            if(c=='!'&&i+1<s.size()&&s[i+1]=='['){size_t e=bracket(s,i+1);if(e!=s.npos&&e+1<s.size()&&s[e+1]=='('){size_t p=s.find(')',e+2);if(p!=s.npos){out+="{\\i\\cf2 [";auto alt=s.substr(i+2,e-i-2);plain(alt.empty()?"image":alt);out+="]}";i=p+1;continue;}}}
            if(c=='['&&!inLink){size_t e=bracket(s,i);if(e!=s.npos&&e+1<s.size()&&s[e+1]=='('){size_t p=s.find(')',e+2);if(p!=s.npos){auto target=trim(s.substr(e+2,p-e-2));auto sp=target.find(' ');if(sp!=target.npos)target=target.substr(0,sp);if(target.size()>1&&target.front()=='<'&&target.back()=='>')target=target.substr(1,target.size()-2);
                auto text=s.substr(i+1,e-i-1);links.push_back({text,target});link(target,text);i=p+1;continue;}}}
            if(c=='<'){size_t e=s.find('>',i);if(e!=s.npos){auto tag=s.substr(i+1,e-i-1);
                if(tag.compare(0,7,"http://")==0||tag.compare(0,8,"https://")==0||tag.compare(0,7,"mailto:")==0){links.push_back({tag,tag});link(tag,tag);i=e+1;continue;}
                if(!tag.empty()&&(isalpha((unsigned char)tag[0])||tag[0]=='/'||tag[0]=='!')&&tag.find('<')==tag.npos){std::string name;for(char t:tag){if(!isalnum((unsigned char)t)&&t!='/')break;name+=(char)tolower((unsigned char)t);}if(name=="br"||name=="br/")out+="\\line ";i=e+1;continue;}}}
            if(c=='&'){size_t e=s.find(';',i);if(e!=s.npos&&e-i<10){auto ent=s.substr(i+1,e-i-1);uint32_t cp=0;
                if(ent=="amp")cp='&';else if(ent=="lt")cp='<';else if(ent=="gt")cp='>';else if(ent=="quot")cp='"';else if(ent=="apos")cp='\'';else if(ent=="nbsp")cp=0xA0;else if(ent=="copy")cp=0xA9;else if(ent=="mdash")cp=0x2014;else if(ent=="ndash")cp=0x2013;
                else if(ent.size()>1&&ent[0]=='#')cp=(uint32_t)strtoul(ent.c_str()+(ent[1]=='x'||ent[1]=='X'?2:1),nullptr,ent[1]=='x'||ent[1]=='X'?16:10);
                if(cp){put(cp);i=e+1;continue;}}}
            if(!inLink&&(c=='h'&&(s.compare(i,7,"http://")==0||s.compare(i,8,"https://")==0))&&(i==0||!word(s[i-1]))){size_t e=i;while(e<s.size()&&s[e]!=' '&&s[e]!='<')++e;while(e>i&&strchr(".,;:!?)*_",s[e-1]))--e;auto url=s.substr(i,e-i);links.push_back({url,url});link(url,url);i=e;continue;}
            unsigned char u=(unsigned char)c;int n=u<0x80?1:u<0xE0?2:u<0xF0?3:4;plain(s.substr(i,n));i+=n;}
    }
    void paragraph(const std::vector<std::string>& lines,int indent,int first,int after,const std::string& lead,const char* fmt=""){
        out+="\\pard\\li"+num(indent)+"\\fi"+num(first)+"\\sa"+num(after)+"\\sl276\\slmult1"+fmt+" "+lead;
        for(size_t i=0;i<lines.size();++i){auto l=lines[i];bool hard=false;if(l.size()>=2&&l.compare(l.size()-2,2,"  ")==0)hard=true;else if(!l.empty()&&l.back()=='\\'){hard=true;l.pop_back();}
            inl(trim(l));if(i+1<lines.size())out+=hard?"\\line ":" ";}
        out+="\\par\n";}
    void rule(int indent,int width){out+="\\pard\\sa0\\sb0\\fs4\\par\\trowd\\trleft"+num(indent)+"\\trrh-30\\clbrdrb\\brdrs\\brdrw15\\brdrcf6\\cellx"+num(indent+width)+"\\pard\\intbl\\fs4\\cell\\row\\pard\\sa120\\fs4\\par\\fs21\n";}
    void code(const std::vector<std::string>& lines,int indent){out+="\\trowd\\trleft"+num(indent)+"\\trpaddl160\\trpaddr160\\trpaddt100\\trpaddb100\\trpaddfl3\\trpaddfr3\\trpaddft3\\trpaddfb3\\clcbpat5\\cellx"+num(std::max(indent+1440,st.width))+"\\pard\\intbl\\sl0\\f1\\fs19\\cf4 ";
        for(size_t i=0;i<lines.size();++i){plain(lines[i]);if(i+1<lines.size())out+="\\line ";}out+="\\cell\\row\\pard\\f0\\fs21\\cf1\\sa160\\fs8\\par\\fs21\n";}
    static std::vector<std::string> cells(std::string row){row=trim(row);if(!row.empty()&&row[0]=='|')row.erase(0,1);if(!row.empty()&&row.back()=='|'&&(row.size()<2||row[row.size()-2]!='\\'))row.pop_back();
        std::vector<std::string> r;std::string cur;bool tick=false;for(size_t i=0;i<row.size();++i){char c=row[i];if(c=='\\'&&i+1<row.size()&&row[i+1]=='|'){cur+='|';++i;continue;}if(c=='`')tick=!tick;if(c=='|'&&!tick){r.push_back(trim(cur));cur.clear();}else cur+=c;}r.push_back(trim(cur));return r;}
    static bool delimiterRow(const std::string& s){auto t=trim(s);if(t.find('-')==t.npos)return false;for(char c:t)if(!strchr("|:- \t",c))return false;return t.find('|')!=t.npos||t.find(':')!=t.npos||t.size()>=3;}
    void table(const std::vector<std::vector<std::string>>& rows,const std::vector<std::string>& align,int indent){
        size_t n=rows[0].size();std::vector<double> weight(n,6);for(auto& r:rows)for(size_t c=0;c<n&&c<r.size();++c)weight[c]=std::max(weight[c],std::min<double>(60,(double)r[c].size()));
        double total=0;for(double w:weight)total+=w;int width=st.width-indent;std::vector<int> edge;double x=indent;for(double w:weight){x+=width*w/total;edge.push_back((int)x);}
        for(size_t r=0;r<rows.size();++r){out+="\\trowd\\trleft"+num(indent)+"\\trpaddl100\\trpaddr100\\trpaddt60\\trpaddb60\\trpaddfl3\\trpaddfr3\\trpaddft3\\trpaddfb3";
            for(size_t c=0;c<n;++c){out+="\\clbrdrt\\brdrs\\brdrw10\\brdrcf6\\clbrdrl\\brdrs\\brdrw10\\brdrcf6\\clbrdrb\\brdrs\\brdrw10\\brdrcf6\\clbrdrr\\brdrs\\brdrw10\\brdrcf6";if(r==0)out+="\\clcbpat7";out+="\\cellx"+num(edge[c]);}
            for(size_t c=0;c<n;++c){out+="\\pard\\intbl"+std::string(c<align.size()&&align[c]=="c"?"\\qc":c<align.size()&&align[c]=="r"?"\\qr":"\\ql")+(r==0?"\\b ":" ");if(c<rows[r].size())inl(rows[r][c]);out+=r==0?"\\b0\\cell":"\\cell";}
            out+="\\row\n";}
        out+="\\pard\\sa160\\fs8\\par\\fs21\n";}
    static bool fence(const std::string& s,std::string& marker){auto t=s.substr(std::min<size_t>(s.size(),(size_t)std::min(indentOf(s),3)));if(t.size()<3)return false;char c=t[0];if(c!='`'&&c!='~')return false;size_t n=0;while(n<t.size()&&t[n]==c)++n;if(n<3)return false;if(c=='`'&&t.find('`',n)!=t.npos)return false;marker=std::string(n,c);return true;}
    static int heading(const std::string& s,std::string& text){int i=indentOf(s);if(i>3)return 0;int n=0;while(i+n<(int)s.size()&&s[i+n]=='#')++n;if(n<1||n>6||(i+n<(int)s.size()&&s[i+n]!=' '&&s[i+n]!='\t'))return 0;text=trim(s.substr(i+n));
        size_t e=text.find_last_not_of('#');if(e==text.npos)text.clear();else if(e+1<text.size()&&(text[e]==' '))text=trim(text.substr(0,e+1));return n;}
    static bool ruleLine(const std::string& s){auto t=trim(s);if(indentOf(s)>3||t.size()<3)return false;char c=t[0];if(c!='-'&&c!='*'&&c!='_')return false;int n=0;for(char x:t){if(x==c)++n;else if(x!=' '&&x!='\t')return false;}return n>=3;}
    // A list marker: returns the content column, or -1. Sets ordered/start.
    static int listItem(const std::string& s,bool& ordered,long& start){int i=indentOf(s);size_t p=(size_t)i;if(p>=s.size())return -1;
        if(strchr("-*+",s[p])){ordered=false;++p;}else{size_t q=p;while(q<s.size()&&q-p<9&&isdigit((unsigned char)s[q]))++q;if(q==p||q>=s.size()||(s[q]!='.'&&s[q]!=')'))return -1;ordered=true;start=atol(s.substr(p,q-p).c_str());p=q+1;}
        if(p<s.size()&&s[p]!=' ')return -1;size_t q=p;while(q<s.size()&&s[q]==' '&&q-p<4)++q;if(q>=s.size())q=p+1;return (int)q;}
    static bool blockStart(const std::string& s){std::string m,t;bool o=false;long st0=1;return fence(s,m)||heading(s,t)||ruleLine(s)||(indentOf(s)<4&&trim(s).compare(0,1,">")==0)||listItem(s,o,st0)>=0;}
    void blocks(const std::vector<std::string>& lines,int indent,int depth,bool tight,std::string lead=""){
        std::vector<std::string> para;int after=tight?40:160;
        auto flush=[&]{if(para.empty())return;paragraph(para,indent,lead.empty()?0:-360,after,lead);lead.clear();para.clear();};
        auto ensureLead=[&]{if(!lead.empty()){out+="\\pard\\li"+num(indent)+"\\fi-360\\sa"+num(after)+" "+lead+"\\par\n";lead.clear();}};
        for(size_t i=0;i<lines.size();){const std::string& line=lines[i];std::string marker,text;bool ordered=false;long start=1;
            if(blank(line)){flush();++i;continue;}
            if(fence(line,marker)){flush();ensureLead();int ind=indentOf(line);std::vector<std::string> code_;++i;
                while(i<lines.size()){auto t=trim(lines[i]);if(t.compare(0,marker.size(),marker)==0&&t.find_first_not_of(marker[0])==t.npos)break;auto l=lines[i];int k=0;while(k<ind&&!l.empty()&&l[0]==' '){l.erase(0,1);++k;}code_.push_back(l);++i;}
                ++i;code(code_,indent);continue;}
            if(para.empty()&&indentOf(line)>=4){ensureLead();std::vector<std::string> code_;while(i<lines.size()&&(indentOf(lines[i])>=4||blank(lines[i]))){code_.push_back(blank(lines[i])?"":lines[i].substr(4));++i;}while(!code_.empty()&&code_.back().empty())code_.pop_back();code(code_,indent);continue;}
            if(int level=heading(line,text)){flush();ensureLead();static const int size[]={0,40,32,27,23,21,20};out+="\\pard\\li"+num(indent)+"\\sb"+num(level<=2?240:200)+"\\sa"+num(level<=2?80:100)+"\\keepn\\b\\fs"+num(size[level])+(level==6?"\\cf2 ":" ");inl(text);out+="\\b0\\fs21\\cf1\\par\n";if(level<=2)rule(indent,st.width-indent);++i;continue;}
            if(!para.empty()&&indentOf(line)<4&&!trim(line).empty()&&(trim(line).find_first_not_of('=')==std::string::npos||(trim(line).find_first_not_of('-')==std::string::npos))){bool h1=trim(line)[0]=='=';auto t=para;para.clear();out+="\\pard\\li"+num(indent)+"\\sb240\\sa80\\keepn\\b\\fs"+num(h1?40:32)+" ";for(size_t k=0;k<t.size();++k){inl(trim(t[k]));if(k+1<t.size())out+=" ";}out+="\\b0\\fs21\\par\n";rule(indent,st.width-indent);++i;continue;}
            if(ruleLine(line)){flush();ensureLead();rule(indent,st.width-indent);++i;continue;}
            if(indentOf(line)<4&&trim(line)[0]=='>'){flush();ensureLead();std::vector<std::string> inner;
                while(i<lines.size()&&!blank(lines[i])){auto t=trim(lines[i]);if(t[0]=='>'){t.erase(0,1);if(!t.empty()&&t[0]==' ')t.erase(0,1);}else if(blockStart(lines[i]))break;inner.push_back(t);++i;}
                out+="{\\cf2 ";blocks(inner,indent+400,depth,false);out+="}";continue;}
            if(listItem(line,ordered,start)>=0&&(para.empty()||!ordered||start==1)){
                flush();ensureLead();bool kind=ordered;long number=start;bool looseList=false;std::vector<std::pair<std::vector<std::string>,bool>> items;
                while(i<lines.size()){bool o=false;long s0=1;int col=listItem(lines[i],o,s0);if(col<0||o!=kind)break;int base=indentOf(lines[i]);
                    std::vector<std::string> item;item.push_back(lines[i].size()>(size_t)col?lines[i].substr(col):"");++i;bool sawBlank=false;
                    while(i<lines.size()){const auto& l=lines[i];if(blank(l)){sawBlank=true;item.push_back("");++i;continue;}int ind=indentOf(l);
                        if(ind>=col){item.push_back(l.substr(std::min<size_t>(l.size(),(size_t)col)));++i;sawBlank=false;continue;}
                        bool o2=false;long s2=1;if(!sawBlank&&listItem(l,o2,s2)<0&&!blockStart(l)){item.push_back(trim(l));++i;continue;}break;}
                    while(!item.empty()&&blank(item.back()))item.pop_back();
                    bool hasBlank=std::find_if(item.begin(),item.end(),[](const std::string& x){return blank(x);})!=item.end();if(hasBlank||(sawBlank&&i<lines.size()&&listItem(lines[i],o,s0)>=0&&o==kind&&indentOf(lines[i])<=base))looseList=true;
                    items.push_back({item,false});if(sawBlank&&(i>=lines.size()||listItem(lines[i],o,s0)<0))break;}
                for(auto& it:items){std::string bullet;auto& item=it.first;
                    if(!item.empty()&&(item[0].compare(0,4,"[ ] ")==0||item[0].compare(0,4,"[x] ")==0||item[0].compare(0,4,"[X] ")==0)){bullet=item[0][1]==' '?"\\u9744?":"\\u9745?";item[0]=item[0].substr(4);}
                    else if(kind)bullet=num(number++)+".";else bullet=depth%3==0?"\\u8226?":depth%3==1?"\\u9702?":"\\u9642?";
                    blocks(item,indent+360,depth+1,!looseList,bullet+"\\tab ");}
                out+="\\pard\\sa0\\fs8\\par\\fs21\n";continue;}
            if(line.find('|')!=line.npos&&i+1<lines.size()&&delimiterRow(lines[i+1])){auto head=cells(line);auto spec=cells(lines[i+1]);if(spec.size()==head.size()){
                std::vector<std::string> align;for(auto& a:spec){bool l=!a.empty()&&a.front()==':',r=!a.empty()&&a.back()==':';align.push_back(l&&r?"c":r?"r":"l");}
                std::vector<std::vector<std::string>> rows{head};i+=2;while(i<lines.size()&&!blank(lines[i])&&lines[i].find('|')!=lines[i].npos){auto r=cells(lines[i]);r.resize(head.size());rows.push_back(r);++i;}
                flush();ensureLead();table(rows,align,indent);continue;}}
            para.push_back(line);++i;}
        flush();ensureLead();}
    static std::string color(COLORREF c){return "\\red"+num(GetRValue(c))+"\\green"+num(GetGValue(c))+"\\blue"+num(GetBValue(c))+";";}
    std::string convert(const std::string& md){out.clear();links.clear();
        auto face=[](const std::wstring& w){std::string s;for(wchar_t c:w)s+=c<0x80?(char)c:'?';return s;};
        out="{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl{\\f0\\fswiss\\fcharset0 "+face(st.body)+";}{\\f1\\fmodern\\fcharset0 "+face(st.mono)+";}}{\\colortbl ;"+color(st.text)+color(st.muted)+color(st.link)+color(st.codeText)+color(st.codeBack)+color(st.rule)+color(st.tableHead)+"}\\viewkind4\\f0\\fs21\\cf1\n";
        std::vector<std::string> lines;std::string cur;for(size_t i=0;i<=md.size();++i){char c=i<md.size()?md[i]:'\n';if(c=='\r'){if(i+1<md.size()&&md[i+1]=='\n')continue;c='\n';}
            if(c=='\n'){lines.push_back(cur);cur.clear();}else if(c=='\t'){do cur+=' ';while(cur.size()%4);}else cur+=c;}
        if(!md.empty()&&lines.size()>1&&lines.back().empty())lines.pop_back();
        size_t skip=0;if(!lines.empty()&&lines[0].compare(0,3,"\xEF\xBB\xBF")==0)lines[0].erase(0,3);
        // YAML front matter is metadata, not content.
        if(!lines.empty()&&trim(lines[0])=="---"){for(size_t k=1;k<lines.size()&&k<200;++k)if(trim(lines[k])=="---"||trim(lines[k])=="..."){skip=k+1;break;}}
        lines.erase(lines.begin(),lines.begin()+skip);blocks(lines,0,0,false);out+="\\pard\\par}";return out;}
};
inline std::string markdownToRtf(const std::string& md,const MarkdownStyle& style,std::vector<std::pair<std::string,std::string>>* links=nullptr){MarkdownRtf r(style);auto s=r.convert(md);if(links)*links=std::move(r.links);return s;}
}
