#include "platform.hpp"
#include <fstream>
#include <iostream>
#include <barrier>
using namespace lf;
void check(bool x,const char* name){if(!x)throw std::runtime_error(name);}
int main(int argc,char** argv) {
 if(argc==2 && std::string(argv[1])=="--close-stdout") {
   CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
   const char message[]="stderr-survives\n"; DWORD written=0;
   return WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr) && written==sizeof(message)-1 ? 0 : 1;
 }
 auto root=fs::temp_directory_path()/wide("LF test 한글 ' "+newId());
 const char* stage="initialization";
 try {
   CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
   Host a; a.id="a";a.name="A";a.target="localhost";a.forwards.push_back({"f",true,15555,"localhost",80});
   stage="initial config CRUD";
   saveHost(root,a); auto b=a;b.id="b";b.name="B";saveHost(root,b);
   check(loadConfig(root).hosts.size()==2,"merge different hosts");
   a.name="Updated";saveHost(root,a);check(loadHost(root,"a").name=="Updated","fresh load");
   deleteHost(root,"b");check(loadConfig(root).hosts.size()==1,"delete");
   stage="concurrent config saves (100 per writer)";
   std::barrier start(2);std::exception_ptr failures[2];
   auto saveConcurrently=[&](int index) {
     try {
       auto host=a;host.name=index==0?"Left":"Right";
       start.arrive_and_wait();
       for(int n=0;n<100;++n)saveHost(root,host);
     }catch(...){failures[index]=std::current_exception();}
   };
   std::thread left(saveConcurrently,0),right(saveConcurrently,1);left.join();right.join();
   for(auto& failure:failures)if(failure)std::rethrow_exception(failure);
   auto raced=loadConfig(root);check(raced.hosts.size()==1 && (raced.hosts[0].name=="Left" || raced.hosts[0].name=="Right"),"concurrent saves remain valid JSON");
   a.name="Final save";saveHost(root,a);check(loadHost(root,"a").name=="Final save","last completed save is freshly loaded");
   stage="invalid config preservation";
   {std::ofstream f(root/L"config.json");f<<"broken";}
   bool threw=false;try{saveHost(root,a);}catch(...){threw=true;}check(threw,"broken config rejected");
   std::ifstream f(root/L"config.json");std::string s;f>>s;f.close();check(s=="broken","broken config preserved");
   stage="child process and independent stderr";
   auto result=capture(systemSSH(),{"-V"});check(result.code==0 && result.text.find("OpenSSH")!=std::string::npos,"real SSH process");
   auto redirected=capture(executablePath(),{"--close-stdout"});
   check(redirected.code==0 && redirected.text=="stderr-survives\n","stderr survives child closing stdout");
   stage="authentication key generation";
   auto keys=root/L"keys";fs::create_directory(keys);protectPath(keys);
   auto keygen=systemSSH().parent_path()/L"ssh-keygen.exe";
   auto selected=keys/L"selected",other=keys/L"other";
   for(auto& key:{selected,other}) {
     auto generated=capture(keygen,{"-q","-t","ed25519","-N","","-C","original-comment","-f",utf8(key.wstring())});
     check(generated.code==0,"generate authentication test key");protectPath(key);
   }
   auto read=[](const fs::path& path){std::ifstream stream(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(stream)),{});};
   auto pub=fs::path(selected.wstring()+L".pub");
   auto correct=read(pub);auto expected=correct;while(!expected.empty() && (expected.back()=='\r' || expected.back()=='\n'))expected.pop_back();
   stage="matching companion public key";
   check(derivePublicKey(selected)==expected,"matching public companion preserves comment");
   auto unrelated=read(fs::path(other.wstring()+L".pub"));
   {std::ofstream stream(pub,std::ios::binary|std::ios::trunc);stream<<unrelated;}
   stage="mismatched companion public key";
   auto derived=derivePublicKey(selected);
   auto keyFields=expected.substr(0,expected.find(' ',expected.find(' ')+1));
   check(derived==keyFields || derived.rfind(keyFields+" ",0)==0,"mismatched public companion never selects another key");
   check(read(pub)==unrelated,"public companion is never overwritten");
   stage="missing companion public key";
   fs::remove(pub);check(!derivePublicKey(selected).empty() && !fs::exists(pub),"missing public companion remains absent");
   stage="cleanup";
   fs::remove_all(root);CoUninitialize();std::cout<<"platform tests passed\n";return 0;
 } catch(const std::exception& e) {std::error_code ignored;fs::remove_all(root,ignored);std::cerr<<stage<<": "<<e.what()<<"\n";return 1;}
}
