#pragma once
#include "platform.hpp"
#include "login_startup.hpp"

namespace lf {
// Injectable key location permits real Win32 registry tests without touching Run.
class RegistryLoginStartup final : public LoginStartup {
    fs::path executable_;
    HKEY root_;
    std::wstring subkey_;
public:
    explicit RegistryLoginStartup(fs::path executable,
        HKEY root=HKEY_CURRENT_USER,
        std::wstring subkey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run")
        :executable_(std::move(executable)),root_(root),subkey_(std::move(subkey)) {}
    bool enabled() override;
    void setEnabled(bool enabled) override;
};

// Refresh immediately before building each popup, including after external OS changes.
void appendLoginStartupMenu(HMENU menu,UINT command,LoginStartupController& controller);
}
