#include "app/seeing_history.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace mei {

bool SeeingHistory::add(const SeeingResult& r, double unixSeconds) {
    if (!r.valid) return false;
    if (r.burstSequence != 0 && r.burstSequence == lastSeq_) return false;
    lastSeq_ = r.burstSequence;

    HistorySample s;
    s.unixSeconds  = unixSeconds;
    s.fwhmZenith   = r.fwhmZenithArcsec;
    s.fwhmLos      = r.fwhmArcsec;
    s.fwhmScience  = r.fwhmAtScienceArcsec;
    s.r0Zenith     = r.r0Zenith;
    s.sigma        = r.sigmaArcsec;
    s.airmass      = r.airmassUsed;
    s.exposureCorrection = r.exposureCorrectionFactor;
    s.framesUsed   = r.framesUsed;
    s.framesRejected = r.framesRejected;
    s.burstSequence = r.burstSequence;

    samples_.push_back(s);
    prune();
    return true;
}

void SeeingHistory::setWindowSeconds(double s) {
    windowS_ = std::max(30.0, s);
    prune();
}

void SeeingHistory::clear() {
    samples_.clear();
    lastSeq_ = 0;
}

void SeeingHistory::prune() {
    if (samples_.empty()) return;
    const double cutoff = samples_.back().unixSeconds - windowS_;
    while (!samples_.empty() && samples_.front().unixSeconds < cutoff)
        samples_.pop_front();

    // Absolute ceiling regardless of window: a very long window with a fast
    // burst cadence should not grow without bound.
    while (samples_.size() > 200000) samples_.pop_front();
}

std::vector<double> SeeingHistory::movingAverage(double spanSeconds) const {
    std::vector<double> out(samples_.size(), 0.0);
    if (samples_.empty()) return out;

    // Trailing window, so the line never anticipates data the observer did not
    // have yet. Two-pointer rather than a fresh scan per point.
    size_t lo = 0;
    double sum = 0.0;
    for (size_t i = 0; i < samples_.size(); ++i) {
        sum += samples_[i].fwhmZenith;
        while (samples_[i].unixSeconds - samples_[lo].unixSeconds > spanSeconds) {
            sum -= samples_[lo].fwhmZenith;
            ++lo;
        }
        out[i] = sum / double(i - lo + 1);
    }
    return out;
}

HistoryStats SeeingHistory::stats() const {
    HistoryStats st;
    if (samples_.empty()) return st;

    std::vector<double> v;
    v.reserve(samples_.size());
    for (const auto& s : samples_) v.push_back(s.fwhmZenith);
    std::sort(v.begin(), v.end());

    auto pct = [&](double p) {
        if (v.empty()) return 0.0;
        const double idx = p * double(v.size() - 1);
        const size_t i = size_t(idx);
        const double f = idx - double(i);
        return (i + 1 < v.size()) ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
    };

    st.count   = int(v.size());
    st.current = samples_.back().fwhmZenith;
    st.median  = pct(0.50);
    st.p10     = pct(0.10);
    st.p90     = pct(0.90);
    st.best    = v.front();
    st.worst   = v.back();
    st.spanSeconds = samples_.back().unixSeconds - samples_.front().unixSeconds;
    return st;
}

bool SeeingHistory::writeCsv(const std::string& path, std::string& err) const {
    FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "wb") != 0) f = nullptr;
#else
    f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) { err = "cannot open " + path; return false; }

    // Both the zenith-normalised and line-of-sight values, plus the geometry
    // that relates them: anyone reanalysing this should be able to redo the
    // projection rather than inherit our arithmetic.
    std::fprintf(f, "utc_iso,unix_seconds,burst,fwhm_zenith_arcsec,"
                    "fwhm_line_of_sight_arcsec,fwhm_science_arcsec,"
                    "r0_zenith_mm,sigma_arcsec,airmass,exposure_correction,"
                    "frames_used,frames_rejected\n");

    for (const auto& s : samples_) {
        const std::time_t t = std::time_t(s.unixSeconds);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char iso[32];
        std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);

        std::fprintf(f, "%s,%.3f,%llu,%.4f,%.4f,%.4f,%.2f,%.4f,%.4f,%.4f,%d,%d\n",
                     iso, s.unixSeconds,
                     static_cast<unsigned long long>(s.burstSequence),
                     s.fwhmZenith, s.fwhmLos, s.fwhmScience,
                     s.r0Zenith * 1000.0, s.sigma, s.airmass,
                     s.exposureCorrection, s.framesUsed, s.framesRejected);
    }
    std::fclose(f);
    return true;
}

} // namespace mei
