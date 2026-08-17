// processor.h -- the measurement loop.
//
// Runs on the grab thread, one call per frame. Centroiding two small windows is
// cheap next to frame delivery, so there is no separate analysis thread; what
// matters is that this sees EVERY frame, unlike the display tap which
// deliberately drops to the newest.

#pragma once

#include "camera/camera.h"
#include "dimm/centroid.h"
#include "dimm/config.h"
#include "dimm/seeing.h"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>

namespace mei {

enum class DimmState { Idle, Acquiring, Tracking, Measuring };

const char* toString(DimmState);

struct DimmSample {
    uint64_t sequence = 0;
    int64_t  timestampNs = 0;
    int64_t  exposureUs = 0;

    SpotMeasurement a, b;
    double sepPx = 0.0;
    double sepAngleDeg = 0.0;

    // Deviation of the separation vector from its running mean, projected onto
    // the baseline axes. This is the measurement.
    double diffLongPx = 0.0;
    double diffTranPx = 0.0;

    bool        accepted = false;
    std::string rejectReason;
};

// One Welford pair per exposure setting. Frames are binned by what their own
// metadata reports, never by call order -- exposure changes land some frames
// after the request, so ordering is not a reliable key.
struct VarianceAccumulator {
    int    n = 0;
    double meanL = 0, m2L = 0;
    double meanT = 0, m2T = 0;
    double noiseSum = 0;

    void reset() { *this = VarianceAccumulator{}; }
    void add(double projL, double projT) {
        ++n;
        const double dL = projL - meanL; meanL += dL / n; m2L += dL * (projL - meanL);
        const double dT = projT - meanT; meanT += dT / n; m2T += dT * (projT - meanT);
    }
    double varL() const { return n > 1 ? m2L / (n - 1) : 0.0; }
    double varT() const { return n > 1 ? m2T / (n - 1) : 0.0; }
    double meanNoise() const { return n > 0 ? noiseSum / n : 0.0; }
};

struct DimmStatus {
    DimmState   state = DimmState::Idle;
    std::string message;

    DimmSample  last;
    bool        haveLast = false;

    // Acquisition results.
    double meanSepPx = 0.0;
    double axisAngleDeg = 0.0;      // longitudinal axis in detector coordinates
    double expectedSepPx = 0.0;

    // Running statistics over the current burst.
    int    nAccepted = 0;
    int    nRejected = 0;
    double varLongPx2 = 0.0;
    double varTranPx2 = 0.0;
    double meanFluxA = 0.0, meanFluxB = 0.0;
    double fluxRatio = 0.0;
    double frameRate = 0.0;

    // Interleave state, surfaced so the correction is auditable rather than
    // something that silently scales the answer.
    bool    interleaving = false;
    int64_t currentExposureUs = 0;
    int     nAtT = 0, nAt2T = 0;
    double  fwhmAtT = 0.0, fwhmAt2T = 0.0;
    double  exposureCorrection = 1.0;
    // Saturation is the one rejection cause that is nearly always a setup
    // error rather than a condition, so it is worth calling out separately.
    double  satRejectFraction = 0.0;
    // Fraction of wall time actually spent integrating. The synthesized 2t leg
    // is exact only at 100%; below that it slightly understates the averaging a
    // real 2t exposure would do, so the correction is mildly conservative.
    double  dutyCycle = 0.0;
    bool    synthesizing = false;

    // Lag-1 autocorrelation of the differential motion, and the decorrelation
    // time implied by it. This is the timescale on which the IMAGE MOTION
    // decorrelates -- not the atmospheric coherence time tau_0, which concerns
    // phase and is shorter. Tilt is dominated by large spatial scales, so it
    // persists longer than the speckle pattern does. Reported under its own
    // name for that reason.
    double  motionRho1 = 0.0;
    double  motionTauMs = 0.0;
    bool    haveMotionTau = false;
    double  frameIntervalMs = 0.0;

    SeeingResult result;
    bool         haveResult = false;
    uint64_t     burstsCompleted = 0;

    // Synthetic only: residual against the generator's own ground truth. This
    // is the number that says whether the centroider is correct, independent of
    // any statistics downstream.
    bool   haveTruth = false;
    double truthResidualAPx = 0.0;
    double truthResidualBPx = 0.0;
    double truthDiffLongPx = 0.0;
    double truthDiffTranPx = 0.0;
};

class DimmProcessor {
public:
    void setConfig(const DimmConfig& c);
    DimmConfig config() const;

    // Wired to the camera by the caller. The processor requests exposure
    // changes; it never touches the camera itself.
    void setExposureSetter(std::function<bool(int64_t)> fn);

    void begin();          // -> Acquiring
    void stopMeasuring();  // -> Idle
    void resetBurst();

    // Called on the grab thread, once per frame.
    void onFrame(const Frame&, bool isSynthetic);

    DimmStatus status() const;

private:
    void acquire(const Frame&);
    void track(const Frame&);
    void finishBurst();
    void publish();
    // Take the config explicitly rather than reading localCfg_. These run
    // inside onFrame() after the snapshot, so the member would be correct
    // today -- but that is an ordering invariant the compiler cannot check,
    // and a future caller outside onFrame() would silently read a
    // default-constructed config instead of failing to build.
    void applyBegin(const DimmConfig&);
    void applyStop();
    void applyResetBurst();

    // Two narrow locks instead of one broad one. The previous single mutex was
    // held for the whole of onFrame() -- including a full-frame spot search --
    // while the UI called status() several times per frame, so a slow
    // acquisition stalled the entire interface.
    mutable std::mutex cfgM_;      // guards cfg_ only
    mutable std::mutex statusM_;   // guards published_ only, held for a copy

    DimmConfig cfg_;          // written by the UI thread
    DimmStatus published_;    // read by the UI thread

    // Grab-thread copy, refreshed once per frame so the rest of the work needs
    // no lock at all.
    DimmConfig localCfg_;

    // Working state, touched only by the grab thread. No lock: onFrame() is
    // called from exactly one thread, and control requests arrive as an atomic
    // rather than by reaching in and mutating this.
    DimmStatus work_;
    enum class Command { None, Begin, Stop, ResetBurst };
    std::atomic<Command> command_{ Command::None };

    // Acquisition is expensive and pointless to retry at frame rate.
    int64_t  lastAcquireNs_ = 0;
    uint64_t burstCounter_ = 0;

    // Tracking state.
    double lastAx_ = 0, lastAy_ = 0, lastBx_ = 0, lastBy_ = 0;
    double axisUx_ = 1, axisUy_ = 0;    // longitudinal unit vector
    int    lostFrames_ = 0;

    // acc_[0] collects the base exposure t, acc_[1] the doubled exposure 2t.
    // With interleaving off only acc_[0] is used.
    VarianceAccumulator acc_[2];
    int     blockCount_ = 0;
    int     settleCount_ = 0;
    int     activeSlot_ = 0;        // which exposure we have REQUESTED
    int     satRejects_ = 0;

    // Half of a synthesized 2t sample, held until its partner arrives. Pairs
    // are non-overlapping: sharing a frame between consecutive pairs would
    // correlate them and bias the variance.
    struct PendingHalf {
        bool     have = false;
        uint64_t sequence = 0;
        double   ax = 0, ay = 0, bx = 0, by = 0;
        double   fa = 0, fb = 0;
        double   noiseVar = 0;
    } pending_;

    // Lag-1 accumulators. Only consecutive accepted frames contribute: a
    // rejected frame breaks the chain, and pairing across a gap would
    // understate the correlation.
    double   lagSumProd_ = 0.0;
    int      lagN_ = 0;
    double   prevProjL_ = 0.0;
    uint64_t prevSeq_ = 0;
    bool     havePrevProj_ = false;

    double lastNoiseVar_ = 0.0;
    double intervalEmaNs_ = 0.0;
    int64_t lastArrivalNs_ = 0;
    int64_t requestedUs_ = 0;
    // Written by the UI thread under cfgM_, read by the grab thread. Copied
    // into localSetter_ alongside the config so the grab thread never touches
    // the shared object -- assigning it once before start() happens to be safe
    // in practice, but it is still a race by the letter of the memory model.
    std::function<bool(int64_t)> exposureSetter_;
    std::function<bool(int64_t)> localSetter_;

    // Combined Welford across both exposures. Not used for the inversion --
    // that reads from acc_[] so the two exposures stay separated -- but it
    // drives the live sigma readout, which should reflect everything measured.
    int    n_ = 0;
    double meanL_ = 0, m2L_ = 0;
    double meanT_ = 0, m2T_ = 0;
    double sumSep_ = 0, sumFa_ = 0, sumFb_ = 0;
    // Separation estimate used by the rejection gate. Deliberately separate
    // from the per-burst sums and carried across burst boundaries: the pair
    // does not move when a burst rolls over, so the gate should not lurch.
    double sepEma_ = 0.0;
    double sumNoiseVarPx2_ = 0;

    int64_t burstStartNs_ = 0;
};

} // namespace mei
