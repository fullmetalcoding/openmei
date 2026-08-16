// seeing.h -- differential image motion variance to r0 and seeing.
//
// Pure functions, no state, no I/O. Everything here is unit-testable against
// the synthetic source, which generates motion from a known r0 using the same
// relations inverted.

#pragma once

#include "dimm/config.h"

#include <string>

namespace mei {

constexpr double kArcsecPerRad = 206264.806247;

// Response coefficients in the form
//   sigma^2 = K * lambda^2 * D^(-1/3) * r0^(-5/3)
// with sigma in radians and D in metres.
struct ResponseCoefficients {
    double kLong = 0.0;
    double kTran = 0.0;
    bool   valid = false;
    std::string warning;   // non-empty when the geometry is outside the model
};

// Sarazin & Roddier expressed in K form:
//   sigma_l^2 = 2 lambda^2 r0^-5/3 [c D^-1/3 - 0.0968 d^-1/3]
//             = 2c * lambda^2 D^-1/3 r0^-5/3 [1 - (0.0968/c) b^-1/3]
// so kLong = 2c(1 - (0.0968/c) b^-1/3), and likewise 0.145 for transverse.
ResponseCoefficients responseCoefficients(const OpticsConfig&,
                                          const ReportingConfig&);

// Fried parameter in metres from a differential motion variance in rad^2.
// Returns 0 for non-positive variance -- which happens legitimately when noise
// bias subtraction overshoots in good seeing, and must not be papered over.
double r0FromVariance(double varianceRad2, double k, double subApertureM,
                      double wavelengthM);

// Seeing FWHM in radians. The 0.98 is the Kolmogorov long-exposure relation.
inline double fwhmFromR0(double r0M, double wavelengthM) {
    return r0M > 0.0 ? 0.98 * wavelengthM / r0M : 0.0;
}
inline double r0FromFwhm(double fwhmRad, double wavelengthM) {
    return fwhmRad > 0.0 ? 0.98 * wavelengthM / fwhmRad : 0.0;
}

// r0 ∝ (cos z)^(3/5). Line-of-sight -> zenith-equivalent.
double r0ToZenith(double r0LineOfSight, double altitudeDeg);
double seeingToZenith(double fwhmLineOfSight, double altitudeDeg);
inline double airmass(double altitudeDeg);

// Tokovinin's modified exponential extrapolation from an interleaved pair.
// eps1 is seeing at the shorter exposure t, eps2 at 2t. Both in the same units;
// the result is the zero-exposure value.
//
// This is not a refinement. At 20 ms the uncorrected value reads roughly 25-30%
// low, so the correction is a third of the answer -- and it grows with wind
// speed, which is not measured.
double extrapolateToZeroExposure(double eps1, double eps2);

// Photon and read noise add variance, biasing seeing HIGH -- opposite to the
// exposure bias. Subtracted before inversion.
inline double subtractNoiseVariance(double measuredVar, double noiseVar) {
    return measuredVar - noiseVar;
}

// Centroid variance contributed by noise, in px^2, for one spot.
// Approximately (FWHM / 2.355)^2 * (nPix * sigmaBg^2) / flux^2 for a
// background-limited centroid; the flux term dominates when photon-limited.
double centroidNoiseVariancePx2(double fwhmPx, double fluxE,
                                double backgroundSigmaE, int windowPixels);

// -----------------------------------------------------------------------------

struct SeeingResult {
    bool   valid = false;
    std::string reason;

    double varLongRad2 = 0.0;      // after rejection, noise subtraction
    double varTranRad2 = 0.0;
    double varLongPx2  = 0.0;      // as measured, for the record
    double varTranPx2  = 0.0;

    double r0Long = 0.0;           // metres, line of sight
    double r0Tran = 0.0;
    double r0     = 0.0;           // combined, line of sight
    double r0Zenith = 0.0;

    double fwhmLongArcsec = 0.0;
    double fwhmTranArcsec = 0.0;
    double fwhmArcsec     = 0.0;   // line of sight
    double fwhmZenithArcsec = 0.0; // the published number

    double sigmaArcsec = 0.0;      // 1-sigma uncertainty
    int    framesUsed  = 0;
    int    framesRejected = 0;
    double airmassUsed = 1.0;
    double exposureCorrectionFactor = 1.0;

    // The two axes are independent estimates of the same quantity and must
    // agree within the noise. Persistent disagreement in a consistent direction
    // is the fingerprint of a swapped longitudinal/transverse axis -- which
    // otherwise produces a wrong answer with no symptom at all.
    double axisAgreementRatio = 1.0;
    bool   axisMismatchSuspected = false;
};

// Full inversion from measured pixel variances.
SeeingResult computeSeeing(double varLongPx2, double varTranPx2,
                           int framesUsed, int framesRejected,
                           const DimmConfig&);

// Replace a result's seeing with an exposure-corrected value and re-derive
// everything that depends on it. Keeps the zenith correction and r0 consistent
// instead of leaving a result whose fields disagree with each other.
void applyCorrectedSeeing(SeeingResult&, double correctedFwhmArcsec,
                          const DimmConfig&);

const char* toString(ScaleMethod);
const char* toString(CoefficientModel);
const char* toString(WedgeOrientation);

} // namespace mei
