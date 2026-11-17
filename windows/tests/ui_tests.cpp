#include "settings.hpp"
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
    std::cout<<"settings model tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
