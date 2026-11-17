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
    bool operator==(const Forward&) const = default;
};
struct Host {
    std::string id, name, target, user, identityFile, description;
    std::optional<int> port;
    std::vector<Forward> forwards;
    bool autoConnect = true, exitOnForwardFailure = true, retryOnConnectionRefused = true;
    bool operator==(const Host&) const = default;
};
struct Config {
    std::vector<Host> hosts;
    int version = 2;
    bool operator==(const Config&) const = default;
};
Config parseConfig(const std::string& text);
std::string writeConfig(const Config& config);
void validate(const Host& host);
void validateForward(const Forward& forward);
bool sameExecutionSettings(const Host& left, const Host& right);
bool safeId(const std::string& value);
std::string forwarding(const Forward& rule);
std::vector<std::string> sshArguments(const Host& host, const std::string& configFile = {}, bool query = false);
std::vector<std::string> authArguments(const Host& host, bool batch);
std::vector<std::string> inheritedForwards(const std::string& effective);
std::vector<std::string> identityFiles(const std::string& effective);
std::wstring quoteWindowsArgument(const std::wstring& value);
std::string quotePosix(const std::string& value);
std::wstring quotePowerShell(const std::wstring& value);
std::string registrationCommand(const std::string& publicKey);
bool authenticationFailure(const std::string& text);
bool isConnectionRefusal(const std::string& targetDiagnostic, const std::string& auxiliaryDiagnostic = {});
std::string describeFailure(const std::string& text);
// Only standard OpenSSH version banners at or above the supported baseline.
bool supportedOpenSSHVersion(const std::string& output);
// Accept anchored OpenSSH LogVerbose envelopes from the actual child PID.
// Peer-echoing kex messages remain auxiliary diagnostics, never state evidence.
std::optional<std::string> targetDiagnosticLine(const std::string& line, uint32_t expectedPid);

// Feed only target diagnostics when requireForwards is false. Proxy stderr must be separate.
class Readiness {
    std::string pending_, expectedTarget_;
    int remoteExpected_ = 0, remoteConfirmed_ = 0, expectedPort_ = 0;
    bool entered_ = false, authenticated_ = false, localListening_ = false, failed_ = false;
    bool requireForwards_ = true, targetScope_ = true;
public:
    explicit Readiness(int expected = 0, bool requireForwards = true,
                       std::string expectedTarget = {}, int expectedPort = 0);
    void consume(const std::string& line);
    bool feed(const std::string& chunk);
    bool ready() const;
    bool authenticated() const { return authenticated_; }
};
int remoteForwardCount(const std::string& effective);
}
