#include "platform.hpp"
#include <shlobj.h>
#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <atomic>
#include <limits>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace lf {
std::wstring wide(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(!n) throw std::runtime_error("UTF-8 문자열이 올바르지 않습니다.");
    std::wstring out(n,0); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),out.data(),n); return out;
}
std::string utf8(const std::wstring& s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    if(!n) throw std::runtime_error("Unicode 문자열이 올바르지 않습니다.");
    std::string out(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr); return out;
}
std::string winError(DWORD code) {
    wchar_t* text=nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,code,0,reinterpret_cast<wchar_t*>(&text),0,nullptr);
    std::string result="Windows error "+std::to_string(code);
    if(text) { result += ": " + utf8(text); LocalFree(text); } return result;
}
std::string newId() { GUID id{}; if(FAILED(CoCreateGuid(&id))) throw std::runtime_error("ID 생성 실패"); wchar_t out[40]{}; StringFromGUID2(id,out,40); return utf8(std::wstring(out+1,36)); }
fs::path homeDirectory() { PWSTR path=nullptr; if(FAILED(SHGetKnownFolderPath(FOLDERID_Profile,0,nullptr,&path))) throw std::runtime_error("사용자 홈을 찾을 수 없습니다."); fs::path p(path); CoTaskMemFree(path); return p; }
fs::path executablePath() { std::wstring p(32768,0); auto n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size())); if(!n || n==p.size()) throw std::runtime_error("실행 경로를 찾을 수 없습니다."); p.resize(n); return p; }
fs::path systemSSH() { wchar_t path[MAX_PATH]{}; if(!GetSystemDirectoryW(path,MAX_PATH)) throw std::runtime_error(winError()); auto p=fs::path(path)/L"OpenSSH"/L"ssh.exe"; if(!fs::exists(p)) throw std::runtime_error("Windows OpenSSH Client가 없습니다. 설정 > 시스템 > 선택적 기능에서 OpenSSH 클라이언트를 설치하세요."); return p; }
fs::path expandHome(const std::string& path) { if(path=="~") return homeDirectory(); if(path.rfind("~/",0)==0 || path.rfind("~\\",0)==0) return homeDirectory()/wide(path.substr(2)); return fs::path(wide(path)); }
std::wstring currentUserSID() {
    HANDLE raw=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw)) throw std::runtime_error(winError());
    Handle token(raw); DWORD count=0;
    GetTokenInformation(token.value,TokenUser,nullptr,0,&count);
    std::vector<unsigned char> data(count);
    if(!GetTokenInformation(token.value,TokenUser,data.data(),count,&count)) throw std::runtime_error(winError());
    LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid)) throw std::runtime_error(winError());
    std::wstring result(sid); LocalFree(sid); return result;
}
namespace {
class PrivateSecurity {
    PSECURITY_DESCRIPTOR descriptor_=nullptr;
public:
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    PrivateSecurity() {
        auto sddl=L"O:"+currentUserSID()+L"D:P(A;OICI;FA;;;"+currentUserSID()+L")";
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor_,nullptr)) throw std::runtime_error(winError());
        attributes.lpSecurityDescriptor=descriptor_;
    }
    ~PrivateSecurity() { LocalFree(descriptor_); }
    PrivateSecurity(const PrivateSecurity&)=delete;
};
void requirePlain(HANDLE file, bool directory=false) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info)) throw std::runtime_error(winError());
    if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
       ((info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0)!=directory)
        throw std::runtime_error("설정 경로는 재분석 지점이 아닌 일반 파일/디렉터리여야 합니다.");
}
void writeBytes(HANDLE file,const std::string& bytes) {
    size_t offset=0;
    while(offset<bytes.size()) {
        DWORD count=0, part=static_cast<DWORD>(std::min<size_t>(bytes.size()-offset,1024*1024));
        if(!WriteFile(file,bytes.data()+offset,part,&count,nullptr) || !count) throw std::runtime_error("설정 파일 쓰기 실패: "+winError());
        offset+=count;
    }
    if(!FlushFileBuffers(file)) throw std::runtime_error("설정 파일 동기화 실패: "+winError());
}
Handle exclusiveFile(const fs::path& path) {
    PrivateSecurity security;
    Handle result(CreateFileW(path.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        &security.attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!result) throw std::runtime_error("설정 파일 생성 실패: "+winError());
    return result;
}
std::optional<std::string> readConfigBytes(const fs::path& directory) {
    auto path=directory/L"config.json";
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file) {
        auto error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return std::nullopt;
        throw std::runtime_error("설정 파일을 열 수 없습니다: "+winError(error));
    }
    requirePlain(file.value);
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.value,&size) || size.QuadPart<0 || static_cast<unsigned long long>(size.QuadPart)>std::numeric_limits<size_t>::max()) throw std::runtime_error("설정 파일 크기를 읽을 수 없습니다.");
    std::string result(static_cast<size_t>(size.QuadPart),'\0'); size_t offset=0;
    while(offset<result.size()) {
        DWORD count=0, part=static_cast<DWORD>(std::min<size_t>(result.size()-offset,1024*1024));
        if(!ReadFile(file.value,result.data()+offset,part,&count,nullptr)) throw std::runtime_error("설정 파일 읽기 실패: "+winError());
        if(!count) throw std::runtime_error("설정 파일이 읽는 동안 변경되었습니다.");
        offset+=count;
    }
    return result;
}
std::mutex mutationMutex;
class ConfigLock {
    Handle file_;
    OVERLAPPED overlap_{};
public:
    explicit ConfigLock(const fs::path& directory) {
        createPrivateDirectory(directory);
        PrivateSecurity security;
        auto path=directory/L"config.lock";
        file_.reset(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,
            &security.attributes,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!file_) throw std::runtime_error("설정 잠금 파일을 열 수 없습니다: "+winError());
        requirePlain(file_.value);
        PSECURITY_DESCRIPTOR descriptor=nullptr; PSID owner=nullptr, expected=nullptr;
        auto status=GetSecurityInfo(file_.value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor);
        if(status!=ERROR_SUCCESS) throw std::runtime_error(winError(status));
        bool valid=ConvertStringSidToSidW(currentUserSID().c_str(),&expected) && owner && EqualSid(owner,expected);
        if(expected) LocalFree(expected);
        LocalFree(descriptor);
        if(!valid) throw std::runtime_error("설정 잠금 파일의 소유자가 현재 사용자와 다릅니다.");
        if(!LockFileEx(file_.value,LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&overlap_)) throw std::runtime_error("설정을 잠글 수 없습니다: "+winError());
    }
    ~ConfigLock() { if(file_) UnlockFileEx(file_.value,0,1,0,&overlap_); }
};
void atomicConfig(const fs::path& directory,const std::string& content) {
    auto temp=directory/wide(".config-"+newId()+".tmp");
    auto file=exclusiveFile(temp);
    try {
        writeBytes(file.value,content);
        auto target=fs::absolute(directory/L"config.json").wstring();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->Flags=FILE_RENAME_FLAG_REPLACE_IF_EXISTS|FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,target.data(),rename->FileNameLength);
        if(!SetFileInformationByHandle(file.value,FileRenameInfoEx,rename,static_cast<DWORD>(buffer.size()))) throw std::runtime_error("설정 파일 교체 실패: "+winError());
    } catch(...) { file.reset(); std::error_code ignored; fs::remove(temp,ignored); throw; }
}
void mutateConfig(const fs::path& directory,const std::function<void(Config&)>& change) {
    std::lock_guard guard(mutationMutex);
    ConfigLock lock(directory);
    auto original=readConfigBytes(directory);
    Config config=original ? parseConfig(*original) : Config{};
    const bool migration=original && config.version==1;
    change(config);
    config.version=2;
    auto encoded=writeConfig(config);
    if(migration) writePrivateFile(directory/wide("config-v1-"+newId()+".json"),*original);
    atomicConfig(directory,encoded);
}
}
void protectPath(const fs::path& path) {
    PrivateSecurity security;
    BOOL present=FALSE,defaulted=FALSE; PACL acl=nullptr;
    if(!GetSecurityDescriptorDacl(security.attributes.lpSecurityDescriptor,&present,&acl,&defaulted)) throw std::runtime_error(winError());
    auto status=SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);
    if(status!=ERROR_SUCCESS) throw std::runtime_error(winError(status));
}
void createPrivateDirectory(const fs::path& path) {
    PrivateSecurity security;
    if(!CreateDirectoryW(path.c_str(),&security.attributes)) {
        auto error=GetLastError();
        if(error==ERROR_PATH_NOT_FOUND) {
            createPrivateDirectory(path.parent_path());
            if(!CreateDirectoryW(path.c_str(),&security.attributes) && GetLastError()!=ERROR_ALREADY_EXISTS) throw std::runtime_error(winError());
        } else if(error!=ERROR_ALREADY_EXISTS) throw std::runtime_error(winError(error));
    }
    Handle directory(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!directory) throw std::runtime_error(winError());
    requirePlain(directory.value,true); protectPath(path);
}
void writePrivateFile(const fs::path& path,const std::string& bytes) {
    auto file=exclusiveFile(path);
    try { writeBytes(file.value,bytes); }
    catch(...) { file.reset(); std::error_code ignored; fs::remove(path,ignored); throw; }
}
Config loadConfig(const fs::path& directory) {
    auto original=readConfigBytes(directory); return original ? parseConfig(*original) : Config{};
}
Host loadHost(const fs::path& directory,const std::string& id) {
    auto config=loadConfig(directory);
    auto found=std::find_if(config.hosts.begin(),config.hosts.end(),[&](const auto& host){return host.id==id;});
    if(found==config.hosts.end()) throw std::runtime_error("저장된 호스트를 찾을 수 없습니다. 설정에서 다시 선택하세요.");
    return *found;
}
void saveHost(const fs::path& directory,const Host& host) {
    validate(host);
    mutateConfig(directory,[&](Config& config) {
        auto found=std::find_if(config.hosts.begin(),config.hosts.end(),[&](const auto& value){return value.id==host.id;});
        if(found==config.hosts.end()) config.hosts.push_back(host); else *found=host;
    });
}
void deleteHost(const fs::path& directory,const std::string& id) {
    mutateConfig(directory,[&](Config& config){std::erase_if(config.hosts,[&](const auto& host){return host.id==id;});});
}
void setIdentity(const fs::path& directory,const std::string& id,const std::string& identity) {
    if(identity.empty()) throw std::runtime_error("개인키 경로가 비어 있습니다.");
    mutateConfig(directory,[&](Config& config) {
        auto found=std::find_if(config.hosts.begin(),config.hosts.end(),[&](const auto& host){return host.id==id;});
        if(found==config.hosts.end()) throw std::runtime_error("호스트가 삭제되어 개인키 설정을 저장하지 않았습니다.");
        found->identityFile=identity; validate(*found);
    });
}
static std::wstring commandLine(const fs::path& executable,const std::vector<std::string>& args) {
    auto out=quoteWindowsArgument(executable.wstring()); for(auto& arg:args) out+=L" "+quoteWindowsArgument(wide(arg)); return out;
}
Child::Child(const fs::path& exe,const std::vector<std::string>& args,Callback callback) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE}; HANDLE readRaw=nullptr,writeRaw=nullptr;
    if(!CreatePipe(&readRaw,&writeRaw,&sa,0)) throw std::runtime_error(winError());
    pipe_.reset(readRaw); Handle writer(writeRaw);
    // Win32 OpenSSH redirects and closes stdout before its forwarding loop.
    // Separate handles keep stderr valid when that happens, even on the same pipe.
    HANDLE errorRaw=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),writer.value,GetCurrentProcess(),&errorRaw,0,TRUE,DUPLICATE_SAME_ACCESS)) throw std::runtime_error(winError());
    Handle errorWriter(errorRaw);
    if(!SetHandleInformation(pipe_.value,HANDLE_FLAG_INHERIT,0)) throw std::runtime_error(winError());
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!input) throw std::runtime_error(winError());
    job_.reset(CreateJobObjectW(nullptr,nullptr)); if(!job_) throw std::runtime_error(winError());
    completion_.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1));
    if(!completion_) throw std::runtime_error(winError());
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey=this; association.CompletionPort=completion_.value;
    if(!SetInformationJobObject(job_.value,JobObjectAssociateCompletionPortInformation,&association,sizeof(association))) throw std::runtime_error(winError());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(job_.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error(winError());
    SIZE_T bytes=0; InitializeProcThreadAttributeList(nullptr,1,0,&bytes); std::vector<unsigned char> attrs(bytes);
    auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    if(!InitializeProcThreadAttributeList(list,1,0,&bytes)) throw std::runtime_error(winError());
    HANDLE inherit[]={input.value,writer.value,errorWriter.value};
    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherit,sizeof(inherit),nullptr,nullptr)) { auto e=GetLastError(); DeleteProcThreadAttributeList(list); throw std::runtime_error(winError(e)); }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=input.value; startup.StartupInfo.hStdOutput=writer.value; startup.StartupInfo.hStdError=errorWriter.value; startup.lpAttributeList=list;
    PROCESS_INFORMATION pi{}; auto cmd=commandLine(exe,args);
    std::vector<std::wstring> environment;
    if(auto* block=GetEnvironmentStringsW()) {
        for(auto* entry=block;*entry;entry+=wcslen(entry)+1) {
            std::wstring value(entry); auto equals=value.find(L'='); auto name=value.substr(0,equals);
            if(_wcsicmp(name.c_str(),L"LC_ALL") && _wcsicmp(name.c_str(),L"SSH_ASKPASS_REQUIRE") && _wcsicmp(name.c_str(),L"SSH_ASKPASS")) environment.push_back(value);
        }
        FreeEnvironmentStringsW(block);
    }
    environment.push_back(L"LC_ALL=C"); environment.push_back(L"SSH_ASKPASS_REQUIRE=never");
    std::sort(environment.begin(),environment.end(),[](auto& a,auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
    std::vector<wchar_t> block;
    for(auto& item:environment) {block.insert(block.end(),item.begin(),item.end());block.push_back(0);} block.push_back(0);
    BOOL created=CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT,block.data(),nullptr,&startup.StartupInfo,&pi);
    auto error=GetLastError(); DeleteProcThreadAttributeList(list);
    if(!created) throw std::runtime_error(winError(error));
    process_.reset(pi.hProcess); Handle thread(pi.hThread);
    if(!AssignProcessToJobObject(job_.value,process_.value)) { auto e=GetLastError(); TerminateProcess(process_.value,1); throw std::runtime_error(winError(e)); }
    if(ResumeThread(thread.value)==static_cast<DWORD>(-1)) { TerminateJobObject(job_.value,1); throw std::runtime_error(winError()); }
    writer.reset(); errorWriter.reset(); input.reset();
    reader_=std::thread([this,callback=std::move(callback)] {
        char data[4096]; DWORD size=0;
        while(ReadFile(pipe_.value,data,sizeof(data),&size,nullptr) && size) callback(false,std::string(data,size),0);
        WaitForSingleObject(process_.value,INFINITE); DWORD exit=1; GetExitCodeProcess(process_.value,&exit); callback(true,{},exit);
    });
}
Child::~Child() { stop(); waitForJob(); if(reader_.joinable()) reader_.join(); }
void Child::stop() { if(job_) TerminateJobObject(job_.value,1); }
void Child::waitForJob() {
    if(!job_ || !completion_) return;
    // Job termination is asynchronous. Wait for every descendant, including ones
    // that inherited no output handles, before reporting stop completion.
    for(;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if(!QueryInformationJobObject(job_.value,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),nullptr) || !accounting.ActiveProcesses) return;
        DWORD message=0; ULONG_PTR key=0; OVERLAPPED* overlap=nullptr;
        if(!GetQueuedCompletionStatus(completion_.value,&message,&key,&overlap,INFINITE)) return;
    }
}
DWORD Child::wait(DWORD timeout) {
    if(WaitForSingleObject(process_.value,timeout)!=WAIT_OBJECT_0) {
        stop(); waitForJob(); if(reader_.joinable()) reader_.join(); return 124;
    }
    DWORD code=1; GetExitCodeProcess(process_.value,&code);
    // Root exit must also release pipes inherited by proxy/query descendants.
    stop(); waitForJob(); if(reader_.joinable()) reader_.join(); return code;
}
Capture capture(const fs::path& executable,const std::vector<std::string>& args,DWORD timeout) {
    std::string result; Child child(executable,args,[&](bool ended,const std::string& text,DWORD){ if(!ended && result.size()<262144) result.append(text,0,262144-result.size()); });
    auto code=child.wait(timeout); return {code,result};
}
int interactive(const fs::path& executable,const std::vector<std::string>& args,const std::optional<std::string>& input) {
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION pi{};
    Handle reader,writer;
    if(input) {
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE}; HANDLE readRaw=nullptr,writeRaw=nullptr;
        if(!CreatePipe(&readRaw,&writeRaw,&security,0)) throw std::runtime_error(winError());
        reader.reset(readRaw); writer.reset(writeRaw);
        if(!SetHandleInformation(writer.value,HANDLE_FLAG_INHERIT,0)) throw std::runtime_error(winError());
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=reader.value; startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE); startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    }
    auto cmd=commandLine(executable,args);
    if(!CreateProcessW(executable.c_str(),cmd.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&pi)) throw std::runtime_error(winError());
    Handle process(pi.hProcess),thread(pi.hThread); reader.reset();
    std::thread feeder;
    if(input) feeder=std::thread([pipe=std::move(writer),bytes=*input]() mutable {
        size_t offset=0;
        while(offset<bytes.size()) {
            DWORD count=0;
            if(!WriteFile(pipe.value,bytes.data()+offset,static_cast<DWORD>(std::min<size_t>(bytes.size()-offset,8192)),&count,nullptr) || !count) break;
            offset+=count;
        }
    });
    WaitForSingleObject(process.value,INFINITE);
    if(feeder.joinable()) feeder.join();
    DWORD code=1; GetExitCodeProcess(process.value,&code); return static_cast<int>(code);
}
void outputLine(const std::string& line) { auto text=line+"\n"; DWORD count=0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data(),static_cast<DWORD>(text.size()),&count,nullptr); }
std::function<void()> launchRecovery(const std::string& id,std::function<void(int)> completed,Post post) {
    if(!safeId(id)) throw std::runtime_error("호스트 ID가 올바르지 않습니다.");
    auto token=newId();
    auto directory=homeDirectory()/L".LocalForwarder"/wide("authentication-"+token);
    createPrivateDirectory(directory);
    auto marker=directory/L"result";
    auto eventName=L"Local\\LocalForwarder.Authentication."+wide(token);
    PrivateSecurity security;
    Handle event(CreateEventW(&security.attributes,TRUE,FALSE,eventName.c_str()));
    Handle cancellation(CreateEventW(nullptr,TRUE,FALSE,nullptr));
    if(!event || !cancellation) { std::error_code ignored; fs::remove_all(directory,ignored); throw std::runtime_error(winError()); }
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system,MAX_PATH);
    auto powershell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
    // Only a validated ID and quoted application-owned paths enter this script.
    auto script=L"$code=1; try { $helper=Start-Process -FilePath "+quotePowerShell(executablePath().wstring())+
        L" -ArgumentList @('--setup-auth',"+quotePowerShell(wide(id))+L") -NoNewWindow -Wait -PassThru; $code=$helper.ExitCode } "
        L"finally { [IO.File]::WriteAllText("+quotePowerShell(marker.wstring())+
        L",[string]$code); try { $done=[Threading.EventWaitHandle]::OpenExisting("+quotePowerShell(eventName)+
        L"); [void]$done.Set(); $done.Dispose() } catch { Remove-Item -LiteralPath "+quotePowerShell(directory.wstring())+L" -Recurse -Force } }";
    auto cmd=commandLine(powershell,{"-NoProfile","-NoExit","-Command",utf8(script)});
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(powershell.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE,nullptr,nullptr,&startup,&pi)) {
        auto error=GetLastError(); std::error_code ignored; fs::remove_all(directory,ignored); throw std::runtime_error(winError(error));
    }
    CloseHandle(pi.hThread);
    auto cancel=std::make_shared<Handle>(std::move(cancellation));
    std::thread([event=std::move(event),process=Handle(pi.hProcess),cancel,directory,marker,completed=std::move(completed),post=std::move(post)]() mutable {
        HANDLE handles[]={event.value,process.value,cancel->value};
        auto result=WaitForMultipleObjects(3,handles,FALSE,INFINITE);
        if(result==WAIT_OBJECT_0+2) return; // The explicit external terminal remains independent.
        int code=1;
        if(result==WAIT_OBJECT_0) { std::ifstream file(marker); file>>code; }
        std::error_code ignored; fs::remove_all(directory,ignored);
        if(completed && WaitForSingleObject(cancel->value,0)!=WAIT_OBJECT_0) {
            auto callback=[cancel,completed=std::move(completed),code] {
                if(WaitForSingleObject(cancel->value,0)!=WAIT_OBJECT_0) completed(code);
            };
            if(post) post(std::move(callback)); else callback();
        }
    }).detach();
    return [cancel] { SetEvent(cancel->value); };
}
}
