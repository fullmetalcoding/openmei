// synthetic_backend.cpp

#include "camera/backends/synthetic_backend.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace mei {

    SyntheticParams& syntheticParams() {
        static SyntheticParams p;
        return p;
    }

    namespace {

        constexpr double kPi = 3.14159265358979323846;
        constexpr double kArcsecPerRad = 206264.806247;
        constexpr double kFwhmToSigma = 1.0 / 2.3548200450309493;

        // std::normal_distribution is far too slow for a per-pixel noise pass at a few
        // hundred frames per second. xorshift128+ with cached Box-Muller is plenty for
        // simulation purposes.
        class FastRng {
        public:
            void seed(uint64_t s) {
                s0_ = s ? s : 0x9E3779B97F4A7C15ull;
                s1_ = s0_ ^ 0xBF58476D1CE4E5B9ull;
                for (int i = 0; i < 16; ++i) next();
                haveSpare_ = false;
                buildTable();
            }
            uint64_t next() {
                uint64_t x = s0_, y = s1_;
                s0_ = y;
                x ^= x << 23;
                s1_ = x ^ y ^ (x >> 17) ^ (y >> 26);
                return s1_ + y;
            }
            double uniform() {   // (0,1)
                return (double((next() >> 11) + 1)) * (1.0 / 9007199254740994.0);
            }
            double normal() {
                if (haveSpare_) { haveSpare_ = false; return spare_; }
                const double u1 = uniform(), u2 = uniform();
                const double r = std::sqrt(-2.0 * std::log(u1));
                const double t = 2.0 * kPi * u2;
                spare_ = r * std::sin(t);
                haveSpare_ = true;
                return r * std::cos(t);
            }

            // Table lookup instead of computing Box-Muller per pixel. A full-frame
            // noise pass needs ~400k deviates per frame at a few hundred fps, which is
            // far beyond what transcendentals per sample can sustain. Values repeat,
            // but they are drawn at random positions from a large table, so there is no
            // spatial structure -- adequate for simulated sensor noise, not for
            // anything claiming statistical rigour.
            float fastNormal() { return table_[next() & (kTableMask)]; }

            // Poisson. Knuth's method loops once per unit of mean, so it is only viable
            // for genuinely small means -- at mean 20 it costs ~21 uniforms per pixel,
            // which dominated everything else. Above the crossover the Gaussian
            // approximation is both accurate and constant-time.
            double shot(double mean) {
                if (mean <= 0.0) return 0.0;
                if (mean > 8.0) return mean + std::sqrt(mean) * fastNormal();
                const double L = std::exp(-mean);
                double p = 1.0;
                int k = 0;
                do { ++k; p *= uniform(); } while (p > L && k < 100);
                return double(k - 1);
            }

            void buildTable() {
                for (size_t i = 0; i < kTableSize; ++i) table_[i] = float(normal());
            }

        private:
            static constexpr size_t kTableSize = 1u << 15;
            static constexpr uint64_t kTableMask = kTableSize - 1;

            uint64_t s0_ = 1, s1_ = 2;
            bool     haveSpare_ = false;
            double   spare_ = 0.0;
            std::vector<float> table_ = std::vector<float>(kTableSize);
        };

        // Ornstein-Uhlenbeck step: stationary, variance sigma^2, correlation time tau.
        inline void ouStep(double& x, double sigma, double dt, double tau, FastRng& rng) {
            if (tau <= 0.0) { x = sigma * rng.normal(); return; }
            const double a = std::exp(-dt / tau);
            x = a * x + sigma * std::sqrt(std::max(0.0, 1.0 - a * a)) * rng.normal();
        }

    } // namespace

    void syntheticExpectedSigma(const SyntheticParams& p, double& sigmaLongPx,
        double& sigmaTranPx, double& r0m) {
        const double lambda = p.wavelengthNm * 1e-9;
        const double epsRad = p.seeingArcsec / kArcsecPerRad;
        r0m = 0.98 * lambda / epsRad;

        const double D = p.subApertureMm * 1e-3;
        const double d = p.baselineMm * 1e-3;

        const double common = 2.0 * lambda * lambda * std::pow(r0m, -5.0 / 3.0);
        const double kD = 0.179 * std::pow(D, -1.0 / 3.0);
        const double varL = common * (kD - 0.0968 * std::pow(d, -1.0 / 3.0));
        const double varT = common * (kD - 0.1450 * std::pow(d, -1.0 / 3.0));

        sigmaLongPx = std::sqrt(std::max(0.0, varL)) * kArcsecPerRad / p.plateScale;
        sigmaTranPx = std::sqrt(std::max(0.0, varT)) * kArcsecPerRad / p.plateScale;
    }

    // For an OU process the variance of the mean over a window T is
    //   sigma^2 * (2 tau / T) * (1 - (tau/T)(1 - e^{-T/tau}))
    // which is the fraction of variance a finite exposure retains.
    double syntheticEPerADU(const SyntheticParams& p, int significantBits, double gain) {
        const double levels = double((1 << significantBits) - 1);
        const double gFac = std::pow(10.0, std::clamp(gain, 0.0, 600.0) / 200.0);
        return std::max(1e-4, p.fullWellE / levels / gFac);
    }

    double syntheticExposureRetention(const SyntheticParams& p, double exposureMs) {
        const double tau = p.coherenceTimeMs;
        const double T = exposureMs;
        if (tau <= 0.0 || T <= 0.0) return 1.0;
        const double r = tau / T;
        return std::clamp(2.0 * r * (1.0 - r * (1.0 - std::exp(-1.0 / r))), 0.0, 1.0);
    }

    namespace {

        class SyntheticCamera final : public ICamera {
        public:
            explicit SyntheticCamera(CameraDesc d) : desc_(std::move(d)) {
                caps_.maxWidth = 1920;
                caps_.maxHeight = 1080;
                caps_.pixelSizeUm = 2.9;
                caps_.adcBits = 12;
                caps_.isColor = false;
                caps_.bins = { 1, 2 };
                caps_.formats = { PixelFormat::Mono8, PixelFormat::Mono16 };
                caps_.exposureUs = { 32, 60'000'000, 5000, true };
                caps_.gain = { 0.0, 500.0, 200.0, true };
                caps_.roiWidthGranularity = 8;
                caps_.roiHeightGranularity = 2;
                caps_.hostUsb = UsbSpeed::Unknown;
                caps_.cameraUsb = UsbSpeed::Unknown;
                caps_.binningIsSoftware = false;
            }

            ~SyntheticCamera() override { stop(); }

            const CameraDesc& desc()   const override { return desc_; }
            const Caps& caps()   const override { return caps_; }
            const StreamConfig& config() const override { return cfg_; }
            bool streaming() const override { return streaming_; }

            StreamConfig configure(const StreamConfig& want, std::string& err) override {
                if (streaming_) { err = "cannot reconfigure while streaming"; return cfg_; }

                StreamConfig c = want;
                c.bin = std::max(1, c.bin);
                c.width = std::min(c.width, caps_.maxWidth / c.bin);
                c.height = std::min(c.height, caps_.maxHeight / c.bin);
                c.width -= c.width % caps_.roiWidthGranularity;
                c.height -= c.height % caps_.roiHeightGranularity;
                if (c.width <= 0 || c.height <= 0) { err = "degenerate ROI"; return cfg_; }

                if (c.format != PixelFormat::Mono8 && c.format != PixelFormat::Mono16) {
                    err = "synthetic source is monochrome";
                    return cfg_;
                }
                c.exposureUs = caps_.exposureUs.clamp(c.exposureUs);
                c.gain = caps_.gain.clamp(c.gain);
                c.monoBin = false;
                c.hardwareBin = false;
                cfg_ = c;
                return cfg_;
            }

            bool setExposureUs(int64_t us, std::string&) override {
                cfg_.exposureUs = caps_.exposureUs.clamp(us);
                return true;
            }
            bool setGain(double g, std::string&) override {
                cfg_.gain = caps_.gain.clamp(g);
                return true;
            }

            bool start(std::string& err) override {
                if (streaming_) return true;
                if (cfg_.width <= 0) { err = "configure() before start()"; return false; }

                bpp_ = bytesPerPixel(cfg_.format);
                frameBytes_ = static_cast<size_t>(cfg_.width) * cfg_.height * bpp_;
                pool_.reset(16, frameBytes_);
                signal_.assign(static_cast<size_t>(cfg_.width) * cfg_.height, 0.0f);

                rng_.seed(syntheticParams().seed);
                diffL_ = diffT_ = comX_ = comY_ = 0.0;
                scintA_ = scintB_ = 0.0;
                sequence_ = 0;
                dropped_ = 0;
                nextDueNs_ = static_cast<int64_t>(SDL_GetTicksNS());
                streaming_ = true;
                return true;
            }

            void stop() override {
                streaming_ = false;
                pool_.wake();
            }

            bool nextFrame(Frame& out, int timeoutMs, std::string& err) override {
                if (!streaming_) { err = "not streaming"; return false; }

                const int64_t intervalNs =
                    std::max<int64_t>(cfg_.exposureUs * 1000 + kReadoutNs, 1'000'000);
                const int64_t deadlineNs =
                    static_cast<int64_t>(SDL_GetTicksNS()) + int64_t(timeoutMs) * 1'000'000;

                // sleep_for() honours nothing finer than the OS timer tick -- 15.6 ms by
                // default on Windows -- so sleeping in small increments overshoots every
                // wait at DIMM frame intervals. Sleep only the bulk, then spin the last
                // millisecond.
                for (;;) {
                    const int64_t now2 = static_cast<int64_t>(SDL_GetTicksNS());
                    if (now2 >= nextDueNs_) break;
                    if (now2 >= deadlineNs) return false;
                    if (!streaming_) { err = "stopped"; return false; }
                    const int64_t remain = nextDueNs_ - now2;
                    if (remain > 2'000'000) {
                        std::this_thread::sleep_for(
                            std::chrono::nanoseconds(remain - 2'000'000));
                    }
                    else {
                        std::this_thread::yield();
                    }
                }
                nextDueNs_ += intervalNs;
                const int64_t now = static_cast<int64_t>(SDL_GetTicksNS());
                if (nextDueNs_ < now - intervalNs) {
                    dropped_ += static_cast<int>((now - nextDueNs_) / intervalNs);
                    nextDueNs_ = now + intervalNs;
                }

                Frame f;
                if (!pool_.acquire(f)) { err = "frame pool exhausted"; return false; }
                render(f);

                FrameMeta& m = f.meta;
                m.width = cfg_.width;
                m.height = cfg_.height;
                m.stride = cfg_.width * bpp_;
                m.format = cfg_.format;
                m.bayer = BayerPattern::None;
                if (bpp_ == 2) {
                    m.significantBits = syntheticParams().significantBits;
                    m.sampleShift = 16 - m.significantBits;
                }
                else {
                    m.significantBits = 8;
                    m.sampleShift = 0;
                }
                m.exposureUs = cfg_.exposureUs;
                m.gain = cfg_.gain;
                m.hostArrivalNs = static_cast<int64_t>(SDL_GetTicksNS());
                m.sequence = sequence_++;

                out = std::move(f);
                return true;
            }

            void recycle(Frame&& f) override { pool_.recycle(std::move(f)); }
            int droppedFrames() const override { return dropped_; }

        private:
            static constexpr int64_t kReadoutNs = 2'000'000;

            // Elliptical Gaussian, evaluated only inside its own bounding box.
            void addSpot(double cx, double cy, double flux,
                double sMaj, double sMin, double ax, double ay) {
                const int W = cfg_.width, H = cfg_.height;
                const double rad = 4.0 * std::max(sMaj, sMin);
                const int x0 = std::max(0, int(std::floor(cx - rad)));
                const int x1 = std::min(W - 1, int(std::ceil(cx + rad)));
                const int y0 = std::max(0, int(std::floor(cy - rad)));
                const int y1 = std::min(H - 1, int(std::ceil(cy + rad)));
                if (x1 < x0 || y1 < y0) return;

                const double amp = flux / (2.0 * kPi * sMaj * sMin);
                const double iMaj = 1.0 / (2.0 * sMaj * sMaj);
                const double iMin = 1.0 / (2.0 * sMin * sMin);

                for (int y = y0; y <= y1; ++y) {
                    const double dy = y - cy;
                    float* row = &signal_[size_t(y) * W];
                    for (int x = x0; x <= x1; ++x) {
                        const double dx = x - cx;
                        const double a = dx * ax + dy * ay;    // along the streak
                        const double b = -dx * ay + dy * ax;    // across it
                        const double e = a * a * iMaj + b * b * iMin;
                        if (e < 12.0) row[x] += float(amp * std::exp(-e));
                    }
                }
            }

            void render(Frame& f) {
                const SyntheticParams& p = syntheticParams();
                const int W = cfg_.width, H = cfg_.height;

                double sigL = 0, sigT = 0, r0 = 0;
                syntheticExpectedSigma(p, sigL, sigT, r0);

                std::fill(signal_.begin(), signal_.end(), 0.0f);

                // Geometry. Longitudinal is the BASELINE axis; the wedge may be clocked
                // across it, in which case the spot separation vector is the transverse
                // axis instead.
                const double th = p.axisAngleDeg * kPi / 180.0;
                const double ux = std::cos(th), uy = std::sin(th);   // longitudinal
                const double vx = -uy, vy = ux;             // transverse
                const double sx = p.wedgeAlongBaseline ? ux : vx;    // deviation axis
                const double sy = p.wedgeAlongBaseline ? uy : vy;

                const double sigma = p.spotFwhmPx * kFwhmToSigma;
                const double expMs = double(cfg_.exposureUs) / 1000.0;
                const double fluxScale = double(cfg_.exposureUs) / 10000.0;
                const double fluxTotal = p.starFluxE * fluxScale;

                // Chromatic smear of the deviated spot: the fan is deviation/V, scaled
                // by how much of the visible band gets through. Treated as a uniform
                // smear, hence /sqrt(12) to an equivalent Gaussian sigma.
                const double bandFrac = std::clamp(p.bandwidthNm / 300.0, 0.0, 1.0);
                const double streakPx = (p.separationPx / std::max(1.0, p.abbeNumber)) * bandFrac;
                const double sMajB = std::sqrt(sigma * sigma +
                    (streakPx * streakPx) / 12.0);

                // Integrate the exposure in substeps so that finite exposure genuinely
                // averages correlated motion. This is what produces exposure-time bias.
                const double subMs = std::max(0.05, p.coherenceTimeMs / 10.0);
                const int    nSub = std::clamp(int(std::ceil(expMs / subMs)), 1, 64);
                const double dt = expMs / nSub;

                for (int s = 0; s < nSub; ++s) {
                    ouStep(diffL_, sigL, dt, p.coherenceTimeMs, rng_);
                    ouStep(diffT_, sigT, dt, p.coherenceTimeMs, rng_);
                    ouStep(comX_, p.trackingRmsPx, dt, p.trackingTauMs, rng_);
                    ouStep(comY_, p.trackingRmsPx, dt, p.trackingTauMs, rng_);

                    const double midX = W * 0.5 + comX_;
                    const double midY = H * 0.5 + comY_;
                    const double half = p.separationPx * 0.5;

                    const double offX = (ux * diffL_ + vx * diffT_) * 0.5;
                    const double offY = (uy * diffL_ + vy * diffT_) * 0.5;

                    const double ax = midX - sx * half + offX;
                    const double ay = midY - sy * half + offY;
                    const double bx = midX + sx * half - offX;
                    const double by = midY + sy * half - offY;

                    // Scintillation, partly shared between the two subapertures.
                    ouStep(scintA_, p.scintillationIndex, dt, 3.0, rng_);
                    ouStep(scintB_, p.scintillationIndex, dt, 3.0, rng_);
                    const double c = std::clamp(p.scintillationCorr, 0.0, 1.0);
                    const double mA = 1.0 + scintA_;
                    const double mB = 1.0 + (c * scintA_ + (1.0 - c) * scintB_);

                    const double fA = fluxTotal / nSub * std::max(0.0, mA);
                    // The wedged aperture loses two Fresnel surfaces, and its image is
                    // smeared along the deviation axis. Both are asymmetries between the
                    // spots, and the smear sits on a measurement axis by construction.
                    const double fB = fluxTotal / nSub * std::max(0.0, mB) * p.wedgeTransmission;

                    addSpot(ax, ay, fA, sigma, sigma, ux, uy);
                    addSpot(bx, by, fB, sMajB, sigma, sx, sy);

                    if (p.injectFieldStar)
                        addSpot(W * 0.25, H * 0.25, fA * 0.15, sigma, sigma, ux, uy);
                }

                // --- detector ---------------------------------------------------------
                const double sky = p.skyE * fluxScale;
                const int    bits = (bpp_ == 2) ? p.significantBits : 8;
                const double maxADU = double((1 << bits) - 1);
                const int    shift = (bpp_ == 2) ? (16 - p.significantBits) : 0;
                const double invEADU = 1.0 / syntheticEPerADU(p, bits, cfg_.gain);

                // The overwhelming majority of pixels see only sky, and for those the
                // shot and read terms collapse into one Gaussian of known variance --
                // a single deviate instead of a Poisson draw plus a normal draw. This
                // is what keeps the generator ahead of the requested cadence.
                const double skySigma = std::sqrt(sky + p.readNoiseE * p.readNoiseE);

                for (int i = 0; i < W * H; ++i) {
                    const double sig = double(signal_[size_t(i)]);
                    double e;
                    if (sig <= 0.0) {
                        e = sky + skySigma * rng_.fastNormal();
                    }
                    else {
                        // Two independent ceilings: the pixel well fills, and the ADC
                        // clips. Either one flattens the core and biases the centroid.
                        e = rng_.shot(std::min(sig + sky, p.fullWellE));
                        e += p.readNoiseE * rng_.fastNormal();
                    }
                    const double adu = std::clamp(e * invEADU, 0.0, maxADU);
                    if (bpp_ == 2) {
                        const uint16_t v = uint16_t(uint16_t(adu) << shift);
                        std::memcpy(&f.pixels[size_t(i) * 2], &v, 2);
                    }
                    else {
                        f.pixels[size_t(i)] = uint8_t(adu);
                    }
                }

                if (p.injectHotPixels) {
                    const uint16_t hot = uint16_t(uint16_t(maxADU) << shift);
                    const int idx[3] = { W / 3 + (H / 3) * W,
                                         W / 2 + (H / 2 + 7) * W,
                                         2 * W / 3 + (2 * H / 3) * W };
                    for (int i : idx) {
                        if (i < 0 || i >= W * H) continue;
                        if (bpp_ == 2) std::memcpy(&f.pixels[size_t(i) * 2], &hot, 2);
                        else           f.pixels[size_t(i)] = 255;
                    }
                }
            }

            CameraDesc   desc_;
            Caps         caps_{};
            StreamConfig cfg_{};
            FramePool    pool_;
            FastRng      rng_;

            std::vector<float> signal_;

            // Persistent atmosphere / mount state -- correlated across frames, which is
            // the whole point.
            double diffL_ = 0, diffT_ = 0;
            double comX_ = 0, comY_ = 0;
            double scintA_ = 0, scintB_ = 0;

            size_t   frameBytes_ = 0;
            int      bpp_ = 1;
            uint64_t sequence_ = 0;
            int      dropped_ = 0;
            int64_t  nextDueNs_ = 0;
            bool     streaming_ = false;
        };

        class SyntheticBackend final : public IBackend {
        public:
            const char* id() const override { return "synthetic"; }
            const char* displayName() const override { return "Synthetic"; }
            bool loaded() const override { return true; }
            std::string sdkVersion() const override { return "built-in"; }

            bool ensureLoaded(const std::string&, std::string& why) override {
                why = "built-in, no SDK required";
                return true;
            }

            std::vector<CameraDesc> enumerate() override {
                CameraDesc d;
                d.backendId = id();
                d.model = "Simulated DIMM pair";
                d.serial = "SYNTH-0";
                d.backendIndex = 0;
                return { d };
            }

            std::unique_ptr<ICamera> open(const CameraDesc& d, std::string&) override {
                return std::make_unique<SyntheticCamera>(d);
            }
        };

    } // namespace

    std::unique_ptr<IBackend> makeSyntheticBackend() {
        return std::make_unique<SyntheticBackend>();
    }

} // namespace mei