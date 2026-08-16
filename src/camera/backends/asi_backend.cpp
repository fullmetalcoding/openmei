// asi_backend.cpp -- ZWO ASI implementation of ICamera / IBackend.

#include "camera/camera.h"
#include "camera/backends/asi_sdk.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mei {

const char* asiErrorString(ASI_ERROR_CODE e) {
    switch (e) {
        case ASI_SUCCESS:                    return "success";
        case ASI_ERROR_INVALID_INDEX:        return "invalid camera index";
        case ASI_ERROR_INVALID_ID:           return "invalid camera id";
        case ASI_ERROR_INVALID_CONTROL_TYPE: return "invalid control type";
        case ASI_ERROR_CAMERA_CLOSED:        return "camera closed";
        case ASI_ERROR_CAMERA_REMOVED:       return "camera removed";
        case ASI_ERROR_INVALID_SIZE:         return "invalid ROI size";
        case ASI_ERROR_INVALID_IMGTYPE:      return "invalid image type";
        case ASI_ERROR_OUTOF_BOUNDARY:       return "ROI outside sensor";
        case ASI_ERROR_TIMEOUT:              return "timeout";
        case ASI_ERROR_INVALID_SEQUENCE:     return "invalid call sequence";
        case ASI_ERROR_BUFFER_TOO_SMALL:     return "buffer too small";
        case ASI_ERROR_VIDEO_MODE_ACTIVE:    return "video mode active";
        case ASI_ERROR_EXPOSURE_IN_PROGRESS: return "exposure in progress";
        case ASI_ERROR_GENERAL_ERROR:        return "general error";
        default:                             return "unknown error";
    }
}

namespace {

int64_t nowNs() {
    return static_cast<int64_t>(SDL_GetTicksNS());
}

PixelFormat fromAsi(ASI_IMG_TYPE t, bool colorCam) {
    switch (t) {
        case ASI_IMG_RAW8:  return colorCam ? PixelFormat::Bayer8  : PixelFormat::Mono8;
        case ASI_IMG_RAW16: return colorCam ? PixelFormat::Bayer16 : PixelFormat::Mono16;
        case ASI_IMG_Y8:    return PixelFormat::Mono8;
        case ASI_IMG_RGB24: return PixelFormat::RGB24;
        default:            return PixelFormat::Unknown;
    }
}

ASI_IMG_TYPE toAsi(PixelFormat f) {
    switch (f) {
        case PixelFormat::Mono8:   return ASI_IMG_RAW8;   // Y8 on mono cams is equivalent
        case PixelFormat::Bayer8:  return ASI_IMG_RAW8;
        case PixelFormat::Mono16:
        case PixelFormat::Bayer16: return ASI_IMG_RAW16;
        case PixelFormat::RGB24:   return ASI_IMG_RGB24;
        default:                   return ASI_IMG_END;
    }
}

BayerPattern fromAsi(ASI_BAYER_PATTERN b) {
    switch (b) {
        case ASI_BAYER_RG: return BayerPattern::RGGB;
        case ASI_BAYER_BG: return BayerPattern::BGGR;
        case ASI_BAYER_GR: return BayerPattern::GRBG;
        case ASI_BAYER_GB: return BayerPattern::GBRG;
        default:           return BayerPattern::None;
    }
}

// ---------------------------------------------------------------------------

class AsiCamera final : public ICamera {
public:
    AsiCamera(AsiApi& api, CameraDesc desc, const ASI_CAMERA_INFO& info)
        : api_(api), desc_(std::move(desc)), info_(info) {
        buildCaps();
    }

    ~AsiCamera() override {
        stop();
        api_.ASICloseCamera(info_.CameraID);
    }

    const CameraDesc& desc() const override { return desc_; }
    const Caps&       caps() const override { return caps_; }
    const StreamConfig& config() const override { return cfg_; }
    bool streaming() const override { return streaming_; }

    StreamConfig configure(const StreamConfig& want, std::string& err) override {
        if (streaming_) { err = "cannot reconfigure while streaming"; return cfg_; }

        StreamConfig c = want;
        c.bin    = std::max(1, c.bin);
        c.width  = std::min(c.width,  caps_.maxWidth  / c.bin);
        c.height = std::min(c.height, caps_.maxHeight / c.bin);

        // Hard constraints -- the SDK rejects rather than snapping.
        c.width  -= c.width  % caps_.roiWidthGranularity;
        c.height -= c.height % caps_.roiHeightGranularity;

        // The original USB2 ASI120MM/MC (not the "-S" revision) additionally
        // requires width*height to be a multiple of 1024.
        const std::string model = desc_.model;
        if (model.find("ASI120") != std::string::npos &&
            model.find("-S")     == std::string::npos) {
            while ((static_cast<long>(c.width) * c.height) % 1024 != 0 && c.height > 2)
                c.height -= 2;
        }

        if (c.width <= 0 || c.height <= 0) { err = "degenerate ROI"; return cfg_; }

        c.x = std::max(0, std::min(c.x, caps_.maxWidth  / c.bin - c.width));
        c.y = std::max(0, std::min(c.y, caps_.maxHeight / c.bin - c.height));
        // Keeping the origin even avoids the Bayer-phase shift on colour
        // sensors; harmless on mono.
        c.x -= c.x % 2;
        c.y -= c.y % 2;

        const ASI_IMG_TYPE it = toAsi(c.format);
        if (it == ASI_IMG_END) { err = "unsupported pixel format"; return cfg_; }

        if (auto e = api_.ASISetROIFormat(info_.CameraID, c.width, c.height, c.bin, it);
            e != ASI_SUCCESS) {
            err = std::string("ASISetROIFormat: ") + asiErrorString(e);
            return cfg_;
        }
        // SetROIFormat recentres the window, so start position must come after.
        if (auto e = api_.ASISetStartPos(info_.CameraID, c.x, c.y); e != ASI_SUCCESS) {
            err = std::string("ASISetStartPos: ") + asiErrorString(e);
            return cfg_;
        }

        c.exposureUs = caps_.exposureUs.clamp(c.exposureUs);
        c.gain       = caps_.gain.clamp(c.gain);
        setControl(ASI_EXPOSURE, static_cast<long>(c.exposureUs));
        setControl(ASI_GAIN,     static_cast<long>(c.gain));
        if (caps_.hasHighSpeedMode) setControl(ASI_HIGH_SPEED_MODE, c.highSpeed ? 1 : 0);

        // Mono binning only applies to colour sensors at bin >= 2. Ask for it
        // only when it can actually engage, and record what we ended up with --
        // it decides whether frames carry a Bayer pattern or not.
        // Mono binning and hardware binning are mutually exclusive in effect.
        // Sensor-level binning sums SAME-COLOUR pixels so that colour survives,
        // which leaves the CFA intact at half resolution. Mono binning sums
        // across the 2x2 block and destroys colour, which is what an unbiased
        // centroid needs. On a colour sensor, mono binning wins: frame rate is
        // worth less than a centroid that is not modulated by the filter array.
        if (caps_.isColor && c.monoBin && c.bin >= 2) c.hardwareBin = false;

        monoBin_ = false;
        if (c.monoBin && caps_.hasMonoBin && caps_.isColor && c.bin >= 2 &&
            !c.hardwareBin) {
            setControl(ASI_MONO_BIN, 1);
            monoBin_ = true;
        } else if (caps_.hasMonoBin) {
            setControl(ASI_MONO_BIN, 0);
        }
        c.monoBin = monoBin_;

        // Hardware binning sums on the sensor, so both readout time and USB
        // payload shrink by bin^2. Software binning shrinks neither -- the host
        // does the summing after a full-resolution transfer. Read the value
        // back rather than assuming the write took.
        if (caps_.hasHardwareBin) {
            setControl(ASI_HARDWARE_BIN, (c.hardwareBin && c.bin > 1) ? 1 : 0);
            long v = 0; ASI_BOOL au = ASI_FALSE;
            api_.ASIGetControlValue(info_.CameraID, ASI_HARDWARE_BIN, &v, &au);
            c.hardwareBin = (v != 0);
            // If hardware binning engaged anyway, the frames are still mosaiced
            // whatever we asked for -- do not let stale state mislabel them.
            if (c.hardwareBin) monoBin_ = false;
        } else {
            c.hardwareBin = false;
        }
        caps_.binningIsSoftware = !c.hardwareBin;
        if (caps_.hasUsbBandwidth && c.usbBandwidth > 0)
            setControl(ASI_BANDWIDTHOVERLOAD, c.usbBandwidth);

        cfg_ = c;
        return cfg_;
    }

    bool setExposureUs(int64_t us, std::string& err) override {
        us = caps_.exposureUs.clamp(us);
        if (auto e = api_.ASISetControlValue(info_.CameraID, ASI_EXPOSURE,
                                             static_cast<long>(us), ASI_FALSE);
            e != ASI_SUCCESS) {
            err = std::string("set exposure: ") + asiErrorString(e);
            return false;
        }
        cfg_.exposureUs = us;
        return true;
    }

    bool setGain(double g, std::string& err) override {
        g = caps_.gain.clamp(g);
        if (auto e = api_.ASISetControlValue(info_.CameraID, ASI_GAIN,
                                             static_cast<long>(g), ASI_FALSE);
            e != ASI_SUCCESS) {
            err = std::string("set gain: ") + asiErrorString(e);
            return false;
        }
        cfg_.gain = g;
        return true;
    }

    bool start(std::string& err) override {
        if (streaming_) return true;
        if (cfg_.width <= 0) { err = "configure() before start()"; return false; }

        frameBytes_ = static_cast<size_t>(cfg_.width) * cfg_.height *
                      bytesPerPixel(cfg_.format);
        pool_.reset(kPoolFrames, frameBytes_);

        if (auto e = api_.ASIStartVideoCapture(info_.CameraID); e != ASI_SUCCESS) {
            err = std::string("ASIStartVideoCapture: ") + asiErrorString(e);
            return false;
        }
        sequence_  = 0;
        streaming_ = true;
        return true;
    }

    void stop() override {
        if (!streaming_) return;
        api_.ASIStopVideoCapture(info_.CameraID);
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

        const ASI_ERROR_CODE e = api_.ASIGetVideoData(
            info_.CameraID, f.pixels.data(),
            static_cast<long>(frameBytes_), timeoutMs);

        if (e == ASI_ERROR_TIMEOUT) { pool_.recycle(std::move(f)); return false; }
        if (e != ASI_SUCCESS) {
            pool_.recycle(std::move(f));
            err = std::string("ASIGetVideoData: ") + asiErrorString(e);
            return false;
        }

        FrameMeta& m = f.meta;
        m.width  = cfg_.width;
        m.height = cfg_.height;
        m.stride = cfg_.width * bytesPerPixel(cfg_.format);
        // Only mono binning removes the CFA. Hardware binning sums same-colour
        // pixels and leaves it in place, so a hardware-binned colour frame must
        // still declare its Bayer pattern -- otherwise downstream skips the
        // summing it needs and the mosaic shows through.
        m.format = monoBin_ ? (bytesPerPixel(cfg_.format) == 2 ? PixelFormat::Mono16
                                                               : PixelFormat::Mono8)
                            : cfg_.format;
        m.bayer  = isMono(m.format) ? BayerPattern::None : caps_.bayer;

        // 12-bit sensors deliver RAW16 left-shifted so the MSBs align. Reported
        // rather than corrected; see FrameMeta. Worth verifying empirically per
        // model -- it is a vendor convention, not a documented guarantee.
        if (bytesPerPixel(cfg_.format) == 2) {
            m.significantBits = caps_.adcBits;
            m.sampleShift     = 16 - caps_.adcBits;
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
        api_.ASIGetDroppedFrames(info_.CameraID, &d);
        return d;
    }

private:
    static constexpr size_t kPoolFrames = 16;

    void setControl(ASI_CONTROL_TYPE c, long v) {
        api_.ASISetControlValue(info_.CameraID, c, v, ASI_FALSE);
    }

    void buildCaps() {
        caps_.maxWidth    = static_cast<int>(info_.MaxWidth);
        caps_.maxHeight   = static_cast<int>(info_.MaxHeight);
        caps_.pixelSizeUm = info_.PixelSize;
        caps_.adcBits     = info_.BitDepth;
        caps_.isColor     = info_.IsColorCam == ASI_TRUE;
        caps_.bayer       = caps_.isColor ? fromAsi(info_.BayerPattern)
                                          : BayerPattern::None;

        caps_.hostUsb   = info_.IsUSB3Host   == ASI_TRUE ? UsbSpeed::USB3 : UsbSpeed::USB2;
        caps_.cameraUsb = info_.IsUSB3Camera == ASI_TRUE ? UsbSpeed::USB3 : UsbSpeed::USB2;

        caps_.roiWidthGranularity  = 8;   // ZWO: width % 8 == 0
        caps_.roiHeightGranularity = 2;   //      height % 2 == 0

        for (int i = 0; i < 16 && info_.SupportedBins[i] != 0; ++i)
            caps_.bins.push_back(info_.SupportedBins[i]);

        for (int i = 0; i < 8 && info_.SupportedVideoFormat[i] != ASI_IMG_END; ++i) {
            PixelFormat f = fromAsi(info_.SupportedVideoFormat[i], caps_.isColor);
            if (f != PixelFormat::Unknown) caps_.formats.push_back(f);
        }

        int n = 0;
        api_.ASIGetNumOfControls(info_.CameraID, &n);
        for (int i = 0; i < n; ++i) {
            ASI_CONTROL_CAPS cc{};
            if (api_.ASIGetControlCaps(info_.CameraID, i, &cc) != ASI_SUCCESS) continue;
            switch (cc.ControlType) {
                case ASI_EXPOSURE:
                    caps_.exposureUs = { cc.MinValue, cc.MaxValue, cc.DefaultValue, true };
                    break;
                case ASI_GAIN:
                    caps_.gain = { double(cc.MinValue), double(cc.MaxValue),
                                   double(cc.DefaultValue), true };
                    break;
                case ASI_OFFSET:
                    caps_.offset = { double(cc.MinValue), double(cc.MaxValue),
                                     double(cc.DefaultValue), true };
                    break;
                case ASI_HIGH_SPEED_MODE:   caps_.hasHighSpeedMode = true; break;
                case ASI_BANDWIDTHOVERLOAD: caps_.hasUsbBandwidth  = true; break;
                case ASI_MONO_BIN:          caps_.hasMonoBin       = true; break;
                case ASI_HARDWARE_BIN:      caps_.hasHardwareBin   = true; break;
                default: break;
            }
        }
    }

    AsiApi&         api_;
    CameraDesc      desc_;
    ASI_CAMERA_INFO info_{};
    mutable Caps    caps_{};
    StreamConfig    cfg_{};
    FramePool       pool_;
    size_t          frameBytes_ = 0;
    uint64_t        sequence_   = 0;
    bool            monoBin_    = false;
    bool            streaming_  = false;
};

// ---------------------------------------------------------------------------

class AsiBackend final : public IBackend {
public:
    const char* id() const override { return "asi"; }
    const char* displayName() const override { return "ZWO ASI"; }
    bool loaded() const override { return api_.ok(); }

    bool ensureLoaded(const std::string& sdkDirHint, std::string& why) override {
        if (api_.ok()) return true;
        return api_.load(sdkDirHint, why);
    }

    std::string sdkVersion() const override {
        if (!api_.ok() || !api_.ASIGetSDKVersion) return "unknown";
        const char* v = api_.ASIGetSDKVersion();
        return v ? v : "unknown";
    }

    std::string diagnostics() const override { return diag_; }

    std::vector<CameraDesc> enumerate() override {
        std::vector<CameraDesc> out;
        diag_.clear();
        if (!api_.ok()) { diag_ = "SDK not loaded"; return out; }

        char buf[512];
        std::snprintf(buf, sizeof(buf), "library: %s\nSDK version: %s\n",
                      api_.lib.path().c_str(), sdkVersion().c_str());
        diag_ = buf;

        const int n = api_.ASIGetNumOfConnectedCameras();
        std::snprintf(buf, sizeof(buf),
                      "ASIGetNumOfConnectedCameras() -> %d\n", n);
        diag_ += buf;
        if (n == 0) {
            diag_ += "SDK loaded but no cameras reported. Check that the vendor "
                     "USB driver is installed and no other application holds "
                     "the camera.\n";
        }
        for (int i = 0; i < n; ++i) {
            ASI_CAMERA_INFO info{};
            if (api_.ASIGetCameraProperty(&info, i) != ASI_SUCCESS) continue;

            CameraDesc d;
            d.backendId    = id();
            d.model        = info.Name;
            d.backendIndex = i;
            d.serial       = readSerial(info.CameraID);
            out.push_back(std::move(d));
        }
        return out;
    }

    std::unique_ptr<ICamera> open(const CameraDesc& d, std::string& err) override {
        if (!api_.ok()) { err = "ZWO SDK not loaded"; return nullptr; }

        ASI_CAMERA_INFO info{};
        if (auto e = api_.ASIGetCameraProperty(&info, d.backendIndex); e != ASI_SUCCESS) {
            err = std::string("ASIGetCameraProperty: ") + asiErrorString(e);
            return nullptr;
        }
        if (auto e = api_.ASIOpenCamera(info.CameraID); e != ASI_SUCCESS) {
            err = std::string("ASIOpenCamera: ") + asiErrorString(e);
            return nullptr;
        }
        // Slow (downloads FPGA configuration) and mandatory before any capture.
        if (auto e = api_.ASIInitCamera(info.CameraID); e != ASI_SUCCESS) {
            api_.ASICloseCamera(info.CameraID);
            err = std::string("ASIInitCamera: ") + asiErrorString(e);
            return nullptr;
        }
        return std::make_unique<AsiCamera>(api_, d, info);
    }

private:
    std::string diag_;

    // Requires the camera to be open, so a closed-camera failure here is normal
    // and simply yields an empty serial.
    std::string readSerial(int camId) {
        if (!api_.ASIGetSerialNumber) return {};
        if (api_.ASIOpenCamera(camId) != ASI_SUCCESS) return {};

        std::string s;
        ASI_SN sn{};
        if (api_.ASIGetSerialNumber(camId, &sn) == ASI_SUCCESS) {
            char buf[17];
            for (int i = 0; i < 8; ++i)
                std::snprintf(buf + i * 2, 3, "%02x", sn.id[i]);
            s.assign(buf, 16);
        }
        api_.ASICloseCamera(camId);
        return s;
    }

    AsiApi api_;
};

} // namespace

std::unique_ptr<IBackend> makeAsiBackend() {
    return std::make_unique<AsiBackend>();
}

} // namespace mei
