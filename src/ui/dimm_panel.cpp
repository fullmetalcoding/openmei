#include "ui/dimm_panel.h"
#include "dimm/seeing.h"

#include "imgui.h"

#include <cmath>

namespace mei::ui {

namespace {
const ImVec4 kGood { 0.45f, 0.80f, 0.50f, 1.0f };
const ImVec4 kWarn { 0.90f, 0.70f, 0.25f, 1.0f };
const ImVec4 kBad  { 0.90f, 0.45f, 0.35f, 1.0f };
const ImVec4 kDim  { 0.60f, 0.60f, 0.65f, 1.0f };

void spotRow(const char* name, const SpotMeasurement& s, double scale) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
    if (!s.valid) {
        ImGui::TableNextColumn(); ImGui::TextColored(kBad, "lost");
        for (int i = 0; i < 5; ++i) { ImGui::TableNextColumn(); ImGui::TextUnformatted("-"); }
        return;
    }
    ImGui::TableNextColumn(); ImGui::Text("%.3f", s.x);
    ImGui::TableNextColumn(); ImGui::Text("%.3f", s.y);
    ImGui::TableNextColumn(); ImGui::Text("%.0f", s.flux);
    ImGui::TableNextColumn();
    if (scale > 0.0) ImGui::Text("%.2f px / %.2f\"", s.fwhmPx, s.fwhmPx * scale);
    else             ImGui::Text("%.2f px", s.fwhmPx);
    ImGui::TableNextColumn();
    if (s.saturated) ImGui::TextColored(kBad, "%.0f SAT", s.snr);
    else             ImGui::Text("%.0f", s.snr);
}
} // namespace

void DimmPanel(bool* open, DimmProcessor& proc, const DimmConfig& cfg,
               bool cameraStreaming, bool isSynthetic,
               const OverlayOptions& overlay) {
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("DIMM", open)) { ImGui::End(); return; }

    const DimmStatus st = proc.status();
    const double scale = cfg.calibration.arcsecPerPixel;

    // --- control -------------------------------------------------------------
    std::string why;
    const bool ready = cfg.canMeasure(why);

    ImGui::BeginDisabled(!cameraStreaming || !ready || st.state != DimmState::Idle);
    if (ImGui::Button("Begin measurement", ImVec2(160, 0))) proc.begin();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(st.state == DimmState::Idle);
    if (ImGui::Button("Stop", ImVec2(80, 0))) proc.stopMeasuring();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reset burst", ImVec2(100, 0))) proc.resetBurst();

    if (!cameraStreaming) ImGui::TextColored(kWarn, "Camera is not streaming.");
    else if (!ready)      ImGui::TextColored(kBad, "%s", why.c_str());

    if (overlay.show) {
        ImGui::SameLine();
        ImGui::Checkbox("Overlay", overlay.show);
        if (*overlay.show) {
            ImGui::SameLine();
            if (overlay.windows) { ImGui::Checkbox("box", overlay.windows); ImGui::SameLine(); }
            if (overlay.axis)    { ImGui::Checkbox("axes", overlay.axis);   ImGui::SameLine(); }
            if (overlay.labels)  ImGui::Checkbox("labels", overlay.labels);
        }
    }

    ImGui::Separator();
    ImGui::Text("State: %s", toString(st.state));
    if (!st.message.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(kDim, "-- %s", st.message.c_str());
    }

    // --- per-frame -----------------------------------------------------------
    if (st.haveLast) {
        ImGui::SeparatorText("Current frame");
        if (ImGui::BeginTable("##spots", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Spot", ImGuiTableColumnFlags_WidthFixed, 42);
            ImGui::TableSetupColumn("x");
            ImGui::TableSetupColumn("y");
            ImGui::TableSetupColumn("flux");
            ImGui::TableSetupColumn("FWHM");
            ImGui::TableSetupColumn("SNR");
            ImGui::TableHeadersRow();
            spotRow("A", st.last.a, scale);
            spotRow("B", st.last.b, scale);
            ImGui::EndTable();
        }

        ImGui::Text("Separation: %.3f px", st.last.sepPx);
        if (scale > 0.0) { ImGui::SameLine(); ImGui::Text("(%.2f\")", st.last.sepPx * scale); }
        if (st.expectedSepPx > 0.0) {
            ImGui::SameLine();
            const double dev = 100.0 * (st.last.sepPx - st.expectedSepPx) / st.expectedSepPx;
            ImGui::TextColored(std::fabs(dev) > cfg.optics.separationTolerancePct ? kWarn : kDim,
                               "  [%.1f%% from predicted]", dev);
        }
        ImGui::Text("Differential: long %+.4f px, tran %+.4f px",
                    st.last.diffLongPx, st.last.diffTranPx);
        if (!st.last.accepted && !st.last.rejectReason.empty())
            ImGui::TextColored(kWarn, "rejected: %s", st.last.rejectReason.c_str());
    }

    // --- ground truth --------------------------------------------------------
    if (isSynthetic && st.haveTruth) {
        ImGui::SeparatorText("Ground truth residual");
        ImGui::TextWrapped(
            "Measured centroid minus the position the generator actually used. "
            "This isolates centroider error from everything downstream.");
        const double worst = std::max(st.truthResidualAPx, st.truthResidualBPx);
        ImGui::TextColored(worst < 0.02 ? kGood : (worst < 0.10 ? kWarn : kBad),
                           "|A| = %.4f px    |B| = %.4f px", st.truthResidualAPx,
                           st.truthResidualBPx);
        ImGui::TextColored(kDim, "true differential: long %+.4f px, tran %+.4f px",
                           st.truthDiffLongPx, st.truthDiffTranPx);
    }

    // --- burst ---------------------------------------------------------------
    ImGui::SeparatorText("Burst");
    ImGui::Text("Accepted %d / %d   (target %d)", st.nAccepted,
                st.nAccepted + st.nRejected, cfg.burst.framesPerBurst);
    ImGui::Text("Mean separation: %.3f px", st.meanSepPx);
    ImGui::Text("Flux ratio B/A: %.3f", st.fluxRatio);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "(expect ~%.2f from wedge transmission)",
                       cfg.acquisition.expectedFluxRatio);
    ImGui::Text("Axis: %.2f deg  (%s)", st.axisAngleDeg,
                toString(cfg.optics.wedgeOrientation));

    const double sL = std::sqrt(std::max(0.0, st.varLongPx2));
    const double sT = std::sqrt(std::max(0.0, st.varTranPx2));
    ImGui::Text("sigma_long = %.4f px", sL);
    if (scale > 0.0) { ImGui::SameLine(); ImGui::Text("(%.4f\")", sL * scale); }
    ImGui::Text("sigma_tran = %.4f px", sT);
    if (scale > 0.0) { ImGui::SameLine(); ImGui::Text("(%.4f\")", sT * scale); }

    // --- result --------------------------------------------------------------
    if (st.haveResult) {
        ImGui::SeparatorText("Seeing");
        const SeeingResult& r = st.result;
        if (!r.valid) {
            ImGui::TextColored(kBad, "%s", r.reason.c_str());
        } else {
            ImGui::Text("FWHM (zenith, 500nm): ");
            ImGui::SameLine();
            ImGui::TextColored(kGood, "%.3f +/- %.3f arcsec",
                               r.fwhmZenithArcsec, r.sigmaArcsec);
            ImGui::Text("line of sight: %.3f\"   airmass %.2f",
                        r.fwhmArcsec, r.airmassUsed);
            ImGui::Text("r0 = %.1f mm (zenith)", r.r0Zenith * 1000.0);
            ImGui::Text("per axis: long %.3f\"  tran %.3f\"",
                        r.fwhmLongArcsec, r.fwhmTranArcsec);
            if (r.axisMismatchSuspected) {
                ImGui::TextColored(kWarn, "axes disagree by %.0f%%",
                                   100.0 * std::fabs(r.axisAgreementRatio - 1.0));
                ImGui::SetItemTooltip(
                    "Turbulence is isotropic, so both axes should agree once "
                    "their different coefficients are applied. A consistent "
                    "discrepancy is the fingerprint of a swapped longitudinal / "
                    "transverse axis -- check the wedge orientation setting.");
            }
        }
    }

    ImGui::End();
}

} // namespace mei::ui
