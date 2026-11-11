#include "core.hpp"
#include "yyjson.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>

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
int numberValue(yyjson_val* obj, const char* key) {
    auto* value = yyjson_obj_get(obj, key);
    if (!yyjson_is_int(value)) throw std::runtime_error(std::string("정수 필드가 아닙니다: ") + key);
    auto n = yyjson_get_sint(value);
    if (n < 1 || n > 65535) throw std::runtime_error("포트는 1~65535 사이여야 합니다.");
    return static_cast<int>(n);
}
void field(yyjson_mut_doc* d, yyjson_mut_val* obj, const char* name, const std::string& value) {
    yyjson_mut_obj_add_strcpy(d, obj, name, value.c_str());
}
bool validToken(const std::string& value) {
    if (value.empty() || value.front() == '-') return false;
    return std::none_of(value.begin(), value.end(), [](unsigned char c) {
        return c <= 32 || c == 127 || c == '"' || c == '\'' || c == '\\' || c == ';' || c == '$' || c == '`' || c == '|' || c == '&' || c == '<' || c == '>';
    });
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
void validate(const Host& h) {
    if (!safeId(h.id)) throw std::runtime_error("호스트 ID가 올바르지 않습니다.");
    if (h.name.empty()) throw std::runtime_error("이름을 입력하세요.");
    if (!validToken(h.target)) throw std::runtime_error("SSH 별칭 또는 호스트 주소를 입력하세요. 공백과 셸 제어문자는 허용하지 않습니다.");
    if (!h.user.empty() && !validToken(h.user)) throw std::runtime_error("사용자명이 올바르지 않습니다.");
    if (h.port && (*h.port < 1 || *h.port > 65535)) throw std::runtime_error("SSH 포트는 1~65535 사이여야 합니다.");
    if (h.identityFile.find_first_of("\r\n") != std::string::npos) throw std::runtime_error("키 파일 경로에 줄바꿈을 사용할 수 없습니다.");
    std::set<int> ports; std::set<std::string> ids;
    for (auto& f : h.forwards) {
        if (!safeId(f.id) || !ids.insert(f.id).second) throw std::runtime_error("규칙 ID가 올바르지 않거나 중복됩니다.");
        if (f.localPort < 1 || f.localPort > 65535 || f.remotePort < 1 || f.remotePort > 65535) throw std::runtime_error("포트는 1~65535 사이여야 합니다.");
        if (!validToken(f.remoteHost)) throw std::runtime_error("목적지 호스트가 올바르지 않습니다.");
        if (f.enabled && !ports.insert(f.localPort).second) throw std::runtime_error("활성 규칙의 로컬 포트가 중복됩니다.");
    }
}
Config parseConfig(const std::string& text) {
    yyjson_read_err error{};
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), 0, nullptr, &error), yyjson_doc_free);
    if (!doc) throw std::runtime_error("설정 JSON을 읽을 수 없습니다: " + std::string(error.msg ? error.msg : "invalid JSON"));
    auto* root = yyjson_doc_get_root(doc.get());
    if (!yyjson_is_obj(root)) throw std::runtime_error("설정은 JSON 객체여야 합니다.");
    auto* version = yyjson_obj_get(root, "version");
    if (!yyjson_is_int(version) || yyjson_get_sint(version) != 1) throw std::runtime_error("지원하지 않는 설정 버전입니다.");
    auto* hosts = yyjson_obj_get(root, "hosts");
    if (!yyjson_is_arr(hosts)) throw std::runtime_error("hosts 배열이 없습니다.");
    Config config; std::set<std::string> ids;
    size_t i, max; yyjson_val* value;
    yyjson_arr_foreach(hosts, i, max, value) {
        if (!yyjson_is_obj(value)) throw std::runtime_error("호스트 설정은 객체여야 합니다.");
        Host h;
        h.id = stringValue(value, "id"); h.name = stringValue(value, "name"); h.target = stringValue(value, "target");
        h.user = stringValue(value, "user", false); h.identityFile = stringValue(value, "identityFile", false);
        if (auto* p = yyjson_obj_get(value, "port"); p && !yyjson_is_null(p)) h.port = numberValue(value, "port");
        auto* forwards = yyjson_obj_get(value, "forwards");
        if (!yyjson_is_arr(forwards)) throw std::runtime_error("forwards 배열이 없습니다.");
        size_t j, count; yyjson_val* entry;
        yyjson_arr_foreach(forwards, j, count, entry) {
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
    yyjson_mut_obj_add_int(doc.get(), root, "version", 1);
    auto* hosts = yyjson_mut_arr(doc.get()); yyjson_mut_obj_add_val(doc.get(), root, "hosts", hosts);
    for (auto& h : config.hosts) {
        validate(h);
        auto* obj = yyjson_mut_obj(doc.get()); yyjson_mut_arr_add_val(hosts, obj);
        field(doc.get(), obj, "id", h.id); field(doc.get(), obj, "name", h.name); field(doc.get(), obj, "target", h.target);
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
    if (query) args.emplace_back("-G");
    else {
        for (auto flag : {"-v", "-N", "-n", "-T"}) args.emplace_back(flag);
        for (auto option : {"BatchMode=yes", "StrictHostKeyChecking=yes", "ExitOnForwardFailure=yes", "ServerAliveInterval=15", "ServerAliveCountMax=3"}) { args.emplace_back("-o"); args.emplace_back(option); }
        for (auto& f : h.forwards) if (f.enabled) {
            std::string destination = f.remoteHost;
            if (destination.find(':') != std::string::npos && destination.front() != '[') destination = '[' + destination + ']';
            args.emplace_back("-L"); args.push_back("127.0.0.1:" + std::to_string(f.localPort) + ':' + destination + ':' + std::to_string(f.remotePort));
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
bool authenticationFailure(const std::string& s) {
    return s.find("Permission denied") != std::string::npos || s.find("Host key verification failed") != std::string::npos || s.find("passphrase") != std::string::npos || s.find("REMOTE HOST IDENTIFICATION HAS CHANGED") != std::string::npos;
}
std::string describeFailure(const std::string& s) {
    if (s.find("Address already in use") != std::string::npos || s.find("cannot listen to port") != std::string::npos || s.find("Only one usage of each socket address") != std::string::npos) return "다른 프로그램이 해당 로컬 포트를 사용 중이거나 포트를 열 수 없습니다.\n" + s;
    return s.empty() ? "SSH 연결을 완료하지 못했습니다." : s;
}
bool Readiness::feed(const std::string& chunk) {
    pending_ += chunk;
    for (size_t n; (n = pending_.find('\n')) != std::string::npos;) {
        auto line = pending_.substr(0, n); pending_.erase(0,n+1);
        if (line.find("Authenticated to ") != std::string::npos) {
            // ProxyJump children share stderr. Their session entry cannot prove
            // the destination's forwarding is ready; start a new evidence set.
            authenticated_ = true; entered_ = false; localListening_ = false; remoteConfirmed_ = 0;
        }
        if (line.find("Local forwarding listening on ") != std::string::npos) localListening_ = true;
        if (line.find("Entering interactive session") != std::string::npos) entered_ = true;
        if (line.find("remote forward success for:") != std::string::npos) ++remoteConfirmed_;
        if (line.find("remote port forwarding failed") != std::string::npos || line.find("Could not request local forwarding") != std::string::npos || line.find("cannot listen to port") != std::string::npos) failed_ = true;
    }
    if (pending_.size() > 65536) pending_.erase(0,pending_.size()-65536);
    return !failed_ && authenticated_ && entered_ &&
        (remoteExpected_ > 0 ? remoteConfirmed_ >= remoteExpected_ : localListening_);
}
const char* stateName(State state) {
    switch(state) {
        case State::editing:return "editing"; case State::connecting:return "connecting"; case State::connected:return "connected";
        case State::waitingReconnect:return "waitingReconnect"; case State::reconnecting:return "reconnecting";
        case State::waitingExit:return "waitingExit"; case State::exiting:return "exiting";
    } return "exiting";
}
Action Machine::begin(bool reconnect, uint64_t now) {
    reconnection = reconnect; attempt = 1; state = reconnect ? State::reconnecting : State::connecting;
    timer = Timer::connect; deadline = now + 20000; return Action::start;
}
Action Machine::failed(uint64_t now) {
    if (!busy()) return Action::none;
    if (attempt < 2) { timer = Timer::retry; deadline = now + 1000; return Action::none; }
    if (reconnection) { state = State::waitingExit; timer = Timer::exit; deadline = now + 60000; return Action::none; }
    state = State::exiting; timer = Timer::none; return Action::initialFailure;
}
Action Machine::disconnected(uint64_t now) {
    if (state != State::connected) return Action::none;
    state = State::waitingReconnect; timer = Timer::automatic; deadline = now + 60000; return Action::none;
}
Action Machine::tick(uint64_t now) {
    if (timer == Timer::none || now < deadline) return Action::none;
    auto expired = timer; timer = Timer::none;
    if (expired == Timer::connect) return Action::timeout;
    if (expired == Timer::retry) { ++attempt; timer = Timer::connect; deadline = now + 20000; return Action::start; }
    if (expired == Timer::automatic) return begin(true, now);
    state = State::exiting; return Action::quit;
}
void Machine::ready() { state = State::connected; timer = Timer::none; }
void Machine::quit() { state = State::exiting; timer = Timer::none; }
}
