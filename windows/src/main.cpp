#include "platform.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>

using namespace lf;
namespace {
constexpr UINT WM_CHILD_EVENT=WM_APP+1, WM_TRAY=WM_APP+2, WM_COMMAND_LINE=WM_APP+3, WM_PREVIEW=WM_APP+4;
constexpr UINT_PTR TIMER_SESSION=1;
enum Control : int { HostList=100,NewHost,DeleteHost,Name,Target,User,Port,Key,Rules,LocalPort,RemoteHost,RemotePort,RuleSave,RuleNew,RuleDelete,Save,Connect,Recovery,Inherited,Status,Preview,Quit,Reconnect };
struct ChildEvent { uint64_t generation; bool ended; DWORD code; std::string text; };
struct PreviewEvent { uint64_t generation; std::string text; };
std::wstring textOf(HWND control) { int size=GetWindowTextLengthW(control); std::wstring text(size+1,0); GetWindowTextW(control,text.data(),size+1); text.resize(size); return text; }
int parsePort(const std::wstring& text) {
    if(text.empty() || !std::all_of(text.begin(),text.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';})) throw std::runtime_error("포트를 숫자로 입력하세요.");
    size_t used=0; int port=std::stoi(text,&used); if(used!=text.size() || port<1 || port>65535) throw std::runtime_error("포트는 1~65535 사이여야 합니다."); return port;
}
std::wstring labelState(State state) {
    switch(state) {
        case State::editing:return L"접속 호스트를 선택하세요.";
        case State::connecting:return L"연결 중…";
        case State::connected:return L"connected";
        case State::waitingReconnect:return L"disconnected — 단절 감지 후 1분 뒤 자동 재연결";
        case State::reconnecting:return L"disconnected — 재연결 중…";
        case State::waitingExit:return L"disconnected — 재연결 실패 후 1분 뒤 종료";
        case State::exiting:return L"종료 중…";
    } return {};
}
HICON stateIcon(bool connected) {
    constexpr int n=32; BITMAPV5HEADER bi{}; bi.bV5Size=sizeof(bi); bi.bV5Width=n; bi.bV5Height=-n; bi.bV5Planes=1; bi.bV5BitCount=32; bi.bV5Compression=BI_BITFIELDS;
    bi.bV5RedMask=0x00ff0000; bi.bV5GreenMask=0x0000ff00; bi.bV5BlueMask=0x000000ff; bi.bV5AlphaMask=0xff000000;
    void* pixels=nullptr; HDC dc=GetDC(nullptr); HBITMAP bitmap=CreateDIBSection(dc,reinterpret_cast<BITMAPINFO*>(&bi),DIB_RGB_COLORS,&pixels,nullptr,0); ReleaseDC(nullptr,dc);
    if(!bitmap) return nullptr;
    auto* p=static_cast<DWORD*>(pixels);
    for(int y=0;y<n;++y) for(int x=0;x<n;++x) {
        int dx=x-16,dy=y-16; bool inside=connected ? dx*dx+dy*dy<=13*13 : std::abs(dx)+std::abs(dy)<=15;
        p[y*n+x]=inside?(connected?0xff248a4f:0xffa33b35):0;
        if(inside && ((connected && (x>=14&&x<=17&&y>=8&&y<=23)) || (!connected && y>=14&&y<=17&&x>=8&&x<=23))) p[y*n+x]=0xffffffff;
    }
    BYTE maskBits[n*n/8]{}; HBITMAP mask=CreateBitmap(n,n,1,1,maskBits); ICONINFO ii{}; ii.fIcon=TRUE; ii.hbmMask=mask; ii.hbmColor=bitmap;
    HICON icon=CreateIconIndirect(&ii); DeleteObject(mask); DeleteObject(bitmap); return icon;
}

class App {
public:
    HWND window=nullptr;
    fs::path directory;
    bool test=false;
    std::string testConfig;
    int exitCode=0;
private:
    std::map<int,HWND> controls;
    std::map<int,RECT> layout;
    int scrollX=0,scrollY=0;
    std::vector<Host> hosts;
    Host draft;
    std::string activeId,activeName;
    int ruleSelection=-1;
    bool loading=false,trayAdded=false;
    HFONT font=nullptr;
    HICON connectedIcon=nullptr,disconnectedIcon=nullptr;
    UINT taskbarCreated=0;
    uint64_t generation=0,previewGeneration=0;
    Machine machine;
    std::unique_ptr<Child> child;
    bool querying=false;
    std::string effective,log,lastError;
    Host attemptHost;
    Readiness readiness;
    NOTIFYICONDATAW tray{};
public:
    explicit App(fs::path configDirectory):directory(std::move(configDirectory)) {}
    ~App() {
        ++generation; child.reset(); if(trayAdded) Shell_NotifyIconW(NIM_DELETE,&tray);
        if(connectedIcon) DestroyIcon(connectedIcon); if(disconnectedIcon) DestroyIcon(disconnectedIcon); if(font) DeleteObject(font);
    }
    void initialize() {
        connectedIcon=stateIcon(true); disconnectedIcon=stateIcon(false); taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        if(!test) { createControls(); updateScroll(); reload(); }
        notifyState();
    }
    void testStart(const std::string& id) { activeId=id; dispatch(machine.begin(false,GetTickCount64())); }
    void quit(int code=0) { exitCode=code; ++generation; KillTimer(window,TIMER_SESSION); child.reset(); machine.quit(); notifyState(); DestroyWindow(window); }
    void reconnect() {
        if(machine.busy() || machine.state==State::exiting || activeId.empty()) return;
        ++generation; child.reset(); dispatch(machine.begin(true,GetTickCount64()));
    }
    void timer() {
        KillTimer(window,TIMER_SESSION); auto action=machine.tick(GetTickCount64());
        if(action==Action::timeout) { ++generation; child.reset(); lastError="SSH 연결 준비 시간이 20초를 초과했습니다.\n"+(querying?effective:log); emitError(); dispatch(machine.failed(GetTickCount64())); }
        else dispatch(action);
    }
    void dispatch(Action action) {
        if(action==Action::start) startAttempt();
        else if(action==Action::quit) { quit(); return; }
        else if(action==Action::initialFailure) {
            notifyState();
            if(test) { quit(1); return; }
            if(authenticationFailure(lastError)) {
                auto message=wide(describeFailure(lastError))+L"\n\n외부 터미널에서 호스트 지문 확인, 로그인 또는 공개키 등록을 진행합니다. 입력은 터미널에서만 받습니다.";
                MessageBoxW(window,message.c_str(),L"SSH 인증 설정",MB_OK|MB_ICONWARNING);
                try { launchRecovery(activeId); } catch(const std::exception& e) { error(e.what()); }
            } else error(describeFailure(lastError));
            quit(1); return;
        }
        notifyState(); schedule();
    }
    void startAttempt() {
        ++generation; child.reset(); effective.clear(); log.clear(); querying=true;
        try {
            attemptHost=loadHost(directory,activeId); activeName=attemptHost.name;
            auto args=sshArguments(attemptHost,testConfig,true); startChild(args);
        } catch(const std::exception& e) { lastError=e.what(); emitError(); dispatch(machine.failed(GetTickCount64())); }
    }
    void startChild(const std::vector<std::string>& args) {
        auto token=generation; auto hwnd=window;
        child=std::make_unique<Child>(systemSSH(),args,[token,hwnd](bool ended,const std::string& text,DWORD code){
            auto* event=new ChildEvent{token,ended,code,text}; if(!PostMessageW(hwnd,WM_CHILD_EVENT,0,reinterpret_cast<LPARAM>(event))) delete event;
        });
    }
    void childEvent(std::unique_ptr<ChildEvent> event) {
        if(event->generation!=generation || machine.state==State::exiting) return;
        if(!event->ended) {
            if(test && !querying) {
                DWORD written=0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE),event->text.data(),static_cast<DWORD>(event->text.size()),&written,nullptr);
            }
            if(querying) { if(effective.size()<262144) effective.append(event->text,0,262144-effective.size()); }
            else {
                log+=event->text; if(log.size()>65536) log.erase(0,log.size()-65536);
                if(readiness.feed(event->text) && machine.busy() && machine.timer==Timer::connect) {
                    machine.ready(); lastError.clear(); notifyState(); schedule();
                    if(!test) ShowWindow(window,SW_HIDE);
                }
            }
            return;
        }
        child.reset();
        if(querying && event->code==0) {
            querying=false;
            if(std::none_of(attemptHost.forwards.begin(),attemptHost.forwards.end(),[](auto& f){return f.enabled;}) && inheritedForwards(effective).empty()) {
                lastError="활성 포워딩 규칙이 없습니다."; emitError(); dispatch(machine.failed(GetTickCount64())); return;
            }
            readiness=Readiness(remoteForwardCount(effective));
            if(test) outputLine("LF_QUERY remote_forwards="+std::to_string(remoteForwardCount(effective)));
            if(!test) set(Inherited,formatInherited(effective));
            try { startChild(sshArguments(attemptHost,testConfig,false)); }
            catch(const std::exception& e) { lastError=e.what(); emitError(); dispatch(machine.failed(GetTickCount64())); }
            return;
        }
        lastError=describeFailure(querying?effective:log); emitError();
        if(machine.state==State::connected) dispatch(machine.disconnected(GetTickCount64()));
        else dispatch(machine.failed(GetTickCount64()));
    }
    void emitError() { if(test) { auto flat=lastError; std::replace(flat.begin(),flat.end(),'\n',' '); std::replace(flat.begin(),flat.end(),'\r',' '); outputLine("LF_ERROR "+flat); } }
    void schedule() {
        KillTimer(window,TIMER_SESSION);
        if(machine.timer==Timer::none) return;
        auto now=GetTickCount64(); auto remaining=machine.deadline>now?machine.deadline-now:1;
        SetTimer(window,TIMER_SESSION,static_cast<UINT>(std::min<uint64_t>(remaining,USER_TIMER_MAXIMUM)),nullptr);
    }
    void notifyState() {
        if(test) { outputLine(std::string("LF_STATE ")+stateName(machine.state)); return; }
        set(Status,labelState(machine.state)+(lastError.empty()?L"":L"\r\n"+wide(lastError)));
        auto editing=machine.state==State::editing;
        for(int id:{HostList,NewHost,DeleteHost,Connect}) EnableWindow(controls[id],editing);
        EnableWindow(controls[Recovery],!machine.busy());
        if(machine.state!=State::editing && machine.state!=State::connecting && machine.state!=State::exiting) updateTray();
    }
    void updateTray() {
        tray={}; tray.cbSize=sizeof(tray); tray.hWnd=window; tray.uID=1; tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP; tray.uCallbackMessage=WM_TRAY;
        tray.hIcon=machine.state==State::connected?connectedIcon:disconnectedIcon;
        auto tip=wide(activeName)+L" — "+labelState(machine.state); wcsncpy_s(tray.szTip,tip.c_str(),_TRUNCATE);
        if(!trayAdded) { trayAdded=Shell_NotifyIconW(NIM_ADD,&tray)!=FALSE; tray.uVersion=NOTIFYICON_VERSION_4; if(trayAdded) Shell_NotifyIconW(NIM_SETVERSION,&tray); }
        else Shell_NotifyIconW(NIM_MODIFY,&tray);
    }
    void trayMenu() {
        HMENU menu=CreatePopupMenu(); AppendMenuW(menu,MF_STRING,Save,L"설정");
        if(machine.state!=State::connected) AppendMenuW(menu,MF_STRING|(machine.busy()?MF_GRAYED:0),Reconnect,L"재연결");
        AppendMenuW(menu,MF_STRING,Quit,L"종료"); POINT point{}; GetCursorPos(&point); SetForegroundWindow(window);
        auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr); DestroyMenu(menu); PostMessageW(window,WM_NULL,0,0);
        if(command==Save) showSettings(); else if(command==Reconnect) reconnect(); else if(command==Quit) quit();
    }
    void showSettings() {
        try { reload(activeId); } catch(const std::exception& e) { error(e.what()); }
        ShowWindow(window,SW_SHOW); SetForegroundWindow(window); notifyState();
    }
    void error(const std::string& message) { if(test) outputLine("LF_ERROR "+message); else MessageBoxW(window,wide(message).c_str(),L"LocalForwarder",MB_OK|MB_ICONERROR); }
    void set(int id,const std::wstring& text) { SetWindowTextW(controls.at(id),text.c_str()); }
    std::wstring get(int id) { return textOf(controls.at(id)); }
    void make(int id,const wchar_t* type,const wchar_t* title,DWORD style,int x,int y,int w,int h) {
        layout[id]={x,y,x+w,y+h};
        auto dpi=GetDpiForWindow(window); auto scale=[dpi](int n){return MulDiv(n,static_cast<int>(dpi),96);};
        auto control=CreateWindowExW(wcscmp(type,L"EDIT")==0?WS_EX_CLIENTEDGE:0,type,title,WS_CHILD|WS_VISIBLE|style,scale(x),scale(y),scale(w),scale(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!control) throw std::runtime_error(winError()); controls[id]=control; SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    }
    void label(int id,const wchar_t* title,int x,int y,int w) { make(id,L"STATIC",title,0,x,y,w,22); }
    void updateScroll() {
        if(test) return;
        RECT client{}; GetClientRect(window,&client);
        int oldX=scrollX,oldY=scrollY; auto dpi=GetDpiForWindow(window);
        SCROLLINFO horizontal{sizeof(horizontal),SIF_RANGE|SIF_PAGE|SIF_POS,0,MulDiv(830,dpi,96)-1,static_cast<UINT>(std::max(0L,client.right)),scrollX,0};
        SCROLLINFO vertical{sizeof(vertical),SIF_RANGE|SIF_PAGE|SIF_POS,0,MulDiv(745,dpi,96)-1,static_cast<UINT>(std::max(0L,client.bottom)),scrollY,0};
        SetScrollInfo(window,SB_HORZ,&horizontal,TRUE); SetScrollInfo(window,SB_VERT,&vertical,TRUE);
        horizontal.fMask=SIF_POS;vertical.fMask=SIF_POS;GetScrollInfo(window,SB_HORZ,&horizontal);GetScrollInfo(window,SB_VERT,&vertical);
        scrollX=horizontal.nPos;scrollY=vertical.nPos;
        if(oldX!=scrollX || oldY!=scrollY) ScrollWindowEx(window,oldX-scrollX,oldY-scrollY,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);
    }
    void scrollTo(int bar,int position) {
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int old=info.nPos;
        info.fMask=SIF_POS;info.nPos=position;SetScrollInfo(window,bar,&info,TRUE);GetScrollInfo(window,bar,&info);
        if(bar==SB_VERT) scrollY=info.nPos; else scrollX=info.nPos;
        if(info.nPos!=old) ScrollWindowEx(window,bar==SB_HORZ?old-info.nPos:0,bar==SB_VERT?old-info.nPos:0,nullptr,nullptr,nullptr,nullptr,SW_SCROLLCHILDREN|SW_INVALIDATE|SW_ERASE);
    }
    void scrollCommand(int bar,int command) {
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(window,bar,&info);int position=info.nPos;int line=MulDiv(24,GetDpiForWindow(window),96);
        switch(command) {
            case SB_LINEUP:position-=line;break;case SB_LINEDOWN:position+=line;break;
            case SB_PAGEUP:position-=static_cast<int>(info.nPage);break;case SB_PAGEDOWN:position+=static_cast<int>(info.nPage);break;
            case SB_THUMBTRACK:case SB_THUMBPOSITION:position=info.nTrackPos;break;
            case SB_TOP:position=info.nMin;break;case SB_BOTTOM:position=info.nMax;break;default:return;
        }
        scrollTo(bar,position);
    }
    void ensureFocusVisible() {
        if(test) return;auto focused=GetFocus();if(!focused || !IsChild(window,focused)) return;
        RECT area{},client{};GetWindowRect(focused,&area);MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&area),2);GetClientRect(window,&client);
        if(area.top<0) scrollTo(SB_VERT,scrollY+area.top);else if(area.bottom>client.bottom) scrollTo(SB_VERT,scrollY+area.bottom-client.bottom);
        if(area.left<0) scrollTo(SB_HORZ,scrollX+area.left);else if(area.right>client.right) scrollTo(SB_HORZ,scrollX+area.right-client.right);
    }
    void dpiChanged(RECT suggested) {
        auto dpi=GetDpiForWindow(window);scrollX=scrollY=0;
        HFONT old=font; font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        for(auto& [id,control]:controls) {
            auto rect=layout.at(id);SetWindowPos(control,nullptr,MulDiv(rect.left,dpi,96),MulDiv(rect.top,dpi,96),MulDiv(rect.right-rect.left,dpi,96),MulDiv(rect.bottom-rect.top,dpi,96),SWP_NOZORDER|SWP_NOACTIVATE);
            SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        }
        if(old)DeleteObject(old);
        if(controls.contains(Rules)) {int widths[]={60,120,440,150};for(int i=0;i<4;++i)ListView_SetColumnWidth(controls[Rules],i,MulDiv(widths[i],dpi,96));}
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromRect(&suggested,MONITOR_DEFAULTTONEAREST),&monitor);
        int width=std::min(suggested.right-suggested.left,monitor.rcWork.right-monitor.rcWork.left),height=std::min(suggested.bottom-suggested.top,monitor.rcWork.bottom-monitor.rcWork.top);
        SetWindowPos(window,nullptr,suggested.left,suggested.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);updateScroll();
    }

    void createControls() {
        font=CreateFontW(-MulDiv(10,GetDpiForWindow(window),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        label(300,L"접속 호스트",16,16,100); make(HostList,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,125,12,480,240);
        make(NewHost,L"BUTTON",L"추가",WS_TABSTOP,620,12,90,28); make(DeleteHost,L"BUTTON",L"삭제",WS_TABSTOP,720,12,90,28);
        label(301,L"이름",16,57,100); make(Name,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,125,53,685,26);
        label(302,L"SSH 별칭 / 주소",16,94,110); make(Target,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,125,90,685,26);
        label(303,L"사용자 (선택)",16,131,110); make(User,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,125,127,300,26);
        label(304,L"SSH 포트 (선택)",450,131,130); make(Port,L"EDIT",L"",ES_NUMBER|ES_AUTOHSCROLL|WS_TABSTOP,590,127,220,26);
        label(305,L"개인키 (선택)",16,168,110); make(Key,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,125,164,685,26);
        label(306,L"포워딩 규칙 — 내 컴퓨터 (127.0.0.1)에서만 접속",16,205,700);
        make(Rules,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER,16,230,794,150);
        ListView_SetExtendedListViewStyle(controls[Rules],LVS_EX_FULLROWSELECT|LVS_EX_CHECKBOXES|LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[]={L"사용",L"로컬 포트",L"목적지 호스트",L"목적지 포트"}; int widths[]={60,120,440,150};
        for(int i=0;i<4;++i) { LVCOLUMNW col{}; col.mask=LVCF_TEXT|LVCF_WIDTH; col.pszText=const_cast<LPWSTR>(titles[i]); col.cx=MulDiv(widths[i],GetDpiForWindow(window),96); ListView_InsertColumn(controls[Rules],i,&col); }
        label(307,L"로컬 포트",16,392,110); label(308,L"목적지 호스트",145,392,250); label(309,L"목적지 포트",490,392,130);
        make(LocalPort,L"EDIT",L"",ES_NUMBER|WS_TABSTOP,16,418,115,26); make(RemoteHost,L"EDIT",L"localhost",ES_AUTOHSCROLL|WS_TABSTOP,145,418,330,26); make(RemotePort,L"EDIT",L"",ES_NUMBER|WS_TABSTOP,490,418,130,26);
        make(RuleSave,L"BUTTON",L"규칙 적용",WS_TABSTOP,635,418,175,26); make(RuleNew,L"BUTTON",L"새 규칙",WS_TABSTOP,635,452,82,26); make(RuleDelete,L"BUTTON",L"규칙 삭제",WS_TABSTOP,728,452,82,26);
        label(310,L"기존 SSH 포워딩 (읽기 전용, 함께 적용됨)",16,466,590); make(Preview,L"BUTTON",L"다시 조회",WS_TABSTOP,700,490,110,26);
        make(Inherited,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,16,495,670,75);
        make(Status,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,16,585,794,95);
        make(Recovery,L"BUTTON",L"인증 설정",WS_TABSTOP,16,695,125,32); make(Save,L"BUTTON",L"설정 저장",WS_TABSTOP,515,695,140,32); make(Connect,L"BUTTON",L"연결",WS_TABSTOP|BS_DEFPUSHBUTTON,670,695,140,32);
    }
    std::wstring formatInherited(const std::string& value) {
        auto lines=inheritedForwards(value); std::string result;
        for(auto& line:lines) result+=line+"\r\n";
        return result.empty()?L"기존 포워딩 없음":wide(result);
    }
    Host collect() {
        Host h=draft; h.name=utf8(get(Name)); h.target=utf8(get(Target)); h.user=utf8(get(User)); h.identityFile=utf8(get(Key));
        auto p=get(Port); h.port=p.empty()?std::optional<int>{}:std::optional<int>{parsePort(p)};
        for(size_t i=0;i<h.forwards.size();++i) h.forwards[i].enabled=ListView_GetCheckState(controls[Rules],static_cast<int>(i))!=FALSE;
        validate(h); return h;
    }
    void reload(const std::string& selected={}) {
        hosts=loadConfig(directory).hosts; SendMessageW(controls[HostList],CB_RESETCONTENT,0,0); int index=0;
        for(size_t i=0;i<hosts.size();++i) { SendMessageW(controls[HostList],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(wide(hosts[i].name).c_str())); if(hosts[i].id==selected) index=static_cast<int>(i); }
        if(!selected.empty() && std::none_of(hosts.begin(),hosts.end(),[&](auto& h){return h.id==selected;})) throw std::runtime_error("현재 호스트가 설정에서 삭제되었습니다. 재연결할 수 없습니다.");
        if(hosts.empty()) { draft={}; draft.id=newId(); fill(); }
        else { SendMessageW(controls[HostList],CB_SETCURSEL,index,0); draft=hosts[index]; fill(); preview(); }
    }
    void fill() {
        loading=true; set(Name,wide(draft.name)); set(Target,wide(draft.target)); set(User,wide(draft.user)); set(Port,draft.port?std::to_wstring(*draft.port):L""); set(Key,wide(draft.identityFile));
        ListView_DeleteAllItems(controls[Rules]);
        for(size_t i=0;i<draft.forwards.size();++i) {
            auto& f=draft.forwards[i]; LVITEMW item{}; item.mask=LVIF_TEXT; item.iItem=static_cast<int>(i); item.pszText=const_cast<LPWSTR>(L""); ListView_InsertItem(controls[Rules],&item);
            auto local=std::to_wstring(f.localPort),host=wide(f.remoteHost),remote=std::to_wstring(f.remotePort);
            ListView_SetItemText(controls[Rules],item.iItem,1,local.data()); ListView_SetItemText(controls[Rules],item.iItem,2,host.data()); ListView_SetItemText(controls[Rules],item.iItem,3,remote.data()); ListView_SetCheckState(controls[Rules],item.iItem,f.enabled);
        }
        loading=false; newRule();
    }
    void newRule() { ruleSelection=-1; set(LocalPort,L""); set(RemoteHost,L"localhost"); set(RemotePort,L""); ListView_SetItemState(controls[Rules],-1,0,LVIS_SELECTED); }
    void ruleChanged(NMLISTVIEW* notification) {
        if(loading || notification->iItem<0 || static_cast<size_t>(notification->iItem)>=draft.forwards.size()) return;
        draft.forwards[notification->iItem].enabled=ListView_GetCheckState(controls[Rules],notification->iItem)!=FALSE;
        if((notification->uNewState & LVIS_SELECTED) && !(notification->uOldState & LVIS_SELECTED)) {
            ruleSelection=notification->iItem; auto& f=draft.forwards[ruleSelection]; set(LocalPort,std::to_wstring(f.localPort)); set(RemoteHost,wide(f.remoteHost)); set(RemotePort,std::to_wstring(f.remotePort));
        }
    }
    void applyRule() {
        // Preserve unsaved host fields while rebuilding the rule list.
        draft.name=utf8(get(Name)); draft.target=utf8(get(Target)); draft.user=utf8(get(User)); draft.identityFile=utf8(get(Key));
        auto port=get(Port); draft.port=port.empty()?std::optional<int>{}:std::optional<int>{parsePort(port)};
        Forward f; f.id=ruleSelection<0?newId():draft.forwards[ruleSelection].id; f.enabled=ruleSelection<0?true:draft.forwards[ruleSelection].enabled;
        f.localPort=parsePort(get(LocalPort)); f.remoteHost=utf8(get(RemoteHost)); f.remotePort=parsePort(get(RemotePort));
        auto updated=draft; if(ruleSelection<0) updated.forwards.push_back(f); else updated.forwards[ruleSelection]=f;
        validate(updated); draft=updated; fill();
    }
    void preview() {
        auto h=collect(); auto token=++previewGeneration; auto hwnd=window; auto ssh=systemSSH(); auto args=sshArguments(h,testConfig,true);
        set(Inherited,L"SSH 설정 조회 중…");
        std::thread([h,token,hwnd,ssh,args] {
            std::string result;
            try { auto r=capture(ssh,args); if(r.code) result="조회 실패: "+r.text; else { for(auto& line:inheritedForwards(r.text)) result+=line+"\r\n"; if(result.empty()) result="기존 포워딩 없음"; } }
            catch(const std::exception& e) { result=e.what(); }
            auto* event=new PreviewEvent{token,result}; if(!PostMessageW(hwnd,WM_PREVIEW,0,reinterpret_cast<LPARAM>(event))) delete event;
        }).detach();
    }
    void command(int id,int notification) {
        try {
            if(id==HostList && notification==CBN_SELCHANGE) { auto index=SendMessageW(controls[HostList],CB_GETCURSEL,0,0); if(index>=0 && static_cast<size_t>(index)<hosts.size()) { draft=loadHost(directory,hosts[index].id); fill(); preview(); } }
            else if(id==NewHost) { ++previewGeneration; draft={}; draft.id=newId(); SendMessageW(controls[HostList],CB_SETCURSEL,-1,0); fill(); set(Inherited,L""); }
            else if(id==DeleteHost) { if(MessageBoxW(window,L"이 호스트 설정을 삭제할까요?",L"LocalForwarder",MB_YESNO|MB_ICONQUESTION)==IDYES) { deleteHost(directory,draft.id); reload(); } }
            else if(id==RuleNew) newRule();
            else if(id==RuleSave) applyRule();
            else if(id==RuleDelete) { if(ruleSelection>=0) { draft=collect(); draft.forwards.erase(draft.forwards.begin()+ruleSelection); fill(); } }
            else if(id==Preview) preview();
            else if(id==Save || id==Connect || id==Recovery) {
                auto h=collect(); saveHost(directory,h); draft=h;
                if(id==Connect) { activeId=h.id; dispatch(machine.begin(false,GetTickCount64())); }
                else if(id==Recovery) {
                    if(MessageBoxW(window,L"외부 터미널에서 지문 확인, 비밀번호 로그인 또는 공개키 등록을 진행합니다. 이 앱은 종료됩니다.",L"인증 설정",MB_OKCANCEL|MB_ICONINFORMATION)==IDOK) { launchRecovery(h.id); quit(); }
                }
                else {
                    if(machine.state==State::connected) {
                        auto answer=MessageBoxW(window,L"설정을 저장했습니다. 지금 연결을 끊고 재연결할까요?\n아니요: 다음 연결(자동 재연결 포함)부터 적용",L"설정 적용",MB_YESNO|MB_ICONQUESTION);
                        if(answer==IDYES) reconnect();
                    } else if(machine.state==State::editing) reload(h.id);
                }
            }
        } catch(const std::exception& e) { error(e.what()); }
    }
    LRESULT message(UINT msg,WPARAM w,LPARAM l) {
        if(msg==taskbarCreated && taskbarCreated) { trayAdded=false; if(machine.state!=State::editing) updateTray(); return 0; }
        switch(msg) {
            case WM_COMMAND: command(LOWORD(w),HIWORD(w)); return 0;
            case WM_SIZE: updateScroll();return 0;
            case WM_VSCROLL:scrollCommand(SB_VERT,LOWORD(w));return 0;
            case WM_HSCROLL:scrollCommand(SB_HORZ,LOWORD(w));return 0;
            case WM_MOUSEWHEEL:{SCROLLINFO info{sizeof(info),SIF_POS};GetScrollInfo(window,SB_VERT,&info);scrollTo(SB_VERT,info.nPos-MulDiv(GET_WHEEL_DELTA_WPARAM(w),72*static_cast<int>(GetDpiForWindow(window)),WHEEL_DELTA*96));return 0;}
            case WM_DPICHANGED:if(!test)dpiChanged(*reinterpret_cast<RECT*>(l));return 0;
            case WM_NOTIFY: { auto* n=reinterpret_cast<NMHDR*>(l); if(n->idFrom==Rules && n->code==LVN_ITEMCHANGED) ruleChanged(reinterpret_cast<NMLISTVIEW*>(l)); return 0; }
            case WM_CHILD_EVENT: childEvent(std::unique_ptr<ChildEvent>(reinterpret_cast<ChildEvent*>(l))); return 0;
            case WM_PREVIEW: { std::unique_ptr<PreviewEvent> e(reinterpret_cast<PreviewEvent*>(l)); if(e->generation==previewGeneration && !test) set(Inherited,wide(e->text)); return 0; }
            case WM_COMMAND_LINE: if(w==1) quit(); else if(w==2) reconnect(); return 0;
            case WM_TIMER: if(w==TIMER_SESSION) timer(); return 0;
            case WM_POWERBROADCAST: if(w==PBT_APMRESUMEAUTOMATIC || w==PBT_APMRESUMESUSPEND) timer(); return TRUE;
            case WM_TRAY: if(LOWORD(l)==WM_CONTEXTMENU || LOWORD(l)==WM_RBUTTONUP || LOWORD(l)==NIN_SELECT || LOWORD(l)==NIN_KEYSELECT) trayMenu(); return 0;
            case WM_CLOSE: if(trayAdded) ShowWindow(window,SW_HIDE); else quit(); return 0;
            case WM_QUERYENDSESSION: return TRUE;
            case WM_ENDSESSION: if(w) quit(); return 0;
            case WM_DESTROY: PostQuitMessage(exitCode); return 0;
        }
        return DefWindowProcW(window,msg,w,l);
    }
};
LRESULT CALLBACK windowProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    App* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if(msg==WM_NCCREATE) { app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams); app->window=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app)); }
    if(app) { try { return app->message(msg,w,l); } catch(const std::exception& e) { app->error(e.what()); return 0; } }
    return DefWindowProcW(hwnd,msg,w,l);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc=0; auto* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    try {
        fs::path directory=homeDirectory()/L".LocalForwarder";
        if(argc==3 && std::wstring(argv[1])==L"--setup-auth") { auto id=utf8(argv[2]); LocalFree(argv); auto result=setupAuthentication(directory,id); CoUninitialize(); return result; }
        bool test=argc==5 && std::wstring(argv[1])==L"--test-session";
        std::string profile,testConfig;
        if(test) { directory=argv[2]; profile=utf8(argv[3]); testConfig=utf8(argv[4]); }
        else if(argc!=1) throw std::runtime_error("지원하지 않는 실행 인자입니다.");
        LocalFree(argv); argv=nullptr;
        INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES}; InitCommonControlsEx(&cc);
        App app(directory); app.test=test; app.testConfig=testConfig;
        WNDCLASSW cls{}; cls.hInstance=instance; cls.lpfnWndProc=windowProc; cls.lpszClassName=L"LocalForwarderWindow"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        if(!RegisterClassW(&cls)) throw std::runtime_error(winError());
        DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_THICKFRAME|WS_VSCROLL|WS_HSCROLL;
        auto dpi=GetDpiForSystem(); RECT rect{0,0,MulDiv(830,dpi,96),MulDiv(745,dpi,96)}; AdjustWindowRectExForDpi(&rect,style,FALSE,0,dpi);
        POINT cursor{};GetCursorPos(&cursor);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&monitor);
        int width=std::min(rect.right-rect.left,monitor.rcWork.right-monitor.rcWork.left),height=std::min(rect.bottom-rect.top,monitor.rcWork.bottom-monitor.rcWork.top);
        int x=monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,y=monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2;
        HWND hwnd=CreateWindowExW(0,cls.lpszClassName,L"LocalForwarder",style,x,y,width,height,nullptr,nullptr,instance,&app);
        if(!hwnd) throw std::runtime_error(winError());
        app.initialize();
        if(test) {
            std::thread([hwnd] {
                HANDLE input=GetStdHandle(STD_INPUT_HANDLE); std::string line; char buffer[256]; DWORD count=0;
                while(ReadFile(input,buffer,sizeof(buffer),&count,nullptr) && count) {
                    for(DWORD i=0;i<count;++i) {
                        if(buffer[i]=='\n') { if(line=="quit") PostMessageW(hwnd,WM_COMMAND_LINE,1,0); if(line=="reconnect") PostMessageW(hwnd,WM_COMMAND_LINE,2,0); line.clear(); }
                        else if(buffer[i]!='\r' && line.size()<1024) line+=buffer[i];
                    }
                }
            }).detach();
            app.testStart(profile);
        } else ShowWindow(hwnd,SW_SHOW);
        MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0) { if(!IsDialogMessageW(hwnd,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } else app.ensureFocusVisible(); }
        CoUninitialize(); return static_cast<int>(msg.wParam);
    } catch(const std::exception& e) {
        if(argv) LocalFree(argv);
        outputLine("LF_ERROR "+std::string(e.what()));
        // Headless test invocations must never block on a dialog.
        if(std::wstring(GetCommandLineW()).find(L"--test-session")==std::wstring::npos) MessageBoxW(nullptr,wide(e.what()).c_str(),L"LocalForwarder",MB_OK|MB_ICONERROR);
        CoUninitialize(); return 1;
    }
}
