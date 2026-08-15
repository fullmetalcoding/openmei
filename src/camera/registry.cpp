#include "camera/registry.h"

#include "camera/backends/synthetic_backend.h"

namespace mei {

    CameraRegistry::CameraRegistry() {
        // Always present: needs no SDK, no hardware, and provides the ground truth
        // the measurement chain is validated against. Listed first so a fresh clone
        // has something to connect to.
        backends_.push_back(makeSyntheticBackend());

        // Vendor backends are compiled in only when their header was present at
        // configure time; see mei_detect_vendor() in CMakeLists.txt.
#ifdef MEI_HAVE_ASI
        backends_.push_back(makeAsiBackend());
#endif
        // Future: SVBony, Touptek, QHY, Player One, INDI, and SER replay.
    }

    void CameraRegistry::setSdkDir(const std::string& backendId, std::string dir) {
        sdkDirs_[backendId] = std::move(dir);
    }

    std::string CameraRegistry::sdkDir(const std::string& backendId) const {
        auto it = sdkDirs_.find(backendId);
        return it == sdkDirs_.end() ? std::string{} : it->second;
    }

    IBackend* CameraRegistry::find(const std::string& id) {
        for (auto& b : backends_)
            if (id == b->id()) return b.get();
        return nullptr;
    }

    void CameraRegistry::refresh() {
        status_.clear();
        cameras_.clear();

        for (auto& b : backends_) {
            BackendStatus s;
            s.id = b->id();
            s.displayName = b->displayName();

            std::string why;
            if (b->ensureLoaded(sdkDir(s.id), why)) {
                s.loaded = true;
                s.sdkVersion = b->sdkVersion();

                auto found = b->enumerate();
                s.cameraCount = static_cast<int>(found.size());
                s.message = found.empty() ? "SDK loaded, no cameras detected"
                    : "SDK " + s.sdkVersion;
                for (auto& d : found) cameras_.push_back(std::move(d));
            }
            else {
                s.loaded = false;
                s.message = why;
            }
            status_.push_back(std::move(s));
        }
    }

    bool CameraRegistry::connect(const CameraDesc& d, std::string& err) {
        disconnect();

        IBackend* b = find(d.backendId);
        if (!b) { err = "no backend named '" + d.backendId + "'"; return false; }

        camera_ = b->open(d, err);
        if (!camera_) return false;

        connectedDesc_ = d;
        return true;
    }

    void CameraRegistry::disconnect() {
        if (camera_) camera_->stop();
        camera_.reset();
        connectedDesc_ = CameraDesc{};
    }

} // namespace mei