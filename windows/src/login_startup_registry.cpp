#include "login_startup_registry.hpp"
#include <vector>

namespace lf {
namespace {
constexpr wchar_t ValueName[]=L"LocalForwarder";
struct RegistryKey {
    HKEY value=nullptr;
    ~RegistryKey(){if(value)RegCloseKey(value);}
};
void registryError(LSTATUS result) {
    if(result!=ERROR_SUCCESS) throw LocalizedError(tr("win.launch_at_login_setting")+winError(result));
}
bool validCommand(const std::wstring& command) {
    if(command.size()<3 || command.size()>260 || command.front()!=L'"' || command.back()!=L'"')return false;
    auto path=command.substr(1,command.size()-2);
    return path.find_first_of(L"\"\r\n") == std::wstring::npos &&
        path.find(L'\0') == std::wstring::npos && fs::path(path).is_absolute() &&
        fs::path(path).has_filename();
}
}
bool RegistryLoginStartup::enabled() {
    RegistryKey key;
    auto result=RegOpenKeyExW(root_,subkey_.c_str(),0,KEY_QUERY_VALUE,&key.value);
    if(result==ERROR_FILE_NOT_FOUND)return false;
    registryError(result);
    DWORD type=0,size=0;
    result=RegQueryValueExW(key.value,ValueName,nullptr,&type,nullptr,&size);
    if(result==ERROR_FILE_NOT_FOUND)return false;
    registryError(result);
    // A conflicting value is unknown, not unchecked: do not silently overwrite it.
    if(type!=REG_SZ || size<sizeof(wchar_t)*4 || size>sizeof(wchar_t)*261 || size%sizeof(wchar_t))
        throw LocalizedError(tr("win.the_launch_at_login_registration_value_has_an_invalid_format"));
    std::vector<wchar_t> value(size/sizeof(wchar_t));
    auto expectedSize=size;
    registryError(RegQueryValueExW(key.value,ValueName,nullptr,&type,reinterpret_cast<BYTE*>(value.data()),&size));
    if(type!=REG_SZ || size!=expectedSize || value.back()!=L'\0' ||
        !validCommand(std::wstring(value.data(),value.size()-1)))
        throw LocalizedError(tr("win.the_launch_at_login_registration_value_has_an_invalid_format"));
    return true;
}
void RegistryLoginStartup::setEnabled(bool enable) {
    // Also validate existing data for callers other than the tray controller.
    auto registered=enabled();
    if(!enable) {
        if(!registered)return;
        RegistryKey key;
        auto result=RegOpenKeyExW(root_,subkey_.c_str(),0,KEY_SET_VALUE,&key.value);
        if(result==ERROR_FILE_NOT_FOUND)return;
        registryError(result);
        result=RegDeleteValueW(key.value,ValueName);
        if(result!=ERROR_FILE_NOT_FOUND)registryError(result);
        return;
    }
    auto command=L"\""+executable_.wstring()+L"\"";
    if(!validCommand(command))
        throw LocalizedError(tr("win.the_startup_executable_path_must_be_absolute_and_at_most_260_char"));
    RegistryKey key;
    registryError(RegCreateKeyExW(root_,subkey_.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key.value,nullptr));
    registryError(RegSetValueExW(key.value,ValueName,0,REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t))));
}
void appendLoginStartupMenu(HMENU menu,UINT command,LoginStartupController& controller) {
    controller.refresh();
    auto state=controller.enabled();
    UINT flags=MF_STRING;
    if(!state || controller.busy()) flags|=MF_GRAYED;
    if(state && *state) flags|=MF_CHECKED;
    auto label=state?tw("win.launch_at_login"):tw("win.launch_at_login_status_unavailable");
    if(!AppendMenuW(menu,flags,command,label))throw LocalizedError(winError());
}
}
