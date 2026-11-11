#include "core.hpp"
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
    rejects([] {parseConfig("{bad");}); rejects([] {parseConfig("{\"version\":2,\"hosts\":[]}");});
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
    Machine m;check(m.begin(false,0)==Action::start,"begin");m.ready();
    m.disconnected(100);check(m.state==State::waitingReconnect && m.deadline==60100,"automatic delay");
    check(m.tick(60099)==Action::none,"no early reconnect");check(m.tick(60100)==Action::start && m.attempt==1,"automatic reconnect");
    m.failed(60200);check(m.deadline==61200 && m.timer==Timer::retry,"inner retry");
    check(m.tick(61200)==Action::start && m.attempt==2,"second attempt");
    m.failed(61300);check(m.state==State::waitingExit && m.deadline==121300,"failure exit delay");
    check(m.tick(121300)==Action::quit,"exit after retry exhausted");
    m.begin(false,0);m.failed(10);m.tick(1010);check(m.failed(1100)==Action::initialFailure,"initial fail exits");
    m.begin(false,0);m.ready();m.disconnected(1);m.begin(true,10);m.ready();
    check(m.tick(100000)==Action::none && m.state==State::connected,"manual success cancels timers");
    m.disconnected(100001);check(m.tick(300000)==Action::start,"resume expired automatic timer");
    m.quit();check(m.tick(999999)==Action::none,"quit cancels timer");
    std::cout<<"core tests passed\n";return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
