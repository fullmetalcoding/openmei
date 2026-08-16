// svbony_backend.cpp -- SVBony implementation of ICamera / IBackend.

#include "camera/camera.h"
#include "camera/backends/svbony_sdk.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mei {

const char* svbErrorString(SVB_ERROR_CODE e) {
    switch (e) {
        case SVB_SUCCESS:                    return "success";
        case SVB_ERROR_INVALID_INDEX:        return "invalid camera index";
        case SVB_ERROR_INVALID_ID:           return "invalid camera id";
        case SVB_ERROR_INVALID_CONTROL_TYPE: return "invalid control type";
        case SVB_ERROR_CAMERA_CLOSED:        return "camera closed";
        case SVB_ERROR_CAMERA_REMOVED:       return "camera removed";
        case SVB_ERROR_INVALID_SIZE:         return "invalid ROI size";
        case SVB_ERROR_INVALID_IMGTYPE:      return "invalid image type";
        case SVB_ERROR_OUTOF_BOUNDARY:       return "ROI outside sensor";
        case SVB_ERROR_TIMEOUT:              return "timeout";
        case SVB_ERROR_INVALID_SEQUENCE:     return "invalid call sequence";
        case SVB_ERROR_BUFFER_TOO_SMALL:     return "buffer too small";
        case SVB_ERROR_VIDEO_MODE_ACTIVE:    return "video mode active";
        case SVB_ERROR_EXPOSURE_IN_PROGRESS: return "exposure in progress";
        case SVB_ERROR_GENERAL_ERROR:        return "general error";
        default:                             return "unknown error";
    }
}

namespace {

int64_t nowNs() { return static_cast<int64_t>(SDL_GetTicksNS()); }

// SVBony exposes intermediate depths that ZWO does not. They all arrive in a
// 16-bit container; MaxBitDepth says how many bits are real.
int significantBitsFor(SVB_IMG_TYPE t, int maxBitDepth) {
    switch (t) {
        case SVB_IMG_RAW8:
        case SVB_IMG_Y8:    return 8;
        case SVB_IMG_RAW10:
        case SVB_IMG_Y10:   return 10;
        case SVB_IMG_RAW12:
        case SVB_IMG_Y12:   return 12;
        case SVB_IMG_RAW14:
        case SVB_IMG_Y14:   return 14;
        case SVB_IMG_RAW16:
        case SVB_IMG_Y16:   return maxBitDepth > 0 ? maxBitDepth : 16;
        default:            return 8;
    }
}

int containerBytes(SVB_IMG_TYPE t) {
    switch (t) {
        case SVB_IMG_RAW8:
        case SVB_IMG_Y8:    return 1;
        case SVB_IMG_RGB24: return 3;
        case SVB_IMG_RGB32: return 4;
        default:            return 2;
    }
}

PixelFormat fromSvb(SVB_IMG_TYPE t, bool colorCam) {
    switch (t) {
        case SVB_IMG_RAW8:  return colorCam ? PixelFormat::Bayer8 : PixelFormat::Mono8;
        case SVB_IMG_RAW10:
        case SVB_IMG_RAW12:
        case SVB_IMG_RAW14:
        case SVB_IMG_RAW16: return colorCam ? PixelFormat::Bayer16 : PixelFormat::Mono16;
        case SVB_IMG_Y8:    return PixelFormat::Mono8;
        case SVB_IMG_Y10:
        case SVB_IMG_Y12:
        case SVB_IMG_Y14:
        case SVB_IMG_Y16:   return PixelFormat::Mono16;
        case SVB_IMG_RGB24: return PixelFormat::RGB24;
        default:            return PixelFormat::Unknown;
    }
}

BayerPattern fromSvb(SVB_BAYER_PATTERN b) {
    switch (b) {
        case SVB_BAYER_RG: return BayerPattern::RGGB;
        case SVB_BAYER_BG: return BayerPattern::BGGR;
        case SVB_BAYER_GR: return BayerPattern::GRBG;
        case SVB_BAYER_GB: return BayerPattern::GBRG;
        default:           return BayerPattern::None;
    }
}

// ---------------------------------------------------------------------------

class SvbCamera final : public ICamera {
public:
    SvbCamera(SvbApi& api, CameraDesc desc, const SVB_CAMERA_INFO& info,
              const SVB_CAMERA_PROPERTY& prop)
        : api_(api), desc_(std::move(desc)), info_(info), prop_(prop) {
        buildCaps();
        neutralizeImagingControls();
    }

    ~SvbCamera() override {
        stop();
        api_.SVBCloseCamera(info_.CameraID);
    }

    const CameraDesc&   desc()   const override { return desc_; }
    const Caps&         caps()   const override { return caps_; }
    const StreamConfig& config() const override { return cfg_; }
    bool streaming() const override { return streaming_; }

    StreamConfig configure(const StreamConfig& want, std::string& err) override {
        if (streaming_) { err = "cannot reconfigure while streaming"; return cfg_; }

        StreamConfig c = want;
        c.bin    = std::max(1, c.bin);
        c.width  = std::min(c.width,  caps_.maxWidth  / c.bin);
        c.height = std::min(c.height, caps_.maxHeight / c.bin);
        c.width  -= c.width  % caps_.roiWidthGranularity;
        c.height -= c.height % caps_.roiHeightGranularity;
        if (c.width <= 0 || c.height <= 0) { err = "degenerate ROI"; return cfg_; }

        c.x = std::max(0, std::min(c.x, caps_.maxWidth  / c.bin - c.width));
        c.y = std::max(0, std::min(c.y, caps_.maxHeight / c.bin - c.height));
        c.x -= c.x % 2;
        c.y -= c.y % 2;

        imgType_ = pickImageType(c.format);
        if (imgType_ == SVB_IMG_END) { err = "unsupported pixel format"; return cfg_; }

        // Unlike ZWO, the origin is part of SetROIFormat and the pixel format is
        // a separate call -- so there is no recentring to undo afterwards.
        if (auto e = api_.SVBSetROIFormat(info_.CameraID, c.x, c.y,
                                          c.width, c.height, c.bin);
            e != SVB_SUCCESS) {
            err = std::string("SVBSetROIFormat: ") + svbErrorString(e);
            return cfg_;
        }
        if (auto e = api_.SVBSetOutputImageType(info_.CameraID, imgType_);
            e != SVB_SUCCESS) {
            err = std::string("SVBSetOutputImageType: ") + svbErrorString(e);
            return cfg_;
        }

        c.format = fromSvb(imgType_, caps_.isColor);
        c.exposureUs = caps_.exposureUs.clamp(c.exposureUs);
        c.gain       = caps_.gain.clamp(c.gain);
        setControl(SVB_EXPOSURE, long(c.exposureUs));
        setControl(SVB_GAIN,     long(c.gain));
        // Closest analogue to ZWO's high-speed mode; there is no USB bandwidth
        // control on this SDK at all.
        if (caps_.hasHighSpeedMode) setControl(SVB_FRAME_SPEED_MODE, c.highSpeed ? 2 : 1);

        c.monoBin = false;       // no mono-bin equivalent on this SDK
        c.hardwareBin = false;
        c.usbBandwidth = -1;

        cfg_ = c;
        return cfg_;
    }

    bool setExposureUs(int64_t us, std::string& err) override {
        us = caps_.exposureUs.clamp(us);
        if (auto e = api_.SVBSetControlValue(info_.CameraID, SVB_EXPOSURE,
                                             long(us), SVB_FALSE); e != SVB_SUCCESS) {
            err = std::string("set exposure: ") + svbErrorString(e);
            return false;
        }
        cfg_.exposureUs = us;
        return true;
    }

    bool setGain(double g, std::string& err) override {
        g = caps_.gain.clamp(g);
        if (auto e = api_.SVBSetControlValue(info_.CameraID, SVB_GAIN,
                                             long(g), SVB_FALSE); e != SVB_SUCCESS) {
            err = std::string("set gain: ") + svbErrorString(e);
            return false;
        }
        cfg_.gain = g;
        return true;
    }

    bool start(std::string& err) override {
        if (streaming_) return true;
        if (cfg_.width <= 0) { err = "configure() before start()"; return false; }

        bpp_ = containerBytes(imgType_);
        frameBytes_ = size_t(cfg_.width) * cfg_.height * bpp_;
        pool_.reset(16, frameBytes_);

        if (auto e = api_.SVBStartVideoCapture(info_.CameraID); e != SVB_SUCCESS) {
            err = std::string("SVBStartVideoCapture: ") + svbErrorString(e);
            return false;
        }
        sequence_  = 0;
        streaming_ = true;
        return true;
    }

    void stop() override {
        if (!streaming_) return;
        api_.SVBStopVideoCapture(info_.CameraID);
        streaming_ = false;
        pool_.wake();
    }

    bool nextFrame(Frame& out, int timeoutMs, std::string& err) override {
        if (!streaming_) { err = "not streaming"; return false; }

        Frame f;
        if (!pool_.acquire(f)) {
            err = "frame pool exhausted -- consumer is not recycling buffers";
            return false;
        }

        const SVB_ERROR_CODE e = api_.SVBGetVideoData(
            info_.CameraID, f.pixels.data(), long(frameBytes_), timeoutMs);

        if (e == SVB_ERROR_TIMEOUT) { pool_.recycle(std::move(f)); return false; }
        if (e != SVB_SUCCESS) {
            pool_.recycle(std::move(f));
            err = std::string("SVBGetVideoData: ") + svbErrorString(e);
            return false;
        }

        FrameMeta& m = f.meta;
        m.width  = cfg_.width;
        m.height = cfg_.height;
        m.stride = cfg_.width * bpp_;
        m.format = cfg_.format;
        m.bayer  = isMono(m.format) ? BayerPattern::None : caps_.bayer;

        if (bpp_ == 2) {
            m.significantBits = significantBitsFor(imgType_, prop_.MaxBitDepth);
            // Assumes the data is left-aligned in the container, as ZWO's is.
            // This is a vendor convention rather than a documented guarantee and
            // SVBony's has been inconsistent across models -- verify against a
            // flat frame before trusting it (comb spacing of 2^shift).
            m.sampleShift = 16 - m.significantBits;
        } else {
            m.significantBits = 8;
            m.sampleShift     = 0;
        }

        m.exposureUs    = cfg_.exposureUs;
        m.gain          = cfg_.gain;
        m.hostArrivalNs = nowNs();
        m.sequence      = sequence_++;

        out = std::move(f);
        return true;
    }

    void recycle(Frame&& f) override { pool_.recycle(std::move(f)); }

    int droppedFrames() const override {
        int d = 0;
        api_.SVBGetDroppedFrames(info_.CameraID, &d);
        return d;
    }

private:
    void setControl(SVB_CONTROL_TYPE c, long v) {
        api_.SVBSetControlValue(info_.CameraID, c, v, SVB_FALSE);
    }

    // SVBony exposes a lot more image processing than ZWO, and every one of
    // these corrupts a centroid. Gamma is the clearest case: any nonlinearity
    // between photons and ADU moves the intensity-weighted centre of gravity.
    // Assert the imaging state rather than inheriting whatever the last
    // application left in the driver.
    void neutralizeImagingControls() {
        for (const auto& c : controls_) {
            switch (c.ControlType) {
                case SVB_GAMMA:
                case SVB_GAMMA_CONTRAST:
                case SVB_CONTRAST:
                case SVB_SHARPNESS:
                case SVB_SATURATION:
                    // Mid-scale is unity for these on SVBony hardware.
                    setControl(c.ControlType, c.DefaultValue);
                    break;
                case SVB_FLIP:
                    setControl(SVB_FLIP, 0);   // flipping rotates the axes
                    break;
                case SVB_BAD_PIXEL_CORRECTION_ENABLE:
                    // On-camera correction interpolates over pixels without
                    // telling us which -- we would rather mask them ourselves.
                    setControl(SVB_BAD_PIXEL_CORRECTION_ENABLE, 0);
                    break;
                default: break;
            }
        }
    }

    SVB_IMG_TYPE pickImageType(PixelFormat want) const {
        const bool want16 = bytesPerPixel(want) == 2;
        const bool mono   = isMono(want);
        SVB_IMG_TYPE best = SVB_IMG_END;
        for (int i = 0; i < SVB_IMG_END && prop_.SupportedVideoFormat[i] != SVB_IMG_END; ++i) {
            const SVB_IMG_TYPE t = prop_.SupportedVideoFormat[i];
            const PixelFormat  f = fromSvb(t, caps_.isColor);
            if (f == PixelFormat::Unknown || f == PixelFormat::RGB24) continue;
            if (isMono(f) != mono && caps_.isColor) continue;
            const bool is16 = bytesPerPixel(f) == 2;
            if (is16 != want16) continue;
            // Prefer the deepest available at the requested container size.
            if (best == SVB_IMG_END ||
                significantBitsFor(t, prop_.MaxBitDepth) >
                significantBitsFor(best, prop_.MaxBitDepth)) {
                best = t;
            }
        }
        if (best != SVB_IMG_END) return best;
        // Fall back to whatever the camera lists first.
        return prop_.SupportedVideoFormat[0];
    }

    void buildCaps() {
        caps_.maxWidth  = int(prop_.MaxWidth);
        caps_.maxHeight = int(prop_.MaxHeight);
        caps_.adcBits   = prop_.MaxBitDepth;
        caps_.isColor   = prop_.IsColorCam == SVB_TRUE;
        caps_.bayer     = caps_.isColor ? fromSvb(prop_.BayerPattern)
                                        : BayerPattern::None;
        caps_.roiWidthGranularity  = 8;
        caps_.roiHeightGranularity = 2;

        float px = 0.0f;
        if (api_.SVBGetSensorPixelSize &&
            api_.SVBGetSensorPixelSize(info_.CameraID, &px) == SVB_SUCCESS) {
            caps_.pixelSizeUm = double(px);
        }

        for (int i = 0; i < 16 && prop_.SupportedBins[i] != 0; ++i)
            caps_.bins.push_back(prop_.SupportedBins[i]);

        for (int i = 0; i < SVB_IMG_END && prop_.SupportedVideoFormat[i] != SVB_IMG_END; ++i) {
            const PixelFormat f = fromSvb(prop_.SupportedVideoFormat[i], caps_.isColor);
            if (f == PixelFormat::Unknown) continue;
            if (std::find(caps_.formats.begin(), caps_.formats.end(), f) ==
                caps_.formats.end())
                caps_.formats.push_back(f);
        }

        int n = 0;
        api_.SVBGetNumOfControls(info_.CameraID, &n);
        for (int i = 0; i < n; ++i) {
            SVB_CONTROL_CAPS cc{};
            if (api_.SVBGetControlCaps(info_.CameraID, i, &cc) != SVB_SUCCESS) continue;
            controls_.push_back(cc);
            switch (cc.ControlType) {
                case SVB_EXPOSURE:
                    caps_.exposureUs = { cc.MinValue, cc.MaxValue, cc.DefaultValue, true };
                    break;
                case SVB_GAIN:
                    caps_.gain = { double(cc.MinValue), double(cc.MaxValue),
                                   double(cc.DefaultValue), true };
                    break;
                case SVB_BLACK_LEVEL:
                    caps_.offset = { double(cc.MinValue), double(cc.MaxValue),
                                     double(cc.DefaultValue), true };
                    break;
                case SVB_FRAME_SPEED_MODE: caps_.hasHighSpeedMode = true; break;
                default: break;
            }
        }

        // No mono-bin, no hardware-bin control, no USB bandwidth on this SDK.
        caps_.hasMonoBin      = false;
        caps_.hasHardwareBin  = false;
        caps_.hasUsbBandwidth = false;
        caps_.binningIsSoftware = true;
        caps_.hostUsb   = UsbSpeed::Unknown;
        caps_.cameraUsb = UsbSpeed::Unknown;
    }

    SvbApi&               api_;
    CameraDesc            desc_;
    SVB_CAMERA_INFO       info_{};
    SVB_CAMERA_PROPERTY   prop_{};
    std::vector<SVB_CONTROL_CAPS> controls_;
    Caps                  caps_{};
    StreamConfig          cfg_{};
    FramePool             pool_;
    SVB_IMG_TYPE          imgType_ = SVB_IMG_END;
    size_t                frameBytes_ = 0;
    int                   bpp_ = 1;
    uint64_t              sequence_ = 0;
    bool                  streaming_ = false;
};

// ---------------------------------------------------------------------------

class SvbBackend final : public IBackend {
public:
    const char* id() const override { return "svbony"; }
    const char* displayName() const override { return "SVBony"; }
    bool loaded() const override { return api_.ok(); }

    bool ensureLoaded(const std::string& sdkDirHint, std::string& why) override {
        if (api_.ok()) return true;
        return api_.load(sdkDirHint, why);
    }

    std::string sdkVersion() const override {
        if (!api_.ok() || !api_.SVBGetSDKVersion) return "unknown";
        const char* v = api_.SVBGetSDKVersion();
        return v ? v : "unknown";
    }

    std::string diagnostics() const override { return diag_; }

    std::vector<CameraDesc> enumerate() override {
        std::vector<CameraDesc> out;
        diag_.clear();
        if (!api_.ok()) { diag_ = "SDK not loaded"; return out; }

        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "library: %s\nSDK version: %s\n",
                      api_.lib.path().c_str(), sdkVersion().c_str());
        diag_ = buf;

        const int n = api_.SVBGetNumOfConnectedCameras();
        std::snprintf(buf, sizeof(buf),
                      "SVBGetNumOfConnectedCameras() -> %d\n", n);
        diag_ += buf;
        if (n == 0) {
            diag_ += "The SDK loaded but reports no cameras. Common causes:\n"
                     "  - another application holds the camera open\n"
                     "  - this DLL predates the camera model\n"
                     "  - the USB device is bound to a different driver\n";
        }

        for (int i = 0; i < n; ++i) {
            SVB_CAMERA_INFO info{};
            const SVB_ERROR_CODE e = api_.SVBGetCameraInfo(&info, i);
            std::snprintf(buf, sizeof(buf), "  [%d] SVBGetCameraInfo -> %s",
                          i, svbErrorString(e));
            diag_ += buf;
            if (e != SVB_SUCCESS) { diag_ += "\n"; continue; }
            std::snprintf(buf, sizeof(buf), "  name='%s' sn='%s' port='%s' id=%d\n",
                          info.FriendlyName, info.CameraSN, info.PortType,
                          info.CameraID);
            diag_ += buf;

            CameraDesc d;
            d.backendId    = id();
            d.model        = info.FriendlyName;
            // Serial comes free here; ZWO needs the camera open to read it.
            d.serial       = info.CameraSN;
            d.backendIndex = i;
            out.push_back(std::move(d));
        }
        return out;
    }

    std::unique_ptr<ICamera> open(const CameraDesc& d, std::string& err) override {
        if (!api_.ok()) { err = "SVBony SDK not loaded"; return nullptr; }

        SVB_CAMERA_INFO info{};
        if (auto e = api_.SVBGetCameraInfo(&info, d.backendIndex); e != SVB_SUCCESS) {
            err = std::string("SVBGetCameraInfo: ") + svbErrorString(e);
            return nullptr;
        }
        // No Init step -- unlike ZWO, opening is sufficient.
        if (auto e = api_.SVBOpenCamera(info.CameraID); e != SVB_SUCCESS) {
            err = std::string("SVBOpenCamera: ") + svbErrorString(e);
            return nullptr;
        }
        SVB_CAMERA_PROPERTY prop{};
        if (auto e = api_.SVBGetCameraProperty(info.CameraID, &prop); e != SVB_SUCCESS) {
            api_.SVBCloseCamera(info.CameraID);
            err = std::string("SVBGetCameraProperty: ") + svbErrorString(e);
            return nullptr;
        }
        return std::make_unique<SvbCamera>(api_, d, info, prop);
    }

private:
    SvbApi      api_;
    std::string diag_;
};

} // namespace

std::unique_ptr<IBackend> makeSvbonyBackend() {
    return std::make_unique<SvbBackend>();
}

} // namespace mei
