#include "platform.hpp"
#include "login_startup_registry.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "presentation.hpp"
#include "product_version.hpp"
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
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using namespace lf;
namespace {
UINT previewDpi=0;
std::optional<RECT> previewWorkArea;
std::wstring previewScene=L"settings";
struct Spacing {
    static constexpr int input=26,button=30,gap=8,margin=12,section=16;
};
HFONT uiFont(UINT dpi,int weight=FW_NORMAL,const wchar_t* face=L"Segoe UI"){
    return CreateFontW(-MulDiv(10,dpi,72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,face);
}
// STATICs, read-only edits and checkboxes share their parent's system surface.
// Leave selection, focus and themed button drawing to Windows.
LRESULT surfaceColor(WPARAM value,LPARAM child){
    HDC dc=reinterpret_cast<HDC>(value);
    SetTextColor(dc,GetSysColor(IsWindowEnabled(reinterpret_cast<HWND>(child))?COLOR_WINDOWTEXT:COLOR_GRAYTEXT));
    SetBkColor(dc,GetSysColor(COLOR_WINDOW));
    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
}
UINT uiDpi(HWND window){return previewDpi?previewDpi:GetDpiForWindow(window);}
constexpr UINT WM_TASK=WM_APP+1,WM_TRAY=WM_APP+2,WM_OPEN_SETTINGS=WM_APP+3,WM_REMEMBER_SETTINGS_FOCUS=WM_APP+4;
constexpr wchar_t AppClass[]=L"LocalForwarder.MultiHost.Window";
constexpr wchar_t FormClass[]=L"LocalForwarder.MultiHost.Form";
constexpr wchar_t RuleClass[]=L"LocalForwarder.MultiHost.Rule";
constexpr wchar_t StatusClass[]=L"LocalForwarder.MultiHost.Status";
constexpr wchar_t ProjectURL[]=L"https://github.com/GuruJung/LocalForwarder";
constexpr wchar_t LicenseURL[]=L"https://github.com/GuruJung/LocalForwarder/blob/main/LICENSE";
enum Control:int {Hosts=100,NewHost,DeleteHost,Name,Description,Target,User,Port,Key,ChooseKey,AutoConnect,ExitFailure,RetryRefused,Rules,RuleNew,RuleEdit,RuleDelete,RuleEnabled,RuleDescription,RuleOkay,RuleCancel,LocalPort,RemoteHost,RemotePort,Save,Connect,Authentication,ConnectionStatus,CommandPreview,Okay,CancelButton,StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose,RuleSort};
std::wstring textOf(HWND control){int n=GetWindowTextLengthW(control);std::wstring out(n+1,0);GetWindowTextW(control,out.data(),n+1);out.resize(n);return out;}
RECT availableWorkArea(HMONITOR display){
    if(previewWorkArea)return *previewWorkArea;
    MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(display,&monitor))return monitor.rcWork;
    RECT fallback{};SystemParametersInfoW(SPI_GETWORKAREA,0,&fallback,0);return fallback;
}
void constrainMinimum(HWND window,MINMAXINFO* info,int width,int height){
    auto work=availableWorkArea(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST));UINT dpi=uiDpi(window);
    info->ptMinTrackSize={std::min(MulDiv(width,dpi,96),std::max(1,static_cast<int>(work.right-work.left)-24)),std::min(MulDiv(height,dpi,96),std::max(1,static_cast<int>(work.bottom-work.top)-24))};
}
void centerInWorkArea(HWND window,int desiredWidth,int desiredHeight,HWND owner=nullptr){
    POINT point{};GetCursorPos(&point);
    HMONITOR display=owner?MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST):MonitorFromPoint(point,MONITOR_DEFAULTTONEAREST);
    auto work=availableWorkArea(display);int dpi=uiDpi(window);int width=std::min(MulDiv(desiredWidth,dpi,96),std::max(1,static_cast<int>(work.right-work.left)-24));int height=std::min(MulDiv(desiredHeight,dpi,96),std::max(1,static_cast<int>(work.bottom-work.top)-24));
    SetWindowPos(window,nullptr,work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
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
        if(!SetTimer(self->window_,id,static_cast<UINT>(std::clamp(seconds*1000.0,1.0,double(USER_TIMER_MAXIMUM))),nullptr)){self->timers_.erase(id);throw LocalizedError(tr("win.could_not_create_a_scheduled_timer"));}
        return [weak,id]{if(auto current=weak.lock()){KillTimer(current->window_,id);current->timers_.erase(id);}};
    };}
    void timer(UINT_PTR id){auto it=timers_.find(id);if(it==timers_.end())return;KillTimer(window_,id);auto work=std::move(it->second);timers_.erase(it);work();}
    void detach(){std::lock_guard lock(lock_);for(auto& [id,work]:timers_)KillTimer(window_,id);timers_.clear();MSG msg{};while(PeekMessageW(&msg,window_,WM_TASK,WM_TASK,PM_REMOVE))delete reinterpret_cast<std::function<void()>*>(msg.lParam);window_=nullptr;}
};
class SingleInstance {
    Handle mutex_,ready_;std::wstring sid_;
public:
    bool acquire(const std::wstring& isolation={}){
        HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw LocalizedError(winError());Handle token(raw);
        DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
        if(!GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size))throw LocalizedError(winError());
        LPWSTR stringSid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&stringSid))throw LocalizedError(winError());sid_=stringSid;LocalFree(stringSid);std::wstring ownerSID=sid_;if(!isolation.empty())sid_+=L".test."+isolation;
        PSECURITY_DESCRIPTOR descriptor=nullptr;std::wstring sddl=L"D:P(A;;GA;;;"+ownerSID+L")";
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))throw LocalizedError(winError());
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
        mutex_.reset(CreateMutexW(&security,FALSE,(L"Local\\LocalForwarder.Instance."+sid_).c_str()));DWORD error=GetLastError();
        ready_.reset(CreateEventW(&security,TRUE,FALSE,(L"Local\\LocalForwarder.Ready."+sid_).c_str()));LocalFree(descriptor);
        if(!mutex_||!ready_)throw LocalizedError(winError());
        if(error!=ERROR_ALREADY_EXISTS)return true;
        if(WaitForSingleObject(ready_.value,5000)!=WAIT_OBJECT_0)throw LocalizedError(tr("win.localforwarder_is_still_starting_try_again_shortly"));
        HWND window=FindWindowW(AppClass,(L"LocalForwarder.Instance."+sid_).c_str());DWORD_PTR result=0;
        if(!window||!SendMessageTimeoutW(window,WM_OPEN_SETTINGS,0,0,SMTO_ABORTIFHUNG,5000,&result))throw LocalizedError(tr("win.could_not_ask_the_existing_localforwarder_instance_to_open_settin"));
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
        if(FAILED(result))throw LocalizedError(tr("win.could_not_subscribe_to_windows_network_notifications"));
        IConnectionPointContainer* container=nullptr;
        result=manager_->QueryInterface(IID_IConnectionPointContainer,reinterpret_cast<void**>(&container));
        if(SUCCEEDED(result)){result=container->FindConnectionPoint(__uuidof(INetworkListManagerEvents),&point_);container->Release();}
        if(FAILED(result))throw LocalizedError(tr("win.could_not_find_windows_network_events"));
        auto* sink=new NetworkSink(std::move(post),changed);result=point_->Advise(sink,&cookie_);sink->Release();
        if(FAILED(result))throw LocalizedError(tr("win.could_not_connect_to_windows_network_events"));
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
// Preview/tests must never read or mutate the user's login registration.
class PreviewLoginStartup final:public LoginStartup {
    bool enabled_=false;
public:
    bool enabled()override{return enabled_;}
    void setEnabled(bool enabled)override{enabled_=enabled;}
};
class PreviewSession final:public SSHSessionProtocol {
    HostRuntime value_;bool detailed_=false;
public:
    explicit PreviewSession(bool detailed=false):detailed_(detailed){}
    const HostRuntime& runtime()const override{return value_;}
    void start(const Host& host)override{value_.executionHost=host;value_.state=ConnectionState::connected;value_.forwards.clear();for(const auto& f:host.forwards)if(f.enabled)value_.forwards.push_back({f.id,forwarding(f),tr("win.app_settings"),"test listener",ForwardState::ready});if(detailed_){for(auto& f:value_.forwards)if(f.id=="web")f.state=ForwardState::failed;value_.forwards.push_back({"ssh-config-0","dynamicforward 127.0.0.1:1080",tr("win.ssh_settings"),tr("win.preview"),ForwardState::unknown});}if(onChange)onChange();}
    void stop(std::function<void()> done)override{value_.state=ConnectionState::disconnected;for(auto& f:value_.forwards)f.state=ForwardState::disconnected;if(onChange)onChange();done();}
};
LRESULT CALLBACK editProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR){
    if(msg==WM_KEYDOWN&&w=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){SendMessageW(hwnd,EM_SETSEL,0,-1);return 0;}
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,editProc,1);
    return DefSubclassProc(hwnd,msg,w,l);
}
// Remember the settings child before Shell/menu activation clears thread focus.
// This also covers moving to a button/list and caret changes made since WM_SETFOCUS.
LRESULT CALLBACK settingsFocusProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR owner){
    if(msg==WM_SETFOCUS||msg==WM_KILLFOCUS)
        SendMessageW(reinterpret_cast<HWND>(owner),WM_REMEMBER_SETTINGS_FOCUS,reinterpret_cast<WPARAM>(hwnd),0);
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(hwnd,settingsFocusProc,2);
    return DefSubclassProc(hwnd,msg,w,l);
}
class App;
LRESULT CALLBACK appProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK formProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK statusProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK ruleProc(HWND,UINT,WPARAM,LPARAM);
class App{
    HINSTANCE instance_;fs::path directory_;bool test_,start_,openConfig_,uiTest_,uiPreview_,quitting_=false,loading_=false,trayAdded_=false,menuOpen_=false;
    std::string sshConfig_;
    std::shared_ptr<EventLoop> loop_=std::make_shared<EventLoop>();std::shared_ptr<int> lifetime_=std::make_shared<int>(0);
    LoginStartupController loginStartup_;
    std::unique_ptr<ConnectionManager> manager_;SettingsModel settings_;NetworkMonitor network_;
    HWND window_=nullptr,settingsWindow_=nullptr,form_=nullptr,status_=nullptr,ruleWindow_=nullptr;HFONT font_=nullptr,headingFont_=nullptr,previewFont_=nullptr,statusFont_=nullptr,statusHeadingFont_=nullptr,ruleFont_=nullptr;HICON icon_=nullptr;
    std::map<int,HWND> controls_;std::map<int,std::string> controlTranslations_;int rule_=-1,scroll_=0;UINT taskbarCreated_=0;NOTIFYICONDATAW tray_{};std::set<std::string> authPending_;
    std::optional<RuleDraft> ruleDraft_;bool creatingRule_=false;
    bool appModal_=false;
    HWND appModalWindow_=nullptr;
    std::function<void(HWND,const TASKDIALOGCONFIG&)> dialogTest_;
    std::function<UINT(HMENU,HWND)> traySelectionTest_;
    struct SettingsFocus { HWND child=nullptr;bool edit=false;DWORD begin=0,end=0; };
    SettingsFocus settingsFocus_;
    void rememberSettingsFocus(HWND child){
        if(!settings_.open||!settingsWindow_||!child||!IsWindow(child)||!IsChild(settingsWindow_,child))return;
        SettingsFocus value;value.child=child;wchar_t kind[32]{};GetClassNameW(child,kind,32);
        value.edit=_wcsicmp(kind,L"EDIT")==0;
        if(value.edit)SendMessageW(child,EM_GETSEL,reinterpret_cast<WPARAM>(&value.begin),reinterpret_cast<LPARAM>(&value.end));
        settingsFocus_=value;
    }
    SettingsFocus languageFocusSnapshot(){rememberSettingsFocus(GetFocus());return settingsFocus_;}
    void restoreSettingsFocus(const SettingsFocus& focus){
        if(quitting_||appModal_||ruleWindow_||status_||!settings_.open||!settingsWindow_||
           !IsWindowVisible(settingsWindow_)||IsIconic(settingsWindow_)||!IsWindowEnabled(settingsWindow_)||!focus.child||
           !IsWindow(focus.child)||!IsChild(settingsWindow_,focus.child)||
           !IsWindowVisible(focus.child)||!IsWindowEnabled(focus.child))return;
        SetForegroundWindow(settingsWindow_);SetFocus(focus.child);
        if(focus.edit)SendMessageW(focus.child,EM_SETSEL,focus.begin,focus.end);
        settingsFocus_=focus;
    }
    std::function<bool(HWND,const wchar_t*)> openURL_=[](HWND owner,const wchar_t* url){
        return reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",url,nullptr,nullptr,SW_SHOWNORMAL))>32;
    };
    struct DialogContext { App* app; const TASKDIALOGCONFIG* config; std::exception_ptr error; };
    struct OwnerDialogProbe { std::function<void(HWND)> activate; std::exception_ptr error; };
    static inline thread_local OwnerDialogProbe* ownerDialogProbe_=nullptr;
    static LRESULT CALLBACK ownerDialogProbeHook(int code,WPARAM value,LPARAM data){
        if(code==HCBT_ACTIVATE&&ownerDialogProbe_){
            try{ownerDialogProbe_->activate(reinterpret_cast<HWND>(value));}
            catch(...){ownerDialogProbe_->error=std::current_exception();PostMessageW(reinterpret_cast<HWND>(value),WM_CLOSE,0,0);}
        }
        return CallNextHookEx(nullptr,code,value,data);
    }
    bool foregroundModal(){
        HWND modal=appModalWindow_?appModalWindow_:(ruleWindow_?ruleWindow_:status_);
        // Native message boxes and file pickers disable their owner without using
        // our dialog fields. The tray's hidden window can still receive messages.
        if(!modal){
            if(settings_.open&&settingsWindow_&&!IsWindowEnabled(settingsWindow_))modal=settingsWindow_;
            else if(window_&&!IsWindowEnabled(window_))modal=window_;
        }
        bool blocked=appModal_||modal;
        std::set<HWND> visited;
        while(modal&&!IsWindowEnabled(modal)&&visited.insert(modal).second){
            HWND popup=GetLastActivePopup(modal);
            if(!popup||popup==modal||!IsWindow(popup))break;
            modal=popup;
        }
        if(modal&&IsWindowVisible(modal))SetForegroundWindow(modal);
        return blocked;
    }
    static HRESULT CALLBACK dialogCallback(HWND hwnd,UINT notification,WPARAM elapsed,LPARAM value,LONG_PTR data){
        auto& context=*reinterpret_cast<DialogContext*>(data);auto& app=*context.app;
        if(notification==TDN_CREATED){
            app.appModalWindow_=hwnd;
            if(app.dialogTest_)try{app.dialogTest_(hwnd,*context.config);}catch(...){context.error=std::current_exception();PostMessageW(hwnd,TDM_CLICK_BUTTON,IDCANCEL,0);}
        }else if(notification==TDN_TIMER&&app.dialogTest_&&elapsed>=3000){
            if(!context.error)context.error=std::make_exception_ptr(std::runtime_error("native TaskDialog test did not complete: "+utf8(context.config->pszWindowTitle)));
            PostMessageW(hwnd,TDM_CLICK_BUTTON,IDCANCEL,0);
        }else if(notification==TDN_DESTROYED)app.appModalWindow_=nullptr;
        else if(notification==TDN_HYPERLINK_CLICKED&&value){
            auto url=reinterpret_cast<const wchar_t*>(value);
            if(std::wstring(url)!=ProjectURL&&std::wstring(url)!=LicenseURL)return S_OK;
            if(!app.openURL_(hwnd,url))
                SendMessageW(hwnd,TDM_SET_ELEMENT_TEXT,TDE_FOOTER,reinterpret_cast<LPARAM>(tw("win.could_not_open_the_default_browser_open_the_github_address_manual")));
        }
        return S_OK;
    }
    int appDialog(TASKDIALOGCONFIG& config){
        HWND focus=GetFocus();DialogContext context{this,&config,{}};
        config.cbSize=sizeof(config);config.hwndParent=settings_.open?settingsWindow_:window_;
        config.dwFlags|=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;
        if(dialogTest_)config.dwFlags|=TDF_CALLBACK_TIMER;
        config.pfCallback=dialogCallback;config.lpCallbackData=reinterpret_cast<LONG_PTR>(&context);
        appModal_=true;int result=IDCANCEL;auto status=TaskDialogIndirect(&config,&result,nullptr,nullptr);
        appModalWindow_=nullptr;appModal_=false;
        if(!quitting_&&focus&&IsWindow(focus)&&IsWindowEnabled(focus))SetFocus(focus);
        if(context.error)std::rethrow_exception(context.error);
        if(FAILED(status))throw LocalizedError(tr("win.could_not_open_the_dialog"));
        return result;
    }
    void showAbout(){
        if(quitting_||foregroundModal())return;
        std::wstring content=wide(tr("win.version_value",{{"version",productVersion}}))+L"\n<a href=\""+LicenseURL+L"\">MIT License</a>\n\n<a href=\""+ProjectURL+L"\">"+ProjectURL+L"</a>";
        TASKDIALOG_BUTTON close{IDCANCEL,tw("win.close")};TASKDIALOGCONFIG config{};
        config.pszWindowTitle=tw("win.about_title");config.pszMainInstruction=L"LocalForwarder";config.pszContent=content.c_str();
        config.dwFlags=TDF_USE_HICON_MAIN|TDF_ENABLE_HYPERLINKS;config.hMainIcon=icon_;
        config.cButtons=1;config.pButtons=&close;config.nDefaultButton=IDCANCEL;
        appDialog(config);
    }
    bool hasDisconnectTargets()const{
        for(const auto& host:manager_->hosts())if(auto r=manager_->runtime(host.id);r&&(r->desiredConnected||r->state!=ConnectionState::disconnected))return true;
        return false;
    }
    bool canDisconnectAll()const{return !quitting_&&!appModal_&&!manager_->isDisconnectingAll()&&hasDisconnectTargets();}
    void confirmDisconnectAll(){
        if(quitting_||foregroundModal()||!canDisconnectAll())return;
        TASKDIALOG_BUTTON buttons[]={{1001,tw("win.disconnect")},{IDCANCEL,tw("win.cancel")}};
        TASKDIALOGCONFIG config{};config.pszWindowTitle=tw("win.disconnect_all");
        config.pszMainInstruction=tw("win.disconnect_all_hosts");
        config.pszContent=tw("win.connection_attempts_retries_and_pending_automatic_recovery_will_a");
        config.cButtons=2;config.pButtons=buttons;config.nDefaultButton=IDCANCEL;
        if(appDialog(config)==1001&&!quitting_)manager_->disconnectAll();
    }
    int scale(int value)const{return MulDiv(value,window_?uiDpi(settingsWindow_?settingsWindow_:window_):96,96);}
    HWND control(int id)const{auto it=controls_.find(id);return it==controls_.end()?nullptr:it->second;}
    void set(int id,const std::wstring& value){if(control(id))setText(control(id),value);}
    std::wstring get(int id)const{return control(id)?textOf(control(id)):L"";}
    void error(const std::string& text){if(test_&&!uiPreview_){auto flat=text;std::replace(flat.begin(),flat.end(),'\n',' ');outputLine("LF_ERROR "+flat);}else MessageBoxW(ruleWindow_?ruleWindow_:(status_?status_:(settings_.open?settingsWindow_:window_)),wide(text).c_str(),L"LocalForwarder",MB_OK|MB_ICONWARNING);}
    HWND make(HWND parent,int id,const wchar_t* kind,const wchar_t* text,DWORD style){auto h=CreateWindowExW((std::wstring(kind)==L"EDIT")?WS_EX_CLIENTEDGE:0,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);if(!h)throw LocalizedError(winError());controls_[id]=h;auto translationKey=wideTranslationKey(text);if(!translationKey.empty())controlTranslations_[id]=translationKey;else controlTranslations_.erase(id);if(std::wstring(kind)==L"EDIT")SetWindowSubclass(h,editProc,1,0);if(settingsWindow_&&(parent==settingsWindow_||IsChild(settingsWindow_,parent)))SetWindowSubclass(h,settingsFocusProc,2,reinterpret_cast<DWORD_PTR>(settingsWindow_));HFONT font=id==CommandPreview?previewFont_:((id==305||id==309)?headingFont_:font_);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;}
    void place(int id,int x,int y,int width,int height){if(control(id)){UINT dpi=uiDpi(GetParent(control(id)));MoveWindow(control(id),MulDiv(x,dpi,96),MulDiv(y,dpi,96),MulDiv(std::max(1,width),dpi,96),MulDiv(std::max(1,height),dpi,96),TRUE);}}
    void label(int id,const wchar_t* text){make(form_,id,L"STATIC",text,(id==305||id==309)?0:SS_CENTERIMAGE);}
    // Measure the selected font, including Korean glyph fallback, before arranging rows.
    int textWidth(int id)const{
        HWND child=control(id);HDC dc=GetDC(child);auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
        RECT bounds{};auto text=textOf(child);DrawTextW(dc,text.c_str(),-1,&bounds,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,old);ReleaseDC(child,dc);UINT dpi=uiDpi(GetParent(child));
        return (bounds.right*96+static_cast<int>(dpi)-1)/dpi;
    }
    int headingHeight(int id,int width)const{
        HWND child=control(id);UINT dpi=uiDpi(GetParent(child));HDC dc=GetDC(child);
        auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(child,WM_GETFONT,0,0)));
        RECT bounds{0,0,MulDiv(width,dpi,96),0};auto text=textOf(child);
        DrawTextW(dc,text.c_str(),-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        SelectObject(dc,old);ReleaseDC(child,dc);
        return (bounds.bottom*96+static_cast<int>(dpi)-1)/dpi;
    }
    void updateFont(){
        HFONT old=font_,oldHeading=headingFont_,oldPreview=previewFont_;UINT dpi=uiDpi(settingsWindow_?settingsWindow_:window_);
        font_=uiFont(dpi);headingFont_=uiFont(dpi,FW_SEMIBOLD);previewFont_=uiFont(dpi,FW_NORMAL,L"Consolas");
        for(auto& [id,h]:controls_){HFONT font=id==CommandPreview?previewFont_:((id==305||id==309)?headingFont_:font_);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);}
        for(auto font:{old,oldHeading,oldPreview})if(font)DeleteObject(font);
        if(status_)updateStatusFont();if(ruleWindow_)updateRuleFont();
    }
    void updateStatusFont(){
        if(!status_)return;HFONT old=statusFont_,oldHeading=statusHeadingFont_;
        statusFont_=uiFont(uiDpi(status_));statusHeadingFont_=uiFont(uiDpi(status_),FW_SEMIBOLD);
        for(int id:{StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose})if(control(id))SendMessageW(control(id),WM_SETFONT,reinterpret_cast<WPARAM>(id==StatusHeading?statusHeadingFont_:statusFont_),TRUE);
        if(old)DeleteObject(old);if(oldHeading)DeleteObject(oldHeading);
    }
    int choose(const wchar_t* title,const wchar_t* body,const std::vector<std::pair<int,const wchar_t*>>& choices,int defaultButton){std::vector<TASKDIALOG_BUTTON> buttons;for(auto& choice:choices)buttons.push_back({choice.first,choice.second});TASKDIALOGCONFIG config{};config.cbSize=sizeof(config);config.hwndParent=settingsWindow_;config.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;config.pszWindowTitle=L"LocalForwarder";config.pszMainInstruction=title;config.pszContent=body;config.cButtons=static_cast<UINT>(buttons.size());config.pButtons=buttons.data();config.nDefaultButton=defaultButton;return appDialog(config);}
public:
    App(HINSTANCE instance,fs::path directory,bool test,bool start,bool openConfig,std::string config,bool uiTest=false,bool uiPreview=false,std::shared_ptr<LoginStartup> startup={}):instance_(instance),directory_(std::move(directory)),test_(test),start_(start),openConfig_(openConfig),uiTest_(uiTest),uiPreview_(uiPreview),sshConfig_(std::move(config)),loginStartup_(startup?std::move(startup):(test?std::shared_ptr<LoginStartup>(std::make_shared<PreviewLoginStartup>()):std::make_shared<RegistryLoginStartup>(executablePath()))){}
    ~App(){lifetime_.reset();network_.stop();if(loop_)loop_->detach();manager_.reset();if(trayAdded_)Shell_NotifyIconW(NIM_DELETE,&tray_);if(ruleWindow_)DestroyWindow(ruleWindow_);if(status_)DestroyWindow(status_);if(settingsWindow_)DestroyWindow(settingsWindow_);if(window_)DestroyWindow(window_);for(auto font:{font_,headingFont_,previewFont_,statusFont_,statusHeadingFont_,ruleFont_})if(font)DeleteObject(font);clearMenuBitmaps();if(icon_)DestroyIcon(icon_);}
    void create(const std::wstring& title){
        WNDCLASSW cls{};cls.hInstance=instance_;cls.lpfnWndProc=appProc;cls.lpszClassName=AppClass;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw LocalizedError(winError());
        cls.lpfnWndProc=formProc;cls.lpszClassName=FormClass;if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw LocalizedError(winError());cls.lpfnWndProc=statusProc;cls.lpszClassName=StatusClass;if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw LocalizedError(winError());
        cls.lpfnWndProc=ruleProc;cls.lpszClassName=RuleClass;if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw LocalizedError(winError());
        window_=CreateWindowExW(0,AppClass,title.c_str(),WS_OVERLAPPED,0,0,1,1,nullptr,nullptr,instance_,this);if(!window_)throw LocalizedError(winError());loop_->attach(window_);
        auto post=loop_->poster();auto schedule=loop_->scheduler();auto directory=directory_;auto config=sshConfig_;
        manager_=std::make_unique<ConnectionManager>(StoreAccess{[directory]{return loadConfig(directory);},[directory](const std::string& id){return loadHost(directory,id);},[directory](const std::string& id){deleteHost(directory,id);}},[post,schedule,config,uiTest=uiTest_,uiPreview=uiPreview_]()->std::shared_ptr<SSHSessionProtocol>{if(uiTest)return std::make_shared<PreviewSession>(uiPreview);return std::make_shared<SSHSession>(post,schedule,config);},schedule);
        manager_->onChange=[this]{refresh();};manager_->onError=[this](const std::string&,const std::string& text){if(test_)error(text);else refresh();};manager_->onExit=[this]{finishQuit();};
        icon_=static_cast<HICON>(LoadImageW(instance_,MAKEINTRESOURCEW(2),IMAGE_ICON,32,32,0));if(!icon_)icon_=forwardingIcon();
        if(!test_||uiPreview_){taskbarCreated_=RegisterWindowMessageW(L"TaskbarCreated");updateTray();try{if(!uiPreview_){std::weak_ptr<int> alive=lifetime_;network_.start(post,[this,alive](bool available){if(!alive.expired()&&!quitting_)manager_->networkChanged(available);});}}catch(const std::exception& e){error(e.what());}}
        try{manager_->reloadConfiguration();if(uiPreview_){manager_->connect("a");openSettings({},true);}else if(test_){if(start_)manager_->startAll();}else if(openConfig_||!fs::exists(directory_/L"config.json"))openSettings({},true);else manager_->startAll();}
        catch(const std::exception& e){if(!test_)openSettings({},true);error(std::string(e.what())+tr("win.nthe_existing_configuration_file_was_preserved"));}
        if(test_&&!uiTest_){snapshot();outputLine("LF_READY");startInput();}
        if(uiPreview_)loop_->post([this]{
            if(previewScene==L"rule"){rule_=0;showRule(false);}
            else if(previewScene==L"status")showStatus();
            else if(previewScene==L"about")showAbout();
            else if(previewScene==L"confirm")confirmDisconnectAll();
            else if(previewScene==L"error"){showRule(true);set(LocalPort,L"invalid");closeRule(true);}
            else if(previewScene==L"tray")trayMenu();
            else if(previewScene==L"tray-host"){
                createTrayMenu();RECT area{};GetWindowRect(settingsWindow_,&area);SetForegroundWindow(window_);menuOpen_=true;
                TrackPopupMenuEx(menus_.at("a").menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,area.left+scale(24),area.top+scale(60),window_,nullptr);
                menuOpen_=false;destroyTrayMenu();
            }
        });
    }
    HWND settingsWindow()const{return settingsWindow_;}HWND statusWindow()const{return status_;}
    bool preprocess(MSG& message){
        if(appModal_)return appModalWindow_&&IsDialogMessageW(appModalWindow_,&message);
        HWND dialog=ruleWindow_?ruleWindow_:(status_?status_:(settings_.open?settingsWindow_:nullptr));
        if(dialog&&message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){if(ruleWindow_)closeRule(false);else if(status_)closeStatus();else closeSettings(false);return true;}
        if(dialog&&message.message==WM_KEYDOWN&&message.wParam==VK_RETURN&&dialog==status_){closeStatus();return true;}
        if(dialog&&message.message==WM_KEYDOWN&&message.wParam==VK_RETURN&&dialog==ruleWindow_){closeRule(GetFocus()!=control(RuleCancel));return true;}
        if(dialog&&IsDialogMessageW(dialog,&message)){ensureFocusVisible();return true;}return false;
    }
private:
    std::map<std::string,lf::Cancel> authObservers_;
    HMENU activeMenu_=nullptr;std::map<UINT,std::pair<std::string,int>> menuActions_;
    void createSettings(){
        settingsWindow_=CreateWindowExW(WS_EX_CONTROLPARENT,AppClass,tw("win.localforwarder_settings"),WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,scale(900),scale(650),window_,nullptr,instance_,this);
        if(!settingsWindow_)throw LocalizedError(winError());centerInWorkArea(settingsWindow_,900,650);updateFont();SendMessageW(settingsWindow_,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(icon_));
        make(settingsWindow_,NewHost,L"BUTTON",tw("win.new_host"),WS_TABSTOP);make(settingsWindow_,DeleteHost,L"BUTTON",tw("win.delete_host"),WS_TABSTOP);
        make(settingsWindow_,Hosts,L"LISTBOX",L"",LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP|WS_BORDER);
        make(settingsWindow_,Okay,L"BUTTON",tw("win.ok"),WS_TABSTOP|BS_DEFPUSHBUTTON);make(settingsWindow_,CancelButton,L"BUTTON",tw("win.cancel"),WS_TABSTOP);
        form_=CreateWindowExW(WS_EX_CONTROLPARENT,FormClass,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL,0,0,1,1,settingsWindow_,nullptr,instance_,this);if(!form_)throw LocalizedError(winError());
        label(300,tw("win.name"));label(310,tw("win.description_optional"));label(301,tw("win.ssh_alias_address"));label(302,tw("win.username_optional"));label(303,tw("win.ssh_port_optional"));label(304,tw("win.private_key_optional"));
        for(int id:{Name,Description,Target,User,Port,Key})make(form_,id,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
        make(form_,ChooseKey,L"BUTTON",tw("win.choose_private_key"),WS_TABSTOP);
        make(form_,AutoConnect,L"BUTTON",tw("win.auto_connect"),BS_AUTOCHECKBOX|WS_TABSTOP);make(form_,ExitFailure,L"BUTTON",L"ExitOnForwardFailure",BS_AUTOCHECKBOX|WS_TABSTOP);make(form_,RetryRefused,L"BUTTON",tw("win.retry_on_connection_refused"),BS_AUTOCHECKBOX|WS_TABSTOP);
        label(305,tw("win.forwarding_rules_app_rules_bind_to_127_0_0_1"));
        make(form_,Rules,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER);
        ListView_SetExtendedListViewStyle(control(Rules),LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        const wchar_t* titles[]={tw("win.enabled"),tw("win.local_port"),tw("win.destination_host"),tw("win.destination_port"),tw("win.description")};for(int i=0;i<5;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<LPWSTR>(titles[i]);col.cx=scale(i==2?260:90);ListView_InsertColumn(control(Rules),i,&col);}
        make(form_,RuleNew,L"BUTTON",tw("win.new_rule"),WS_TABSTOP);make(form_,RuleEdit,L"BUTTON",tw("win.edit_rule"),WS_TABSTOP);make(form_,RuleDelete,L"BUTTON",tw("win.delete_rule"),WS_TABSTOP);make(form_,RuleSort,L"BUTTON",tw("win.sort_by_local_port"),WS_TABSTOP);
        label(309,tw("win.command_preview_edited_values"));make(form_,CommandPreview,L"EDIT",L"",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP);
        make(form_,Save,L"BUTTON",tw("win.save_selected_host"),WS_TABSTOP);make(form_,Connect,L"BUTTON",tw("win.connect_reconnect"),WS_TABSTOP);make(form_,Authentication,L"BUTTON",tw("win.authentication_setup"),WS_TABSTOP);make(form_,ConnectionStatus,L"BUTTON",tw("win.connection_status"),WS_TABSTOP);
        layoutSettings();
    }
    void layoutSettings(){if(!settingsWindow_||!form_)return;RECT rect{};GetClientRect(settingsWindow_,&rect);int w=MulDiv(rect.right,96,uiDpi(settingsWindow_)),h=MulDiv(rect.bottom,96,uiDpi(settingsWindow_));
        const int m=Spacing::margin,gap=Spacing::gap,bh=Spacing::button;
        const int sidebar=w>=740?216:std::clamp(w/4,120,180);bool stacked=sidebar<216||textWidth(NewHost)+textWidth(DeleteHost)+48>sidebar;
        if(stacked){place(NewHost,m,m,sidebar,bh);place(DeleteHost,m,m+bh+gap,sidebar,bh);}
        else{place(NewHost,m,m,100,bh);place(DeleteHost,m+108,m,108,bh);}
        int listTop=m+(stacked?2:1)*(bh+gap);place(Hosts,m,listTop,sidebar,std::max(24,h-listTop-68));
        int footerWidth=std::max({82,textWidth(Okay)+24,textWidth(CancelButton)+24});place(Okay,w-m-2*footerWidth-gap,h-m-bh,footerWidth,bh);place(CancelButton,w-m-footerWidth,h-m-bh,footerWidth,bh);
        int formLeft=sidebar+2*m;MoveWindow(form_,scale(formLeft),scale(m),scale(std::max(1,w-formLeft-m)),scale(std::max(32,h-70)),TRUE);layoutForm();
    }
    void layoutForm(){if(!form_)return;RECT rect{};GetClientRect(form_,&rect);int w=rect.right*96/uiDpi(settingsWindow_),h=rect.bottom*96/uiDpi(settingsWindow_);
        struct Placement{int id,x,y,width,height;};std::vector<Placement> places;
        auto add=[&](int id,int x,int y,int width,int height){places.push_back({id,x,y,width,height});};
        const int gap=Spacing::gap,ih=Spacing::input,bh=Spacing::button,section=Spacing::section;
        int content=std::max(1,w-Spacing::margin),y=0,labelWidth=0;
        for(int id:{300,310,301,302,303,304})labelWidth=std::max(labelWidth,textWidth(id)+4);
        int inputLeft=labelWidth+gap;bool narrow=content<std::max(490,inputLeft+220);
        auto field=[&](int labelID,int editID){
            if(narrow){add(labelID,0,y,content,ih);add(editID,0,y+ih+4,content,ih);y+=2*ih+4+gap;}
            else{add(labelID,0,y,labelWidth,ih);add(editID,inputLeft,y,content-inputLeft,ih);y+=ih+gap;}
        };
        field(300,Name);field(310,Description);field(301,Target);
        int portLabel=textWidth(303)+4,portWidth=80;
        if(!narrow&&content>=inputLeft+140+section+portLabel+gap+portWidth){
            int portLeft=content-portWidth,portLabelLeft=portLeft-gap-portLabel;
            add(302,0,y,labelWidth,ih);add(User,inputLeft,y,portLabelLeft-section-inputLeft,ih);
            add(303,portLabelLeft,y,portLabel,ih);add(Port,portLeft,y,portWidth,ih);y+=ih+gap;
        }else{field(302,User);field(303,Port);}
        if(narrow){field(304,Key);add(ChooseKey,0,y,content,bh);y+=bh+gap;}
        else{
            int chooseWidth=std::max(132,textWidth(ChooseKey)+20),offset=(bh-ih)/2;
            add(304,0,y+offset,labelWidth,ih);add(Key,inputLeft,y+offset,content-inputLeft-gap-chooseWidth,ih);
            add(ChooseKey,content-chooseWidth,y,chooseWidth,bh);y+=bh+gap;
        }
        int optionWidth=0;for(int id:{AutoConnect,ExitFailure,RetryRefused})optionWidth+=textWidth(id)+30;
        bool stackOptions=optionWidth+2*gap>content;int x=0;
        for(int id:{AutoConnect,ExitFailure,RetryRefused}){
            int width=stackOptions?content:textWidth(id)+30;add(id,x,y,width,ih);
            if(stackOptions)y+=ih+gap;else x+=width+gap;
        }
        y+=stackOptions?section-gap:ih+section;
        auto heading=[&](int id){int height=headingHeight(id,content);add(id,0,y,content,height);y+=height+gap;};
        auto buttons=[&](std::initializer_list<int> ids,int columns){
            int required=0;for(int id:ids)required=std::max(required,textWidth(id)+24);while(columns>1&&(content-(columns-1)*gap)/columns<required)--columns;int width=(content-(columns-1)*gap)/columns,index=0;
            for(int id:ids){add(id,(index%columns)*(width+gap),y+(index/columns)*(bh+gap),width,bh);++index;}
            y+=((index+columns-1)/columns)*(bh+gap)-gap;
        };
        heading(305);add(Rules,0,y,content,100);y+=100+gap;
        buttons({RuleNew,RuleEdit,RuleDelete,RuleSort},content>=432?4:(content>=324?3:(content>=216?2:1)));y+=section;
        heading(309);add(CommandPreview,0,y,content,64);y+=64+section;
        buttons({Save,Connect,Authentication,ConnectionStatus},content>=564?4:(content>=278?2:1));y+=Spacing::margin;
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS,0,y-1,static_cast<UINT>(std::max(1,h)),scroll_,0};SetScrollInfo(form_,SB_VERT,&si,TRUE);si.fMask=SIF_POS;GetScrollInfo(form_,SB_VERT,&si);scroll_=si.nPos;
        for(const auto& item:places)place(item.id,item.x,item.y-scroll_,item.width,item.height);
        int enabledWidth=content<360?36:48,portWidthColumn=content<360?70:90;
        int available=content-enabledWidth-2*portWidthColumn-4,hostWidth=std::max(140,available/2);
        ListView_SetColumnWidth(control(Rules),0,scale(enabledWidth));ListView_SetColumnWidth(control(Rules),1,scale(portWidthColumn));ListView_SetColumnWidth(control(Rules),2,scale(hostWidth));ListView_SetColumnWidth(control(Rules),3,scale(portWidthColumn));ListView_SetColumnWidth(control(Rules),4,scale(std::max(160,available-hostWidth)));
    }
    void scrollForm(int code,int wheel=0){SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(form_,SB_VERT,&si);int next=si.nPos;switch(code){case SB_LINEUP:next-=24;break;case SB_LINEDOWN:next+=24;break;case SB_PAGEUP:next-=si.nPage;break;case SB_PAGEDOWN:next+=si.nPage;break;case SB_THUMBPOSITION:case SB_THUMBTRACK:next=si.nTrackPos;break;default:next-=wheel;}scroll_=next;layoutForm();}
    void ensureFocusVisible(){if(!form_)return;HWND focus=GetFocus();if(!focus||!IsChild(form_,focus))return;RECT area{},client{};GetWindowRect(focus,&area);MapWindowPoints(nullptr,form_,reinterpret_cast<POINT*>(&area),2);GetClientRect(form_,&client);int dpi=uiDpi(settingsWindow_);if(area.top<0)scroll_-=(-area.top*96+dpi-1)/dpi;else if(area.bottom>client.bottom)scroll_+=((area.bottom-client.bottom)*96+dpi-1)/dpi;layoutForm();}
    void populateHosts(){if(!settings_.open)return;loading_=true;SendMessageW(control(Hosts),LB_RESETCONTENT,0,0);int selected=-1;for(size_t i=0;i<settings_.order.size();++i){auto& id=settings_.order[i];auto& h=settings_.drafts.at(id).host;auto runtime=manager_->runtime(id);std::string title=(settings_.dirty(id)?"● ":"")+h.name+(runtime&&runtime->temporary?tr("win.temporary"):"");SendMessageW(control(Hosts),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(wide(title).c_str()));if(id==settings_.selected)selected=static_cast<int>(i);}SendMessageW(control(Hosts),LB_SETCURSEL,selected,0);loading_=false;}
    void capture(){if(loading_||!settings_.open||!settings_.drafts.contains(settings_.selected))return;auto& draft=settings_.drafts.at(settings_.selected);draft.host.name=utf8(get(Name));draft.host.description=utf8(get(Description));draft.host.target=utf8(get(Target));draft.host.user=utf8(get(User));draft.host.identityFile=utf8(get(Key));draft.port=utf8(get(Port));draft.host.autoConnect=SendMessageW(control(AutoConnect),BM_GETCHECK,0,0)==BST_CHECKED;draft.host.exitOnForwardFailure=SendMessageW(control(ExitFailure),BM_GETCHECK,0,0)==BST_CHECKED;draft.host.retryOnConnectionRefused=SendMessageW(control(RetryRefused),BM_GETCHECK,0,0)==BST_CHECKED;
    }
    void preview(){if(!settings_.drafts.contains(settings_.selected))return;try{auto host=settings_.validated();std::wstring command=quoteWindowsArgument(systemSSH().wstring());for(auto& argument:sshArguments(host))command+=L" "+quoteWindowsArgument(wide(argument));set(CommandPreview,command);}catch(const std::exception& e){set(CommandPreview,wide(e.what()));}}
    void fillRules(){loading_=true;ListView_DeleteAllItems(control(Rules));if(settings_.drafts.contains(settings_.selected)){auto& draft=settings_.drafts.at(settings_.selected);for(size_t i=0;i<draft.host.forwards.size();++i){auto& f=draft.host.forwards[i];LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=const_cast<LPWSTR>(L"");ListView_InsertItem(control(Rules),&item);auto ports=draft.rulePorts.at(f.id);auto local=wide(ports.first),host=wide(f.remoteHost),remote=wide(ports.second),description=wide(f.description);ListView_SetItemText(control(Rules),item.iItem,1,local.data());ListView_SetItemText(control(Rules),item.iItem,2,host.data());ListView_SetItemText(control(Rules),item.iItem,3,remote.data());ListView_SetItemText(control(Rules),item.iItem,4,description.data());ListView_SetCheckState(control(Rules),item.iItem,f.enabled);}}
        loading_=false;
    }
    void fillRuleEditor(){bool valid=settings_.drafts.contains(settings_.selected)&&rule_>=0&&static_cast<size_t>(rule_)<settings_.drafts.at(settings_.selected).host.forwards.size();for(int id:{RuleEdit,RuleDelete})EnableWindow(control(id),valid);EnableWindow(control(RuleSort),settings_.canSortRules());}
    void sortRulesByLocalPort(){
        if(!settings_.canSortRules())return;
        capture();const auto& before=settings_.drafts.at(settings_.selected).host.forwards;
        int selectedRow=ListView_GetNextItem(control(Rules),-1,LVNI_SELECTED);
        std::string selectedID=selectedRow>=0&&static_cast<size_t>(selectedRow)<before.size()?before[selectedRow].id:"";
        if(!settings_.sortRulesByLocalPort())return;
        rule_=-1;fillRules();
        const auto& rules=settings_.drafts.at(settings_.selected).host.forwards;
        if(!selectedID.empty())for(size_t i=0;i<rules.size();++i)if(rules[i].id==selectedID){
            rule_=static_cast<int>(i);loading_=true;
            ListView_SetItemState(control(Rules),rule_,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            ListView_EnsureVisible(control(Rules),rule_,FALSE);loading_=false;break;
        }
        fillRuleEditor();populateHosts();preview();
    }
    void updateRuleFont(){if(!ruleWindow_)return;HFONT old=ruleFont_;ruleFont_=uiFont(uiDpi(ruleWindow_));for(int id:std::initializer_list<int>{RuleEnabled,RuleDescription,LocalPort,RemoteHost,RemotePort,RuleOkay,RuleCancel,320,321,322,323})if(control(id))SendMessageW(control(id),WM_SETFONT,reinterpret_cast<WPARAM>(ruleFont_),TRUE);if(old)DeleteObject(old);}
    void layoutRule(){if(!ruleWindow_||!control(RuleOkay))return;RECT rect{};GetClientRect(ruleWindow_,&rect);int w=MulDiv(rect.right,96,uiDpi(ruleWindow_)),h=MulDiv(rect.bottom,96,uiDpi(ruleWindow_));
        const int m=Spacing::margin,gap=Spacing::gap,ih=Spacing::input,bh=Spacing::button;
        int row=h<240?30:ih+gap,first=h<240?36:44,footer=h-m-bh,labelWidth=0;
        for(int id:{323,320,321,322})labelWidth=std::max(labelWidth,textWidth(id)+4);
        place(RuleEnabled,m,6,w-2*m,ih);int index=0;
        for(auto [labelID,editID]:{std::pair{323,RuleDescription},{320,LocalPort},{321,RemoteHost},{322,RemotePort}}){
            int y=first+row*index++;place(labelID,m,y,labelWidth,ih);place(editID,m+labelWidth+gap,y,w-2*m-labelWidth-gap,ih);
        }
        int footerWidth=std::max({84,textWidth(RuleOkay)+24,textWidth(RuleCancel)+24});place(RuleOkay,w-m-2*footerWidth-gap,footer,footerWidth,bh);place(RuleCancel,w-m-footerWidth,footer,footerWidth,bh);
    }
    void showRule(bool creating){if(appModal_){foregroundModal();return;}if(ruleWindow_){SetForegroundWindow(ruleWindow_);return;}if(status_||!settings_.open)return;capture();auto& host=settings_.drafts.at(settings_.selected).host;
        if(!creating&&(rule_<0||static_cast<size_t>(rule_)>=host.forwards.size()))return;
        ruleDraft_.emplace(creating?Forward{newId(),true,8080,"localhost",80}:host.forwards[rule_]);creatingRule_=creating;settings_.modalHost=settings_.selected;
        ruleWindow_=CreateWindowExW(WS_EX_CONTROLPARENT,RuleClass,creating?tw("win.new_rule_120"):tw("win.edit_rule_121"),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,CW_USEDEFAULT,CW_USEDEFAULT,scale(460),scale(280),settingsWindow_,nullptr,instance_,this);
        if(!ruleWindow_){settings_.modalHost.clear();ruleDraft_.reset();throw LocalizedError(winError());}
        RECT desiredClient{0,0,0,scale(230)};
        AdjustWindowRectExForDpi(&desiredClient,static_cast<DWORD>(GetWindowLongPtrW(ruleWindow_,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(ruleWindow_,GWL_EXSTYLE)),GetDpiForWindow(ruleWindow_));
        centerInWorkArea(ruleWindow_,460,MulDiv(desiredClient.bottom-desiredClient.top,96,uiDpi(ruleWindow_)),settingsWindow_);
        make(ruleWindow_,RuleEnabled,L"BUTTON",tw("win.enabled"),BS_AUTOCHECKBOX|WS_TABSTOP);SendMessageW(control(RuleEnabled),BM_SETCHECK,ruleDraft_->rule.enabled?BST_CHECKED:BST_UNCHECKED,0);
        make(ruleWindow_,323,L"STATIC",tw("win.description_optional"),SS_CENTERIMAGE);make(ruleWindow_,320,L"STATIC",tw("win.local_port"),SS_CENTERIMAGE);make(ruleWindow_,321,L"STATIC",tw("win.destination_host"),SS_CENTERIMAGE);make(ruleWindow_,322,L"STATIC",tw("win.destination_port"),SS_CENTERIMAGE);
        for(int id:{RuleDescription,LocalPort,RemoteHost,RemotePort})make(ruleWindow_,id,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
        set(RuleDescription,wide(ruleDraft_->rule.description));set(LocalPort,wide(ruleDraft_->localPort));set(RemoteHost,wide(ruleDraft_->rule.remoteHost));set(RemotePort,wide(ruleDraft_->remotePort));
        make(ruleWindow_,RuleOkay,L"BUTTON",tw("win.ok"),WS_TABSTOP|BS_DEFPUSHBUTTON);make(ruleWindow_,RuleCancel,L"BUTTON",tw("win.cancel"),WS_TABSTOP);
        EnableWindow(settingsWindow_,FALSE);updateRuleFont();layoutRule();ShowWindow(ruleWindow_,SW_SHOW);SetForegroundWindow(ruleWindow_);SetFocus(control(RuleDescription));
    }
    void closeRule(bool accepted){if(!ruleWindow_)return;
        if(accepted){try{ruleDraft_->rule.enabled=SendMessageW(control(RuleEnabled),BM_GETCHECK,0,0)==BST_CHECKED;ruleDraft_->rule.description=utf8(get(RuleDescription));ruleDraft_->rule.remoteHost=utf8(get(RemoteHost));ruleDraft_->localPort=utf8(get(LocalPort));ruleDraft_->remotePort=utf8(get(RemotePort));settings_.applyRule(settings_.modalHost,*ruleDraft_,creatingRule_);}catch(const std::exception& e){error(e.what());return;}}
        auto selectedID=ruleDraft_->rule.id;HWND old=ruleWindow_;ruleWindow_=nullptr;ruleDraft_.reset();for(int id:std::initializer_list<int>{RuleEnabled,RuleDescription,LocalPort,RemoteHost,RemotePort,RuleOkay,RuleCancel,320,321,322,323})controls_.erase(id);DestroyWindow(old);if(ruleFont_){DeleteObject(ruleFont_);ruleFont_=nullptr;}settings_.modalHost.clear();EnableWindow(settingsWindow_,TRUE);
        if(accepted){fillRules();auto& rules=settings_.drafts.at(settings_.selected).host.forwards;for(size_t i=0;i<rules.size();++i)if(rules[i].id==selectedID){rule_=static_cast<int>(i);ListView_SetItemState(control(Rules),rule_,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);ListView_EnsureVisible(control(Rules),rule_,FALSE);break;}populateHosts();preview();}
        fillRuleEditor();if(settings_.open){SetForegroundWindow(settingsWindow_);SetFocus(control(Rules));}
    }
    void fill(){loading_=true;if(settings_.drafts.contains(settings_.selected)){auto& d=settings_.drafts.at(settings_.selected);set(Name,wide(d.host.name));set(Description,wide(d.host.description));set(Target,wide(d.host.target));set(User,wide(d.host.user));set(Port,wide(d.port));set(Key,wide(d.host.identityFile));SendMessageW(control(AutoConnect),BM_SETCHECK,d.host.autoConnect?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(control(ExitFailure),BM_SETCHECK,d.host.exitOnForwardFailure?BST_CHECKED:BST_UNCHECKED,0);SendMessageW(control(RetryRefused),BM_SETCHECK,d.host.retryOnConnectionRefused?BST_CHECKED:BST_UNCHECKED,0);}loading_=false;rule_=-1;fillRules();fillRuleEditor();populateHosts();preview();refreshButtons();}
    void addHost(){Host host;host.id=newId();host.name=tr("win.new_host_123");settings_.add(host);fill();}
    void openSettings(const std::string& id={},bool startup=false){if(quitting_||foregroundModal())return;if(settings_.open){if(!status_&&!ruleWindow_&&!id.empty()){capture();if(settings_.select(id))fill();}ShowWindow(settingsWindow_,SW_RESTORE);SetForegroundWindow(ruleWindow_?ruleWindow_:(status_?status_:settingsWindow_));return;}
        if(!settingsWindow_)createSettings();Config saved;std::vector<Host> transient;std::string failure;try{manager_->reloadConfiguration();saved=loadConfig(directory_);for(auto host:manager_->hosts()){auto runtime=manager_->runtime(host.id);if(runtime&&runtime->executionHost)host=*runtime->executionHost;transient.push_back(host);}}catch(const std::exception& e){failure=e.what();}
        settings_.load(saved,transient,startup);if(settings_.order.empty())addHost();if(!id.empty())settings_.select(id);fill();ShowWindow(settingsWindow_,SW_SHOW);SetForegroundWindow(settingsWindow_);if(!failure.empty())error(failure);
    }
    void closeSettings(bool accepted){if(appModal_||!settings_.open||ruleWindow_||status_)return;capture();if(accepted&&settings_.anyDirty()){if(choose(tw("win.you_have_unsaved_changes"),tw("win.ok_does_not_save_automatically_saved_settings_and_changes_applied"),{{1001,tw("win.continue_editing")},{1002,tw("win.close_without_saving")}},1001)!=1002)return;}closeStatus();bool connect=settings_.close(accepted);ShowWindow(settingsWindow_,SW_HIDE);if(connect)try{manager_->startAll();}catch(const std::exception& e){error(e.what());}}
    Host saveSelected(){capture();auto host=settings_.validated();saveHost(directory_,host);manager_->saved(host);settings_.saved(host);populateHosts();refreshButtons();return host;}
    void connectSelected(){capture();auto host=settings_.validated();if(settings_.dirty(host.id)){int answer=choose(tw("win.connect_or_reconnect_using_the_edited_settings"),tw("win.settings_used_without_saving_remain_active_for_this_app_session"),{{1001,tw("win.save_and_reconnect")},{1002,tw("win.reconnect_without_saving")},{IDCANCEL,tw("win.cancel")}},1001);if(answer==1001){host=saveSelected();manager_->connect(host,false);}else if(answer==1002)manager_->connect(host,true);}else manager_->connect(host.id);}
    void authenticate(){if(uiPreview_)return;capture();if(authPending_.contains(settings_.selected))return;if(choose(tw("win.set_up_authentication_for_the_selected_host"),tw("win.save_the_selected_host_and_verify_the_fingerprint_and_password_in"),{{1001,tw("win.save_and_open_terminal")},{IDCANCEL,tw("win.cancel")}},1001)!=1001)return;Host original=saveSelected();authPending_.insert(original.id);refreshButtons();std::weak_ptr<int> alive=lifetime_;
        try{authObservers_[original.id]=launchRecovery(original.id,[this,alive,original](int result){if(alive.expired()||quitting_)return;authPending_.erase(original.id);authObservers_.erase(original.id);capture();try{auto updated=loadHost(directory_,original.id);manager_->saved(updated);settings_.authenticationFinished(original,updated);if(settings_.open&&settings_.selected==original.id){if(!ruleWindow_)fill();else {loading_=true;set(Key,wide(settings_.drafts.at(original.id).host.identityFile));loading_=false;populateHosts();preview();}}else populateHosts();if(result!=0)error(tr("win.authentication_setup_could_not_be_completed_check_the_result_in_t"));}catch(const std::exception& e){error(e.what());}refreshButtons();},loop_->poster());}
        catch(...){authPending_.erase(original.id);refreshButtons();throw;}
    }
    void refreshButtons(){if(!settings_.open)return;auto runtime=manager_->runtime(settings_.selected);bool busy=runtime&&(runtime->state==ConnectionState::connecting||runtime->state==ConnectionState::stopping);EnableWindow(control(Connect),!busy&&!quitting_);EnableWindow(control(Authentication),!uiPreview_&&!quitting_&&!authPending_.contains(settings_.selected));}
    void chooseKey(){std::vector<wchar_t> file(32768,0);auto value=get(Key);std::copy_n(value.begin(),std::min(value.size(),file.size()-1),file.begin());OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=settingsWindow_;dialog.lpstrFile=file.data();dialog.nMaxFile=static_cast<DWORD>(file.size());dialog.lpstrTitle=tw("win.choose_private_key_136");dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;if(GetOpenFileNameW(&dialog)){set(Key,file.data());capture();preview();populateHosts();}}
    void showStatus(){if(appModal_){foregroundModal();return;}if(ruleWindow_)return;capture();if(status_){SetForegroundWindow(status_);return;}settings_.showStatus();if(settings_.modalHost.empty())return;status_=CreateWindowExW(WS_EX_CONTROLPARENT,StatusClass,tw("win.current_connection_status"),WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,CW_USEDEFAULT,CW_USEDEFAULT,scale(760),scale(520),settingsWindow_,nullptr,instance_,this);if(!status_){settings_.closeStatus();throw LocalizedError(winError());}centerInWorkArea(status_,760,520,settingsWindow_);
        make(status_,StatusHeading,L"STATIC",L"",SS_CENTERIMAGE|SS_ENDELLIPSIS);make(status_,StatusSummary,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);make(status_,StatusForwards,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER);ListView_SetExtendedListViewStyle(control(StatusForwards),LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);const wchar_t* names[]={tw("win.rule"),tw("win.source"),tw("win.state"),tw("win.details")};for(int i=0;i<4;++i){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<LPWSTR>(names[i]);col.cx=scale(i==0?200:120);ListView_InsertColumn(control(StatusForwards),i,&col);}
        make(status_,StatusErrors,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP);make(status_,StatusClose,L"BUTTON",tw("win.close"),WS_TABSTOP|BS_DEFPUSHBUTTON);EnableWindow(settingsWindow_,FALSE);updateStatusFont();layoutStatus();refreshStatus();ShowWindow(status_,SW_SHOW);SetForegroundWindow(status_);SetFocus(control(StatusClose));
    }
    void closeStatus(){if(!status_)return;HWND old=status_;status_=nullptr;for(int id:{StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose})controls_.erase(id);DestroyWindow(old);if(statusFont_){DeleteObject(statusFont_);statusFont_=nullptr;}if(statusHeadingFont_){DeleteObject(statusHeadingFont_);statusHeadingFont_=nullptr;}settings_.closeStatus();if(settingsWindow_){EnableWindow(settingsWindow_,TRUE);if(settings_.open)SetForegroundWindow(settingsWindow_);}}
    void layoutStatus(){if(!status_||!control(StatusClose))return;RECT rect{};GetClientRect(status_,&rect);UINT dpi=uiDpi(status_);auto statusScale=[dpi](int value){return MulDiv(value,dpi,96);};int w=MulDiv(rect.right,96,dpi),h=MulDiv(rect.bottom,96,dpi);int footer=h-Spacing::margin-Spacing::button,summaryHeight=std::clamp((h-150)/3,40,76),forwardsTop=54+summaryHeight,errorsHeight=std::clamp((h-150)/2,40,92),errorsTop=footer-10-errorsHeight;place(StatusHeading,12,12,w-24,26);place(StatusSummary,12,44,w-24,summaryHeight);place(StatusForwards,12,forwardsTop,w-24,std::max(24,errorsTop-forwardsTop-10));place(StatusErrors,12,errorsTop,w-24,errorsHeight);place(StatusClose,w-Spacing::margin-88,footer,88,Spacing::button);ListView_SetColumnWidth(control(StatusForwards),0,statusScale(180));ListView_SetColumnWidth(control(StatusForwards),1,statusScale(85));ListView_SetColumnWidth(control(StatusForwards),2,statusScale(90));ListView_SetColumnWidth(control(StatusForwards),3,statusScale(std::max(160,w-395)));}
    static std::wstring forwardTitle(ForwardState state){switch(state){case ForwardState::preparing:return tw("win.preparing");case ForwardState::ready:return tw("win.ready");case ForwardState::failed:return tw("win.failed_to_open");case ForwardState::disconnected:return tw("win.disconnected");case ForwardState::unknown:return tw("win.unknown_146");}return tw("win.unknown_146");}
    void refreshStatus(){if(!status_||settings_.modalHost.empty())return;auto runtime=manager_->runtime(settings_.modalHost);Host host;auto draft=settings_.drafts.find(settings_.modalHost);if(draft!=settings_.drafts.end())host=draft->second.host;if(runtime&&runtime->executionHost)host=*runtime->executionHost;set(StatusHeading,wide(host.name+" · "+host.target));std::string summary=runtime?runtime->summary():tr("win.ssh_disconnected_forwarding_ready_0_failed_0").render();if(runtime&&runtime->temporary)summary+=tr("win.nusing_temporary_settings_for_this_session");else if(runtime&&runtime->executionHost&&draft!=settings_.drafts.end()&&!sameExecutionSettings(*runtime->executionHost,draft->second.host))summary+=tr("win.nrunning_settings_differ_from_edited_settings");set(StatusSummary,nativeLines(summary));
        SendMessageW(control(StatusForwards),WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(control(StatusForwards));if(runtime){for(size_t i=0;i<runtime->forwards.size();++i){auto& f=runtime->forwards[i];auto description=wide(f.description);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=description.data();ListView_InsertItem(control(StatusForwards),&item);auto source=wide(f.source),state=forwardTitle(f.state),detail=wide(f.detail);ListView_SetItemText(control(StatusForwards),item.iItem,1,source.data());ListView_SetItemText(control(StatusForwards),item.iItem,2,state.data());ListView_SetItemText(control(StatusForwards),item.iItem,3,detail.data());}}
        SendMessageW(control(StatusForwards),WM_SETREDRAW,TRUE,0);InvalidateRect(control(StatusForwards),nullptr,TRUE);std::string details=tr("win.ready_means_forwarding_was_opened_it_does_not_guarantee_destinati");if(runtime){if(!runtime->lastError.empty())details+=tr("win.nrecent_error")+runtime->lastError;for(auto& e:runtime->recentErrors)details+="\n"+e.time+" · "+e.message;}set(StatusErrors,nativeLines(details));
    }
    void updateTray(){if(test_&&!uiPreview_)return;size_t connected=0,disconnected=0;for(auto& host:manager_->hosts()){auto runtime=manager_->runtime(host.id);if(runtime&&runtime->state==ConnectionState::connected)++connected;else if(host.autoConnect)++disconnected;}tray_={};tray_.cbSize=sizeof(tray_);tray_.hWnd=window_;tray_.uID=1;tray_.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP;tray_.uCallbackMessage=WM_TRAY;tray_.hIcon=icon_;auto tip=wide(tr("win.tooltip",{{"connected",std::to_string(connected)},{"disconnected",std::to_string(disconnected)}}));wcsncpy_s(tray_.szTip,tip.c_str(),_TRUNCATE);if(!trayAdded_){trayAdded_=Shell_NotifyIconW(NIM_ADD,&tray_)!=FALSE;if(trayAdded_){tray_.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&tray_);}}else Shell_NotifyIconW(NIM_MODIFY,&tray_);if(menuOpen_)updateMenu();}
    // The cache lives through menu tracking and is destroyed only after the menu closes.
    std::map<std::pair<MenuIcon,UINT>,HBITMAP> menuBitmaps_;
    UINT menuDpi_=96,menuNext_=100;
    struct HostMenu {HMENU menu=nullptr;UINT summary=0,connect=0,disconnect=0,settings=0;std::vector<std::pair<std::string,UINT>> rows;};
    std::map<std::string,HostMenu> menus_;
    void clearMenuBitmaps(){for(auto& [key,bitmap]:menuBitmaps_)DeleteObject(bitmap);menuBitmaps_.clear();}
    HBITMAP menuBitmap(MenuIcon icon){
        auto key=std::make_pair(icon,menuDpi_);if(auto it=menuBitmaps_.find(key);it!=menuBitmaps_.end())return it->second;
        int n=std::max(16,GetSystemMetricsForDpi(SM_CXMENUCHECK,menuDpi_));BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=n;info.bmiHeader.biHeight=-n;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* pixels=nullptr;HDC screen=GetDC(nullptr);HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);HDC dc=CreateCompatibleDC(screen);ReleaseDC(nullptr,screen);
        if(!bitmap||!dc){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return nullptr;}
        auto oldBitmap=SelectObject(dc,bitmap);std::fill_n(static_cast<DWORD*>(pixels),n*n,0);
        COLORREF color=GetSysColor(COLOR_MENUTEXT);if(color==RGB(0,0,0))color=RGB(32,32,32);
        HIGHCONTRASTW contrast{sizeof(contrast)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0);
        if(!(contrast.dwFlags&HCF_HIGHCONTRASTON)){if(icon==MenuIcon::ready)color=RGB(32,140,70);else if(icon==MenuIcon::failed)color=RGB(195,48,48);else if(icon==MenuIcon::preparing)color=RGB(40,112,190);else if(icon==MenuIcon::disabled||icon==MenuIcon::disconnected)color=RGB(116,116,116);}
        int stroke=std::max(1,MulDiv(1,menuDpi_,96));
        HPEN pen=CreatePen(PS_SOLID,stroke,color);auto oldPen=SelectObject(dc,pen);auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
        int inset=std::max(1,MulDiv(1,menuDpi_,96));
        auto x=[n,inset](int value){return inset+MulDiv(value,n-1-2*inset,16);};
        auto line=[&](int a,int b,int c,int d){MoveToEx(dc,x(a),x(b),nullptr);LineTo(dc,x(c),x(d));};
        auto circle=[&]{Ellipse(dc,x(2),x(2),x(15),x(15));};
        switch(icon){
        case MenuIcon::ready:circle();line(4,8,7,11);line(7,11,12,5);break;
        case MenuIcon::failed:circle();line(5,5,11,11);line(11,5,5,11);break;
        case MenuIcon::disconnected:circle();break;
        case MenuIcon::disabled:circle();line(5,8,11,8);break;
        case MenuIcon::preparing:case MenuIcon::reconnect:Arc(dc,x(2),x(2),x(15),x(15),x(3),x(5),x(13),x(5));line(13,2,13,6);line(13,6,9,6);break;
        case MenuIcon::unknown:{circle();SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);HFONT font=CreateFontW(-(n-2*inset),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");auto oldFont=SelectObject(dc,font);RECT area{inset,inset,n-inset,n-inset};DrawTextW(dc,L"?",1,&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,oldFont);DeleteObject(font);break;}
        case MenuIcon::settings:for(int i=0;i<8;++i){double angle=i*3.141592653589793/4;line(8+static_cast<int>(4*std::cos(angle)),8+static_cast<int>(4*std::sin(angle)),8+static_cast<int>(7*std::cos(angle)),8+static_cast<int>(7*std::sin(angle)));}Ellipse(dc,x(4),x(4),x(13),x(13));Ellipse(dc,x(6),x(6),x(11),x(11));break;
        case MenuIcon::connect:line(2,8,14,8);line(10,4,14,8);line(14,8,10,12);break;
        case MenuIcon::disconnect:Rectangle(dc,x(4),x(4),x(13),x(13));break;
        case MenuIcon::edit:line(3,13,12,4);line(5,15,14,6);line(12,4,14,6);line(3,13,3,15);line(3,15,5,15);break;
        case MenuIcon::about:circle();line(8,7,8,12);line(8,4,8,5);break;
        case MenuIcon::quit:line(8,1,8,8);Arc(dc,x(2),x(2),x(15),x(15),x(5),x(2),x(11),x(2));break;
        }
        SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);GdiFlush();auto* values=static_cast<DWORD*>(pixels);for(int i=0;i<n*n;++i)if(values[i]&0xffffff)values[i]|=0xff000000;
        SelectObject(dc,oldBitmap);DeleteDC(dc);menuBitmaps_[key]=bitmap;return bitmap;
    }
    static std::wstring menuText(const std::string& text){auto value=wide(text);std::wstring escaped;for(auto c:value){if(c==L'&')escaped+=L'&';escaped+=(c==L'\n'||c==L'\r'||c==L'\t')?L' ':c;}return escaped;}
    void menuItem(HMENU menu,UINT id,const std::wstring& text,MenuIcon icon,bool enabled=true){MENUITEMINFOW item{};item.cbSize=sizeof(item);item.fMask=MIIM_STRING|MIIM_STATE|MIIM_BITMAP;item.dwTypeData=const_cast<LPWSTR>(text.c_str());item.fState=enabled?MFS_ENABLED:MFS_DISABLED;item.hbmpItem=menuBitmap(icon);SetMenuItemInfoW(menu,id,FALSE,&item);}
    void syncForwardMenu(HostMenu& menu,const std::vector<MenuForwardRow>& input){
        auto rows=input;if(rows.empty())rows.push_back({"empty",tr("win.no_forwarding_rules"),MenuIcon::disabled});
        // Keep matching IDs and menu handles during status-only updates. Only structural
        // changes from a different execution snapshot insert/remove/reorder rows.
        for(size_t i=0;i<rows.size();++i){
            auto found=std::find_if(menu.rows.begin()+std::min(i,menu.rows.size()),menu.rows.end(),[&](const auto& entry){return entry.first==rows[i].id;});
            if(found==menu.rows.end()){UINT id=menuNext_++;InsertMenuW(menu.menu,static_cast<UINT>(2+i),MF_BYPOSITION|MF_STRING|MF_GRAYED,id,L"");menu.rows.insert(menu.rows.begin()+i,{rows[i].id,id});}
            else if(static_cast<size_t>(found-menu.rows.begin())!=i){auto entry=*found;DeleteMenu(menu.menu,entry.second,MF_BYCOMMAND);menu.rows.erase(found);InsertMenuW(menu.menu,static_cast<UINT>(2+i),MF_BYPOSITION|MF_STRING|MF_GRAYED,entry.second,L"");menu.rows.insert(menu.rows.begin()+i,entry);}
            menuItem(menu.menu,menu.rows[i].second,menuText(rows[i].text),rows[i].icon,false);
        }
        while(menu.rows.size()>rows.size()){DeleteMenu(menu.menu,menu.rows.back().second,MF_BYCOMMAND);menu.rows.pop_back();}
    }
    void updateMenu(){if(!activeMenu_)return;bool busy=false;
        for(auto& host:manager_->hosts()){auto r=manager_->runtime(host.id);bool connected=r&&r->state==ConnectionState::connected,working=r&&(r->state==ConnectionState::connecting||r->state==ConnectionState::stopping);busy=busy||working;auto found=menus_.find(host.id);if(found==menus_.end())continue;auto& menu=found->second;
            auto state=hostMenuIcon(host,r);std::string title=host.name+(working?tr("win.working"):"")+(r&&r->temporary?tr("win.temporary"):"");
            MENUITEMINFOW row{};row.cbSize=sizeof(row);row.fMask=MIIM_STRING|MIIM_BITMAP;auto text=menuText(title);row.dwTypeData=text.data();row.hbmpItem=menuBitmap(state);for(int i=0;i<GetMenuItemCount(activeMenu_);++i)if(GetSubMenu(activeMenu_,i)==menu.menu){SetMenuItemInfoW(activeMenu_,i,TRUE,&row);break;}
            MENUITEMINFOW summary{};summary.cbSize=sizeof(summary);summary.fMask=MIIM_STRING;auto summaryText=menuText(r?r->summary():tr("win.ssh_disconnected_forwarding_ready_0_failed_0").render());summary.dwTypeData=summaryText.data();SetMenuItemInfoW(menu.menu,menu.summary,FALSE,&summary);
            syncForwardMenu(menu,menuForwards(host,r));
            menuItem(menu.menu,menu.connect,working?tw("win.connecting_156"):(connected?tw("win.reconnect"):tw("win.connect")),connected?MenuIcon::reconnect:MenuIcon::connect,!working&&!quitting_);
            if(connected||working){if(GetMenuState(menu.menu,menu.disconnect,MF_BYCOMMAND)==UINT(-1))InsertMenuW(menu.menu,menu.settings,MF_BYCOMMAND|MF_STRING,menu.disconnect,tw("win.disconnect_159"));menuItem(menu.menu,menu.disconnect,tw("win.disconnect_159"),MenuIcon::disconnect,!quitting_);}else DeleteMenu(menu.menu,menu.disconnect,MF_BYCOMMAND);
            menuItem(menu.menu,menu.settings,tw("win.host_settings"),MenuIcon::edit);
        }
        menuItem(activeMenu_,1,tw("win.settings"),MenuIcon::settings);menuItem(activeMenu_,2,tw("win.reconnect_all"),MenuIcon::reconnect,!busy&&!quitting_);menuItem(activeMenu_,5,tw("win.disconnect_all"),MenuIcon::disconnect,canDisconnectAll());menuItem(activeMenu_,6,tw("win.about"),MenuIcon::about,!quitting_);menuItem(activeMenu_,3,tw("win.quit"),MenuIcon::quit);
    }
    void createTrayMenu(){activeMenu_=CreatePopupMenu();menus_.clear();menuActions_.clear();menuNext_=100;menuDpi_=uiDpi(settingsWindow_?settingsWindow_:window_);
        AppendMenuW(activeMenu_,MF_STRING,1,tw("win.settings"));HMENU languageMenu=CreatePopupMenu();
        AppendMenuW(languageMenu,MF_STRING|(languageSelection()=="auto"?MF_CHECKED:0),20,tw("win.language_automatic"));AppendMenuW(languageMenu,MF_SEPARATOR,0,nullptr);
        for(size_t i=0;i<languages().size();++i){const auto& language=languages()[i];AppendMenuW(languageMenu,MF_STRING|(languageSelection()==language.id?MF_CHECKED:0),21+i,wide(language.title).c_str());}
        AppendMenuW(activeMenu_,MF_POPUP,reinterpret_cast<UINT_PTR>(languageMenu),tw("win.language"));appendLoginStartupMenu(activeMenu_,4,loginStartup_);AppendMenuW(activeMenu_,MF_STRING,2,tw("win.reconnect_all"));AppendMenuW(activeMenu_,MF_STRING,5,tw("win.disconnect_all"));AppendMenuW(activeMenu_,MF_SEPARATOR,0,nullptr);
        for(auto& host:manager_->hosts()){HostMenu menu;menu.menu=CreatePopupMenu();menu.summary=menuNext_++;menu.connect=menuNext_++;menu.disconnect=menuNext_++;menu.settings=menuNext_++;
            AppendMenuW(menu.menu,MF_STRING|MF_GRAYED,menu.summary,tw("win.ssh_status"));AppendMenuW(menu.menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu.menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu.menu,MF_STRING,menu.connect,tw("win.connect"));AppendMenuW(menu.menu,MF_STRING,menu.disconnect,tw("win.disconnect_159"));AppendMenuW(menu.menu,MF_STRING,menu.settings,tw("win.host_settings"));menuActions_[menu.connect]={host.id,1};menuActions_[menu.disconnect]={host.id,2};menuActions_[menu.settings]={host.id,3};AppendMenuW(activeMenu_,MF_POPUP,reinterpret_cast<UINT_PTR>(menu.menu),menuText(host.name).c_str());menus_[host.id]=std::move(menu);
        }
        AppendMenuW(activeMenu_,MF_SEPARATOR,0,nullptr);AppendMenuW(activeMenu_,MF_STRING,6,tw("win.about"));AppendMenuW(activeMenu_,MF_STRING,3,tw("win.quit"));updateMenu();
    }
    void refreshLanguage(){
        if(settingsWindow_){
            setText(settingsWindow_,tw("win.localforwarder_settings"));
            for(const auto& [id,key]:controlTranslations_)if(control(id))set(id,tw(key));
            const char* keys[]={"win.enabled","win.local_port","win.destination_host","win.destination_port","win.description"};
            for(int i=0;i<5;++i){LVCOLUMNW column{};column.mask=LVCF_TEXT;column.pszText=const_cast<wchar_t*>(tw(keys[i]));ListView_SetColumn(control(Rules),i,&column);}
            auto listTop=SendMessageW(control(Hosts),LB_GETTOPINDEX,0,0);
            populateHosts();SendMessageW(control(Hosts),LB_SETTOPINDEX,listTop,0);
            layoutSettings();preview();
        }
        updateTray();
    }
    void changeLanguage(const std::string& requested,const SettingsFocus* trayFocus=nullptr){
        if(quitting_||foregroundModal())return;
        capture();auto focus=trayFocus?*trayFocus:languageFocusSnapshot();
        try{
            selectLanguage(requested,systemUILanguages(),test_?std::function<void(const std::string&)>{}:persistLanguage);
            refreshLanguage();
        }catch(...){restoreSettingsFocus(focus);throw;}
        restoreSettingsFocus(focus);
    }
    void destroyTrayMenu(){if(activeMenu_)DestroyMenu(activeMenu_);activeMenu_=nullptr;menus_.clear();clearMenuBitmaps();}
    void trayMenu(){if(quitting_||menuOpen_||foregroundModal())return;auto languageFocus=languageFocusSnapshot();createTrayMenu();POINT point{};GetCursorPos(&point);SetForegroundWindow(window_);menuOpen_=true;UINT selected=test_&&traySelectionTest_?traySelectionTest_(activeMenu_,window_):TrackPopupMenuEx(activeMenu_,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,window_,nullptr);menuOpen_=false;destroyTrayMenu();PostMessageW(window_,WM_NULL,0,0);
        try{if(selected>=20&&selected<21+languages().size()){changeLanguage(selected==20?"auto":languages().at(selected-21).id,&languageFocus);}else if(selected==1)openSettings();else if(selected==4){auto message=loginStartup_.toggle();if(!message.empty()){if(test_)error(message);else MessageBoxW(window_,wide(message).c_str(),tw("win.launch_at_login"),MB_OK|MB_ICONWARNING);}}else if(selected==2)manager_->reconnectAll();else if(selected==5)confirmDisconnectAll();else if(selected==6)showAbout();else if(selected==3)quit();else if(menuActions_.contains(selected)){auto [id,operation]=menuActions_.at(selected);if(operation==1)manager_->connect(id);else if(operation==2)manager_->disconnect(id);else openSettings(id);}}catch(const std::exception& e){error(e.what());}menuActions_.clear();
    }
    void refresh(){if(!manager_)return;if(test_&&!uiTest_){snapshot();return;}updateTray();if(test_&&!uiPreview_&&menuOpen_)updateMenu();if(settings_.open){populateHosts();refreshButtons();refreshStatus();}}
    void quit(){if(quitting_)return;quitting_=true;if(appModalWindow_)SendMessageW(appModalWindow_,TDM_CLICK_BUTTON,IDCANCEL,0);network_.stop();closeRule(false);closeStatus();for(auto& [id,cancel]:authObservers_)if(cancel)cancel();authObservers_.clear();manager_->quit();}
    void finishQuit(){if(trayAdded_){Shell_NotifyIconW(NIM_DELETE,&tray_);trayAdded_=false;}loop_->detach();if(settingsWindow_)ShowWindow(settingsWindow_,SW_HIDE);PostQuitMessage(0);}
    void snapshot(const std::string& request={}){
        if(!test_||!manager_)return;for(auto& host:manager_->hosts()){auto r=manager_->runtime(host.id);if(!r)continue;
            yyjson_mut_doc* doc=yyjson_mut_doc_new(nullptr);auto* object=yyjson_mut_obj(doc);yyjson_mut_doc_set_root(doc,object);auto str=[&](yyjson_mut_val* obj,const char* key,const std::string& value){yyjson_mut_obj_add_strcpy(doc,obj,key,value.c_str());};str(object,"id",host.id);str(object,"state",stateName(r->state));str(object,"error",r->lastError);yyjson_mut_obj_add_bool(doc,object,"intent",r->desiredConnected);yyjson_mut_obj_add_bool(doc,object,"temporary",r->temporary);if(!request.empty())str(object,"requestID",request);
            auto* forwards=yyjson_mut_arr(doc);yyjson_mut_obj_add_val(doc,object,"forwards",forwards);for(auto& f:r->forwards){auto* row=yyjson_mut_arr_add_obj(doc,forwards);str(row,"id",f.id);str(row,"state",forwardStateName(f.state));str(row,"source",f.source);str(row,"description",f.description);str(row,"detail",f.detail);}auto* errors=yyjson_mut_arr(doc);yyjson_mut_obj_add_val(doc,object,"recentErrors",errors);for(auto& e:r->recentErrors){auto* row=yyjson_mut_arr_add_obj(doc,errors);str(row,"time",e.time);str(row,"message",e.message);}char* json=yyjson_mut_write(doc,0,nullptr);if(json){outputLine("LF_HOST "+std::string(json));free(json);}yyjson_mut_doc_free(doc);
        }
    }
    void input(const std::string& line){if(quitting_)return;try{
        std::unique_ptr<yyjson_doc,decltype(&yyjson_doc_free)> doc(yyjson_read(line.data(),line.size(),0),yyjson_doc_free);if(!doc)throw LocalizedError(tr("win.invalid_command_json"));auto* root=yyjson_doc_get_root(doc.get());if(!yyjson_is_obj(root))throw LocalizedError(tr("win.commands_must_be_json_objects"));auto text=[&](const char* key,bool required=true){auto* value=yyjson_obj_get(root,key);if(!yyjson_is_str(value)){if(!required)return std::string{};throw LocalizedError(tr("win.required_string_field")+key);}return std::string(yyjson_get_str(value),yyjson_get_len(value));};auto command=text("command");
        if(command=="quit")quit();else if(command=="connect")manager_->connect(text("id"));else if(command=="disconnect")manager_->disconnect(text("id"));else if(command=="delete")manager_->remove(text("id"));else if(command=="saved")manager_->saved(loadHost(directory_,text("id")));else if(command=="reconnect-all")manager_->reconnectAll();else if(command=="disconnect-all")manager_->disconnectAll();else if(command=="wake")manager_->wake();else if(command=="sleep")manager_->sleep();else if(command=="network")manager_->networkChanged(yyjson_is_true(yyjson_obj_get(root,"available")));else if(command=="snapshot")snapshot(text("requestID",false));else if(command=="temporary"){auto* host=yyjson_obj_get(root,"host");if(!yyjson_is_obj(host))throw LocalizedError(tr("win.host_settings_are_required"));char* value=yyjson_val_write(host,0,nullptr);if(!value)throw LocalizedError(tr("win.could_not_read_host_settings"));std::string encoded=value;free(value);auto config=parseConfig("{\"version\":2,\"hosts\":["+encoded+"]}");manager_->connect(config.hosts.at(0),true);}else throw LocalizedError(tr("win.unknown_command")+command);
        }catch(const std::exception& e){error(e.what());}}
    void startInput(){auto post=loop_->poster();std::weak_ptr<int> alive=lifetime_;std::thread([this,alive,post]{HANDLE handle=GetStdHandle(STD_INPUT_HANDLE);std::string line;char buffer[4096];DWORD count=0;while(ReadFile(handle,buffer,sizeof(buffer),&count,nullptr)&&count){for(DWORD i=0;i<count;++i){if(buffer[i]=='\n'){auto copy=line;line.clear();post([this,alive,copy]{if(!alive.expired())input(copy);});}else if(buffer[i]!='\r'){if(line.size()>=4*1024*1024){post([this,alive]{if(!alive.expired())quit();});return;}line+=buffer[i];}}}post([this,alive]{if(!alive.expired())quit();});}).detach();}
    void command(int id,int notification){if(loading_||quitting_||appModal_)return;try{
        if(id==Hosts&&notification==LBN_SELCHANGE){capture();auto row=SendMessageW(control(Hosts),LB_GETCURSEL,0,0);if(row>=0&&static_cast<size_t>(row)<settings_.order.size()&&settings_.select(settings_.order[row]))fill();}
        else if(id==NewHost){capture();addHost();}
        else if(id==DeleteHost){capture();if(choose(tw("win.delete_this_host"),tw("win.remove_this_host_s_connection_and_saved_settings"),{{1001,tw("win.delete_host_173")},{IDCANCEL,tw("win.cancel")}},IDCANCEL)==1001){auto selected=settings_.selected;manager_->remove(selected);settings_.remove(selected);if(settings_.order.empty())addHost();else fill();}}
        else if(id==RuleNew)showRule(true);
        else if(id==RuleEdit)showRule(false);
        else if(id==RuleSort)sortRulesByLocalPort();
        else if(id==RuleDelete){capture();auto& draft=settings_.drafts.at(settings_.selected);if(rule_>=0&&static_cast<size_t>(rule_)<draft.host.forwards.size()){draft.rulePorts.erase(draft.host.forwards[rule_].id);draft.host.forwards.erase(draft.host.forwards.begin()+rule_);rule_=-1;fillRules();fillRuleEditor();populateHosts();preview();}}
        else if(id==Save)saveSelected();else if(id==Connect)connectSelected();else if(id==Authentication)authenticate();else if(id==ConnectionStatus)showStatus();else if(id==ChooseKey)chooseKey();else if(id==Okay||id==IDOK)closeSettings(true);else if(id==CancelButton||id==IDCANCEL)closeSettings(false);
        else if((notification==EN_CHANGE&&(id==Name||id==Description||id==Target||id==User||id==Port||id==Key))||id==AutoConnect||id==ExitFailure||id==RetryRefused){capture();populateHosts();preview();}
        }catch(const std::exception& e){error(e.what());}}
    void ruleChanged(NMLISTVIEW* event){if(loading_||event->iItem<0||!settings_.drafts.contains(settings_.selected))return;auto& draft=settings_.drafts.at(settings_.selected);if(static_cast<size_t>(event->iItem)>=draft.host.forwards.size())return;draft.host.forwards[event->iItem].enabled=ListView_GetCheckState(control(Rules),event->iItem)!=FALSE;if((event->uNewState&LVIS_SELECTED)&&!(event->uOldState&LVIS_SELECTED)){capture();rule_=event->iItem;fillRuleEditor();}populateHosts();preview();}
public:
    void nativeLayoutChecks(){
        auto check=[](bool value,const char* why){if(!value)throw LocalizedError(why);};
        auto bounds=[](HWND child,HWND parent){RECT rect{};GetWindowRect(child,&rect);MapWindowPoints(HWND_DESKTOP,parent,reinterpret_cast<POINT*>(&rect),2);return rect;};
        auto inside=[](const RECT& inner,const RECT& outer){return inner.left>=outer.left&&inner.top>=outer.top&&inner.right<=outer.right&&inner.bottom<=outer.bottom&&inner.right>inner.left&&inner.bottom>inner.top;};
        auto disjoint=[&](const std::vector<int>& ids,HWND parent){
            for(size_t i=0;i<ids.size();++i){check(control(ids[i])!=nullptr,"layout test control exists");auto a=bounds(control(ids[i]),parent);
                for(size_t j=i+1;j<ids.size();++j){auto b=bounds(control(ids[j]),parent);RECT overlap{};if(IntersectRect(&overlap,&a,&b))throw LocalizedError("native controls overlap: "+std::to_string(ids[i])+" / "+std::to_string(ids[j])+" at simulated DPI "+std::to_string(previewDpi));}
            }
        };
        auto verifyLabel=[&](int labelID,int editID,HWND parent){
            auto label=bounds(control(labelID),parent),edit=bounds(control(editID),parent);
            check((GetWindowLongPtrW(control(labelID),GWL_STYLE)&SS_CENTERIMAGE)!=0,"field label vertically centers text");
            if(label.right<=edit.left)check(label.top==edit.top&&label.bottom==edit.bottom,"label occupies the full input row");
            else check(label.bottom<edit.top,"stacked label has space before its input");
            HDC dc=GetDC(control(labelID));auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control(labelID),WM_GETFONT,0,0)));
            RECT text{};auto value=textOf(control(labelID));DrawTextW(dc,value.c_str(),-1,&text,DT_CALCRECT|DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,old);ReleaseDC(control(labelID),dc);
            check(text.right<=label.right-label.left&&text.bottom<=label.bottom-label.top,"label glyphs fit without clipping");
        };
        auto verifySettings=[&]{
            RECT client{},formClient{};GetClientRect(settingsWindow_,&client);GetClientRect(form_,&formClient);
            auto okay=bounds(control(Okay),settingsWindow_),cancel=bounds(control(CancelButton),settingsWindow_),form=bounds(form_,settingsWindow_);
            check(inside(okay,client)&&inside(cancel,client),"settings footer remains visible");
            check(okay.right<=cancel.left,"settings OK precedes Cancel");
            check(form.bottom<=std::min(okay.top,cancel.top),"scrolling form cannot cover footer");
            check(inside(bounds(control(Hosts),settingsWindow_),client),"host list remains inside settings");
            disjoint({NewHost,DeleteHost,Hosts,CancelButton,Okay},settingsWindow_);
            std::vector<int> ids={Name,Description,Target,User,Port,Key,ChooseKey,AutoConnect,ExitFailure,RetryRefused,Rules,RuleNew,RuleEdit,RuleDelete,RuleSort,CommandPreview,Save,Connect,Authentication,ConnectionStatus,300,301,302,303,304,305,309,310};
            disjoint(ids,form_);
            for(auto [labelID,editID]:{std::pair{300,Name},{310,Description},{301,Target},{302,User},{303,Port},{304,Key}})verifyLabel(labelID,editID,form_);
            for(int id:{ChooseKey,RuleNew,RuleEdit,RuleDelete,RuleSort,Save,Connect,Authentication,ConnectionStatus}){
                auto button=bounds(control(id),form_);check(button.bottom-button.top==okay.bottom-okay.top,"all ordinary buttons share the footer height");
            }
            for(int id:ids){auto rect=bounds(control(id),form_);check(rect.left>=formClient.left&&rect.right<=formClient.right,"form controls fit horizontally");}
            auto sortButton=bounds(control(RuleSort),form_);check(sortButton.right-sortButton.left>=scale(textWidth(RuleSort)+20),"localized sort button text fits at every size and DPI");
            SetFocus(control(ConnectionStatus));ensureFocusVisible();check(inside(bounds(control(ConnectionStatus),form_),formClient),"keyboard focus scrolls final action into view");
            SetFocus(control(Name));ensureFocusVisible();check(inside(bounds(control(Name),form_),formClient),"keyboard focus scrolls first input into view");
            check(Header_GetItemCount(ListView_GetHeader(control(Rules)))==5,"rule list has final description column");
            check(ListView_GetColumnWidth(control(Rules),1)>=scale(70)&&ListView_GetColumnWidth(control(Rules),3)>=scale(70)&&ListView_GetColumnWidth(control(Rules),4)>=scale(160),"rule list preserves port and description access");
            SCROLLINFO horizontal{sizeof(horizontal),SIF_ALL};GetScrollInfo(control(Rules),SB_HORZ,&horizontal);
            if(horizontal.nMax>=static_cast<int>(horizontal.nPage)){ListView_Scroll(control(Rules),10000,0);horizontal.fMask=SIF_POS;GetScrollInfo(control(Rules),SB_HORZ,&horizontal);check(horizontal.nPos>0,"narrow rule list can scroll to description");ListView_Scroll(control(Rules),-10000,0);}
        };
        auto resizeClient=[](HWND window,int width,int height,UINT dpi){
            RECT outer{0,0,MulDiv(width,dpi,96),MulDiv(height,dpi,96)};
            AdjustWindowRectExForDpi(&outer,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),GetDpiForWindow(window));
            SetWindowPos(window,nullptr,0,0,outer.right-outer.left,outer.bottom-outer.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        };
        UINT previousDpi=previewDpi;RECT original{};GetWindowRect(settingsWindow_,&original);
        for(UINT dpi:{96u,120u,144u,192u,240u}){
            previewDpi=dpi;updateFont();
            for(const auto& logicalSize:std::vector<std::pair<int,int>>{{900,650},{800,430},{740,430},{739,430},{820,430},{850,430}}){
                outputLine("layout probe DPI "+std::to_string(dpi)+" size "+std::to_string(logicalSize.first)+"x"+std::to_string(logicalSize.second));
                resizeClient(settingsWindow_,logicalSize.first,logicalSize.second,dpi);
                scroll_=0;layoutSettings();verifySettings();
            }
            // Exercise the actual work-area clamping path, without changing monitor DPI.
            centerInWorkArea(settingsWindow_,900,650);layoutSettings();
            RECT clampedClient{},clampedViewport{};GetClientRect(settingsWindow_,&clampedClient);GetClientRect(form_,&clampedViewport);
            check(inside(bounds(control(Okay),settingsWindow_),clampedClient)&&inside(bounds(control(CancelButton),settingsWindow_),clampedClient),"work-area clamp preserves fixed footer");
            // The clamped layout must preserve keyboard access at every scale.
            SetFocus(control(ConnectionStatus));ensureFocusVisible();auto lastAction=bounds(control(ConnectionStatus),form_);
            check(lastAction.top>=clampedViewport.top&&lastAction.bottom<=clampedViewport.bottom,"work-area height clamp preserves vertical action access");
            MONITORINFO monitor{sizeof(monitor)};check(GetMonitorInfoW(MonitorFromWindow(settingsWindow_,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE,"layout monitor information");
            RECT actual{};GetWindowRect(settingsWindow_,&actual);check(inside(actual,monitor.rcWork),"settings initial size fits available work area");
            verifySettings();
            previewWorkArea=RECT{0,0,MulDiv(520,dpi,96),MulDiv(360,dpi,96)};
            centerInWorkArea(settingsWindow_,900,650);layoutSettings();verifySettings();
            GetWindowRect(settingsWindow_,&actual);check(inside(actual,*previewWorkArea),"settings fits synthetic narrow work area");
            auto before=settings_.drafts.at(settings_.selected);
            showRule(true);
            GetWindowRect(ruleWindow_,&actual);check(inside(actual,*previewWorkArea),"rule modal initial size fits narrow work area");
            RECT ruleClient{};GetClientRect(ruleWindow_,&ruleClient);
            check(bounds(control(RuleOkay),ruleWindow_).right<=bounds(control(RuleCancel),ruleWindow_).left,"rule modal OK precedes Cancel");
            std::vector<int> ruleControls={RuleEnabled,RuleDescription,LocalPort,RemoteHost,RemotePort,RuleOkay,RuleCancel,320,321,322,323};
            disjoint(ruleControls,ruleWindow_);for(int id:ruleControls)check(inside(bounds(control(id),ruleWindow_),ruleClient),"clamped rule modal controls remain accessible");
            for(const auto& logicalSize:std::vector<std::pair<int,int>>{{460,250},{400,220}}){
                resizeClient(ruleWindow_,logicalSize.first,logicalSize.second,dpi);layoutRule();
                RECT client{};GetClientRect(ruleWindow_,&client);std::vector<int> ids={RuleEnabled,RuleDescription,LocalPort,RemoteHost,RemotePort,RuleOkay,RuleCancel,320,321,322,323};
                disjoint(ids,ruleWindow_);for(int id:ids)check(inside(bounds(control(id),ruleWindow_),client),"rule modal controls fit within client area");
            }
            for(auto [labelID,editID]:{std::pair{323,RuleDescription},{320,LocalPort},{321,RemoteHost},{322,RemotePort}})verifyLabel(labelID,editID,ruleWindow_);
            closeRule(false);showStatus();
            GetWindowRect(status_,&actual);check(inside(actual,*previewWorkArea),"status modal fits narrow work area");
            RECT statusClient{};GetClientRect(status_,&statusClient);
            std::vector<int> statusControls={StatusHeading,StatusSummary,StatusForwards,StatusErrors,StatusClose};
            disjoint(statusControls,status_);for(int id:statusControls)check(inside(bounds(control(id),status_),statusClient),"clamped status modal controls remain accessible");
            closeStatus();previewWorkArea.reset();check(settings_.drafts.at(settings_.selected)==before,"layout probes leave draft unchanged");
            outputLine("native layout checks passed at simulated DPI "+std::to_string(dpi));
        }
        previewDpi=previousDpi;updateFont();scroll_=0;SetWindowPos(settingsWindow_,nullptr,original.left,original.top,original.right-original.left,original.bottom-original.top,SWP_NOZORDER|SWP_NOACTIVATE);layoutSettings();
    }
    void nativeOwnerModalChecks(HWND owner){
        auto check=[](bool value,const char* why){if(!value)throw LocalizedError(why);};
        struct Probe { bool active=true,scheduled=false,ran=false,trayOpened=false; int nested=0; std::exception_ptr error; };
        auto probe=std::make_shared<Probe>();
        auto selected=settings_.selected;auto drafts=settings_.drafts;
        auto title=wide("LocalForwarder modal owner test "+newId());
        const std::string ownerName=owner==window_?"hidden app":"settings";
        dialogTest_=[probe](HWND dialog,const TASKDIALOGCONFIG&){++probe->nested;PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);};
        // Observe the actual MessageBox activation, then run assertions through
        // its normal message loop. No FindWindow race or guessed HWND is needed.
        OwnerDialogProbe activation;
        activation.activate=[this,probe,owner,title,selected,check,ownerName](HWND prompt){
            if(probe->scheduled||GetWindow(prompt,GW_OWNER)!=owner||textOf(prompt)!=title)return;
            probe->scheduled=true;outputLine("native owner probe activated: "+ownerName);
            loop_->post([this,probe,owner,prompt,selected,check,ownerName]{
                if(!probe->active)return;
                probe->ran=true;outputLine("native owner probe callback: "+ownerName);
                try{
                    check(!IsWindowEnabled(owner)&&!appModal_,"native prompt disables owner without app modal registration");
                    check(foregroundModal(),"disabled native owner blocks new app dialogs");
                    showAbout();confirmDisconnectAll();openSettings(selected=="a"?"b":"a");
                    SendMessageW(window_,WM_OPEN_SETTINGS,0,0);
                    loop_->post([this,probe]{if(probe->active&&menuOpen_){probe->trayOpened=true;EndMenu();}});
                    SendMessageW(window_,WM_TRAY,0,MAKELPARAM(WM_CONTEXTMENU,0));
                    check(!probe->nested&&!probe->trayOpened&&settings_.selected==selected,"native warning blocks About disconnect tray and followup selection changes");
                    check(!IsWindowEnabled(owner)&&IsWindow(prompt),"native warning remains the active modal");
                    outputLine("native owner probe actions passed: "+ownerName);
                }catch(...){probe->error=std::current_exception();}
                // Close the real warning through its window-close path. Native
                // MessageBox implementations need not expose an IDOK child HWND.
                SendMessageW(prompt,WM_CLOSE,0,0);
                outputLine("native owner probe dismissed: "+ownerName);
            });
        };
        ownerDialogProbe_=&activation;
        HHOOK hook=SetWindowsHookExW(WH_CBT,ownerDialogProbeHook,nullptr,GetCurrentThreadId());
        if(!hook){ownerDialogProbe_=nullptr;dialogTest_={};throw LocalizedError("cannot install native modal test hook");}
        outputLine("native owner probe MessageBox start: "+ownerName);
        int result=MessageBoxW(owner,L"모달 소유 관계 회귀 검사",title.c_str(),MB_OK|MB_ICONWARNING);
        outputLine("native owner probe MessageBox returned: "+ownerName+" result="+std::to_string(result));
        UnhookWindowsHookEx(hook);ownerDialogProbe_=nullptr;probe->active=false;dialogTest_={};
        if(activation.error)std::rethrow_exception(activation.error);
        if(probe->error)std::rethrow_exception(probe->error);
        check(probe->ran&&result==IDOK&&IsWindowEnabled(owner),"native warning closes and restores its owner");
        check(settings_.selected==selected&&settings_.drafts==drafts,"native warning probe preserves selection and drafts");
    }
    void nativeTrayLanguageFocusChecks(){
        auto check=[](bool value,const char* why){if(!value)throw LocalizedError(why);};
        auto originalLanguage=languageSelection();auto originalPort=get(Port);int originalScroll=scroll_;
        set(Port,L"invalid");capture();auto drafts=settings_.drafts;auto selected=settings_.selected;
        auto saved=loadConfig(directory_);auto execution=manager_->runtime("a")->executionHost;
        SetForegroundWindow(settingsWindow_);SetFocus(control(Name));SendMessageW(control(Name),EM_SETSEL,2,5);
        scrollForm(SB_LINEDOWN);int scrollBefore=scroll_;bool popupCalled=false,focusMoved=false;
        traySelectionTest_=[&](HMENU menu,HWND owner)->UINT{
            popupCalled=menu&&owner==window_&&GetSubMenu(menu,1);
            // The real tray path has already foregrounded its owner. Force loss of
            // thread focus, as Shell/menu activation can do, before dispatching.
            SetFocus(nullptr);focusMoved=GetFocus()!=control(Name);return 24; // German
        };
        // Simulate deactivation before WM_TRAY arrives, not merely before selection.
        SetFocus(nullptr);SendMessageW(window_,WM_TRAY,0,MAKELPARAM(WM_CONTEXTMENU,0));
        DWORD begin=0,end=0;SendMessageW(control(Name),EM_GETSEL,reinterpret_cast<WPARAM>(&begin),reinterpret_cast<LPARAM>(&end));
        check(popupCalled&&focusMoved&&currentLanguage()=="de","WM_TRAY selects a language after owner focus changes");
        check(GetFocus()==control(Name)&&begin==2&&end==5,"WM_TRAY restores cached settings edit focus and caret");
        check(scroll_==scrollBefore&&settings_.drafts==drafts&&settings_.selected==selected&&get(Port)==L"invalid","WM_TRAY preserves scroll selected host and invalid input");
        check(loadConfig(directory_)==saved&&manager_->runtime("a")->executionHost==execution&&manager_->runtime("a")->state==ConnectionState::connected&&manager_->runtime("a")->desiredConnected,"WM_TRAY preserves saved config and active connection");
        showStatus();popupCalled=false;SendMessageW(window_,WM_TRAY,0,MAKELPARAM(WM_CONTEXTMENU,0));
        check(!popupCalled&&currentLanguage()=="de"&&status_,"modal blocks the actual tray language path");closeStatus();
        traySelectionTest_={};set(Port,originalPort);capture();changeLanguage(originalLanguage);
        scroll_=originalScroll;layoutForm();
    }
    void nativeAppDialogChecks(){
        outputLine("native app dialog checks started");
        auto check=[](bool value,const char* why){if(!value)throw LocalizedError(why);};
        openSettings();set(Name,L"About 모달 전 미저장 이름");capture();
        nativeTrayLanguageFocusChecks();
        auto drafts=settings_.drafts;auto selected=settings_.selected;auto saved=loadConfig(directory_);
        auto execution=manager_->runtime("a")->executionHost;
        auto originalLanguage=languageSelection();
        for(const auto& language:languages()){
            changeLanguage(language.id);
            check(settings_.drafts==drafts&&settings_.selected==selected&&loadConfig(directory_)==saved,"language switch preserves drafts selected host and file");
            check(manager_->runtime("a")->executionHost==execution&&manager_->runtime("a")->state==ConnectionState::connected&&manager_->runtime("a")->desiredConnected,"language switch preserves active connection and intent");
        }
        changeLanguage(originalLanguage);
        nativeOwnerModalChecks(settingsWindow_);
        nativeOwnerModalChecks(window_); // Login-startup warnings use this owner even while settings is open.
        auto originalOpenURL=std::move(openURL_);
        std::vector<std::wstring> openedURLs;
        openURL_=[&](HWND,const wchar_t* url){openedURLs.emplace_back(url);return true;};
        int shown=0;
        for(int dismissal=0;dismissal<3;++dismissal){
            openedURLs.clear();
            outputLine("native About dismissal probe "+std::to_string(dismissal));
            bool backgroundDelivered=false;
            dialogTest_=[&,dismissal](HWND dialog,const TASKDIALOGCONFIG& config){
                ++shown;
                check(appModalWindow_==dialog&&!IsWindowEnabled(settingsWindow_),"About disables its settings owner");auto previousLanguage=languageSelection();changeLanguage(previousLanguage=="de"?"ja":"de");check(languageSelection()==previousLanguage,"modal blocks language changes");
                check(config.hwndParent==settingsWindow_&&config.hMainIcon&&std::wstring(config.pszMainInstruction)==L"LocalForwarder","About has program icon and name");
                auto content=std::wstring(config.pszContent);
                auto versionPosition=content.find(wide(tr("win.version_value",{{"version",productVersion}})));
                auto licensePosition=content.find(std::wstring(L"<a href=\"")+LicenseURL+L"\">MIT License</a>");
                auto projectPosition=content.find(std::wstring(L"<a href=\"")+ProjectURL+L"\">"+ProjectURL+L"</a>");
                check(versionPosition!=std::wstring::npos&&licensePosition!=std::wstring::npos&&projectPosition!=std::wstring::npos&&versionPosition<licensePosition&&licensePosition<projectPosition,"About has ordered version license and project links");
                check(content.find(L"개발자 GuruJung")==std::wstring::npos,"About omits the developer row");
                check((config.dwFlags&TDF_ENABLE_HYPERLINKS)!=0,"About URLs are clickable");
                check(openedURLs.empty(),"About opening must not open browser");
                for(auto url:{LicenseURL,ProjectURL,L"https://example.com",L"https://github.com/GuruJung/LocalForwarder/blob/main/LICENSE?other",L"file:///LICENSE"})
                    config.pfCallback(dialog,TDN_HYPERLINK_CLICKED,0,reinterpret_cast<LPARAM>(url),config.lpCallbackData);
                config.pfCallback(dialog,TDN_HYPERLINK_CLICKED,0,0,config.lpCallbackData);
                check(openedURLs==std::vector<std::wstring>{LicenseURL,ProjectURL},"About opens each allowed link once and rejects other URLs");
                check(appModalWindow_==dialog&&appModal_,"About remains open after link clicks");
                showAbout();confirmDisconnectAll();openSettings("b");trayMenu();
                check(appModalWindow_==dialog&&settings_.selected==selected&&!activeMenu_,"reentrant actions retain the current modal and selection");
                loop_->post([&,dialog,dismissal]{
                    backgroundDelivered=true;refresh();
                    if(dismissal==0)PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);
                    else if(dismissal==1)PostMessageW(dialog,WM_KEYDOWN,VK_ESCAPE,0);
                    else PostMessageW(dialog,WM_CLOSE,0,0);
                });
            };
            showAbout();dialogTest_={};
            check(backgroundDelivered&&!appModal_&&!appModalWindow_&&IsWindowEnabled(settingsWindow_),"About closes and keeps background event processing active");
            check(settings_.drafts==drafts&&settings_.selected==selected&&manager_->runtime("a")->executionHost==execution&&manager_->runtime("a")->state==ConnectionState::connected,"About preserves drafts and existing connection");
        }
        openURL_=std::move(originalOpenURL);
        check(shown==3,"About duplicate invocation creates no extra dialogs");
        showStatus();showAbout();check(shown==3&&status_&&!IsWindowEnabled(settingsWindow_),"About does not nest over connection-status modal");closeStatus();
        showRule(true);showAbout();check(shown==3&&ruleWindow_,"About does not nest over rule modal");closeRule(false);
        createTrayMenu();check(GetMenuItemID(activeMenu_,3)==2&&GetMenuItemID(activeMenu_,4)==5,"disconnect-all immediately follows reconnect-all");
        check(GetMenuItemID(activeMenu_,GetMenuItemCount(activeMenu_)-2)==6&&GetMenuItemID(activeMenu_,GetMenuItemCount(activeMenu_)-1)==3,"About immediately precedes quit");
        check(!(GetMenuState(activeMenu_,5,MF_BYCOMMAND)&MF_GRAYED),"disconnect-all enabled for an active connection");destroyTrayMenu();
        for(int dismissal=0;dismissal<4;++dismissal){
            outputLine("native disconnect dismissal probe "+std::to_string(dismissal));
            dialogTest_=[&,dismissal](HWND dialog,const TASKDIALOGCONFIG& config){
                check(config.nDefaultButton==IDCANCEL&&config.cButtons==2&&!IsWindowEnabled(settingsWindow_),"disconnect confirmation defaults to cancel and is modal");
                check(std::wstring(config.pszMainInstruction)==tw("win.disconnect_all_hosts"),"disconnect confirmation instruction");
                confirmDisconnectAll();
                if(dismissal==0)PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);
                else if(dismissal==1)PostMessageW(dialog,WM_KEYDOWN,VK_ESCAPE,0);
                else if(dismissal==2)PostMessageW(dialog,WM_CLOSE,0,0);
                else PostMessageW(dialog,WM_KEYDOWN,VK_RETURN,0);
            };
            confirmDisconnectAll();dialogTest_={};check(manager_->runtime("a")->state==ConnectionState::connected&&manager_->runtime("a")->desiredConnected,"cancel Escape and close preserve connection intent");
        }
        dialogTest_=[&](HWND dialog,const TASKDIALOGCONFIG&){
            // The operation uses hosts at confirmation time, including manual OFF hosts.
            manager_->connect("b");PostMessageW(dialog,TDM_CLICK_BUTTON,1001,0);
        };
        confirmDisconnectAll();dialogTest_={};
        for(const auto& host:manager_->hosts())check(manager_->runtime(host.id)->state==ConnectionState::disconnected&&!manager_->runtime(host.id)->desiredConnected,"confirmed disconnect stops every current host");
        check(settings_.drafts==drafts&&loadConfig(directory_)==saved,"confirmed disconnect preserves unsaved drafts and saved options");
        createTrayMenu();check(GetMenuState(activeMenu_,5,MF_BYCOMMAND)&MF_GRAYED,"disconnect-all disabled without work or intent");destroyTrayMenu();
        manager_->reconnectAll();check(manager_->runtime("a")->state==ConnectionState::connected&&!manager_->runtime("b")->desiredConnected,"full reconnect still uses saved auto-connect selection");
        closeSettings(false);
        dialogTest_=[&](HWND dialog,const TASKDIALOGCONFIG& config){check(config.hwndParent==window_&&!IsWindowEnabled(window_),"tray-only About owns the hidden application window");PostMessageW(dialog,TDM_CLICK_BUTTON,IDCANCEL,0);};
        showAbout();dialogTest_={};check(!settings_.open&&manager_->runtime("a")->state==ConnectionState::connected,"tray-only About does not open settings or stop SSH");
    }
    void nativeSortChecks(){
        auto check=[](bool value,const char* why){if(!value)throw LocalizedError(why);};
        Host fixture=loadHost(directory_,"a");
        fixture.forwards={{"high",true,10000,"high.example",90,"high description"},{"tie-off",false,1000,"off.example",91,"disabled tie"},{"low",true,80,"low.example",92,"low description"},{"tie-on",true,1000,"on.example",93,"active tie"},{"nine",false,9,"nine.example",94,"single digit"}};
        auto expected=std::vector<Forward>{fixture.forwards[4],fixture.forwards[2],fixture.forwards[1],fixture.forwards[3],fixture.forwards[0]};
        saveHost(directory_,fixture);manager_->saved(fixture);manager_->connect(fixture,false);openSettings("a");
        settings_.drafts.at("b").host.description="unsaved peer";auto peer=settings_.drafts.at("b");
        auto disk=loadConfig(directory_);auto execution=manager_->runtime("a")->executionHost;auto forwards=manager_->runtime("a")->forwards;
        check(IsWindowEnabled(control(RuleSort)),"sort button enabled with multiple rules");
        SetFocus(control(RuleSort));ensureFocusVisible();check(GetFocus()==control(RuleSort),"sort button receives keyboard focus");
        set(Name,L"");set(Target,L"");set(Port,L"invalid");
        ListView_SetItemState(control(Rules),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        SendMessageW(control(RuleSort),BM_CLICK,0,0);
        check(settings_.drafts.at("a").host.forwards==expected&&rule_==4&&ListView_GetNextItem(control(Rules),-1,LVNI_SELECTED)==4,"sort preserves selected rule by ID after numeric stable reorder");
        check(get(Name).empty()&&get(Target).empty()&&get(Port)==L"invalid"&&settings_.drafts.at("b")==peer,"sort preserves unfinished input and peer draft");
        check(loadConfig(directory_)==disk&&manager_->runtime("a")->executionHost==execution&&manager_->runtime("a")->forwards==forwards&&manager_->runtime("a")->state==ConnectionState::connected&&manager_->runtime("a")->desiredConnected,"sort does not save or change running forwarding state");
        command(RuleEdit,BN_CLICKED);check(ruleWindow_&&ruleDraft_->rule.id=="high","edit after sort opens the preserved rule");set(RuleDescription,L"edited high");closeRule(true);
        check(settings_.drafts.at("a").host.forwards[4].id=="high"&&settings_.drafts.at("a").host.forwards[4].description=="edited high","edit after sort updates the correct rule");
        ListView_SetCheckState(control(Rules),4,FALSE);
        check(!settings_.drafts.at("a").host.forwards[4].enabled&&settings_.drafts.at("a").host.forwards[1].enabled,"checkbox after sort updates the correct rule");
        command(RuleDelete,BN_CLICKED);
        const auto& deleted=settings_.drafts.at("a").host.forwards;
        check(deleted.size()==4&&std::none_of(deleted.begin(),deleted.end(),[](const auto& rule){return rule.id=="high";}),"delete after sort removes the selected ID");
        settings_.drafts.at("a").host.forwards=fixture.forwards;settings_.drafts.at("a").rulePorts=HostDraft(fixture).rulePorts;fillRules();fillRuleEditor();
        check(ListView_GetNextItem(control(Rules),-1,LVNI_SELECTED)==-1,"unselected fixture has no selected row");command(RuleSort,BN_CLICKED);
        check(rule_==-1&&ListView_GetNextItem(control(Rules),-1,LVNI_SELECTED)==-1&&!IsWindowEnabled(control(RuleEdit))&&!IsWindowEnabled(control(RuleDelete)),"sorting without selection keeps none and disables row actions");
        auto sorted=settings_.drafts.at("a");command(RuleSort,BN_CLICKED);check(settings_.drafts.at("a")==sorted,"native already-sorted click is a no-op");
        settings_.drafts.at("a").host.forwards=fixture.forwards;fillRules();showRule(true);auto modalDraft=settings_.drafts;
        command(RuleSort,BN_CLICKED);check(settings_.drafts==modalDraft&&ruleWindow_,"rule modal blocks sort");closeRule(false);
        showStatus();command(RuleSort,BN_CLICKED);check(settings_.drafts==modalDraft&&status_,"status modal blocks sort");closeStatus();
        auto selected=settings_.selected;settings_.selected.clear();fillRuleEditor();check(!IsWindowEnabled(control(RuleSort)),"sort disabled without host selection");command(RuleSort,BN_CLICKED);settings_.selected=selected;
        auto fullRules=settings_.drafts.at(selected).host.forwards;settings_.drafts.at(selected).host.forwards.resize(1);fillRuleEditor();check(!IsWindowEnabled(control(RuleSort)),"sort disabled with one rule");settings_.drafts.at(selected).host.forwards.clear();fillRuleEditor();check(!IsWindowEnabled(control(RuleSort)),"sort disabled with zero rules");settings_.drafts.at(selected).host.forwards=fullRules;fillRules();fillRuleEditor();
        set(Name,wide(fixture.name));set(Target,wide(fixture.target));set(Port,fixture.port?wide(std::to_string(*fixture.port)):L"");command(RuleSort,BN_CLICKED);
        auto commandText=get(CommandPreview);auto lowPosition=commandText.find(L"127.0.0.1:80:"),tiePosition=commandText.find(L"127.0.0.1:1000:"),highPosition=commandText.find(L"127.0.0.1:10000:");check(lowPosition!=std::wstring::npos&&tiePosition!=std::wstring::npos&&highPosition!=std::wstring::npos&&lowPosition<tiePosition&&tiePosition<highPosition,"command preview follows sorted active-rule order");
        command(Save,BN_CLICKED);check(loadHost(directory_,"a").forwards==expected&&!settings_.dirty("a")&&settings_.drafts.at("b")==peer,"explicit save persists sort without changing peer draft");
        check(manager_->runtime("a")->executionHost==execution&&manager_->runtime("a")->forwards==forwards,"sort save preserves current execution snapshot");
        closeSettings(false);openSettings("a");check(settings_.drafts.at("a").host.forwards==expected,"saved order survives closing and reopening");
        closeSettings(false);saveHost(directory_,fixture);manager_->saved(fixture);openSettings("a");command(RuleSort,BN_CLICKED);check(settings_.dirty("a"),"unsaved sort marks dirty");closeSettings(false);openSettings("a");
        check(settings_.drafts.at("a").host.forwards==fixture.forwards&&!settings_.dirty("a"),"discarded sort restores original saved order");
        closeSettings(false);outputLine("native local-port sort checks passed");
    }
    void nativeChecks(const std::wstring& instanceKey){
        auto check=[](bool value,const char* why){if(!value)throw LocalizedError(why);};
        openSettings({},true);check(settings_.startup&&settings_.order.size()==2,"startup settings load");
        auto originalLanguage=languageSelection();
        for (const std::string choice : {"ko", "auto"}) {
            selectLanguage(choice, {"en-US"});
            createTrayMenu();
            auto submenu = GetSubMenu(activeMenu_, 1);
            wchar_t title[128]{};
            GetMenuStringW(submenu, 20, title, 128, MF_BYCOMMAND);
            const bool automatic = choice == "auto";
            check(std::wstring(title) == (automatic ? L"Automatic (System Language)" : L"자동(시스템 언어)"),
                  "localized automatic menu title");
            check((GetMenuState(submenu, 20, MF_BYCOMMAND) & MF_CHECKED) == (automatic ? MF_CHECKED : 0),
                  "automatic check follows selected mode");
            destroyTrayMenu();
        }
        selectLanguage(originalLanguage, systemUILanguages());
        set(Name,L"Unsaved {host} — 日本語");set(Port,L"invalid");capture();auto untranslatedDraft=settings_.drafts;
        SetFocus(control(Name));SendMessageW(control(Name),EM_SETSEL,2,5);
        for(const auto& language:languages()){
            changeLanguage(language.id);DWORD begin=0,end=0;SendMessageW(control(Name),EM_GETSEL,reinterpret_cast<WPARAM>(&begin),reinterpret_cast<LPARAM>(&end));
            check(GetFocus()==control(Name)&&begin==2&&end==5,"language switch preserves edit selection and focus");
            check(settings_.drafts==untranslatedDraft&&get(Port)==L"invalid","language switch preserves invalid unsaved values");
            createTrayMenu();auto submenu=GetSubMenu(activeMenu_,1);check(submenu&&GetMenuItemCount(submenu)==9,"language submenu has automatic separator and seven languages");
            int checked=0;for(UINT id=20;id<28;++id)if(GetMenuState(submenu,id,MF_BYCOMMAND)&MF_CHECKED)++checked;
            check(checked==1&&!(GetMenuState(submenu,20,MF_BYCOMMAND)&MF_CHECKED),"manual language menu has one check");destroyTrayMenu();
            nativeLayoutChecks();SetFocus(control(Name));SendMessageW(control(Name),EM_SETSEL,2,5);
        }
        changeLanguage(originalLanguage);set(Name,L"A");set(Port,L"");capture();
        auto originalRules=settings_.drafts.at("a").host.forwards;
        showRule(true);check(ruleWindow_&&!IsWindowEnabled(settingsWindow_)&&settings_.modalHost=="a","rule modal blocks owner");check(get(RuleDescription).empty(),"new rule description defaults empty");set(RuleDescription,L"취소할 설명");
        set(LocalPort,L"invalid");closeRule(true);check(ruleWindow_&&!IsWindowEnabled(settingsWindow_)&&get(LocalPort)==L"invalid"&&settings_.drafts.at("a").host.forwards==originalRules,"invalid modal warns and stays open with input and draft preserved");
        openSettings("b");check(settings_.selected=="a"&&ruleWindow_,"second open preserves rule modal target");
        closeRule(false);check(IsWindowEnabled(settingsWindow_)&&settings_.drafts.at("a").host.forwards==originalRules,"cancel new rule preserves draft");
        auto enterFocused=[&](int id){
            SetFocus(control(id));check(GetFocus()==control(id),"rule keyboard target receives focus");
            MSG key{};key.hwnd=GetFocus();key.message=WM_KEYDOWN;key.wParam=VK_RETURN;
            check(preprocess(key),"rule Enter key is handled");
        };
        auto beforeNew=settings_.drafts.at("a");showRule(true);set(LocalPort,L"18081");enterFocused(RuleCancel);
        check(!ruleWindow_&&IsWindowEnabled(settingsWindow_)&&settings_.drafts.at("a")==beforeNew,"Cancel Enter discards valid new rule without changing draft");
        showRule(true);set(LocalPort,L"18080");set(RuleDescription,L"개발 웹 서비스");enterFocused(RuleDescription);check(!ruleWindow_&&ListView_GetItemCount(control(Rules))==2,"Enter in field confirms new rule");
        auto added=settings_.drafts.at("a").host.forwards.back();check(added.description=="개발 웹 서비스","new rule description committed");wchar_t ruleText[256]{};ListView_GetItemText(control(Rules),1,4,ruleText,256);check(std::wstring(ruleText)==L"개발 웹 서비스","rule list displays description in final column");auto beforeEdit=settings_.drafts.at("a");showRule(false);check(get(RuleDescription)==L"개발 웹 서비스","edit loads saved draft description");set(RuleDescription,L"취소할 변경");set(RemoteHost,L"cancelled.example");enterFocused(RuleCancel);
        check(!ruleWindow_&&IsWindowEnabled(settingsWindow_)&&settings_.drafts.at("a")==beforeEdit,"Cancel Enter discards rule edit without changing draft");
        showRule(false);set(RuleDescription,L"수정된 웹 서비스");set(RemoteHost,L"accepted.example");enterFocused(RuleOkay);check(!ruleWindow_&&settings_.drafts.at("a").host.forwards.back().id==added.id&&settings_.drafts.at("a").host.forwards.back().remoteHost=="accepted.example","OK Enter confirms edit and preserves ID");
        check(settings_.drafts.at("a").host.forwards.back().description=="수정된 웹 서비스","rule description edit committed");
        auto beforeCancel=settings_.drafts.at("a");showRule(false);set(RuleDescription,L"Escape 취소");
        MSG escape{};escape.hwnd=control(RuleDescription);escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;check(preprocess(escape),"rule Escape handled");
        check(!ruleWindow_&&settings_.drafts.at("a")==beforeCancel,"Escape discards description changes");
        showRule(false);set(RuleDescription,L"닫기 취소");SendMessageW(ruleWindow_,WM_CLOSE,0,0);
        check(!ruleWindow_&&settings_.drafts.at("a")==beforeCancel,"window close discards description changes");
        NMHDR doubleClick{control(Rules),Rules,NM_DBLCLK};SendMessageW(settingsWindow_,WM_NOTIFY,Rules,reinterpret_cast<LPARAM>(&doubleClick));
        check(ruleWindow_&&get(RuleDescription)==L"수정된 웹 서비스","double click opens description editor");set(RuleDescription,L"");enterFocused(RuleOkay);
        check(settings_.drafts.at("a").host.forwards.back().description.empty()&&settings_.drafts.at("a").host.forwards.back().id==added.id,"clearing description keeps ID");
        command(RuleDelete,BN_CLICKED);check(settings_.drafts.at("a").host.forwards==originalRules,"delete selected rule after modal");
        set(Name,L"A edited");set(Port,L"invalid");
        SendMessageW(control(Hosts),LB_SETCURSEL,1,0);command(Hosts,LBN_SELCHANGE);
        set(Name,L"B saved");set(Description,L"테스트 DB와 웹");command(Save,BN_CLICKED);
        check(loadHost(directory_,"b").description=="테스트 DB와 웹","native description save");
        check(loadHost(directory_,"b").name=="B saved"&&loadHost(directory_,"a").name=="A","native selected save");
        check(settings_.drafts.at("a").port=="invalid"&&settings_.dirty("a"),"native host switch preserves invalid draft");
        showStatus();check(status_&&!IsWindowEnabled(settingsWindow_)&&settings_.modalHost=="b","native modal blocks owner");
        auto host=loadHost(directory_,"b");manager_->connect(host,false);
        check(get(StatusHeading).find(L"server-b")!=std::wstring::npos&&ListView_GetItemCount(control(StatusForwards))==1,"native live status initial target");
        auto executionBeforeDescription=manager_->runtime("b")->executionHost;closeStatus();
        ListView_SetItemState(control(Rules),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);showRule(false);set(RuleDescription,L"실행 중 DB 설명");closeRule(true);
        check(settings_.dirty("b"),"native description-only edit marks dirty");command(Save,BN_CLICKED);showStatus();
        check(loadHost(directory_,"b").forwards[0].description=="실행 중 DB 설명"&&!settings_.dirty("b"),"native rule description persists and clears dirty flag");
        check(manager_->runtime("b")->executionHost==executionBeforeDescription&&manager_->runtime("b")->state==ConnectionState::connected,"native description save preserves running connection");
        check(get(StatusSummary).find(tw("win.running_settings_differ_from_edited_settings"))==std::wstring::npos,"description-only change has no execution difference warning");
        createTrayMenu();menuOpen_=true;auto rowID=menus_.at("b").rows.front().second;
        MENUITEMINFOW image{};image.cbSize=sizeof(image);image.fMask=MIIM_BITMAP;check(GetMenuItemInfoW(activeMenu_,1,FALSE,&image)&&image.hbmpItem,"settings menu has bitmap");
        updateMenu();check(menus_.at("b").rows.front().second==rowID,"status update retains rule menu identity");
        check(GetMenuItemCount(menus_.at("b").menu)==7,"summary separators rule and actions only");
        MENUITEMINFOW before{};before.cbSize=sizeof(before);before.fMask=MIIM_BITMAP;GetMenuItemInfoW(menus_.at("b").menu,rowID,FALSE,&before);manager_->disconnect("b");
        MENUITEMINFOW after{};after.cbSize=sizeof(after);after.fMask=MIIM_BITMAP;GetMenuItemInfoW(menus_.at("b").menu,rowID,FALSE,&after);check(menus_.at("b").rows.front().second==rowID&&before.hbmpItem!=after.hbmpItem,"open menu updates disconnected rule without replacing identity");menuOpen_=false;destroyTrayMenu();
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
        outputLine("native existing controller checks passed");
        nativeAppDialogChecks();
        nativeSortChecks();
        manager_->quit();outputLine("native UI controller tests passed");
    }
    LRESULT message(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
        if(msg==WM_CTLCOLORSTATIC||msg==WM_CTLCOLORBTN)return surfaceColor(w,l);
        if(hwnd==window_){if(taskbarCreated_&&msg==taskbarCreated_){trayAdded_=false;updateTray();return 0;}switch(msg){case WM_TASK:{std::unique_ptr<std::function<void()>> task(reinterpret_cast<std::function<void()>*>(l));(*task)();return 0;}case WM_TIMER:loop_->timer(w);return 0;case WM_OPEN_SETTINGS:if(menuOpen_)EndMenu();openSettings();return 1;case WM_TRAY:if(LOWORD(l)==WM_CONTEXTMENU||LOWORD(l)==WM_RBUTTONUP||LOWORD(l)==NIN_SELECT||LOWORD(l)==NIN_KEYSELECT)trayMenu();return 0;case WM_POWERBROADCAST:if(w==PBT_APMSUSPEND)manager_->sleep();else if(w==PBT_APMRESUMEAUTOMATIC||w==PBT_APMRESUMESUSPEND)manager_->wake();return TRUE;case WM_QUERYENDSESSION:return TRUE;case WM_ENDSESSION:if(w)quit();return 0;case WM_CLOSE:quit();return 0;}}
        else{switch(msg){case WM_REMEMBER_SETTINGS_FOCUS:rememberSettingsFocus(reinterpret_cast<HWND>(w));return 0;case WM_ACTIVATE:if(LOWORD(w)==WA_INACTIVE)rememberSettingsFocus(GetFocus());break;case WM_COMMAND:command(LOWORD(w),HIWORD(w));return 0;case WM_NOTIFY:{auto* notification=reinterpret_cast<NMHDR*>(l);if(notification->idFrom==Rules){if(notification->code==LVN_ITEMCHANGED)ruleChanged(reinterpret_cast<NMLISTVIEW*>(l));else if(notification->code==NM_DBLCLK)showRule(false);}return 0;}case WM_SIZE:layoutSettings();return 0;case WM_GETMINMAXINFO:{constrainMinimum(hwnd,reinterpret_cast<MINMAXINFO*>(l),800,430);return 0;}case WM_DPICHANGED:{auto* rect=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);updateFont();layoutSettings();return 0;}case WM_CLOSE:closeSettings(false);return 0;}}
        return DefWindowProcW(hwnd,msg,w,l);
    }
    LRESULT formMessage(HWND hwnd,UINT msg,WPARAM w,LPARAM l){switch(msg){case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:return surfaceColor(w,l);case WM_COMMAND:case WM_NOTIFY:return SendMessageW(settingsWindow_,msg,w,l);case WM_VSCROLL:scrollForm(LOWORD(w));return 0;case WM_MOUSEWHEEL:scrollForm(-1,MulDiv(GET_WHEEL_DELTA_WPARAM(w),72,WHEEL_DELTA));return 0;}return DefWindowProcW(hwnd,msg,w,l);}
    LRESULT statusMessage(HWND hwnd,UINT msg,WPARAM w,LPARAM l){switch(msg){case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:return surfaceColor(w,l);case WM_COMMAND:if(LOWORD(w)==StatusClose||LOWORD(w)==IDOK||LOWORD(w)==IDCANCEL)closeStatus();return 0;case WM_CLOSE:closeStatus();return 0;case WM_SIZE:layoutStatus();return 0;case WM_GETMINMAXINFO:constrainMinimum(hwnd,reinterpret_cast<MINMAXINFO*>(l),600,390);return 0;case WM_DPICHANGED:{auto* rect=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);updateStatusFont();layoutStatus();return 0;}}return DefWindowProcW(hwnd,msg,w,l);}
    LRESULT ruleMessage(HWND hwnd,UINT msg,WPARAM w,LPARAM l){switch(msg){case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:return surfaceColor(w,l);case WM_COMMAND:if(LOWORD(w)==RuleOkay||LOWORD(w)==IDOK)closeRule(true);else if(LOWORD(w)==RuleCancel||LOWORD(w)==IDCANCEL)closeRule(false);return 0;case WM_CLOSE:closeRule(false);return 0;case WM_SIZE:layoutRule();return 0;case WM_GETMINMAXINFO:constrainMinimum(hwnd,reinterpret_cast<MINMAXINFO*>(l),400,270);return 0;case WM_DPICHANGED:{auto* rect=reinterpret_cast<RECT*>(l);SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE);updateRuleFont();layoutRule();return 0;}}return DefWindowProcW(hwnd,msg,w,l);}
    void caught(const std::exception& exception){error(exception.what());}
};
template<class F>LRESULT dispatchWindow(HWND hwnd,UINT msg,WPARAM w,LPARAM l,F&& invoke){auto* app=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_NCCREATE){app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}if(app){try{return invoke(*app);}catch(const std::exception& e){app->caught(e);return 0;}}return DefWindowProcW(hwnd,msg,w,l);}
LRESULT CALLBACK appProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.message(hwnd,msg,w,l);});}
LRESULT CALLBACK formProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.formMessage(hwnd,msg,w,l);});}
LRESULT CALLBACK ruleProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.ruleMessage(hwnd,msg,w,l);});}
LRESULT CALLBACK statusProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l){return dispatchWindow(hwnd,msg,w,l,[&](App& app){return app.statusMessage(hwnd,msg,w,l);});}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);bool headless=false;int result=1;fs::path ownedTestDirectory;
    try{
        if(!argv)throw LocalizedError(winError());std::vector<std::wstring> args(argv,argv+argc);LocalFree(argv);argv=nullptr;
        bool isolated=args.size()>=2&&args[1].rfind(L"--test-",0)==0;
        if(isolated)selectLanguage("en",{});else initializeLanguage();
        if(args.size()>=3&&args[args.size()-2]==L"--ui-language"){
            auto language=utf8(args.back());if(!supportedLanguage(language))throw LocalizedError(tr("win.unsupported_ui_language"));
            selectLanguage(language,{});args.resize(args.size()-2);
        }
        auto directory=homeDirectory()/L".LocalForwarder";bool test=false,start=true,open=false,uiTest=false,uiPreview=false;std::string config;
        if(args.size()>=2){
            headless=args[1].rfind(L"--test-",0)==0;
            if(args[1]==L"--setup-auth"&&args.size()==3){result=setupAuthentication(directory,utf8(args[2]));if(SUCCEEDED(com))CoUninitialize();return result;}
            if(args[1]==L"--test-setup-auth"&&args.size()==5){result=setupAuthentication(args[2],utf8(args[3]),utf8(args[4]),false);if(SUCCEEDED(com))CoUninitialize();return result;}
            if(args[1]==L"--test-instance-peer"&&args.size()==3){if(!safeId(utf8(args[2])))throw LocalizedError("Invalid test instance key");SingleInstance peer;if(peer.acquire(args[2]))throw LocalizedError("Test instance was not already running");if(SUCCEEDED(com))CoUninitialize();return 0;}
            if(args[1]==L"--open-config"&&args.size()==2)open=true;
            else if(args[1]==L"--test-manager"&&(args.size()==4||(args.size()==5&&args[4]==L"--no-start"))){test=true;directory=args[2];config=utf8(args[3]);start=args.size()==4;}
            else if(args[1]==L"--test-ui-preview"&&(args.size()>=2&&args.size()<=4)){
                if(args.size()>=3){if(args[2]!=L"96"&&args[2]!=L"120"&&args[2]!=L"144"&&args[2]!=L"192"&&args[2]!=L"240")throw LocalizedError("Preview DPI must be 96, 120, 144, 192 or 240");previewDpi=static_cast<UINT>(std::stoi(args[2]));}
                if(args.size()==4){previewScene=args[3];if(std::set<std::wstring>{L"settings",L"long",L"empty",L"rule",L"status",L"about",L"confirm",L"error",L"tray",L"tray-host"}.count(previewScene)==0)throw LocalizedError("Unknown preview scene");}
                test=true;uiTest=true;uiPreview=true;start=false;directory=fs::temp_directory_path()/wide("LocalForwarder-preview-"+newId());ownedTestDirectory=directory;
                Host a;a.id="a";a.name="개발 서버";a.description="개발 DB와 웹 서비스의 연결 용도를 설명하는 긴 한 줄 설명";a.target="development-server.example.invalid";a.forwards={{"db",true,15432,"database.internal.example.invalid",5432,"개발 데이터베이스 조회와 관리 도구에서 사용하는 연결"},{"web",true,18080,"localhost",80,"로컬 웹 서비스 확인"},{"disabled",false,16379,"cache.internal",6379,"필요할 때만 사용하는 캐시 관리"}};
                for(int i=0;i<8;++i)a.forwards.push_back({"extra-"+std::to_string(i),false,19000+i,"localhost",80});
                if(previewScene==L"empty")a.forwards.clear();
                if(previewScene==L"long"){a.name="개발 서버 — 긴 한글 이름과 English name";a.user="developer";a.port=2222;a.identityFile="C:\\Users\\developer\\Documents\\SSH keys\\development-environment-very-long-key-name";}
                Host b=a;b.id="b";b.name="수동 연결 서버";b.autoConnect=false;saveHost(directory,a);saveHost(directory,b);
            }
            else if(args[1]==L"--test-ui"&&args.size()==2){test=true;uiTest=true;start=false;directory=fs::temp_directory_path()/wide("LocalForwarder-ui-"+newId());ownedTestDirectory=directory;Host a;a.id="a";a.name="A";a.target="server-a";a.forwards.push_back({"f",true,12000,"localhost",80});Host b=a;b.id="b";b.name="B";b.target="server-b";b.autoConnect=false;saveHost(directory,a);saveHost(directory,b);}
            else throw LocalizedError(tr("win.usage"));
        }
        SingleInstance singleton;std::wstring instanceKey=uiTest?wide(newId()):L"";
        if((!test||uiTest)&&!singleton.acquire(instanceKey)){if(SUCCEEDED(com))CoUninitialize();return 0;}
        INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
        {
            App app(instance,directory,test,start,open,config,uiTest,uiPreview);app.create(!test||uiTest?singleton.windowTitle():L"LocalForwarder test");if(!test||uiTest)singleton.ready();
            if(uiTest&&!uiPreview){app.nativeChecks(instanceKey);result=0;}
            else{MSG message{};BOOL received;while((received=GetMessageW(&message,nullptr,0,0))>0){if(!app.preprocess(message)){TranslateMessage(&message);DispatchMessageW(&message);}}result=received==-1?1:static_cast<int>(message.wParam);}
        }
    }catch(const std::exception& e){if(argv)LocalFree(argv);outputLine("LF_ERROR "+std::string(e.what()));if(!headless)MessageBoxW(nullptr,wide(e.what()).c_str(),L"LocalForwarder",MB_OK|MB_ICONERROR);}
    if(!ownedTestDirectory.empty()){std::error_code ignored;fs::remove_all(ownedTestDirectory,ignored);}if(SUCCEEDED(com))CoUninitialize();return result;
}
