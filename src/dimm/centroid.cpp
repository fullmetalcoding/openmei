#include "dimm/centroid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mei {

    namespace {
        constexpr double kFwhmPerSigma = 2.3548200450309493;

        // Ceiling on candidate peaks per frame. Hitting it means the detection
        // threshold is far too low for the current background -- which happens after a
        // bit-depth or ROI change shifts the statistics. Bounded work is better than
        // correct-but-unusable.
        constexpr size_t kMaxPeaks = 2048;
    } // namespace

    // Set by detectSpots when the candidate ceiling was hit.
    thread_local bool detectionOverflowed = false;

    namespace {

        double medianOf(std::vector<double>& v) {
            if (v.empty()) return 0.0;
            const size_t mid = v.size() / 2;
            std::nth_element(v.begin(), v.begin() + mid, v.end());
            return v[mid];
        }
    } // namespace

    void estimateBackground(const Frame& f, double cx, double cy,
        const CentroidConfig& cfg, double& level, double& sigma) {
        level = 0.0;
        sigma = 1.0;

        if (cfg.background == BackgroundMethod::FixedLevel) {
            level = cfg.fixedBackground;
            sigma = std::max(1.0, std::sqrt(std::max(0.0, level)));
            return;
        }

        const int r0 = std::max(1, cfg.annulusInnerPx);
        const int r1 = std::max(r0 + 1, cfg.annulusOuterPx);
        const int ix = int(std::lround(cx)), iy = int(std::lround(cy));

        static thread_local std::vector<double> samples;
        samples.clear();
        samples.reserve(size_t(4 * r1 * r1));
        for (int y = iy - r1; y <= iy + r1; ++y) {
            for (int x = ix - r1; x <= ix + r1; ++x) {
                if (x < 0 || y < 0 || x >= f.meta.width || y >= f.meta.height) continue;
                const double dx = x - cx, dy = y - cy;
                const double rr = dx * dx + dy * dy;
                if (rr < double(r0) * r0 || rr > double(r1) * r1) continue;
                samples.push_back(pixelAt(f, x, y));
            }
        }
        if (samples.size() < 8) return;

        level = medianOf(samples);

        // MAD, scaled to a Gaussian-equivalent sigma. Robust to a field star or a
        // hot pixel landing in the annulus, which a plain stddev is not.
        static thread_local std::vector<double> dev;
        dev.assign(samples.size(), 0.0);
        for (size_t i = 0; i < samples.size(); ++i) dev[i] = std::fabs(samples[i] - level);
        sigma = std::max(1e-3, 1.4826 * medianOf(dev));
    }

    SpotMeasurement measureSpot(const Frame& f, double guessX, double guessY,
        const CentroidConfig& cfg) {
        SpotMeasurement out;
        const int R = std::max(2, cfg.windowRadiusPx);
        const double full = frameFullScale(f);

        double cx = guessX, cy = guessY;
        double bg = 0.0, bgSigma = 1.0;

        const int iters = std::max(1, cfg.iterations);
        for (int it = 0; it < iters; ++it) {
            estimateBackground(f, cx, cy, cfg, bg, bgSigma);
            const double thresh = cfg.thresholdSigma * bgSigma;

            const int ix = int(std::lround(cx)), iy = int(std::lround(cy));
            double sumV = 0.0, sumX = 0.0, sumY = 0.0;
            double peak = 0.0;
            int    used = 0;
            bool   sat = false;

            for (int y = iy - R; y <= iy + R; ++y) {
                for (int x = ix - R; x <= ix + R; ++x) {
                    if (x < 0 || y < 0 || x >= f.meta.width || y >= f.meta.height) continue;
                    const double raw = pixelAt(f, x, y);
                    if (raw >= full) sat = true;
                    const double v = raw - bg;
                    if (v <= thresh) continue;
                    sumV += v;
                    sumX += v * x;
                    sumY += v * y;
                    peak = std::max(peak, v);
                    ++used;
                }
            }

            if (sumV <= 0.0 || used < 4) return out;   // invalid; caller decides

            cx = sumX / sumV;
            cy = sumY / sumV;

            out.flux = sumV;
            out.peak = peak;
            out.pixelsUsed = used;
            out.saturated = sat;
        }

        // Second moments, from the same threshold set. The moment matrix gives both
        // an FWHM and an ellipticity; the latter is what wind-shake rejection keys
        // off, and it is also where the wedge's chromatic streak shows up.
        {
            estimateBackground(f, cx, cy, cfg, bg, bgSigma);
            const double thresh = cfg.thresholdSigma * bgSigma;
            const int ix = int(std::lround(cx)), iy = int(std::lround(cy));

            double sumV = 0.0, mxx = 0.0, myy = 0.0, mxy = 0.0;
            for (int y = iy - R; y <= iy + R; ++y) {
                for (int x = ix - R; x <= ix + R; ++x) {
                    if (x < 0 || y < 0 || x >= f.meta.width || y >= f.meta.height) continue;
                    const double v = pixelAt(f, x, y) - bg;
                    if (v <= thresh) continue;
                    const double dx = x - cx, dy = y - cy;
                    sumV += v;
                    mxx += v * dx * dx;
                    myy += v * dy * dy;
                    mxy += v * dx * dy;
                }
            }
            if (sumV > 0.0) {
                mxx /= sumV; myy /= sumV; mxy /= sumV;
                const double tr = mxx + myy;
                const double det = mxx * myy - mxy * mxy;
                const double disc = std::sqrt(std::max(0.0, 0.25 * tr * tr - det));
                const double lMaj = std::max(1e-9, 0.5 * tr + disc);
                const double lMin = std::max(1e-9, 0.5 * tr - disc);
                out.fwhmPx = kFwhmPerSigma * std::sqrt(0.5 * (lMaj + lMin));
                out.ellipticity = 1.0 - std::sqrt(lMin / lMaj);
            }
        }

        out.x = cx;
        out.y = cy;
        out.background = bg;
        out.backgroundSigma = bgSigma;
        // Background-limited SNR over the pixels actually used.
        out.snr = (bgSigma > 0.0 && out.pixelsUsed > 0)
            ? out.flux / (bgSigma * std::sqrt(double(out.pixelsUsed)))
            : 0.0;
        out.valid = out.flux > 0.0;
        return out;
    }

    std::vector<Detection> detectSpots(const Frame& f, const AcquisitionConfig& acq,
        const CentroidConfig& cen, int maxCount) {
        std::vector<Detection> out;
        const int W = f.meta.width, H = f.meta.height;
        if (W < 8 || H < 8) return out;

        // Coarse global background -- enough to find candidates that a proper
        // windowed measurement then refines.
        static thread_local std::vector<double> samples, dev;
        samples.clear();
        const int step = std::max(1, (W * H) / 4096);
        for (int i = 0; i < W * H; i += step)
            samples.push_back(pixelAt(f, i % W, i / W));
        const double bg = medianOf(samples);
        dev.assign(samples.size(), 0.0);
        for (size_t i = 0; i < samples.size(); ++i) dev[i] = std::fabs(samples[i] - bg);
        const double sigma = std::max(1e-3, 1.4826 * medianOf(dev));

        const double thresh = bg + acq.detectThresholdSigma * sigma;
        const int guard = 3;

        // Phase 1: locate local maxima only. Cheap per pixel, and crucially it does
        // NOT run a full centroid measurement yet -- doing that on every candidate
        // is what turns a badly set threshold into thousands of annulus fits and a
        // frozen interface.
        struct Peak { int x, y; double v; };
        static thread_local std::vector<Peak> peaks;
        peaks.clear();

        bool overflowed = false;
        for (int y = guard; y < H - guard && !overflowed; ++y) {
            for (int x = guard; x < W - guard; ++x) {
                const double v = pixelAt(f, x, y);
                if (v < thresh) continue;
                bool isMax = true;
                for (int dy = -2; dy <= 2 && isMax; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        if ((dx || dy) && pixelAt(f, x + dx, y + dy) > v) { isMax = false; break; }
                if (!isMax) continue;

                peaks.push_back({ x, y, v });
                if (peaks.size() >= kMaxPeaks) {   // hard ceiling, see below
                    overflowed = true;
                    break;
                }
            }
        }
        if (peaks.empty()) return out;
        if (overflowed) {
            // Leave a marker the caller can report. Two spots is the expected
            // count; thousands means the threshold no longer suits the background,
            // which is what a bit-depth or ROI change does to it.
            detectionOverflowed = true;
        }
        else {
            detectionOverflowed = false;
        }

        // Phase 2: keep only the brightest handful. A DIMM has two spots; anything
        // beyond a small multiple of that is noise or a mis-set threshold, and
        // measuring it costs time we do not have at frame rate.
        const size_t keep = std::min(peaks.size(), size_t(std::max(2, maxCount) * 3));
        std::partial_sort(peaks.begin(), peaks.begin() + keep, peaks.end(),
            [](const Peak& a, const Peak& b) { return a.v > b.v; });
        peaks.resize(keep);

        // Phase 3: full measurement, on a bounded set.
        for (const Peak& p : peaks) {
            const SpotMeasurement m = measureSpot(f, p.x, p.y, cen);
            if (!m.valid) continue;

            Detection d;
            d.x = m.x; d.y = m.y; d.flux = m.flux; d.peak = m.peak;

            bool dup = false;
            for (auto& e : out) {
                const double dx = e.x - d.x, dy = e.y - d.y;
                if (dx * dx + dy * dy < 16.0) {
                    if (d.flux > e.flux) e = d;
                    dup = true;
                    break;
                }
            }
            if (!dup) out.push_back(d);
            if (int(out.size()) >= maxCount) break;
        }

        std::sort(out.begin(), out.end(),
            [](const Detection& a, const Detection& b) { return a.flux > b.flux; });
        return out;
    }

    bool selectPair(const std::vector<Detection>& dets, const DimmConfig& cfg,
        double expectedSepPx, Detection& a, Detection& b,
        std::string& why) {
        if (dets.size() < 2) { why = "fewer than two spots detected"; return false; }

        const AcquisitionConfig& acq = cfg.acquisition;
        struct Cand { size_t i, j; double sep, ratio, score; };
        std::vector<Cand> ok;

        for (size_t i = 0; i < dets.size(); ++i) {
            for (size_t j = i + 1; j < dets.size(); ++j) {
                const double dx = dets[j].x - dets[i].x;
                const double dy = dets[j].y - dets[i].y;
                const double sep = std::sqrt(dx * dx + dy * dy);
                if (sep < acq.minSpotSeparationPx) continue;

                // Predicted separation rejects a close double star, a ghost, or a
                // field star adopted after losing a spot. Only available when the
                // wedge is specified -- without it this gate does nothing, which is
                // why the flux ranking below has to carry the decision.
                if (expectedSepPx > 0.0) {
                    const double tol = expectedSepPx * cfg.optics.separationTolerancePct / 100.0;
                    if (std::fabs(sep - expectedSepPx) > tol) continue;
                }

                const double hi = std::max(dets[i].flux, dets[j].flux);
                const double lo = std::min(dets[i].flux, dets[j].flux);
                const double ratio = (hi > 0.0) ? lo / hi : 0.0;
                if (std::fabs(ratio - acq.expectedFluxRatio) > acq.fluxRatioTolerance) continue;

                // A DIMM pair is two images of the same star and is normally the
                // brightest thing in the field by a wide margin, so total flux is
                // the discriminator that actually works when no separation is
                // predicted.
                ok.push_back({ i, j, sep, ratio, dets[i].flux + dets[j].flux });
            }
        }

        if (ok.empty()) { why = "no spot pair passed the separation and flux gates"; return false; }

        std::sort(ok.begin(), ok.end(),
            [](const Cand& x, const Cand& y) { return x.score > y.score; });

        // Ambiguity means a competing interpretation of comparable brightness --
        // not merely that some other pair also cleared the gates. Pairs sharing a
        // detection with the winner are variations on the same identification, so
        // only disjoint pairs count as rivals.
        if (acq.requireUniquePair && ok.size() > 1) {
            const Cand& top = ok.front();
            double rivalScore = 0.0;
            for (size_t k = 1; k < ok.size(); ++k) {
                const Cand& c = ok[k];
                const bool disjoint = c.i != top.i && c.i != top.j &&
                    c.j != top.i && c.j != top.j;
                if (disjoint) { rivalScore = c.score; break; }
            }
            // A clear winner is accepted. Refusal is reserved for the case where
            // picking wrong is a real risk, because a silently wrong pair produces
            // plausible seeing indefinitely.
            if (rivalScore > 0.0 && top.score < rivalScore * 1.5) {
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                    "ambiguous: two comparable pairs (flux %.0f vs %.0f) "
                    "-- narrow the detection threshold or specify the wedge",
                    top.score, rivalScore);
                why = buf;
                return false;
            }
        }

        a = dets[ok.front().i];
        b = dets[ok.front().j];
        return true;
    }

} // namespace mei