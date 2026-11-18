#pragma once
#include "manager.hpp"
#include "platform.hpp"
#include <memory>

namespace lf {
// All public calls and notifications use the supplied event-loop dispatcher.
class SSHSession final : public SSHSessionProtocol, public std::enable_shared_from_this<SSHSession> {
    struct Operation;
    Post post_;
    Schedule schedule_;
    std::string configuration_;
    Host host_;
    HostRuntime runtime_;
    Readiness readiness_{0,false};
    std::unique_ptr<ForwardTracker> tracker_;
    std::string diagnostic_,targetDiagnostic_,targetPending_,auxiliaryPending_;
    uint64_t generation_=0;
    int attempt_=0;
    Cancel timer_;
    std::vector<std::shared_ptr<Operation>> operations_;
    std::vector<std::function<void()>> cleanupCallbacks_;
    void changed();
    void cancelTimer();
    void cancelOperations();
    void afterCleanup(std::function<void()> completion);
    void finishOperation(const std::shared_ptr<Operation>& operation);
    void launch();
    void consume(const std::string& bytes,bool target,uint64_t generation,bool flush=false);
    void fail(const Message& error,bool allowRetry);
public:
    SSHSession(Post post,Schedule schedule,std::string sshConfiguration={});
    ~SSHSession() override;
    const HostRuntime& runtime() const override { return runtime_; }
    void start(const Host& host) override;
    void stop(std::function<void()> completion) override;
};
}
