#pragma once
#include "core.hpp"
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace lf {
enum class ConnectionState { disconnected, connecting, connected, stopping };
enum class ForwardState { preparing, ready, failed, disconnected, unknown };
const char* stateName(ConnectionState state);
const char* forwardStateName(ForwardState state);
struct ForwardRuntime {
    std::string id, description, source, detail;
    ForwardState state = ForwardState::preparing;
    bool operator==(const ForwardRuntime&) const = default;
};
struct DestinationError {
    std::string time, message;
    bool operator==(const DestinationError&) const = default;
};
struct HostRuntime {
    ConnectionState state = ConnectionState::disconnected;
    std::optional<Host> executionHost;
    bool desiredConnected = false, temporary = false;
    std::vector<ForwardRuntime> forwards;
    std::vector<DestinationError> recentErrors;
    std::string lastError;
    std::string summary() const;
};
class ForwardTracker {
    struct Binding { bool remote; std::optional<int> port; std::optional<std::string> address; };
    std::vector<Binding> bindings_;
    std::optional<std::pair<std::string,int>> pendingListener_;
    std::map<size_t,std::set<std::string>> establishedAddresses_;
    void mark(bool remote, int port, std::optional<std::string> address, ForwardState state, const std::string& line);
public:
    std::vector<ForwardRuntime> forwards;
    std::vector<DestinationError> recentErrors;
    explicit ForwardTracker(const Host& host, const std::vector<std::string>& inherited = {});
    void consume(const std::string& line, const std::string& timestamp = {});
    void sessionReady();
    void disconnected();
};
using Cancel = std::function<void()>;
using Schedule = std::function<Cancel(double, std::function<void()>)>;
class SSHSessionProtocol {
public:
    std::function<void()> onChange;
    virtual ~SSHSessionProtocol() = default;
    virtual const HostRuntime& runtime() const = 0;
    virtual void start(const Host& host) = 0;
    // All callbacks, including completion, execute on the manager's event thread.
    // Completion means all processes and readers owned before this stop are finished.
    virtual void stop(std::function<void()> completion) = 0;
};
struct StoreAccess {
    std::function<Config()> load;
    std::function<Host(const std::string&)> host;
    std::function<void(const std::string&)> remove;
};
class ConnectionManager {
    struct Entry {
        Host host;
        HostRuntime runtime;
        std::optional<Host> overrideHost, pending;
        uint64_t generation = 0;
        std::shared_ptr<SSHSessionProtocol> session;
    };
    StoreAccess store_;
    std::function<std::shared_ptr<SSHSessionProtocol>()> factory_;
    Schedule schedule_;
    std::vector<Host> savedHosts_;
    std::map<std::string,std::shared_ptr<Entry>> entries_;
    std::vector<std::string> order_;
    Cancel recovery_;
    uint64_t recoveryGeneration_ = 0;
    bool available_ = true, networkInitialized_ = false, sleeping_ = false, quitting_ = false, exitNotified_ = false, disconnectingAll_ = false;
    size_t barriers_ = 0, outstandingStops_ = 0;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    std::shared_ptr<Entry> entry(const Host& host);
    void updateSaved(const Host& host);
    void launch(const std::shared_ptr<Entry>& value);
    void launchPending();
    void stop(const std::shared_ptr<Entry>& value, std::function<void()> completion);
    void changed();
    void checkExit();
    void cancelRecovery();
    void scheduleRecovery();
public:
    std::function<void()> onChange, onExit;
    std::function<void(const std::string&,const std::string&)> onError;
    ConnectionManager(StoreAccess store, std::function<std::shared_ptr<SSHSessionProtocol>()> factory, Schedule schedule);
    ~ConnectionManager();
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;
    std::vector<Host> hosts() const;
    const HostRuntime* runtime(const std::string& id) const;
    void reloadConfiguration();
    void startAll();
    void reconnectAll();
    void connect(const std::string& id);
    void connect(const Host& host, bool temporary);
    void saved(const Host& host);
    void disconnect(const std::string& id);
    void disconnectAll();
    bool isDisconnectingAll() const { return disconnectingAll_; }
    void remove(const std::string& id);
    void networkChanged(bool available);
    void sleep();
    void wake();
    void quit();
};
}
