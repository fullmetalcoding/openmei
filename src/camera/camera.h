// camera.h -- vendor-neutral camera abstraction for OpenMEI.
//
// Deliberately narrow. A DIMM needs six things from a camera: enumerate, open,
// configure a streaming ROI, set exposure/gain, start, and pull timestamped
// frames. No cooling, no filter wheels, no sequencing. That narrowness is what
// makes a new vendor backend ~300 lines instead of a driver.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace mei {

// -----------------------------------------------------------------------------
//  Pixel formats
// -----------------------------------------------------------------------------

enum class PixelFormat {
    Unknown = 0,
    Mono8,      // 1 byte/px, no CFA
    Mono16,     // 2 bytes/px, no CFA
    Bayer8,     // 1 byte/px, CFA per Frame::bayer
    Bayer16,    // 2 bytes/px, CFA per Frame::bayer
    RGB24,      // 3 bytes/px
};

enum class BayerPattern { None = 0, RGGB, BGGR, GRBG, GBRG };

inline int bytesPerPixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::Mono8:
        case PixelFormat::Bayer8:  return 1;
        case PixelFormat::Mono16:
        case PixelFormat::Bayer16: return 2;
        case PixelFormat::RGB24:   return 3;
        default:                   return 0;
    }
}

inline bool isMono(PixelFormat f) {
    return f == PixelFormat::Mono8 || f == PixelFormat::Mono16;
}

// Inline so they carry no translation-unit dependency -- these get used from
// UI code that has no other reason to link the camera implementation.
inline const char* toString(PixelFormat f) {
    switch (f) {
        case PixelFormat::Mono8:   return "Mono8";
        case PixelFormat::Mono16:  return "Mono16";
        case PixelFormat::Bayer8:  return "Bayer8";
        case PixelFormat::Bayer16: return "Bayer16";
        case PixelFormat::RGB24:   return "RGB24";
        default:                   return "Unknown";
    }
}

inline const char* toString(BayerPattern b) {
    switch (b) {
        case BayerPattern::RGGB: return "RGGB";
        case BayerPattern::BGGR: return "BGGR";
        case BayerPattern::GRBG: return "GRBG";
        case BayerPattern::GBRG: return "GBRG";
        default:                 return "None";
    }
}

// -----------------------------------------------------------------------------
//  Frames
// -----------------------------------------------------------------------------

struct FrameMeta {
    int          width  = 0;
    int          height = 0;
    int          stride = 0;          // bytes per row
    PixelFormat  format = PixelFormat::Unknown;
    BayerPattern bayer  = BayerPattern::None;

    // Bit alignment inside the container. A 12-bit ZWO sensor delivers RAW16
    // with the data shifted up 4 bits, so raw ADU = sample >> sampleShift.
    //
    // Backends report this rather than normalising, because normalising costs a
    // full-frame pass at 200 fps and centroiding does not need it: the centre of
    // gravity is invariant under a global scale factor. Only display, flux, and
    // saturation checks care.
    int significantBits = 8;
    int sampleShift     = 0;

    // Exposure actually in force for this frame. Not merely informational --
    // the exposure-time bias correction interleaves t and 2t exposures, so the
    // analysis stage must know which one produced each frame.
    int64_t exposureUs = 0;
    double  gain       = 0.0;

    // Host arrival time. No consumer camera SDK in this class offers hardware
    // timestamps, so this is stamped when the frame is handed to us and carries
    // scheduling jitter. Recorded so downstream can quantify that rather than
    // assume it away.
    int64_t  hostArrivalNs = 0;
    uint64_t sequence      = 0;       // monotonic since stream start
};

// Owning, move-only. Buffers cycle through FramePool rather than being
// allocated per frame.
struct Frame {
    FrameMeta            meta;
    std::vector<uint8_t> pixels;

    Frame() = default;
    Frame(Frame&&) noexcept = default;
    Frame& operator=(Frame&&) noexcept = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    size_t byteCount() const {
        return static_cast<size_t>(meta.stride) * meta.height;
    }
    bool empty() const { return pixels.empty(); }
};

// Bounded pool shared by every backend. Bounded on purpose: if the consumer
// falls behind we drop frames deliberately rather than let memory grow.
// Push-model SDKs (Touptek) use publish/consume; pull-model SDKs (ZWO, SVBony,
// QHY) only need acquire/recycle.
class FramePool {
public:
    void reset(size_t count, size_t bytesEach);

    bool acquire(Frame& out);                 // false when exhausted
    void recycle(Frame&& f);

    void publish(Frame&& f);                  // producer -> consumer
    bool consume(Frame& out, int timeoutMs);  // false on timeout

    void wake();
    size_t freeCount() const;

private:
    mutable std::mutex      m_;
    std::condition_variable cv_;
    std::queue<Frame>       free_;
    std::queue<Frame>       ready_;
    size_t                  bytesEach_ = 0;
};

// -----------------------------------------------------------------------------
//  Capabilities and configuration
// -----------------------------------------------------------------------------

template <typename T>
struct Range {
    T min{}, max{}, def{};
    bool supported = false;
    T clamp(T v) const { return v < min ? min : (v > max ? max : v); }
};

enum class UsbSpeed { Unknown, USB2, USB3 };

inline const char* toString(UsbSpeed s) {
    switch (s) {
        case UsbSpeed::USB2: return "USB2";
        case UsbSpeed::USB3: return "USB3";
        default:             return "USB?";
    }
}

struct Caps {
    int    maxWidth    = 0;
    int    maxHeight   = 0;
    double pixelSizeUm = 0.0;
    int    adcBits     = 8;

    bool         isColor = false;
    BayerPattern bayer   = BayerPattern::None;

    std::vector<int>         bins;
    std::vector<PixelFormat> formats;

    Range<int64_t> exposureUs;
    Range<double>  gain;
    Range<double>  offset;

    // Hardware ROI quantisation. Exposed so the config layer can clamp without
    // knowing vendor rules -- ZWO wants width%8==0 and height%2==0, and returns
    // a hard error rather than snapping.
    int roiWidthGranularity  = 1;
    int roiHeightGranularity = 1;

    bool hasHighSpeedMode = false;
    bool hasUsbBandwidth  = false;

    // Colour sensors only: hardware 2x2 summing that cancels the CFA response
    // instead of interpolating across it. For centroiding this is the only
    // correct way to use a colour camera -- see StreamConfig::monoBin.
    bool hasMonoBin = false;

    // Not every SDK reports these; Unknown is a legitimate answer.
    UsbSpeed hostUsb   = UsbSpeed::Unknown;
    UsbSpeed cameraUsb = UsbSpeed::Unknown;

    bool hasHardwareBin = false;

    // Dual conversion gain as an explicit control. ZWO hides this behind a
    // magic gain threshold that moves between SDK revisions; the ToupTek family
    // exposes it, so it can actually be set and recorded.
    bool hasConversionGain = false;

    // True when binning is being done on the host rather than on the sensor,
    // which means a binned ROI still reads out and transfers at full sensor
    // resolution. Set by the backend after configure(), from what actually
    // engaged -- not guessed from capability.
    bool binningIsSoftware = true;
};

struct StreamConfig {
    int         x = 0, y = 0;
    int         width = 0, height = 0;
    int         bin = 1;
    PixelFormat format     = PixelFormat::Mono8;
    int64_t     exposureUs = 5000;
    double      gain       = 200.0;
    bool        highSpeed  = true;
    int         usbBandwidth = -1;   // -1 = leave at driver default

    // Requires bin >= 2 and a colour sensor. When active the camera delivers
    // true monochrome and the backend reports Mono8/Mono16 with no CFA.
    bool        monoBin     = false;
    bool        hardwareBin = false;   // on-sensor binning where supported
    int         conversionGain = 1;    // 0 = LCG, 1 = HCG, 2 = HDR where offered
};

// -----------------------------------------------------------------------------
//  Discovery
// -----------------------------------------------------------------------------

struct CameraDesc {
    std::string backendId;     // "asi", "svbony", ...
    std::string model;         // "ZWO ASI662MM"
    std::string serial;        // may be empty
    int         backendIndex = 0;

    // Stable across restarts when a serial is available, so a saved profile
    // reconnects to the same physical camera rather than to whatever enumerated
    // first. Falls back to model+index when it isn't.
    std::string uniqueKey() const {
        return backendId + ":" + (serial.empty()
                                      ? model + "#" + std::to_string(backendIndex)
                                      : serial);
    }
    std::string displayName() const {
        return serial.empty() ? model : model + " (" + serial + ")";
    }
};

// -----------------------------------------------------------------------------
//  Interfaces
// -----------------------------------------------------------------------------

class ICamera {
public:
    virtual ~ICamera() = default;

    virtual const CameraDesc& desc() const = 0;
    virtual const Caps&       caps() const = 0;

    // Clamps the request to hardware granularity and returns what was applied.
    // Must not be called while streaming.
    virtual StreamConfig configure(const StreamConfig& want, std::string& err) = 0;
    virtual const StreamConfig& config() const = 0;

    // Safe to call mid-stream. Note the change takes effect some frames later;
    // FrameMeta::exposureUs reports what the driver believed was in force, so
    // t/2t interleaving must key off metadata rather than off call order.
    virtual bool setExposureUs(int64_t us, std::string& err) = 0;
    virtual bool setGain(double gain, std::string& err) = 0;

    virtual bool start(std::string& err) = 0;
    virtual void stop() = 0;
    virtual bool streaming() const = 0;

    // Pull one frame. Returns false on timeout, which is normal and not an
    // error. Ownership moves to the caller; return it with recycle().
    virtual bool nextFrame(Frame& out, int timeoutMs, std::string& err) = 0;
    virtual void recycle(Frame&& f) = 0;

    // Frames the SDK discarded because we did not pull fast enough.
    virtual int droppedFrames() const = 0;
};

class IBackend {
public:
    virtual ~IBackend() = default;

    virtual const char* id() const = 0;           // "asi"
    virtual const char* displayName() const = 0;  // "ZWO ASI"

    // Attempt to load the vendor SDK. Idempotent. On failure `why` explains it
    // in terms a user can act on -- this string is shown in the connect dialog,
    // so "SDK not found; set the ZWO SDK path in Preferences" beats "dlopen
    // failed".
    virtual bool ensureLoaded(const std::string& sdkDirHint, std::string& why) = 0;
    virtual bool loaded() const = 0;
    virtual std::string sdkVersion() const = 0;

    virtual std::vector<CameraDesc> enumerate() = 0;
    virtual std::unique_ptr<ICamera> open(const CameraDesc&, std::string& err) = 0;

    // Free-form multi-line report: which library actually loaded, what the SDK
    // reported, and what each enumeration step returned. When a backend loads
    // but finds no cameras there is no error to show, so without this the only
    // signal is an empty list -- which is indistinguishable from "no camera
    // plugged in".
    virtual std::string diagnostics() const { return {}; }
};

std::unique_ptr<IBackend> makeAsiBackend();
std::unique_ptr<IBackend> makeSvbonyBackend();
std::unique_ptr<IBackend> makeToupcamBackend();

} // namespace mei
