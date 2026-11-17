#pragma once
#include "manager.hpp"
#include <algorithm>

namespace lf {
enum class MenuIcon { settings, reconnect, about, quit, connect, disconnect, edit, ready, disconnected, disabled, preparing, failed, unknown };
struct MenuForwardRow {
    std::string id, text;
    MenuIcon icon;
    bool operator==(const MenuForwardRow&) const = default;
};
inline MenuIcon forwardMenuIcon(ForwardState state) {
    switch(state){case ForwardState::preparing:return MenuIcon::preparing;case ForwardState::ready:return MenuIcon::ready;
    case ForwardState::failed:return MenuIcon::failed;case ForwardState::disconnected:return MenuIcon::disconnected;case ForwardState::unknown:return MenuIcon::unknown;}
    return MenuIcon::unknown;
}
inline MenuIcon hostMenuIcon(const Host& host,const HostRuntime* runtime) {
    if(runtime&&runtime->state==ConnectionState::connected)return MenuIcon::ready;
    return host.autoConnect?MenuIcon::disconnected:MenuIcon::disabled;
}
inline std::vector<MenuForwardRow> menuForwards(const Host& saved,const HostRuntime* runtime) {
    const auto& host=runtime&&runtime->executionHost?*runtime->executionHost:saved;
    std::vector<MenuForwardRow> rows;
    for(const auto& rule:host.forwards){
        auto icon=rule.enabled?MenuIcon::disconnected:MenuIcon::disabled;
        if(rule.enabled&&runtime&&runtime->executionHost){
            auto found=std::find_if(runtime->forwards.begin(),runtime->forwards.end(),[&](const auto& f){return f.id==rule.id&&f.source=="앱 설정";});
            if(found!=runtime->forwards.end())icon=forwardMenuIcon(found->state);
            else if(runtime->state==ConnectionState::connecting)icon=MenuIcon::preparing;
            else if(runtime->state==ConnectionState::connected)icon=MenuIcon::unknown;
        }
        rows.push_back({"app:"+rule.id,"앱 설정 · "+forwarding(rule),icon});
    }
    if(runtime&&runtime->executionHost)for(const auto& rule:runtime->forwards)if(rule.source!="앱 설정")
        rows.push_back({"inherited:"+rule.id,rule.source+" · "+rule.description,forwardMenuIcon(rule.state)});
    return rows;
}
}
