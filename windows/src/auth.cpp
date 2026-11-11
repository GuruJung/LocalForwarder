#include "platform.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <set>

namespace lf {
namespace {
int runTwice(const fs::path& exe,const std::vector<std::string>& args) {
    int code=interactive(exe,args);
    if(code) { std::cout << "첫 시도가 실패했습니다. 1초 뒤 한 번 더 시도합니다.\n" << std::flush; Sleep(1000); code=interactive(exe,args); }
    return code;
}
int verify(const fs::path& ssh,const Host& host) {
    auto args=authArguments(host,true); args.push_back("true");
    for(int attempt=0;attempt<2;++attempt) {
        auto result=capture(ssh,args,20000);
        if(result.code==0) return 0;
        std::cout << result.text << std::flush;
        if(attempt==0) Sleep(1000);
    }
    return 1;
}
std::string readPublic(const fs::path& key) {
    auto pub=key; pub+=L".pub";
    if(!fs::exists(pub)) {
        std::cout << "공개키를 추출합니다. 기존 키에 암호가 있으면 입력하세요.\n" << std::flush;
        auto exe=systemSSH().parent_path()/L"ssh-keygen.exe";
        auto temp=pub; temp+=wide("."+newId()+".tmp");
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        Handle file(CreateFileW(temp.c_str(),GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!file) throw std::runtime_error(winError());
        STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
        si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdError=GetStdHandle(STD_ERROR_HANDLE); si.hStdOutput=file.value;
        auto cmd=quoteWindowsArgument(exe.wstring())+L" -y -f "+quoteWindowsArgument(key.wstring());
        PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&si,&pi)) { file.reset(); fs::remove(temp); throw std::runtime_error(winError()); }
        Handle process(pi.hProcess),thread(pi.hThread); WaitForSingleObject(process.value,INFINITE);
        DWORD code=1; GetExitCodeProcess(process.value,&code); file.reset();
        if(code) { fs::remove(temp); throw std::runtime_error("공개키 추출에 실패했습니다."); }
        if(!MoveFileExW(temp.c_str(),pub.c_str(),MOVEFILE_WRITE_THROUGH)) { fs::remove(temp); throw std::runtime_error("공개키 파일을 저장할 수 없습니다."); }
    }
    std::ifstream file(pub,std::ios::binary); std::string line; std::getline(file,line);
    if(!line.empty() && line.back()=='\r') line.pop_back();
    if(line.empty()) throw std::runtime_error("공개키 파일이 비어 있습니다."); return line;
}
}
int setupAuthentication(const fs::path& directory,const std::string& profileId) {
    // The GUI-subsystem executable is explicitly attached to its external terminal only here.
    AttachConsole(ATTACH_PARENT_PROCESS);
    FILE* stream=nullptr;
    freopen_s(&stream,"CONIN$","r",stdin); freopen_s(&stream,"CONOUT$","w",stdout); freopen_s(&stream,"CONOUT$","w",stderr);
    SetConsoleCP(CP_UTF8); SetConsoleOutputCP(CP_UTF8);
    try {
        auto host=loadHost(directory,profileId); auto ssh=systemSSH();
        std::cout << "LocalForwarder 인증 설정 — " << host.name << "\n"
                  << "서버 비밀번호와 기존 키 암호는 이 터미널에서만 입력합니다.\n"
                  << "먼저 호스트 지문을 확인하고 로그인합니다. 변경된 지문을 무시하지 마세요.\n" << std::flush;
        auto interactiveArgs=authArguments(host,false); interactiveArgs.push_back("true");
        if(runTwice(ssh,interactiveArgs)) throw std::runtime_error("대화형 로그인 실패. 서버 주소·계정·호스트 키를 확인하세요.");
        if(verify(ssh,host)==0) { std::cout << "입력 없는 접속이 정상입니다. 앱을 다시 실행하세요.\n"; return 0; }
        auto effective=capture(ssh,sshArguments(host,{},true));
        if(effective.code) throw std::runtime_error(effective.text);
        std::vector<fs::path> keys;
        auto add=[&](const fs::path& p){ if(fs::is_regular_file(p) && std::find(keys.begin(),keys.end(),p)==keys.end()) keys.push_back(p); };
        if(!host.identityFile.empty()) {
            auto explicitKey=expandHome(host.identityFile);
            if(!fs::is_regular_file(explicitKey)) throw std::runtime_error("지정한 개인키 파일이 없습니다.");
            add(explicitKey);
        }
        if(keys.empty()) for(auto& path:identityFiles(effective.text)) add(expandHome(path));
        auto appKey=directory/L"keys"/wide(profileId);
        if(keys.empty()) add(appKey);
        fs::path key;
        bool created=false;
        if(keys.size()>1) {
            std::cout << "사용할 기존 키 번호를 선택하세요.\n";
            for(size_t i=0;i<keys.size();++i) std::cout<<i+1<<") "<<utf8(keys[i].wstring())<<"\n";
            std::cout<<"> "<<std::flush; std::string input; std::getline(std::cin,input);
            size_t consumed=0; auto index=std::stoul(input,&consumed);
            if(consumed!=input.size() || index<1 || index>keys.size()) throw std::runtime_error("잘못된 키 번호입니다."); key=keys[index-1];
        } else if(!keys.empty()) key=keys.front();
        else {
            key=appKey;
            fs::create_directories(key.parent_path()); protectPath(key.parent_path());
            if(fs::exists(key) || fs::exists(fs::path(key.wstring()+L".pub"))) throw std::runtime_error("키 파일이 이미 있어 덮어쓰지 않습니다.");
            auto keygen=ssh.parent_path()/L"ssh-keygen.exe";
            std::cout<<"암호 없는 새 Ed25519 키를 생성합니다: "<<utf8(key.wstring())<<"\n"<<std::flush;
            if(interactive(keygen,{"-t","ed25519","-N","","-C","LocalForwarder-"+profileId,"-f",utf8(key.wstring())})) throw std::runtime_error("키 생성 실패");
            protectPath(key); created=true;
        }
        auto publicKey=readPublic(key);
        if(!created) {
            std::cout<<"기존 키를 SSH 에이전트에 등록합니다. 키 암호가 있으면 입력하세요.\n"<<std::flush;
            if(interactive(ssh.parent_path()/L"ssh-add.exe",{utf8(key.wstring())})) {
                std::cout<<"에이전트 등록에 실패했습니다. 암호화 키를 쓰는 경우 관리자 PowerShell에서\n"
                         <<"Set-Service ssh-agent -StartupType Automatic; Start-Service ssh-agent\n"
                         <<"를 실행하고 이 도우미를 다시 실행하세요. 암호 없는 키는 계속 사용할 수 있습니다.\n"<<std::flush;
            }
        }
        host.identityFile=utf8(key.wstring());
        std::cout<<"원격 계정의 ~/.ssh/authorized_keys에 공개키를 추가합니다. 기존 키는 유지합니다.\n"<<std::flush;
        auto registration=authArguments(host,false); registration.push_back(registrationCommand(publicKey));
        if(runTwice(ssh,registration)) throw std::runtime_error("공개키 등록 실패. 원격 홈 권한과 POSIX 셸 지원을 확인하세요.");
        if(verify(ssh,host)) throw std::runtime_error("입력 없는 접속 검증 실패. 서버의 공개키 인증 정책, 추가 인증, 사용자 정의 AuthorizedKeysFile 또는 SSH 에이전트 상태를 확인하세요.");
        auto fresh=loadHost(directory,profileId); fresh.identityFile=host.identityFile; saveHost(directory,fresh);
        std::cout<<"인증 설정이 완료되었습니다. LocalForwarder를 다시 실행하세요.\n"<<std::flush; return 0;
    } catch(const std::exception& e) { std::cerr<<"인증 설정 실패: "<<e.what()<<"\n"<<std::flush; return 1; }
}
}
