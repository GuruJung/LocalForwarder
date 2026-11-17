#include "platform.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "yyjson.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <sddl.h>
#include <netlistmgr.h>
#include <ocidl.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using namespace lf;
namespace {
constexpr UINT WM_TASK=WM_APP+1,WM_TRAY=WM_APP+2,WM_OPEN_SETTINGS=WM_APP+3;
constexpr wchar_t AppClass[]=L"LocalForwarder.MultiHost.Window";
constexpr wchar_t FormClass[]=L"LocalForwarder.MultiHost.Form";
constexpr wchar_t StatusClass[]=L"LocalForwarder.MultiHost.Status";
enum Control:int {Hosts=100,NewHost,DeleteHost,Name,Target,User,Port,Key,ChooseKey,AutoConnect,ExitFailure,RetryRefused,Rules,RuleNew,RuleDelete,LocalPort,RemoteHost,RemotePort,Save,Connect,Authentication,ConnectionStatus,CommandPreview,Validation,Okay,CancelButton,StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose};
std::wstring textOf(HWND control){int n=GetWindowTextLengthW(control);std::wstring out(n+1,0);GetWindowTextW(control,out.data(),n+1);out.resize(n);return out;}
void centerInWorkArea(HWND window,int desiredWidth,int desiredHeight,HWND owner=nullptr){
    POINT point{};GetCursorPos(&point);MONITORINFO monitor{sizeof(monitor)};
    HMONITOR display=owner?MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST):MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(display,&monitor);int dpi=GetDpiForWindow(window);int width=std::min(MulDiv(desiredWidth,dpi,96),int(monitor.rcWork.right-monitor.rcWork.left)-24);int height=std::min(MulDiv(desiredHeight,dpi,96),int(monitor.rcWork.bottom-monitor.rcWork.top)-24);
    SetWindowPos(window,nullptr,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
}
void setText(HWND control,const std::wstring& text){if(textOf(control)!=text)SetWindowTextW(control,text.c_str());}
std::wstring nativeLines(std::string text){std::string out;for(char c:text){if(c=='\n'&&(out.empty()||out.back()!='\r'))out+='\r';out+=c;}return wide(out);}

// Posting owns the closure until delivery or shutdown; HWND reuse cannot receive old callbacks.
class EventLoop:public std::enable_shared_from_this<EventLoop>{
    std::mutex lock_;HWND window_=nullptr;UINT_PTR next_=10;
    std::map<UINT_PTR,std::function<void()>> timers_;
public:
    void attach(HWND window){std::lock_guard lock(lock_);window_=window;}
    void post(std::function<void()> work){std::lock_guard lock(lock_);if(!window_)return;auto* task=new std::function<void()>(std::move(work));if(!PostMessageW(window_,WM_TASK,0,reinterpret_cast<LPARAM>(task)))delete task;}
    Post poster(){std::weak_ptr<EventLoop> weak=shared_from_this();return [weak](std::function<void()> work){if(auto self=weak.lock())self->post(std::move(work));};}
    Schedule scheduler(){std::weak_ptr<EventLoop> weak=shared_from_this();return [weak](double seconds,std::function<void()> work)->lf::Cancel{
        auto self=weak.lock();if(!self)return []{};UINT_PTR id=++self->next_;
        self->timers_[id]=std::move(work);
        if(!SetTimer(self->window_,id,static_cast<UINT>(std::clamp(seconds*1000.0,1.0,double(USER_TIMER_MAXIMUM))),nullptr)){self->timers_.erase(id);throw std::runtime_error("예약 타이머를 만들 수 없습니다.");}
        return [weak,id]{if(auto current=weak.lock()){KillTimer(current->window_,id);current->timers_.erase(id);}};
    };}
    void timer(UINT_PTR id){auto it=timers_.find(id);if(it==timers_.end())return;KillTimer(window_,id);auto work=std::move(it->second);timers_.erase(it);work();}
    void detach(){std::lock_guard lock(lock_);for(auto& [id,work]:timers_)KillTimer(window_,id);timers_.clear();MSG msg{};while(PeekMessageW(&msg,window_,WM_TASK,WM_TASK,PM_REMOVE))delete reinterpret_cast<std::function<void()>*>(msg.lParam);window_=nullptr;}
};
class SingleInstance {
    Handle mutex_,ready_;std::wstring sid_;
public:
    bool acquire(const std::wstring& isolation={}){
        HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw std::runtime_error(winError());Handle token(raw);
        DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
        if(!GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size))throw std::runtime_error(winError());
        LPWSTR stringSid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&stringSid))throw std::runtime_error(winError());sid_=stringSid;LocalFree(stringSid);std::wstring ownerSID=sid_;if(!isolation.empty())sid_+=L".test."+isolation;
        PSECURITY_DESCRIPTOR descriptor=nullptr;std::wstring sddl=L"D:P(A;;GA;;;"+ownerSID+L")";
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error(winError());
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        mutex_.reset(CreateMutexW(&security,FALSE,(L"Local\\LocalForwarder.Instance."+sid_).c_str()));DWORD error=GetLastError();
        ready_.reset(CreateEventW(&security,TRUE,FALSE,(L"Local\\LocalForwarder.Ready."+sid_).c_str()));LocalFree(descriptor);
        if(!mutex_||!ready_)throw std::runtime_error(winError());
        if(error!=ERROR_ALREADY_EXISTS)return true;
        if(WaitForSingleObject(ready_.value,5000)!=WAIT_OBJECT_0)throw std::runtime_error("기존 LocalForwarder가 아직 시작 중입니다. 잠시 뒤 다시 실행하세요.");
        HWND window=FindWindowW(AppClass,(L"LocalForwarder.Instance."+sid_).c_str());DWORD_PTR result=0;
        if(!window||!SendMessageTimeoutW(window,WM_OPEN_SETTINGS,0,0,SMTO_ABORTIFHUNG,5000,&result))throw std::runtime_error("기존 LocalForwarder에 설정 열기를 요청하지 못했습니다.");
        return false;
    }
    std::wstring windowTitle()const{return L"LocalForwarder.Instance."+sid_;}
    void ready(){SetEvent(ready_.value);}
};
bool networkAvailable(NLM_CONNECTIVITY state){return (state&(NLM_CONNECTIVITY_IPV4_SUBNET|NLM_CONNECTIVITY_IPV4_LOCALNETWORK|NLM_CONNECTIVITY_IPV4_INTERNET|NLM_CONNECTIVITY_IPV6_SUBNET|NLM_CONNECTIVITY_IPV6_LOCALNETWORK|NLM_CONNECTIVITY_IPV6_INTERNET))!=0;}
class NetworkSink final:public INetworkListManagerEvents{
    LONG refs_=1;Post post_;std::function<void(bool)> changed_;
public:
    NetworkSink(Post post,std::function<void(bool)> changed):post_(std::move(post)),changed_(std::move(changed)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==__uuidof(INetworkListManagerEvents)){*out=static_cast<INetworkListManagerEvents*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}
    ULONG STDMETHODCALLTYPE Release()override{auto count=InterlockedDecrement(&refs_);if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE ConnectivityChanged(NLM_CONNECTIVITY state)override{auto changed=changed_;post_([changed,state]{changed(networkAvailable(state));});return S_OK;}
};
class NetworkMonitor{
    INetworkListManager* manager_=nullptr;IConnectionPoint* point_=nullptr;DWORD cookie_=0;
public:
    ~NetworkMonitor(){stop();}
    void start(Post post,std::function<void(bool)> changed){
        HRESULT result=CoCreateInstance(CLSID_NetworkListManager,nullptr,CLSCTX_ALL,__uuidof(INetworkListManager),reinterpret_cast<void**>(&manager_));
        if(FAILED(result))throw std::runtime_error("Windows 네트워크 알림을 구독하지 못했습니다.");
        IConnectionPointContainer* container=nullptr;
        result=manager_->QueryInterface(IID_IConnectionPointContainer,reinterpret_cast<void**>(&container));
        if(SUCCEEDED(result)){result=container->FindConnectionPoint(__uuidof(INetworkListManagerEvents),&point_);container->Release();}
        if(FAILED(result))throw std::runtime_error("Windows 네트워크 이벤트를 찾지 못했습니다.");
        auto* sink=new NetworkSink(std::move(post),changed);result=point_->Advise(sink,&cookie_);sink->Release();
        if(FAILED(result))throw std::runtime_error("Windows 네트워크 이벤트를 연결하지 못했습니다.");
        NLM_CONNECTIVITY connectivity{};if(SUCCEEDED(manager_->GetConnectivity(&connectivity)))changed(networkAvailable(connectivity));
    }
    void stop(){if(point_){if(cookie_)point_->Unadvise(cookie_);point_->Release();point_=nullptr;cookie_=0;}if(manager_){manager_->Release();manager_=nullptr;}}
};
HICON forwardingIcon(){
    constexpr int n=32;BITMAPV5HEADER bi{};bi.bV5Size=sizeof(bi);bi.bV5Width=n;bi.bV5Height=-n;bi.bV5Planes=1;bi.bV5BitCount=32;bi.bV5Compression=BI_BITFIELDS;bi.bV5RedMask=0xff0000;bi.bV5GreenMask=0xff00;bi.bV5BlueMask=0xff;bi.bV5AlphaMask=0xff000000;
    void* pixels=nullptr;HDC dc=GetDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,reinterpret_cast<BITMAPINFO*>(&bi),DIB_RGB_COLORS,&pixels,nullptr,0);ReleaseDC(nullptr,dc);if(!bitmap)return nullptr;
    auto* p=static_cast<DWORD*>(pixels);for(int y=0;y<n;++y)for(int x=0;x<n;++x){bool forward=(y>=8&&y<=11&&x>=4&&x<=26)||(x>=21&&x<=27&&std::abs(y-10)<=27-x);bool back=(y>=21&&y<=24&&x>=5&&x<=27)||(x>=4&&x<=10&&std::abs(y-22)<=x-4);p[y*n+x]=forward||back?0xff357ac9:0;}
    BYTE bits[n*n/8]{};HBITMAP mask=CreateBitmap(n,n,1,1,bits);ICONINFO ii{};ii.fIcon=TRUE;ii.hbmMask=mask;ii.hbmColor=bitmap;HICON icon=CreateIconIndirect(&ii);DeleteObject(bitmap);DeleteObject(mask);return icon;
}
class PreviewSession final:public SSHSessionProtocol {
    HostRuntime value_;
public:
    const HostRuntime& runtime()const override{return value_;}
    void start(const Host& host)override{value_.executionHost=host;value_.state=ConnectionState::connected;value_.forwards.clear();for(const auto& f:host.forwards)if(f.enabled)value_.forwards.push_back({f.id,forwarding(f),"앱 설정","test listener",ForwardState::ready});if(onChange)onChange();}
    void stop(std::function<void()> done)override{value_.state=ConnectionState::disconnected;for(auto& f:value_.forwards)f.state=ForwardState::disconnected;if(onChange)onChange();done();}
};
LRESULT CALLBACK editProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){
    if(msg==WM_KEYDOWN&&w=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(hwnd,EM_SETSEL,0,-1);return 0;}
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,editProc,1);
    return DefSubclassProc(hwnd,msg,w,l);
}
class App;
LRESULT CALLBACK appProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK formProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK statusProc(HWND,UINT,WPARAM,LPARAM);
class App{
    HINSTANCE instance_;fs::path directory_;bool test_,start_,openConfig_,uiTest_,quitting_=false,loading_=false,trayAdded_=false,menuOpen_=false;
    std::string sshConfig_,singleID_;std::optional<ConnectionState> previous_;
    std::shared_ptr<EventLoop> loop_=std::make_shared<EventLoop>();std::shared_ptr<int> lifetime_=std::make_shared<int>(0);
    std::unique_ptr<ConnectionManager> manager_;SettingsModel settings_;NetworkMonitor network_;
    HWND window_=nullptr,settingsWindow_=nullptr,form_=nullptr,status_=nullptr;HFONT font_=nullptr,statusFont_=nullptr;HICON icon_=nullptr;
    std::map<int,HWND> controls_;int rule_=-1,scroll_=0;UINT taskbarCreated_=0;NOTIFYICONDATAW tray_{};std::set<std::string> authPending_;
    int scale(int value)const{return MulDiv(value,window_?GetDpiForWindow(settingsWindow_?settingsWindow_:window_):96,96);}
    HWND control(int id)const{auto it=controls_.find(id);return it==controls_.end()?nullptr:it->second;}
    void set(int id,const std::wstring& value){if(control(id))setText(control(id),value);}
    std::wstring get(int id)const{return control(id)?textOf(control(id)):L"";}
    void error(const std::string& text){if(test_){auto flat=text;std::replace(flat.begin(),flat.end(),'\n',' ');outputLine("LF_ERROR "+flat);}else if(settings_.open)set(Validation,nativeLines(text));else MessageBoxW(window_,wide(text).c_str(),L"LocalForwarder",MB_OK|MB_ICONWARNING);}
    HWND make(HWND parent,int id,const wchar_t* kind,const wchar_t* text,DWORD style){auto h=CreateWindowExW((std::wstring(kind)==L"EDIT")?WS_EX_CLIENTEDGE:0,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);if(!h)throw std::runtime_error(winError());controls_[id]=h;if(std::wstring(kind)==L"EDIT")SetWindowSubclass(h,editProc,1,0);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);return h;}
    void place(int id,int x,int y,int width,int height){if(control(id)){UINT dpi=GetDpiForWindow(GetParent(control(id)));MoveWindow(control(id),MulDiv(x,dpi,96),MulDiv(y,dpi,96),MulDiv(std::max(1,width),dpi,96),MulDiv(std::max(1,height),dpi,96),TRUE);}}
    void label(int id,const wchar_t* text){make(form_,id,L"STATIC",text,0);}
    void updateFont(){HFONT old=font_;font_=CreateFontW(-MulDiv(10,GetDpiForWindow(settingsWindow_?settingsWindow_:window_),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");for(auto& [id,h]:controls_)SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);if(old)DeleteObject(old);if(status_)updateStatusFont();}
    void updateStatusFont(){if(!status_)return;HFONT old=statusFont_;statusFont_=CreateFontW(-MulDiv(10,GetDpiForWindow(status_),72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");for(int id:{StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose})if(control(id))SendMessageW(control(id),WM_SETFONT,reinterpret_cast<WPARAM>(statusFont_),TRUE);if(old)DeleteObject(old);}
    int choose(const wchar_t* title,const wchar_t* body,const std::vector<std::pair<int,const wchar_t*>>& choices,int defaultButton){std::vector<TASKDIALOG_BUTTON> buttons;for(auto& choice:choices)buttons.push_back({choice.first,choice.second});TASKDIALOGCONFIG config{};config.cbSize=sizeof(config);config.hwndParent=settingsWindow_;config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;config.pszWindowTitle=L"LocalForwarder";config.pszMainInstruction=title;config.pszContent=body;config.cButtons=static_cast<UINT>(buttons.size());config.pButtons=buttons.data();config.nDefaultButton=defaultButton;int result=IDCANCEL;if(FAILED(TaskDialogIndirect(&config,&result,nullptr,nullptr)))throw std::runtime_error("확인 대화상자를 열 수 없습니다.");return result;}
public:
    App(HINSTANCE instance,fs::path directory,bool test,bool start,bool openConfig,std::string config,std::string single,bool uiTest=false):instance_(instance),directory_(std::move(directory)),test_(test),start_(start),openConfig_(openConfig),uiTest_(uiTest),sshConfig_(std::move(config)),singleID_(std::move(single)){}
    ~App(){lifetime_.reset();network_.stop();if(loop_)loop_->detach();manager_.reset();if(trayAdded_)Shell_NotifyIconW(NIM_DELETE,&tray_);if(status_)DestroyWindow(status_);if(settingsWindow_)DestroyWindow(settingsWindow_);if(window_)DestroyWindow(window_);if(font_)DeleteObject(font_);if(statusFont_)DeleteObject(statusFont_);if(icon_)DestroyIcon(icon_);}
    void create(const std::wstring& title){
        WNDCLASSW cls{};cls.hInstance=instance_;cls.lpfnWndProc=appProc;cls.lpszClassName=AppClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error(winError());
        cls.lpfnWndProc=formProc;cls.lpszClassName=FormClass;if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error(winError());cls.lpfnWndProc=statusProc;cls.lpszClassName=StatusClass;if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error(winError());
        window_=CreateWindowExW(0,AppClass,title.c_str(),WS_OVERLAPPED,0,0,1,1,nullptr,nullptr,instance_,this);if(!window_)throw std::runtime_error(winError());loop_->attach(window_);
        auto post=loop_->poster();auto schedule=loop_->scheduler();auto directory=directory_;auto config=sshConfig_;
        manager_=std::make_unique<ConnectionManager>(StoreAccess{[directory]{return loadConfig(directory);},[directory](const std::string& id){return loadHost(directory,id);},[directory](const std::string& id){deleteHost(directory,id);}},[post,schedule,config,uiTest=uiTest_]()->std::shared_ptr<SSHSessionProtocol>{if(uiTest)return std::make_shared<PreviewSession>();return std::make_shared<SSHSession>(post,schedule,config);},schedule);
        manager_->onChange=[this]{refresh();};manager_->onError=[this](const std::string&,const std::string& text){if(test_)error(text);else refresh();};manager_->onExit=[this]{finishQuit();};
        if(!test_){icon_=static_cast<HICON>(LoadImageW(instance_,MAKEINTRESOURCEW(2),IMAGE_ICON,32,32,0));if(!icon_)icon_=forwardingIcon();taskbarCreated_=RegisterWindowMessageW(L"TaskbarCreated");updateTray();try{std::weak_ptr<int> alive=lifetime_;network_.start(post,[this,alive](bool available){if(!alive.expired()&&!quitting_)manager_->networkChanged(available);});}catch(const std::exception& e){error(e.what());}}
        try{manager_->reloadConfiguration();if(test_){if(!singleID_.empty())manager_->connect(singleID_);else if(start_)manager_->startAll();}else if(openConfig_||!fs::exists(directory_/L"config.json"))openSettings({},true);else manager_->startAll();}
        catch(const std::exception& e){if(!test_)openSettings({},true);error(std::string(e.what())+"\n기존 설정 파일은 보존했습니다.");}
        if(test_&&!uiTest_){snapshot();outputLine("LF_READY");startInput();}
    }
    HWND settingsWindow()const{return settingsWindow_;}HWND statusWindow()const{return status_;}
    bool preprocess(MSG& message){
        HWND dialog=status_?status_:(settings_.open?settingsWindow_:nullptr);
        if(dialog&&message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){if(status_)closeStatus();else closeSettings(false);return true;}
        if(dialog&&message.message==WM_KEYDOWN&&message.wParam==VK_RETURN&&dialog==status_){closeStatus();return true;}
        if(dialog&&IsDialogMessageW(dialog,&message)){ensureFocusVisible();return true;}return false;
    }
private:
    std::map<std::string,lf::Cancel> authObservers_;
    HMENU activeMenu_=nullptr;std::map<std::string,HMENU> hostMenus_;std::map<UINT,std::pair<std::string,int>> menuActions_;
    void createSettings(){
        settingsWindow_=CreateWindowExW(WS_EX_CONTROLPARENT,AppClass,L"LocalForwarder 설정",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,scale(1080),scale(850),window_,nullptr,instance_,this);
        if(!settingsWindow_)throw std::runtime_error(winError());centerInWorkArea(settingsWindow_,1080,850);updateFont();SendMessageW(settingsWindow_,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon_));
        make(settingsWindow_,NewHost,L"BUTTON",L"새로 만들기",WS_TABSTOP);make(settingsWindow_,DeleteHost,L"BUTTON",L"호스트 삭제…",WS_TABSTOP);
        make(settingsWindow_,Hosts,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP|WS_BORDER);
        make(settingsWindow_,CancelButton,L"BUTTON",L"Cancel",WS_TABSTOP);make(settingsWindow_,Okay,L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON);
        form_=CreateWindowExW(WS_EX_CONTROLPARENT,FormClass,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL,0,0,1,1,settingsWindow_,nullptr,instance_,this);if(!form_)throw std::runtime_error(winError());
        label(300,L"이름");label(301,L"SSH 별칭 / 주소");label(302,L"사용자명 (선택)");label(303,L"SSH 포트 (선택)");label(304,L"개인키 (선택)");
        for(int id:{Name,Target,User,Port,Key})make(form_,id,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
        make(form_,ChooseKey,L"BUTTON",L"개인키 파일 선택…",WS_TABSTOP);
        make(form_,AutoConnect,L"BUTTON",L"자동 연결",BS_AUTOCHECKBOX|WS_TABSTOP);make(form_,ExitFailure,L"BUTTON",L"ExitOnForwardFailure",BS_AUTOCHECKBOX|WS_TABSTOP);make(form_,RetryRefused,L"BUTTON",L"연결 거부 시 재시도",BS_AUTOCHECKBOX|WS_TABSTOP);
        label(305,L"포워딩 규칙 — 앱 규칙은 127.0.0.1에 바인딩합니다.");
        make(form_,Rules,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER);
        ListView_SetExtendedListViewStyle(control(Rules),LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[]={L"사용",L"로컬 포트",L"목적지 호스트",L"목적지 포트"};for(int i=0;i<4;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<LPWSTR>(titles[i]);col.cx=scale(i==2?260:90);ListView_InsertColumn(control(Rules),i,&col);}
        make(form_,RuleNew,L"BUTTON",L"새 규칙",WS_TABSTOP);make(form_,RuleDelete,L"BUTTON",L"규칙 삭제",WS_TABSTOP);
        label(306,L"로컬 포트");label(307,L"목적지 호스트");label(308,L"목적지 포트");
        for(int id:{LocalPort,RemoteHost,RemotePort})make(form_,id,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
        label(309,L"실행 명령 프리뷰 (편집한 값)");make(form_,CommandPreview,L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP);
        make(form_,Save,L"BUTTON",L"선택 호스트 저장",WS_TABSTOP);make(form_,Connect,L"BUTTON",L"연결·재연결",WS_TABSTOP);make(form_,Authentication,L"BUTTON",L"인증 설정…",WS_TABSTOP);make(form_,ConnectionStatus,L"BUTTON",L"연결 상태…",WS_TABSTOP);
        make(form_,Validation,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);
        layoutSettings();
    }
    void layoutSettings(){if(!settingsWindow_||!form_)return;RECT rect{};GetClientRect(settingsWindow_,&rect);int w=MulDiv(rect.right,96,GetDpiForWindow(settingsWindow_)),h=MulDiv(rect.bottom,96,GetDpiForWindow(settingsWindow_));
        place(NewHost,12,12,102,30);place(DeleteHost,120,12,108,30);place(Hosts,12,50,216,std::max(40,h-118));
        place(CancelButton,w-190,h-44,82,30);place(Okay,w-96,h-44,82,30);MoveWindow(form_,scale(240),scale(12),scale(std::max(400,w-252)),scale(std::max(80,h-70)),TRUE);layoutForm();
    }
    void layoutForm(){if(!form_)return;RECT rect{};GetClientRect(form_,&rect);int w=MulDiv(rect.right,96,GetDpiForWindow(settingsWindow_)),h=MulDiv(rect.bottom,96,GetDpiForWindow(settingsWindow_));
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,719,static_cast<UINT>(std::max(1,h)),scroll_,0};SetScrollInfo(form_,SB_VERT,&si,TRUE);si.fMask=SIF_POS;GetScrollInfo(form_,SB_VERT,&si);scroll_=si.nPos;
        auto pos=[&](int id,int x,int y,int width,int height){place(id,x,y-scroll_,width,height);};int content=w-12;
        pos(300,0,4,105,22);pos(Name,115,0,content-115,26);pos(301,0,40,110,22);pos(Target,115,36,content-115,26);
        int userWidth=std::max(90,content-310);pos(302,0,76,110,22);pos(User,115,72,userWidth,26);pos(303,125+userWidth,76,100,22);pos(Port,225+userWidth,72,content-225-userWidth,26);
        pos(304,0,112,110,22);pos(Key,115,108,content-255,26);pos(ChooseKey,content-132,108,132,26);
        pos(AutoConnect,0,148,110,26);pos(ExitFailure,120,148,175,26);pos(RetryRefused,300,148,content-300,26);
        pos(305,0,188,content,22);pos(Rules,0,214,content,150);ListView_SetColumnWidth(control(Rules),0,scale(48));ListView_SetColumnWidth(control(Rules),1,scale(90));ListView_SetColumnWidth(control(Rules),2,scale(std::max(100,content-242)));ListView_SetColumnWidth(control(Rules),3,scale(100));
        pos(RuleNew,0,372,100,28);pos(RuleDelete,108,372,100,28);pos(306,0,410,100,22);pos(307,110,410,content-230,22);pos(308,content-110,410,110,22);pos(LocalPort,0,434,100,26);pos(RemoteHost,110,434,content-230,26);pos(RemotePort,content-110,434,110,26);
        pos(309,0,478,content,22);pos(CommandPreview,0,504,content,72);
        int gap=6;int button=(content-3*gap)/4;pos(Save,0,588,button,32);pos(Connect,button+gap,588,button,32);pos(Authentication,2*(button+gap),588,button,32);pos(ConnectionStatus,3*(button+gap),588,button,32);pos(Validation,0,630,content,76);
    }
    void scrollForm(int code,int wheel=0){SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(form_,SB_VERT,&si);int next=si.nPos;switch(code){case SB_LINEUP:next-=24;break;case SB_LINEDOWN:next+=24;break;case SB_PAGEUP:next-=si.nPage;break;case SB_PAGEDOWN:next+=si.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:next=si.nTrackPos;break;default:next-=wheel;}scroll_=next;layoutForm();}
    void ensureFocusVisible(){if(!form_)return;HWND focus=GetFocus();if(!focus||!IsChild(form_,focus))return;RECT area{},client{};GetWindowRect(focus,&area);MapWindowPoints(nullptr,form_,reinterpret_cast<POINT*>(&area),2);GetClientRect(form_,&client);if(area.top<0)scroll_+=MulDiv(area.top,96,GetDpiForWindow(settingsWindow_));else if(area.bottom>client.bottom)scroll_+=MulDiv(area.bottom-client.bottom,96,GetDpiForWindow(settingsWindow_));layoutForm();}
    void populateHosts(){if(!settings_.open)return;loading_=true;SendMessageW(control(Hosts),LB_RESETCONTENT,0,0);int selected=-1;for(size_t i=0;i<settings_.order.size();++i){auto& id=settings_.order[i];auto& h=settings_.drafts.at(id).host;auto runtime=manager_->runtime(id);std::string title=(settings_.dirty(id)?"● ":"")+h.name+(runtime&&runtime->temporary?" · 임시":"");SendMessageW(control(Hosts),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(wide(title).c_str()));if(id==settings_.selected)selected=static_cast<int>(i);}SendMessageW(control(Hosts),LB_SETCURSEL,selected,0);loading_=false;}
    void capture(){if(loading_||!settings_.open||!settings_.drafts.contains(settings_.selected))return;auto& draft=settings_.drafts.at(settings_.selected);draft.host.name=utf8(get(Name));draft.host.target=utf8(get(Target));draft.host.user=utf8(get(User));draft.host.identityFile=utf8(get(Key));draft.port=utf8(get(Port));draft.host.autoConnect=SendMessageW(control(AutoConnect),BM_GETCHECK,0,0)==BST_CHECKED;draft.host.exitOnForwardFailure=SendMessageW(control(ExitFailure),BM_GETCHECK,0,0)==BST_CHECKED;draft.host.retryOnConnectionRefused=SendMessageW(control(RetryRefused),BM_GETCHECK,0,0)==BST_CHECKED;
        if(rule_>=0&&static_cast<size_t>(rule_)<draft.host.forwards.size()){auto& rule=draft.host.forwards[rule_];rule.remoteHost=utf8(get(RemoteHost));draft.rulePorts[rule.id]={utf8(get(LocalPort)),utf8(get(RemotePort))};}
    }
    void preview(){if(!settings_.drafts.contains(settings_.selected))return;try{auto host=settings_.validated();std::wstring command=quoteWindowsArgument(systemSSH().wstring());for(auto& argument:sshArguments(host))command+=L" "+quoteWindowsArgument(wide(argument));set(CommandPreview,command);}catch(const std::exception& e){set(CommandPreview,wide(e.what()));}}
    void fillRules(){loading_=true;ListView_DeleteAllItems(control(Rules));if(settings_.drafts.contains(settings_.selected)){auto& draft=settings_.drafts.at(settings_.selected);for(size_t i=0;i<draft.host.forwards.size();++i){auto& f=draft.host.forwards[i];LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=const_cast<LPWSTR>(L"");ListView_InsertItem(control(Rules),&item);auto ports=draft.rulePorts.at(f.id);auto local=wide(ports.first),host=wide(f.remoteHost),remote=wide(ports.second);ListView_SetItemText(control(Rules),item.iItem,1,local.data());ListView_SetItemText(control(Rules),item.iItem,2,host.data());ListView_SetItemText(control(Rules),item.iItem,3,remote.data());ListView_SetCheckState(control(Rules),item.iItem,f.enabled);}}
        loading_=false;
    }
    void fillRuleEditor(){loading_=true;bool valid=settings_.drafts.contains(settings_.selected)&&rule_>=0&&static_cast<size_t>(rule_)<settings_.drafts.at(settings_.selected).host.forwards.size();for(int id:{LocalPort,RemoteHost,RemotePort,RuleDelete})EnableWindow(control(id),valid);if(valid){auto& draft=settings_.drafts.at(settings_.selected);auto& f=draft.host.forwards[rule_];auto ports=draft.rulePorts.at(f.id);set(LocalPort,wide(ports.first));set(RemoteHost,wide(f.remoteHost));set(RemotePort,wide(ports.second));}else{set(LocalPort,L"");set(RemoteHost,L"");set(RemotePort,L"");}loading_=false;}
    void fill(){loading_=true;if(settings_.drafts.contains(settings_.selected)){auto& d=settings_.drafts.at(settings_.selected);set(Name,wide(d.host.name));set(Target,wide(d.host.target));set(User,wide(d.host.user));set(Port,wide(d.port));set(Key,wide(d.host.identityFile));SendMessageW(control(AutoConnect),BM_SETCHECK,d.host.autoConnect?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(control(ExitFailure),BM_SETCHECK,d.host.exitOnForwardFailure?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(control(RetryRefused),BM_SETCHECK,d.host.retryOnConnectionRefused?BST_CHECKED:BST_UNCHECKED,0);}loading_=false;rule_=-1;fillRules();fillRuleEditor();populateHosts();preview();refreshButtons();}
    void addHost(){Host host;host.id=newId();host.name="새 호스트";settings_.add(host);fill();}
    void openSettings(const std::string& id={},bool startup=false){if(quitting_)return;if(settings_.open){if(!status_&&!id.empty()){capture();if(settings_.select(id))fill();}ShowWindow(settingsWindow_,SW_RESTORE);SetForegroundWindow(status_?status_:settingsWindow_);return;}
        if(!settingsWindow_)createSettings();Config saved;std::vector<Host> transient;std::string failure;try{manager_->reloadConfiguration();saved=loadConfig(directory_);for(auto host:manager_->hosts()){auto runtime=manager_->runtime(host.id);if(runtime&&runtime->executionHost)host=*runtime->executionHost;transient.push_back(host);}}catch(const std::exception& e){failure=e.what();}
        settings_.load(saved,transient,startup);if(settings_.order.empty())addHost();if(!id.empty())settings_.select(id);fill();set(Validation,wide(failure));ShowWindow(settingsWindow_,SW_SHOW);SetForegroundWindow(settingsWindow_);
    }
    void closeSettings(bool accepted){if(!settings_.open)return;capture();if(accepted&&settings_.anyDirty()){if(choose(L"저장하지 않은 편집 내용이 있습니다.",L"OK는 자동 저장하지 않습니다. 이미 저장하거나 연결에 적용한 결과는 유지됩니다.",{{1001,L"계속 편집"},{1002,L"저장하지 않고 닫기"}},1001)!=1002)return;}closeStatus();bool connect=settings_.close(accepted);ShowWindow(settingsWindow_,SW_HIDE);if(connect)try{manager_->startAll();}catch(const std::exception& e){error(e.what());}}
    Host saveSelected(){capture();auto host=settings_.validated();saveHost(directory_,host);manager_->saved(host);settings_.saved(host);populateHosts();set(Validation,L"저장했습니다. 변경 사항은 다음 연결부터 적용됩니다.");refreshButtons();return host;}
    void connectSelected(){capture();auto host=settings_.validated();if(settings_.dirty(host.id)){int answer=choose(L"편집한 설정으로 연결·재연결합니다.",L"저장 없이 연결한 값은 이번 앱 실행 동안 유지됩니다.",{{1001,L"저장 후 재연결"},{1002,L"저장 없이 재연결"},{IDCANCEL,L"취소"}},1001);if(answer==1001){host=saveSelected();manager_->connect(host,false);}else if(answer==1002)manager_->connect(host,true);}else manager_->connect(host.id);}
    void authenticate(){capture();if(authPending_.contains(settings_.selected))return;if(choose(L"선택한 호스트의 인증을 설정합니다.",L"선택한 호스트를 저장하고 외부 터미널에서 지문과 비밀번호를 확인합니다. 앱과 다른 연결은 유지되며 완료 후 연결은 직접 요청하세요.",{{1001,L"저장 후 터미널 열기"},{IDCANCEL,L"취소"}},1001)!=1001)return;Host original=saveSelected();authPending_.insert(original.id);refreshButtons();std::weak_ptr<int> alive=lifetime_;
        try{authObservers_[original.id]=launchRecovery(original.id,[this,alive,original](int result){if(alive.expired()||quitting_)return;authPending_.erase(original.id);authObservers_.erase(original.id);capture();try{auto updated=loadHost(directory_,original.id);manager_->saved(updated);settings_.authenticationFinished(original,updated);if(settings_.open&&settings_.selected==original.id)fill();else populateHosts();if(result!=0)error("인증 설정을 완료하지 못했습니다. 터미널에서 결과를 확인하세요.");}catch(const std::exception& e){error(e.what());}refreshButtons();},loop_->poster());}
        catch(...){authPending_.erase(original.id);refreshButtons();throw;}
    }
    void refreshButtons(){if(!settings_.open)return;auto runtime=manager_->runtime(settings_.selected);bool busy=runtime&&(runtime->state==ConnectionState::connecting||runtime->state==ConnectionState::stopping);EnableWindow(control(Connect),!busy&&!quitting_);EnableWindow(control(Authentication),!quitting_&&!authPending_.contains(settings_.selected));}
    void chooseKey(){std::vector<wchar_t> file(32768,0);auto value=get(Key);std::copy_n(value.begin(),std::min(value.size(),file.size()-1),file.begin());OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=settingsWindow_;dialog.lpstrFile=file.data();dialog.nMaxFile=static_cast<DWORD>(file.size());dialog.lpstrTitle=L"개인키 파일 선택";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&dialog)){set(Key,file.data());capture();preview();populateHosts();}}
    void showStatus(){capture();if(status_){SetForegroundWindow(status_);return;}settings_.showStatus();if(settings_.modalHost.empty())return;status_=CreateWindowExW(WS_EX_CONTROLPARENT,StatusClass,L"현재 연결 상태",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,CW_USEDEFAULT,CW_USEDEFAULT,scale(760),scale(520),settingsWindow_,nullptr,instance_,this);if(!status_){settings_.closeStatus();throw std::runtime_error(winError());}centerInWorkArea(status_,760,520,settingsWindow_);
        make(status_,StatusHeading,L"STATIC",L"",0);make(status_,StatusSummary,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);make(status_,StatusForwards,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER);ListView_SetExtendedListViewStyle(control(StatusForwards),LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);const wchar_t* names[]={L"규칙",L"출처",L"상태",L"상세"};for(int i=0;i<4;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<LPWSTR>(names[i]);col.cx=scale(i==0?200:120);ListView_InsertColumn(control(StatusForwards),i,&col);}
        make(status_,StatusErrors,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);make(status_,StatusClose,L"BUTTON",L"닫기",WS_TABSTOP|BS_DEFPUSHBUTTON);EnableWindow(settingsWindow_,FALSE);updateStatusFont();layoutStatus();refreshStatus();ShowWindow(status_,SW_SHOW);SetForegroundWindow(status_);SetFocus(control(StatusClose));
    }
    void closeStatus(){if(!status_)return;HWND old=status_;status_=nullptr;for(int id:{StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose})controls_.erase(id);DestroyWindow(old);if(statusFont_){DeleteObject(statusFont_);statusFont_=nullptr;}settings_.closeStatus();if(settingsWindow_){EnableWindow(settingsWindow_,TRUE);if(settings_.open)SetForegroundWindow(settingsWindow_);}}
    void layoutStatus(){if(!status_||!control(StatusClose))return;RECT rect{};GetClientRect(status_,&rect);UINT dpi=GetDpiForWindow(status_);auto statusScale=[dpi](int value){return MulDiv(value,dpi,96);};int w=MulDiv(rect.right,96,dpi),h=MulDiv(rect.bottom,96,dpi);place(StatusHeading,12,12,w-24,26);place(StatusSummary,12,44,w-24,76);place(StatusForwards,12,130,w-24,std::max(60,h-286));place(StatusErrors,12,std::max(200,h-146),w-24,92);place(StatusClose,w-100,h-44,88,30);ListView_SetColumnWidth(control(StatusForwards),0,statusScale(180));ListView_SetColumnWidth(control(StatusForwards),1,statusScale(85));ListView_SetColumnWidth(control(StatusForwards),2,statusScale(90));ListView_SetColumnWidth(control(StatusForwards),3,statusScale(std::max(160,w-395)));}
    static std::wstring forwardTitle(ForwardState state){switch(state){case ForwardState::preparing:return L"준비 중";case ForwardState::ready:return L"준비됨";case ForwardState::failed:return L"개설 실패";case ForwardState::disconnected:return L"연결 끊김";case ForwardState::unknown:return L"확인 불가";}return L"확인 불가";}
    void refreshStatus(){if(!status_||settings_.modalHost.empty())return;auto runtime=manager_->runtime(settings_.modalHost);Host host;auto draft=settings_.drafts.find(settings_.modalHost);if(draft!=settings_.drafts.end())host=draft->second.host;if(runtime&&runtime->executionHost)host=*runtime->executionHost;set(StatusHeading,wide(host.name+" · "+host.target));std::string summary=runtime?runtime->summary():"SSH 연결 끊김 · 포워딩 준비 0 / 실패 0";if(runtime&&runtime->temporary)summary+="\n이번 실행의 임시 설정 사용 중";else if(runtime&&runtime->executionHost&&draft!=settings_.drafts.end()&&*runtime->executionHost!=draft->second.host)summary+="\n현재 실행 설정과 편집한 설정이 다릅니다.";set(StatusSummary,nativeLines(summary));
        SendMessageW(control(StatusForwards),WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(control(StatusForwards));if(runtime){for(size_t i=0;i<runtime->forwards.size();++i){auto& f=runtime->forwards[i];auto description=wide(f.description);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=description.data();ListView_InsertItem(control(StatusForwards),&item);auto source=wide(f.source),state=forwardTitle(f.state),detail=wide(f.detail);ListView_SetItemText(control(StatusForwards),item.iItem,1,source.data());ListView_SetItemText(control(StatusForwards),item.iItem,2,state.data());ListView_SetItemText(control(StatusForwards),item.iItem,3,detail.data());}}
        SendMessageW(control(StatusForwards),WM_SETREDRAW,TRUE,0);InvalidateRect(control(StatusForwards),nullptr,TRUE);std::string details="준비됨은 포워딩 개설 성공이며 목적지 서비스 정상 여부를 보장하지 않습니다.";if(runtime){if(!runtime->lastError.empty())details+="\n최근 오류: "+runtime->lastError;for(auto& e:runtime->recentErrors)details+="\n"+e.time+" · "+e.message;}set(StatusErrors,nativeLines(details));
    }
    void updateTray(){if(test_)return;size_t connected=0,disconnected=0;for(auto& host:manager_->hosts()){auto runtime=manager_->runtime(host.id);if(runtime&&runtime->state==ConnectionState::connected)++connected;else if(host.autoConnect)++disconnected;}tray_={};tray_.cbSize=sizeof(tray_);tray_.hWnd=window_;tray_.uID=1;tray_.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray_.uCallbackMessage=WM_TRAY;tray_.hIcon=icon_;auto tip=L"LocalForwarder · 연결됨 "+std::to_wstring(connected)+L" · 연결 안 됨 "+std::to_wstring(disconnected);wcsncpy_s(tray_.szTip,tip.c_str(),_TRUNCATE);if(!trayAdded_){trayAdded_=Shell_NotifyIconW(NIM_ADD,&tray_)!=FALSE;if(trayAdded_){tray_.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray_);}}else Shell_NotifyIconW(NIM_MODIFY,&tray_);if(menuOpen_)updateMenu();}
    void updateMenu(){if(!activeMenu_)return;bool busy=false;for(auto& host:manager_->hosts()){auto r=manager_->runtime(host.id);busy=busy||(r&&(r->state==ConnectionState::connecting||r->state==ConnectionState::stopping));auto it=hostMenus_.find(host.id);if(it==hostMenus_.end())continue;HMENU menu=it->second;bool connected=r&&r->state==ConnectionState::connected,working=r&&(r->state==ConnectionState::connecting||r->state==ConnectionState::stopping);std::string state=connected?"connected":(host.autoConnect?"disconnected":"disabled");std::string title=host.name+" · "+state+(working?" · 작업 중…":"")+(r&&r->temporary?" · 임시":"");MENUITEMINFOW row{};row.cbSize=sizeof(row);row.fMask=MIIM_STRING;auto text=wide(title);row.dwTypeData=text.data();for(int i=0;i<GetMenuItemCount(activeMenu_);++i){if(GetSubMenu(activeMenu_,i)==menu){SetMenuItemInfoW(activeMenu_,i,TRUE,&row);break;}}
            auto change=[&](UINT id,const std::wstring& value,UINT flags){ModifyMenuW(menu,id,MF_BYCOMMAND|MF_STRING|flags,id,value.c_str());};UINT base=0;for(auto& [id,action]:menuActions_)if(action.first==host.id&&action.second==1){base=id;break;}
            if(!base)continue;change(base+10,wide(r?r->summary():"연결 끊김"),MF_GRAYED);std::string error=r?r->lastError:"";auto errorText=wide(error.empty()?"최근 오류 없음":"최근 오류: "+error);if(errorText.size()>240)errorText.resize(240);change(base+11,errorText,MF_GRAYED);change(base,working?L"연결 작업 중…":(connected?L"재연결":L"연결"),working?MF_GRAYED:MF_ENABLED);if(connected||working){if(GetMenuState(menu,base+1,MF_BYCOMMAND)==UINT(-1))InsertMenuW(menu,base+2,MF_BYCOMMAND|MF_STRING,base+1,L"끊기");}else DeleteMenu(menu,base+1,MF_BYCOMMAND);std::string note;if(r&&r->temporary)note="이번 실행의 임시 설정 사용 중";else if(r&&r->executionHost&&*r->executionHost!=host&&(connected||working))note="현재 실행 설정과 저장된 설정이 다릅니다";change(base+12,wide(note.empty()?"저장된 설정 사용":note),MF_GRAYED);std::string recent;if(r&&!r->recentErrors.empty())recent=r->recentErrors.back().time+" · "+r->recentErrors.back().message;auto recentText=wide(recent.empty()?"관측된 목적지 오류 없음":recent);if(recentText.size()>240)recentText.resize(240);change(base+13,recentText,MF_GRAYED);
        }EnableMenuItem(activeMenu_,2,MF_BYCOMMAND|(busy||quitting_?MF_GRAYED:MF_ENABLED));}
    void trayMenu(){if(quitting_||menuOpen_)return;activeMenu_=CreatePopupMenu();hostMenus_.clear();menuActions_.clear();AppendMenuW(activeMenu_,MF_STRING,1,L"설정");AppendMenuW(activeMenu_,MF_STRING,2,L"전체 재연결");AppendMenuW(activeMenu_,MF_SEPARATOR,0,nullptr);UINT base=1000;
        for(auto& host:manager_->hosts()){HMENU menu=CreatePopupMenu();hostMenus_[host.id]=menu;AppendMenuW(menu,MF_STRING|MF_GRAYED,base+10,L"연결 상태");AppendMenuW(menu,MF_STRING|MF_GRAYED,base+11,L"최근 오류 없음");AppendMenuW(menu,MF_STRING|MF_GRAYED,base+12,L"실행 설정");AppendMenuW(menu,MF_STRING|MF_GRAYED,base+13,L"관측된 목적지 오류 없음");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,base,L"연결");AppendMenuW(menu,MF_STRING,base+1,L"끊기");AppendMenuW(menu,MF_STRING,base+2,L"호스트 설정");for(int i=0;i<3;++i)menuActions_[base+i]={host.id,i+1};AppendMenuW(activeMenu_,MF_POPUP,reinterpret_cast<UINT_PTR>(menu),wide(host.name).c_str());base+=20;}
        AppendMenuW(activeMenu_,MF_SEPARATOR,0,nullptr);AppendMenuW(activeMenu_,MF_STRING,3,L"종료");updateMenu();POINT point{};GetCursorPos(&point);SetForegroundWindow(window_);menuOpen_=true;UINT selected=TrackPopupMenuEx(activeMenu_,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window_,nullptr);menuOpen_=false;DestroyMenu(activeMenu_);activeMenu_=nullptr;hostMenus_.clear();PostMessageW(window_,WM_NULL,0,0);
        try{if(selected==1)openSettings();else if(selected==2)manager_->reconnectAll();else if(selected==3)quit();else if(menuActions_.contains(selected)){auto [id,operation]=menuActions_.at(selected);if(operation==1)manager_->connect(id);else if(operation==2)manager_->disconnect(id);else openSettings(id);}}catch(const std::exception& e){error(e.what());}menuActions_.clear();
    }
    void refresh(){if(!manager_)return;if(test_&&!uiTest_){snapshot();return;}updateTray();if(settings_.open){populateHosts();refreshButtons();refreshStatus();}}
    void quit(){if(quitting_)return;quitting_=true;network_.stop();closeStatus();for(auto& [id,cancel]:authObservers_)if(cancel)cancel();authObservers_.clear();manager_->quit();}
    void finishQuit(){if(trayAdded_){Shell_NotifyIconW(NIM_DELETE,&tray_);trayAdded_=false;}loop_->detach();if(settingsWindow_)ShowWindow(settingsWindow_,SW_HIDE);PostQuitMessage(0);}
    void snapshot(const std::string& request={}){
        if(!test_||!manager_)return;for(auto& host:manager_->hosts()){auto r=manager_->runtime(host.id);if(!r)continue;if(!singleID_.empty()&&singleID_==host.id&&previous_!=r->state){previous_=r->state;outputLine(std::string("LF_STATE ")+stateName(r->state));}
            yyjson_mut_doc* doc=yyjson_mut_doc_new(nullptr);auto* object=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,object);auto str=[&](yyjson_mut_val* obj,const char* key,const std::string& value){yyjson_mut_obj_add_strcpy(doc,obj,key,value.c_str());};str(object,"id",host.id);str(object,"state",stateName(r->state));str(object,"error",r->lastError);yyjson_mut_obj_add_bool(doc,object,"intent",r->desiredConnected);yyjson_mut_obj_add_bool(doc,object,"temporary",r->temporary);if(!request.empty())str(object,"requestID",request);
            auto* forwards=yyjson_mut_arr(doc);yyjson_mut_obj_add_val(doc,object,"forwards",forwards);for(auto& f:r->forwards){auto* row=yyjson_mut_arr_add_obj(doc,forwards);str(row,"id",f.id);str(row,"state",forwardStateName(f.state));str(row,"source",f.source);str(row,"description",f.description);str(row,"detail",f.detail);}auto* errors=yyjson_mut_arr(doc);yyjson_mut_obj_add_val(doc,object,"recentErrors",errors);for(auto& e:r->recentErrors){auto* row=yyjson_mut_arr_add_obj(doc,errors);str(row,"time",e.time);str(row,"message",e.message);}char* json=yyjson_mut_write(doc,0,nullptr);if(json){outputLine("LF_HOST "+std::string(json));free(json);}yyjson_mut_doc_free(doc);
        }
    }
    void input(const std::string& line){if(quitting_)return;try{
        if(!singleID_.empty()&&(line.empty()||line.front()!='{')){if(line=="quit")quit();else if(line=="reconnect")manager_->connect(singleID_);else if(line=="disconnect")manager_->disconnect(singleID_);else if(line=="wake")manager_->wake();else if(line=="sleep")manager_->sleep();return;}
        std::unique_ptr<yyjson_doc,decltype(&yyjson_doc_free)> doc(yyjson_read(line.data(),line.size(),0),yyjson_doc_free);if(!doc)throw std::runtime_error("명령 JSON이 올바르지 않습니다.");auto* root=yyjson_doc_get_root(doc.get());if(!yyjson_is_obj(root))throw std::runtime_error("명령은 JSON 객체여야 합니다.");auto text=[&](const char* key,bool required=true){auto* value=yyjson_obj_get(root,key);if(!yyjson_is_str(value)){if(!required)return std::string{};throw std::runtime_error(std::string("문자열 필드가 필요합니다: ")+key);}return std::string(yyjson_get_str(value),yyjson_get_len(value));};auto command=text("command");
        if(command=="quit")quit();else if(command=="connect")manager_->connect(text("id"));else if(command=="disconnect")manager_->disconnect(text("id"));else if(command=="delete")manager_->remove(text("id"));else if(command=="saved")manager_->saved(loadHost(directory_,text("id")));else if(command=="reconnect-all")manager_->reconnectAll();else if(command=="wake")manager_->wake();else if(command=="sleep")manager_->sleep();else if(command=="network")manager_->networkChanged(yyjson_is_true(yyjson_obj_get(root,"available")));else if(command=="snapshot")snapshot(text("requestID",false));else if(command=="temporary"){auto* host=yyjson_obj_get(root,"host");if(!yyjson_is_obj(host))throw std::runtime_error("호스트 설정이 필요합니다.");char* value=yyjson_val_write(host,0,nullptr);if(!value)throw std::runtime_error("호스트 설정을 읽지 못했습니다.");std::string encoded=value;free(value);auto config=parseConfig("{\"version\":2,\"hosts\":["+encoded+"]}");manager_->connect(config.hosts.at(0),true);}else throw std::runtime_error("알 수 없는 명령: "+command);
        }catch(const std::exception& e){error(e.what());}}
    void startInput(){auto post=loop_->poster();std::weak_ptr<int> alive=lifetime_;std::thread([this,alive,post]{HANDLE handle=GetStdHandle(STD_INPUT_HANDLE);std::string line;char buffer[4096];DWORD count=0;while(ReadFile(handle,buffer,sizeof(buffer),&count,nullptr)&&count){for(DWORD i=0;i<count;++i){if(buffer[i]=='\n'){auto copy=line;line.clear();post([this,alive,copy]{if(!alive.expired())input(copy);});}else if(buffer[i]!='\r'){if(line.size()>=4*1024*1024){post([this,alive]{if(!alive.expired())quit();});return;}line+=buffer[i];}}}post([this,alive]{if(!alive.expired())quit();});}).detach();}
    void command(int id,int notification){if(loading_||quitting_)return;try{
        if(id==Hosts&&notification==LBN_SELCHANGE){capture();auto row=SendMessageW(control(Hosts),LB_GETCURSEL,0,0);if(row>=0&&static_cast<size_t>(row)<settings_.order.size()&&settings_.select(settings_.order[row]))fill();}
        else if(id==NewHost){capture();addHost();}
        else if(id==DeleteHost){capture();if(choose(L"이 호스트를 삭제할까요?",L"해당 호스트의 연결과 저장 설정을 제거합니다.",{{1001,L"호스트 삭제"},{IDCANCEL,L"취소"}},IDCANCEL)==1001){auto selected=settings_.selected;manager_->remove(selected);settings_.remove(selected);if(settings_.order.empty())addHost();else fill();}}
        else if(id==RuleNew){capture();auto& draft=settings_.drafts.at(settings_.selected);Forward f{newId(),true,8080,"localhost",80};draft.host.forwards.push_back(f);draft.rulePorts[f.id]={"8080","80"};fillRules();rule_=static_cast<int>(draft.host.forwards.size()-1);loading_=true;ListView_SetItemState(control(Rules),rule_,LVIS_SELECTED,LVIS_SELECTED);loading_=false;fillRuleEditor();populateHosts();preview();}
        else if(id==RuleDelete){capture();auto& draft=settings_.drafts.at(settings_.selected);if(rule_>=0&&static_cast<size_t>(rule_)<draft.host.forwards.size()){draft.rulePorts.erase(draft.host.forwards[rule_].id);draft.host.forwards.erase(draft.host.forwards.begin()+rule_);rule_=-1;fillRules();fillRuleEditor();populateHosts();preview();}}
        else if(id==Save)saveSelected();else if(id==Connect)connectSelected();else if(id==Authentication)authenticate();else if(id==ConnectionStatus)showStatus();else if(id==ChooseKey)chooseKey();else if(id==Okay||id==IDOK)closeSettings(true);else if(id==CancelButton||id==IDCANCEL)closeSettings(false);
        else if((notification==EN_CHANGE&&(id==Name||id==Target||id==User||id==Port||id==Key||id==LocalPort||id==RemoteHost||id==RemotePort))||id==AutoConnect||id==ExitFailure||id==RetryRefused){capture();populateHosts();preview();if(id==LocalPort||id==RemoteHost||id==RemotePort){auto& draft=settings_.drafts.at(settings_.selected);if(rule_>=0&&static_cast<size_t>(rule_)<draft.host.forwards.size()){auto& f=draft.host.forwards[rule_];auto ports=draft.rulePorts.at(f.id);auto l=wide(ports.first),r=wide(ports.second),host=wide(f.remoteHost);ListView_SetItemText(control(Rules),rule_,1,l.data());ListView_SetItemText(control(Rules),rule_,2,host.data());ListView_SetItemText(control(Rules),rule_,3,r.data());}}}
        }catch(const std::exception& e){error(e.what());}}
    void ruleChanged(NMLISTVIEW* event){if(loading_||event->iItem<0||!settings_.drafts.contains(settings_.selected))return;auto& draft=settings_.drafts.at(settings_.selected);if(static_cast<size_t>(event->iItem)>=draft.host.forwards.size())return;draft.host.forwards[event->iItem].enabled=ListView_GetCheckState(control(Rules),event->iItem)!=FALSE;if((event->uNewState&LVIS_SELECTED)&&!(event->uOldState&LVIS_SELECTED)){capture();rule_=event->iItem;fillRuleEditor();}populateHosts();preview();}
public:
    void nativeChecks(const std::wstring& instanceKey){
        auto check=[](bool value,const char* why){if(!value)throw std::runtime_error(why);};
        openSettings({},true);check(settings_.startup&&settings_.order.size()==2,"startup settings load");
        set(Name,L"A edited");set(Port,L"invalid");
        SendMessageW(control(Hosts),LB_SETCURSEL,1,0);command(Hosts,LBN_SELCHANGE);
        set(Name,L"B saved");command(Save,BN_CLICKED);
        check(loadHost(directory_,"b").name=="B saved"&&loadHost(directory_,"a").name=="A","native selected save");
        check(settings_.drafts.at("a").port=="invalid"&&settings_.dirty("a"),"native host switch preserves invalid draft");
        showStatus();check(status_&&!IsWindowEnabled(settingsWindow_)&&settings_.modalHost=="b","native modal blocks owner");
        auto host=loadHost(directory_,"b");manager_->connect(host,false);
        check(get(StatusHeading).find(L"server-b")!=std::wstring::npos&&ListView_GetItemCount(control(StatusForwards))==1,"native live status initial target");
        host.target="server-b-updated";saveHost(directory_,host);manager_->saved(host);manager_->connect(host,false);
        check(get(StatusHeading).find(L"server-b-updated")!=std::wstring::npos,"native open modal updates execution heading");
        openSettings("a");check(settings_.selected=="b"&&settings_.modalHost=="b","native open request preserves modal target");
        // Exercise the actual named mutex, ready event and second-process open request.
        Handle finished(CreateEventW(nullptr,TRUE,FALSE,nullptr));std::string peerError;auto executable=executablePath();
        std::thread peer([&]{try{auto result=lf::capture(executable,{"--test-instance-peer",utf8(instanceKey)});if(result.code)peerError="second instance exited "+std::to_string(result.code)+": "+result.text;}catch(const std::exception& e){peerError=e.what();}SetEvent(finished.value);});
        DWORD started=GetTickCount();while(WaitForSingleObject(finished.value,0)!=WAIT_OBJECT_0){if(GetTickCount()-started>10000){peerError="second-instance request timed out";break;}MsgWaitForMultipleObjects(1,&finished.value,FALSE,100,QS_ALLINPUT);MSG event{};while(PeekMessageW(&event,nullptr,0,0,PM_REMOVE)){if(event.message==WM_QUIT)continue;if(!preprocess(event)){TranslateMessage(&event);DispatchMessageW(&event);}}}
        peer.join();check(peerError.empty(),peerError.c_str());check(settings_.drafts.at("a").port=="invalid"&&settings_.modalHost=="b","second process preserves modal and unsaved draft");
        closeStatus();check(IsWindowEnabled(settingsWindow_)&&settings_.modalHost.empty(),"native modal close re-enables settings");
        closeSettings(false);check(!settings_.open&&!manager_->runtime("a")->desiredConnected,"native startup cancel has no auto-connect intent");
        manager_->disconnect("b");openSettings({},true);closeSettings(true);
        check(manager_->runtime("a")->state==ConnectionState::connected&&!manager_->runtime("b")->desiredConnected,"native startup OK applies autoConnect selection");
        openSettings();set(Name,L"cancelled edit");closeSettings(false);check(loadHost(directory_,"a").name=="A","native cancel preserves saved value");
        check(manager_->runtime("a")->state==ConnectionState::connected,"native close keeps existing connection");
        manager_->quit();outputLine("native UI controller tests passed");
    }
    LRESULT message(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
        if(hwnd==window_){if(taskbarCreated_&&msg==taskbarCreated_){trayAdded_=false;updateTray();return 0;}switch(msg){case WM_TASK:{std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(l));(*task)();return 0;}case WM_TIMER:loop_->timer(w);return 0;case WM_OPEN_SETTINGS:if(menuOpen_)EndMenu();openSettings();return 1;case WM_TRAY:if(LOWORD(l)==WM_CONTEXTMENU||LOWORD(l)==WM_RBUTTONUP||LOWORD(l)==NIN_SELECT||LOWORD(l)==NIN_KEYSELECT)trayMenu();return 0;case WM_POWERBROADCAST:if(w==PBT_APMSUSPEND)manager_->sleep();else if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND)manager_->wake();return TRUE;case WM_QUERYENDSESSION:return TRUE;case WM_ENDSESSION:if(w)quit();return 0;case WM_CLOSE:quit();return 0;}}
        else{switch(msg){case WM_COMMAND:command(LOWORD(w),HIWORD(w));return 0;case WM_NOTIFY:{auto* notification=reinterpret_cast<NMHDR*>(l);if(notification->idFrom==Rules&&notification->code==LVN_ITEMCHANGED)ruleChanged(reinterpret_cast<NMLISTVIEW*>(l));return 0;}case WM_SIZE:layoutSettings();return 0;case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={scale(800),scale(430)};return 0;}case WM_DPICHANGED:{auto* rect=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);updateFont();layoutSettings();return 0;}case WM_CLOSE:closeSettings(false);return 0;}}
        return DefWindowProcW(hwnd,msg,w,l);
    }
    LRESULT formMessage(HWND hwnd,UINT msg,WPARAM w,LPARAM l){switch(msg){case WM_COMMAND:case WM_NOTIFY:return SendMessageW(settingsWindow_,msg,w,l);case WM_VSCROLL:scrollForm(LOWORD(w));return 0;case WM_MOUSEWHEEL:scrollForm(-1,MulDiv(GET_WHEEL_DELTA_WPARAM(w),72,WHEEL_DELTA));return 0;}return DefWindowProcW(hwnd,msg,w,l);}
    LRESULT statusMessage(HWND hwnd,UINT msg,WPARAM w,LPARAM l){switch(msg){case WM_COMMAND:if(LOWORD(w)==StatusClose||LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)closeStatus();return 0;case WM_CLOSE:closeStatus();return 0;case WM_SIZE:layoutStatus();return 0;case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={MulDiv(600,GetDpiForWindow(hwnd),96),MulDiv(390,GetDpiForWindow(hwnd),96)};return 0;case WM_DPICHANGED:{auto* rect=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);updateStatusFont();layoutStatus();return 0;}}return DefWindowProcW(hwnd,msg,w,l);}
    void caught(const std::exception& exception){error(exception.what());}
};
template<class F>LRESULT dispatchWindow(HWND hwnd,UINT msg,WPARAM w,LPARAM l,F&& invoke){auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(app){try{return invoke(*app);}catch(const std::exception& e){app->caught(e);return 0;}}return DefWindowProcW(hwnd,msg,w,l);}
LRESULT CALLBACK appProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.message(hwnd,msg,w,l);});}
LRESULT CALLBACK formProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.formMessage(hwnd,msg,w,l);});}
LRESULT CALLBACK statusProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.statusMessage(hwnd,msg,w,l);});}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool headless=false;int result=1;fs::path ownedTestDirectory;
    try{
        if(!argv)throw std::runtime_error(winError());std::vector<std::wstring> args(argv,argv+argc);LocalFree(argv);argv=nullptr;
        auto directory=homeDirectory()/L".LocalForwarder";bool test=false,start=true,open=false,uiTest=false;std::string config,single;
        if(args.size()>=2){
            headless=args[1].rfind(L"--test-",0)==0;
            if(args[1]==L"--setup-auth"&&args.size()==3){result=setupAuthentication(directory,utf8(args[2]));if(SUCCEEDED(com))CoUninitialize();return result;}
            if(args[1]==L"--test-setup-auth"&&args.size()==5){result=setupAuthentication(args[2],utf8(args[3]),utf8(args[4]),false);if(SUCCEEDED(com))CoUninitialize();return result;}
            if(args[1]==L"--test-instance-peer"&&args.size()==3){if(!safeId(utf8(args[2])))throw std::runtime_error("Invalid test instance key");SingleInstance peer;if(peer.acquire(args[2]))throw std::runtime_error("Test instance was not already running");if(SUCCEEDED(com))CoUninitialize();return 0;}
            if(args[1]==L"--open-config"&&args.size()==2)open=true;
            else if(args[1]==L"--test-manager"&&(args.size()==4||(args.size()==5&&args[4]==L"--no-start"))){test=true;directory=args[2];config=utf8(args[3]);start=args.size()==4;}
            else if(args[1]==L"--test-session"&&args.size()==5){test=true;directory=args[2];single=utf8(args[3]);config=utf8(args[4]);start=false;}
            else if(args[1]==L"--test-ui"&&args.size()==2){test=true;uiTest=true;start=false;directory=fs::temp_directory_path()/wide("LocalForwarder-ui-"+newId());ownedTestDirectory=directory;Host a;a.id="a";a.name="A";a.target="server-a";a.forwards.push_back({"f",true,12000,"localhost",80});Host b=a;b.id="b";b.name="B";b.target="server-b";b.autoConnect=false;saveHost(directory,a);saveHost(directory,b);}
            else throw std::runtime_error("Usage: LocalForwarder [--open-config | --setup-auth PROFILE_ID]");
        }
        SingleInstance singleton;std::wstring instanceKey=uiTest?wide(newId()):L"";
        if((!test||uiTest)&&!singleton.acquire(instanceKey)){if(SUCCEEDED(com))CoUninitialize();return 0;}
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        {
            App app(instance,directory,test,start,open,config,single,uiTest);app.create(!test||uiTest?singleton.windowTitle():L"LocalForwarder test");if(!test||uiTest)singleton.ready();
            if(uiTest){app.nativeChecks(instanceKey);result=0;}
            else{MSG message{};BOOL received;while((received=GetMessageW(&message,nullptr,0,0))>0){if(!app.preprocess(message)){TranslateMessage(&message);DispatchMessageW(&message);}}result=received==-1?1:static_cast<int>(message.wParam);}
        }
    }catch(const std::exception& e){if(argv)LocalFree(argv);outputLine("LF_ERROR "+std::string(e.what()));if(!headless)MessageBoxW(nullptr,wide(e.what()).c_str(),L"LocalForwarder",MB_OK|MB_ICONERROR);}
    if(!ownedTestDirectory.empty()){std::error_code ignored;fs::remove_all(ownedTestDirectory,ignored);}if(SUCCEEDED(com))CoUninitialize();return result;
}
