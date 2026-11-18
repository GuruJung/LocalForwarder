#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lf {
struct LanguageChoice { std::string id, title; };
const std::vector<LanguageChoice>& languages();
bool supportedLanguage(const std::string& id);
std::string resolveLanguage(const std::vector<std::string>& preferences);
std::string languageSelection();
std::string currentLanguage();
// Selection is updated only after persistence succeeds. Tests inject both adapters.
void selectLanguage(const std::string& selection,const std::vector<std::string>& preferences,
                    const std::function<void(const std::string&)>& save={});
std::string translated(const std::string& key,const std::map<std::string,std::string>& arguments={},const std::string& language={});

// Raw diagnostic/user data and translated templates remain separate across language changes.
class Message {
public:
    struct Part { std::string text; bool key=false; std::map<std::string,std::string> arguments; bool operator==(const Part&) const=default; };
private:
    std::vector<Part> parts_;
public:
    Message()=default;
    Message(const char* raw):Message(std::string(raw)){}
    Message(std::string raw){if(!raw.empty())parts_.push_back({std::move(raw),false,{}});}
    static Message localized(std::string key,std::map<std::string,std::string> args={}){Message value;value.parts_.push_back({std::move(key),true,std::move(args)});return value;}
    std::string render()const;
    operator std::string()const{return render();}
    bool empty()const{return parts_.empty();}
    void clear(){parts_.clear();}
    size_t find(const std::string& value)const{return render().find(value);}
    Message& operator+=(const Message& other){parts_.insert(parts_.end(),other.parts_.begin(),other.parts_.end());return *this;}
    bool operator==(const Message&)const=default;
    friend Message operator+(Message left,const Message& right){left+=right;return left;}
    friend std::ostream& operator<<(std::ostream& out,const Message& value){return out<<value.render();}
};
inline Message tr(const std::string& key,std::map<std::string,std::string> args={}){return Message::localized(key,std::move(args));}
inline Message msg(const std::string& key,std::map<std::string,std::string> args={}){return tr(key,std::move(args));}
class LocalizedError:public std::runtime_error {
    Message message_;
public:
    explicit LocalizedError(Message message):std::runtime_error(message.render()),message_(std::move(message)){}
    const Message& message()const{return message_;}
};
inline Message messageFrom(const std::exception& error){if(auto localized=dynamic_cast<const LocalizedError*>(&error))return localized->message();return Message(error.what());}
#ifdef _WIN32
// Stable immutable pointers, including across language changes, for Win32 dialog APIs.
const wchar_t* tw(const std::string& key);
std::string wideTranslationKey(const wchar_t* text);
std::vector<std::string> systemUILanguages();
void initializeLanguage();
std::string readLanguagePreference(const std::wstring& subkey);
void writeLanguagePreference(const std::wstring& subkey,const std::string& selection);
void persistLanguage(const std::string& selection);
#endif
}
