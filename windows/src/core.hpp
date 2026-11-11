#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace lf {
struct Forward {
    std::string id;
    bool enabled = true;
    int localPort = 0;
    std::string remoteHost = "localhost";
    int remotePort = 0;
};
struct Host {
    std::string id, name, target, user, identityFile;
    std::optional<int> port;
    std::vector<Forward> forwards;
};
struct Config { std::vector<Host> hosts; };
Config parseConfig(const std::string& text);
std::string writeConfig(const Config& config);
void validate(const Host& host);
bool safeId(const std::string& value);
std::vector<std::string> sshArguments(const Host& host, const std::string& configFile = {}, bool query = false);
std::vector<std::string> authArguments(const Host& host, bool batch);
std::vector<std::string> inheritedForwards(const std::string& effective);
std::vector<std::string> identityFiles(const std::string& effective);
std::wstring quoteWindowsArgument(const std::wstring& value);
std::string quotePosix(const std::string& value);
std::wstring quotePowerShell(const std::wstring& value);
std::string registrationCommand(const std::string& publicKey);
bool authenticationFailure(const std::string& text);
std::string describeFailure(const std::string& text);

class Readiness {
    std::string pending_;
    int remoteExpected_ = 0, remoteConfirmed_ = 0;
    bool entered_ = false, authenticated_ = false, failed_ = false;
public:
    explicit Readiness(int expected = 0) : remoteExpected_(expected) {}
    bool feed(const std::string& chunk);
};
int remoteForwardCount(const std::string& effective);

enum class State { editing, connecting, connected, waitingReconnect, reconnecting, waitingExit, exiting };
enum class Action { none, start, timeout, initialFailure, quit };
enum class Timer { none, connect, retry, automatic, exit };
const char* stateName(State state);
struct Machine {
    State state = State::editing;
    Timer timer = Timer::none;
    uint64_t deadline = 0;
    int attempt = 0;
    bool reconnection = false;
    Action begin(bool reconnect, uint64_t now);
    Action failed(uint64_t now);
    Action disconnected(uint64_t now);
    Action tick(uint64_t now);
    void ready();
    void quit();
    bool busy() const { return state == State::connecting || state == State::reconnecting; }
};
}
