// synthetic_backend.h -- a camera that isn't.
//
// Generates a DIMM spot pair whose differential motion is drawn from a *known*
// r0, so the measurement chain can be validated against ground truth.
//
// The motion is temporally correlated with a settable coherence time, and each
// frame integrates over its exposure. That is what makes exposure-time bias
// real here: at 20 ms the recovered seeing should read ~25% low, and the t/2t
// correction should recover it. With per-frame independent motion -- what an
// obvious implementation gives you -- no such bias exists and the correction
// cannot be tested at all.

#pragma once

#include "camera/camera.h"

namespace mei {

struct SyntheticParams {
    // --- ground truth --------------------------------------------------------
    double seeingArcsec = 2.0;      // FWHM at 500 nm, zenith. What we recover.
    double wavelengthNm = 500.0;

    // Atmospheric coherence time. Sets how fast the wavefront tilt decorrelates
    // and therefore how much a finite exposure averages away. Typical sites run
    // 1-10 ms; good sites longer.
    double coherenceTimeMs = 5.0;

    // --- instrument geometry -------------------------------------------------
    double subApertureMm = 25.0;    // D
    double baselineMm    = 120.0;   // d
    double plateScale    = 0.80;    // arcsec/px
    bool   wedgeAlongBaseline = true;

    // --- image formation -----------------------------------------------------
    double separationPx = 100.0;    // wedge deviation, in pixels
    double axisAngleDeg = 0.0;      // baseline orientation on the detector
    double spotFwhmPx   = 5.3;      // diffraction + optics, ex-atmosphere

    // The wedge sits over one subaperture only, so that spot is dimmer by its
    // transmission and smeared along the deviation axis by dispersion. Both are
    // asymmetries between the two spots, and the smear lies on a measurement
    // axis by construction.
    double wedgeTransmission = 0.92;  // 0.92 uncoated (2 x ~4% Fresnel), ~0.99 AR
    double abbeNumber        = 64.2;  // N-BK7
    double bandwidthNm       = 300.0; // passband; a filter shrinks the streak

    // --- photometry ----------------------------------------------------------
    double starFluxE   = 120000.0;  // electrons per spot per frame at 10 ms
    double skyE        = 40.0;      // electrons per pixel per frame
    double readNoiseE  = 1.5;
    int    significantBits = 12;    // exercises the sampleShift path in Mono16

    // Full well sets the electron scale; the ADC spreads it across whatever
    // levels the bit depth gives. So 8-bit is not less headroom, only coarser
    // quantisation -- e-/ADU rises to compensate. Gain then divides it.
    // A fixed e-/ADU would make 8-bit saturate at 255 electrons, which is not
    // how any real sensor behaves.
    double fullWellE   = 38000.0;   // IMX662-ish at low conversion gain

    // Fractional flux modulation per subaperture. Partly shared between the two
    // apertures depending on how the baseline compares to the Fresnel scale.
    double scintillationIndex = 0.06;
    double scintillationCorr  = 0.3;   // 0 = independent, 1 = fully common

    // --- mount ---------------------------------------------------------------
    // Common-mode motion: a random walk, not white noise, because real tracking
    // wanders. A correct analysis is completely insensitive to this.
    double trackingRmsPx  = 2.0;
    double trackingTauMs  = 400.0;

    // --- test modes ----------------------------------------------------------
    // Freezes the atmosphere and the mount so spots sit exactly where commanded.
    // Required for the pixel-phase sweep: without it you cannot separate
    // centroid bias from real motion.
    bool   staticMode    = false;
    double staticOffsetX = 0.0;   // sub-pixel offset applied to both spots
    double staticOffsetY = 0.0;
    bool   noiseFree     = false; // no shot or read noise -- Tier 0 only
    // Skip frame pacing. Tests need thousands of frames; waiting out real
    // exposure time would make the suite take minutes instead of seconds.
    bool   freeRun       = false;

    // --- faults, for testing the guards --------------------------------------
    bool injectHotPixels = false;
    bool injectFieldStar = false;

    unsigned seed = 20260814u;
};

SyntheticParams& syntheticParams();

// Electrons per ADU for the given bit depth and gain. ZWO-style gain units are
// 0.1 dB, so the linear factor is 10^(gain/200).
double syntheticEPerADU(const SyntheticParams&, int significantBits, double gain);

// True zero-exposure differential motion RMS in pixels, plus r0 in metres. A
// finite exposure will measure LESS than sigmaLongPx/sigmaTranPx; that gap is
// the exposure-time bias the analysis has to correct.
void syntheticExpectedSigma(const SyntheticParams&, double& sigmaLongPx,
                            double& sigmaTranPx, double& r0m);

// Rough fraction of the zero-exposure variance surviving an exposure of the
// given length, for the current coherence time. Indicative only -- the real
// number depends on the temporal spectrum, which is the point of measuring it.
double syntheticExposureRetention(const SyntheticParams&, double exposureMs);

// What the generator actually did for a given frame. The exposure-averaged,
// flux-weighted true centroid of each spot -- which is exactly what a perfect
// centroider would return, so the residual is the centroider's error and
// nothing else.
struct SyntheticTruth {
    uint64_t sequence = 0;
    double   ax = 0, ay = 0;
    double   bx = 0, by = 0;
    double   fluxA = 0, fluxB = 0;
    double   diffLongPx = 0, diffTranPx = 0;   // true differential motion
    bool     valid = false;
};

// Look up by frame sequence number. False once the entry has aged out of the
// ring, which is why analysis should consume truth promptly rather than
// batching it up.
bool syntheticTruthFor(uint64_t sequence, SyntheticTruth& out);

std::unique_ptr<IBackend> makeSyntheticBackend();

} // namespace mei
