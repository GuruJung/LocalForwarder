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
    bool close(bool accepted){bool connect=accepted&&startup;open=false;startup=false;drafts.clear();baseline.clear();order.clear();selected.clear();modalHost.clear();return connect;}
};
}
