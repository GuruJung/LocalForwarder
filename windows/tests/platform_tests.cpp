#include "platform.hpp"
#include <fstream>
#include <iostream>
#include <barrier>
#include <exception>
#include <sstream>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
using namespace lf;
void check(bool x,const char* name){if(!x)throw std::runtime_error(name);}
std::string readBytes(const fs::path& path) {
 std::ifstream stream(path,std::ios::binary);
 return std::string((std::istreambuf_iterator<char>(stream)),{});
}
void privateACL(const fs::path& path) {
 PSECURITY_DESCRIPTOR descriptor=nullptr; PACL acl=nullptr; PSID owner=nullptr,expected=nullptr;
 auto result=GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|OWNER_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&descriptor);
 check(result==ERROR_SUCCESS,"read security descriptor");
 check(ConvertStringSidToSidW(currentUserSID().c_str(),&expected)!=FALSE,"parse current SID");
 bool valid=owner && EqualSid(owner,expected) && acl && acl->AceCount==1;
 if(valid) {
   void* pointer=nullptr; valid=GetAce(acl,0,&pointer)!=FALSE;
   if(valid) {
     auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(pointer);
     valid=ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE && EqualSid(&ace->SidStart,expected);
   }
 }
 SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision=0;
 valid=valid && GetSecurityDescriptorControl(descriptor,&control,&revision) && (control&SE_DACL_PROTECTED);
 LocalFree(expected); LocalFree(descriptor); check(valid,"current user only protected ACL");
}
// Failure fixtures own only disposable test paths and restore their metadata
// even when an assertion throws.
class RestoredAttributes {
 fs::path path_; DWORD original_;
public:
 explicit RestoredAttributes(const fs::path& path):path_(path),original_(GetFileAttributesW(path.c_str())) {
   if(original_==INVALID_FILE_ATTRIBUTES) throw std::runtime_error("read test file attributes: "+winError());
 }
 void readOnly() {if(!SetFileAttributesW(path_.c_str(),original_|FILE_ATTRIBUTE_READONLY))throw std::runtime_error("set test readonly attribute: "+winError());}
 void restore() {if(!SetFileAttributesW(path_.c_str(),original_))throw std::runtime_error("restore test file attributes: "+winError());}
 ~RestoredAttributes() {SetFileAttributesW(path_.c_str(),original_);}
};
class RestoredDirectoryDACL {
 fs::path path_; PSECURITY_DESCRIPTOR original_=nullptr; PACL originalACL_=nullptr;
public:
 explicit RestoredDirectoryDACL(const fs::path& path):path_(path) {
   auto error=GetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,
       nullptr,nullptr,&originalACL_,nullptr,&original_);
   if(error!=ERROR_SUCCESS)throw std::runtime_error("read test directory DACL: "+winError(error));
 }
 // No inheritance flags: deny FILE_ADD_FILE on this directory alone, leaving
 // the already-created config and lock file readable/writable.
 void setCreationDenied(bool denied) {
   auto sid=currentUserSID();auto sddl=L"D:P"+(denied?L"(D;;0x2;;;"+sid+L")":L"")+L"(A;;FA;;;"+sid+L")";
   PSECURITY_DESCRIPTOR descriptor=nullptr;
   if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))throw std::runtime_error(winError());
   BOOL present=FALSE,defaulted=FALSE;PACL acl=nullptr;
   auto valid=GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted);
   auto error=valid?SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()),SE_FILE_OBJECT,
       DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr):GetLastError();
   LocalFree(descriptor);
   if(error!=ERROR_SUCCESS)throw std::runtime_error("set test directory DACL: "+winError(error));
 }
 void restore() {
   auto error=SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()),SE_FILE_OBJECT,
       DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,originalACL_,nullptr);
   if(error!=ERROR_SUCCESS)throw std::runtime_error("restore test directory DACL: "+winError(error));
 }
 ~RestoredDirectoryDACL() {
   if(original_) {SetNamedSecurityInfoW(const_cast<LPWSTR>(path_.c_str()),SE_FILE_OBJECT,
       DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,originalACL_,nullptr);LocalFree(original_);}
 }
};
class HeldConfigLock {
 Handle file_;OVERLAPPED overlap_{};bool locked_=false;
public:
 explicit HeldConfigLock(const fs::path& path):file_(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
     FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)) {
   if(!file_ || !LockFileEx(file_.value,LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&overlap_))throw std::runtime_error("hold test config lock: "+winError());
   locked_=true;
 }
 void release() {if(locked_){UnlockFileEx(file_.value,0,1,0,&overlap_);locked_=false;}}
 ~HeldConfigLock(){release();}
};
class SecurityChangeNotification {
 HANDLE event_;
public:
 explicit SecurityChangeNotification(const fs::path& parent):event_(FindFirstChangeNotificationW(parent.c_str(),FALSE,FILE_NOTIFY_CHANGE_SECURITY)) {
   if(event_==INVALID_HANDLE_VALUE)throw std::runtime_error("watch test directory security: "+winError());
 }
 bool wait(DWORD milliseconds){return WaitForSingleObject(event_,milliseconds)==WAIT_OBJECT_0;}
 ~SecurityChangeNotification(){FindCloseChangeNotification(event_);}
};
int main(int argc,char** argv) {
 if(argc==2 && std::string(argv[1])=="--close-stdout") {
   CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE));
   const char message[]="stderr-survives\n"; DWORD written=0;
   return WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr) && written==sizeof(message)-1 ? 0 : 1;
 }
 int wideCount=0; auto* wideArgs=CommandLineToArgvW(GetCommandLineW(),&wideCount);
 std::vector<std::wstring> arguments;
 for(int index=0;index<wideCount;++index) arguments.emplace_back(wideArgs[index]);
 LocalFree(wideArgs);
 if(arguments.size()==4 && arguments[1]==L"--save-many") {
   try {
     auto id=utf8(arguments[3]); Host host;host.id=id;host.name=id;host.target="localhost";
     for(int index=0;index<80;++index) {host.name=id+"-"+std::to_string(index);saveHost(arguments[2],host);}
     return 0;
   } catch(const std::exception& error) {std::cerr<<error.what();return 1;}
 }
 if(arguments.size()==4 && arguments[1]==L"--identity-many") {
   try {
     for(int index=0;index<80;++index) setIdentity(arguments[2],utf8(arguments[3]),"key-"+std::to_string(index));
     return 0;
   } catch(const std::exception& error) {std::cerr<<error.what();return 1;}
 }
 if(arguments.size()==2 && arguments[1]==L"--sleep") {Sleep(30000);return 0;}
 if(arguments.size()==3 && arguments[1]==L"--spawn-descendant") {
   auto command=quoteWindowsArgument(executablePath().wstring())+L" --sleep";
   STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
   if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))return 1;
   {std::ofstream marker{fs::path(arguments[2])};marker<<process.dwProcessId;}
   CloseHandle(process.hThread);WaitForSingleObject(process.hProcess,INFINITE);CloseHandle(process.hProcess);return 0;
 }
 auto root=fs::temp_directory_path()/wide("LF test 한글 ' "+newId());
 const char* stage="initialization";
 try {
   CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
   Host a; a.id="a";a.name="A";a.target="localhost";a.forwards.push_back({"f",true,15555,"localhost",80});
   stage="v1 migration and user-only permissions";
   createPrivateDirectory(root);
   stage="supported system OpenSSH and PID-tagged diagnostics";
   auto version=capture(systemSSH(),{"-V"},5000);
   check(version.code==0 && supportedOpenSSHVersion(version.text),"Windows OpenSSH 9.5 or newer is required");
   auto diagnosticConfig=root/L"diagnostic-config";writePrivateFile(diagnosticConfig,"Host *\n  BatchMode yes\n");
   DWORD targetPid=0;std::string diagnosticText;
   Host diagnosticHost;diagnosticHost.id="diagnostic";diagnosticHost.name="Diagnostic";diagnosticHost.target="127.0.0.1";diagnosticHost.port=1;
   auto diagnosticArgs=sshArguments(diagnosticHost,utf8(diagnosticConfig.wstring()),false);
   diagnosticArgs.insert(diagnosticArgs.begin(),{"-o","ConnectTimeout=1","-o","ConnectionAttempts=1"});
   Child diagnostic(systemSSH(),diagnosticArgs,
       [&](DWORD pid,bool ended,const std::string& bytes,DWORD) {targetPid=pid;if(!ended)diagnosticText+=bytes;});
   auto diagnosticCode=diagnostic.wait(5000);
   std::string targetText;std::istringstream lines(diagnosticText);std::string line;
   while(std::getline(lines,line)) {
     if(auto target=targetDiagnosticLine(line,targetPid)) {
       targetText+=*target+"\n";
       check(!targetDiagnosticLine(line,targetPid+1),"another process cannot supply target diagnostics");
     }
   }
   if(diagnosticCode==124 || targetText.find("Connecting to 127.0.0.1")==std::string::npos) {
     // Preserve the production assertion; a diagnostic-only broad probe makes
     // an unexpected vendor log format visible in the failed native test.
     diagnosticArgs.insert(diagnosticArgs.begin(),{"-o","LogVerbose=*"});
     auto formatProbe=capture(systemSSH(),diagnosticArgs,5000);
     throw std::runtime_error("PID-tagged system OpenSSH diagnostics missing (PID="+std::to_string(targetPid)+", exit="+std::to_string(diagnosticCode)+"): "+diagnosticText+"\nVendor log format probe:\n"+formatProbe.text);
   }
   stage="v1 migration and user-only permissions";
   const std::string original="{ \"version\": 1, \"hosts\": [{\"id\":\"a\",\"name\":\"A\",\"target\":\"localhost\",\"forwards\":[]}] }\n";
   writePrivateFile(root/L"config.json",original);
   check(loadConfig(root).version==1,"v1 read supported");
   check(readBytes(root/L"config.json")==original,"v1 read does not migrate");
   saveHost(root,a);
   std::vector<fs::path> backups;
   for(const auto& entry:fs::directory_iterator(root)) if(entry.path().filename().wstring().rfind(L"config-v1-",0)==0) backups.push_back(entry.path());
   check(backups.size()==1 && readBytes(backups[0])==original,"exact original migration backup");
   check(loadConfig(root).version==2,"v2 saved");
   for(const auto& path:{root,root/L"config.json",root/L"config.lock",backups[0]}) privateACL(path);
   stage="backup creation failure preserves original v1 bytes";
   {
     auto directory=root/L"backup-denied";createPrivateDirectory(directory);
     writePrivateFile(directory/L"config.json",original);writePrivateFile(directory/L"config.lock",{});
     HeldConfigLock held(directory/L"config.lock");
     RestoredDirectoryDACL permissions(directory);
     // ConfigLock reapplies the private ACL before waiting. Start with a
     // different (non-inheriting) ACL so its security notification is observable.
     permissions.setCreationDenied(false);
     SecurityChangeNotification changed(root);
     std::string failure;
     Child saving(executablePath(),{"--save-many",utf8(directory.wstring()),"a"},
         [&](bool ended,const std::string& bytes,DWORD){if(!ended)failure+=bytes;});
     try {
       check(changed.wait(5000),"save reaches directory protection before blocked config lock");
       permissions.setCreationDenied(true);
       held.release();
       auto code=saving.wait(10000);
       if(code!=1 || failure.find("설정 파일 생성 실패:")==std::string::npos ||
           failure.find("Windows error "+std::to_string(ERROR_ACCESS_DENIED))==std::string::npos)
         throw std::runtime_error("expected backup CreateFile access denial, exit="+std::to_string(code)+": "+failure);
       check(readBytes(directory/L"config.json")==original,"backup failure preserves exact original v1 bytes");
       check(loadConfig(directory).version==1 && loadHost(directory,"a").name=="A","backup failure leaves original parseable without migration");
       size_t count=0;
       for(const auto& entry:fs::directory_iterator(directory)) {
         auto name=entry.path().filename();check(name==L"config.json" || name==L"config.lock","backup failure leaves no backup or temporary residue");++count;
       }
       check(count==2,"backup failure preserves only original config and lock");
       permissions.restore();
       saveHost(directory,a);
       std::vector<fs::path> recoveredBackups;
       for(const auto& entry:fs::directory_iterator(directory))if(entry.path().filename().wstring().rfind(L"config-v1-",0)==0)recoveredBackups.push_back(entry.path());
       check(loadConfig(directory).version==2 && recoveredBackups.size()==1 && readBytes(recoveredBackups[0])==original,
           "save after restoring permissions migrates with exact original backup");
     } catch(...) {held.release();saving.stop();saving.wait(5000);throw;}
   }
   stage="atomic replacement failure preserves original v2 bytes";
   {
     auto directory=root/L"replacement-denied";createPrivateDirectory(directory);saveHost(directory,a);
     auto path=directory/L"config.json";auto before=readBytes(path);RestoredAttributes attributes(path);attributes.readOnly();
     auto edited=a;edited.name="Must not replace original";std::string failure;
     try{saveHost(directory,edited);}catch(const std::exception& error){failure=error.what();}
     if(failure.find("설정 파일 교체 실패:")==std::string::npos)
       throw std::runtime_error("expected readonly destination atomic replacement failure: "+failure);
     check(readBytes(path)==before,"replacement failure preserves exact original config bytes");
     check(loadConfig(directory).version==2 && loadHost(directory,a.id).name==a.name,"replacement failure leaves original parseable");
     for(const auto& entry:fs::directory_iterator(directory))
       check(entry.path().filename().wstring().rfind(L".config-",0)!=0,"replacement failure removes temporary config file");
     attributes.restore();saveHost(directory,edited);
     check(loadHost(directory,a.id).name==edited.name,"save succeeds after restoring destination attributes");
   }
   stage="initial config CRUD";
   a.description="개발 DB";a.forwards={{"db",true,15432,"localhost",5432,"분석용 데이터베이스"}};saveHost(root,a); auto b=a;b.id="b";b.name="B";saveHost(root,b);
   check(loadConfig(root).hosts.size()==2,"merge different hosts");
   check(loadHost(root,"a").description=="개발 DB"&&loadHost(root,"a").forwards==a.forwards,"other host save preserves host and rule descriptions");
   auto unchangedBytes=readBytes(root/L"config.json");loadConfig(root);check(readBytes(root/L"config.json")==unchangedBytes,"v2 description read preserves exact file bytes");
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
   stage="cross-process saves preserve distinct hosts";
   auto concurrent=root/L"concurrent";createPrivateDirectory(concurrent);
   auto ignore=[](bool,const std::string&,DWORD){};
   Child first(executablePath(),{"--save-many",utf8(concurrent.wstring()),"left"},ignore);
   Child second(executablePath(),{"--save-many",utf8(concurrent.wstring()),"right"},ignore);
   check(first.wait(60000)==0 && second.wait(60000)==0,"concurrent processes complete");
   auto both=loadConfig(concurrent);
   check(both.hosts.size()==2,"distinct process writes preserve both hosts");
   check(loadHost(concurrent,"left").name=="left-79" && loadHost(concurrent,"right").name=="right-79","all process writes retained");
   auto describedIdentity=loadHost(concurrent,"left");describedIdentity.description="설명 보존";describedIdentity.forwards={{"db",true,15432,"localhost",5432,"인증 갱신 중 설명 보존"}};saveHost(concurrent,describedIdentity);
   Child identity(executablePath(),{"--identity-many",utf8(concurrent.wstring()),"left"},ignore);
   Child edit(executablePath(),{"--save-many",utf8(concurrent.wstring()),"right"},ignore);
   check(identity.wait(60000)==0 && edit.wait(60000)==0,"identity and editor processes complete");
   check(loadHost(concurrent,"left").name=="left-79" && loadHost(concurrent,"left").identityFile=="key-79","identity mutation preserves latest other fields");
   check(loadHost(concurrent,"right").name=="right-79","identity mutation preserves other host");
   check(loadHost(concurrent,"left").description=="설명 보존"&&loadHost(concurrent,"left").forwards==describedIdentity.forwards,"identity mutation preserves host and rule descriptions");
   bool emptyRejected=false;try{setIdentity(concurrent,"left","");}catch(...){emptyRejected=true;}
   check(emptyRejected && loadHost(concurrent,"left").identityFile=="key-79","empty helper key update rejected");
   deleteHost(concurrent,"left");bool deletedRejected=false;
   try{setIdentity(concurrent,"left","deleted-key");}catch(...){deletedRejected=true;}
   check(deletedRejected && loadConfig(concurrent).hosts.size()==1,"helper never resurrects deleted host");
   stage="lock failure preserves config";
   auto blocked=root/L"blocked";createPrivateDirectory(blocked);writePrivateFile(blocked/L"config.json",original);
   createPrivateDirectory(blocked/L"config.lock");bool lockRejected=false;
   try{saveHost(blocked,a);}catch(...){lockRejected=true;}
   check(lockRejected && readBytes(blocked/L"config.json")==original,"invalid lock path preserves original");
   stage="invalid config preservation";
   {std::ofstream f(root/L"config.json");f<<"broken";}
   bool threw=false;try{saveHost(root,a);}catch(...){threw=true;}check(threw,"broken config rejected");
   std::ifstream f(root/L"config.json");std::string s;f>>s;f.close();check(s=="broken","broken config preserved");
   stage="child process and independent stderr";
   auto result=capture(systemSSH(),{"-V"});check(result.code==0 && result.text.find("OpenSSH")!=std::string::npos,"real SSH process");
   auto redirected=capture(executablePath(),{"--close-stdout"});
   check(redirected.code==0 && redirected.text=="stderr-survives\n","stderr survives child closing stdout");
   stage="timeout kills query descendants";
   auto childMarker=root/L"descendant.pid";
   auto timeout=capture(executablePath(),{"--spawn-descendant",utf8(childMarker.wstring())},2000);
   check(timeout.code==124,"capture timeout");
   DWORD childPID=0;{std::ifstream marker(childMarker);marker>>childPID;}check(childPID!=0,"descendant started");
   Handle descendant(OpenProcess(SYNCHRONIZE,FALSE,childPID));
   check(!descendant || WaitForSingleObject(descendant.value,3000)==WAIT_OBJECT_0,"owned descendant stopped");
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
