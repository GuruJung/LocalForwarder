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
std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
std::string winError(DWORD error = GetLastError());
std::string newId();
fs::path homeDirectory();
fs::path executablePath();
fs::path systemSSH();
fs::path expandHome(const std::string& path);
void protectPath(const fs::path& path);
Config loadConfig(const fs::path& directory);
Host loadHost(const fs::path& directory, const std::string& id);
void saveHost(const fs::path& directory, const Host& host);
void deleteHost(const fs::path& directory, const std::string& id);

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
    Handle process_, job_, pipe_;
    std::thread reader_;
public:
    using Callback = std::function<void(bool ended, const std::string& output, DWORD exitCode)>;
    Child(const fs::path& executable, const std::vector<std::string>& args, Callback callback);
    ~Child();
    Child(const Child&) = delete;
    void stop();
    DWORD wait(DWORD timeout);
};
struct Capture { DWORD code; std::string text; };
Capture capture(const fs::path& executable, const std::vector<std::string>& args, DWORD timeout = 20000);
int interactive(const fs::path& executable, const std::vector<std::string>& args);
void outputLine(const std::string& line);
void launchRecovery(const std::string& profileId);
int setupAuthentication(const fs::path& directory, const std::string& profileId);
}
