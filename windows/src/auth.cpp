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
        std::cout<<tr("win.connection_refused_retrying_verification_once_in_1_second_n")<<std::flush; Sleep(1000);
    }
    return 1;
}
std::string checkedPublicKey(const std::string& value) {
    std::istringstream stream(value); std::string type,blob;
    stream>>type>>blob;
    if(!(type.rfind("ssh-",0)==0 || type.rfind("ecdsa-",0)==0) || blob.empty() ||
       blob.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=")!=std::string::npos)
        throw LocalizedError(tr("win.could_not_read_a_valid_public_key_from_the_private_key"));
    auto result=value;
    while(!result.empty() && (result.back()=='\r' || result.back()=='\n')) result.pop_back();
    if(result.find_first_of("\r\n")!=std::string::npos) result=type+" "+blob;
    return result+"\n";
}
// Public key bytes are sent through stdin, never interpreted as shell source.
const std::string installCommand=R"LF(sh -c 'set -eu; umask 077; mkdir -p "$HOME/.ssh"; chmod 700 "$HOME/.ssh"; key=$(cat); blob=$(printf "%s\n" "$key" | awk "{print \$2}"); test -n "$blob"; touch "$HOME/.ssh/authorized_keys"; if ! awk -v wanted="$blob" "{for(i=1;i<=NF;i++) if (\$i == wanted) found=1} END {exit !found}" "$HOME/.ssh/authorized_keys"; then printf "\n%s\n" "$key" >> "$HOME/.ssh/authorized_keys"; fi; chmod 600 "$HOME/.ssh/authorized_keys"')LF";
}
std::string derivePublicKey(const fs::path& key) {
    std::cout << tr("win.extracting_the_public_key_enter_the_existing_key_passphrase_if_re") << std::flush;
    auto exe=systemSSH().parent_path()/L"ssh-keygen.exe";
    struct TemporaryDirectory {
        fs::path path;
        ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path,ignored); }
    } temp{fs::temp_directory_path()/wide("LocalForwarder-public-"+newId())};
    createPrivateDirectory(temp.path);
    auto output=temp.path/L"public-key";
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle file(CreateFileW(output.c_str(),GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file) throw LocalizedError(winError());
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE); si.hStdError=GetStdHandle(STD_ERROR_HANDLE); si.hStdOutput=file.value;
    auto cmd=quoteWindowsArgument(exe.wstring())+L" -y -f "+quoteWindowsArgument(key.wstring());
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&si,&pi)) throw LocalizedError(winError());
    Handle process(pi.hProcess),thread(pi.hThread); WaitForSingleObject(process.value,INFINITE);
    DWORD code=1; GetExitCodeProcess(process.value,&code); file.reset();
    if(code) throw LocalizedError(tr("win.public_key_extraction_failed"));
    auto readLine=[](const fs::path& path) {
        std::ifstream stream(path,std::ios::binary); std::string line; std::getline(stream,line);
        if(!line.empty() && line.back()=='\r') line.pop_back();
        return line;
    };
    auto derived=readLine(output);
    if(derived.empty()) throw LocalizedError(tr("win.the_extracted_public_key_is_empty"));
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
        if(version.code!=0 || !supportedOpenSSHVersion(version.text)) throw LocalizedError(sshUpdateGuidance);
        std::cout<<tr("win.authentication_heading",{{"host",host.name}})<<"\n"
                 <<tr("win.enter_server_passwords_and_existing_key_passphrases_only_in_this_")
                 <<tr("win.server_host_key_verification_and_authentication_policies_remain_u")<<std::flush;
        auto effective=capture(ssh,sshArguments(host,configuration,true));
        if(effective.code) throw LocalizedError(effective.text);
        std::vector<fs::path> keys;
        auto add=[&](const fs::path& path) {
            if(fs::is_regular_file(path) && std::find(keys.begin(),keys.end(),path)==keys.end()) keys.push_back(path);
        };
        if(!host.identityFile.empty()) {
            auto path=expandHome(host.identityFile);
            if(!fs::is_regular_file(path)) throw LocalizedError(tr("win.the_specified_private_key_file_does_not_exist"));
            add(path);
        } else for(const auto& path:identityFiles(effective.text)) add(expandHome(path));
        auto appKey=directory/L"keys"/wide(profileId);
        if(keys.empty()) add(appKey);
        fs::path key;
        if(keys.size()>1) {
            std::cout<<tr("win.choose_the_number_of_the_existing_key_to_use_n");
            for(size_t index=0;index<keys.size();++index) std::cout<<index+1<<") "<<utf8(keys[index].wstring())<<"\n";
            std::cout<<"> "<<std::flush; std::string input; std::getline(std::cin,input);
            size_t consumed=0; auto index=std::stoul(input,&consumed);
            if(consumed!=input.size() || index<1 || index>keys.size()) throw LocalizedError(tr("win.key_selection_was_cancelled"));
            key=keys[index-1];
        } else if(!keys.empty()) key=keys.front();
        else {
            key=appKey;
            createPrivateDirectory(key.parent_path());
            if(fs::exists(key) || fs::exists(fs::path(key.wstring()+L".pub"))) throw LocalizedError(tr("win.a_key_file_already_exists_and_will_not_be_overwritten"));
            std::cout<<tr("win.creating_a_new_ed25519_key_without_a_passphrase")<<utf8(key.wstring())<<"\n"<<std::flush;
            if(interactive(ssh.parent_path()/L"ssh-keygen.exe",{"-t","ed25519","-N","","-C","LocalForwarder/"+profileId,"-f",utf8(key.wstring())})) throw LocalizedError(tr("win.key_generation_failed"));
            protectPath(key);
        }
        auto derived=capture(ssh.parent_path()/L"ssh-keygen.exe",{"-y","-P","","-f",utf8(key.wstring())});
        if(derived.code) {
            std::cout<<tr("win.the_existing_key_is_encrypted_or_locked_enter_its_passphrase_to_r")<<std::flush;
            if(interactive(ssh.parent_path()/L"ssh-add.exe",{utf8(key.wstring())})) {
                throw LocalizedError(tr("win.could_not_register_the_key_with_the_ssh_agent_the_existing_key_wa") +
                    tr("win.if_ssh_agent_is_stopped_run_the_following_commands_in_administrat") +
                    "Set-Service ssh-agent -StartupType Automatic\nStart-Service ssh-agent\n" +
                    tr("win.the_app_does_not_request_administrator_privileges_or_change_servi"));
            }
        }
        // Always derive from the selected private key; preserve a matching companion's comment only.
        auto publicKey=checkedPublicKey(derivePublicKey(key));
        host.identityFile=utf8(key.wstring());
        std::cout<<tr("win.adding_the_public_key_to_ssh_authorized_keys_on_the_remote_accoun")<<std::flush;
        auto registration=authenticationArgs(host,false,configuration); registration.push_back(installCommand);
        if(interactive(ssh,registration,publicKey)) throw LocalizedError(tr("win.public_key_registration_failed_check_the_terminal_error_and_the_r"));
        if(verify(ssh,host,configuration)) throw LocalizedError(tr("win.the_public_key_was_registered_but_unattended_access_could_not_be_"));
        setIdentity(directory,profileId,host.identityFile);
        std::cout<<tr("win.authentication_setup_is_complete_select_connect_for_this_host_in_")<<std::flush;
        return 0;
    } catch(const std::exception& error) { std::cerr<<tr("win.authentication_setup_failed")<<error.what()<<"\n"<<std::flush; return 1; }
}
}
