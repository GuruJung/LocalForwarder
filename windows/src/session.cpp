#include "session.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>

namespace lf {
namespace {
void retainTail(std::string& value,size_t size) {
    if(value.size()<=size) return;
    auto begin=value.size()-size;
    while(begin<value.size() && (static_cast<unsigned char>(value[begin])&0xc0)==0x80) ++begin;
    value.erase(0,begin);
}
std::string diagnosticUTF8(const std::string& input) {
    std::string output;
    for(size_t index=0;index<input.size();) {
        auto first=static_cast<unsigned char>(input[index]);
        if(first>0 && first<0x80) { output+=input[index++]; continue; }
        size_t length=first>=0xc2 && first<=0xdf?2:first>=0xe0 && first<=0xef?3:first>=0xf0 && first<=0xf4?4:0;
        bool valid=length && index+length<=input.size();
        for(size_t part=1;valid && part<length;++part) valid=(static_cast<unsigned char>(input[index+part])&0xc0)==0x80;
        if(valid && length>=3) {
            auto second=static_cast<unsigned char>(input[index+1]);
            valid=!(first==0xe0 && second<0xa0) && !(first==0xed && second>=0xa0) &&
                !(first==0xf0 && second<0x90) && !(first==0xf4 && second>=0x90);
        }
        if(valid) { output.append(input,index,length); index+=length; }
        else { output+="\xef\xbf\xbd"; ++index; }
    }
    return output;
}
void boundedAppend(std::string& target,const std::string& text) {
    target+=text;
    if(target.size()>65536) retainTail(target,32768);
}
std::string timestamp() {
    SYSTEMTIME now{}; GetSystemTime(&now); char text[32]{};
    std::snprintf(text,sizeof(text),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
    return text;
}

}
struct SSHSession::Operation {
    std::atomic<bool> cancelled=false;
    std::mutex mutex;
    std::shared_ptr<Child> child;
    void attach(const std::shared_ptr<Child>& value) {
        std::lock_guard guard(mutex); child=value; if(cancelled) child->stop();
    }
    void release() { std::lock_guard guard(mutex); child.reset(); }
    void cancel() {
        cancelled=true;
        std::shared_ptr<Child> active;
        { std::lock_guard guard(mutex); active=child; }
        if(active) active->stop();
    }
};
SSHSession::SSHSession(Post post,Schedule schedule,std::string configuration)
    :post_(std::move(post)),schedule_(std::move(schedule)),configuration_(std::move(configuration)) {
    if(!post_ || !schedule_) throw std::invalid_argument("SSHSession requires an event dispatcher and scheduler");
}
SSHSession::~SSHSession() { cancelTimer(); cancelOperations(); }
void SSHSession::changed() {
    if(tracker_) { runtime_.forwards=tracker_->forwards; runtime_.recentErrors=tracker_->recentErrors; }
    if(onChange) onChange();
}
void SSHSession::cancelTimer() { if(timer_) { auto cancel=std::move(timer_); timer_={}; cancel(); } }
void SSHSession::cancelOperations() { for(auto& operation:operations_) operation->cancel(); }
void SSHSession::afterCleanup(std::function<void()> completion) {
    if(operations_.empty()) completion(); else cleanupCallbacks_.push_back(std::move(completion));
}
void SSHSession::finishOperation(const std::shared_ptr<Operation>& operation) {
    std::erase(operations_,operation);
    if(operations_.empty()) {
        auto callbacks=std::move(cleanupCallbacks_); cleanupCallbacks_.clear();
        for(auto& callback:callbacks) callback();
    }
}
void SSHSession::start(const Host& host) {
    validate(host);
    if(runtime_.state!=ConnectionState::disconnected || !operations_.empty()) {
        auto self=shared_from_this(); stop([self,host] { self->start(host); }); return;
    }
    host_=host; attempt_=0; runtime_.executionHost=host; runtime_.lastError.clear();
    runtime_.recentErrors.clear(); launch();
}
void SSHSession::stop(std::function<void()> completion) {
    cancelTimer(); ++generation_; auto token=generation_;
    runtime_.state=ConnectionState::stopping;
    if(tracker_) tracker_->disconnected();
    cancelOperations(); changed();
    auto self=shared_from_this();
    afterCleanup([self,token,completion=std::move(completion)] {
        if(token==self->generation_) { self->runtime_.state=ConnectionState::disconnected; self->changed(); }
        completion();
    });
}
void SSHSession::fail(const std::string& error,bool allowRetry) {
    bool retry=allowRetry && runtime_.state==ConnectionState::connecting && !readiness_.authenticated() &&
        attempt_==0 && host_.retryOnConnectionRefused && isConnectionRefusal(targetDiagnostic_.empty()?error:targetDiagnostic_,diagnostic_);
    cancelTimer(); ++generation_; auto token=generation_;
    runtime_.lastError=describeFailure(diagnosticUTF8(error));
    if(tracker_) tracker_->disconnected();
    cancelOperations();
    runtime_.state=retry?ConnectionState::connecting:ConnectionState::disconnected;
    if(retry) {
        ++attempt_; auto self=shared_from_this();
        afterCleanup([self,token] {
            if(token!=self->generation_) return;
            self->timer_=self->schedule_(1,[weak=std::weak_ptr<SSHSession>(self),token] {
                if(auto value=weak.lock(); value && token==value->generation_) { value->timer_={}; value->launch(); }
            });
        });
    }
    changed();
}
void SSHSession::consume(const std::string& bytes,bool target,uint64_t token,bool flush) {
    if(token!=generation_) return;
    auto& pending=target?targetPending_:auxiliaryPending_;
    pending+=bytes;
    if(flush && !pending.empty() && pending.back()!='\n') pending+='\n';
    size_t end=0;
    while((end=pending.find('\n'))!=std::string::npos) {
        auto line=pending.substr(0,end); pending.erase(0,end+1);
        if(!line.empty() && line.back()=='\r') line.pop_back();
        line=diagnosticUTF8(line);
        boundedAppend(diagnostic_,line+"\n");
        if(!target) continue;
        boundedAppend(targetDiagnostic_,line+"\n");
        if(line.rfind("LocalForwarder diagnostic error:",0)==0) { fail(line,false); return; }
        readiness_.consume(line);
        if(readiness_.authenticated() && tracker_) tracker_->consume(line,timestamp());
        if(readiness_.ready() && runtime_.state==ConnectionState::connecting) {
            cancelTimer(); runtime_.state=ConnectionState::connected; runtime_.lastError.clear();
            if(tracker_) tracker_->sessionReady();
        }
        if(tracker_ && !tracker_->recentErrors.empty() && tracker_->recentErrors.back().message==line) {
            const auto& error=tracker_->recentErrors.back(); runtime_.lastError=error.time+" · "+error.message;
        }
        changed();
    }
    if(pending.size()>65536) retainTail(pending,32768);
}
void SSHSession::launch() {
    cancelTimer(); ++generation_; const auto token=generation_;
    diagnostic_.clear(); targetDiagnostic_.clear(); targetPending_.clear(); auxiliaryPending_.clear();
    readiness_=Readiness(0,false); tracker_=std::make_unique<ForwardTracker>(host_);
    runtime_.state=ConnectionState::connecting; changed();
    auto self=shared_from_this();
    timer_=schedule_(20,[weak=std::weak_ptr<SSHSession>(self),token] {
        if(auto value=weak.lock(); value && token==value->generation_ && value->runtime_.state==ConnectionState::connecting)
            value->fail("SSH 연결 준비 시간이 20초를 초과했습니다.\n"+value->diagnostic_,false);
    });
    auto operation=std::make_shared<Operation>(); operations_.push_back(operation);
    auto host=host_; auto configuration=configuration_; auto post=post_;
    std::thread([self,operation,token,host=std::move(host),configuration=std::move(configuration),post=std::move(post)]() mutable {
        DWORD code=1; std::string error;
        try {
            if(!host.identityFile.empty()) host.identityFile=utf8(expandHome(host.identityFile).wstring());
            auto ssh=systemSSH(); std::string effective,version;
            if(!operation->cancelled) {
                auto child=std::make_shared<Child>(ssh,std::vector<std::string>{"-V"},[&](bool ended,const std::string& text,DWORD) {
                    if(!ended && version.size()<4096) version.append(text,0,4096-version.size());
                });
                operation->attach(child); code=child->wait(10000); operation->release();
            }
            if(!operation->cancelled) {
                if(code!=0 || !supportedOpenSSHVersion(version)) throw std::runtime_error(sshUpdateGuidance);
                auto child=std::make_shared<Child>(ssh,sshArguments(host,configuration,true),[&](bool ended,const std::string& text,DWORD) {
                    if(!ended && effective.size()<262144) effective.append(text,0,262144-effective.size());
                });
                operation->attach(child); code=child->wait(INFINITE); operation->release();
            }
            if(!operation->cancelled && code==0) {
                effective=diagnosticUTF8(effective);
                post([self,token,effective,host] {
                    if(token!=self->generation_) return;
                    std::string target=host.target; int port=host.port.value_or(22);
                    std::istringstream lines(effective); std::string line;
                    while(std::getline(lines,line)) {
                        if(!line.empty() && line.back()=='\r') line.pop_back();
                        if(line.rfind("hostname ",0)==0) target=line.substr(9);
                        if(line.rfind("port ",0)==0) { try { port=std::stoi(line.substr(5)); } catch(...) {} }
                    }
                    self->readiness_=Readiness(0,false,target,port);
                    self->tracker_=std::make_unique<ForwardTracker>(host,inheritedForwards(effective)); self->changed();
                });
                if(!operation->cancelled) {
                    auto child=std::make_shared<Child>(ssh,sshArguments(host,configuration,false),
                        [self,post,token,pending=std::string{},discarding=false](DWORD pid,bool ended,const std::string& bytes,DWORD) mutable {
                            pending+=bytes;
                            if(ended && !pending.empty() && pending.back()!='\n') pending+='\n';
                            size_t end=0;
                            while((end=pending.find('\n'))!=std::string::npos) {
                                auto line=pending.substr(0,end); pending.erase(0,end+1);
                                if(discarding) { discarding=false; continue; }
                                if(!line.empty() && line.back()=='\r') line.pop_back();
                                auto target=targetDiagnosticLine(line,pid);
                                auto output=target?*target:line;
                                post([self,output=std::move(output),isTarget=target.has_value(),token] {
                                    self->consume(output+"\n",isTarget,token);
                                });
                            }
                            // Never reinterpret a tail fragment as a fresh PID-tagged line.
                            if(pending.size()>65536) { pending.clear(); discarding=true; }
                        });
                    operation->attach(child); code=child->wait(INFINITE); operation->release();
                }
            } else if(!operation->cancelled) error=effective;
        } catch(const std::exception& exception) { error=exception.what(); }
        operation->release();
        post([self,operation,token,code,error=std::move(error)] {
            // Every stream has posted its final bytes before this completion is queued.
            if(token==self->generation_) {
                self->consume({},true,token,true); self->consume({},false,token,true);
                auto message=error.empty()?self->diagnostic_:error;
                if(message.empty()) message="SSH가 종료되었습니다 ("+std::to_string(code)+").";
                self->fail(message,true);
            }
            self->finishOperation(operation);
        });
    }).detach();
}
}
