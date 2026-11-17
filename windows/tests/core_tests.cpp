#include "manager.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace lf;
void check(bool value,const char* name) { if(!value) throw std::runtime_error(name); }
template<class F> void rejects(F f) { bool threw=false; try { f(); } catch(...) { threw=true; } check(threw,"expected rejection"); }
Host sample() { Host h; h.id="host-1"; h.name="개발 서버"; h.target="my-server"; h.forwards.push_back({"rule-1",true,15432,"localhost",5432}); return h; }
int main() {
 try {
    auto h=sample(); auto json=writeConfig({{h}}); auto parsed=parseConfig(json);
    check(parsed.hosts.size()==1 && parsed.hosts[0].name==h.name,"UTF-8 round trip");
    rejects([] {parseConfig("{bad");}); rejects([] {parseConfig("{\"version\":99,\"hosts\":[]}");});
    rejects([&] {auto bad=h;bad.target="-oProxyCommand=bad";validate(bad);});
    rejects([&] {auto bad=h;bad.target="a\nb";validate(bad);});
    rejects([&] {auto bad=h;bad.forwards.push_back({"rule-2",true,15432,"db",5432});validate(bad);});
    auto disabled=h;disabled.forwards.push_back({"rule-2",false,15432,"db",5432});validate(disabled);
    auto ipv6=h;ipv6.forwards[0].remoteHost="::1";
    auto args=sshArguments(ipv6,"C:\\space dir\\ssh_config");
    check(std::find(args.begin(),args.end(),"127.0.0.1:15432:[::1]:5432")!=args.end(),"IPv6 forwarding");
    check(std::find(args.begin(),args.end(),"ClearAllForwardings=no")!=args.end(),"forward override");
    check(args.back()==h.target && args[args.size()-2]=="--","destination separation");
    check(quoteWindowsArgument(L"")==L"\"\"","empty arg");
    check(quoteWindowsArgument(L"a b")==L"\"a b\"","space arg");
    check(quoteWindowsArgument(L"a\\")==L"\"a\\\\\"","trailing slash");
    check(quoteWindowsArgument(L"a\"b")==L"\"a\\\"b\"","quote arg");
    check(quotePosix("a'b")=="'a'\\''b'","POSIX quote");
    check(quotePowerShell(L"a'b")==L"'a''b'","PowerShell quote");
    check(remoteForwardCount("remoteforward 127.0.0.1:1234 localhost:22\nlocalforward 123 localhost:80\n")==1,"remote count");
    Readiness r(1);
    check(!r.feed("Authenticated to h\ndebug1: Entering inter"),"partial marker");
    check(!r.feed("active session.\n"),"remote acknowledgement required");
    check(r.feed("debug1: remote forward success for: listen 1234, connect localhost:22\n"),"remote acknowledged");
    Readiness jump;
    check(!jump.feed("Authenticated to jump\ndebug1: Entering interactive session.\n"),"jump child alone never ready");
    check(!jump.feed("Authenticated to target\ndebug1: Local forwarding listening on 127.0.0.1 port 15432.\n"),"target listener still requires target session entry");
    check(jump.feed("debug1: Entering interactive session.\n"),"target session with local listener ready");
    Readiness remoteJump(1);
    check(!remoteJump.feed("Authenticated to jump\ndebug1: Entering interactive session.\n"),"jump child cannot satisfy remote forwarding");
    check(!remoteJump.feed("debug1: remote forward success for: listen 1234, connect localhost:22\nAuthenticated to target\ndebug1: Entering interactive session.\n"),"target authentication resets preceding acknowledgements");
    check(remoteJump.feed("debug1: remote forward success for: listen 1234, connect localhost:22\n"),"target remote acknowledgement ready");
    Readiness failed;
    check(!failed.feed("Authenticated to h\nCould not request local forwarding.\ndebug1: Entering interactive session.\n"),"failed forward never connected");
    // New v2 storage preserves v1 reads, defaults missing/null options, and rejects explicit empty optional strings.
    auto v1=parseConfig(R"({"version":1,"hosts":[{"id":"old","name":"old","target":"server","forwards":[]}]})");
    check(v1.version==1 && v1.hosts[0].autoConnect && v1.hosts[0].exitOnForwardFailure && v1.hosts[0].retryOnConnectionRefused,"v1 defaults");
    check(parseConfig(writeConfig(v1)).version==2,"write upgrades v1 to v2");
    auto empty=parseConfig(R"({"version":2,"hosts":[]})"); check(empty.hosts.empty(),"empty config permitted");
    auto optional=parseConfig(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","user":null,"identityFile":null,"port":null,"forwards":[],"autoConnect":null}]})");
    check(optional.hosts[0].user.empty() && optional.hosts[0].autoConnect,"null defaults");
    for(auto field:{"user","identityFile"}) {
        auto text=R"({"version":1,"hosts":[{"id":"h","name":"h","target":"s","forwards":[],")"+std::string(field)+R"(":""}]})";
        rejects([&]{parseConfig(text);});
    }
    rejects([]{parseConfig(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","forwards":[],"autoConnect":1}]})");});
    for(auto version:{"1.0","2e0","0.2e1"}) {
        auto text=std::string(R"({"version":)")+version+R"(,"hosts":[]})";
        check(parseConfig(text).version>=1,"integral JSON version accepted");
    }
    for(auto number:{"22.0","2.2e1","220e-1","0.022e3","65535.000"}) {
        auto text=std::string(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","port":)")+number+R"(,"forwards":[]}]})";
        check(parseConfig(text).hosts[0].port.has_value(),"integral JSON port accepted");
    }
    for(auto number:{"true","22.5","22.000000000000000001","65536","1e999","1e-999","18446744073709551616","-22","0.0"}) {
        auto text=std::string(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","port":)")+number+R"(,"forwards":[]}]})";
        rejects([&]{parseConfig(text);});
    }
    for(auto version:{"true","2.1","2.000000000000000001","1e999","-1"}) {
        auto text=std::string(R"({"version":)")+version+R"(,"hosts":[]})"; rejects([&]{parseConfig(text);});
    }
    // Win32 NUL-terminated labels cannot represent this name safely; reject it
    // instead of displaying/serializing a silently truncated host name.
    rejects([]{parseConfig(R"({"version":2,"hosts":[{"id":"h","name":"a\u0000b","target":"s","forwards":[]}]})");});
    auto options=h;options.autoConnect=false;options.exitOnForwardFailure=false;options.retryOnConnectionRefused=false;
    check(parseConfig(writeConfig({{options}})).hosts[0]==options,"options survive roundtrip");
    auto noFailure=sshArguments(options); check(std::find(noFailure.begin(),noFailure.end(),"ExitOnForwardFailure=no")!=noFailure.end(),"exit failure option");
    auto inspect=sshArguments(options,{},true); check(std::find(inspect.begin(),inspect.end(),"ExitOnForwardFailure=no")!=inspect.end(),"inspect same failure option");
    rejects([&]{writeConfig({{h,h}});});
    for(const auto& whitespace:std::vector<std::string>{" ","\t", "\xc2\xa0", "\xe3\x80\x80", "\xe2\x80\x89"}) {
        auto invalid=h;invalid.name=whitespace;rejects([&]{validate(invalid);});
        invalid=h;invalid.target="a"+whitespace+"b";rejects([&]{validate(invalid);});
    }
    for(const auto& control:std::vector<std::string>{"\x7f", "\xc2\x85", "\xe2\x80\x8b", "\xe2\x80\xae", "\xef\xbb\xbf"}) {
        auto invalid=h;invalid.target="a"+control+"b";rejects([&]{validate(invalid);});
        invalid=h;invalid.identityFile="C:/a"+control+"b";rejects([&]{validate(invalid);});
    }
    auto malformed=h;malformed.name=std::string("\xc0\xaf",2);rejects([&]{validate(malformed);});
    auto noRules=h;noRules.forwards.clear();validate(noRules);
    auto keyPath=h;keyPath.identityFile="C:/한 글/키";validate(keyPath);
    for(const auto& diagnostic:std::vector<std::string>{"ssh: connect to host server port 22: Connection refused", "kex_exchange_identification: read: Connection reset by peer", "Connection closed by 127.0.0.1 port 22"})
        check(isConnectionRefusal(diagnostic),"eligible pre-auth refusal");
    for(const auto& diagnostic:std::vector<std::string>{"Permission denied (publickey).", "Host key verification failed.", "load key: failure", "Connection timed out", "bind [127.0.0.1]:4: Address already in use", "channel 1: open failed: connect failed: Connection refused", "Unclassified error"})
        check(!isConnectionRefusal(diagnostic),"ineligible retry");
    check(!isConnectionRefusal("Connection closed by target", "Permission denied (publickey)"),"proxy auth veto");
    check(isConnectionRefusal("Connection closed by target", "Authenticated to jump"),"proxy success does not hide target refusal");
    check(!isConnectionRefusal("Authenticated to target\nConnection closed by target"),"post-auth no retry");
    Readiness target(0,false,"target",22);
    target.consume("Authenticated to jump ([127.0.0.1]:22).");target.consume("Entering interactive session.");check(!target.ready(),"wrong target cannot connect");
    target.consume("Authenticated to target ([127.0.0.1]:22).");target.consume("Could not request local forwarding.");target.consume("Entering interactive session.");
    check(target.ready(),"SSH readiness independent from failed forwards");
    Readiness sameTarget(0,false,"target",22);
    sameTarget.consume("Authenticated to target ([127.0.0.1]:22).");sameTarget.consume("debug1: channel 0: new stdio-forward [stdio-forward]");sameTarget.consume("Entering interactive session.");
    check(!sameTarget.ready(),"identical-address jump scope cannot establish target");
    ForwardTracker tracker(h,{"remoteforward 19090 localhost:5432","dynamicforward /tmp/socket"});
    tracker.consume("debug1: Local forwarding listening on 127.0.0.1 port 15432.");check(tracker.forwards[0].state==ForwardState::preparing,"listener announcement not proof");
    tracker.consume("debug1: channel 0: new port-listener [port listener]");check(tracker.forwards[0].state==ForwardState::ready,"listener creation proof");
    tracker.consume("debug1: remote forward success for: listen localhost:19090, connect localhost:5432");check(tracker.forwards[1].state==ForwardState::ready,"remote ack mapped");
    tracker.sessionReady();check(tracker.forwards[2].state==ForwardState::unknown,"unidentifiable inherited forward unknown");
    tracker.consume("channel 2: open failed: connect failed: Connection refused","2026-11-17T00:00:00Z");
    check(tracker.recentErrors.size()==1 && tracker.recentErrors[0].time=="2026-11-17T00:00:00Z","destination failure time retained");
    check(tracker.forwards[0].state==ForwardState::ready,"destination error does not undo listener");
    tracker.disconnected();check(std::all_of(tracker.forwards.begin(),tracker.forwards.end(),[](auto& f){return f.state==ForwardState::disconnected;}),"exit clears all forward readiness");
    ForwardTracker failedBind(h);failedBind.consume("debug1: Local forwarding listening on 127.0.0.1 port 15432.");
    failedBind.consume("bind [127.0.0.1]:15432: Address already in use");failedBind.sessionReady();check(failedBind.forwards[0].state==ForwardState::failed,"bind failure mapped");
    ForwardTracker dual(noRules,{"localforward 15432 localhost:80"});
    dual.consume("Local forwarding listening on ::1 port 15432.");dual.consume("channel 0: new port-listener [port listener]");
    dual.consume("Local forwarding listening on 127.0.0.1 port 15432.");dual.consume("bind [127.0.0.1]:15432: Address already in use");
    check(dual.forwards[0].state==ForwardState::ready && dual.forwards[0].detail.find("일부 주소")!=std::string::npos,"one address success retained");
    ForwardTracker ambiguous(h,{"localforward 127.0.0.1:15432 localhost:80"});
    ambiguous.consume("Local forwarding listening on 127.0.0.1 port 15432.");ambiguous.consume("channel 0: new port-listener [port listener]");ambiguous.sessionReady();
    check(ambiguous.forwards[0].state==ForwardState::unknown && ambiguous.forwards[1].state==ForwardState::unknown,"shared listener not guessed");
    check(registrationCommand("ssh-ed25519 AAAA comment").find("for(i=1;i<=NF;i++)")!=std::string::npos,"restricted authorized key entry retained");
    std::cout<<"core tests passed\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
