#include "ui/history_panel.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace mei::ui {

    namespace {

        const ImVec4 kGood{ 0.45f, 0.80f, 0.50f, 1.0f };
        const ImVec4 kDim{ 0.60f, 0.60f, 0.65f, 1.0f };
        const ImVec4 kWarn{ 0.90f, 0.70f, 0.25f, 1.0f };

        std::string defaultCsvName() {
            const std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char b[64];
            std::strftime(b, sizeof(b), "openmei-seeing-%Y%m%d-%H%M%S.csv", &tm);
            return b;
        }

    } // namespace

    void SeeingHistoryPanel(bool* open, SeeingHistory& hist, const DimmConfig& cfg,
        HistoryView& view, bool measuring) {
        if (!*open) return;

        ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Seeing history", open)) { ImGui::End(); return; }

        hist.setWindowSeconds(cfg.reporting.historyWindowS);

        // ImPlot formats time axes from global style, so this has to be set before
        // BeginPlot rather than passed to it.
        ImPlot::GetStyle().UseLocalTime = cfg.reporting.displayLocalTime;
        ImPlot::GetStyle().Use24HourClock = cfg.reporting.use24HourClock;

        // --- controls ------------------------------------------------------------
        ImGui::SetNextItemWidth(130);
        ImGui::SliderFloat("Smoothing (min)", &view.smoothingMinutes, 0.0f, 30.0f, "%.1f");
        ImGui::SetItemTooltip("Trailing average drawn over the burst points. Zero "
            "hides it. Purely presentational -- it does not "
            "affect what is published or logged.");
        ImGui::SameLine();
        ImGui::Checkbox("+/-1 sigma", &view.showBand);
        ImGui::SameLine();
        ImGui::Checkbox("r0", &view.showR0);
        ImGui::SameLine();
        ImGui::Checkbox("line of sight", &view.showLos);
        ImGui::SetItemTooltip("The raw measurement at the DIMM's own airmass. The "
            "zenith trace is the comparable one.");

        ImGui::SameLine();
        ImGui::Checkbox("Auto-fit", &view.autoFitY);
        ImGui::SetItemTooltip("Rescale the vertical axis to the data every frame. "
            "Turn off to zoom and keep it -- but then a rise from "
            "1\" to 5\" will run off the top until you refit.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Legend", &view.legendCorner,
            "Top left\0Top right\0Bottom left\0Bottom right\0");
        ImGui::SameLine();
        ImGui::Checkbox("outside", &view.legendOutside);
        ImGui::SameLine();
        ImGui::Checkbox("hide", &view.legendHidden);

        ImGui::SameLine();
        if (ImGui::SmallButton("Export CSV")) {
            std::string err;
            const std::string path = defaultCsvName();
            if (hist.writeCsv(path, err))
                ImGui::OpenPopup("##csvok");
            else
                ImGui::OpenPopup("##csvfail");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) hist.clear();

        if (ImGui::BeginPopup("##csvok")) {
            ImGui::TextUnformatted("Written to the working directory.");
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("##csvfail")) {
            ImGui::TextColored(kWarn, "Export failed.");
            ImGui::EndPopup();
        }

        // --- statistics ----------------------------------------------------------
        const HistoryStats st = hist.stats();
        if (st.count > 0) {
            ImGui::Separator();
            if (ImGui::BeginTable("##stats", 6,
                ImGuiTableFlags_SizingStretchProp)) {
                auto cell = [](const char* label, const char* fmt, double v,
                    const ImVec4& col) {
                        ImGui::TableNextColumn();
                        ImGui::TextColored(kDim, "%s", label);
                        char b[48];
                        std::snprintf(b, sizeof(b), fmt, v);
                        ImGui::TextColored(col, "%s", b);
                    };
                ImGui::TableNextRow();
                cell("current", "%.2f\"", st.current, kGood);
                // Median rather than mean: seeing distributions are strongly
                // right-skewed, so a mean sits above the value you spend most of
                // the night at.
                cell("median", "%.2f\"", st.median, kGood);
                cell("best", "%.2f\"", st.best, kDim);
                cell("worst", "%.2f\"", st.worst, kDim);
                cell("10-90%%", "%.2f\"", st.p90 - st.p10, kDim);
                ImGui::TableNextColumn();
                ImGui::TextColored(kDim, "bursts");
                ImGui::Text("%d over %.0f min", st.count, st.spanSeconds / 60.0);
                ImGui::EndTable();
            }
        }

        ImGui::Separator();

        // --- plot ----------------------------------------------------------------
        if (hist.empty()) {
            ImGui::TextColored(kDim, measuring
                ? "Waiting for the first burst to complete..."
                : "No measurements yet. Start a measurement to populate the history.");
            ImGui::End();
            return;
        }

        const auto& s = hist.samples();
        const size_t n = s.size();

        std::vector<double> t(n), y(n), lo(n), hi(n), los(n), r0(n);
        for (size_t i = 0; i < n; ++i) {
            t[i] = s[i].unixSeconds;
            y[i] = s[i].fwhmZenith;
            lo[i] = s[i].fwhmZenith - s[i].sigma;
            hi[i] = s[i].fwhmZenith + s[i].sigma;
            los[i] = s[i].fwhmLos;
            r0[i] = s[i].r0Zenith * 100.0;   // cm reads better than metres
        }

        const double now = double(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        const double xMin = now - cfg.reporting.historyWindowS;

        const ImPlotFlags plotFlags = view.legendHidden ? ImPlotFlags_NoLegend
            : ImPlotFlags_None;
        if (ImPlot::BeginPlot("##seeinghistory", ImVec2(-1, -1), plotFlags)) {
            // Clock labels rather than elapsed time: an observer correlating this
            // against a capture log wants wall time.
            //
            // ImPlot 0.14 replaced ImPlotAxisFlags_Time with the scale concept, so
            // a time axis is now a scale rather than a flag. X values remain Unix
            // seconds as double.
            // Label the axis with the zone in force, so a screenshot is not
            // ambiguous about what its timestamps mean.
            ImPlot::SetupAxis(ImAxis_X1,
                cfg.reporting.displayLocalTime ? "Local time" : "UTC");
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            // AutoFit refits every frame, so a rise the plot has not seen before
            // comes into view without intervention. It also disables manual zoom,
            // which is the whole reason it is a toggle rather than the only mode.
            const ImPlotAxisFlags yFlags =
                view.autoFitY ? ImPlotAxisFlags_AutoFit : ImPlotAxisFlags_None;

            ImPlot::SetupAxis(ImAxis_Y1, "Seeing (arcsec)", yFlags);
            ImPlot::SetupAxisLimits(ImAxis_X1, xMin, now, ImGuiCond_Always);
            if (!view.autoFitY) {
                // Only meaningful when the user controls the range: seeing has no
                // meaning below zero. Cosmetic -- delete if your ImPlot predates
                // 0.16, where the call was added.
                ImPlot::SetupAxisLimitsConstraints(ImAxis_Y1, 0.0, INFINITY);
            }
            if (view.showR0) {
                ImPlot::SetupAxis(ImAxis_Y2, "r0 (cm)",
                    ImPlotAxisFlags_AuxDefault | yFlags);
            }

            if (!view.legendHidden) {
                // Data arrives at the right edge, so the default north-east corner
                // sits directly on top of the values being watched.
                static const ImPlotLocation kCorners[4] = {
                    ImPlotLocation_NorthWest, ImPlotLocation_NorthEast,
                    ImPlotLocation_SouthWest, ImPlotLocation_SouthEast
                };
                const int idx = std::clamp(view.legendCorner, 0, 3);
                ImPlot::SetupLegend(kCorners[idx],
                    view.legendOutside ? ImPlotLegendFlags_Outside
                    : ImPlotLegendFlags_None);
            }

            if (view.showBand && n > 1) {
                ImPlot::SetNextFillStyle(IMPLOT_AUTO_COL, 0.20f);
                ImPlot::PlotShaded("+/-1 sigma", t.data(), lo.data(), hi.data(), int(n));
            }

            ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
            ImPlot::PlotLine("Seeing (zenith)", t.data(), y.data(), int(n));

            if (view.showLos) {
                ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 1.0f);
                ImPlot::PlotLine("Line of sight", t.data(), los.data(), int(n));
            }

            if (view.smoothingMinutes > 0.01f && n > 2) {
                const std::vector<double> ma = hist.movingAverage(view.smoothingMinutes * 60.0);
                ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 2.5f);
                ImPlot::PlotLine("Trend", t.data(), ma.data(), int(n));
            }

            if (view.showR0) {
                ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
                ImPlot::PlotLine("r0", t.data(), r0.data(), int(n));
            }

            ImPlot::EndPlot();
        }

        ImGui::End();
    }

} // namespace mei::ui