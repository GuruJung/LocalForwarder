#include "manager.hpp"
#include <algorithm>
#include <utility>

namespace lf {
ConnectionManager::ConnectionManager(StoreAccess store,std::function<std::shared_ptr<SSHSessionProtocol>()> factory,Schedule schedule)
    :store_(std::move(store)),factory_(std::move(factory)),schedule_(std::move(schedule)) {
    if(!store_.load || !store_.host || !store_.remove || !factory_ || !schedule_) throw std::invalid_argument("연결 관리자 의존성이 없습니다.");
}
ConnectionManager::~ConnectionManager() {
    lifetime_.reset();
    if(recovery_) recovery_();
    for(auto& [id,value]:entries_) { value->session->onChange={}; try { value->session->stop([]{}); } catch(...) {} }
}
void ConnectionManager::changed() { if(onChange) onChange(); }
std::vector<Host> ConnectionManager::hosts() const {
    auto result=savedHosts_;
    for(const auto& id:order_) {
        if(std::none_of(savedHosts_.begin(),savedHosts_.end(),[&](auto& h){return h.id==id;})) {
            if(auto it=entries_.find(id);it!=entries_.end()) result.push_back(it->second->host);
        }
    }
    return result;
}
const HostRuntime* ConnectionManager::runtime(const std::string& id) const {
    auto it=entries_.find(id); return it==entries_.end() ? nullptr : &it->second->runtime;
}
std::shared_ptr<ConnectionManager::Entry> ConnectionManager::entry(const Host& host) {
    if(auto it=entries_.find(host.id);it!=entries_.end()) return it->second;
    auto value=std::make_shared<Entry>(); value->host=host; value->session=factory_();
    if(!value->session) throw std::runtime_error("SSH 세션을 만들 수 없습니다.");
    entries_[host.id]=value; order_.push_back(host.id);
    std::weak_ptr<Entry> weak=value; std::weak_ptr<int> alive=lifetime_;
    value->session->onChange=[this,weak,alive,id=host.id] {
        if(alive.expired()) return;
        auto value=weak.lock(); auto found=entries_.find(id);
        if(!value || found==entries_.end() || found->second!=value) return;
        const auto& session=value->session->runtime();
        auto previousError=value->runtime.lastError;
        value->runtime.state=session.state; value->runtime.forwards=session.forwards;
        value->runtime.lastError=session.lastError; value->runtime.recentErrors=session.recentErrors;
        if(!session.lastError.empty() && session.lastError!=previousError && onError) onError(id,session.lastError);
        if(!alive.expired()) changed();
    };
    return value;
}
void ConnectionManager::reloadConfiguration() {
    auto config=store_.load(); savedHosts_=std::move(config.hosts);
    for(auto& host:savedHosts_) entry(host)->host=host;
    changed();
}
void ConnectionManager::updateSaved(const Host& host) {
    auto it=std::find_if(savedHosts_.begin(),savedHosts_.end(),[&](auto& h){return h.id==host.id;});
    if(it==savedHosts_.end()) savedHosts_.push_back(host); else *it=host;
}
void ConnectionManager::saved(const Host& host) {
    if(quitting_) return;
    validate(host); updateSaved(host);
    auto value=entry(host); value->host=host; value->overrideHost.reset(); value->runtime.temporary=false;
    changed();
}
void ConnectionManager::startAll() { reconnectAll(); }
void ConnectionManager::reconnectAll() {
    if(quitting_) return;
    // Load and validate before canceling work or disturbing healthy connections.
    auto config=store_.load();
    for(auto& host:config.hosts) validate(host);
    cancelRecovery(); savedHosts_=std::move(config.hosts);
    for(auto& host:savedHosts_) entry(host)->host=host;
    std::vector<std::shared_ptr<Entry>> current;
    for(auto& [id,value]:entries_) current.push_back(value);
    ++barriers_;
    for(auto& value:current) {
        auto saved=std::find_if(savedHosts_.begin(),savedHosts_.end(),[&](auto& h){return h.id==value->host.id;});
        ++value->generation; value->overrideHost.reset(); value->runtime.temporary=saved==savedHosts_.end();
        value->runtime.desiredConnected=saved!=savedHosts_.end() && saved->autoConnect;
        value->pending=value->runtime.desiredConnected ? std::optional<Host>(*saved) : std::nullopt;
        value->runtime.state=ConnectionState::stopping;
    }
    if(current.empty()) { --barriers_; changed(); return; }
    auto remaining=std::make_shared<size_t>(current.size());
    std::weak_ptr<int> alive=lifetime_;
    for(auto& value:current) stop(value,[this,remaining,alive] {
        if(alive.expired()) return;
        if(--*remaining==0) { --barriers_; launchPending(); if(!alive.expired()) changed(); }
    });
    if(!alive.expired()) changed();
}
void ConnectionManager::connect(const std::string& id) {
    if(quitting_) return;
    auto it=entries_.find(id); auto existing=it==entries_.end() ? std::shared_ptr<Entry>{} : it->second;
    bool saved=std::any_of(savedHosts_.begin(),savedHosts_.end(),[&](auto& h){return h.id==id;});
    bool temporary=existing && (existing->overrideHost.has_value() || !saved);
    Host host;
    if(existing && existing->overrideHost) host=*existing->overrideHost;
    else if(existing && !saved) host=existing->host;
    else { host=store_.host(id); validate(host); updateSaved(host); }
    connect(host,temporary);
}
void ConnectionManager::connect(const Host& host,bool temporary) {
    if(quitting_) return;
    validate(host);
    auto value=entry(host); auto generation=++value->generation;
    value->host=host; value->overrideHost=temporary ? std::optional<Host>(host) : std::nullopt;
    value->runtime.temporary=temporary; value->runtime.desiredConnected=true;
    value->pending=host; value->runtime.state=ConnectionState::stopping;
    std::weak_ptr<int> alive=lifetime_;
    stop(value,[this,value,generation,alive] {
        if(alive.expired()) return;
        auto found=entries_.find(value->host.id);
        if(found!=entries_.end() && found->second==value && value->generation==generation && !quitting_) launch(value);
    });
    if(!alive.expired()) changed();
}
void ConnectionManager::launch(const std::shared_ptr<Entry>& value) {
    if(quitting_ || barriers_ || !value->runtime.desiredConnected || !value->pending) return;
    auto host=*value->pending; value->pending.reset();
    value->runtime.executionHost=host; value->runtime.state=ConnectionState::connecting;
    value->runtime.lastError.clear(); value->runtime.recentErrors.clear(); value->runtime.forwards=ForwardTracker(host).forwards;
    value->session->start(host);
    changed();
}
void ConnectionManager::launchPending() {
    if(barriers_) return;
    std::vector<std::shared_ptr<Entry>> current;
    for(auto& [id,value]:entries_) current.push_back(value);
    for(auto& value:current) {
        auto found=entries_.find(value->host.id);
        if(found!=entries_.end() && found->second==value) launch(value);
    }
}
void ConnectionManager::stop(const std::shared_ptr<Entry>& value,std::function<void()> completion) {
    ++outstandingStops_;
    std::weak_ptr<int> alive=lifetime_;
    auto once=std::make_shared<bool>(false);
    value->session->stop([this,value,alive,completion=std::move(completion),once] {
        if(*once) return;
        *once=true;
        if(alive.expired()) return;
        --outstandingStops_;
        completion();
        if(!alive.expired()) checkExit();
    });
}
void ConnectionManager::disconnect(const std::string& id) {
    if(quitting_) return;
    auto it=entries_.find(id); if(it==entries_.end()) return;
    auto value=it->second; ++value->generation; value->pending.reset();
    value->runtime.desiredConnected=false; value->runtime.state=ConnectionState::stopping;
    stop(value,[]{}); changed();
}
void ConnectionManager::remove(const std::string& id) {
    if(quitting_) return;
    if(std::any_of(savedHosts_.begin(),savedHosts_.end(),[&](auto& h){return h.id==id;})) store_.remove(id);
    disconnect(id); entries_.erase(id); std::erase(order_,id);
    std::erase_if(savedHosts_,[&](auto& h){return h.id==id;}); changed();
}
void ConnectionManager::networkChanged(bool available) {
    bool previous=available_; available_=available;
    bool initial=!networkInitialized_; networkInitialized_=true;
    if(!available) cancelRecovery();
    else if(!initial && !previous) scheduleRecovery();
}
void ConnectionManager::sleep() { sleeping_=true; cancelRecovery(); }
void ConnectionManager::wake() { sleeping_=false; scheduleRecovery(); }
void ConnectionManager::cancelRecovery() {
    if(recovery_) { auto cancel=std::move(recovery_); recovery_={}; cancel(); }
    ++recoveryGeneration_;
}
void ConnectionManager::scheduleRecovery() {
    cancelRecovery();
    if(quitting_ || !available_ || sleeping_) return;
    auto generation=recoveryGeneration_;
    std::map<std::string,uint64_t> generations;
    for(auto& [id,value]:entries_) generations[id]=value->generation;
    std::weak_ptr<int> alive=lifetime_;
    recovery_=schedule_(5,[this,alive,generation,generations] {
        if(alive.expired() || quitting_ || !available_ || sleeping_ || generation!=recoveryGeneration_) return;
        recovery_={};
        for(auto& [id,operation]:generations) {
            auto it=entries_.find(id); if(it==entries_.end()) continue;
            auto value=it->second;
            if(value->generation!=operation || !value->runtime.desiredConnected || value->runtime.state!=ConnectionState::disconnected) continue;
            try {
                auto saved=std::find_if(savedHosts_.begin(),savedHosts_.end(),[&](auto& h){return h.id==id;});
                std::optional<Host> host=value->overrideHost;
                if(!host && saved!=savedHosts_.end()) host=*saved;
                if(host) connect(*host,value->overrideHost.has_value());
            } catch(const std::exception& e) {
                value->runtime.lastError=e.what(); if(onError) onError(id,e.what());
            }
        }
        changed();
    });
}
void ConnectionManager::checkExit() {
    if(quitting_ && !outstandingStops_ && !exitNotified_) {
        exitNotified_=true; if(onExit) onExit();
    }
}
void ConnectionManager::quit() {
    if(quitting_) return;
    quitting_=true; cancelRecovery();
    // Sentinel prevents synchronous completions from exiting halfway through this loop.
    ++outstandingStops_;
    for(auto& [id,value]:entries_) {
        ++value->generation; value->pending.reset(); value->runtime.desiredConnected=false;
        stop(value,[]{});
    }
    changed(); --outstandingStops_; checkExit();
}
}
