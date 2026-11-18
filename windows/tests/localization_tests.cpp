#include "localization.hpp"
#include "manager.hpp"
#include "presentation.hpp"
#include <iostream>
#include <thread>
#include <atomic>
using namespace lf;
void check(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
int main(){try{
    check(resolveLanguage({"fr-CA","ja-JP","de-DE"})=="ja","first supported preference");
    check(resolveLanguage({"fr","ar"})=="en","English fallback");
    for(auto value:{"en-US","en-GB"})check(resolveLanguage({value})=="en","English regional variants");
    for(auto value:{"es-MX","es-ES"})check(resolveLanguage({value})=="es","Spanish regional variants");
    for(auto value:{"zh","zh-CN","zh-SG","zh-Hans-TW","zh_Hans_HK"})check(resolveLanguage({value})=="zh-Hans","simplified and script precedence");
    for(auto value:{"zh-TW","zh-HK","zh-MO","zh-Hant-CN"})check(resolveLanguage({value})=="zh-Hant","traditional and script precedence");
    selectLanguage("unknown",{"ko-KR"});check(languageSelection()=="auto"&&currentLanguage()=="ko","invalid preference resolves automatically");
    std::string saved;selectLanguage("de",{"ja"},[&](const auto& value){saved=value;});
    check(saved=="de"&&currentLanguage()=="de"&&languageSelection()=="de","explicit language persists");
    selectLanguage(saved,{"en"});check(currentLanguage()=="de","stored selection survives restart model");
    bool failed=false;try{selectLanguage("es",{},[](const auto&){throw std::runtime_error("disk failure");});}catch(...){failed=true;}
    check(failed&&currentLanguage()=="de"&&languageSelection()=="de","save failure preserves current selection");
    selectLanguage("auto",{"fr","ja-JP"});check(currentLanguage()=="ja"&&languageSelection()=="auto","auto reevaluates preferences");
    check(languages().size()==7&&languages()[0].id=="zh-Hans"&&languages()[1].id=="zh-Hant"&&languages()[6].id=="es","stable alphabetic order");
    selectLanguage("en",{});
    Message retained=tr("win.language_save_failed")+"\n"+std::string("raw {language} & <peer> 日本語");
    check(retained.render()=="Could not save the language setting.\nraw {language} & <peer> 日本語","independent English output and raw preservation");
    selectLanguage("ko",{});
    check(retained.render()=="언어 설정을 저장하지 못했습니다.\nraw {language} & <peer> 日本語","retained message rerenders without touching raw data");
    check(translated("win.language_automatic",{{"language","{other} & raw"}},"en")=="Automatic (System Language — {other} & raw)","replacement values are not reparsed");
    check(translated("win.language",{},"missing")=="Language","missing catalog falls back to English");
    Host host;host.id="host";host.name="name";host.target="server";host.forwards={{"f",true,8080,"localhost",80}};
    ForwardTracker tracker(host);HostRuntime runtime;runtime.executionHost=host;runtime.state=ConnectionState::connected;runtime.forwards=tracker.forwards;
    auto englishSource=runtime.forwards[0].source;auto original=runtime.forwards;
    for(const auto& language:languages()){
        selectLanguage(language.id,{});auto rows=menuForwards(host,&runtime);
        check(rows.size()==1&&rows[0].icon==MenuIcon::preparing,"source identity remains stable in every language");
        check(runtime.forwards==original,"render does not alter execution state");
        check(!translated("win.language").empty(),"language label exists");
    }
    // Exercise thread-safe snapshots while the UI changes language.
    std::atomic<bool> correct=true;
    std::thread reader([&]{for(int i=0;i<2000;++i){auto text=retained.render();if(text.find("raw {language} & <peer> 日本語")==std::string::npos)correct=false;}});
    for(int i=0;i<2000;++i)selectLanguage(i%2?"ko":"en",{});reader.join();check(correct,"concurrent rendering preserves raw diagnostics");
    std::cout<<"localization tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
