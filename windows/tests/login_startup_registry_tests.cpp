#include "login_startup_registry.hpp"
#include <sddl.h>
#include <iostream>
#include <vector>
using namespace lf;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(const char* message,F operation){bool rejected=false;try{operation();}catch(const std::exception&){rejected=true;}check(rejected,message);}
struct Fixture {
    std::wstring subkey=L"Software\\LocalForwarderTests-"+wide(newId());HKEY key=nullptr;
    Fixture(){check(RegCreateKeyExW(HKEY_CURRENT_USER,subkey.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&key,nullptr)==ERROR_SUCCESS,"create disposable registry key");}
    void deny(DWORD access) {
        auto sid=currentUserSID();
        check(access==0||access==KEY_QUERY_VALUE||access==KEY_SET_VALUE,"supported test denial");
        auto mask=access==KEY_QUERY_VALUE?L"0x1":L"0x2";
        auto sddl=L"D:P"+(access?L"(D;;"+std::wstring(mask)+L";;;"+sid+L")":L"")+L"(A;;KA;;;"+sid+L")";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"create test DACL");
        auto result=RegSetKeySecurity(key,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor);
        LocalFree(descriptor);check(result==ERROR_SUCCESS,"set test DACL");
    }
    void put(const std::wstring& value,DWORD type=REG_SZ,const wchar_t* name=L"LocalForwarder") {
        check(RegSetValueExW(key,name,0,type,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS,"seed test registry value");
    }
    std::wstring value(const wchar_t* name=L"LocalForwarder") {
        DWORD type=0,size=0;check(RegQueryValueExW(key,name,nullptr,&type,nullptr,&size)==ERROR_SUCCESS,"read test value size");
        std::vector<wchar_t> text(size/sizeof(wchar_t));
        check(RegQueryValueExW(key,name,nullptr,&type,reinterpret_cast<BYTE*>(text.data()),&size)==ERROR_SUCCESS,"read test value");
        return std::wstring(text.data(),text.size()-1);
    }
    void erase(){auto result=RegDeleteValueW(key,L"LocalForwarder");check(result==ERROR_SUCCESS||result==ERROR_FILE_NOT_FOUND,"erase test value");}
    ~Fixture(){if(key){try{deny(0);}catch(...){}RegCloseKey(key);}RegDeleteTreeW(HKEY_CURRENT_USER,subkey.c_str());}
};
struct FakeStartup:LoginStartup {
    bool registered=false,fail=false;int reads=0,writes=0;
    bool enabled()override{++reads;if(fail)throw std::runtime_error("read denied");return registered;}
    void setEnabled(bool value)override{++writes;registered=value;}
};
void menuChecks() {
    auto fake=std::make_shared<FakeStartup>();LoginStartupController controller(fake);
    for(int state=0;state<3;++state) {
        fake->registered=state==1;fake->fail=state==2;
        auto menu=CreatePopupMenu();check(menu!=nullptr,"create native test menu");
        AppendMenuW(menu,MF_STRING,1,tw("win.settings"));
        appendLoginStartupMenu(menu,4,controller);
        AppendMenuW(menu,MF_STRING,2,tw("win.reconnect_all"));
        auto flags=GetMenuState(menu,4,MF_BYCOMMAND);
        bool order=GetMenuItemID(menu,0)==1&&GetMenuItemID(menu,1)==4&&GetMenuItemID(menu,2)==2;
        wchar_t label[100]{};GetMenuStringW(menu,4,label,100,MF_BYCOMMAND);
        bool checked=(flags&MF_CHECKED)!=0,disabled=(flags&MF_GRAYED)!=0;
        DestroyMenu(menu);
        check(order,"startup item between settings and reconnect");
        check(checked==(state==1),"native menu checkmark follows OS state");
        check(disabled==(state==2),"query failure disables native menu");
        check(state!=2||std::wstring(label).find(tw("win.launch_at_login_status_unavailable"))!=std::wstring::npos,"explicit unknown state label");
    }
    check(fake->reads==3&&fake->writes==0,"each native menu opening rereads without writes");
}
int main(){try{
    menuChecks();
    Fixture fixture;
    check(readLanguagePreference(fixture.subkey)=="auto","missing language preference defaults to auto");
    writeLanguagePreference(fixture.subkey,"ko");check(readLanguagePreference(fixture.subkey)=="ko","native language preference round trip");
    selectLanguage(readLanguagePreference(fixture.subkey),{"en"});check(currentLanguage()=="ko","native preference restored");
    fixture.deny(KEY_SET_VALUE);bool saveFailed=false;
    try{selectLanguage("de",{},[&](const auto& value){writeLanguagePreference(fixture.subkey,value);});}catch(...){saveFailed=true;}
    check(saveFailed&&currentLanguage()=="ko"&&readLanguagePreference(fixture.subkey)=="ko","failed native save preserves preference and current locale");fixture.deny(0);
    fixture.put(L"future-language",REG_SZ,L"DisplayLanguage");selectLanguage(readLanguagePreference(fixture.subkey),{"ja-JP"});check(languageSelection()=="auto"&&currentLanguage()=="ja","unknown native preference uses system language");
    selectLanguage("en",{});
    RegistryLoginStartup adapter(L"C:\\한글 경로\\LocalForwarder.exe",HKEY_CURRENT_USER,fixture.subkey);
    check(!adapter.enabled(),"missing registration defaults unchecked");
    adapter.setEnabled(false);check(!adapter.enabled(),"unregister absent is idempotent");
    fixture.put(L"unrelated",REG_SZ,L"OtherApplication");
    adapter.setEnabled(true);check(adapter.enabled(),"registered value is checked");
    check(fixture.value()==L"\"C:\\한글 경로\\LocalForwarder.exe\"","quoted absolute Unicode executable without arguments");
    adapter.setEnabled(true);check(adapter.enabled(),"registration is idempotent");
    fixture.put(L"\"D:\\previous folder\\LocalForwarder.exe\"");
    check(adapter.enabled(),"older valid executable registration still checked");
    auto old=fixture.value();
    RegistryLoginStartup longPath(fs::path(L"C:\\"+std::wstring(255,L'a')+L".exe"),HKEY_CURRENT_USER,fixture.subkey);
    rejects("overlength command must fail",[&]{longPath.setEnabled(true);});check(fixture.value()==old,"overlength failure preserves existing value");
    RegistryLoginStartup boundary(fs::path(L"C:\\"+std::wstring(255,L'a')),HKEY_CURRENT_USER,fixture.subkey);
    boundary.setEnabled(true);check(fixture.value().size()==260&&boundary.enabled(),"260 character quoted command accepted");
    RegistryLoginStartup relative(L"relative.exe",HKEY_CURRENT_USER,fixture.subkey);
    rejects("relative executable must fail",[&]{relative.setEnabled(true);});
    adapter.setEnabled(false);check(!adapter.enabled(),"unregister removes own value");
    check(fixture.value(L"OtherApplication")==L"unrelated","other startup values preserved");
    for(auto malformed:{L"C:\\unquoted.exe",L"\"C:\\app.exe\" --argument",L"\"relative.exe\""}) {
        fixture.put(malformed);rejects("malformed command read must fail",[&]{adapter.enabled();});rejects("malformed command enable must fail",[&]{adapter.setEnabled(true);});
        rejects("malformed command disable must fail",[&]{adapter.setEnabled(false);});check(fixture.value()==malformed,"malformed value preserved");fixture.erase();
    }
    fixture.put(L"\"C:\\app.exe\"",REG_EXPAND_SZ);rejects("REG_EXPAND_SZ read must fail",[&]{adapter.enabled();});fixture.erase();
    auto embedded=std::wstring(L"\"C:\\app.exe\"");embedded.push_back(L'\0');embedded+=L"junk";
    fixture.put(embedded);rejects("embedded NUL read must fail",[&]{adapter.enabled();});fixture.erase();
    // RegSetValueExW can append a NUL just beyond cbData. Keep a non-NUL
    // sentinel beyond the requested payload and verify the actual raw fixture.
    const wchar_t unterminated[]=L"\"C:\\app.exe\"!";
    constexpr DWORD unterminatedBytes=sizeof(unterminated)-2*sizeof(wchar_t);
    check(RegSetValueExW(fixture.key,L"LocalForwarder",0,REG_SZ,reinterpret_cast<const BYTE*>(unterminated),unterminatedBytes)==ERROR_SUCCESS,"seed unterminated value");
    DWORD rawBytes=0,rawType=0;
    check(RegQueryValueExW(fixture.key,L"LocalForwarder",nullptr,&rawType,nullptr,&rawBytes)==ERROR_SUCCESS&&rawType==REG_SZ&&rawBytes==unterminatedBytes,"fixture must store exactly the nonterminated payload");
    std::vector<wchar_t> rawValue(rawBytes/sizeof(wchar_t));
    check(RegQueryValueExW(fixture.key,L"LocalForwarder",nullptr,nullptr,reinterpret_cast<BYTE*>(rawValue.data()),&rawBytes)==ERROR_SUCCESS&&rawValue.back()==L'"',"fixture must have no terminal NUL");
    rejects("unterminated REG_SZ read must fail",[&]{adapter.enabled();});fixture.erase();
    adapter.setEnabled(true);old=fixture.value();
    fixture.deny(KEY_SET_VALUE);rejects("write-denied disable must fail",[&]{adapter.setEnabled(false);});rejects("write-denied enable must fail",[&]{adapter.setEnabled(true);});
    check(adapter.enabled(),"write failure keeps registration");fixture.deny(0);
    check(fixture.value()==old,"denied write preserves data");
    fixture.deny(KEY_QUERY_VALUE);rejects("read-denied query must fail",[&]{adapter.enabled();});rejects("read-denied disable must fail",[&]{adapter.setEnabled(false);});fixture.deny(0);
    RegistryLoginStartup missing(L"C:\\app.exe",HKEY_CURRENT_USER,fixture.subkey+L"\\Missing");
    check(!missing.enabled(),"missing key is unchecked");missing.setEnabled(false);
    HKEY absent=nullptr;check(RegOpenKeyExW(fixture.key,L"Missing",0,KEY_READ,&absent)==ERROR_FILE_NOT_FOUND,"query and disabled setting create no keys");
    std::cout<<"login startup native registry and menu checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
