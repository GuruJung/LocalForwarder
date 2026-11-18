#include "localization.hpp"
#include "generated_translations.hpp"
#include <algorithm>
#include <sstream>

namespace lf {
namespace {std::mutex languageMutex;std::string selection="auto",effective="en";}
const std::vector<LanguageChoice>& languages(){
    static const std::vector<LanguageChoice> values={{"zh-Hans","Chinese (Simplified) — 简体中文"},{"zh-Hant","Chinese (Traditional) — 繁體中文"},{"en","English"},{"de","German — Deutsch"},{"ja","Japanese — 日本語"},{"ko","Korean — 한국어"},{"es","Spanish — Español"}};
    return values;
}
bool supportedLanguage(const std::string& id){return std::any_of(languages().begin(),languages().end(),[&](const auto& l){return l.id==id;});}
std::string resolveLanguage(const std::vector<std::string>& preferences){
    for(auto tag:preferences){
        for(auto& c:tag){if(c=='_')c='-';else if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');}
        std::vector<std::string> parts;std::istringstream stream(tag);for(std::string part;std::getline(stream,part,'-');)parts.push_back(part);
        if(parts.empty())continue;
        if(parts[0]=="zh"){
            // Script precedes region, regardless of contradictory region subtags.
            if(std::find(parts.begin(),parts.end(),"hans")!=parts.end())return "zh-Hans";
            if(std::find(parts.begin(),parts.end(),"hant")!=parts.end())return "zh-Hant";
            for(const auto& p:parts)if(p=="tw"||p=="hk"||p=="mo")return "zh-Hant";
            return "zh-Hans";
        }
        if(supportedLanguage(parts[0]))return parts[0];
    }
    return "en";
}
std::string languageSelection(){std::lock_guard lock(languageMutex);return selection;}
std::string currentLanguage(){std::lock_guard lock(languageMutex);return effective;}
void selectLanguage(const std::string& requested,const std::vector<std::string>& preferences,const std::function<void(const std::string&)>& save){
    const auto selected=supportedLanguage(requested)?requested:"auto";
    const auto resolved=selected=="auto"?resolveLanguage(preferences):selected;
    if(save)save(selected);
    std::lock_guard lock(languageMutex);selection=selected;effective=resolved;
}
std::string translated(const std::string& key,const std::map<std::string,std::string>& arguments,const std::string& language){
    auto locale=generated::catalogs.find(language.empty()?currentLanguage():language);
    const std::string* format=nullptr;
    if(locale!=generated::catalogs.end()){auto found=locale->second.find(key);if(found!=locale->second.end()&&!found->second.empty())format=&found->second;}
    if(!format){auto english=generated::catalogs.find("en");if(english!=generated::catalogs.end()){auto found=english->second.find(key);if(found!=english->second.end())format=&found->second;}}
    if(!format)return key;
    // Only scan the template. A raw value containing braces is never interpreted.
    std::string result;
    for(size_t at=0;at<format->size();){
        if((*format)[at]=='{'){auto end=format->find('}',at+1);if(end!=std::string::npos){auto found=arguments.find(format->substr(at+1,end-at-1));if(found!=arguments.end()){result+=found->second;at=end+1;continue;}}}
        result+=(*format)[at++];
    }
    return result;
}
std::string Message::render()const{std::string result;auto language=currentLanguage();for(const auto& part:parts_)result+=part.key?translated(part.text,part.arguments,language):part.text;return result;}
}
