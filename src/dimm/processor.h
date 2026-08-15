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

#include <deque>
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

    SeeingResult result;
    bool         haveResult = false;

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

    void begin();          // -> Acquiring
    void stopMeasuring();  // -> Idle
    void resetBurst();

    // Called on the grab thread, once per frame.
    void onFrame(const Frame&, bool isSynthetic);

    DimmStatus status() const;

private:
    void acquire(const Frame&);
    void track(const Frame&);
    void accumulate(DimmSample&);
    void finishBurst();

    mutable std::mutex m_;
    DimmConfig  cfg_;
    DimmStatus  st_;

    // Tracking state.
    double lastAx_ = 0, lastAy_ = 0, lastBx_ = 0, lastBy_ = 0;
    double axisUx_ = 1, axisUy_ = 0;    // longitudinal unit vector
    int    lostFrames_ = 0;

    // Welford accumulators on the projected separation.
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
