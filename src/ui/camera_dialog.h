#pragma once

#include "camera/registry.h"

#include <functional>
#include <string>

namespace mei::ui {

// Modal "Connect Camera" dialog. Call open() to request it, then draw() once
// per frame. onResult reports success or failure so the caller can push a
// status-bar message without the dialog knowing about the app state.
class CameraDialog {
public:
    using ResultFn = std::function<void(bool ok, std::string message)>;

    void open() { requestOpen_ = true; }
    void draw(CameraRegistry& reg, const ResultFn& onResult);

private:
    bool        requestOpen_  = false;
    bool        refreshed_    = false;
    int         selected_     = -1;
    std::string sdkDirEdit_;
    std::string sdkDirBackend_;
    char        sdkDirBuf_[512] = {};
};

} // namespace mei::ui
