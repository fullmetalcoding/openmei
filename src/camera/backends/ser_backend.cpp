// ser_backend.cpp -- replay a SER capture as a camera.
//
// A recorded file is a frame source like any other, so it implements ICamera
// rather than living in a separate viewer. That is what lets a capture run the
// whole measurement chain unmodified: acquisition, centroiding, variance,
// seeing, history and the Alpaca feed all work without knowing the frames came
// from disk. Reprocessing last night with corrected geometry becomes the same
// operation as measuring live.

#include "camera/camera.h"
#include "camera/backends/ser_reader.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace mei {

    namespace {

        std::mutex  g_fileMutex;
        std::string g_file;

    } // namespace

    void setSerReplayFile(const std::string& path) {
        std::lock_guard<std::mutex> lk(g_fileMutex);
        g_file = path;
    }

    std::string serReplayFile() {
        std::lock_guard<std::mutex> lk(g_fileMutex);
        return g_file;
    }

    namespace {

        class SerCamera final : public ICamera, public IPlayback {
        public:
            SerCamera(CameraDesc desc, std::unique_ptr<SerReader> reader)
                : desc_(std::move(desc)), rd_(std::move(reader)) {
                const SerHeaderInfo& h = rd_->header();
                caps_.maxWidth = h.width;
                caps_.maxHeight = h.height;
                caps_.adcBits = rd_->significantBits();
                caps_.isColor = rd_->bayerPattern() != BayerPattern::None;
                caps_.bayer = rd_->bayerPattern();
                caps_.bins = { 1 };
                caps_.formats = { rd_->pixelFormat() };
                // A file's exposure is whatever it was; these ranges exist only so the
                // UI has something coherent to show.
                caps_.exposureUs = { 1, 60'000'000, 5000, false };
                caps_.gain = { 0.0, 0.0, 0.0, false };
                caps_.roiWidthGranularity = 1;
                caps_.roiHeightGranularity = 1;
                caps_.binningIsSoftware = false;
                caps_.hostUsb = UsbSpeed::Unknown;
                caps_.cameraUsb = UsbSpeed::Unknown;

                intervalMs_ = rd_->detection().medianIntervalMs > 0.0
                    ? rd_->detection().medianIntervalMs : 20.0;
            }

            ~SerCamera() override { stop(); }

            const CameraDesc& desc()   const override { return desc_; }
            const Caps& caps()   const override { return caps_; }
            const StreamConfig& config() const override { return cfg_; }
            bool streaming() const override { return streaming_; }

            // ROI, binning and format all come from the file. The request is accepted
            // and then overwritten, rather than rejected, so generic callers that
            // configure before starting keep working.
            StreamConfig configure(const StreamConfig& want, std::string& err) override {
                if (streaming_) { err = "cannot reconfigure while streaming"; return cfg_; }
                StreamConfig c = want;
                c.x = 0; c.y = 0;
                c.width = caps_.maxWidth;
                c.height = caps_.maxHeight;
                c.bin = 1;
                c.format = rd_->pixelFormat();
                c.monoBin = false;
                c.hardwareBin = false;
                c.usbBandwidth = -1;
                cfg_ = c;
                return cfg_;
            }

            bool setExposureUs(int64_t, std::string& err) override {
                err = "exposure is fixed by the recording";
                return false;
            }
            bool setGain(double, std::string& err) override {
                err = "gain is fixed by the recording";
                return false;
            }

            bool start(std::string& err) override {
                if (streaming_) return true;
                frameBytes_ = rd_->header().frameBytes;
                if (frameBytes_ == 0) { err = "empty frames"; return false; }
                pool_.reset(16, frameBytes_);
                sequence_ = 0;
                pos_ = 0;
                finished_ = false;
                nextDueNs_ = int64_t(SDL_GetTicksNS());
                streaming_ = true;
                return true;
            }

            void stop() override {
                streaming_ = false;
                pool_.wake();
            }

            bool nextFrame(Frame& out, int timeoutMs, std::string& err) override {
                if (!streaming_) { err = "not streaming"; return false; }
                if (finished_)   return false;

                if (paused_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        std::min(timeoutMs, 30)));
                    return false;
                }

                // Pace to the capture's own cadence at speed 1.0. Speed 0 means replay
                // as fast as the machine manages, which is what reprocessing wants and
                // what makes a night's data take seconds rather than a night.
                if (speed_ > 0.0) {
                    const int64_t deadline = int64_t(SDL_GetTicksNS()) +
                        int64_t(timeoutMs) * 1'000'000;
                    for (;;) {
                        const int64_t now = int64_t(SDL_GetTicksNS());
                        if (now >= nextDueNs_) break;
                        if (now >= deadline) return false;
                        if (!streaming_ || paused_) return false;
                        const int64_t remain = nextDueNs_ - now;
                        if (remain > 2'000'000)
                            std::this_thread::sleep_for(std::chrono::nanoseconds(remain - 2'000'000));
                        else
                            std::this_thread::yield();
                    }
                    nextDueNs_ += int64_t(intervalMs_ * 1e6 / speed_);
                }

                uint64_t idx;
                {
                    std::lock_guard<std::mutex> lk(posM_);
                    if (pos_ >= rd_->frameCount()) {
                        if (!looping_) {
                            // End of file is a terminal state, not a timeout. A
                            // consumer that could not tell them apart would spin here.
                            finished_ = true;
                            pool_.wake();
                            return false;
                        }
                        pos_ = 0;
                    }
                    idx = pos_++;
                }

                Frame f;
                if (!pool_.acquire(f)) { err = "frame pool exhausted"; return false; }
                if (!rd_->readFrame(idx, f.pixels.data(), err)) {
                    pool_.recycle(std::move(f));
                    finished_ = true;
                    return false;
                }

                const SerHeaderInfo& hi = rd_->header();
                FrameMeta& m = f.meta;
                m.width = hi.width;
                m.height = hi.height;
                m.stride = hi.width * hi.planes * hi.bytesPerSample;
                m.format = rd_->pixelFormat();
                m.bayer = isMono(m.format) ? BayerPattern::None : rd_->bayerPattern();
                m.significantBits = rd_->significantBits();
                // SER stores samples right-aligned, unlike the left-aligned convention
                // several camera SDKs use -- so no shift, whatever the source camera did.
                m.sampleShift = 0;
                m.exposureUs = int64_t(intervalMs_ * 1000.0);
                m.gain = 0.0;
                // The recording's own time, not now: the seeing history and every
                // logged record should carry when the light arrived, not when it was
                // reprocessed.
                m.hostArrivalNs = int64_t(rd_->frameTimeUnix(idx) * 1e9);
                m.sequence = sequence_++;

                out = std::move(f);
                return true;
            }

            void recycle(Frame&& f) override { pool_.recycle(std::move(f)); }
            int  droppedFrames() const override { return 0; }
            bool finished() const override { return finished_; }
            IPlayback* playback() override { return this; }

            // --- IPlayback ----------------------------------------------------------
            uint64_t frameCount() const override { return rd_->frameCount(); }
            uint64_t position() const override {
                std::lock_guard<std::mutex> lk(posM_);
                return pos_;
            }
            void seek(uint64_t frame) override {
                std::lock_guard<std::mutex> lk(posM_);
                pos_ = std::min(frame, rd_->frameCount());
                finished_ = false;
                nextDueNs_ = int64_t(SDL_GetTicksNS());
            }
            void setPaused(bool p) override { paused_ = p; nextDueNs_ = int64_t(SDL_GetTicksNS()); }
            bool paused() const override { return paused_; }
            void setSpeed(double s) override { speed_ = std::max(0.0, s); }
            double speed() const override { return speed_; }
            void setLooping(bool l) override { looping_ = l; }
            bool looping() const override { return looping_; }

            std::string sourceDescription() const override {
                const SerHeaderInfo& h = rd_->header();
                const SerDetection& d = rd_->detection();
                char buf[512];
                std::snprintf(buf, sizeof(buf),
                    "%dx%d, %d frames, %d bit%s%s\n"
                    "instrument: %s\nobserver: %s\n"
                    "timestamps: %s%s",
                    h.width, h.height, h.frameCount, d.detectedSignificantBits,
                    d.depthOverridden ? " (header said "
                    : "",
                    d.depthOverridden ? (std::to_string(h.pixelDepthPerPlane) + ")").c_str()
                    : "",
                    h.instrument.empty() ? "(none)" : h.instrument.c_str(),
                    h.observer.empty() ? "(none)" : h.observer.c_str(),
                    d.haveTimestamps ? "present" : "absent",
                    d.endiannessOverridden
                    ? "\nNOTE: byte order sniffed from the data; the "
                    "header's LittleEndian flag disagrees and is widely "
                    "mis-set in the wild."
                    : "");
                return buf;
            }

        private:
            CameraDesc   desc_;
            std::unique_ptr<SerReader> rd_;
            Caps         caps_{};
            StreamConfig cfg_{};
            FramePool    pool_;

            mutable std::mutex posM_;
            uint64_t pos_ = 0;

            size_t   frameBytes_ = 0;
            uint64_t sequence_ = 0;
            double   intervalMs_ = 20.0;
            int64_t  nextDueNs_ = 0;

            std::atomic<bool> streaming_{ false };
            std::atomic<bool> finished_{ false };
            std::atomic<bool> paused_{ false };
            std::atomic<bool> looping_{ false };
            std::atomic<double> speed_{ 1.0 };
        };

        // ---------------------------------------------------------------------------

        class SerBackend final : public IBackend {
        public:
            const char* id() const override { return "ser"; }
            const char* displayName() const override { return "SER file replay"; }
            bool loaded() const override { return true; }
            std::string sdkVersion() const override { return "built-in"; }

            bool ensureLoaded(const std::string&, std::string& why) override {
                why = "built-in; choose a file to replay";
                return true;
            }

            std::string diagnostics() const override { return diag_; }

            // Enumeration means "has a file been chosen", so the picker in the connect
            // dialog plays the role a USB scan does for a real camera.
            std::vector<CameraDesc> enumerate() override {
                diag_.clear();
                std::vector<CameraDesc> out;
                const std::string path = serReplayFile();
                if (path.empty()) {
                    diag_ = "No file selected. Use Choose file... in the connect dialog.";
                    return out;
                }

                SerReader probe;
                std::string err;
                if (!probe.open(path, err)) {
                    diag_ = "Cannot read " + path + ": " + err;
                    return out;
                }

                const SerHeaderInfo& h = probe.header();
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%dx%d, %d frames, %d-bit",
                    h.width, h.height, h.frameCount,
                    probe.significantBits());

                CameraDesc d;
                d.backendId = id();
                d.model = std::string("SER: ") + baseName(path);
                d.serial = buf;
                d.backendIndex = 0;
                out.push_back(std::move(d));

                diag_ = path;
                if (probe.detection().endiannessOverridden) {
                    diag_ += "\nByte order sniffed from the data; the header's "
                        "LittleEndian flag disagrees. That flag is widely mis-set, "
                        "so the data wins.";
                }
                if (probe.detection().depthOverridden) {
                    diag_ += "\nHeader claims " + std::to_string(h.pixelDepthPerPlane) +
                        " bits but the data uses " +
                        std::to_string(probe.significantBits()) + ".";
                }
                return out;
            }

            std::unique_ptr<ICamera> open(const CameraDesc& d, std::string& err) override {
                const std::string path = serReplayFile();
                if (path.empty()) { err = "no SER file selected"; return nullptr; }

                auto rd = std::make_unique<SerReader>();
                if (!rd->open(path, err)) return nullptr;
                return std::make_unique<SerCamera>(d, std::move(rd));
            }

        private:
            static std::string baseName(const std::string& p) {
                const size_t i = p.find_last_of("/\\");
                return i == std::string::npos ? p : p.substr(i + 1);
            }

            std::string diag_;
        };

    } // namespace

    std::unique_ptr<IBackend> makeSerBackend() {
        return std::make_unique<SerBackend>();
    }

} // namespace mei