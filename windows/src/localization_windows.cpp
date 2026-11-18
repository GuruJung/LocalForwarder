#include "platform.hpp"
#include "localization.hpp"

namespace lf {
namespace {std::mutex wideMutex;std::map<std::pair<std::string,std::string>,std::wstring> wideCache;constexpr wchar_t SettingsKey[]=L"Software\\GuruJung\\LocalForwarder";constexpr wchar_t LanguageValue[]=L"DisplayLanguage";}
const wchar_t* tw(const std::string& key){
    std::lock_guard lock(wideMutex);auto language=currentLanguage();auto pair=std::make_pair(language,key);
    auto found=wideCache.find(pair);if(found==wideCache.end())found=wideCache.emplace(pair,wide(translated(key,{},language))).first;
    return found->second.c_str();
}
std::string wideTranslationKey(const wchar_t* text){std::lock_guard lock(wideMutex);for(const auto& [pair,value]:wideCache)if(value.c_str()==text)return pair.second;return {};}
std::vector<std::string> systemUILanguages(){
    ULONG count=0,size=0;
    if(!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,&count,nullptr,&size)||!size)return {};
    std::vector<wchar_t> buffer(size);
    if(!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME,&count,buffer.data(),&size))return {};
    std::vector<std::string> result;
    for(size_t at=0;at<buffer.size()&&buffer[at];){size_t end=at;while(end<buffer.size()&&buffer[end])++end;if(end==buffer.size())break;result.push_back(utf8(std::wstring(buffer.data()+at,end-at)));at=end+1;}
    return result;
}
std::string readLanguagePreference(const std::wstring& subkey){
    wchar_t value[32]{};DWORD bytes=sizeof(value);
    auto result=RegGetValueW(HKEY_CURRENT_USER,subkey.c_str(),LanguageValue,RRF_RT_REG_SZ,nullptr,value,&bytes);
    if(result!=ERROR_SUCCESS)return "auto";
    try{return utf8(value);}catch(const std::exception&){return "auto";}
}
void initializeLanguage(){selectLanguage(readLanguagePreference(SettingsKey),systemUILanguages());}
void persistLanguage(const std::string& selection){writeLanguagePreference(SettingsKey,selection);}
void writeLanguagePreference(const std::wstring& subkey,const std::string& selection){
    HKEY key=nullptr;auto result=RegCreateKeyExW(HKEY_CURRENT_USER,subkey.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(result==ERROR_SUCCESS){auto value=wide(selection);result=RegSetValueExW(key,LanguageValue,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));RegCloseKey(key);}
    if(result!=ERROR_SUCCESS)throw LocalizedError(tr("win.language_save_failed")+"\n"+winError(result));
}
}
