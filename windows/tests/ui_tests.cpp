#include "settings.hpp"
#include "presentation.hpp"
#include <iostream>
using namespace lf;
static void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){try{
    Host a;a.id="a";a.name="A";a.target="server-a";a.forwards.push_back({"f",true,12000,"localhost",80});
    Host b=a;b.id="b";b.name="B";b.target="server-b";
    SettingsModel model;model.load(Config{{a,b}}, {}, true);
    model.drafts.at("a").host.name="A edited";model.drafts.at("a").port="not-a-number";
    check(model.select("b"),"host switch");model.drafts.at("b").host.name="B edited";
    Host saved=model.validated();model.saved(saved);
    check(model.dirty("a")&&!model.dirty("b"),"selected save preserves peer draft");
    model.select("a");check(model.drafts.at("a").port=="not-a-number","invalid raw input preserved");
    bool invalid=false;try{model.validated();}catch(...){invalid=true;}check(invalid,"invalid input rejected before connection");
    model.showStatus();check(model.modalHost=="a"&&!model.select("b"),"modal target is fixed");
    model.load(Config{{b}}, {}, false);check(model.selected=="a"&&model.dirty("a"),"second open cannot reload edits");
    model.closeStatus();check(model.select("b"),"modal close restores selection");
    Host original=saved,updated=saved;updated.identityFile="new-key";model.drafts.at("b").host.target="unsaved-target";
    model.authenticationFinished(original,updated);check(model.drafts.at("b").host.identityFile=="new-key"&&model.drafts.at("b").host.target=="unsaved-target","helper merges only unchanged key field");
    model.drafts.at("b").host.identityFile="user-edited-key";updated.identityFile="another-key";
    model.authenticationFinished(original,updated);check(model.drafts.at("b").host.identityFile=="user-edited-key","helper preserves new key edit");
    check(!model.close(false),"startup cancel never starts all");
    model.load(Config{{a}}, {}, true);check(model.close(true),"startup OK starts all");
    model.load(Config{{a}}, {}, false);check(!model.close(true),"operating OK does not reconnect");
    model.load(Config{{a}}, {b});check(model.dirty("b"),"transient host remains unsaved");model.remove("b");model.authenticationFinished(b,b);check(!model.drafts.contains("b"),"helper cannot resurrect deleted draft");
    model.close(false);model.load(Config{{a,b}}, {}, false);
    auto originalDraft=model.drafts.at("a");
    RuleDraft newRule(Forward{"new",true,18080,"localhost",80});
    newRule.remotePort="bad";bool ruleInvalid=false;try{model.applyRule("a",newRule,true);}catch(...){ruleInvalid=true;}
    check(ruleInvalid&&model.drafts.at("a")==originalDraft,"invalid modal commit is atomic");
    newRule.remotePort="80";newRule.rule.description="새 웹 규칙";model.drafts.at("a").host.target="";model.drafts.at("a").host.name="";
    model.applyRule("a",newRule,true);check(model.drafts.at("a").host.forwards.size()==2,"rules can be added before host fields");
    auto edited=newRule;edited.rule.remoteHost="new-destination";model.applyRule("a",edited,false);
    check(model.drafts.at("a").host.forwards[1].id=="new"&&model.drafts.at("a").host.forwards[1].remoteHost=="new-destination"&&model.drafts.at("a").host.forwards[1].description=="새 웹 규칙","edit keeps ID and order");
    RuleDraft duplicate(Forward{"duplicate",true,18080,"localhost",80});ruleInvalid=false;try{model.applyRule("a",duplicate,true);}catch(...){ruleInvalid=true;}
    check(ruleInvalid,"modal validates active duplicates");duplicate.rule.enabled=false;model.applyRule("a",duplicate,true);
    check(model.drafts.at("b")==HostDraft(b),"modal leaves peer host unchanged");
    SettingsModel metadata;metadata.load(Config{{a,b}}, {}, false);
    RuleDraft metadataEdit(a.forwards[0]);metadataEdit.rule.description="개발 DB 접속";
    metadata.applyRule("a",metadataEdit,false);
    check(metadata.dirty("a")&&!metadata.dirty("b"),"description edit marks only selected host dirty");
    auto described=metadata.validated();check(described.forwards[0].description=="개발 DB 접속"&&sameExecutionSettings(described,a),"description edit validated without execution difference");
    auto authenticated=described;authenticated.identityFile="new-key";
    metadata.authenticationFinished(a,authenticated);
    check(metadata.drafts.at("a").host.forwards[0].description=="개발 DB 접속","authentication preserves unsaved rule description");
    metadata.saved(authenticated);check(!metadata.dirty("a"),"description save resets dirty flag");
    metadataEdit.rule.description.clear();metadata.applyRule("a",metadataEdit,false);
    check(metadata.dirty("a")&&metadata.validated().forwards[0].description.empty(),"description clearing is a saved edit");
    auto beforeInvalid=metadata.drafts.at("a");metadataEdit.rule.description="invalid\nline";ruleInvalid=false;
    try{metadata.applyRule("a",metadataEdit,false);}catch(...){ruleInvalid=true;}
    check(ruleInvalid&&metadata.drafts.at("a")==beforeInvalid,"invalid rule description commit is atomic");
    auto savedSnapshot=a;savedSnapshot.forwards.push_back({"disabled",false,12001,"db",5432});
    auto rows=menuForwards(savedSnapshot,nullptr);check(rows.size()==2&&rows[0].icon==MenuIcon::disconnected&&rows[1].icon==MenuIcon::disabled,"preconnection menu includes inactive rules");
    HostRuntime runtime;runtime.state=ConnectionState::connected;runtime.executionHost=savedSnapshot;
    runtime.forwards={{"f","live-forward","앱 설정","",ForwardState::ready},{"ssh-config-0","localforward 13000 localhost:80","SSH 설정","",ForwardState::failed}};
    auto changed=savedSnapshot;changed.forwards.clear();rows=menuForwards(changed,&runtime);
    check(rows.size()==3&&rows[0].icon==MenuIcon::ready&&rows[1].icon==MenuIcon::disabled&&rows[2].icon==MenuIcon::failed,"execution snapshot plus inherited rules, not newly savedSnapshot config");
    check(hostMenuIcon(changed,&runtime)==MenuIcon::ready,"partial forwarding failure keeps SSH-connected host icon");
    changed.autoConnect=false;check(hostMenuIcon(changed,&runtime)==MenuIcon::ready,"manual SSH connection takes precedence over autoconnect disabled");
    runtime.executionHost->forwards[0].id="ssh-config-0";runtime.forwards[0].id="ssh-config-0";
    rows=menuForwards(changed,&runtime);check(rows.size()==3&&rows[0].icon==MenuIcon::ready&&rows[2].icon==MenuIcon::failed,"app and inherited ID collision retains both categories");
    for(auto state:{ForwardState::preparing,ForwardState::ready,ForwardState::failed,ForwardState::disconnected,ForwardState::unknown}){runtime.forwards[0].state=state;check(menuForwards(changed,&runtime)[0].icon==forwardMenuIcon(state),"forward state projection");}
    for(auto state:{ConnectionState::connecting,ConnectionState::stopping,ConnectionState::disconnected}){
        runtime.state=state;changed.autoConnect=true;check(hostMenuIcon(changed,&runtime)==MenuIcon::disconnected,"nonconnected SSH state with autoconnect enabled");
        changed.autoConnect=false;check(hostMenuIcon(changed,&runtime)==MenuIcon::disabled,"nonconnected SSH state with autoconnect disabled");
    }
    changed.autoConnect=true;check(hostMenuIcon(changed,nullptr)==MenuIcon::disconnected,"unstarted autoconnect host");
    changed.autoConnect=false;check(hostMenuIcon(changed,nullptr)==MenuIcon::disabled,"unstarted disabled host");
    check(menuForwards(Host{},nullptr).empty(),"empty menu projection");
    std::cout<<"settings model tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
