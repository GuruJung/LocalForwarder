#include "manager.hpp"
#include <algorithm>
#include <iostream>
#include <deque>
using namespace lf;
namespace {
void check(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action) { bool rejected=false;try{action();}catch(...){rejected=true;}check(rejected,"expected rejection"); }
Host host(std::string id="a") { Host h;h.id=id;h.name=id;h.target="server";h.forwards.push_back({"f",true,15432,"localhost",80});return h; }
class MockSession:public SSHSessionProtocol {
public:
    HostRuntime value;
    std::vector<Host> starts;
    bool delayed=false;
    std::deque<std::function<void()>> completions;
    const HostRuntime& runtime() const override {return value;}
    void start(const Host& h) override { starts.push_back(h);value.state=ConnectionState::connected;value.executionHost=h;value.lastError.clear();if(onChange)onChange(); }
    void stop(std::function<void()> completion) override {
        if(delayed) {value.state=ConnectionState::stopping;if(onChange)onChange();completions.push_back(std::move(completion));}
        else {value.state=ConnectionState::disconnected;if(onChange)onChange();completion();}
    }
    void finish() { auto done=std::move(completions.front());completions.pop_front();value.state=ConnectionState::disconnected;if(onChange)onChange();done(); }
    void lost() {value.state=ConnectionState::disconnected;value.lastError="lost";if(onChange)onChange();}
};
struct Clock {
    struct Task {double at;bool cancelled=false;std::function<void()> action;};
    double now=0;std::vector<std::shared_ptr<Task>> tasks;
    Cancel schedule(double seconds,std::function<void()> action) {auto t=std::make_shared<Task>(Task{now+seconds,false,std::move(action)});tasks.push_back(t);return [t]{t->cancelled=true;};}
    void advance(double delta) {
        now+=delta;
        for(;;) {
            auto it=std::find_if(tasks.begin(),tasks.end(),[&](auto& t){return !t->cancelled && t->at<=now;});
            if(it==tasks.end()) break;
            auto t=*it;t->cancelled=true;t->action();
        }
    }
};
struct Fixture {
    Config disk{{host("a"),host("b")}};bool damaged=false;Clock clock;
    std::vector<std::shared_ptr<MockSession>> sessions;
    ConnectionManager manager;
    Fixture():manager(StoreAccess{
        [this]{if(damaged)throw std::runtime_error("damaged");return disk;},
        [this](const std::string& id){if(damaged)throw std::runtime_error("damaged");for(auto& h:disk.hosts)if(h.id==id)return h;throw std::runtime_error("deleted");},
        [this](const std::string& id){if(damaged)throw std::runtime_error("damaged");std::erase_if(disk.hosts,[&](auto& h){return h.id==id;});}},
        [this]{auto s=std::make_shared<MockSession>();sessions.push_back(s);return s;},
        [this](double delay,std::function<void()> action){return clock.schedule(delay,std::move(action));}) {manager.reloadConfiguration();}
};
void independentAndIntent() {
    Fixture f;f.disk.hosts[1].autoConnect=false;f.manager.startAll();
    check(f.manager.runtime("a")->desiredConnected && f.manager.runtime("a")->state==ConnectionState::connected,"autostart a");
    check(!f.manager.runtime("b")->desiredConnected && f.sessions[1]->starts.empty(),"disabled b idle");
    f.manager.connect("b");check(f.manager.runtime("b")->desiredConnected && f.sessions[1]->starts.size()==1,"disabled manual connect");
    check(!f.disk.hosts[1].autoConnect,"manual doesn't change auto setting");
    f.manager.disconnect("a");f.manager.wake();f.clock.advance(5);
    check(f.sessions[0]->starts.size()==1 && !f.manager.runtime("a")->desiredConnected,"manual disconnected excluded");
    check(f.sessions[1]->starts.size()==1,"healthy peer untouched");
}
void recoveryBoundaries() {
    Fixture f;f.manager.startAll();f.sessions[0]->lost();f.manager.networkChanged(false);f.manager.networkChanged(true);
    f.clock.advance(2);f.manager.wake();f.clock.advance(4.99);check(f.sessions[0]->starts.size()==1,"no early recovery");
    f.clock.advance(.01);check(f.sessions[0]->starts.size()==2 && f.sessions[1]->starts.size()==1,"debounced recovery and healthy isolation");
    f.sessions[0]->lost();f.manager.wake();f.manager.networkChanged(false);f.clock.advance(10);check(f.sessions[0]->starts.size()==2,"offline cancels pending recovery");
    f.manager.networkChanged(true);f.manager.disconnect("a");f.clock.advance(5);check(f.sessions[0]->starts.size()==2,"manual operation wins");
    f.manager.connect("a");f.manager.wake();f.clock.advance(5);f.sessions[0]->lost();f.clock.advance(65);check(f.sessions[0]->starts.size()==3,"late disconnect waits for next event");
}
void startupAndInitialNetwork() {
    Fixture f;f.manager.networkChanged(true);f.manager.wake();f.clock.advance(100);
    check(f.sessions[0]->starts.empty() && !f.manager.runtime("a")->desiredConnected,"cancel startup no recovery intent");
    f.manager.startAll();f.sessions[0]->lost();f.manager.networkChanged(true);f.clock.advance(5);check(f.sessions[0]->starts.size()==1,"available notification not recovery");
    f.manager.sleep();f.manager.networkChanged(false);f.manager.networkChanged(true);f.clock.advance(5);check(f.sessions[0]->starts.size()==1,"sleep suppresses network recovery");
    f.manager.wake();f.clock.advance(5);check(f.sessions[0]->starts.size()==2,"wake recovers");
}
void temporaryAndSave() {
    Fixture f;f.manager.startAll();auto temporary=host("a");temporary.target="temporary";
    f.manager.connect(temporary,true);check(f.manager.runtime("a")->temporary && f.sessions[0]->starts.back().target=="temporary","temporary applied");
    f.manager.connect("a");check(f.sessions[0]->starts.back().target=="temporary","manual remembers temporary");
    f.sessions[0]->lost();f.manager.wake();f.clock.advance(5);check(f.sessions[0]->starts.back().target=="temporary","recovery remembers temporary");
    auto count=f.sessions[0]->starts.size();f.disk.hosts[0].target="saved";f.manager.saved(f.disk.hosts[0]);
    check(!f.manager.runtime("a")->temporary && f.sessions[0]->starts.size()==count && f.manager.runtime("a")->executionHost->target=="temporary","save keeps active snapshot");
    f.manager.connect("a");check(f.sessions[0]->starts.back().target=="saved","saved applied next time");
    f.disk.hosts[0].target="external";f.manager.connect("a");check(f.sessions[0]->starts.back().target=="external","manual reads disk");
    f.manager.connect(host("unsaved"),true);check(f.manager.hosts().size()==3,"temporary host visible");
    f.disk.hosts[1].autoConnect=false;f.manager.reconnectAll();
    check(!f.manager.runtime("unsaved")->desiredConnected && !f.manager.runtime("b")->desiredConnected,"full reconnect resets intents");
    check(f.manager.runtime("a")->executionHost->target=="external","full reconnect uses persisted config");
}
void failuresAndDeletion() {
    Fixture f;f.manager.startAll();auto bad=host("a");bad.target="bad name";
    rejects([&]{f.manager.connect(bad,true);});check(f.manager.runtime("a")->state==ConnectionState::connected,"invalid input preserves live connection");
    f.damaged=true;rejects([&]{f.manager.remove("a");});rejects([&]{f.manager.reconnectAll();});
    check(f.manager.runtime("a")->state==ConnectionState::connected && f.manager.runtime("b")->state==ConnectionState::connected,"damaged file doesn't disrupt peers");
    f.damaged=false;f.manager.remove("a");check(!f.manager.runtime("a") && f.manager.hosts().size()==1,"delete removes host");
    f.sessions[0]->lost();check(!f.manager.runtime("a"),"deleted callback ignored");
}
void delayedStopsAndQuit() {
    Fixture f;f.manager.startAll();auto a=f.sessions[0];auto b=f.sessions[1];a->delayed=b->delayed=true;
    auto second=host("a");second.target="second";f.manager.connect(second,true);
    auto third=second;third.target="third";f.manager.connect(third,true);
    a->finish();check(a->starts.size()==1,"stale stop completion cannot start");a->finish();check(a->starts.back().target=="third","latest stop completion starts selected snapshot");
    f.manager.reconnectAll();a->finish();check(a->starts.size()==2,"whole reconnect waits for all hosts");b->finish();check(a->starts.size()==3 && b->starts.size()==2,"whole barrier releases once peers stopped");
    f.manager.remove("a");bool exited=false;f.manager.onExit=[&]{exited=true;};f.manager.quit();
    check(!exited,"quit awaits removed host and remaining host");b->finish();check(!exited,"deleted host cleanup retained");a->finish();check(exited,"quit completed after all owned stops");
    f.clock.advance(100);check(a->starts.size()==3 && b->starts.size()==2,"quit schedules no restart");
}
void cancelledTimerCannotFireAfterDestruction() {
    Clock clock;auto s=std::make_shared<MockSession>();Config disk{{host()}};
    {
        ConnectionManager manager({[&]{return disk;},[&](const std::string&){return disk.hosts[0];},[](const std::string&){}},[&]{return s;},[&](double d,std::function<void()> a){return clock.schedule(d,std::move(a));});
        manager.startAll();s->lost();manager.wake();
    }
    // Simulate an already dispatched callback despite scheduler cancellation.
    for(auto& task:clock.tasks) task->action();
    check(s->starts.size()==1,"destroyed manager safely ignores callbacks");
}
}
int main() {
    try {independentAndIntent();recoveryBoundaries();startupAndInitialNetwork();temporaryAndSave();failuresAndDeletion();delayedStopsAndQuit();cancelledTimerCannotFireAfterDestruction();std::cout<<"manager tests passed\n";return 0;}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
