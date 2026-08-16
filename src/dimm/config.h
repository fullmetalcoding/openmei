// config.h -- everything that defines a DIMM measurement.
//
// Split by concern rather than by UI layout. Anything here that affects a
// published number must also end up in the per-burst record: a seeing value
// nobody can reconstruct the conditions for is worth very little.

#pragma once

#include <cstdint>
#include <string>

namespace mei {

// -----------------------------------------------------------------------------
//  Optics
// -----------------------------------------------------------------------------

// The wedge deviates one subaperture's image. Which way it is clocked relative
// to the mask baseline is a hardware fact the software cannot infer from an
// image -- and getting it wrong silently swaps sigma_l and sigma_t, which are
// inverted with different coefficients. Wrong answer, no symptom.
enum class WedgeOrientation {
    AlongBaseline,        // separation vector IS the longitudinal axis
    AcrossBaseline,       // separation vector is the TRANSVERSE axis
    ExplicitAngle         // measured angle from the separation vector
};

struct OpticsConfig {
    double subApertureMm = 25.0;    // D -- mask hole diameter
    double baselineMm    = 120.0;   // d -- hole centre separation

    WedgeOrientation wedgeOrientation = WedgeOrientation::AlongBaseline;
    double wedgeAngleFromSeparationDeg = 0.0;   // used when ExplicitAngle

    // Only used to predict where the second spot should land, as an acquisition
    // sanity check. Never used to calibrate: shallow wedges carry proportionally
    // large manufacturing tolerance.
    bool   haveWedgeSpec   = false;
    double wedgeApexArcmin = 30.0;
    double wedgeIndex      = 1.5168;   // N-BK7 at 546 nm
    double wedgeAbbe       = 64.2;
    double separationTolerancePct = 10.0;

    // Which subaperture carries the wedge. That image is dimmer (Fresnel loss)
    // and chromatically streaked along the deviation axis -- an asymmetry that
    // lies on a measurement axis by construction.
    bool   wedgeOnSpotB = true;

    double b() const {              // baseline-to-aperture ratio
        return subApertureMm > 0.0 ? baselineMm / subApertureMm : 0.0;
    }
    // Below ~2 the Sarazin & Roddier approximation degrades and the two terms
    // being differenced are close in size, amplifying geometry errors.
    bool geometryValid() const { return b() >= 2.0; }
};

// -----------------------------------------------------------------------------
//  Plate scale
// -----------------------------------------------------------------------------

enum class ScaleMethod {
    NotCalibrated,
    Drift,          // star trail timing -- the recommended method
    PlateSolve,     // imported from an external solve, mask removed
    Computed,       // focal length x pixel size; an estimate, not a measurement
    Manual          // entered by hand -- legitimate against a synthetic source,
                    // where the scale is a known generator parameter
};

struct CalibrationConfig {
    double      arcsecPerPixel = 0.0;
    ScaleMethod method = ScaleMethod::NotCalibrated;

    // Provenance travels with every published measurement. When someone
    // questions the numbers six months from now, this is what settles it.
    std::string calibratedUtc;          // ISO-8601
    double      fitResidualPx  = 0.0;   // drift fit RMS
    double      declinationDeg = 0.0;   // drift rate is 15.041*cos(dec) "/s
    double      driftSeconds   = 0.0;
    std::string notes;

    // Convenience path only; flagged as an estimate wherever it is reported.
    double focalLengthMm = 750.0;
    double pixelSizeUm   = 2.9;
    int    binning       = 1;

    double computedScale() const {
        return focalLengthMm > 0.0
            ? 206.264806 * pixelSizeUm * binning / focalLengthMm : 0.0;
    }
    bool valid() const {
        return arcsecPerPixel > 0.0 && method != ScaleMethod::NotCalibrated;
    }
};

// -----------------------------------------------------------------------------
//  Acquisition and centroiding
// -----------------------------------------------------------------------------

struct AcquisitionConfig {
    double detectThresholdSigma = 8.0;   // above background, for spot finding
    int    searchWindowPx       = 48;    // tracking window once acquired
    double minSpotSeparationPx  = 20.0;

    // Both spots are the same star, so their burst-mean fluxes should match to
    // within the wedge transmission. Single-frame ratios wander because
    // scintillation is only partly correlated between subapertures.
    double fluxRatioTolerance = 0.25;
    double expectedFluxRatio  = 0.92;    // uncoated wedge, two Fresnel surfaces

    bool   requireUniquePair = true;     // refuse rather than guess
};

enum class BackgroundMethod { AnnulusMedian, FrameMode, FixedLevel };

struct CentroidConfig {
    int  windowRadiusPx = 12;            // half-size of the centroid box

    BackgroundMethod background = BackgroundMethod::AnnulusMedian;
    int    annulusInnerPx = 14;
    int    annulusOuterPx = 22;
    double fixedBackground = 0.0;

    // Threshold is itself a bias knob: too high clips the wings asymmetrically,
    // too low lets noise pull the centroid. Tokovinin shows the recovered
    // response depends measurably on the window size, so both belong in the
    // provenance record.
    double thresholdSigma = 3.0;
    int    iterations     = 2;           // recentre the window, then re-measure

    bool   subtractDark = true;
    bool   maskHotPixels = true;
};

// -----------------------------------------------------------------------------
//  Burst statistics
// -----------------------------------------------------------------------------

// How the 2t leg of the exposure-bias correction is obtained.
enum class ExposurePairing {
    // Pair consecutive t frames and combine their centroids. The centroid of a
    // summed image is the flux-weighted mean of the individual centroids, so no
    // pixel work is needed. Gain is set for t alone, both series come from
    // identical conditions, and nothing has to switch mid-run.
    //
    // Approximate in one respect: a real 2t exposure integrates continuously,
    // while a synthesized one skips the readout gap. Exact only at 100% duty
    // cycle, so the duty cycle is measured and reported.
    Synthesized,
    // Physically alternate the camera between t and 2t. Exact, but gain must be
    // set for the long leg -- costing SNR on the short one -- and the switching
    // introduces settle frames.
    PhysicalAlternation
};

struct BurstConfig {
    int    framesPerBurst   = 500;
    double publishIntervalS = 60.0;

    // The single largest systematic. A finite exposure averages image motion
    // and biases seeing LOW -- roughly 25-30% at 20 ms. Interleaving t and 2t
    // and extrapolating to zero exposure is the standard correction, and it is
    // not optional at any exposure worth using.
    bool    interleaveExposures = true;
    int64_t baseExposureUs      = 5000;
    ExposurePairing pairing = ExposurePairing::Synthesized;

    // Alternate in blocks rather than frame by frame. Exposure changes take
    // effect some frames after the call, so per-frame alternation would spend
    // most of its time in an indeterminate state.
    int interleaveBlockFrames = 50;
    // Frames discarded after each switch. FrameMeta reports what the driver
    // believed was in force, which can lead the hardware by a frame or two.
    int interleaveSettleFrames = 3;

    // Photon noise adds variance and biases seeing HIGH -- the opposite
    // direction. Estimated from per-frame SNR and subtracted.
    bool subtractNoiseBias = true;

    // Frames too close together are correlated, which biases the variance low.
    double minFrameSpacingMs = 0.0;   // 0 = accept the native cadence
};

struct RejectionConfig {
    // Separation far from its running mean means a lost spot, a cloud, or a
    // tracking jump.
    double maxSeparationDeviationPct = 15.0;
    double maxCentroidExcursionPx    = 20.0;
    double minPerFrameSnr            = 10.0;
    bool   rejectSaturated           = true;

    // Above this fraction rejected, discard the whole burst rather than publish
    // a number computed from whatever survived.
    double maxRejectedFraction = 0.25;

    // MASS-DIMM rejects elongated frames as wind-shake spoiled. Tune carefully:
    // an over-eager filter preferentially removes bad-seeing frames and biases
    // the statistics toward good.
    bool   rejectElongated       = false;
    double maxSpotEllipticity    = 0.35;
};

// -----------------------------------------------------------------------------
//  Site and reporting
// -----------------------------------------------------------------------------

struct SiteConfig {
    double latitudeDeg  = 0.0;
    double longitudeDeg = 0.0;
    double elevationM   = 0.0;
    std::string name;

    // Airmass for the zenith correction. Read from a mount over Alpaca where
    // available, otherwise entered by hand.
    bool   useMountAltitude = false;
    double manualAltitudeDeg = 90.0;
};

// Sarazin & Roddier is the standard approximation and what most amateur DIMMs
// use. Tokovinin's coefficients are more accurate, especially at small b, but
// the published K(b) fit is not reproduced here -- enter the values from the
// paper via Manual if you need them. Reference check: at b = 2.5 the G-tilt
// coefficients are K_l = 0.1956, K_t = 0.1270.
enum class CoefficientModel {
    SarazinRoddier,       // 0.179 leading coefficient
    SarazinRoddierMini,   // 0.182 -- the mini-DIMM correction
    Manual                // K_l and K_t supplied directly
};

struct ReportingConfig {
    double wavelengthNm = 500.0;        // DIMM convention; do not vary
    CoefficientModel coefficients = CoefficientModel::SarazinRoddier;
    double manualKLong = 0.1956;
    double manualKTran = 0.1270;

    bool zenithCorrect = true;          // r0 ∝ (cos z)^(3/5)

    std::string sqlitePath = "openmei.sqlite";
    bool        logRawCentroids = true; // keeps re-derivation possible later
};

// -----------------------------------------------------------------------------

struct DimmConfig {
    OpticsConfig      optics;
    CalibrationConfig calibration;
    AcquisitionConfig acquisition;
    CentroidConfig    centroid;
    BurstConfig       burst;
    RejectionConfig   rejection;
    SiteConfig        site;
    ReportingConfig   reporting;

    // Hard gate. An uncalibrated DIMM publishing plausible arcseconds is the
    // worst failure this instrument has, because nothing about the output looks
    // wrong until someone compares against another site.
    bool canMeasure(std::string& why) const;
};

} // namespace mei
