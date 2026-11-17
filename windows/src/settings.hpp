#pragma once
#include "manager.hpp"
#include <algorithm>
#include <map>
#include <string>

namespace lf {
// Raw port text remains part of a draft, including invalid edits, until explicit validation.
struct HostDraft {
    Host host;
    std::string port;
    std::map<std::string,std::pair<std::string,std::string>> rulePorts;
    HostDraft() = default;
    explicit HostDraft(const Host& h) : host(h), port(h.port ? std::to_string(*h.port) : "") {
        for (const auto& f : h.forwards) rulePorts[f.id] = {std::to_string(f.localPort),std::to_string(f.remotePort)};
    }
    static int parsePort(const std::string& text) {
        if (text.empty() || text.size()>5 || !std::all_of(text.begin(),text.end(),[](char c){return c>='0'&&c<='9';}))
            throw std::runtime_error("포트를 1–65535 사이의 숫자로 입력하세요.");
        int result=std::stoi(text);
        if(result<1 || result>65535) throw std::runtime_error("포트를 1–65535 사이의 숫자로 입력하세요.");
        return result;
    }
    Host validated() const {
        Host result=host;
        result.port=port.empty()?std::optional<int>{}:std::optional<int>{parsePort(port)};
        for(auto& f:result.forwards) {
            auto it=rulePorts.find(f.id);
            if(it!=rulePorts.end()) {f.localPort=parsePort(it->second.first);f.remotePort=parsePort(it->second.second);}
        }
        validate(result); return result;
    }
    bool operator==(const HostDraft&) const = default;
};
// A modal owns its buffer; validation never requires an otherwise-complete host.
struct RuleDraft {
    Forward rule;
    std::string localPort, remotePort;
    explicit RuleDraft(const Forward& f) : rule(f),localPort(std::to_string(f.localPort)),remotePort(std::to_string(f.remotePort)) {}
    Forward validated(const Host& host) const {
        auto result=rule;result.localPort=HostDraft::parsePort(localPort);result.remotePort=HostDraft::parsePort(remotePort);
        validateForward(result);
        for(const auto& other:host.forwards) if(other.id!=result.id&&other.enabled&&result.enabled&&other.localPort==result.localPort)
            throw std::runtime_error("활성 규칙의 로컬 포트가 중복됩니다.");
        return result;
    }
};
class SettingsModel {
public:
    std::map<std::string,HostDraft> drafts, baseline;
    std::vector<std::string> order;
    std::string selected, modalHost;
    bool open=false, startup=false;
    void load(const Config& config,const std::vector<Host>& transient,bool atStartup=false) {
        if(open) return;
        drafts.clear();baseline.clear();order.clear();selected.clear();modalHost.clear();startup=atStartup;open=true;
        for(const auto& h:config.hosts){order.push_back(h.id);drafts.emplace(h.id,HostDraft(h));baseline.emplace(h.id,HostDraft(h));}
        for(const auto& h:transient)if(!drafts.contains(h.id)){order.push_back(h.id);drafts.emplace(h.id,HostDraft(h));}
        if(!order.empty())selected=order.front();
    }
    void add(Host host){if(!modalHost.empty())return;order.push_back(host.id);selected=host.id;drafts.emplace(host.id,HostDraft(host));}
    bool select(const std::string& id){if(!modalHost.empty()||!drafts.contains(id))return false;selected=id;return true;}
    bool dirty(const std::string& id) const {auto d=drafts.find(id);auto b=baseline.find(id);return d!=drafts.end()&&(b==baseline.end()||d->second!=b->second);}
    bool anyDirty() const {return std::any_of(order.begin(),order.end(),[&](const auto& id){return dirty(id);});}
    Host validated() const {return drafts.at(selected).validated();}
    void saved(const Host& host){drafts.insert_or_assign(host.id,HostDraft(host));baseline.insert_or_assign(host.id,HostDraft(host));}
    void remove(const std::string& id){drafts.erase(id);baseline.erase(id);std::erase(order,id);if(selected==id)selected=order.empty()?"":order.front();}
    void authenticationFinished(const Host& original,const Host& updated){
        auto it=drafts.find(original.id);if(it==drafts.end())return;
        if(it->second.host.identityFile==original.identityFile)it->second.host.identityFile=updated.identityFile;
        baseline.insert_or_assign(original.id,HostDraft(updated));
    }
    void showStatus(){if(modalHost.empty()&&drafts.contains(selected))modalHost=selected;}
    void closeStatus(){modalHost.clear();}
    void applyRule(const std::string& hostID,const RuleDraft& draft,bool creating) {
        auto& hostDraft=drafts.at(hostID);auto rule=draft.validated(hostDraft.host);
        auto it=std::find_if(hostDraft.host.forwards.begin(),hostDraft.host.forwards.end(),[&](const auto& f){return f.id==rule.id;});
        if(creating) {if(it!=hostDraft.host.forwards.end())throw std::runtime_error("규칙 ID가 중복됩니다.");hostDraft.host.forwards.push_back(rule);}
        else {if(it==hostDraft.host.forwards.end())throw std::runtime_error("편집할 규칙이 없습니다.");*it=rule;}
        hostDraft.rulePorts[rule.id]={std::to_string(rule.localPort),std::to_string(rule.remotePort)};
    }
    bool close(bool accepted){bool connect=accepted&&startup;open=false;startup=false;drafts.clear();baseline.clear();order.clear();selected.clear();modalHost.clear();return connect;}
};
}
