#include "dimm/processor.h"
#include "camera/backends/synthetic_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace mei {

namespace {
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
constexpr double kRad2Deg = 1.0 / kDeg2Rad;
}

const char* toString(DimmState s) {
    switch (s) {
        case DimmState::Acquiring: return "acquiring";
        case DimmState::Tracking:  return "tracking";
        case DimmState::Measuring: return "measuring";
        default:                   return "idle";
    }
}

void DimmProcessor::setConfig(const DimmConfig& c) {
    std::lock_guard<std::mutex> lk(m_);
    cfg_ = c;
}

DimmConfig DimmProcessor::config() const {
    std::lock_guard<std::mutex> lk(m_);
    return cfg_;
}

void DimmProcessor::setExposureSetter(std::function<bool(int64_t)> fn) {
    std::lock_guard<std::mutex> lk(m_);
    exposureSetter_ = std::move(fn);
}

void DimmProcessor::begin() {
    std::lock_guard<std::mutex> lk(m_);
    st_ = DimmStatus{};
    st_.state = DimmState::Acquiring;
    st_.message = "searching for spot pair";
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    sepEma_ = 0.0;
    lostFrames_ = 0;
    acc_[0].reset(); acc_[1].reset();
    blockCount_ = settleCount_ = 0;
    activeSlot_ = 0;
    satRejects_ = 0;
    st_.interleaving = cfg_.burst.interleaveExposures;
    st_.synthesizing = st_.interleaving &&
                       cfg_.burst.pairing == ExposurePairing::Synthesized;
    pending_ = PendingHalf{};
    intervalEmaNs_ = 0.0;
    lastArrivalNs_ = 0;
    // Only the physical mode touches the camera. Synthesizing leaves exposure
    // alone entirely, which is most of why it is simpler.
    if (st_.interleaving && !st_.synthesizing && exposureSetter_) {
        requestedUs_ = cfg_.burst.baseExposureUs;
        exposureSetter_(requestedUs_);
        settleCount_ = cfg_.burst.interleaveSettleFrames;
    }
}

void DimmProcessor::stopMeasuring() {
    std::lock_guard<std::mutex> lk(m_);
    st_.state = DimmState::Idle;
    st_.message = "stopped";
}

void DimmProcessor::resetBurst() {
    std::lock_guard<std::mutex> lk(m_);
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    st_.nAccepted = st_.nRejected = 0;
    st_.varLongPx2 = st_.varTranPx2 = 0.0;
}

DimmStatus DimmProcessor::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

void DimmProcessor::onFrame(const Frame& f, bool isSynthetic) {
    std::lock_guard<std::mutex> lk(m_);
    if (st_.state == DimmState::Idle) return;

    if (st_.state == DimmState::Acquiring) {
        acquire(f);
        if (st_.state == DimmState::Acquiring) return;
    }
    track(f);

    // Ground-truth comparison. Only the synthetic source can do this, and it is
    // the only way to separate centroider error from everything downstream.
    if (isSynthetic && st_.haveLast) {
        SyntheticTruth t;
        if (syntheticTruthFor(f.meta.sequence, t)) {
            const double dax = st_.last.a.x - t.ax, day = st_.last.a.y - t.ay;
            const double dbx = st_.last.b.x - t.bx, dby = st_.last.b.y - t.by;
            st_.haveTruth = true;
            st_.truthResidualAPx = std::sqrt(dax * dax + day * day);
            st_.truthResidualBPx = std::sqrt(dbx * dbx + dby * dby);
            st_.truthDiffLongPx  = t.diffLongPx;
            st_.truthDiffTranPx  = t.diffTranPx;
        }
    }
}

void DimmProcessor::acquire(const Frame& f) {
    // Predicted separation from the wedge spec, when known. Used only to reject
    // a wrong pair, never to calibrate.
    double expectedSep = 0.0;
    if (cfg_.optics.haveWedgeSpec && cfg_.calibration.valid()) {
        const double devArcsec =
            (cfg_.optics.wedgeIndex - 1.0) * cfg_.optics.wedgeApexArcmin * 60.0;
        expectedSep = devArcsec / cfg_.calibration.arcsecPerPixel;
    }
    st_.expectedSepPx = expectedSep;

    const std::vector<Detection> dets =
        detectSpots(f, cfg_.acquisition, cfg_.centroid, 8);

    Detection a{}, b{};
    std::string why;
    if (!selectPair(dets, cfg_, expectedSep, a, b, why)) {
        st_.message = why + " (" + std::to_string(dets.size()) + " detected)";
        return;
    }

    lastAx_ = a.x; lastAy_ = a.y;
    lastBx_ = b.x; lastBy_ = b.y;

    // The separation vector is the WEDGE DEVIATION axis. Longitudinal and
    // transverse are defined by the mask BASELINE, which is a hardware fact the
    // image cannot reveal -- hence the configured orientation.
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double sepAngle = std::atan2(dy, dx);
    double axisAngle = sepAngle;
    switch (cfg_.optics.wedgeOrientation) {
        case WedgeOrientation::AcrossBaseline:
            axisAngle = sepAngle + 1.5707963267948966;   // +90 deg
            break;
        case WedgeOrientation::ExplicitAngle:
            axisAngle = sepAngle +
                        cfg_.optics.wedgeAngleFromSeparationDeg * kDeg2Rad;
            break;
        default: break;   // AlongBaseline
    }
    axisUx_ = std::cos(axisAngle);
    axisUy_ = std::sin(axisAngle);

    st_.axisAngleDeg = axisAngle * kRad2Deg;
    st_.meanSepPx    = std::sqrt(dx * dx + dy * dy);
    sepEma_          = st_.meanSepPx;
    st_.state        = DimmState::Measuring;
    st_.message      = "locked";
    lostFrames_      = 0;
}

void DimmProcessor::track(const Frame& f) {
    DimmSample s;
    s.sequence    = f.meta.sequence;
    s.timestampNs = f.meta.hostArrivalNs;
    s.exposureUs  = f.meta.exposureUs;

    s.a = measureSpot(f, lastAx_, lastAy_, cfg_.centroid);
    s.b = measureSpot(f, lastBx_, lastBy_, cfg_.centroid);

    if (!s.a.valid || !s.b.valid) {
        s.rejectReason = "spot lost";
        if (++lostFrames_ > 30) {
            st_.state   = DimmState::Acquiring;
            st_.message = "lost both spots -- reacquiring";
        }
        ++st_.nRejected;
        st_.last = s; st_.haveLast = true;
        return;
    }
    lostFrames_ = 0;

    const double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y;
    s.sepPx       = std::sqrt(dx * dx + dy * dy);
    s.sepAngleDeg = std::atan2(dy, dx) * kRad2Deg;

    // Project the separation vector onto the baseline axes. The differential
    // motion is its deviation from the running mean, which is why the mean must
    // be accumulated rather than assumed.
    const double projL =  dx * axisUx_ + dy * axisUy_;
    const double projT = -dx * axisUy_ + dy * axisUx_;

    // --- rejection gates -----------------------------------------------------
    const RejectionConfig& rej = cfg_.rejection;
    if (rej.rejectSaturated && (s.a.saturated || s.b.saturated)) {
        // A clipped core flattens the peak and pulls the centroid toward the
        // middle of the flat region, SUPPRESSING apparent motion -- it makes the
        // instrument look better than it is.
        //
        // With interleaving on, gain must be set for the 2t leg: it carries
        // twice the signal, so a level that is comfortable at t clips at 2t.
        const bool longLeg = st_.interleaving &&
                             s.exposureUs > cfg_.burst.baseExposureUs * 3 / 2;
        char buf[128];
        std::snprintf(buf, sizeof(buf), "saturated (spot %s)%s",
                      s.a.saturated ? (s.b.saturated ? "A+B" : "A") : "B",
                      longLeg ? " on the 2t leg -- reduce gain" : "");
        s.rejectReason = buf;
        ++satRejects_;
    } else if (s.a.snr < rej.minPerFrameSnr || s.b.snr < rej.minPerFrameSnr) {
        s.rejectReason = "low SNR";
    } else if (sepEma_ > 0.0 &&
               std::fabs(s.sepPx - sepEma_) >
                   sepEma_ * rej.maxSeparationDeviationPct / 100.0) {
        // Carry the numbers in the message. A bare "out of range" tells you a
        // gate fired but not whether the frame or the reference is wrong.
        char buf[96];
        std::snprintf(buf, sizeof(buf), "separation %.2f px vs expected %.2f px",
                      s.sepPx, sepEma_);
        s.rejectReason = buf;
    } else if (rej.rejectElongated &&
               (s.a.ellipticity > rej.maxSpotEllipticity ||
                s.b.ellipticity > rej.maxSpotEllipticity)) {
        s.rejectReason = "elongated";
    }

    if (!s.rejectReason.empty()) {
        ++st_.nRejected;
        st_.satRejectFraction = (st_.nAccepted + st_.nRejected) > 0
            ? double(satRejects_) / double(st_.nAccepted + st_.nRejected) : 0.0;
        st_.last = s; st_.haveLast = true;
        return;
    }

    s.accepted = true;

    // Computed here so both the t accumulator and the synthesized pair can use
    // it without recomputing.
    lastNoiseVar_ = 0.0;
    if (cfg_.burst.subtractNoiseBias) {
        const int nPix = std::max(1, s.a.pixelsUsed);
        lastNoiseVar_ =
            centroidNoiseVariancePx2(s.a.fwhmPx, s.a.flux, s.a.backgroundSigma, nPix) +
            centroidNoiseVariancePx2(s.b.fwhmPx, s.b.flux, s.b.backgroundSigma, nPix);
    }

    lastAx_ = s.a.x; lastAy_ = s.a.y;
    lastBx_ = s.b.x; lastBy_ = s.b.y;

    // Bin by the exposure the FRAME reports, not by which block we think we are
    // in. The two disagree for a few frames after every switch, and treating a
    // 2t frame as t would quietly flatten the very bias we are measuring.
    // Duty cycle: what fraction of wall time is spent integrating. Only
    // meaningful for the synthesized leg, but cheap to track always.
    if (lastArrivalNs_ > 0) {
        const double dt = double(s.timestampNs - lastArrivalNs_);
        if (dt > 0.0)
            intervalEmaNs_ = intervalEmaNs_ > 0.0 ? intervalEmaNs_ + 0.02 * (dt - intervalEmaNs_)
                                                  : dt;
    }
    lastArrivalNs_ = s.timestampNs;
    st_.dutyCycle = intervalEmaNs_ > 0.0
        ? std::min(1.0, double(s.exposureUs) * 1000.0 / intervalEmaNs_) : 0.0;

    int slot = 0;
    if (st_.interleaving && !st_.synthesizing) {
        const int64_t t  = cfg_.burst.baseExposureUs;
        const int64_t d0 = std::llabs(s.exposureUs - t);
        const int64_t d1 = std::llabs(s.exposureUs - 2 * t);
        slot = (d1 < d0) ? 1 : 0;

        if (settleCount_ > 0) {
            // Discard while the requested change works through the pipeline.
            --settleCount_;
            s.accepted = false;
            s.rejectReason = "exposure settling";
            ++st_.nRejected;
            st_.last = s; st_.haveLast = true;
            return;
        }
        if (slot != activeSlot_) {
            s.accepted = false;
            s.rejectReason = "exposure mismatch";
            ++st_.nRejected;
            st_.last = s; st_.haveLast = true;
            return;
        }

        // Advance the block on every frame we SEE at this exposure, not on
        // every frame we accept. Counting accepted frames deadlocks: if a gate
        // fires systematically on one leg -- saturation at 2t is the obvious
        // case -- the counter stops, the alternation never switches back, and
        // the instrument sits at the bad setting forever.
        st_.currentExposureUs = s.exposureUs;
        if (exposureSetter_ &&
            ++blockCount_ >= std::max(2, cfg_.burst.interleaveBlockFrames)) {
            blockCount_ = 0;
            activeSlot_ = 1 - activeSlot_;
            const int64_t t = cfg_.burst.baseExposureUs;
            requestedUs_ = activeSlot_ == 0 ? t : 2 * t;
            exposureSetter_(requestedUs_);
            settleCount_ = std::max(1, cfg_.burst.interleaveSettleFrames);
        }
    } else {
        st_.currentExposureUs = s.exposureUs;
    }


    acc_[slot].add(projL, projT);

    if (st_.synthesizing) {
        const double nv = lastNoiseVar_;
        if (pending_.have && s.sequence == pending_.sequence + 1) {
            // Flux-weighted mean per spot, then difference. The two spots carry
            // different weights -- wedge transmission and scintillation are not
            // common -- so they must be combined separately rather than
            // averaging the differential.
            const double wa = pending_.fa + s.a.flux;
            const double wb = pending_.fb + s.b.flux;
            if (wa > 0.0 && wb > 0.0) {
                const double ax = (pending_.fa * pending_.ax + s.a.flux * s.a.x) / wa;
                const double ay = (pending_.fa * pending_.ay + s.a.flux * s.a.y) / wa;
                const double bx = (pending_.fb * pending_.bx + s.b.flux * s.b.x) / wb;
                const double by = (pending_.fb * pending_.by + s.b.flux * s.b.y) / wb;

                const double ddx = bx - ax, ddy = by - ay;
                acc_[1].add( ddx * axisUx_ + ddy * axisUy_,
                            -ddx * axisUy_ + ddy * axisUx_);

                // Averaging two independent measurements reduces centroid noise
                // by the square of the weights, not by half, since the weights
                // are unequal.
                const double w1 = pending_.fa / wa, w2 = s.a.flux / wa;
                acc_[1].noiseSum += w1 * w1 * pending_.noiseVar + w2 * w2 * nv;
            }
            pending_.have = false;
        } else {
            // Start a new non-overlapping pair.
            pending_.have = true;
            pending_.sequence = s.sequence;
            pending_.ax = s.a.x; pending_.ay = s.a.y;
            pending_.bx = s.b.x; pending_.by = s.b.y;
            pending_.fa = s.a.flux; pending_.fb = s.b.flux;
            pending_.noiseVar = nv;
        }
    }

    // Welford on the combined stream, for the live sigma readout.
    ++n_;
    const double dL = projL - meanL_;
    meanL_ += dL / n_;
    m2L_   += dL * (projL - meanL_);
    const double dT = projT - meanT_;
    meanT_ += dT / n_;
    m2T_   += dT * (projT - meanT_);

    s.diffLongPx = projL - meanL_;
    s.diffTranPx = projT - meanT_;

    sumSep_ += s.sepPx;
    sumFa_  += s.a.flux;
    sumFb_  += s.b.flux;

    // Slow EMA so the gate tracks genuine drift (focus, flexure, temperature)
    // without chasing per-frame seeing motion.
    sepEma_ += 0.01 * (s.sepPx - sepEma_);

    // Centroid noise adds variance and biases seeing HIGH -- the opposite
    // direction to exposure bias. Accumulated per frame from the measured SNR
    // so it can be subtracted before inversion.
    if (cfg_.burst.subtractNoiseBias) {
        sumNoiseVarPx2_ += lastNoiseVar_;
        acc_[slot].noiseSum += lastNoiseVar_;
    }

    ++st_.nAccepted;
    const double nAcc = std::max(1, st_.nAccepted);
    st_.meanSepPx = sumSep_ / nAcc;
    st_.meanFluxA = sumFa_ / nAcc;
    st_.meanFluxB = sumFb_ / nAcc;
    st_.fluxRatio = st_.meanFluxA > 0.0 ? st_.meanFluxB / st_.meanFluxA : 0.0;

    if (n_ > 1) {
        st_.varLongPx2 = m2L_ / (n_ - 1);
        st_.varTranPx2 = m2T_ / (n_ - 1);
    }

    st_.last = s;
    st_.haveLast = true;

    st_.nAtT  = acc_[0].n;
    st_.nAt2T = acc_[1].n;

    if (st_.nAccepted >= cfg_.burst.framesPerBurst) finishBurst();
}

void DimmProcessor::finishBurst() {
    auto invert = [&](const VarianceAccumulator& a, int rejected) {
        double vL = a.varL(), vT = a.varT();
        if (cfg_.burst.subtractNoiseBias) {
            const double noise = a.meanNoise();
            vL -= noise;
            vT -= noise;
        }
        return computeSeeing(vL, vT, a.n, rejected, cfg_);
    };

    st_.fwhmAtT = st_.fwhmAt2T = 0.0;
    st_.exposureCorrection = 1.0;

    if (st_.interleaving && acc_[0].n > 2 && acc_[1].n > 2) {
        SeeingResult rT  = invert(acc_[0], st_.nRejected);
        SeeingResult r2T = invert(acc_[1], 0);
        st_.fwhmAtT  = rT.fwhmArcsec;
        st_.fwhmAt2T = r2T.fwhmArcsec;

        if (rT.valid && r2T.valid) {
            // A finite exposure averages correlated image motion and biases
            // seeing LOW, by more at longer exposure. The pair pins down how
            // much, and the extrapolation removes it.
            const double eps0 = extrapolateToZeroExposure(rT.fwhmArcsec,
                                                          r2T.fwhmArcsec);
            applyCorrectedSeeing(rT, eps0, cfg_);
            st_.exposureCorrection = rT.exposureCorrectionFactor;
            st_.result = rT;
        } else {
            st_.result = rT.valid ? rT : r2T;
        }
    } else {
        // Uncorrected. Honest, but reads low by roughly (retention)^0.6 --
        // about 17% at exposure equal to the coherence time.
        st_.result = invert(acc_[0], st_.nRejected);
    }
    st_.haveResult = true;

    const double frac = (st_.nAccepted + st_.nRejected) > 0
        ? double(st_.nRejected) / double(st_.nAccepted + st_.nRejected) : 0.0;
    if (frac > cfg_.rejection.maxRejectedFraction) {
        // Publishing a number computed from whatever survived a heavy rejection
        // pass is worse than publishing nothing.
        st_.result.valid = false;
        st_.result.reason = "rejected fraction too high (" +
                            std::to_string(int(frac * 100)) + "%)";
    }

    // Clear every per-burst accumulator. Missing sumSep_/sumFa_/sumFb_ here
    // meant meanSepPx was recomputed as a whole burst's worth of sum divided by
    // one frame -- the gate then rejected everything from the second burst on.
    // sepEma_ deliberately survives: the pair has not moved.
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    acc_[0].reset(); acc_[1].reset();
    st_.nAccepted = st_.nRejected = 0;
}

void DimmProcessor::accumulate(DimmSample&) {}

} // namespace mei
