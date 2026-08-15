// asi_sdk.h -- runtime binding to ZWO's ASICamera2 library.
//
// Only ASICamera2.h is needed at build time (for the structs and enums); the
// binary is resolved at runtime.

#pragma once

#include "camera/dynlib.h"

#include <ASICamera2.h>

#include <string>
#include <vector>

namespace mei {

// ret, name, params, required
//
// The `required` column carries real weight. Users routinely drop a newer
// vendor DLL in to pick up a camera released after our last build, and newer
// SDKs add functions -- so marking recent additions optional keeps an OLDER
// DLL working too. Marking everything required makes us brittle both ways.
#define MEI_ASI_SYMBOLS(X)                                                                 \
    X(int,            ASIGetNumOfConnectedCameras, (void),                            true)\
    X(ASI_ERROR_CODE, ASIGetCameraProperty,   (ASI_CAMERA_INFO*, int),                true)\
    X(ASI_ERROR_CODE, ASIOpenCamera,          (int),                                  true)\
    X(ASI_ERROR_CODE, ASIInitCamera,          (int),                                  true)\
    X(ASI_ERROR_CODE, ASICloseCamera,         (int),                                  true)\
    X(ASI_ERROR_CODE, ASIGetNumOfControls,    (int, int*),                            true)\
    X(ASI_ERROR_CODE, ASIGetControlCaps,      (int, int, ASI_CONTROL_CAPS*),          true)\
    X(ASI_ERROR_CODE, ASISetControlValue,     (int, ASI_CONTROL_TYPE, long, ASI_BOOL),true)\
    X(ASI_ERROR_CODE, ASIGetControlValue,     (int, ASI_CONTROL_TYPE, long*, ASI_BOOL*), true)\
    X(ASI_ERROR_CODE, ASISetROIFormat,        (int, int, int, int, ASI_IMG_TYPE),     true)\
    X(ASI_ERROR_CODE, ASIGetROIFormat,        (int, int*, int*, int*, ASI_IMG_TYPE*), true)\
    X(ASI_ERROR_CODE, ASISetStartPos,         (int, int, int),                        true)\
    X(ASI_ERROR_CODE, ASIStartVideoCapture,   (int),                                  true)\
    X(ASI_ERROR_CODE, ASIStopVideoCapture,    (int),                                  true)\
    X(ASI_ERROR_CODE, ASIGetVideoData,        (int, unsigned char*, long, int),       true)\
    X(ASI_ERROR_CODE, ASIGetDroppedFrames,    (int, int*),                            true)\
    /* later SDK revisions -- absence must not be fatal */                                 \
    X(ASI_ERROR_CODE, ASIGetSerialNumber,     (int, ASI_SN*),                        false)\
    X(char*,          ASIGetSDKVersion,       (void),                                false)

struct AsiApi {
    DynLib lib;

#define MEI_ASI_DECL(ret, name, params, req) ret (*name) params = nullptr;
    MEI_ASI_SYMBOLS(MEI_ASI_DECL)
#undef MEI_ASI_DECL

    bool load(const std::string& sdkDir, std::string& err) {
        if (!lib.open(libCandidates("ASICamera2", sdkDir), err)) {
            err = "ZWO SDK not found. Install the ASI camera driver, or set the "
                  "SDK path in Preferences. (" + err + ")";
            return false;
        }

        std::vector<std::string> missing;
#define MEI_ASI_BIND(ret, name, params, req) \
        if (!lib.bind(#name, name) && (req)) missing.emplace_back(#name);
        MEI_ASI_SYMBOLS(MEI_ASI_BIND)
#undef MEI_ASI_BIND

        if (!missing.empty()) {
            err = "loaded " + lib.path() + " but required symbols are missing:";
            for (const auto& m : missing) err += " " + m;
            err += " -- this does not look like an ASICamera2 library.";
            lib.close();
            return false;
        }
        return true;
    }

    void unload() { lib.close(); }
    bool ok() const { return lib.isOpen(); }
};

const char* asiErrorString(ASI_ERROR_CODE);

} // namespace mei
