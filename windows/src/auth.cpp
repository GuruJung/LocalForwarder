#include "platform.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace lf {
namespace {
std::vector<std::string> authenticationArgs(const Host& host,bool batch,const std::string& configuration) {
    auto args=authArguments(host,batch);
    if(!configuration.empty()) args.insert(args.begin(),{"-F",configuration});
    return args;
}
int verify(const fs::path& ssh,const Host& host,const std::string& configuration) {
    auto args=authenticationArgs(host,true,configuration); args.push_back("true");
    for(int attempt=0;attempt<2;++attempt) {
        auto result=capture(ssh,args,20000);
        if(result.code==0) return 0;
        std::cout<<result.text<<std::flush;
        if(attempt || !host.retryOnConnectionRefused || !isConnectionRefusal(result.text)) break;
        std::cout<<"연결이 거부되어 1초 뒤 한 번 더 검증합니다.\n"<<std::flush; Sleep(1000);
    }
    return 1;
}
std::string checkedPublicKey(const std::string& value) {
    std::istringstream stream(value); std::string type,blob;
    stream>>type>>blob;
    if(!(type.rfind("ssh-",0)==0 || type.rfind("ecdsa-",0)==0) || blob.empty() ||
       blob.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")!=std::string::npos)
        throw std::runtime_error("개인키에서 유효한 공개키를 읽지 못했습니다.");
    auto result=value;
    while(!result.empty() && (result.back()=='\r' || result.back()=='\n')) result.pop_back();
    if(result.find_first_of("\r\n")!=std::string::npos) result=type+" "+blob;
    return result+"\n";
}
// Public key bytes are sent through stdin, never interpreted as shell source.
const std::string installCommand=R"LF(sh -c 'set -eu; umask 077; mkdir -p "$HOME/.ssh"; chmod 700 "$HOME/.ssh"; key=$(cat); blob=$(printf "%s\n" "$key" | awk "{print \$2}"); test -n "$blob"; touch "$HOME/.ssh/authorized_keys"; if ! awk -v wanted="$blob" "{for(i=1;i<=NF;i++) if (\$i == wanted) found=1} END {exit !found}" "$HOME/.ssh/authorized_keys"; then printf "\n%s\n" "$key" >> "$HOME/.ssh/authorized_keys"; fi; chmod 600 "$HOME/.ssh/authorized_keys"')LF";
}
std::string derivePublicKey(const fs::path& key) {
    std::cout << "공개키를 추출합니다. 기존 키에 암호가 있으면 입력하세요.\n" << std::flush;
    auto exe=systemSSH().parent_path()/L"ssh-keygen.exe";
    struct TemporaryDirectory {
        fs::path path;
        ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path,ignored); }
    } temp{fs::temp_directory_path()/wide("LocalForwarder-public-"+newId())};
    createPrivateDirectory(temp.path);
    auto output=temp.path/L"public-key";
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle file(CreateFileW(output.c_str(),GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file) throw std::runtime_error(winError());
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdError=GetStdHandle(STD_ERROR_HANDLE); si.hStdOutput=file.value;
    auto cmd=quoteWindowsArgument(exe.wstring())+L" -y -f "+quoteWindowsArgument(key.wstring());
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&si,&pi)) throw std::runtime_error(winError());
    Handle process(pi.hProcess),thread(pi.hThread); WaitForSingleObject(process.value,INFINITE);
    DWORD code=1; GetExitCodeProcess(process.value,&code); file.reset();
    if(code) throw std::runtime_error("공개키 추출에 실패했습니다.");
    auto readLine=[](const fs::path& path) {
        std::ifstream stream(path,std::ios::binary); std::string line; std::getline(stream,line);
        if(!line.empty() && line.back()=='\r') line.pop_back();
        return line;
    };
    auto derived=readLine(output);
    if(derived.empty()) throw std::runtime_error("추출한 공개키가 비어 있습니다.");
    // Only the selected private key determines which public key is authorized.
    // An existing companion file may supply a comment, never a different key.
    auto fields=[](const std::string& line) {
        std::istringstream stream(line); std::string type,blob; stream>>type>>blob;
        return std::pair{type,blob};
    };
    auto pub=key; pub+=L".pub";
    auto existing=readLine(pub);
    return !existing.empty() && fields(existing)==fields(derived) ? existing : derived;
}
int setupAuthentication(const fs::path& directory,const std::string& profileId,const std::string& configuration,bool attachTerminal) {
    if(attachTerminal) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        FILE* stream=nullptr;
        freopen_s(&stream,"CONIN$","r",stdin); freopen_s(&stream,"CONOUT$","w",stdout); freopen_s(&stream,"CONOUT$","w",stderr);
        SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    }
    try {
        auto host=loadHost(directory,profileId); auto ssh=systemSSH();
        auto version=capture(ssh,{"-V"},10000);
        if(version.code!=0 || !supportedOpenSSHVersion(version.text)) throw std::runtime_error(sshUpdateGuidance);
        std::cout<<"LocalForwarder 인증 설정 — "<<host.name<<"\n"
                 <<"서버 비밀번호와 기존 키 암호는 이 터미널에서만 입력합니다.\n"
                 <<"서버의 호스트 키 검증과 인증 정책은 변경하지 않습니다.\n"<<std::flush;
        auto effective=capture(ssh,sshArguments(host,configuration,true));
        if(effective.code) throw std::runtime_error(effective.text);
        std::vector<fs::path> keys;
        auto add=[&](const fs::path& path) {
            if(fs::is_regular_file(path) && std::find(keys.begin(),keys.end(),path)==keys.end()) keys.push_back(path);
        };
        if(!host.identityFile.empty()) {
            auto path=expandHome(host.identityFile);
            if(!fs::is_regular_file(path)) throw std::runtime_error("지정한 개인키 파일이 없습니다.");
            add(path);
        } else for(const auto& path:identityFiles(effective.text)) add(expandHome(path));
        auto appKey=directory/L"keys"/wide(profileId);
        if(keys.empty()) add(appKey);
        fs::path key;
        if(keys.size()>1) {
            std::cout<<"사용할 기존 키 번호를 선택하세요.\n";
            for(size_t index=0;index<keys.size();++index) std::cout<<index+1<<") "<<utf8(keys[index].wstring())<<"\n";
            std::cout<<"> "<<std::flush; std::string input; std::getline(std::cin,input);
            size_t consumed=0; auto index=std::stoul(input,&consumed);
            if(consumed!=input.size() || index<1 || index>keys.size()) throw std::runtime_error("키 선택을 취소했습니다.");
            key=keys[index-1];
        } else if(!keys.empty()) key=keys.front();
        else {
            key=appKey;
            createPrivateDirectory(key.parent_path());
            if(fs::exists(key) || fs::exists(fs::path(key.wstring()+L".pub"))) throw std::runtime_error("키 파일이 이미 있어 덮어쓰지 않습니다.");
            std::cout<<"암호 없는 새 Ed25519 키를 생성합니다: "<<utf8(key.wstring())<<"\n"<<std::flush;
            if(interactive(ssh.parent_path()/L"ssh-keygen.exe",{"-t","ed25519","-N","","-C","LocalForwarder/"+profileId,"-f",utf8(key.wstring())})) throw std::runtime_error("키 생성 실패");
            protectPath(key);
        }
        auto derived=capture(ssh.parent_path()/L"ssh-keygen.exe",{"-y","-P","","-f",utf8(key.wstring())});
        if(derived.code) {
            std::cout<<"기존 키가 암호화되어 있거나 잠겨 있습니다. 키 암호를 입력하여 SSH 에이전트에 등록합니다.\n"<<std::flush;
            if(interactive(ssh.parent_path()/L"ssh-add.exe",{utf8(key.wstring())})) {
                throw std::runtime_error("SSH 에이전트에 키를 등록하지 못했습니다. 기존 키는 보존했습니다.\n"
                    "ssh-agent 서비스가 꺼져 있다면 관리자 PowerShell에서 다음 명령을 직접 실행한 뒤 인증 설정을 다시 실행하세요:\n"
                    "Set-Service ssh-agent -StartupType Automatic\nStart-Service ssh-agent\n"
                    "앱은 관리자 권한을 요청하거나 서비스 설정을 변경하지 않습니다.");
            }
        }
        // Always derive from the selected private key; preserve a matching companion's comment only.
        auto publicKey=checkedPublicKey(derivePublicKey(key));
        host.identityFile=utf8(key.wstring());
        std::cout<<"원격 계정의 ~/.ssh/authorized_keys에 공개키를 추가합니다. 기존 키와 제한 옵션은 유지합니다.\n"<<std::flush;
        auto registration=authenticationArgs(host,false,configuration); registration.push_back(installCommand);
        if(interactive(ssh,registration,publicKey)) throw std::runtime_error("공개키 등록 실패. 터미널 오류와 원격 계정의 POSIX 셸·인증 정책을 확인하세요.");
        if(verify(ssh,host,configuration)) throw std::runtime_error("공개키는 등록했지만 입력 없는 접속을 검증하지 못했습니다. 서버 인증 정책과 SSH 에이전트 상태를 확인하세요.");
        setIdentity(directory,profileId,host.identityFile);
        std::cout<<"인증 설정이 완료되었습니다. LocalForwarder에서 이 호스트의 연결을 선택하세요.\n"<<std::flush;
        return 0;
    } catch(const std::exception& error) { std::cerr<<"인증 설정 실패: "<<error.what()<<"\n"<<std::flush; return 1; }
}
}
