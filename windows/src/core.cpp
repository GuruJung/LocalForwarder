#include "manager.hpp"
#include "yyjson.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>
#include <regex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <charconv>
#include <string_view>

namespace lf {
namespace {
std::string stringValue(yyjson_val* obj, const char* key, bool required = true) {
    auto* value = yyjson_obj_get(obj, key);
    if (!value || yyjson_is_null(value)) {
        if (!required) return {};
        throw std::runtime_error(std::string("설정 필드가 없습니다: ") + key);
    }
    if (!yyjson_is_str(value)) throw std::runtime_error(std::string("문자열 필드가 아닙니다: ") + key);
    std::string result(yyjson_get_str(value), yyjson_get_len(value));
    if (result.find('\0') != std::string::npos) throw std::runtime_error("문자열에 NUL 문자를 사용할 수 없습니다.");
    return result;
}
// Keep JSON numbers as source tokens so 22.000000000000000001 cannot round to
// an accepted integer. Foundation-compatible integral decimal/exponent forms
// (22.0, 2.2e1) are accepted without floating-point overflow or precision loss.
std::optional<int> exactInteger(yyjson_val* value, int minimum, int maximum) {
    if (!yyjson_is_raw(value)) return {};
    std::string token(yyjson_get_raw(value),yyjson_get_len(value));
    if (token.empty() || token.front()=='-') return {};
    auto exponentAt=token.find_first_of("eE");
    auto mantissa=token.substr(0,exponentAt);
    int64_t exponent=0;
    if (exponentAt!=std::string::npos) {
        size_t i=exponentAt+1; bool negative=false;
        if (token[i]=='-' || token[i]=='+') negative=token[i++]=='-';
        // Larger exponents already prove out-of-range/nonintegral for our bounded fields.
        for (;i<token.size();++i) exponent=std::min<int64_t>(1000000000,exponent*10+(token[i]-'0'));
        if (negative) exponent=-exponent;
    }
    auto dot=mantissa.find('.');
    int64_t shift=exponent-(dot==std::string::npos ? 0 : static_cast<int64_t>(mantissa.size()-dot-1));
    if (dot!=std::string::npos) mantissa.erase(dot,1);
    auto first=mantissa.find_first_not_of('0');
    if (first==std::string::npos) return minimum<=0 && maximum>=0 ? std::optional<int>(0) : std::nullopt;
    mantissa.erase(0,first);
    while (mantissa.back()=='0') {mantissa.pop_back();++shift;}
    if (shift<0 || shift>10 || mantissa.size()>10 || static_cast<int64_t>(mantissa.size())+shift>10) return {};
    int64_t number=0;
    for (char digit:mantissa) {number=number*10+(digit-'0');if(number>maximum)return {};}
    while (shift-->0) {number*=10;if(number>maximum)return {};}
    return number>=minimum ? std::optional<int>(static_cast<int>(number)) : std::nullopt;
}
int numberValue(yyjson_val* obj, const char* key) {
    auto number=exactInteger(yyjson_obj_get(obj,key),1,65535);
    if (!number) throw std::runtime_error(std::string("포트는 1~65535 사이의 정수여야 합니다: ")+key);
    return *number;
}
void field(yyjson_mut_doc* d, yyjson_mut_val* obj, const char* name, const std::string& value) {
    yyjson_mut_obj_add_strcpy(d, obj, name, value.c_str());
}
std::vector<uint32_t> scalars(const std::string& value) {
    std::vector<uint32_t> result;
    for (size_t i=0; i<value.size();) {
        const auto first=static_cast<unsigned char>(value[i++]);
        uint32_t code=first; size_t count=0;
        if (first>=0xc2 && first<=0xdf) { code=first&31; count=1; }
        else if (first>=0xe0 && first<=0xef) { code=first&15; count=2; }
        else if (first>=0xf0 && first<=0xf4) { code=first&7; count=3; }
        else if (first>=0x80) throw std::runtime_error("UTF-8 문자열이 올바르지 않습니다.");
        const auto continuationCount=count;
        if (i+count>value.size()) throw std::runtime_error("UTF-8 문자열이 올바르지 않습니다.");
        while (count--) {
            auto byte=static_cast<unsigned char>(value[i++]);
            if ((byte&0xc0)!=0x80) throw std::runtime_error("UTF-8 문자열이 올바르지 않습니다.");
            code=(code<<6)|(byte&63);
        }
        if ((continuationCount==1 && code<0x80) || (continuationCount==2 && code<0x800) ||
            (continuationCount==3 && code<0x10000) || code>0x10ffff || (code>=0xd800 && code<=0xdfff))
            throw std::runtime_error("UTF-8 문자열이 올바르지 않습니다.");
        result.push_back(code);
    }
    return result;
}
bool whitespace(uint32_t c) {
    return (c>=9 && c<=13) || c==0x20 || c==0x85 || c==0xa0 || c==0x1680 ||
           (c>=0x2000 && c<=0x200a) || c==0x2028 || c==0x2029 || c==0x202f || c==0x205f || c==0x3000;
}
bool control(uint32_t c) {
    if (c<32 || (c>=0x7f && c<=0x9f)) return true;
    // Foundation's controlCharacters comprises Unicode Cc and Cf scalars.
    constexpr std::pair<uint32_t,uint32_t> format[] = {
        {0xad,0xad},{0x600,0x605},{0x61c,0x61c},{0x6dd,0x6dd},{0x70f,0x70f},
        {0x890,0x891},{0x8e2,0x8e2},{0x180e,0x180e},{0x200b,0x200f},
        {0x202a,0x202e},{0x2060,0x2064},{0x2066,0x206f},{0xfeff,0xfeff},
        {0xfff9,0xfffb},{0x110bd,0x110bd},{0x110cd,0x110cd},{0x13430,0x1343f},
        {0x1bca0,0x1bca3},{0x1d173,0x1d17a},{0xe0001,0xe0001},{0xe0020,0xe007f}
    };
    return std::any_of(std::begin(format),std::end(format),[c](auto range){return c>=range.first && c<=range.second;});
}
bool validToken(const std::string& value) {
    if (value.empty() || value.front()=='-') return false;
    auto codes=scalars(value);
    return std::none_of(codes.begin(), codes.end(), [](uint32_t c) {
        return whitespace(c) || control(c) || (c<128 && std::string("\"'\\;$`|&<>").find(static_cast<char>(c))!=std::string::npos);
    });
}
bool boolValue(yyjson_val* object,const char* key) {
    auto* value=yyjson_obj_get(object,key);
    if (!value || yyjson_is_null(value)) return true;
    if (!yyjson_is_bool(value)) throw std::runtime_error(std::string("boolean 필드가 아닙니다: ")+key);
    return yyjson_get_bool(value);
}
std::vector<std::string> transport(const Host& host) {
    std::vector<std::string> args;
    if (!host.user.empty()) { args.push_back("-l"); args.push_back(host.user); }
    if (host.port) { args.push_back("-p"); args.push_back(std::to_string(*host.port)); }
    if (!host.identityFile.empty()) { args.push_back("-i"); args.push_back(host.identityFile); }
    for (auto option : {"ControlMaster=no", "ControlPath=none", "ControlPersist=no", "ForkAfterAuthentication=no", "RemoteCommand=none", "RequestTTY=no", "PermitLocalCommand=no", "ConnectTimeout=10", "ConnectionAttempts=1"}) {
        args.emplace_back("-o"); args.emplace_back(option);
    }
    return args;
}
std::vector<std::string> settingLines(const std::string& input, bool forwards) {
    std::istringstream stream(input);
    std::string line; std::vector<std::string> result;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(' ');
        if (pos == std::string::npos || line.substr(pos + 1) == "none") continue;
        auto key = line.substr(0, pos);
        if (forwards ? (key == "localforward" || key == "remoteforward" || key == "dynamicforward") : key == "identityfile") result.push_back(forwards ? line : line.substr(pos + 1));
    }
    return result;
}
}

bool safeId(const std::string& value) {
    return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
}
void validateForward(const Forward& f) {
    if (!safeId(f.id)) throw std::runtime_error("규칙 ID가 올바르지 않습니다.");
    if (f.localPort < 1 || f.localPort > 65535 || f.remotePort < 1 || f.remotePort > 65535) throw std::runtime_error("포트는 1~65535 사이여야 합니다.");
    if (!validToken(f.remoteHost)) throw std::runtime_error("목적지 호스트가 올바르지 않습니다.");
}
bool sameExecutionSettings(const Host& left, const Host& right) {
    auto a=left,b=right;a.description.clear();b.description.clear();return a==b;
}
void validate(const Host& h) {
    if (!safeId(h.id)) throw std::runtime_error("호스트 ID가 올바르지 않습니다.");
    auto descriptionScalars=scalars(h.description);
    if (std::any_of(descriptionScalars.begin(),descriptionScalars.end(),[](auto c){return c==0 || c=='\r' || c=='\n' || c==0x85 || c==0x2028 || c==0x2029;}))
        throw std::runtime_error("설명은 개행과 NUL이 없는 한 줄이어야 합니다.");
    auto nameScalars=scalars(h.name);
    if (std::all_of(nameScalars.begin(),nameScalars.end(),whitespace)) throw std::runtime_error("이름을 입력하세요.");
    if (h.name.find('\0')!=std::string::npos) throw std::runtime_error("이름에 NUL 문자를 사용할 수 없습니다.");
    if (!validToken(h.target)) throw std::runtime_error("SSH 별칭 또는 호스트 주소를 입력하세요. 공백과 셸 제어문자는 허용하지 않습니다.");
    if (!h.user.empty() && !validToken(h.user)) throw std::runtime_error("사용자명이 올바르지 않습니다.");
    if (h.port && (*h.port < 1 || *h.port > 65535)) throw std::runtime_error("SSH 포트는 1~65535 사이여야 합니다.");
    auto keyScalars=scalars(h.identityFile);
    if (std::any_of(keyScalars.begin(),keyScalars.end(),control)) throw std::runtime_error("개인키 경로에 제어문자를 사용할 수 없습니다.");
    std::set<int> ports; std::set<std::string> ids;
    for (auto& f : h.forwards) {
        validateForward(f);
        if (!safeId(f.id) || !ids.insert(f.id).second) throw std::runtime_error("규칙 ID가 올바르지 않거나 중복됩니다.");
        if (f.localPort < 1 || f.localPort > 65535 || f.remotePort < 1 || f.remotePort > 65535) throw std::runtime_error("포트는 1~65535 사이여야 합니다.");
        if (!validToken(f.remoteHost)) throw std::runtime_error("목적지 호스트가 올바르지 않습니다.");
        if (f.enabled && !ports.insert(f.localPort).second) throw std::runtime_error("활성 규칙의 로컬 포트가 중복됩니다.");
    }
}
Config parseConfig(const std::string& text) {
    yyjson_read_err error{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_NUMBER_AS_RAW, nullptr, &error), yyjson_doc_free);
    if (!doc) throw std::runtime_error("설정 JSON을 읽을 수 없습니다: " + std::string(error.msg ? error.msg : "invalid JSON"));
    auto* root = yyjson_doc_get_root(doc.get());
    if (!yyjson_is_obj(root)) throw std::runtime_error("설정은 JSON 객체여야 합니다.");
    auto version = exactInteger(yyjson_obj_get(root, "version"),1,2);
    if (!version) throw std::runtime_error("지원하지 않는 설정 버전입니다.");
    auto* hosts = yyjson_obj_get(root, "hosts");
    if (!yyjson_is_arr(hosts)) throw std::runtime_error("hosts 배열이 없습니다.");
    Config config; config.version=*version; std::set<std::string> ids;
    size_t i, max; yyjson_val* value;
    yyjson_arr_foreach(hosts, i, max, value) {
        if (!yyjson_is_obj(value)) throw std::runtime_error("호스트 설정은 객체여야 합니다.");
        Host h;
        h.id = stringValue(value, "id"); h.name = stringValue(value, "name"); h.target = stringValue(value, "target");
        h.description = stringValue(value, "description", false);
        h.user = stringValue(value, "user", false); h.identityFile = stringValue(value, "identityFile", false);
        for (auto key : {"user", "identityFile"}) {
            auto* optional=yyjson_obj_get(value,key);
            if (optional && yyjson_is_str(optional) && yyjson_get_len(optional)==0)
                throw std::runtime_error(std::string("빈 선택 필드를 생략하거나 null로 지정하세요: ")+key);
        }
        h.autoConnect=boolValue(value,"autoConnect");
        h.exitOnForwardFailure=boolValue(value,"exitOnForwardFailure");
        h.retryOnConnectionRefused=boolValue(value,"retryOnConnectionRefused");
        if (auto* p = yyjson_obj_get(value, "port"); p && !yyjson_is_null(p)) h.port = numberValue(value, "port");
        auto* forwards = yyjson_obj_get(value, "forwards");
        if (!yyjson_is_arr(forwards)) throw std::runtime_error("forwards 배열이 없습니다.");
        size_t j, count; yyjson_val* entry;
        yyjson_arr_foreach(forwards, j, count, entry) {
            if (!yyjson_is_obj(entry)) throw std::runtime_error("포워딩 규칙은 객체여야 합니다.");
            Forward f;
            f.id = stringValue(entry, "id"); f.remoteHost = stringValue(entry, "remoteHost");
            f.localPort = numberValue(entry, "localPort"); f.remotePort = numberValue(entry, "remotePort");
            auto* enabled = yyjson_obj_get(entry, "enabled");
            if (!yyjson_is_bool(enabled)) throw std::runtime_error("enabled 필드는 boolean이어야 합니다.");
            f.enabled = yyjson_get_bool(enabled); h.forwards.push_back(f);
        }
        validate(h);
        if (!ids.insert(h.id).second) throw std::runtime_error("호스트 ID가 중복됩니다.");
        config.hosts.push_back(h);
    }
    return config;
}
std::string writeConfig(const Config& config) {
    std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> doc(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
    if (!doc) throw std::bad_alloc();
    auto* root = yyjson_mut_obj(doc.get()); yyjson_mut_doc_set_root(doc.get(), root);
    yyjson_mut_obj_add_int(doc.get(), root, "version", 2);
    auto* hosts = yyjson_mut_arr(doc.get()); yyjson_mut_obj_add_val(doc.get(), root, "hosts", hosts);
    std::set<std::string> ids;
    for (auto& h : config.hosts) {
        validate(h);
        if (!ids.insert(h.id).second) throw std::runtime_error("호스트 ID가 중복됩니다.");
        auto* obj = yyjson_mut_obj(doc.get()); yyjson_mut_arr_add_val(hosts, obj);
        field(doc.get(), obj, "id", h.id); field(doc.get(), obj, "name", h.name); field(doc.get(), obj, "target", h.target);
        yyjson_mut_obj_add_bool(doc.get(),obj,"autoConnect",h.autoConnect);
        yyjson_mut_obj_add_bool(doc.get(),obj,"exitOnForwardFailure",h.exitOnForwardFailure);
        yyjson_mut_obj_add_bool(doc.get(),obj,"retryOnConnectionRefused",h.retryOnConnectionRefused);
        if (!h.description.empty()) field(doc.get(), obj, "description", h.description);
        if (!h.user.empty()) field(doc.get(), obj, "user", h.user);
        if (h.port) yyjson_mut_obj_add_int(doc.get(), obj, "port", *h.port);
        if (!h.identityFile.empty()) field(doc.get(), obj, "identityFile", h.identityFile);
        auto* forwards = yyjson_mut_arr(doc.get()); yyjson_mut_obj_add_val(doc.get(), obj, "forwards", forwards);
        for (auto& f : h.forwards) {
            auto* rule = yyjson_mut_obj(doc.get()); yyjson_mut_arr_add_val(forwards, rule);
            field(doc.get(), rule, "id", f.id); field(doc.get(), rule, "remoteHost", f.remoteHost);
            yyjson_mut_obj_add_bool(doc.get(), rule, "enabled", f.enabled);
            yyjson_mut_obj_add_int(doc.get(), rule, "localPort", f.localPort);
            yyjson_mut_obj_add_int(doc.get(), rule, "remotePort", f.remotePort);
        }
    }
    size_t size = 0; char* bytes = yyjson_mut_write(doc.get(), YYJSON_WRITE_PRETTY, &size);
    if (!bytes) throw std::runtime_error("설정을 직렬화할 수 없습니다.");
    std::string result(bytes, size); free(bytes); return result + "\n";
}
std::vector<std::string> sshArguments(const Host& h, const std::string& configFile, bool query) {
    validate(h); auto args = transport(h);
    args.emplace_back("-o"); args.emplace_back("ClearAllForwardings=no");
    if (!configFile.empty()) { args.emplace_back("-F"); args.push_back(configFile); }
    args.emplace_back("-o"); args.emplace_back(h.exitOnForwardFailure ? "ExitOnForwardFailure=yes" : "ExitOnForwardFailure=no");
    if (query) { args.emplace_back("-G"); args.emplace_back("-N"); args.emplace_back("-T"); args.emplace_back("-o"); args.emplace_back("BatchMode=yes"); }
    else {
        for (auto flag : {"-v", "-N", "-n", "-T"}) args.emplace_back(flag);
        // Match functions because Windows __FILE__ can contain full build paths.
        // Limit channel/loop logging to lifecycle events, avoiding per-packet DEBUG3 traffic.
        for (auto option : {"BatchMode=yes", "StrictHostKeyChecking=yes", "ServerAliveInterval=15", "ServerAliveCountMax=3", "LogVerbose=*:main():*,*:ssh_connect_direct():*,*:ssh_userauth2():*,*:kex_exchange_identification():*,*:client_loop():*,*:ssh_confirm_remote_forward():*,*:channel_setup_fwd_listener_*:*,*:channel_input_open_failure():*,*:channel_new():*"}) { args.emplace_back("-o"); args.emplace_back(option); }
        for (auto& f : h.forwards) if (f.enabled) {
            args.emplace_back("-L"); args.push_back(forwarding(f));
        }
    }
    args.emplace_back("--"); args.push_back(h.target); return args;
}
std::vector<std::string> authArguments(const Host& h, bool batch) {
    validate(h); auto args = transport(h);
    for (auto option : {"SessionType=default", "ClearAllForwardings=yes", "ExitOnForwardFailure=yes"}) { args.emplace_back("-o"); args.emplace_back(option); }
    args.emplace_back("-o"); args.emplace_back(batch ? "BatchMode=yes" : "BatchMode=no");
    args.emplace_back("-o"); args.emplace_back(batch ? "StrictHostKeyChecking=yes" : "StrictHostKeyChecking=ask");
    args.emplace_back("-T"); args.emplace_back("--"); args.push_back(h.target); return args;
}
std::vector<std::string> inheritedForwards(const std::string& e) { return settingLines(e, true); }
std::vector<std::string> identityFiles(const std::string& e) { return settingLines(e, false); }
int remoteForwardCount(const std::string& e) {
    auto lines = inheritedForwards(e);
    return static_cast<int>(std::count_if(lines.begin(), lines.end(), [](auto& s) { return s.rfind("remoteforward ", 0) == 0; }));
}
std::wstring quoteWindowsArgument(const std::wstring& value) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { out.append(slashes * 2 + 1, L'\\'); out += c; }
        else { out.append(slashes, L'\\'); out += c; }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\'); out += L'\"'; return out;
}
std::string quotePosix(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1,c);
    return out + "'";
}
std::wstring quotePowerShell(const std::wstring& value) {
    std::wstring out = L"'";
    for (wchar_t c : value) out += c == L'\'' ? L"''" : std::wstring(1,c);
    return out + L"'";
}
std::string registrationCommand(const std::string& publicKey) {
    if (publicKey.find_first_of("\r\n") != std::string::npos || (publicKey.rfind("ssh-",0) != 0 && publicKey.rfind("ecdsa-sha2-",0) != 0)) throw std::runtime_error("공개키 형식이 올바르지 않습니다.");
    return "sh -c " + quotePosix("set -eu; umask 077; mkdir -p \"$HOME/.ssh\"; chmod 700 \"$HOME/.ssh\"; touch \"$HOME/.ssh/authorized_keys\"; chmod 600 \"$HOME/.ssh/authorized_keys\"; key=" + quotePosix(publicKey) + "; keytype=$(printf '%s\\n' \"$key\" | cut -d ' ' -f1); keydata=$(printf '%s\\n' \"$key\" | cut -d ' ' -f2); if ! awk -v k=\"$keydata\" '{for(i=1;i<=NF;i++) if($i==k) found=1} END{exit !found}' \"$HOME/.ssh/authorized_keys\"; then printf '\\n%s\\n' \"$key\" >> \"$HOME/.ssh/authorized_keys\"; fi");
}
namespace {
std::string lower(std::string value) {
    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return value;
}
bool containsAny(const std::string& value,std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(),needles.end(),[&](auto needle){return value.find(needle)!=std::string::npos;});
}
std::optional<int> portNumber(const std::string& value) {
    if(value.empty() || value.size()>5 || !std::all_of(value.begin(),value.end(),[](unsigned char c){return c>='0' && c<='9';})) return {};
    int port=std::stoi(value); return port<=65535 ? std::optional<int>(port) : std::nullopt;
}
std::string nowISO() {
    auto now=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm,&now);
#else
    gmtime_r(&now,&tm);
#endif
    std::ostringstream out; out<<std::put_time(&tm,"%Y-%m-%dT%H:%M:%SZ"); return out.str();
}
}
std::string forwarding(const Forward& rule) {
    auto destination=rule.remoteHost;
    if(destination.find(':')!=std::string::npos && !destination.starts_with('[')) destination='['+destination+']';
    return "127.0.0.1:"+std::to_string(rule.localPort)+':'+destination+':'+std::to_string(rule.remotePort);
}
bool authenticationFailure(const std::string& text) {
    return containsAny(lower(text),{"permission denied","host key verification failed","remote host identification has changed",
        "no such identity","sign_and_send_pubkey","load key","passphrase"});
}
bool isConnectionRefusal(const std::string& targetDiagnostic,const std::string& auxiliaryDiagnostic) {
    auto auxiliary=lower(auxiliaryDiagnostic);
    if(authenticationFailure(auxiliary) || containsAny(auxiliary,{"could not resolve hostname","name or service not known",
        "network is unreachable","no route to host","timed out","timeout, server ","bad configuration","address already in use",
        "remote port forwarding failed","could not request local forwarding","only one usage of each socket address"})) return false;
    auto text=lower(targetDiagnostic);
    // Forced DEBUG3 includes remaining-time countdowns and channel inactivity
    // settings. Only timeout failure messages veto a proven pre-auth refusal.
    if(authenticationFailure(text) || containsAny(text,{"authenticated to ","authentication succeeded","address already in use",
        "forwarding failed","could not request","cannot listen","open failed:","connect failed:","timed out","timeout, server ",
        "only one usage of each socket address"})) return false;
    std::istringstream stream(text); std::string line;
    while(std::getline(stream,line)) {
        if((line.find("connect to host ")!=std::string::npos && line.find("connection refused")!=std::string::npos) ||
           (containsAny(line,{"kex_exchange_identification:","ssh_exchange_identification:"}) && containsAny(line,{"connection closed","connection reset"})) ||
           line.starts_with("connection closed by ") || line.starts_with("connection reset by ")) return true;
    }
    return false;
}
std::string describeFailure(const std::string& text) {
    if(containsAny(lower(text),{"address already in use","cannot listen to port","only one usage of each socket address"}))
        return "다른 프로그램이 해당 로컬 포트를 사용 중이거나 포트를 열 수 없습니다.\n"+text;
    return text.empty() ? "SSH 연결을 완료하지 못했습니다." : text;
}
namespace {
bool asciiDigit(char c) { return c>='0' && c<='9'; }
bool identifierFirst(char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_'; }
bool identifierRest(char c) { return identifierFirst(c) || asciiDigit(c); }
std::optional<uint32_t> unsignedDecimal(std::string_view text, bool positive=false) {
    if(text.empty() || text.size()>10 || !std::all_of(text.begin(),text.end(),asciiDigit)) return {};
    if(text.size()>1 && text.front()=='0') return {};
    uint32_t number=0; auto result=std::from_chars(text.data(),text.data()+text.size(),number);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size() || (positive && number==0)) return {};
    return number;
}
std::string withoutSeverity(const std::string& input) {
    std::string_view text(input);
    for(std::string_view prefix:{"debug1: ","debug2: ","debug3: ","error: ","fatal: ","internal error: "}) {
        if(text.starts_with(prefix)) {text.remove_prefix(prefix.size());break;}
    }
    return std::string(text);
}
bool consumePrefix(std::string_view& text,std::string_view prefix) {
    if(!text.starts_with(prefix)) return false;
    text.remove_prefix(prefix.size());return true;
}
}
bool supportedOpenSSHVersion(const std::string& output) {
    std::string_view text(output);
    if(text.ends_with("\n")) text.remove_suffix(1);
    if(text.ends_with("\r")) text.remove_suffix(1);
    if(text.find_first_of("\r\n")!=std::string_view::npos || text.find('\0')!=std::string_view::npos) return false;
    if(!consumePrefix(text,"OpenSSH_for_Windows_") && !consumePrefix(text,"OpenSSH_")) return false;
    auto dot=text.find('.'); if(dot==std::string_view::npos) return false;
    auto major=unsignedDecimal(text.substr(0,dot),true);text.remove_prefix(dot+1);
    size_t count=0;while(count<text.size() && asciiDigit(text[count])) ++count;
    auto minor=unsignedDecimal(text.substr(0,count));text.remove_prefix(count);
    if(!major || !minor) return false;
    if(consumePrefix(text,"p")) {
        count=0;while(count<text.size() && asciiDigit(text[count])) ++count;
        auto patch=unsignedDecimal(text.substr(0,count));text.remove_prefix(count);
        if(!patch) return false;
    }
    if(!text.empty()) {
        if(!consumePrefix(text,", ") || text.empty()) return false;
        if(std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32 || c==127;})) return false;
    }
    return *major>9 || (*major==9 && *minor>=5);
}
std::optional<std::string> targetDiagnosticLine(const std::string& line,uint32_t expectedPid) {
    if(expectedPid==0) return {};
    std::string_view text(line),severity;
    if(text.ends_with("\r")) text.remove_suffix(1);
    if(text.find_first_of("\r\n")!=std::string_view::npos || text.find('\0')!=std::string_view::npos) return {};
    for(std::string_view prefix:{"debug1: ","debug2: ","debug3: ","error: ","fatal: ","internal error: "}) {
        if(text.starts_with(prefix)) {severity=prefix;text.remove_prefix(prefix.size());break;}
    }
    // OpenSSH bounds the source/function tag fields to 48 characters. Locate
    // the function delimiter after the source, which may include a drive colon.
    auto functionEnd=text.find("():"); if(functionEnd==std::string_view::npos) return {};
    auto sourceEnd=text.rfind(':',functionEnd); if(sourceEnd==std::string_view::npos) return {};
    auto encodedSource=text.substr(0,sourceEnd);
    // log.c truncates __FILE__ before strnvis doubles Windows backslashes.
    // Bound both representations and decode only paired backslashes, preserving
    // older/plain source paths and never interpreting escapes in message bodies.
    if(encodedSource.empty() || encodedSource.size()>96) return {};
    std::string decodedSource;
    for(size_t i=0;i<encodedSource.size();++i) {
        decodedSource+=encodedSource[i];
        if(encodedSource[i]=='\\' && i+1<encodedSource.size() && encodedSource[i+1]=='\\') ++i;
    }
    std::string_view source(decodedSource);
    if(source.size()>48 || (!source.ends_with(".c") && source.size()!=48)) return {};
    bool drive=source.size()>=3 && ((source[0]>='a' && source[0]<='z') || (source[0]>='A' && source[0]<='Z')) &&
        source[1]==':' && (source[2]=='\\' || source[2]=='/');
    bool rooted=drive || source.starts_with("/") || source.starts_with("\\") ||
        source.starts_with("./") || source.starts_with(".\\") || source.starts_with("../") || source.starts_with("..\\");
    for(size_t i=0;i<source.size();++i) {
        char c=source[i];
        if(identifierRest(c) || c=='.' || c=='-' || c=='/' || c=='\\') continue;
        if(c==':' && drive && i==1) continue;
        if(c==' ' && rooted) continue;
        return {};
    }
    auto function=text.substr(sourceEnd+1,functionEnd-sourceEnd-1);
    if(function.empty() || function.size()>48 || !identifierFirst(function.front()) || !std::all_of(function.begin(),function.end(),identifierRest)) return {};
    text.remove_prefix(functionEnd+3);
    auto lineEnd=text.find(' '); if(lineEnd==std::string_view::npos || !unsignedDecimal(text.substr(0,lineEnd),true)) return {};
    text.remove_prefix(lineEnd+1);
    if(!consumePrefix(text,"(pid=") && !consumePrefix(text,"(bin=ssh, pid=") &&
       !consumePrefix(text,"(bin=ssh.exe, pid=")) return {};
    auto pidEnd=text.find("): "); if(pidEnd==std::string_view::npos) return {};
    auto pid=unsignedDecimal(text.substr(0,pidEnd),true);
    if(!pid || *pid!=expectedPid) return {};
    text.remove_prefix(pidEnd+3);
    // Kex logs echo arbitrary peer banners/software strings. Preserve those as
    // auxiliary diagnostics, never as target state evidence. Only canonical
    // transport errors may leave this function on the target channel.
    if(function=="kex_exchange_identification") {
        if(!severity.empty() && severity!="error: " && severity!="fatal: ") return {};
        consumePrefix(text,"kex_exchange_identification: ");
        if(!text.starts_with("read: ") && !text.starts_with("Connection closed by ") && !text.starts_with("Connection reset by ")) return {};
        return std::string(severity)+"kex_exchange_identification: "+std::string(text);
    }
    return std::string(severity)+std::string(text);
}
Readiness::Readiness(int expected,bool requireForwards,std::string expectedTarget,int expectedPort)
    :expectedTarget_(std::move(expectedTarget)),remoteExpected_(expected),expectedPort_(expectedPort),requireForwards_(requireForwards) {}
void Readiness::consume(const std::string& input) {
    auto line=withoutSeverity(input); if(!line.empty() && line.back()=='\r') line.pop_back();
    static const std::regex stdioChannel(R"(^channel [0-9]+: new stdio-forward \[stdio-forward\])");
    if(line.starts_with("channel_connect_stdio_fwd:") || std::regex_search(line,stdioChannel)) {
        targetScope_=authenticated_=entered_=localListening_=false; remoteConfirmed_=0; return;
    }
    if(line.starts_with("Authenticated to ") || line.starts_with("Authentication succeeded")) {
        targetScope_=expectedTarget_.empty() || line.starts_with("Authenticated to "+expectedTarget_+" ") || line=="Authenticated to "+expectedTarget_;
        if(expectedPort_ && line.find("]:")!=std::string::npos)
            targetScope_=targetScope_ && line.find("]:"+std::to_string(expectedPort_)+")")!=std::string::npos;
        authenticated_=targetScope_; entered_=localListening_=false; remoteConfirmed_=0;
    }
    if(!targetScope_) return;
    if(line.starts_with("Local forwarding listening on ")) localListening_=true;
    if(line=="Entering interactive session.") entered_=true;
    if(line.starts_with("remote forward success for:")) ++remoteConfirmed_;
    if(line.starts_with("Could not request local forwarding") || line.starts_with("remote port forwarding failed") ||
       line.starts_with("Could not request remote forwarding") || line.starts_with("cannot listen to port")) failed_=true;
}
bool Readiness::feed(const std::string& chunk) {
    pending_+=chunk;
    for(size_t n;(n=pending_.find('\n'))!=std::string::npos;) {
        consume(pending_.substr(0,n)); pending_.erase(0,n+1);
    }
    if(pending_.size()>65536) pending_.erase(0,pending_.size()-65536);
    return ready();
}
bool Readiness::ready() const {
    bool forwardsReady=remoteExpected_>0 ? remoteConfirmed_>=remoteExpected_ : localListening_;
    return authenticated_ && entered_ && (!requireForwards_ || (forwardsReady && !failed_));
}
const char* stateName(ConnectionState state) {
    switch(state) {
        case ConnectionState::disconnected:return "disconnected";case ConnectionState::connecting:return "connecting";
        case ConnectionState::connected:return "connected";case ConnectionState::stopping:return "stopping";
    }
    return "disconnected";
}
const char* forwardStateName(ForwardState state) {
    switch(state) {
        case ForwardState::preparing:return "preparing";case ForwardState::ready:return "ready";case ForwardState::failed:return "failed";
        case ForwardState::disconnected:return "disconnected";case ForwardState::unknown:return "unknown";
    }
    return "unknown";
}
std::string HostRuntime::summary() const {
    std::string title;
    switch(state) {
        case ConnectionState::disconnected:title="연결 끊김";break;case ConnectionState::connecting:title="연결 중";break;
        case ConnectionState::connected:title="SSH 연결됨";break;case ConnectionState::stopping:title="정리 중";break;
    }
    auto count=[&](ForwardState s){return std::count_if(forwards.begin(),forwards.end(),[s](auto& f){return f.state==s;});};
    auto unknown=count(ForwardState::unknown);
    return title+" · 포워딩 준비 "+std::to_string(count(ForwardState::ready))+" / 실패 "+std::to_string(count(ForwardState::failed))+
        (unknown ? " / 확인 불가 "+std::to_string(unknown) : "")+(temporary ? " · 임시 실행 설정" : "");
}
ForwardTracker::ForwardTracker(const Host& host,const std::vector<std::string>& inherited) {
    for(const auto& rule:host.forwards) if(rule.enabled) {
        forwards.push_back({rule.id,forwarding(rule),"앱 설정",{},ForwardState::preparing});
        bindings_.push_back({false,rule.localPort,"127.0.0.1"});
    }
    for(size_t i=0;i<inherited.size();++i) {
        std::istringstream stream(inherited[i]); std::string kind,endpoint; stream>>kind>>endpoint;
        auto colon=endpoint.rfind(':'); auto port=portNumber(colon==std::string::npos ? endpoint : endpoint.substr(colon+1));
        std::optional<std::string> address;
        if(colon!=std::string::npos) { address=endpoint.substr(0,colon); std::erase(*address,'['); std::erase(*address,']'); }
        forwards.push_back({"ssh-config-"+std::to_string(i),inherited[i],"SSH 설정",port ? "" : "이 규칙의 개설 결과를 SSH 진단에서 식별할 수 없습니다.",ForwardState::preparing});
        bindings_.push_back({kind=="remoteforward",port,address});
    }
}
void ForwardTracker::mark(bool remote,int port,std::optional<std::string> address,ForwardState state,const std::string& line) {
    std::vector<size_t> candidates;
    for(size_t i=0;i<bindings_.size();++i) {
        auto& b=bindings_[i];
        if(b.remote==remote && b.port==port && (!address || !b.address || b.address==address || *b.address=="*" || b.address->empty() ||
            (*b.address=="localhost" && (*address=="127.0.0.1" || *address=="::1")))) candidates.push_back(i);
    }
    if(candidates.size()!=1) {
        for(auto i:candidates) if(forwards[i].state==ForwardState::preparing) forwards[i].detail="같은 리스너를 사용하는 규칙들의 개별 결과를 식별할 수 없습니다.";
        return;
    }
    auto index=candidates[0]; auto& f=forwards[index];
    if(!remote) {
        auto& established=establishedAddresses_[index];
        if(state==ForwardState::ready) established.insert(address.value_or(""));
        else if(state==ForwardState::failed) { if(address) established.erase(*address); else established.clear(); }
        if(!established.empty()) {
            f.state=ForwardState::ready;
            f.detail=state==ForwardState::failed ? "일부 주소의 개설 실패: "+line : "포워딩 개설 성공 · 목적지 서비스 상태는 검사하지 않음";
            return;
        }
    }
    f.state=state; f.detail=state==ForwardState::ready ? "포워딩 개설 성공 · 목적지 서비스 상태는 검사하지 않음" : line;
}
void ForwardTracker::consume(const std::string& input,const std::string& timestamp) {
    const auto line=withoutSeverity(input);
    static const std::regex listening(R"(^Local forwarding listening on (.+) port ([0-9]+)\.)");
    static const std::regex bindFailure(R"(^(?:bind|listen) \[([^\]]+)\]:([0-9]+):)");
    static const std::regex cannotListen(R"(^cannot listen to port: ([0-9]+))");
    static const std::regex remotePort(R"(^remote forward (?:success|failure) for: listen (?:.*:)?([0-9]+),)");
    static const std::regex remoteFailure(R"(^remote port forwarding failed for listen port ([0-9]+))");
    static const std::regex created(R"(^channel [0-9]+: new port-listener \[port listener\])");
    static const std::regex destinationFailure(R"(^channel [0-9]+: open failed:)");
    std::smatch match;
    if(std::regex_search(line,match,listening)) {
        if(auto port=portNumber(match[2])) pendingListener_=std::pair{match[1].str(),*port}; else pendingListener_.reset();
    } else if(std::regex_search(line,created) && pendingListener_) {
        mark(false,pendingListener_->second,pendingListener_->first,ForwardState::ready,line); pendingListener_.reset();
    } else if(std::regex_search(line,match,bindFailure)) {
        if(auto port=portNumber(match[2])) mark(false,*port,match[1].str(),ForwardState::failed,line);
        pendingListener_.reset();
    } else if(std::regex_search(line,match,cannotListen)) {
        if(auto port=portNumber(match[1])) mark(false,*port,{},ForwardState::failed,line);
        pendingListener_.reset();
    } else if(line.starts_with("remote forward success for:") && std::regex_search(line,match,remotePort)) {
        if(auto port=portNumber(match[1])) mark(true,*port,{},ForwardState::ready,line);
    } else if(std::regex_search(line,match,remoteFailure)) {
        if(auto port=portNumber(match[1])) mark(true,*port,{},ForwardState::failed,line);
    } else if(line.starts_with("remote forward failure for:") && std::regex_search(line,match,remotePort)) {
        if(auto port=portNumber(match[1])) mark(true,*port,{},ForwardState::failed,line);
    }
    if(std::regex_search(line,destinationFailure) || line.starts_with("connect failed:")) {
        size_t begin=input.size()>2048 ? input.size()-2048 : 0;
        while(begin<input.size() && (static_cast<unsigned char>(input[begin])&0xc0)==0x80) ++begin;
        recentErrors.push_back({timestamp.empty() ? nowISO() : timestamp,input.substr(begin)});
        if(recentErrors.size()>10) recentErrors.erase(recentErrors.begin(),recentErrors.end()-10);
    }
}
void ForwardTracker::sessionReady() {
    pendingListener_.reset();
    for(auto& f:forwards) if(f.state==ForwardState::preparing) {
        f.state=ForwardState::unknown;
        if(f.detail.empty()) f.detail="SSH 연결은 성립했지만 이 규칙의 개설 결과는 아직 확인되지 않았습니다.";
    }
}
void ForwardTracker::disconnected() {
    pendingListener_.reset(); establishedAddresses_.clear();
    for(auto& f:forwards) f.state=ForwardState::disconnected;
}
}
