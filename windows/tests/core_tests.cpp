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
    auto described=h;described.description="개발 DB와 웹 서비스";
    check(parseConfig(writeConfig({{described}})).hosts[0]==described,"description UTF-8 round trip");
    check(sshArguments(described)==sshArguments(h)&&sameExecutionSettings(described,h),"description is not an SSH setting");
    check(described!=h,"description participates in dirty equality");
    for(auto value:{"null","\"\""}) {
        auto data=std::string(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","forwards":[],"description":)")+value+"}]}";
        check(parseConfig(data).hosts[0].description.empty(),"empty optional description");
    }
    check(json.find("description")==std::string::npos,"empty description omitted");
    for(auto value:{"1","true","[]","{}","\"a\\nb\"","\"a\\rb\"","\"a\\u0000b\"","\"a\\u2028b\""}) {
        auto data=std::string(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","forwards":[],"description":)")+value+"}]}";
        rejects([&]{parseConfig(data);});
    }
    auto describedRule=h;describedRule.forwards[0].description="개발 DB 조회";
    describedRule.forwards.push_back({"disabled",false,15433,"localhost",80,"사용 중지 웹 서비스"});
    auto undescribedRules=describedRule;for(auto& rule:undescribedRules.forwards)rule.description.clear();
    check(parseConfig(writeConfig({{describedRule}})).hosts[0]==describedRule,"rule descriptions UTF-8 round trip");
    check(sshArguments(describedRule)==sshArguments(undescribedRules)&&sameExecutionSettings(describedRule,undescribedRules),"all rule descriptions excluded from execution settings");
    check(describedRule!=undescribedRules,"rule descriptions participate in dirty equality");
    auto changedPort=describedRule;changedPort.forwards[0].remotePort++;
    check(!sameExecutionSettings(changedPort,undescribedRules),"real forwarding changes remain execution differences");
    auto ruleJSON=[](int version,const std::string& suffix){return std::string("{\"version\":")+std::to_string(version)+R"(,"hosts":[{"id":"h","name":"h","target":"s","forwards":[{"id":"f","enabled":true,"localPort":1234,"remoteHost":"localhost","remotePort":80)"+suffix+"}]}]}";};
    for(auto suffix:{"",",\"description\":null",",\"description\":\"\""}){
        auto old=parseConfig(ruleJSON(2,suffix));
        check(old.hosts[0].forwards[0].description.empty(),"v2 missing nullable empty rule description");
        check(writeConfig(old).find("description")==std::string::npos,"empty rule description omitted on save");
    }
    for(auto value:{"1","true","[]","{}","\"a\\nb\"","\"a\\rb\"","\"a\\u0000b\"","\"a\\u0085b\"","\"a\\u2028b\"","\"a\\u2029b\""})
        rejects([&]{parseConfig(ruleJSON(2,std::string(",\"description\":")+value));});
    for(const auto& invalid:std::vector<std::string>{"a\nb","a\rb",std::string("a\0b",3),"a\xc2\x85" "b","a\xe2\x80\xa8" "b","a\xe2\x80\xa9" "b"}){
        auto bad=h;bad.forwards[0].description=invalid;
        rejects([&]{validateForward(bad.forwards[0]);});rejects([&]{writeConfig({{bad}});});
    }
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
    // v2 defaults missing/null options and rejects explicit empty optional strings.
    auto defaults=parseConfig(R"({"version":2,"hosts":[{"id":"old","name":"old","target":"server","forwards":[]}]})");
    check(defaults.version==2 && defaults.hosts[0].autoConnect && defaults.hosts[0].exitOnForwardFailure && defaults.hosts[0].retryOnConnectionRefused,"v2 defaults");
    check(parseConfig(writeConfig(defaults)).version==2,"v2 round trip");
    rejects([]{parseConfig(R"({"version":1,"hosts":[{"id":"old","name":"old","target":"server","forwards":[]}]})");});
    auto empty=parseConfig(R"({"version":2,"hosts":[]})"); check(empty.hosts.empty(),"empty config permitted");
    auto optional=parseConfig(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","user":null,"identityFile":null,"port":null,"forwards":[],"autoConnect":null}]})");
    check(optional.hosts[0].user.empty() && optional.hosts[0].autoConnect,"null defaults");
    for(auto field:{"user","identityFile"}) {
        auto text=R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","forwards":[],")"+std::string(field)+R"(":""}]})";
        rejects([&]{parseConfig(text);});
    }
    rejects([]{parseConfig(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","forwards":[],"autoConnect":1}]})");});
    for(auto version:{"2.0","2e0","0.2e1"}) {
        auto text=std::string(R"({"version":)")+version+R"(,"hosts":[]})";
        check(parseConfig(text).version==2,"integral JSON version accepted");
    }
    for(auto number:{"22.0","2.2e1","220e-1","0.022e3","65535.000"}) {
        auto text=std::string(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","port":)")+number+R"(,"forwards":[]}]})";
        check(parseConfig(text).hosts[0].port.has_value(),"integral JSON port accepted");
    }
    for(auto number:{"true","22.5","22.000000000000000001","65536","1e999","1e-999","18446744073709551616","-22","0.0"}) {
        auto text=std::string(R"({"version":2,"hosts":[{"id":"h","name":"h","target":"s","port":)")+number+R"(,"forwards":[]}]})";
        rejects([&]{parseConfig(text);});
    }
    for(auto version:{"1","1.0","1e0","0","3","true","2.1","2.000000000000000001","1e999","-1"}) {
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
    const std::string countdownClose="debug1: Connection established.\ndebug3: timeout: 10000 ms remain after connect\ndebug1: Local version string SSH-2.0-OpenSSH_for_Windows_9.5\nkex_exchange_identification: Connection closed by remote host";
    check(isConnectionRefusal(countdownClose,countdownClose),"remaining-time countdown does not veto canonical banner close");
    check(isConnectionRefusal("debug3: timeout: 10000 ms remain after connect\nssh: connect to host target port 22: Connection refused"),"countdown does not veto TCP refusal");
    check(!isConnectionRefusal("debug3: timeout: 10000 ms remain after connect"),"countdown alone is not refusal evidence");
    for(auto failure:{"Connection timed out during banner exchange", "ssh: connect to host target port 22: Connection timed out", "Timeout, server target not responding.", "Permission denied (publickey).", "Host key verification failed.", "load key: failure"}) {
        check(!isConnectionRefusal(countdownClose+"\n"+failure),"real target timeout or auth failure vetoes close");
        check(!isConnectionRefusal(countdownClose,failure),"real auxiliary timeout or auth failure vetoes close");
    }
    check(!isConnectionRefusal("kex_exchange_identification: read: Unknown error"),"ambiguous Windows errno is not proof of reset");
    check(!isConnectionRefusal("kex_exchange_identification: read: Unknown error\nbanner exchange: Connection to 127.0.0.1 port 22: Unknown error"),"matching banner failure does not disambiguate unknown errno");
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
    for(auto banner:{"OpenSSH_for_Windows_9.5p2, LibreSSL 3.8.2\r\n", "OpenSSH_9.5p1, OpenSSL 3.0.0\n", "OpenSSH_9.5", "OpenSSH_9.10p1", "OpenSSH_for_Windows_10.0p1, LibreSSL 4.0.0"})
        check(supportedOpenSSHVersion(banner),"supported system OpenSSH baseline");
    for(auto banner:{"OpenSSH_for_Windows_9.4p1, LibreSSL 3.8.2", "OpenSSH_8.9p1", "prefix OpenSSH_10.0p1", "OpenSSH_9.5garbage", "OpenSSH_9.5p", "OpenSSH_9.x", "OpenSSH_9.5.1", "OpenSSH_99999999999999999999.0", "OpenSSH_09.5", "OpenSSH_9.5p1\nextra", "OpenSSH_9.5p1, "})
        check(!supportedOpenSSHVersion(banner),"old or malformed version rejected");
    auto tagged=targetDiagnosticLine("debug1: sshconnect2.c:ssh_userauth2():449 (pid=1234): Authenticated to target ([127.0.0.1]:22).",1234);
    check(tagged && *tagged=="debug1: Authenticated to target ([127.0.0.1]:22).","target tag removed, severity retained");
    auto plainTagged=targetDiagnosticLine("ssh.c:main():123 (pid=1234): Connection closed by target",1234);
    check(plainTagged && *plainTagged=="Connection closed by target","tagged info severity preserved");
    auto futureTagged=targetDiagnosticLine("debug2: channels.c:channel_new():512 (bin=ssh, pid=1234): channel 0: new port-listener [port listener]\r",1234);
    check(futureTagged && *futureTagged=="debug2: channel 0: new port-listener [port listener]","future binary PID format");
    auto failureTagged=targetDiagnosticLine("fatal: kex.c:kex_exchange_identification():1337 (pid=1234): kex_exchange_identification: read: Connection reset by peer",1234);
    check(failureTagged && isConnectionRefusal(*failureTagged),"target preauth failure classification after tag removal");
    check(targetDiagnosticLine("ssh.c:main():123 (bin=ssh.exe, pid=1234): message",1234)=="message","future Windows binary PID format");
    check(targetDiagnosticLine(R"(debug1: C:\build\openssh\sshconnect.c:ssh_connect_direct():1 (pid=1234): Connecting to target)",1234)=="debug1: Connecting to target","Windows absolute source path");
    check(targetDiagnosticLine(R"(debug1: .\ssh.c:main():1 (pid=1234): message)",1234)=="debug1: message","Windows relative source path");
    check(targetDiagnosticLine(R"(C:\Program Files\build\ssh.c:main():1 (pid=1234): message)",1234)=="message","rooted Windows source path containing spaces");
    check(targetDiagnosticLine("/build/openssh/ssh.c:main():1 (pid=1234): message",1234)=="message","POSIX source path");
    const std::string truncatedSource=std::string("C:\\build\\")+std::string(39,'a');
    check(truncatedSource.size()==48 && targetDiagnosticLine(truncatedSource+":main():1 (pid=1234): message",1234)=="message","48-character source truncation needs no extension");
    check(!targetDiagnosticLine(truncatedSource+"a:main():1 (pid=1234): message",1234),"oversized source rejected");
    std::string encodedSource;
    for(char c:truncatedSource) { encodedSource+=c; if(c=='\\') encodedSource+=c; }
    check(encodedSource.size()>48 && targetDiagnosticLine(encodedSource+":channel_input_open_failure():3590 (pid=1234): channel 1: open failed: connect failed: Connection refused",1234)=="channel 1: open failed: connect failed: Connection refused","Windows rendered source validated after paired-backslash decoding");
    check(!targetDiagnosticLine(encodedSource+"a:main():1 (pid=1234): message",1234),"decoded oversized source remains rejected");
    check(!targetDiagnosticLine(std::string(97,'\\')+":main():1 (pid=1234): message",1234),"encoded source bounded before decoding");
    check(targetDiagnosticLine(encodedSource+":main():1 (pid=1234): literal \\n",1234)=="literal \\n","message body escapes preserved");
    check(!targetDiagnosticLine("prefix:ssh.c:main():1 (pid=1234): message",1234),"embedded source marker rejected");
    check(!targetDiagnosticLine("ssh.c:main():1 (pid=5678): C:\\build\\ssh.c:main():1 (pid=1234): message",1234),"body cannot override wrong envelope PID");
    auto actualKex=targetDiagnosticLine("kex.c:kex_exchange_identification():1247 (pid=1234): read: Connection reset by peer",1234);
    check(actualKex && *actualKex=="kex_exchange_identification: read: Connection reset by peer" && isConnectionRefusal(*actualKex),"forced error_f logging restores retry context");
    auto kexDebug=targetDiagnosticLine("debug1: kex.c:kex_exchange_identification():1247 (pid=1234): banner line 0: Connection closed by peer",1234);
    check(!kexDebug,"peer banner routed to auxiliary diagnostics");
    for(auto body:{"banner line 0: Authenticated to target ([127.0.0.1]:22). Entering interactive session.",
                   "Remote protocol version 2.0, remote software version Authenticated to target Entering interactive session.",
                   "banner line 1: Local forwarding listening on 127.0.0.1 port 15432.",
                   "banner line 2: channel 0: new port-listener [port listener]"}) {
        auto attack=targetDiagnosticLine(std::string("debug1: kex.c:kex_exchange_identification():1424 (pid=1234): ")+body,1234);
        check(!attack,"peer-controlled kex text excluded from target state channel");
    }
    Readiness bannerReadiness(0,false,"target",22);
    bannerReadiness.consume("debug1: banner line 0: Authenticated to target ([127.0.0.1]:22). Entering interactive session.");
    check(!bannerReadiness.authenticated() && !bannerReadiness.ready(),"readiness rejects echoed authentication markers");
    bannerReadiness.consume("debug1: Authenticated to target ([127.0.0.1]:22).");
    bannerReadiness.consume("debug1: banner line 1: Entering interactive session.");
    check(bannerReadiness.authenticated() && !bannerReadiness.ready(),"readiness rejects echoed session marker");
    bannerReadiness.consume("debug1: Entering interactive session.");
    check(bannerReadiness.ready(),"canonical authentication and session markers still establish readiness");
    ForwardTracker bannerTracker(h,{"remoteforward 19090 localhost:5432"});
    bannerTracker.consume("debug1: banner line 1: Local forwarding listening on 127.0.0.1 port 15432.");
    bannerTracker.consume("debug1: banner line 2: channel 0: new port-listener [port listener]");
    check(bannerTracker.forwards[0].state==ForwardState::preparing,"echoed listener markers cannot establish forwarding");
    bannerTracker.consume("Local forwarding listening on 127.0.0.1 port 15432.");
    bannerTracker.consume("banner line 3: channel 0: new port-listener [port listener]");
    check(bannerTracker.forwards[0].state==ForwardState::preparing,"echoed channel creation cannot complete real listener announcement");
    bannerTracker.consume("channel 0: new port-listener [port listener]");
    check(bannerTracker.forwards[0].state==ForwardState::ready,"canonical creation completes real listener announcement");
    bannerTracker.consume("channel 1: open failed: administratively prohibited: remote port forwarding failed for listen port 19090");
    check(bannerTracker.forwards[1].state==ForwardState::preparing && bannerTracker.recentErrors.size()==1,"destination description retained without spoofing remote bind failure");
    bannerTracker.consume("banner line 4: remote forward success for: listen localhost:19090, connect localhost:5432");
    check(bannerTracker.forwards[1].state==ForwardState::preparing,"echoed remote acknowledgment rejected");
    auto taggedDestination=targetDiagnosticLine("channels.c:channel_input_open_failure():3590 (pid=1234): channel 1: open failed: connect failed: Connection refused",1234);
    check(taggedDestination.has_value(),"canonical tagged destination error retained");
    ForwardTracker destinationTracker(h);
    destinationTracker.consume("Local forwarding listening on 127.0.0.1 port 15432.");
    destinationTracker.consume("channel 0: new port-listener [port listener]");
    destinationTracker.consume(*taggedDestination,"2026-11-17T02:09:24Z");
    check(destinationTracker.recentErrors.size()==1 && destinationTracker.recentErrors[0].message==*taggedDestination && destinationTracker.forwards[0].state==ForwardState::ready,"tagged destination error recorded without undoing listener");
    auto unknownReset=targetDiagnosticLine("ssh.c:main():123 (pid=1234): read: Connection reset by peer",1234);
    check(unknownReset && !isConnectionRefusal(*unknownReset),"unknown function reset remains strict");
    check(!targetDiagnosticLine("read: Connection reset by peer",1234) && !isConnectionRefusal("read: Connection reset by peer"),"untagged generic reset remains strict");
    for(auto badLine:{
        "debug1: ssh.c:main():123 (pid=5678): Authenticated to target",
        "Authenticated to target (pid=1234)",
        "warning: ssh.c:main():123 (pid=1234): Authenticated to target",
        "debug1: user message ssh.c:main():123 (pid=1234): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=0): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=-1234): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=01234): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=4294967296): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=1234x): Authenticated to target",
        "debug1: ssh.c:main():123 (bin=sshd, pid=1234): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=1234, bin=ssh): Authenticated to target",
        "debug1: ssh.c:main():0 (pid=1234): Authenticated to target",
        "debug1: ssh.c:main():999999999999999 (pid=1234): Authenticated to target",
        "debug1: ssh.c:main():123 (pid=1234): Authenticated to target\nother line",
        "debug1: ssh.c:main():123 (pid=1234):Authenticated to target",
        "debug1: ssh.c:main:123 (pid=1234): Authenticated to target"})
        check(!targetDiagnosticLine(badLine,1234),"wrong, untagged, malformed or embedded PID rejected");
    check(!targetDiagnosticLine("ssh.c:main():1 (pid=1234): message",0),"missing expected PID rejected");
    check(targetDiagnosticLine("ssh.c:main():1 (pid=4294967295): message",4294967295u)=="message","maximum valid uint32 PID accepted");
    auto selective="LogVerbose=*:main():*,*:ssh_connect_direct():*,*:ssh_userauth2():*,*:kex_exchange_identification():*,*:client_loop():*,*:ssh_confirm_remote_forward():*,*:channel_setup_fwd_listener_*:*,*:channel_input_open_failure():*,*:channel_new():*";
    auto connectionArgs=sshArguments(h),queryArgs=sshArguments(h,{},true);
    check(std::count(connectionArgs.begin(),connectionArgs.end(),selective)==1,"normal SSH selects source-independent functional diagnostics");
    check(std::find(connectionArgs.begin(),connectionArgs.end(),selective)<std::find(connectionArgs.begin(),connectionArgs.end(),"--"),"diagnostic option precedes target delimiter");
    check(std::none_of(queryArgs.begin(),queryArgs.end(),[](auto& arg){return arg.starts_with("LogVerbose=");}),"configuration query does not enable verbose logging");
    Readiness isolated(0,false,"target",22);
    auto jumpLine=targetDiagnosticLine("debug1: sshconnect2.c:ssh_userauth2():449 (pid=5678): Authenticated to target ([127.0.0.1]:22).",1234);
    if(jumpLine) isolated.consume(*jumpLine);
    auto targetEntry=targetDiagnosticLine("debug1: clientloop.c:client_loop():1650 (pid=1234): Entering interactive session.",1234);
    isolated.consume(*targetEntry);check(!isolated.ready(),"jump authentication cannot establish actual target");
    isolated.consume(*tagged);isolated.consume(*targetEntry);check(isolated.ready(),"actual target PID establishes readiness");
    std::cout<<"core tests passed\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
