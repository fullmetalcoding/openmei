// registry.h -- owns every camera backend and the current connection.

#pragma once

#include "camera/camera.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mei {

struct BackendStatus {
    std::string id;
    std::string displayName;
    bool        loaded = false;
    std::string sdkVersion;
    std::string message;        // why it failed, in user-actionable terms
    int         cameraCount = 0;
};

class CameraRegistry {
public:
    CameraRegistry();

    // Per-backend SDK search path override, keyed by backend id. Empty means
    // use the system search path.
    void setSdkDir(const std::string& backendId, std::string dir);
    std::string sdkDir(const std::string& backendId) const;

    // Tries to load every backend's SDK, then enumerates. Cheap enough to call
    // on dialog open; ASIInitCamera-level cost is not incurred here.
    void refresh();

    const std::vector<BackendStatus>& backends() const { return status_; }
    const std::vector<CameraDesc>&    cameras() const { return cameras_; }

    bool connect(const CameraDesc&, std::string& err);
    void disconnect();

    ICamera*          camera() { return camera_.get(); }
    const ICamera*    camera() const { return camera_.get(); }
    bool              connected() const { return camera_ != nullptr; }
    const CameraDesc& connectedDesc() const { return connectedDesc_; }

private:
    IBackend* find(const std::string& id);

    std::vector<std::unique_ptr<IBackend>> backends_;
    std::vector<BackendStatus>             status_;
    std::vector<CameraDesc>                cameras_;
    std::map<std::string, std::string>     sdkDirs_;

    std::unique_ptr<ICamera> camera_;
    CameraDesc               connectedDesc_;
};

} // namespace mei
