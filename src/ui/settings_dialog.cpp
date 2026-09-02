#include "ui/settings_dialog.h"
#include "dimm/seeing.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace mei::ui {

    namespace {

        const ImVec4 kWarn{ 0.90f, 0.70f, 0.25f, 1.0f };
        const ImVec4 kBad{ 0.90f, 0.45f, 0.35f, 1.0f };
        const ImVec4 kGood{ 0.45f, 0.80f, 0.50f, 1.0f };

        bool DragD(const char* label, double* v, double step, double lo, double hi,
            const char* fmt = "%.3f") {
            float f = float(*v);
            const bool changed = ImGui::DragFloat(label, &f, float(step), float(lo),
                float(hi), fmt);
            if (changed) *v = double(f);
            return changed;
        }

        void InputStr(const char* label, std::string& s, size_t cap = 128) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", s.c_str());
            if (ImGui::InputText(label, buf, std::min(cap, sizeof(buf)))) s = buf;
        }

    } // namespace

    void SettingsDialog::draw(DimmConfig& cfg) {
        if (requestOpen_) {
            ImGui::OpenPopup("Settings");
            requestOpen_ = false;
            backup_ = cfg;
            haveBackup_ = true;
        }

        ImGui::SetNextWindowSize(ImVec2(660, 560), ImGuiCond_FirstUseEver);
        if (!ImGui::BeginPopupModal("Settings", nullptr,
            ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }

        // Gate state up top: this is the thing that decides whether anything can be
        // published, so it should not be buried in a tab.
        std::string why;
        if (cfg.canMeasure(why)) {
            ImGui::TextColored(kGood, "Ready to measure.");
        }
        else {
            ImGui::TextColored(kBad, "Cannot measure: %s", why.c_str());
        }
        ImGui::Separator();

        if (ImGui::BeginTabBar("##settings")) {
            if (ImGui::BeginTabItem("Optics")) { drawOptics(cfg);      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Plate scale")) { drawCalibration(cfg); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Detection")) { drawDetection(cfg);   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Statistics")) { drawStatistics(cfg);  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Site")) { drawSite(cfg);        ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Reporting")) { drawReporting(cfg);   ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(100, 0))) {
            haveBackup_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            if (haveBackup_) cfg = backup_;
            haveBackup_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // -----------------------------------------------------------------------------

    void SettingsDialog::drawOptics(DimmConfig& cfg) {
        OpticsConfig& o = cfg.optics;

        ImGui::SeparatorText("Aperture mask");
        DragD("Sub-aperture D (mm)", &o.subApertureMm, 0.5, 5.0, 200.0, "%.1f");
        ImGui::SetItemTooltip(
            "Mask hole diameter. Must stay comfortably inside r0 or the spot breaks "
            "into speckle: at 2-3\" seeing r0 is only 35-50 mm, so 25 mm is a good "
            "choice and 50 mm is not.");
        DragD("Baseline d (mm)", &o.baselineMm, 1.0, 10.0, 500.0, "%.1f");
        ImGui::SetItemTooltip("Centre-to-centre hole separation. Enters r0 directly, "
            "so measure it rather than trusting the drawing.");

        const double b = o.b();
        ImGui::Text("d/D = %.2f", b);
        ImGui::SameLine();
        if (b >= 3.0) {
            ImGui::TextColored(kGood, "comfortable");
        }
        else if (b >= 2.0) {
            ImGui::TextColored(kWarn, "at the validity edge");
            ImGui::SetItemTooltip("Below d/D = 2 the approximation degrades and the "
                "two differenced terms are close in size, so "
                "geometry errors are amplified.");
        }
        else {
            ImGui::TextColored(kBad, "invalid");
        }

        ImGui::SeparatorText("Wedge");
        int wo = int(o.wedgeOrientation);
        if (ImGui::Combo("Deviation vs baseline", &wo,
            "Along baseline\0Across baseline\0Explicit angle\0")) {
            o.wedgeOrientation = WedgeOrientation(wo);
        }
        ImGui::SetItemTooltip(
            "How the wedge is clocked relative to the mask baseline. This cannot be "
            "inferred from an image: the spot separation vector is the DEVIATION "
            "axis, while longitudinal and transverse are defined by the BASELINE. "
            "Getting it wrong swaps the two variances, which are inverted with "
            "different coefficients -- a wrong answer with no symptom.");
        if (o.wedgeOrientation == WedgeOrientation::ExplicitAngle) {
            DragD("Angle from separation (deg)", &o.wedgeAngleFromSeparationDeg,
                0.5, -180.0, 180.0, "%.1f");
        }

        ImGui::Checkbox("Wedge is on spot B", &o.wedgeOnSpotB);
        ImGui::SetItemTooltip("That spot is dimmer by the wedge transmission and "
            "chromatically streaked along the deviation axis.");

        ImGui::Checkbox("Wedge spec known (acquisition check only)", &o.haveWedgeSpec);
        if (o.haveWedgeSpec) {
            ImGui::Indent();
            DragD("Apex angle (arcmin)", &o.wedgeApexArcmin, 0.5, 0.5, 240.0, "%.2f");
            DragD("Refractive index", &o.wedgeIndex, 0.001, 1.3, 2.0, "%.4f");
            DragD("Abbe number", &o.wedgeAbbe, 0.5, 20.0, 100.0, "%.1f");
            DragD("Separation tolerance (%)", &o.separationTolerancePct,
                0.5, 1.0, 50.0, "%.1f");

            const double devArcsec = (o.wedgeIndex - 1.0) * o.wedgeApexArcmin * 60.0;
            ImGui::Text("Predicted deviation: %.0f arcsec", devArcsec);
            if (cfg.calibration.valid()) {
                ImGui::Text("  = %.0f px at %.3f\"/px", devArcsec /
                    cfg.calibration.arcsecPerPixel,
                    cfg.calibration.arcsecPerPixel);
            }
            ImGui::TextDisabled("Chromatic streak ~ deviation / Abbe = %.1f arcsec "
                "over the full visible band.",
                devArcsec / o.wedgeAbbe);
            ImGui::TextWrapped(
                "Used only to reject a wrong spot pair -- a close double star, a "
                "ghost, or a field star adopted after losing one spot. Never used "
                "to calibrate: shallow wedges carry proportionally large tolerance.");
            ImGui::Unindent();
        }
    }

    void SettingsDialog::drawCalibration(DimmConfig& cfg) {
        CalibrationConfig& c = cfg.calibration;

        ImGui::TextWrapped(
            "Everything scales as arcsec/pixel squared, and r0 goes as scale^(-6/5), "
            "so a 5%% scale error becomes a 6%% seeing error. Measure it; do not "
            "compute it from a nameplate focal length.");
        ImGui::Separator();

        if (DragD("Plate scale (\"/px)", &c.arcsecPerPixel, 0.001, 0.0, 10.0, "%.4f")) {
            // Typing a value is a deliberate act, so treat it as a calibration --
            // otherwise the measurement gate can never be cleared by hand, which
            // blocks testing against a synthetic source whose scale is known.
            if (c.method == ScaleMethod::NotCalibrated) c.method = ScaleMethod::Manual;
        }
        ImGui::Text("Method: %s", toString(c.method));
        if (c.method == ScaleMethod::Computed) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "(estimate)");
        }
        if (!c.calibratedUtc.empty())
            ImGui::TextDisabled("Calibrated %s, fit residual %.3f px",
                c.calibratedUtc.c_str(), c.fitResidualPx);

        ImGui::SeparatorText("Drift calibration (recommended)");
        ImGui::TextWrapped(
            "Stop tracking and time the star crossing a known number of pixels. "
            "Rate is 15.041 x cos(dec) arcsec/sec. Works with the mask on and needs "
            "only the one bright star you are already using; the fitted direction "
            "also gives the RA axis in detector coordinates for free. Calibrate near "
            "the meridian above 45 deg -- differential refraction compresses the "
            "apparent rate near the horizon and biases the scale low.");
        DragD("Declination used (deg)", &c.declinationDeg, 0.5, -89.0, 89.0, "%.2f");
        DragD("Drift duration (s)", &c.driftSeconds, 1.0, 0.0, 600.0, "%.1f");
        if (ImGui::Button("Run drift calibration...")) {
            // Wizard lands here.
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(not implemented yet)");

        ImGui::SeparatorText("Fallbacks");
        DragD("Focal length (mm)", &c.focalLengthMm, 1.0, 50.0, 5000.0, "%.1f");
        DragD("Pixel size (um)", &c.pixelSizeUm, 0.01, 0.5, 20.0, "%.2f");
        ImGui::InputInt("Binning", &c.binning);
        c.binning = std::max(1, c.binning);
        ImGui::Text("Computed: %.4f \"/px", c.computedScale());
        ImGui::SameLine();
        if (ImGui::SmallButton("Use as estimate")) {
            c.arcsecPerPixel = c.computedScale();
            c.method = ScaleMethod::Computed;
        }
        ImGui::TextWrapped(
            "Nameplate focal lengths are routinely several percent off, and on an "
            "SCT or Maksutov focusing moves the primary and changes the focal "
            "length outright -- so a computed scale drifts with temperature.");

        InputStr("Notes", c.notes);
    }

    void SettingsDialog::drawDetection(DimmConfig& cfg) {
        AcquisitionConfig& a = cfg.acquisition;
        CentroidConfig& n = cfg.centroid;

        ImGui::SeparatorText("Spot acquisition");
        DragD("Detection threshold (sigma)", &a.detectThresholdSigma, 0.5, 3.0, 50.0, "%.1f");
        ImGui::InputInt("Tracking window (px)", &a.searchWindowPx);
        DragD("Min spot separation (px)", &a.minSpotSeparationPx, 1.0, 4.0, 500.0, "%.0f");
        DragD("Expected flux ratio B/A", &a.expectedFluxRatio, 0.01, 0.5, 1.0, "%.3f");
        ImGui::SetItemTooltip("Both spots are the same star, so their burst-mean "
            "fluxes differ only by the wedge transmission -- about "
            "0.92 uncoated, ~0.99 AR-coated.");
        DragD("Flux ratio tolerance", &a.fluxRatioTolerance, 0.01, 0.02, 1.0, "%.2f");
        ImGui::Checkbox("Refuse to guess when the pair is ambiguous", &a.requireUniquePair);
        ImGui::SetItemTooltip(
            "If more than one candidate pair passes the gates, ask rather than pick "
            "the best-scoring one. Silently choosing wrong produces plausible "
            "arcseconds indefinitely -- the failure mode this instrument is most "
            "vulnerable to.");

        ImGui::SeparatorText("Centroiding");
        ImGui::InputInt("Window radius (px)", &n.windowRadiusPx);
        ImGui::SetItemTooltip("Tokovinin shows the measured response depends on this, "
            "so it belongs in the provenance record.");
        int bg = int(n.background);
        if (ImGui::Combo("Background", &bg, "Annulus median\0Frame mode\0Fixed level\0"))
            n.background = BackgroundMethod(bg);
        if (n.background == BackgroundMethod::AnnulusMedian) {
            ImGui::InputInt("Annulus inner (px)", &n.annulusInnerPx);
            ImGui::InputInt("Annulus outer (px)", &n.annulusOuterPx);
        }
        else if (n.background == BackgroundMethod::FixedLevel) {
            DragD("Fixed level (ADU)", &n.fixedBackground, 1.0, 0.0, 65535.0, "%.0f");
        }
        DragD("Threshold above background (sigma)", &n.thresholdSigma, 0.1, 0.0, 20.0, "%.2f");
        ImGui::SetItemTooltip("A bias knob in both directions: too high clips the "
            "wings asymmetrically, too low lets noise pull the "
            "centroid.");
        ImGui::InputInt("Recentre iterations", &n.iterations);
        ImGui::Checkbox("Subtract dark / hot-pixel map", &n.subtractDark);
        ImGui::Checkbox("Mask hot pixels in window", &n.maskHotPixels);
        ImGui::SetItemTooltip("A hot pixel inside a centroid window pulls that "
            "centroid the same way every frame.");
    }

    void SettingsDialog::drawStatistics(DimmConfig& cfg) {
        BurstConfig& b = cfg.burst;
        RejectionConfig& r = cfg.rejection;

        ImGui::SeparatorText("Burst");
        ImGui::InputInt("Frames per burst", &b.framesPerBurst);
        DragD("Publish interval (s)", &b.publishIntervalS, 1.0, 1.0, 3600.0, "%.0f");
        DragD("Min frame spacing (ms)", &b.minFrameSpacingMs, 0.1, 0.0, 100.0, "%.1f");
        ImGui::SetItemTooltip("Frames too close together are correlated, which "
            "biases the variance low. 0 accepts the native cadence.");

        ImGui::SeparatorText("Bias corrections");
        ImGui::Checkbox("Interleave t / 2t exposures", &b.interleaveExposures);
        ImGui::TextWrapped(
            "A finite exposure averages image motion and biases seeing LOW -- around "
            "25-30%% at 20 ms, under 3%% at 1 ms. Interleaving an exposure and its "
            "double and extrapolating to zero is the standard correction, and at any "
            "usable exposure it is a substantial fraction of the answer rather than "
            "a refinement.");
        int pm = int(b.pairing);
        if (ImGui::Combo("2t source", &pm,
            "Synthesized from frame pairs\0Physically alternate exposure\0"))
            b.pairing = ExposurePairing(pm);
        if (b.pairing == ExposurePairing::Synthesized) {
            ImGui::TextWrapped(
                "Pairs consecutive frames and combines their centroids -- the "
                "centroid of a summed image is the flux-weighted mean of the "
                "individual centroids, so no pixel work is needed. Exposure never "
                "changes, so gain is set for t alone rather than compromised for "
                "the long leg, and both series come from identical conditions. "
                "Approximate only in skipping the readout gap between frames.");
        }
        else {
            ImGui::TextWrapped(
                "Exact, but gain must be set so the 2t leg does not clip -- which "
                "costs SNR on the t leg -- and switching needs settle frames. Useful "
                "as a cross-check: if the two modes disagree, the synthesized "
                "duty-cycle approximation is not holding.");
            ImGui::InputInt("Block length (frames)", &b.interleaveBlockFrames);
            ImGui::InputInt("Settle frames", &b.interleaveSettleFrames);
        }

        int expUs = int(b.baseExposureUs);
        if (ImGui::InputInt("Base exposure t (us)", &expUs, 100, 1000))
            b.baseExposureUs = std::max(32, expUs);
        ImGui::TextDisabled("Interleaved pair: %.2f ms and %.2f ms",
            b.baseExposureUs / 1000.0, b.baseExposureUs / 500.0);

        ImGui::Checkbox("Subtract centroid noise variance", &b.subtractNoiseBias);
        ImGui::TextWrapped("Photon and read noise add variance and bias seeing HIGH "
            "-- the opposite direction to exposure bias.");

        ImGui::SeparatorText("Frame rejection");
        DragD("Max separation deviation (%)", &r.maxSeparationDeviationPct, 0.5, 1.0, 50.0, "%.1f");
        DragD("Max centroid excursion (px)", &r.maxCentroidExcursionPx, 0.5, 1.0, 200.0, "%.1f");
        DragD("Min per-frame SNR", &r.minPerFrameSnr, 0.5, 1.0, 200.0, "%.1f");
        ImGui::Checkbox("Reject saturated frames", &r.rejectSaturated);
        ImGui::SetItemTooltip("A clipped core flattens the peak and pulls the "
            "centroid toward the middle of the flat region, which "
            "SUPPRESSES apparent motion -- it makes the instrument "
            "look good while being wrong.");
        ImGui::Checkbox("Reject elongated spots (wind shake)", &r.rejectElongated);
        if (r.rejectElongated) {
            ImGui::Indent();
            DragD("Max ellipticity", &r.maxSpotEllipticity, 0.01, 0.05, 1.0, "%.2f");
            ImGui::TextColored(kWarn, "Tune carefully.");
            ImGui::SameLine();
            ImGui::TextWrapped("An over-eager filter preferentially removes "
                "bad-seeing frames and biases the statistics good.");
            ImGui::Unindent();
        }
        DragD("Discard burst above rejected fraction", &r.maxRejectedFraction,
            0.01, 0.05, 1.0, "%.2f");
    }

    void SettingsDialog::drawSite(DimmConfig& cfg) {
        SiteConfig& s = cfg.site;
        InputStr("Site name", s.name);
        DragD("Latitude (deg)", &s.latitudeDeg, 0.001, -90.0, 90.0, "%.5f");
        DragD("Longitude (deg)", &s.longitudeDeg, 0.001, -180.0, 180.0, "%.5f");
        DragD("Elevation (m)", &s.elevationM, 1.0, -500.0, 6000.0, "%.0f");

        ImGui::SeparatorText("Pointing");
        ImGui::Checkbox("Read altitude from mount (Alpaca)", &s.useMountAltitude);
        ImGui::BeginDisabled(s.useMountAltitude);
        DragD("Target altitude (deg)", &s.manualAltitudeDeg, 0.5, 1.0, 90.0, "%.1f");
        ImGui::EndDisabled();
        ImGui::Text("Airmass: %.3f", airmass(s.manualAltitudeDeg));

        ImGui::SeparatorText("Science instrument");
        ImGui::Checkbox("Paired with a science target", &s.haveScienceTarget);
        if (s.haveScienceTarget) {
            DragD("Target altitude (deg)", &s.scienceAltitudeDeg, 0.5, 1.0, 90.0, "%.1f");
            ImGui::Text("Airmass: %.3f", airmass(s.scienceAltitudeDeg));
            const double ratio =
                std::pow(std::sin(s.manualAltitudeDeg * 3.14159265358979 / 180.0) /
                    std::sin(s.scienceAltitudeDeg * 3.14159265358979 / 180.0), 0.6);
            ImGui::Text("Seeing at the target is %.2fx the DIMM's own line of sight", ratio);
            if (std::fabs(s.manualAltitudeDeg - s.scienceAltitudeDeg) > 10.0) {
                ImGui::TextColored(kWarn,
                    "The two pointings differ by %.0f degrees in altitude.",
                    std::fabs(s.manualAltitudeDeg - s.scienceAltitudeDeg));
                ImGui::TextWrapped(
                    "Airmass dominates directional variation in seeing, so matching "
                    "the DIMM to the science target in ALTITUDE keeps the projection "
                    "small and its errors smaller. A large mismatch is workable but "
                    "leans harder on the (cos z)^(3/5) law.");
            }
        }
        ImGui::TextWrapped(
            "Airmass dominates directional variation in seeing: r0 goes as "
            "(cos z)^(3/5), so a target at 60 deg zenith distance sees roughly 50%% "
            "worse seeing than zenith. Match the science target in ALTITUDE, not "
            "azimuth -- and correct to zenith before publishing or the number cannot "
            "be compared with anyone else's.");
    }

    void SettingsDialog::drawReporting(DimmConfig& cfg) {
        ReportingConfig& r = cfg.reporting;

        ImGui::Text("Wavelength: %.0f nm (DIMM convention)", r.wavelengthNm);
        ImGui::TextWrapped(
            "Not a knob. Because r0 goes as lambda^(6/5), the wavelength terms "
            "cancel and differential image motion is achromatic to first order -- so "
            "500 nm is used unconditionally regardless of your actual passband.");

        ImGui::SeparatorText("Response coefficients");
        int cm = int(r.coefficients);
        if (ImGui::Combo("Model", &cm,
            "Sarazin & Roddier (0.179)\0"
            "Yu/Sasiela\0"
            "Sarazin & Roddier corrected (0.182)\0"
            "Manual K\0"))
            r.coefficients = CoefficientModel(cm);

        if (r.coefficients == CoefficientModel::Manual) {
            DragD("K longitudinal", &r.manualKLong, 0.001, 0.01, 1.0, "%.4f");
            DragD("K transverse", &r.manualKTran, 0.001, 0.01, 1.0, "%.4f");
            ImGui::TextWrapped(
                "Tokovinin's coefficients are more accurate, especially at small "
                "d/D, and are worth entering from the paper. Reference check: for "
                "b = 2.5 the G-tilt values are K_l = 0.1956, K_t = 0.1270.");
        }

        const ResponseCoefficients k = responseCoefficients(cfg.optics, r);
        if (k.valid) {
            ImGui::Text("K_l = %.4f   K_t = %.4f   (at d/D = %.2f)",
                k.kLong, k.kTran, cfg.optics.b());
        }
        if (!k.warning.empty())
            ImGui::TextColored(k.valid ? kWarn : kBad, "%s", k.warning.c_str());

        ImGui::SeparatorText("Output");
        ImGui::Checkbox("Publish zenith-corrected seeing", &r.zenithCorrect);
        ImGui::TextWrapped(
            "DIMM convention reports zenith-corrected seeing at 500 nm. What an "
            "imaging app calls star FWHM is at its own airmass and wavelength and "
            "includes optics, focus and guiding -- routinely 1.5-2x different. "
            "Publishing one where people expect the other makes the instrument look "
            "broken.");
        int tz = r.displayLocalTime ? 1 : 0;
        if (ImGui::Combo("Plot time axis", &tz, "UTC\0Local time\0"))
            r.displayLocalTime = (tz == 1);
        ImGui::Checkbox("24-hour clock", &r.use24HourClock);
        ImGui::TextWrapped(
            "Display only. Exported CSV, logs and the Alpaca payload remain in UTC "
            "regardless -- a dataset carrying local times becomes ambiguous as soon "
            "as it leaves this machine, and doubly so across a DST boundary.");

        DragD("History window (s)", &r.historyWindowS, 30.0, 60.0, 43200.0, "%.0f");
        ImGui::SetItemTooltip("How much of the seeing history the plot retains. "
            "One point per completed burst, so at a few seconds "
            "per burst 1800 s is roughly 500 points.");

        ImGui::Checkbox("Log raw centroid series", &r.logRawCentroids);
        ImGui::SetItemTooltip("Keeps re-derivation possible after the constants are "
            "revised. Cheap, and the alternative is recapturing a "
            "season of data.");
        InputStr("SQLite path", r.sqlitePath);
    }

} // namespace mei::ui