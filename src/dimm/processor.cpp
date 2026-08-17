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
    std::lock_guard<std::mutex> lk(cfgM_);
    cfg_ = c;
}

DimmConfig DimmProcessor::config() const {
    std::lock_guard<std::mutex> lk(cfgM_);
    return cfg_;
}

void DimmProcessor::setExposureSetter(std::function<bool(int64_t)> fn) {
    std::lock_guard<std::mutex> lk(cfgM_);
    exposureSetter_ = std::move(fn);
}

// Requests only. The grab thread applies them at a frame boundary, so the UI
// never touches working state and never waits on the measurement.
void DimmProcessor::begin()         { command_ = Command::Begin; }
void DimmProcessor::stopMeasuring() { command_ = Command::Stop; }
void DimmProcessor::resetBurst()    { command_ = Command::ResetBurst; }

DimmStatus DimmProcessor::status() const {
    std::lock_guard<std::mutex> lk(statusM_);
    return published_;
}

void DimmProcessor::publish() {
    std::lock_guard<std::mutex> lk(statusM_);
    published_ = work_;
}

void DimmProcessor::applyBegin(const DimmConfig& cfg) {
    work_ = DimmStatus{};
    work_.state = DimmState::Acquiring;
    work_.message = "searching for spot pair";
    lastAcquireNs_ = 0;
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    sepEma_ = 0.0;
    lostFrames_ = 0;
    burstCounter_ = 0;
    lagSumProd_ = 0.0; lagN_ = 0; havePrevProj_ = false;
    acc_[0].reset(); acc_[1].reset();
    blockCount_ = settleCount_ = 0;
    activeSlot_ = 0;
    satRejects_ = 0;
    work_.interleaving = cfg.burst.interleaveExposures;
    work_.synthesizing = work_.interleaving &&
                       cfg.burst.pairing == ExposurePairing::Synthesized;
    pending_ = PendingHalf{};
    intervalEmaNs_ = 0.0;
    lastArrivalNs_ = 0;
    // Only the physical mode touches the camera. Synthesizing leaves exposure
    // alone entirely, which is most of why it is simpler.
    if (work_.interleaving && !work_.synthesizing && localSetter_) {
        requestedUs_ = cfg.burst.baseExposureUs;
        localSetter_(requestedUs_);
        settleCount_ = cfg.burst.interleaveSettleFrames;
    }
}

void DimmProcessor::applyStop() {
    work_.state = DimmState::Idle;
    work_.message = "stopped";
}

void DimmProcessor::applyResetBurst() {
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    work_.nAccepted = work_.nRejected = 0;
    work_.varLongPx2 = work_.varTranPx2 = 0.0;
}

void DimmProcessor::onFrame(const Frame& f, bool isSynthetic) {
    // Pick up the config once per frame, then run entirely unlocked.
    {
        std::lock_guard<std::mutex> lk(cfgM_);
        localCfg_    = cfg_;
        localSetter_ = exposureSetter_;
    }

    switch (command_.exchange(Command::None)) {
        case Command::Begin:      applyBegin(localCfg_); break;
        case Command::Stop:       applyStop();       break;
        case Command::ResetBurst: applyResetBurst(); break;
        default: break;
    }

    if (work_.state == DimmState::Idle) { publish(); return; }

    if (work_.state == DimmState::Acquiring) {
        // Retrying a full-frame search at frame rate is pure waste, and when
        // the threshold is badly set it is what makes the UI unusable.
        const int64_t now = f.meta.hostArrivalNs;
        if (lastAcquireNs_ != 0 && now - lastAcquireNs_ < 250'000'000) {
            publish();
            return;
        }
        lastAcquireNs_ = now;
        acquire(f);
        if (work_.state == DimmState::Acquiring) { publish(); return; }
    }
    track(f);

    // Ground-truth comparison. Only the synthetic source can do this, and it is
    // the only way to separate centroider error from everything downstream.
    if (isSynthetic && work_.haveLast) {
        SyntheticTruth t;
        if (syntheticTruthFor(f.meta.sequence, t)) {
            const double dax = work_.last.a.x - t.ax, day = work_.last.a.y - t.ay;
            const double dbx = work_.last.b.x - t.bx, dby = work_.last.b.y - t.by;
            work_.haveTruth = true;
            work_.truthResidualAPx = std::sqrt(dax * dax + day * day);
            work_.truthResidualBPx = std::sqrt(dbx * dbx + dby * dby);
            work_.truthDiffLongPx  = t.diffLongPx;
            work_.truthDiffTranPx  = t.diffTranPx;
        }
    }

    publish();
}

void DimmProcessor::acquire(const Frame& f) {
    // Predicted separation from the wedge spec, when known. Used only to reject
    // a wrong pair, never to calibrate.
    double expectedSep = 0.0;
    if (localCfg_.optics.haveWedgeSpec && localCfg_.calibration.valid()) {
        const double devArcsec =
            (localCfg_.optics.wedgeIndex - 1.0) * localCfg_.optics.wedgeApexArcmin * 60.0;
        expectedSep = devArcsec / localCfg_.calibration.arcsecPerPixel;
    }
    work_.expectedSepPx = expectedSep;

    const std::vector<Detection> dets =
        detectSpots(f, localCfg_.acquisition, localCfg_.centroid, 8);

    Detection a{}, b{};
    std::string why;
    if (!selectPair(dets, cfg_, expectedSep, a, b, why)) {
        work_.message = why + " (" + std::to_string(dets.size()) + " detected)";
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
    switch (localCfg_.optics.wedgeOrientation) {
        case WedgeOrientation::AcrossBaseline:
            axisAngle = sepAngle + 1.5707963267948966;   // +90 deg
            break;
        case WedgeOrientation::ExplicitAngle:
            axisAngle = sepAngle +
                        localCfg_.optics.wedgeAngleFromSeparationDeg * kDeg2Rad;
            break;
        default: break;   // AlongBaseline
    }
    axisUx_ = std::cos(axisAngle);
    axisUy_ = std::sin(axisAngle);

    work_.axisAngleDeg = axisAngle * kRad2Deg;
    work_.meanSepPx    = std::sqrt(dx * dx + dy * dy);
    sepEma_          = work_.meanSepPx;
    work_.state        = DimmState::Measuring;
    work_.message      = "locked";
    lostFrames_      = 0;
}

void DimmProcessor::track(const Frame& f) {
    DimmSample s;
    s.sequence    = f.meta.sequence;
    s.timestampNs = f.meta.hostArrivalNs;
    s.exposureUs  = f.meta.exposureUs;

    s.a = measureSpot(f, lastAx_, lastAy_, localCfg_.centroid);
    s.b = measureSpot(f, lastBx_, lastBy_, localCfg_.centroid);

    if (!s.a.valid || !s.b.valid) {
        s.rejectReason = "spot lost";
        if (++lostFrames_ > 30) {
            work_.state   = DimmState::Acquiring;
            work_.message = "lost both spots -- reacquiring";
        }
        ++work_.nRejected;
        work_.last = s; work_.haveLast = true;
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
    const RejectionConfig& rej = localCfg_.rejection;
    if (rej.rejectSaturated && (s.a.saturated || s.b.saturated)) {
        // A clipped core flattens the peak and pulls the centroid toward the
        // middle of the flat region, SUPPRESSING apparent motion -- it makes the
        // instrument look better than it is.
        //
        // With interleaving on, gain must be set for the 2t leg: it carries
        // twice the signal, so a level that is comfortable at t clips at 2t.
        const bool longLeg = work_.interleaving &&
                             s.exposureUs > localCfg_.burst.baseExposureUs * 3 / 2;
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
        ++work_.nRejected;
        work_.satRejectFraction = (work_.nAccepted + work_.nRejected) > 0
            ? double(satRejects_) / double(work_.nAccepted + work_.nRejected) : 0.0;
        work_.last = s; work_.haveLast = true;
        return;
    }

    s.accepted = true;

    // Computed here so both the t accumulator and the synthesized pair can use
    // it without recomputing.
    lastNoiseVar_ = 0.0;
    if (localCfg_.burst.subtractNoiseBias) {
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
    work_.dutyCycle = intervalEmaNs_ > 0.0
        ? std::min(1.0, double(s.exposureUs) * 1000.0 / intervalEmaNs_) : 0.0;

    int slot = 0;
    if (work_.interleaving && !work_.synthesizing) {
        const int64_t t  = localCfg_.burst.baseExposureUs;
        const int64_t d0 = std::llabs(s.exposureUs - t);
        const int64_t d1 = std::llabs(s.exposureUs - 2 * t);
        slot = (d1 < d0) ? 1 : 0;

        if (settleCount_ > 0) {
            // Discard while the requested change works through the pipeline.
            --settleCount_;
            s.accepted = false;
            s.rejectReason = "exposure settling";
            ++work_.nRejected;
            work_.last = s; work_.haveLast = true;
            return;
        }
        if (slot != activeSlot_) {
            s.accepted = false;
            s.rejectReason = "exposure mismatch";
            ++work_.nRejected;
            work_.last = s; work_.haveLast = true;
            return;
        }

        // Advance the block on every frame we SEE at this exposure, not on
        // every frame we accept. Counting accepted frames deadlocks: if a gate
        // fires systematically on one leg -- saturation at 2t is the obvious
        // case -- the counter stops, the alternation never switches back, and
        // the instrument sits at the bad setting forever.
        work_.currentExposureUs = s.exposureUs;
        if (localSetter_ &&
            ++blockCount_ >= std::max(2, localCfg_.burst.interleaveBlockFrames)) {
            blockCount_ = 0;
            activeSlot_ = 1 - activeSlot_;
            const int64_t t = localCfg_.burst.baseExposureUs;
            requestedUs_ = activeSlot_ == 0 ? t : 2 * t;
            localSetter_(requestedUs_);
            settleCount_ = std::max(1, localCfg_.burst.interleaveSettleFrames);
        }
    } else {
        work_.currentExposureUs = s.exposureUs;
    }


    acc_[slot].add(projL, projT);

    // Lag-1 product, for the decorrelation estimate below.
    if (havePrevProj_ && s.sequence == prevSeq_ + 1) {
        lagSumProd_ += projL * prevProjL_;
        ++lagN_;
    }
    prevProjL_    = projL;
    prevSeq_      = s.sequence;
    havePrevProj_ = true;

    if (work_.synthesizing) {
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
    if (localCfg_.burst.subtractNoiseBias) {
        sumNoiseVarPx2_ += lastNoiseVar_;
        acc_[slot].noiseSum += lastNoiseVar_;
    }

    ++work_.nAccepted;
    const double nAcc = std::max(1, work_.nAccepted);
    work_.meanSepPx = sumSep_ / nAcc;
    work_.meanFluxA = sumFa_ / nAcc;
    work_.meanFluxB = sumFb_ / nAcc;
    work_.fluxRatio = work_.meanFluxA > 0.0 ? work_.meanFluxB / work_.meanFluxA : 0.0;

    if (n_ > 1) {
        work_.varLongPx2 = m2L_ / (n_ - 1);
        work_.varTranPx2 = m2T_ / (n_ - 1);
    }

    // Decorrelation time from the lag-1 autocorrelation, assuming exponential
    // decay. Model-dependent: real atmospheric tilt does not decay exponentially
    // (see docs/synthetic-model.md), so treat this as indicative. It is still
    // directly useful -- it says whether consecutive frames are independent,
    // which the variance estimate assumes.
    work_.frameIntervalMs = intervalEmaNs_ / 1e6;
    if (lagN_ > 30 && work_.varLongPx2 > 0.0 && work_.frameIntervalMs > 0.0) {
        const double autocov1 = lagSumProd_ / lagN_ - meanL_ * meanL_;
        const double rho1 = autocov1 / work_.varLongPx2;
        work_.motionRho1 = rho1;
        if (rho1 > 0.02 && rho1 < 0.999) {
            work_.motionTauMs = -work_.frameIntervalMs / std::log(rho1);
            work_.haveMotionTau = true;
        } else {
            // rho1 at the floor means frames are already independent at this
            // cadence, so the timescale is shorter than one frame and cannot be
            // resolved. Saying nothing beats extrapolating.
            work_.haveMotionTau = false;
        }
    }

    work_.last = s;
    work_.haveLast = true;

    work_.nAtT  = acc_[0].n;
    work_.nAt2T = acc_[1].n;

    if (work_.nAccepted >= localCfg_.burst.framesPerBurst) finishBurst();
}

void DimmProcessor::finishBurst() {
    auto invert = [&](const VarianceAccumulator& a, int rejected) {
        double vL = a.varL(), vT = a.varT();
        if (localCfg_.burst.subtractNoiseBias) {
            const double noise = a.meanNoise();
            vL -= noise;
            vT -= noise;
        }
        return computeSeeing(vL, vT, a.n, rejected, cfg_);
    };

    work_.fwhmAtT = work_.fwhmAt2T = 0.0;
    work_.exposureCorrection = 1.0;

    if (work_.interleaving && acc_[0].n > 2 && acc_[1].n > 2) {
        SeeingResult rT  = invert(acc_[0], work_.nRejected);
        SeeingResult r2T = invert(acc_[1], 0);
        work_.fwhmAtT  = rT.fwhmArcsec;
        work_.fwhmAt2T = r2T.fwhmArcsec;

        if (rT.valid && r2T.valid) {
            // A finite exposure averages correlated image motion and biases
            // seeing LOW, by more at longer exposure. The pair pins down how
            // much, and the extrapolation removes it.
            const double eps0 = extrapolateToZeroExposure(rT.fwhmArcsec,
                                                          r2T.fwhmArcsec);
            applyCorrectedSeeing(rT, eps0, cfg_);
            work_.exposureCorrection = rT.exposureCorrectionFactor;
            work_.result = rT;
        } else {
            work_.result = rT.valid ? rT : r2T;
        }
    } else {
        // Uncorrected. Honest, but reads low by roughly (retention)^0.6 --
        // about 17% at exposure equal to the coherence time.
        work_.result = invert(acc_[0], work_.nRejected);
    }
    work_.result.burstSequence = ++burstCounter_;
    work_.burstsCompleted = burstCounter_;
    work_.haveResult = true;

    const double frac = (work_.nAccepted + work_.nRejected) > 0
        ? double(work_.nRejected) / double(work_.nAccepted + work_.nRejected) : 0.0;
    if (frac > localCfg_.rejection.maxRejectedFraction) {
        // Publishing a number computed from whatever survived a heavy rejection
        // pass is worse than publishing nothing.
        work_.result.valid = false;
        work_.result.reason = "rejected fraction too high (" +
                            std::to_string(int(frac * 100)) + "%)";
    }

    // Clear every per-burst accumulator. Missing sumSep_/sumFa_/sumFb_ here
    // meant meanSepPx was recomputed as a whole burst's worth of sum divided by
    // one frame -- the gate then rejected everything from the second burst on.
    // sepEma_ deliberately survives: the pair has not moved.
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    lagSumProd_ = 0.0; lagN_ = 0; havePrevProj_ = false;
    acc_[0].reset(); acc_[1].reset();
    work_.nAccepted = work_.nRejected = 0;
}



} // namespace mei
