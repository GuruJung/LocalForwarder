#include "login_startup.hpp"
#include <functional>
#include <iostream>
using namespace lf;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct FakeStartup final:LoginStartup {
    bool registered=false,failRead=false,failWrite=false,failAfterWrite=false,ignoreWrite=false;
    int reads=0,writes=0;std::function<void()> duringWrite;
    bool enabled()override{++reads;if(failRead)throw std::runtime_error("query failed");return registered;}
    void setEnabled(bool value)override{++writes;if(duringWrite)duringWrite();if(failWrite)throw std::runtime_error("write failed");if(!ignoreWrite)registered=value;if(failAfterWrite)failRead=true;}
};
int main(){try{
    auto fake=std::make_shared<FakeStartup>();LoginStartupController controller(fake);
    check(fake->reads==0&&fake->writes==0,"construction must not query or mutate startup");
    controller.refresh();check(controller.enabled()==false,"default unchecked");
    fake->registered=true;controller.refresh();check(controller.enabled()==true,"external registration reflected");
    auto reads=fake->reads;check(controller.toggle().empty()&&!fake->registered,"unregister existing");
    check(fake->reads==reads+2,"requery before and after mutation");
    fake->duringWrite=[&]{check(controller.busy(),"busy during mutation");controller.toggle();};
    auto writes=fake->writes;controller.toggle();check(fake->writes==writes+1,"duplicate toggle ignored");
    fake->duringWrite={};fake->failWrite=true;
    check(controller.toggle()=="write failed"&&controller.enabled()==true,"write failure preserves actual checkmark");
    fake->failWrite=false;fake->failRead=true;writes=fake->writes;
    check(controller.toggle()=="query failed"&&!controller.enabled().has_value()&&fake->writes==writes,"read failure prevents mutation");
    fake->failRead=false;fake->failAfterWrite=true;
    check(controller.toggle()=="query failed"&&!controller.enabled().has_value(),"failed verification remains unknown");
    fake->failAfterWrite=false;fake->failRead=false;controller.refresh();
    check(controller.enabled()==false,"later query recovers actual registration");
    fake->ignoreWrite=true;check(!controller.toggle().empty()&&controller.enabled()==false,"unconfirmed write cannot report success");
    std::cout<<"login startup controller checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
