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
// OpenSSH's -E file receives only the target process log. A directory notification
// wakes the reader on append; EOF is drained again after SSH/descendants exit.
class DiagnosticFile {
    fs::path directory_,path_;
    Handle directoryHandle_,file_,cancel_,event_;
    std::thread worker_;
    std::function<void(const std::string&)> output_;
    OVERLAPPED overlap_{};
    unsigned char changes_[4096]{};
    bool armed_=false;
    void arm() {
        ResetEvent(event_.value);
        if(!ReadDirectoryChangesW(directoryHandle_.value,changes_,sizeof(changes_),FALSE,
            FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE,nullptr,&overlap_,nullptr))
            throw std::runtime_error("SSH 진단 알림을 시작할 수 없습니다: "+winError());
        armed_=true;
    }
    void drain() {
        char buffer[8192]; DWORD count=0;
        for(;;) {
            if(!ReadFile(file_.value,buffer,sizeof(buffer),&count,nullptr)) {
                output_("LocalForwarder diagnostic error: "+winError()+"\n"); break;
            }
            if(!count) break;
            output_(std::string(buffer,count));
        }
    }
public:
    explicit DiagnosticFile(std::function<void(const std::string&)> output):output_(std::move(output)) {
        directory_=fs::temp_directory_path()/wide("LocalForwarder-diagnostics-"+newId());
        createPrivateDirectory(directory_); path_=directory_/L"target.log";
        try {
            writePrivateFile(path_,{});
            file_.reset(CreateFileW(path_.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            directoryHandle_.reset(CreateFileW(directory_.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,nullptr));
            cancel_.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr)); event_.reset(CreateEventW(nullptr,TRUE,FALSE,nullptr));
            if(!file_ || !directoryHandle_ || !cancel_ || !event_) throw std::runtime_error(winError());
            overlap_.hEvent=event_.value;
            arm(); // Subscribe before SSH can write its first byte.
            worker_=std::thread([this] {
                HANDLE handles[]={cancel_.value,event_.value};
                while(WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0+1) {
                    DWORD count=0;
                    if(!GetOverlappedResult(directoryHandle_.value,&overlap_,&count,FALSE)) {
                        output_("LocalForwarder diagnostic error: "+winError()+"\n"); break;
                    }
                    armed_=false;
                    try { arm(); } catch(const std::exception& error) { output_(std::string("LocalForwarder diagnostic error: ")+error.what()+"\n"); break; }
                    drain();
                }
                if(armed_) {
                    CancelIoEx(directoryHandle_.value,&overlap_); DWORD ignored=0;
                    GetOverlappedResult(directoryHandle_.value,&overlap_,&ignored,TRUE); armed_=false;
                }
                drain();
            });
        } catch(...) {
            if(armed_) { CancelIoEx(directoryHandle_.value,&overlap_); DWORD ignored=0; GetOverlappedResult(directoryHandle_.value,&overlap_,&ignored,TRUE); }
            file_.reset(); directoryHandle_.reset(); std::error_code ignored; fs::remove_all(directory_,ignored); throw;
        }
    }
    const fs::path& path() const { return path_; }
    void finish() { if(worker_.joinable()) { SetEvent(cancel_.value); worker_.join(); } }
    ~DiagnosticFile() {
        finish(); file_.reset(); directoryHandle_.reset(); std::error_code ignored; fs::remove_all(directory_,ignored);
    }
};
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
            auto ssh=systemSSH(); std::string effective;
            {
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
                DiagnosticFile diagnostic([self,post,token](const std::string& bytes) {
                    post([self,bytes,token] { self->consume(bytes,true,token); });
                });
                if(!operation->cancelled) {
                    auto args=sshArguments(host,configuration,false);
                    args.insert(args.begin(),{"-E",utf8(diagnostic.path().wstring())});
                    auto child=std::make_shared<Child>(ssh,args,[self,post,token](bool ended,const std::string& bytes,DWORD) {
                        if(!ended) post([self,bytes,token] { self->consume(bytes,false,token); });
                    });
                    operation->attach(child); code=child->wait(INFINITE); operation->release();
                }
                diagnostic.finish();
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
