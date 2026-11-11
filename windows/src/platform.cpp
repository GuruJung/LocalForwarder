#include "platform.hpp"
#include <shlobj.h>
#include <aclapi.h>
#include <objbase.h>
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
void protectPath(const fs::path& path) {
    HANDLE tokenRaw=nullptr; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&tokenRaw)) throw std::runtime_error(winError()); Handle token(tokenRaw);
    DWORD size=0; GetTokenInformation(token.value,TokenUser,nullptr,0,&size); std::vector<unsigned char> data(size);
    if(!GetTokenInformation(token.value,TokenUser,data.data(),size,&size)) throw std::runtime_error(winError());
    auto* user=reinterpret_cast<TOKEN_USER*>(data.data()); EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions=FILE_ALL_ACCESS; entry.grfAccessMode=SET_ACCESS; entry.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    entry.Trustee.TrusteeForm=TRUSTEE_IS_SID; entry.Trustee.TrusteeType=TRUSTEE_IS_USER; entry.Trustee.ptstrName=reinterpret_cast<LPWSTR>(user->User.Sid);
    PACL acl=nullptr; auto status=SetEntriesInAclW(1,&entry,nullptr,&acl); if(status!=ERROR_SUCCESS) throw std::runtime_error(winError(status));
    status=SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr);
    LocalFree(acl); if(status!=ERROR_SUCCESS) throw std::runtime_error(winError(status));
}
Config loadConfig(const fs::path& directory) {
    auto path=directory/L"config.json"; if(!fs::exists(path)) return {};
    // A concurrent instance may atomically replace the path while this handle
    // reads the old file. Sharing deletion allows that without locks or retries.
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file) throw std::runtime_error("설정 파일을 열 수 없습니다: "+winError());
    LARGE_INTEGER size{}; if(!GetFileSizeEx(file.value,&size)) throw std::runtime_error("GetFileSizeEx(config.json): "+winError());
    if(size.QuadPart>4*1024*1024) throw std::runtime_error("설정 파일이 너무 큽니다 (최대 4 MiB).");
    std::string text(static_cast<size_t>(size.QuadPart),'\0'); size_t offset=0;
    while(offset<text.size()) {
        DWORD count=0;
        if(!ReadFile(file.value,text.data()+offset,static_cast<DWORD>(text.size()-offset),&count,nullptr)) throw std::runtime_error("설정 파일을 읽을 수 없습니다: "+winError());
        if(!count) break; offset+=count;
    }
    text.resize(offset);
    return parseConfig(text);
}
Host loadHost(const fs::path& directory,const std::string& id) {
    auto c=loadConfig(directory); auto it=std::find_if(c.hosts.begin(),c.hosts.end(),[&](auto& h){return h.id==id;});
    if(it==c.hosts.end()) throw std::runtime_error("저장된 호스트를 찾을 수 없습니다. 설정에서 다시 선택하세요."); return *it;
}
static void saveConfig(const fs::path& directory,const Config& config) {
    fs::create_directories(directory); auto temp=directory/wide("config."+newId()+".tmp");
    auto content=writeConfig(config);
    Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file) throw std::runtime_error("CreateFile(config temporary): "+winError());
    try {
        DWORD written=0;
        if(!WriteFile(file.value,content.data(),static_cast<DWORD>(content.size()),&written,nullptr)) throw std::runtime_error("WriteFile(config temporary): "+winError());
        if(written!=content.size()) throw std::runtime_error("WriteFile(config temporary): incomplete write");
        if(!FlushFileBuffers(file.value)) throw std::runtime_error("FlushFileBuffers(config temporary): "+winError());
        // Windows 11 POSIX rename semantics replace an open target atomically.
        // MoveFileEx can leave the target delete-pending during concurrent reads.
        auto target=fs::absolute(directory/L"config.json").wstring();
        std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO)+target.size()*sizeof(wchar_t));
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        rename->Flags=FILE_RENAME_FLAG_REPLACE_IF_EXISTS|FILE_RENAME_FLAG_POSIX_SEMANTICS;
        rename->FileNameLength=static_cast<DWORD>(target.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,target.data(),rename->FileNameLength);
        if(!SetFileInformationByHandle(file.value,FileRenameInfoEx,rename,static_cast<DWORD>(buffer.size()))) throw std::runtime_error("SetFileInformationByHandle(FileRenameInfoEx, config.json): "+winError());
    } catch(...) { file.reset(); std::error_code e; fs::remove(temp,e); throw; }
}
void saveHost(const fs::path& directory,const Host& host) {
    validate(host); auto c=loadConfig(directory); auto it=std::find_if(c.hosts.begin(),c.hosts.end(),[&](auto& h){return h.id==host.id;});
    if(it==c.hosts.end()) c.hosts.push_back(host); else *it=host; saveConfig(directory,c);
}
void deleteHost(const fs::path& directory,const std::string& id) {
    auto c=loadConfig(directory); std::erase_if(c.hosts,[&](auto& h){return h.id==id;}); saveConfig(directory,c);
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
Child::~Child() { stop(); if(reader_.joinable()) reader_.join(); }
void Child::stop() { if(job_) TerminateJobObject(job_.value,1); }
DWORD Child::wait(DWORD timeout) {
    if(WaitForSingleObject(process_.value,timeout)!=WAIT_OBJECT_0) { stop(); if(reader_.joinable()) reader_.join(); return 124; }
    if(reader_.joinable()) reader_.join(); DWORD code=1; GetExitCodeProcess(process_.value,&code); return code;
}
Capture capture(const fs::path& executable,const std::vector<std::string>& args,DWORD timeout) {
    std::string result; Child child(executable,args,[&](bool ended,const std::string& text,DWORD){ if(!ended && result.size()<262144) result.append(text,0,262144-result.size()); });
    auto code=child.wait(timeout); return {code,result};
}
int interactive(const fs::path& executable,const std::vector<std::string>& args) {
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION pi{};
    auto cmd=commandLine(executable,args);
    if(!CreateProcessW(executable.c_str(),cmd.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&pi)) throw std::runtime_error(winError());
    Handle process(pi.hProcess),thread(pi.hThread); WaitForSingleObject(process.value,INFINITE); DWORD code=1; GetExitCodeProcess(process.value,&code); return static_cast<int>(code);
}
void outputLine(const std::string& line) { auto text=line+"\n"; DWORD count=0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data(),static_cast<DWORD>(text.size()),&count,nullptr); }
void launchRecovery(const std::string& id) {
    if(!safeId(id)) throw std::runtime_error("호스트 ID가 올바르지 않습니다.");
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system,MAX_PATH);
    auto powershell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
    auto script=L"& "+quotePowerShell(executablePath().wstring())+L" '--setup-auth' "+quotePowerShell(wide(id))+L" | Out-Host";
    std::vector<std::string> args={"-NoProfile","-NoExit","-Command",utf8(script)};
    auto cmd=commandLine(powershell,args); STARTUPINFOW si{}; si.cb=sizeof(si); PROCESS_INFORMATION pi{};
    if(!CreateProcessW(powershell.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE,nullptr,nullptr,&si,&pi)) throw std::runtime_error(winError());
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}
}
