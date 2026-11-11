#include "platform.hpp"
#include <fstream>
#include <iostream>
using namespace lf;
void check(bool x,const char* name){if(!x)throw std::runtime_error(name);}
int main(int argc,char** argv) {
 if(argc==2 && std::string(argv[1])=="--close-stdout") {
   CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
   const char message[]="stderr-survives\n"; DWORD written=0;
   return WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr) && written==sizeof(message)-1 ? 0 : 1;
 }
 auto root=fs::temp_directory_path()/wide("LF test 한글 "+newId());
 try {
   CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
   Host a; a.id="a";a.name="A";a.target="localhost";a.forwards.push_back({"f",true,15555,"localhost",80});
   saveHost(root,a); auto b=a;b.id="b";b.name="B";saveHost(root,b);
   check(loadConfig(root).hosts.size()==2,"merge different hosts");
   a.name="Updated";saveHost(root,a);check(loadHost(root,"a").name=="Updated","fresh load");
   deleteHost(root,"b");check(loadConfig(root).hosts.size()==1,"delete");
   {std::ofstream f(root/L"config.json");f<<"broken";}
   bool threw=false;try{saveHost(root,a);}catch(...){threw=true;}check(threw,"broken config rejected");
   std::ifstream f(root/L"config.json");std::string s;f>>s;f.close();check(s=="broken","broken config preserved");
   auto result=capture(systemSSH(),{"-V"});check(result.code==0 && result.text.find("OpenSSH")!=std::string::npos,"real SSH process");
   auto redirected=capture(executablePath(),{"--close-stdout"});
   check(redirected.code==0 && redirected.text=="stderr-survives\n","stderr survives child closing stdout");
   fs::remove_all(root);CoUninitialize();std::cout<<"platform tests passed\n";return 0;
 } catch(const std::exception& e) {std::error_code ignored;fs::remove_all(root,ignored);std::cerr<<e.what()<<"\n";return 1;}
}
