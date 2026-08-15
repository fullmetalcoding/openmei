#include "dimm/processor.h"
#include "camera/backends/synthetic_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

void DimmProcessor::begin() {
    std::lock_guard<std::mutex> lk(m_);
    st_ = DimmStatus{};
    st_.state = DimmState::Acquiring;
    st_.message = "searching for spot pair";
    n_ = 0; meanL_ = m2L_ = meanT_ = m2T_ = 0.0;
    sumSep_ = sumFa_ = sumFb_ = sumNoiseVarPx2_ = 0.0;
    sepEma_ = 0.0;
    lostFrames_ = 0;
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
        s.rejectReason = "saturated";
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
        st_.last = s; st_.haveLast = true;
        return;
    }

    s.accepted = true;
    lastAx_ = s.a.x; lastAy_ = s.a.y;
    lastBx_ = s.b.x; lastBy_ = s.b.y;

    // Welford on the projected components. Differential motion is the deviation
    // from the mean, so report it against the mean so far.
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
        const int nPix = std::max(1, s.a.pixelsUsed);
        sumNoiseVarPx2_ +=
            centroidNoiseVariancePx2(s.a.fwhmPx, s.a.flux, s.a.backgroundSigma, nPix) +
            centroidNoiseVariancePx2(s.b.fwhmPx, s.b.flux, s.b.backgroundSigma, nPix);
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

    if (st_.nAccepted >= cfg_.burst.framesPerBurst) finishBurst();
}

void DimmProcessor::finishBurst() {
    double vL = st_.varLongPx2;
    double vT = st_.varTranPx2;

    if (cfg_.burst.subtractNoiseBias && st_.nAccepted > 0) {
        const double noise = sumNoiseVarPx2_ / st_.nAccepted;
        vL -= noise;
        vT -= noise;
    }

    st_.result = computeSeeing(vL, vT, st_.nAccepted, st_.nRejected, cfg_);
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
    st_.nAccepted = st_.nRejected = 0;
}

void DimmProcessor::accumulate(DimmSample&) {}

} // namespace mei
