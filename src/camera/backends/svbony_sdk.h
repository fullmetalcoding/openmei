// svbony_sdk.h -- runtime binding to SVBony's SVBCameraSDK.
//
// The API is near-identical to ZWO's: same error-code return convention, same
// GetControlCaps discovery loop, same video model. The differences that matter
// are structural, not cosmetic:
//
//   * enumeration is split across SVB_CAMERA_INFO and SVB_CAMERA_PROPERTY
//   * there is no Init step -- SVBOpenCamera is enough
//   * SVBSetROIFormat carries the origin, and pixel format is a separate call,
//     so the "set ROI then re-set start position" trap does not exist here
//   * the image-type enum exposes RAW10/12/14 as well as 8 and 16

#pragma once

#include "camera/dynlib.h"

#include <SVBCameraSDK.h>

#include <string>
#include <vector>

namespace mei {

#define MEI_SVB_SYMBOLS(X)                                                                  \
    X(int,            SVBGetNumOfConnectedCameras, (void),                             true)\
    X(SVB_ERROR_CODE, SVBGetCameraInfo,      (SVB_CAMERA_INFO*, int),                  true)\
    X(SVB_ERROR_CODE, SVBGetCameraProperty,  (int, SVB_CAMERA_PROPERTY*),              true)\
    X(SVB_ERROR_CODE, SVBOpenCamera,         (int),                                    true)\
    X(SVB_ERROR_CODE, SVBCloseCamera,        (int),                                    true)\
    X(SVB_ERROR_CODE, SVBGetNumOfControls,   (int, int*),                              true)\
    X(SVB_ERROR_CODE, SVBGetControlCaps,     (int, int, SVB_CONTROL_CAPS*),            true)\
    X(SVB_ERROR_CODE, SVBSetControlValue,    (int, SVB_CONTROL_TYPE, long, SVB_BOOL),  true)\
    X(SVB_ERROR_CODE, SVBGetControlValue,    (int, SVB_CONTROL_TYPE, long*, SVB_BOOL*),true)\
    X(SVB_ERROR_CODE, SVBSetROIFormat,       (int, int, int, int, int, int),           true)\
    X(SVB_ERROR_CODE, SVBGetROIFormat,       (int, int*, int*, int*, int*, int*),      true)\
    X(SVB_ERROR_CODE, SVBSetOutputImageType, (int, SVB_IMG_TYPE),                      true)\
    X(SVB_ERROR_CODE, SVBGetOutputImageType, (int, SVB_IMG_TYPE*),                     true)\
    X(SVB_ERROR_CODE, SVBStartVideoCapture,  (int),                                    true)\
    X(SVB_ERROR_CODE, SVBStopVideoCapture,   (int),                                    true)\
    X(SVB_ERROR_CODE, SVBGetVideoData,       (int, unsigned char*, long, int),         true)\
    X(SVB_ERROR_CODE, SVBGetDroppedFrames,   (int, int*),                              true)\
    /* later revisions -- absence must not be fatal */                                      \
    X(SVB_ERROR_CODE, SVBGetSensorPixelSize, (int, float*),                           false)\
    X(SVB_ERROR_CODE, SVBRestoreDefaultParam,(int),                                   false)\
    X(char*,          SVBGetSDKVersion,      (void),                                  false)

struct SvbApi {
    DynLib lib;

#define MEI_SVB_DECL(ret, name, params, req) ret (*name) params = nullptr;
    MEI_SVB_SYMBOLS(MEI_SVB_DECL)
#undef MEI_SVB_DECL

    bool load(const std::string& sdkDir, std::string& err) {
        if (!lib.open(libCandidates("SVBCameraSDK", sdkDir), err)) {
            err = "SVBony SDK not found. Install the SVBony camera driver, or "
                  "set the SDK path in the connect dialog. (" + err + ")";
            return false;
        }
        std::vector<std::string> missing;
#define MEI_SVB_BIND(ret, name, params, req) \
        if (!lib.bind(#name, name) && (req)) missing.emplace_back(#name);
        MEI_SVB_SYMBOLS(MEI_SVB_BIND)
#undef MEI_SVB_BIND

        if (!missing.empty()) {
            err = "loaded " + lib.path() + " but required symbols are missing:";
            for (const auto& m : missing) err += " " + m;
            lib.close();
            return false;
        }
        return true;
    }

    void unload() { lib.close(); }
    bool ok() const { return lib.isOpen(); }
};

const char* svbErrorString(SVB_ERROR_CODE);

} // namespace mei
