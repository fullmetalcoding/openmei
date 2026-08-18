// seeing_history.h -- rolling record of completed bursts.
//
// One sample per burst, not per second. A burst is already an average over
// several hundred frames, so re-binning to a fixed cadence would be
// interpolating data that does not exist at that rate. The plot shows the
// bursts as measured, with a moving average drawn over them for trend.

#pragma once

#include "dimm/seeing.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace mei {

struct HistorySample {
    double   unixSeconds = 0.0;   // for a clock-labelled axis
    double   fwhmZenith  = 0.0;
    double   fwhmLos     = 0.0;
    double   fwhmScience = 0.0;
    double   r0Zenith    = 0.0;   // metres
    double   sigma       = 0.0;
    double   airmass     = 1.0;
    double   exposureCorrection = 1.0;
    int      framesUsed  = 0;
    int      framesRejected = 0;
    uint64_t burstSequence = 0;
};

struct HistoryStats {
    int    count = 0;
    double current = 0.0;
    double median  = 0.0;
    double best    = 0.0;      // smallest FWHM
    double worst   = 0.0;
    double p10 = 0.0, p90 = 0.0;
    double spanSeconds = 0.0;
};

class SeeingHistory {
public:
    // Ignores repeats: bursts are identified by their sequence number, since a
    // steady atmosphere produces near-identical values and frame counts are the
    // same every time by construction.
    bool add(const SeeingResult&, double unixSeconds);

    void   setWindowSeconds(double s);
    double windowSeconds() const { return windowS_; }
    void   clear();

    const std::deque<HistorySample>& samples() const { return samples_; }
    bool   empty() const { return samples_.empty(); }

    // Trailing mean over `spanSeconds`, evaluated at each sample. Returned
    // parallel to samples() so it can be plotted directly.
    std::vector<double> movingAverage(double spanSeconds) const;

    HistoryStats stats() const;

    // Percentiles rather than mean and standard deviation: seeing distributions
    // are strongly right-skewed, so a mean sits above the value you actually
    // spend most of the night at.
    bool writeCsv(const std::string& path, std::string& err) const;

private:
    void prune();

    std::deque<HistorySample> samples_;
    double   windowS_ = 1800.0;
    uint64_t lastSeq_ = 0;
};

} // namespace mei
