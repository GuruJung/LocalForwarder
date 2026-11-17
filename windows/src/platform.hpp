#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include "core.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace lf {
namespace fs = std::filesystem;
using Post = std::function<void(std::function<void()>)>;
inline constexpr const char* sshUpdateGuidance =
    "Windows OpenSSH 9.5 이상이 필요합니다. Windows Update 또는 Microsoft의 OpenSSH 배포판으로 직접 업데이트한 뒤 다시 연결하세요. "
    "앱이 사용하는 Windows\\System32\\OpenSSH\\ssh.exe의 버전은 해당 경로에서 ssh -V로 확인할 수 있습니다.";
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
std::string winError(DWORD error = GetLastError());
std::string newId();
std::wstring currentUserSID();
fs::path homeDirectory();
fs::path executablePath();
fs::path systemSSH();
fs::path expandHome(const std::string& path);
void protectPath(const fs::path& path);
void createPrivateDirectory(const fs::path& path);
void writePrivateFile(const fs::path& path, const std::string& bytes);
Config loadConfig(const fs::path& directory);
Host loadHost(const fs::path& directory, const std::string& id);
void saveHost(const fs::path& directory, const Host& host);
void deleteHost(const fs::path& directory, const std::string& id);
void setIdentity(const fs::path& directory, const std::string& id, const std::string& identity);

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { reset(other.release()); return *this; }
    HANDLE release() { auto h=value; value=nullptr; return h; }
    void reset(HANDLE h=nullptr) { if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); value=h; }
    explicit operator bool() const { return value && value!=INVALID_HANDLE_VALUE; }
};
class Child {
    Handle process_, job_, pipe_, completion_;
    void waitForJob();
    std::thread reader_;
public:
    using Callback = std::function<void(bool ended, const std::string& output, DWORD exitCode)>;
    using ProcessCallback = std::function<void(DWORD processId, bool ended, const std::string& output, DWORD exitCode)>;
    Child(const fs::path& executable, const std::vector<std::string>& args, Callback callback);
    Child(const fs::path& executable, const std::vector<std::string>& args, ProcessCallback callback);
    ~Child();
    Child(const Child&) = delete;
    void stop();
    DWORD wait(DWORD timeout);
};
struct Capture { DWORD code; std::string text; };
Capture capture(const fs::path& executable, const std::vector<std::string>& args, DWORD timeout = 20000);
int interactive(const fs::path& executable, const std::vector<std::string>& args,
                const std::optional<std::string>& input = std::nullopt);
void outputLine(const std::string& line);
// Returns a cancellation action for the GUI observer, not the independent terminal.
std::function<void()> launchRecovery(const std::string& profileId,
                                    std::function<void(int)> completed = {}, Post post = {});
std::string derivePublicKey(const fs::path& key);
int setupAuthentication(const fs::path& directory, const std::string& profileId,
                        const std::string& configuration = {}, bool attachTerminal = true);
}
