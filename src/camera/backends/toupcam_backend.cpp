// toupcam_backend.cpp -- ToupTek and OEM rebadges.
//
// Structurally different from ZWO and SVBony: this SDK pushes. It calls back on
// its own thread when a frame is ready and you pull the pixels from inside that
// callback. The adapter here turns that into the same pull interface every
// other backend exposes, using FramePool's publish/consume path -- which is why
// the pool has one.

#include "camera/camera.h"
#include "camera/backends/toupcam_sdk.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>

namespace mei {

    namespace {

        int64_t nowNs() { return static_cast<int64_t>(SDL_GetTicksNS()); }

        // Two overloads rather than one templated helper: on Linux the SDK's strings
        // are already narrow, and pretending otherwise would mean a conversion that
        // does nothing on one platform and matters on the other.
        std::string narrow(const wchar_t* w) {
            if (!w) return {};
            std::string s;
            // Device names and ids are ASCII in practice; anything else is replaced
            // rather than mangled, since these strings end up in a UI and in the
            // settings file as a camera key.
            for (; *w; ++w) s.push_back(*w < 128 ? char(*w) : '?');
            return s;
        }

        std::string narrow(const char* c) {
            return c ? std::string(c) : std::string();
        }

        // The SDK reports the CFA as a FourCC naming the top-left 2x2. Unpacked here
        // rather than compared against MAKEFOURCC constants so that a byte-order
        // surprise shows up as an unrecognised pattern rather than a silently wrong
        // one -- getting this backwards swaps colour channels and, more importantly,
        // would put the CFA phase out by a pixel.
        BayerPattern bayerFromFourCC(unsigned fcc) {
            const char s[5] = {
                char(fcc & 0xFF), char((fcc >> 8) & 0xFF),
                char((fcc >> 16) & 0xFF), char((fcc >> 24) & 0xFF), '\0'
            };
            const std::string t(s);
            if (t == "GBRG") return BayerPattern::GBRG;
            if (t == "RGGB") return BayerPattern::RGGB;
            if (t == "BGGR") return BayerPattern::BGGR;
            if (t == "GRBG") return BayerPattern::GRBG;
            return BayerPattern::None;
        }

        class ToupCamera final : public ICamera {
        public:
            ToupCamera(ToupApi& api, CameraDesc desc, HToupcam h,
                const ToupcamDeviceV2& dev)
                : api_(api), desc_(std::move(desc)), h_(h), dev_(dev) {
                buildCaps();
                neutralizeImagingControls();
            }

            ~ToupCamera() override {
                stop();
                if (h_) api_.Close(h_);
            }

            const CameraDesc& desc()   const override { return desc_; }
            const Caps& caps()   const override { return caps_; }
            const StreamConfig& config() const override { return cfg_; }
            bool streaming() const override { return streaming_; }

            StreamConfig configure(const StreamConfig& want, std::string& err) override {
                if (streaming_) { err = "cannot reconfigure while streaming"; return cfg_; }

                StreamConfig c = want;
                c.bin = std::max(1, c.bin);
                if (caps_.hasHardwareBin && c.bin > 1)
                    api_.put_Option(h_, TOUPCAM_OPTION_BINNING, c.bin);
                else
                    api_.put_Option(h_, TOUPCAM_OPTION_BINNING, 1);

                // Raw Bayer, not demosaiced RGB: any interpolation across the CFA
                // correlates neighbouring pixels and reshapes the PSF.
                api_.put_Option(h_, TOUPCAM_OPTION_RAW, 1);

                const bool want16 = bytesPerPixel(c.format) == 2;
                api_.put_Option(h_, TOUPCAM_OPTION_BITDEPTH, want16 ? 1 : 0);

                int w = 0, h = 0;
                api_.get_Size(h_, &w, &h);
                c.width = std::min(c.width > 0 ? c.width : w, w);
                c.height = std::min(c.height > 0 ? c.height : h, h);
                // The SDK requires ROI dimensions and origin to be even.
                c.width -= c.width % 2;
                c.height -= c.height % 2;
                c.x = std::max(0, std::min(c.x, w - c.width)) & ~1;
                c.y = std::max(0, std::min(c.y, h - c.height)) & ~1;
                if (c.width <= 0 || c.height <= 0) { err = "degenerate ROI"; return cfg_; }

                if (FAILED(api_.put_Roi(h_, unsigned(c.x), unsigned(c.y),
                    unsigned(c.width), unsigned(c.height)))) {
                    err = "put_Roi failed";
                    return cfg_;
                }

                // Conversion gain, which ZWO hides behind a magic gain value and this
                // SDK exposes directly. HCG gives lower read noise, which is what short
                // DIMM exposures want.
                if (caps_.hasConversionGain)
                    api_.put_Option(h_, TOUPCAM_OPTION_CG, c.conversionGain);

                api_.put_AutoExpoEnable(h_, 0);   // auto-exposure would invalidate a burst
                c.exposureUs = caps_.exposureUs.clamp(c.exposureUs);
                c.gain = caps_.gain.clamp(c.gain);
                api_.put_ExpoTime(h_, unsigned(c.exposureUs));
                api_.put_ExpoAGain(h_, static_cast<unsigned short>(c.gain));

                bits_ = want16 ? 16 : 8;
                c.format = want16 ? (caps_.isColor ? PixelFormat::Bayer16 : PixelFormat::Mono16)
                    : (caps_.isColor ? PixelFormat::Bayer8 : PixelFormat::Mono8);
                c.monoBin = false;
                c.usbBandwidth = -1;

                cfg_ = c;
                return cfg_;
            }

            bool setExposureUs(int64_t us, std::string& err) override {
                us = caps_.exposureUs.clamp(us);
                if (FAILED(api_.put_ExpoTime(h_, unsigned(us)))) {
                    err = "put_ExpoTime failed";
                    return false;
                }
                cfg_.exposureUs = us;
                return true;
            }

            bool setGain(double g, std::string& err) override {
                g = caps_.gain.clamp(g);
                if (FAILED(api_.put_ExpoAGain(h_, static_cast<unsigned short>(g)))) {
                    err = "put_ExpoAGain failed";
                    return false;
                }
                cfg_.gain = g;
                return true;
            }

            bool start(std::string& err) override {
                if (streaming_) return true;
                if (cfg_.width <= 0) { err = "configure() before start()"; return false; }

                bpp_ = bits_ / 8;
                frameBytes_ = size_t(cfg_.width) * cfg_.height * bpp_;
                pool_.reset(24, frameBytes_);
                sequence_ = 0;
                dropped_ = 0;
                streaming_ = true;

                if (FAILED(api_.StartPullModeWithCallback(h_, &ToupCamera::eventThunk, this))) {
                    streaming_ = false;
                    err = "StartPullModeWithCallback failed";
                    return false;
                }
                return true;
            }

            void stop() override {
                if (!streaming_) return;
                streaming_ = false;
                api_.Stop(h_);       // must precede pool teardown: the callback thread
                pool_.wake();        // may still be inside onImage()
            }

            // The callback thread has already filled the pool; here we just wait on it.
            bool nextFrame(Frame& out, int timeoutMs, std::string& err) override {
                if (!streaming_) { err = "not streaming"; return false; }
                if (!pool_.consume(out, timeoutMs)) return false;   // timeout is normal
                return true;
            }

            void recycle(Frame&& f) override { pool_.recycle(std::move(f)); }
            int droppedFrames() const override { return dropped_; }

        private:
            static void __stdcall eventThunk(unsigned event, void* ctx) {
                auto* self = static_cast<ToupCamera*>(ctx);
                if (event == TOUPCAM_EVENT_IMAGE) self->onImage();
                else if (event == TOUPCAM_EVENT_DISCONNECTED ||
                    event == TOUPCAM_EVENT_NOFRAMETIMEOUT) {
                    self->streaming_ = false;
                    self->pool_.wake();
                }
            }

            // Runs on the SDK's own thread. Keep it short: blocking here stalls
            // delivery for every subsequent frame.
            void onImage() {
                if (!streaming_) return;

                Frame f;
                if (!pool_.acquire(f)) {
                    // Nothing free means the consumer is behind. Pull and discard so the
                    // SDK's own buffer does not back up, and count it -- silently
                    // skipping would look like a slow camera rather than a slow reader.
                    ++dropped_;
                    api_.PullImageV3(h_, nullptr, 0, bits_, -1, nullptr);
                    return;
                }

                ToupcamFrameInfoV3 info{};
                // rowPitch -1 requests tightly packed rows, matching our stride.
                if (FAILED(api_.PullImageV3(h_, f.pixels.data(), 0, bits_, -1, &info))) {
                    pool_.recycle(std::move(f));
                    return;
                }

                FrameMeta& m = f.meta;
                m.width = int(info.width ? info.width : unsigned(cfg_.width));
                m.height = int(info.height ? info.height : unsigned(cfg_.height));
                m.stride = m.width * bpp_;
                m.format = cfg_.format;
                m.bayer = isMono(m.format) ? BayerPattern::None : caps_.bayer;

                if (bpp_ == 2) {
                    m.significantBits = caps_.adcBits;
                    // Assumed left-aligned as ZWO's is. Verify against a flat frame --
                    // the comb spacing tells you the shift.
                    m.sampleShift = 16 - caps_.adcBits;
                }
                else {
                    m.significantBits = 8;
                    m.sampleShift = 0;
                }

                m.exposureUs = cfg_.exposureUs;
                m.gain = cfg_.gain;
                m.hostArrivalNs = nowNs();
                m.sequence = sequence_++;

                pool_.publish(std::move(f));
            }

            // This SDK applies far more processing by default than ZWO does, and every
            // one of these moves an intensity-weighted centroid.
            void neutralizeImagingControls() {
                api_.put_Option(h_, TOUPCAM_OPTION_LINEAR, 0);    // no tone curve
                api_.put_Option(h_, TOUPCAM_OPTION_CURVE, 0);     // no gamma curve
                api_.put_Option(h_, TOUPCAM_OPTION_DEMOSAIC, 0);
                api_.put_AutoExpoEnable(h_, 0);
                // Flushing on start avoids inheriting a half-filled internal queue.
                api_.put_Option(h_, TOUPCAM_OPTION_FLUSH, 1);
            }

            void buildCaps() {
                int w = 0, h = 0;
                api_.get_Size(h_, &w, &h);
                caps_.maxWidth = w;
                caps_.maxHeight = h;

                const unsigned long long flag = dev_.model ? dev_.model->flag : 0ull;
                caps_.isColor = (flag & TOUPCAM_FLAG_MONO) == 0;

                // Deepest raw mode the model advertises.
                caps_.adcBits = 8;
                if (flag & TOUPCAM_FLAG_RAW16)      caps_.adcBits = 16;
                else if (flag & TOUPCAM_FLAG_RAW14) caps_.adcBits = 14;
                else if (flag & TOUPCAM_FLAG_RAW12) caps_.adcBits = 12;
                else if (flag & TOUPCAM_FLAG_RAW10) caps_.adcBits = 10;

                caps_.hasHardwareBin = (flag & TOUPCAM_FLAG_BINSKIP_SUPPORTED) != 0;
                // Conversion gain as a first-class control rather than a hidden gain
                // threshold -- a genuine advantage of this SDK over ZWO's.
                caps_.hasConversionGain = (flag & TOUPCAM_FLAG_CG) != 0;

                if (dev_.model) {
                    caps_.pixelSizeUm = double(dev_.model->xpixsz);
                }
                if (api_.get_PixelSize) {
                    float px = 0, py = 0;
                    if (SUCCEEDED(api_.get_PixelSize(h_, 0, &px, &py)) && px > 0)
                        caps_.pixelSizeUm = double(px);
                }

                if (caps_.isColor && api_.get_RawFormat) {
                    unsigned fcc = 0, rawbits = 0;
                    if (SUCCEEDED(api_.get_RawFormat(h_, &fcc, &rawbits))) {
                        caps_.bayer = bayerFromFourCC(fcc);
                        // get_RawFormat also reports the real raw depth, which is more
                        // trustworthy than inferring it from the model flags.
                        if (rawbits >= 8 && rawbits <= 16) caps_.adcBits = int(rawbits);
                        if (caps_.bayer == BayerPattern::None) {
                            std::snprintf(fourccNote_, sizeof(fourccNote_),
                                "unrecognised CFA FourCC 0x%08X", fcc);
                        }
                    }
                }

                caps_.bins = { 1 };
                if (caps_.hasHardwareBin) { caps_.bins.push_back(2); caps_.bins.push_back(4); }

                caps_.formats = caps_.isColor
                    ? std::vector<PixelFormat>{ PixelFormat::Bayer8, PixelFormat::Bayer16 }
                : std::vector<PixelFormat>{ PixelFormat::Mono8,  PixelFormat::Mono16 };

                unsigned emin = 0, emax = 0, edef = 0;
                if (SUCCEEDED(api_.get_ExpTimeRange(h_, &emin, &emax, &edef)))
                    caps_.exposureUs = { int64_t(emin), int64_t(emax), int64_t(edef), true };

                unsigned short gmin = 0, gmax = 0, gdef = 0;
                if (SUCCEEDED(api_.get_ExpoAGainRange(h_, &gmin, &gmax, &gdef)))
                    caps_.gain = { double(gmin), double(gmax), double(gdef), true };

                // ROI granularity is 2 here, not 8 as on ZWO.
                caps_.roiWidthGranularity = 2;
                caps_.roiHeightGranularity = 2;
                caps_.hasMonoBin = false;
                caps_.hasUsbBandwidth = false;
                caps_.binningIsSoftware = !caps_.hasHardwareBin;
                caps_.hostUsb = UsbSpeed::Unknown;
                caps_.cameraUsb = UsbSpeed::Unknown;
            }

            ToupApi& api_;
            CameraDesc      desc_;
            HToupcam        h_ = nullptr;
            ToupcamDeviceV2 dev_{};
            Caps            caps_{};
            StreamConfig    cfg_{};
            FramePool       pool_;

            size_t   frameBytes_ = 0;
            int      bits_ = 8;
            int      bpp_ = 1;
            uint64_t sequence_ = 0;
            int      dropped_ = 0;
            char     fourccNote_[64] = { 0 };
            std::atomic<bool> streaming_{ false };
        };

        // ---------------------------------------------------------------------------

        class ToupBackend final : public IBackend {
        public:
            const char* id() const override { return "toupcam"; }
            const char* displayName() const override { return "ToupTek / OEM"; }
            bool loaded() const override { return api_.ok(); }
            std::string sdkVersion() const override {
                return api_.ok() ? (api_.label + " " + api_.version()) : "unknown";
            }
            std::string diagnostics() const override { return diag_; }

            // SDK availability and device presence are independent questions, and this
            // must only answer the first. Reporting "absent" because nothing is plugged
            // in would be wrong -- and would also strand us on a variant chosen before
            // the user attached a camera.
            bool ensureLoaded(const std::string& sdkDirHint, std::string& why) override {
                sdkDir_ = sdkDirHint;
                if (api_.ok()) return true;
                if (!probeVariants(why)) return false;
                return true;
            }

            std::vector<CameraDesc> enumerate() override {
                std::vector<CameraDesc> out;
                if (!api_.ok()) return out;

                count_ = api_.EnumV2(devs_);
                if (count_ == 0) {
                    // We may be holding a variant that loaded before anything was
                    // attached. A camera plugged in since could belong to a different
                    // rebadge, so re-probe rather than reporting empty.
                    std::string why;
                    api_.unload();
                    if (!probeVariants(why)) return out;
                    count_ = api_.EnumV2(devs_);
                }
                for (unsigned i = 0; i < count_; ++i) {
                    CameraDesc d;
                    d.backendId = id();
                    d.model = narrow(devs_[i].model ? devs_[i].model->name : nullptr);
                    if (d.model.empty()) d.model = narrow(devs_[i].displayname);
                    d.model = api_.label + ": " + d.model;
                    d.serial = narrow(devs_[i].id);
                    d.backendIndex = int(i);
                    out.push_back(std::move(d));
                }
                return out;
            }

            std::unique_ptr<ICamera> open(const CameraDesc& d, std::string& err) override {
                if (!api_.ok()) { err = "ToupTek SDK not loaded"; return nullptr; }
                if (d.backendIndex < 0 || unsigned(d.backendIndex) >= count_) {
                    err = "camera index out of range -- rescan";
                    return nullptr;
                }
                HToupcam h = api_.Open(devs_[d.backendIndex].id);
                if (!h) {
                    err = "Open failed -- another application may hold the camera";
                    return nullptr;
                }
                return std::make_unique<ToupCamera>(api_, d, h, devs_[d.backendIndex]);
            }

        private:
            // Tries each rebadge in turn, preferring one that actually reports a
            // camera; falls back to the first that merely loads. Only one is kept --
            // enumerating them all would list the same hardware several times, which is
            // exactly why SharpCap and N.I.N.A. can show one camera under two brands.
            bool probeVariants(std::string& why) {
                diag_.clear();
                char buf[256];

                const ToupVariant* fallback = nullptr;
                for (const auto& v : toupVariants()) {
                    std::string err;
                    if (!api_.loadVariant(v, sdkDir_, err)) {
                        std::snprintf(buf, sizeof(buf), "  %-14s not loaded\n", v.lib);
                        diag_ += buf;
                        continue;
                    }
                    const unsigned n = api_.EnumV2(devs_);
                    std::snprintf(buf, sizeof(buf), "  %-14s loaded (%s), EnumV2 -> %u\n",
                        v.lib, api_.version().c_str(), n);
                    diag_ += buf;
                    if (n > 0) { count_ = n; return true; }
                    if (!fallback) fallback = &v;
                    api_.unload();
                }

                if (fallback) {
                    // Loaded, just nothing attached. Keep it so the backend reports as
                    // available, and re-probe on the next enumerate().
                    std::string err;
                    if (api_.loadVariant(*fallback, sdkDir_, err)) {
                        count_ = 0;
                        diag_ += "  (no camera attached; holding ";
                        diag_ += fallback->lib;
                        diag_ += ")\n";
                        return true;
                    }
                }

                why = "No ToupTek-family library found. Install the vendor driver, or "
                    "point the SDK path at the folder containing toupcam.dll, "
                    "svbonycam.dll, altaircam.dll or similar.";
                return false;
            }

            std::string     sdkDir_;
            ToupApi         api_;
            ToupcamDeviceV2 devs_[TOUPCAM_MAX]{};
            unsigned        count_ = 0;
            std::string     diag_;
        };

    } // namespace

    std::unique_ptr<IBackend> makeToupcamBackend() {
        return std::make_unique<ToupBackend>();
    }

} // namespace mei