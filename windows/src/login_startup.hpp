#pragma once
#include "localization.hpp"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace lf {
// The OS registration is authoritative; this state is never persisted to config.json.
class LoginStartup {
public:
    virtual ~LoginStartup() = default;
    virtual bool enabled() = 0;
    virtual void setEnabled(bool enabled) = 0;
};

class LoginStartupController {
    std::shared_ptr<LoginStartup> adapter_;
    std::optional<bool> enabled_;
    std::string queryError_;
    bool busy_ = false;
public:
    explicit LoginStartupController(std::shared_ptr<LoginStartup> adapter):adapter_(std::move(adapter)) {}
    std::optional<bool> enabled() const { return enabled_; }
    const std::string& queryError() const { return queryError_; }
    bool busy() const { return busy_; }
    void refresh() {
        try { enabled_=adapter_->enabled(); queryError_.clear(); }
        catch(const std::exception& error) { enabled_.reset(); queryError_=error.what(); }
    }
    // Return an error for the frontend to display. Never optimistically flip the checkmark.
    std::string toggle() {
        if(busy_) return {};
        busy_=true;
        struct Reset { bool& value; ~Reset(){value=false;} } reset{busy_};
        refresh();
        if(!enabled_) return queryError_;
        const bool requested=!*enabled_;
        std::string mutationError;
        try { adapter_->setEnabled(requested); }
        catch(const std::exception& error) { mutationError=error.what(); }
        refresh();
        if(!queryError_.empty()) {
            if(!mutationError.empty()) mutationError+="\n";
            mutationError+=queryError_;
        }
        if(mutationError.empty() && enabled_ && *enabled_!=requested)
            mutationError=tr("win.could_not_confirm_the_launch_at_login_setting_change");
        return mutationError;
    }
};
}
