#include "dimm/seeing.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <limits>

namespace mei {

namespace {
    template <size_t P, size_t Q>
    double hypergeometricPFQ(const std::array<double, P>& a,
        const std::array<double, Q>& b,
        double z)
    {
        constexpr int kMaxIterations = 10000;
        constexpr double kTolerance = 1e-14;

        double sum = 1.0;
        double term = 1.0;

        for (int n = 1; n <= kMaxIterations; ++n) {
            term *= z / double(n);

            const double nm1 = double(n - 1);

            for (double ai : a)
                term *= ai + nm1;

            for (double bi : b)
                term /= bi + nm1;

            sum += term;

            if (std::abs(term) <=
                kTolerance * std::max(1.0, std::abs(sum))) {
                return sum;
            }
        }

        return std::numeric_limits<double>::quiet_NaN();
    }
constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
}

const char* toString(ScaleMethod m) {
    switch (m) {
        case ScaleMethod::Drift:      return "drift";
        case ScaleMethod::PlateSolve: return "plate solve";
        case ScaleMethod::Computed:   return "computed (estimate)";
        case ScaleMethod::Manual:     return "manual entry";
        default:                      return "not calibrated";
    }
}

const char* toString(CoefficientModel m) {
    switch (m) {
        case CoefficientModel::SarazinRoddierMini: return "Sarazin & Roddier (0.182)";
        case CoefficientModel::Manual:             return "manual K";
        case CoefficientModel::YuSasiela:          return "Yu/Sasiela (full treatment)";
        default:                                   return "Sarazin & Roddier (0.179)";
    }
}

const char* toString(WedgeOrientation w) {
    switch (w) {
        case WedgeOrientation::AcrossBaseline: return "across baseline";
        case WedgeOrientation::ExplicitAngle:  return "explicit angle";
        default:                               return "along baseline";
    }
}
ResponseCoefficients yuSasielaCoefficients(double b)
{
    ResponseCoefficients out;

    // b = d/D. Non-overlapping equal circular subapertures require d >= D.
    if (b < 1.0) {
        out.warning =
            "Yu/Sasiela coefficients require d/D >= 1; "
            "sub-apertures would overlap for this geometry";
        return out;
    }

    const double x = 1.0 / b;     // D/d
    const double z = x * x;

    const double fLong =
        hypergeometricPFQ(
            std::array{ -5.0 / 6.0, 5.0 / 2.0, 1.0 / 6.0, 2.0 / 3.0 },
            std::array{ 5.0, 3.0, -1.0 / 3.0 },
            z);

    const double fTran =
        hypergeometricPFQ(
            std::array{ -5.0 / 6.0, 5.0 / 6.0, 1.0 / 6.0 },
            std::array{ 5.0, 3.0 },
            z);

    if (!std::isfinite(fLong) || !std::isfinite(fTran)) {
        out.warning = "Yu/Sasiela coefficient evaluation did not converge";
        return out;
    }

    const double x13 = std::cbrt(x);

    out.kLong =
        0.364 * (1.0 - 0.531 * x13 * fLong);

    out.kTran =
        0.364 * (1.0 - 0.799 * x13 * fTran);

    out.valid =
        out.kLong > 0.0 &&
        out.kTran > 0.0;

    if (!out.valid) {
        out.warning =
            "Yu/Sasiela geometry produced a non-positive response coefficient";
    }

    return out;
}

ResponseCoefficients responseCoefficients(const OpticsConfig& o,
                                          const ReportingConfig& r) {
    ResponseCoefficients out;
    const double b = o.b();
    if (o.subApertureMm <= 0.0 || o.baselineMm <= 0.0 || b <= 0.0) {
        out.warning = "sub-aperture and baseline must both be positive";
        return out;
    }

    if (r.coefficients == CoefficientModel::Manual) {
        out.kLong = r.manualKLong;
        out.kTran = r.manualKTran;
        out.valid = out.kLong > 0.0 && out.kTran > 0.0;
        if (!out.valid) out.warning = "manual coefficients must be positive";
        return out;
    }
    if (r.coefficients == CoefficientModel::YuSasiela)
        return yuSasielaCoefficients(b);

    // Sarazin & Roddier, rearranged into K form. The leading 0.179 becomes
    // 0.182 in the mini-DIMM analysis, which argues the original is an
    // approximation to Sasiela's result valid for d >= 2D.
    const double c   = (r.coefficients == CoefficientModel::SarazinRoddierMini)
                           ? 0.182 : 0.179;
    const double bm13 = std::pow(b, -1.0 / 3.0);

    out.kLong = 2.0 * c * (1.0 - (0.0968 / c) * bm13);
    out.kTran = 2.0 * c * (1.0 - (0.1450 / c) * bm13);
    out.valid = out.kLong > 0.0 && out.kTran > 0.0;

    if (!out.valid) {
        out.warning = "geometry gives a non-positive response coefficient; "
                      "baseline is far too small relative to the sub-aperture";
    } else if (b < 2.0) {
        // Below b = 2 the two differenced terms are close in size, so errors in
        // D and d amplify, and the approximation itself degrades.
        out.warning =
            "d/D < 2: outside the validated range for the "
            "Sarazin & Roddier approximation; select Yu/Sasiela "
            "for close-spaced sub-apertures";
    }
    return out;
}

double r0FromVariance(double varianceRad2, double k, double subApertureM,
                      double wavelengthM) {
    if (varianceRad2 <= 0.0 || k <= 0.0 || subApertureM <= 0.0) return 0.0;
    // sigma^2 = K lambda^2 D^-1/3 r0^-5/3  =>  r0 = (K lambda^2 D^-1/3 / sigma^2)^(3/5)
    const double num = k * wavelengthM * wavelengthM *
                       std::pow(subApertureM, -1.0 / 3.0);
    return std::pow(num / varianceRad2, 3.0 / 5.0);
}

double airmass(double altitudeDeg) {
    const double alt = std::clamp(altitudeDeg, 1.0, 90.0);
    return 1.0 / std::sin(alt * kDeg2Rad);
}

double r0ToZenith(double r0LineOfSight, double altitudeDeg) {
    if (r0LineOfSight <= 0.0) return 0.0;
    const double cosZ = std::sin(std::clamp(altitudeDeg, 1.0, 90.0) * kDeg2Rad);
    // r0 ∝ (cos z)^(3/5): a line-of-sight measurement through more air gives a
    // smaller r0 than the same turbulence would at zenith.
    return r0LineOfSight / std::pow(cosZ, 3.0 / 5.0);
}

double seeingToZenith(double fwhmLineOfSight, double altitudeDeg) {
    if (fwhmLineOfSight <= 0.0) return 0.0;
    const double cosZ = std::sin(std::clamp(altitudeDeg, 1.0, 90.0) * kDeg2Rad);
    return fwhmLineOfSight * std::pow(cosZ, 3.0 / 5.0);
}

double seeingFromZenith(double fwhmZenith, double altitudeDeg) {
    if (fwhmZenith <= 0.0) return 0.0;
    const double cosZ = std::sin(std::clamp(altitudeDeg, 1.0, 90.0) * kDeg2Rad);
    // Looking through more air degrades seeing, so this always increases the
    // number as altitude falls.
    return fwhmZenith / std::pow(cosZ, 3.0 / 5.0);
}

double extrapolateToZeroExposure(double eps1, double eps2) {
    if (eps1 <= 0.0 || eps2 <= 0.0) return eps1;
    // c1 = (eps1/eps2)^(3/4); eps0 = 0.5 (c1 eps1 + c1^(7/3) eps2).
    // Degenerate when the two exposures give the same answer, which is the
    // no-bias case -- the expression correctly returns eps1 there.
    const double c1 = std::pow(eps1 / eps2, 0.75);
    const double e0 = 0.5 * (c1 * eps1 + std::pow(c1, 7.0 / 3.0) * eps2);
    // A correction should never make seeing better; if it does, the inputs are
    // noise-dominated and the raw short-exposure value is the safer answer.
    return (e0 >= eps1) ? e0 : eps1;
}

double centroidNoiseVariancePx2(double fwhmPx, double fluxE,
                                double backgroundSigmaE, int windowPixels) {
    if (fluxE <= 0.0 || fwhmPx <= 0.0) return 0.0;
    const double sigmaPsf = fwhmPx / 2.3548200450309493;

    // Photon-limited term: sigma_c^2 ~ sigma_psf^2 / N.
    const double photon = (sigmaPsf * sigmaPsf) / fluxE;

    // Background-limited term: grows with window area, which is why an
    // over-large centroid window costs precision even though it captures more
    // of the wings.
    const double n  = std::max(1, windowPixels);
    const double bg = (backgroundSigmaE > 0.0)
        ? (sigmaPsf * sigmaPsf) * (n * backgroundSigmaE * backgroundSigmaE) /
          (fluxE * fluxE)
        : 0.0;

    return photon + bg;
}

SeeingResult computeSeeing(double varLongPx2, double varTranPx2,
                           int framesUsed, int framesRejected,
                           const DimmConfig& cfg) {
    SeeingResult out;
    out.varLongPx2    = varLongPx2;
    out.varTranPx2    = varTranPx2;
    out.framesUsed    = framesUsed;
    out.framesRejected = framesRejected;

    std::string why;
    if (!cfg.canMeasure(why)) { out.reason = why; return out; }
    if (framesUsed < 2) { out.reason = "not enough frames"; return out; }

    const ResponseCoefficients k = responseCoefficients(cfg.optics, cfg.reporting);
    if (!k.valid) { out.reason = k.warning; return out; }

    const double scale  = cfg.calibration.arcsecPerPixel;
    const double lambda = cfg.reporting.wavelengthNm * 1e-9;
    const double D      = cfg.optics.subApertureMm * 1e-3;

    // px^2 -> arcsec^2 -> rad^2
    const double toRad2 = (scale / kArcsecPerRad) * (scale / kArcsecPerRad);
    out.varLongRad2 = varLongPx2 * toRad2;
    out.varTranRad2 = varTranPx2 * toRad2;

    if (out.varLongRad2 <= 0.0 && out.varTranRad2 <= 0.0) {
        // Legitimate in excellent seeing once noise bias is subtracted. Report
        // it rather than clamping to something plausible.
        out.reason = "variance non-positive after noise subtraction; "
                     "seeing may be below the instrument's noise floor";
        return out;
    }

    out.r0Long = r0FromVariance(out.varLongRad2, k.kLong, D, lambda);
    out.r0Tran = r0FromVariance(out.varTranRad2, k.kTran, D, lambda);

    out.fwhmLongArcsec = fwhmFromR0(out.r0Long, lambda) * kArcsecPerRad;
    out.fwhmTranArcsec = fwhmFromR0(out.r0Tran, lambda) * kArcsecPerRad;

    // Both axes measure the same atmosphere. Averaging in seeing rather than in
    // variance keeps the weighting even between them.
    int n = 0;
    double sum = 0.0;
    if (out.fwhmLongArcsec > 0.0) { sum += out.fwhmLongArcsec; ++n; }
    if (out.fwhmTranArcsec > 0.0) { sum += out.fwhmTranArcsec; ++n; }
    if (n == 0) { out.reason = "no usable axis"; return out; }
    out.fwhmArcsec = sum / n;
    out.r0 = r0FromFwhm(out.fwhmArcsec / kArcsecPerRad, lambda);

    if (out.fwhmLongArcsec > 0.0 && out.fwhmTranArcsec > 0.0) {
        out.axisAgreementRatio = out.fwhmLongArcsec / out.fwhmTranArcsec;
        // Kolmogorov turbulence is isotropic, so the two axes should agree once
        // their different response coefficients are applied. A consistent
        // discrepancy points at a swapped axis rather than at the atmosphere.
        out.axisMismatchSuspected =
            out.axisAgreementRatio < 0.80 || out.axisAgreementRatio > 1.25;
    }

    const double alt = cfg.site.manualAltitudeDeg;
    out.airmassUsed = airmass(alt);
    if (cfg.reporting.zenithCorrect) {
        out.fwhmZenithArcsec = seeingToZenith(out.fwhmArcsec, alt);
        out.r0Zenith         = r0ToZenith(out.r0, alt);
    } else {
        out.fwhmZenithArcsec = out.fwhmArcsec;
        out.r0Zenith         = out.r0;
    }

    if (cfg.site.haveScienceTarget) {
        out.scienceAirmass = airmass(cfg.site.scienceAltitudeDeg);
        out.fwhmAtScienceArcsec =
            seeingFromZenith(out.fwhmZenithArcsec, cfg.site.scienceAltitudeDeg);
    }

    // Variance of a variance estimate from n samples is 2/(n-1) fractionally,
    // and seeing goes as variance^(3/5), so the fractional error scales by 3/5.
    const double fracVar = std::sqrt(2.0 / std::max(1, framesUsed - 1));
    out.sigmaArcsec = out.fwhmZenithArcsec * 0.6 * fracVar;

    out.valid = true;
    return out;
}

void applyCorrectedSeeing(SeeingResult& r, double corrected,
                          const DimmConfig& cfg) {
    if (!r.valid || corrected <= 0.0 || r.fwhmArcsec <= 0.0) return;
    const double lambda = cfg.reporting.wavelengthNm * 1e-9;

    r.exposureCorrectionFactor = corrected / r.fwhmArcsec;
    r.fwhmArcsec = corrected;
    r.r0 = r0FromFwhm(corrected / kArcsecPerRad, lambda);

    const double alt = cfg.site.manualAltitudeDeg;
    if (cfg.reporting.zenithCorrect) {
        r.fwhmZenithArcsec = seeingToZenith(r.fwhmArcsec, alt);
        r.r0Zenith         = r0ToZenith(r.r0, alt);
    } else {
        r.fwhmZenithArcsec = r.fwhmArcsec;
        r.r0Zenith         = r.r0;
    }
    if (cfg.site.haveScienceTarget) {
        r.fwhmAtScienceArcsec =
            seeingFromZenith(r.fwhmZenithArcsec, cfg.site.scienceAltitudeDeg);
    }

    const double fracVar = std::sqrt(2.0 / std::max(1, r.framesUsed - 1));
    r.sigmaArcsec = r.fwhmZenithArcsec * 0.6 * fracVar;
}

bool DimmConfig::canMeasure(std::string& why) const {
    if (!calibration.valid()) {
        why = "plate scale is not calibrated -- run the drift calibration "
              "before measuring";
        return false;
    }
    if (optics.subApertureMm <= 0.0 || optics.baselineMm <= 0.0) {
        why = "mask geometry is not set";
        return false;
    }
    const ResponseCoefficients k = responseCoefficients(optics, reporting);
    if (!k.valid) { why = k.warning; return false; }
    return true;
}

} // namespace mei
