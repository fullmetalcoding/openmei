// main.cpp -- OpenMei application shell.
//
// SDL3 window + OpenGL 3.3 core + GLEW + Dear ImGui, a menu bar, and a
// PHD2-style status bar with condition lamps. No measurement code yet; this
// is the frame everything else hangs off.

#include <SDL3/SDL.h>
#include <GL/glew.h>          // must be included before any GL headers

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar -- see note at StatusBar()
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"

#include "camera/registry.h"
#include "camera/stream.h"
#include "camera/backends/synthetic_backend.h"
#include "dimm/config.h"
#include "dimm/processor.h"
#include "dimm/seeing.h"
#include "ui/camera_dialog.h"
#include "app/settings_store.h"
#ifdef OPENMEI_ALPACA
#include "net/alpaca_server.h"
#endif
#include "ui/dimm_panel.h"
#include "ui/settings_dialog.h"
#include "ui/viewport.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>

// =============================================================================
//  Application state
// =============================================================================

namespace mei {

enum class Severity { Info, Warning, Error };

struct LogEntry {
    Severity    severity;
    std::string text;
    Uint64      when;      // SDL_GetTicks()
};

struct MeiApp {
    bool running = true;

    // Condition lamps. Both start false -- an uncalibrated DIMM must not be
    // able to publish a number, and the UI should say so from the first frame.
    bool calibrated  = false;   // plate scale + geometry validated
    bool haveDarks   = false;   // dark / hot-pixel library loaded

    // Most recent status line, plus a short history for a future log window.
    LogEntry             status{ Severity::Info, "Ready. No calibration loaded.", 0 };
    std::deque<LogEntry> history;

    bool showDemo = false;

    // Equipment. The registry owns every backend and the live connection.
    CameraRegistry     cameras;
    ui::CameraDialog   cameraDialog;
    StreamController   stream;
    ui::Viewport       viewport;

    // Requested stream settings. Applied through ICamera::configure(), which
    // clamps to hardware granularity and hands back what was actually set.
    StreamConfig want{};
    bool showSynthPanel = false;

    // Measurement configuration and its editor.
    AppSettings        prefs;
    DimmConfig&        dimm = prefs.dimm;   // alias: everything already uses this
    ui::SettingsDialog settings;
    bool               prefsDirty = false;
    uint64_t           lastPrefsSaveMs = 0;
    DimmProcessor      processor;
    bool               showDimmPanel = true;

#ifdef OPENMEI_ALPACA
    AlpacaServer  alpaca;
    AlpacaConfig& alpacaCfg = prefs.alpaca;
    bool         showAlpacaPanel = false;
    // Only republish when a genuinely new burst completes, so clients see a
    // stable value between measurements rather than a field that churns.
    uint64_t     lastPublishedBurst = 0;
#endif

    // Spot overlay, PHD2-style.
    bool overlayShow      = true;
    bool overlayWindows   = true;   // centroid window boxes
    bool overlayAxis      = false;  // longitudinal / transverse axes
    bool overlayLabels    = true;

    bool isSynthetic() const {
        return cameras.connectedDesc().backendId == "synthetic";
    }

    bool cameraConnected() const { return cameras.connected(); }
    bool cameraStreaming() const { return stream.running(); }

    void setStatus(Severity sev, std::string text) {
        status = { sev, std::move(text), SDL_GetTicks() };
        history.push_back(status);
        if (history.size() > 512) history.pop_front();
    }
};

} // namespace mei

// =============================================================================
//  UI helpers
// =============================================================================

namespace mei::ui {


// PHD2-style condition lamp: a small filled box with a short label, green when
// the condition is satisfied and amber when it isn't. Hover for detail.
static void Lamp(const char* label, bool ok, const char* tipOk, const char* tipBad) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const ImVec2 pad      = ImVec2(style.FramePadding.x, style.FramePadding.y * 0.5f);
    const ImVec2 box      = ImVec2(textSize.x + pad.x * 2.0f, textSize.y + pad.y * 2.0f);

    const ImU32 bg = ok ? IM_COL32( 40, 140,  60, 255)    // green
                        : IM_COL32(170, 120,  20, 255);   // amber
    const ImU32 fg = IM_COL32(235, 235, 235, 255);

    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + box.x, p.y + box.y), bg, 3.0f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), fg, label);

    // Dummy reserves the layout rect so IsItemHovered() and SameLine() work.
    ImGui::Dummy(box);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", ok ? tipOk : tipBad);
    }
}

static ImU32 SeverityColor(Severity s) {
    switch (s) {
        case Severity::Error:   return IM_COL32(235,  90,  80, 255);
        case Severity::Warning: return IM_COL32(230, 180,  60, 255);
        default:                return IM_COL32(210, 210, 210, 255);
    }
}

// Configure a sensible default ROI and start the grab thread. A DIMM wants a
// small window at high cadence, not a full frame.
static void startStream(MeiApp& app) {
    ICamera* cam = app.cameras.camera();
    if (!cam) return;

    const Caps& c = cam->caps();
    StreamConfig w = app.want;
    if (w.width <= 0) {
        // Prefer 16-bit. Quantisation is not what limits centroid precision --
        // photon noise is, by an order of magnitude -- but 8 bits leaves no
        // saturation headroom, and a clipped core biases the measurement toward
        // BETTER seeing by flattening the peak and suppressing apparent motion.
        // The bandwidth cost is only paid during full-frame acquisition; the
        // measurement ROI is small enough that 16-bit is free there.
        if (c.isColor) {
            // A CFA modulates pixel sensitivity in a 2x2 pattern, which biases
            // the centroid by an amount that varies with sub-pixel position.
            // Hardware mono binning sums each 2x2 block so the modulation
            // cancels exactly. Half the resolution, but an unbiased centroid --
            // and interpolating instead would only correlate neighbours.
            w.bin     = (c.hasMonoBin && std::find(c.bins.begin(), c.bins.end(), 2)
                                         != c.bins.end()) ? 2 : 1;
            w.monoBin = (w.bin >= 2);
            w.format = PixelFormat::Bayer8;
            for (auto f : c.formats) if (f == PixelFormat::Bayer16) w.format = f;
        } else {
            w.bin    = 1;
            w.format = PixelFormat::Mono8;
            for (auto f : c.formats) if (f == PixelFormat::Mono16) w.format = f;
        }
        // ZWO's high-speed mode buys throughput by reading out through a
        // lower-bit ADC, so a RAW16 frame would carry ~10 real bits in a 16-bit
        // container -- and nothing in the API reports that. Never combine them.
        w.highSpeed = (bytesPerPixel(w.format) == 1);
        // ROI is in binned coordinates.
        w.width  = std::min(800, c.maxWidth  / w.bin);
        w.height = std::min(600, c.maxHeight / w.bin);
        w.x = (c.maxWidth  / w.bin - w.width)  / 2;
        w.y = (c.maxHeight / w.bin - w.height) / 2;

        w.exposureUs = 5000;
        w.gain = c.gain.supported ? c.gain.def : 200.0;
        // 100 = let the camera use its full rated USB throughput. Lower this
        // if you see torn frames or timeouts on a weak host controller; it is
        // the first knob to turn when chasing drops, not the last.
        w.usbBandwidth = 100;
    }

    std::string err;
    app.want = cam->configure(w, err);
    if (!err.empty()) { app.setStatus(Severity::Error, "Configure failed: " + err); return; }

    // Analysis sees every frame; the display tap deliberately does not.
    app.processor.setConfig(app.dimm);
    // The processor drives the t/2t alternation but never touches the camera.
    app.processor.setExposureSetter([&app](int64_t us) {
        ICamera* c = app.cameras.camera();
        if (!c) return false;
        std::string e;
        const bool ok = c->setExposureUs(us, e);
        if (ok) app.want.exposureUs = c->config().exposureUs;
        return ok;
    });
    const bool synth = app.isSynthetic();
    app.stream.setAnalysisSink([&app, synth](const Frame& f) {
        app.processor.onFrame(f, synth);
    });

    if (!app.stream.start(cam, err)) {
        app.setStatus(Severity::Error, "Start failed: " + err);
        return;
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "Streaming %dx%d @ (%d,%d) bin%d, %s%s, %.1f ms, gain %.0f",
                  app.want.width, app.want.height, app.want.x, app.want.y,
                  app.want.bin, toString(app.want.format),
                  app.want.monoBin ? " (mono bin)" : "",
                  app.want.exposureUs / 1000.0, app.want.gain);
    app.setStatus(Severity::Info, buf);
    app.prefsDirty = true;
}

static void SyntheticPanel(MeiApp& app) {
    if (!app.showSynthPanel) return;
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Synthetic source", &app.showSynthPanel)) { ImGui::End(); return; }

    SyntheticParams& p = syntheticParams();
    ImGui::TextWrapped("Ground truth for validating the measurement chain. "
                       "Whatever seeing is set here is what the analysis must "
                       "recover.");
    ImGui::Separator();

    float seeing = float(p.seeingArcsec);
    if (ImGui::SliderFloat("Seeing (arcsec)", &seeing, 0.4f, 6.0f, "%.2f"))
        p.seeingArcsec = seeing;

    float sep = float(p.separationPx);
    if (ImGui::SliderFloat("Spot separation (px)", &sep, 20.0f, 400.0f, "%.0f"))
        p.separationPx = sep;

    float fwhm = float(p.spotFwhmPx);
    if (ImGui::SliderFloat("Spot FWHM (px)", &fwhm, 1.5f, 20.0f, "%.2f"))
        p.spotFwhmPx = fwhm;

    float scale = float(p.plateScale);
    if (ImGui::SliderFloat("Plate scale (\"/px)", &scale, 0.10f, 3.0f, "%.3f"))
        p.plateScale = scale;

    float angle = float(p.axisAngleDeg);
    if (ImGui::SliderFloat("Baseline angle (deg)", &angle, 0.0f, 180.0f, "%.1f"))
        p.axisAngleDeg = angle;

    ImGui::Checkbox("Wedge along baseline", &p.wedgeAlongBaseline);
    ImGui::SetItemTooltip("Off simulates a wedge clocked across the baseline, "
                          "which swaps the longitudinal and transverse axes.");

    float flux = float(p.starFluxE);
    if (ImGui::SliderFloat("Star flux (e- @ 10ms)", &flux, 1000.0f, 500000.0f,
                           "%.0f", ImGuiSliderFlags_Logarithmic))
        p.starFluxE = flux;

    float tau = float(p.coherenceTimeMs);
    if (ImGui::SliderFloat("Coherence time (ms)", &tau, 0.5f, 30.0f, "%.1f"))
        p.coherenceTimeMs = tau;
    ImGui::SetItemTooltip("How fast the wavefront tilt decorrelates. Together "
                          "with exposure this sets the exposure-time bias.");

    float common = float(p.trackingRmsPx);
    if (ImGui::SliderFloat("Tracking wander (px)", &common, 0.0f, 20.0f, "%.1f"))
        p.trackingRmsPx = common;
    ImGui::SetItemTooltip("Common-mode motion. A correct analysis is insensitive "
                          "to this -- it cancels in the difference.");

    ImGui::SeparatorText("Wedge");
    float trans = float(p.wedgeTransmission);
    if (ImGui::SliderFloat("Transmission", &trans, 0.70f, 1.00f, "%.3f"))
        p.wedgeTransmission = trans;
    ImGui::SetItemTooltip("Only the deviated spot passes through glass. 0.92 is "
                          "uncoated (two ~4%% Fresnel surfaces), ~0.99 AR-coated.");

    float band = float(p.bandwidthNm);
    if (ImGui::SliderFloat("Passband (nm)", &band, 10.0f, 300.0f, "%.0f"))
        p.bandwidthNm = band;
    ImGui::SetItemTooltip("Sets the chromatic streak on the deviated spot. "
                          "Narrowing the band is what makes the wedge usable.");

    // The streak is deviation/Abbe, so it scales with separation -- which is
    // why a fixed 30' wedge at a long focal length is unusable while the same
    // glass in a Risley pair set to a small net deviation is fine.
    {
        const double bandFrac = std::clamp(p.bandwidthNm / 300.0, 0.0, 1.0);
        const double streak   = (p.separationPx / std::max(1.0, p.abbeNumber)) * bandFrac;
        const double sig      = p.spotFwhmPx / 2.35482;
        const double sMaj     = std::sqrt(sig * sig + streak * streak / 12.0);
        const double ellip    = 1.0 - sig / sMaj;
        ImGui::TextDisabled("Streak %.2f px -> FWHM %.2f x %.2f px, ellipticity %.3f",
                            streak, sMaj * 2.35482, p.spotFwhmPx, ellip);
        if (ellip > 0.15) {
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f),
                               "Spot B is badly smeared -- narrow the passband "
                               "or reduce the deviation.");
        }
    }

    ImGui::SeparatorText("Detector");
    float fw = float(p.fullWellE);
    if (ImGui::SliderFloat("Full well (e-)", &fw, 5000.0f, 100000.0f, "%.0f"))
        p.fullWellE = fw;
    float rn = float(p.readNoiseE);
    if (ImGui::SliderFloat("Read noise (e-)", &rn, 0.5f, 10.0f, "%.2f"))
        p.readNoiseE = rn;

    ImGui::SeparatorText("Scintillation");
    float sc = float(p.scintillationIndex);
    if (ImGui::SliderFloat("Index", &sc, 0.0f, 0.30f, "%.3f"))
        p.scintillationIndex = sc;
    float scc = float(p.scintillationCorr);
    if (ImGui::SliderFloat("Aperture correlation", &scc, 0.0f, 1.0f, "%.2f"))
        p.scintillationCorr = scc;

    ImGui::Separator();
    ImGui::Checkbox("Inject hot pixels", &p.injectHotPixels);
    ImGui::Checkbox("Inject field star", &p.injectFieldStar);

    ImGui::Separator();
    double sigL = 0, sigT = 0, r0 = 0;
    syntheticExpectedSigma(p, sigL, sigT, r0);
    // Peak level is what decides whether a frame is usable, and it depends on
    // flux, exposure, gain and bit depth together. Showing it makes saturation
    // predictable instead of something you discover from the lamp.
    if (app.cameras.connected() && app.stream.running() &&
        app.cameras.connectedDesc().backendId == "synthetic") {
        const StreamConfig& sc = app.cameras.camera()->config();
        const int    bits  = (bytesPerPixel(sc.format) == 2) ? p.significantBits : 8;
        const double eadu  = syntheticEPerADU(p, bits, sc.gain);
        const double fs    = double(sc.exposureUs) / 10000.0;
        const double sig   = p.spotFwhmPx * (1.0 / 2.35482);
        const double peakE = (p.starFluxE * fs) / (2.0 * 3.14159265 * sig * sig);
        const double peak  = std::min(peakE, p.fullWellE) / eadu;
        const double full  = double((1 << bits) - 1);
        ImGui::TextDisabled("Levels: %.2f e-/ADU at %d bits, gain %.0f", eadu, bits, sc.gain);
        ImGui::TextColored(peak >= full ? ImVec4(0.90f, 0.45f, 0.35f, 1.0f)
                                        : ImVec4(0.55f, 0.55f, 0.60f, 1.0f),
                           "Predicted peak: %.0f / %.0f ADU (%.0f%%)%s",
                           peak, full, 100.0 * peak / full,
                           peak >= full ? "  CLIPPED" : "");
        ImGui::Separator();
    }

    ImGui::TextDisabled("Ground truth (zero exposure):");
    ImGui::BulletText("r0 = %.1f mm", r0 * 1000.0);
    ImGui::BulletText("sigma_long = %.3f px  (%.3f\")", sigL, sigL * p.plateScale);
    ImGui::BulletText("sigma_tran = %.3f px  (%.3f\")", sigT, sigT * p.plateScale);

    // The gap between these two is the systematic the t/2t correction exists to
    // remove. Showing it makes the correction testable rather than theoretical.
    if (app.cameras.connected() && app.stream.running()) {
        const double expMs = double(app.cameras.camera()->config().exposureUs) / 1000.0;
        const double ret   = syntheticExposureRetention(p, expMs);
        ImGui::Spacing();
        ImGui::TextDisabled("At %.2f ms exposure:", expMs);
        ImGui::BulletText("variance retained  ~%.0f%%", ret * 100.0);
        ImGui::BulletText("seeing would read  ~%.0f%% low",
                          (1.0 - std::pow(ret, 0.3)) * 100.0);
    }

    ImGui::End();
}

static void MenuBar(MeiApp& app) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Configuration...", "Ctrl+O")) {
            app.setStatus(Severity::Info, "Configuration load not implemented yet.");
        }
        if (ImGui::MenuItem("Save Configuration", "Ctrl+S")) {
            app.setStatus(Severity::Info, "Configuration save not implemented yet.");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) app.running = false;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Camera")) {
        if (ImGui::MenuItem("Connect...", "Ctrl+K", false, !app.cameraConnected())) {
            app.cameraDialog.open();
        }
        if (ImGui::MenuItem("Disconnect", nullptr, false, app.cameraConnected())) {
            const std::string name = app.cameras.connectedDesc().displayName();
            app.stream.stop();
            app.viewport.release();
            app.cameras.disconnect();
            app.setStatus(Severity::Info, "Disconnected from " + name + ".");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Start Stream", "Ctrl+R", false,
                            app.cameraConnected() && !app.cameraStreaming())) {
            startStream(app);
        }
        if (ImGui::MenuItem("Stop Stream", nullptr, false, app.cameraStreaming())) {
            app.processor.stopMeasuring();
            app.stream.stop();
            app.setStatus(Severity::Info, "Stream stopped.");
        }
        ImGui::Separator();
        ImGui::MenuItem("Synthetic source...", nullptr, &app.showSynthPanel);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Measure")) {
        ImGui::MenuItem("DIMM panel", nullptr, &app.showDimmPanel);
#ifdef OPENMEI_ALPACA
        ImGui::Separator();
        ImGui::MenuItem("Alpaca server...", nullptr, &app.showAlpacaPanel);
#endif
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Settings...", "Ctrl+,")) {
            app.settings.open();
        }
        ImGui::Separator();
        // Temporary developer affordances -- these will move to a Tools menu.
        ImGui::MenuItem("Show ImGui Demo", nullptr, &app.showDemo);
        ImGui::Separator();
        if (ImGui::MenuItem("Simulate: toggle CAL"))  app.calibrated = !app.calibrated;
        if (ImGui::MenuItem("Simulate: toggle DRK"))  app.haveDarks  = !app.haveDarks;
        if (ImGui::MenuItem("Simulate: warning")) {
            app.setStatus(Severity::Warning, "Spot separation drifted 6% from expected.");
        }
        if (ImGui::MenuItem("Simulate: error")) {
            app.setStatus(Severity::Error, "Lost second spot -- reacquiring.");
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// BeginViewportSideBar lives in imgui_internal.h rather than the public header,
// but it is the sanctioned way to do this: unlike a manually positioned window,
// it shrinks the viewport work area, so a future DockSpaceOverViewport() will
// stop above the status bar instead of sliding underneath it.
static void StatusBar(MeiApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float    h  = ImGui::GetFrameHeight();

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_MenuBar;

    if (ImGui::BeginViewportSideBar("##StatusBar", vp, ImGuiDir_Down, h, flags)) {
        if (ImGui::BeginMenuBar()) {
            // Zone 1: transient message, coloured by severity. This is a log
            // line -- it reflects the last event, not current state.
            ImGui::PushStyleColor(ImGuiCol_Text, SeverityColor(app.status.severity));
            ImGui::TextUnformatted(app.status.text.c_str());
            ImGui::PopStyleColor();

            // Zone 2: live state, rebuilt every frame. Anything the user can
            // change while streaming belongs here rather than in the message,
            // or the two disagree the moment a slider moves.
            std::string live;
            if (app.stream.running()) {
                const ICamera*    cam = app.cameras.camera();
                const StreamStats st  = app.stream.stats();
                char b[192];
                std::snprintf(b, sizeof(b),
                              "%dx%d bin%d  |  %s/%db  |  %.2f ms  |  gain %.0f"
                              "  |  %.1f fps  |  %.0f MB/s  |  %s%s",
                              cam->config().width, cam->config().height,
                              cam->config().bin,
                              toString(cam->config().format),
                              app.viewport.lastSignificantBits(),
                              cam->config().exposureUs / 1000.0,
                              cam->config().gain, st.fps, st.mbPerSec,
                              toString(cam->caps().hostUsb),
                              app.viewport.lastSaturated() ? "  |  SAT" : "");
                live = b;
            } else if (app.cameras.connected()) {
                live = "idle";
            }

            const float lampsWidth = 142.0f;
            const float liveWidth  = live.empty()
                ? 0.0f
                : ImGui::CalcTextSize(live.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2;
            const float avail = ImGui::GetContentRegionAvail().x;
            const float need  = lampsWidth + liveWidth;

            if (avail > need) {
                ImGui::SameLine(ImGui::GetCursorPosX() + avail - need);
            } else {
                ImGui::SameLine();
            }
            if (!live.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    app.viewport.lastSaturated() ? IM_COL32(230, 180, 60, 255)
                                                 : IM_COL32(160, 160, 165, 255));
                ImGui::TextUnformatted(live.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            Lamp("CAM", app.cameraConnected(),
                 "Camera connected and ready.",
                 "No camera connected. Camera > Connect...");
            ImGui::SameLine();
            {
                std::string why;
                const bool ready = app.dimm.canMeasure(why);
                static std::string tip;
                tip = "NOT CALIBRATED: " + why;
                Lamp("CAL", ready,
                     "Plate scale and mask geometry validated.", tip.c_str());
            }
            ImGui::SameLine();
            Lamp("DRK", app.haveDarks,
                 "Dark / hot-pixel library loaded.",
                 "No dark library. Hot pixels inside a centroid window bias "
                 "that centroid every frame.");

            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// Always-visible seeing readout. The point of a monitoring instrument is that
// the number is in front of you without opening anything, so this sits in the
// main window rather than only in the measurement panel.
static void SeeingReadout(MeiApp& app) {
    const DimmStatus st = app.processor.status();

    const ImU32 dim  = IM_COL32(150, 150, 155, 255);
    const ImU32 good = IM_COL32(120, 215, 130, 255);
    const ImU32 warn = IM_COL32(230, 180,  60, 255);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float  h  = ImGui::GetFrameHeight() * 1.6f;
    const float  w  = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h),
                      IM_COL32(28, 28, 34, 255), 4.0f);

    ImGui::Dummy(ImVec2(w, h));
    const float pad = 10.0f;
    float x = p0.x + pad;
    const float yTop = p0.y + 4.0f;
    const float yBot = p0.y + h - ImGui::GetTextLineHeight() - 4.0f;

    auto field = [&](const char* label, const std::string& value, ImU32 col,
                     float width) {
        dl->AddText(ImVec2(x, yTop), dim, label);
        dl->AddText(ImVec2(x, yBot), col, value.c_str());
        x += width;
    };

    char buf[96];
    const bool have = st.haveResult && st.result.valid;

    if (have) {
        // Zenith at 500 nm is the DIMM reporting convention -- it is the only
        // form comparable across nights, sites and instruments, because
        // r0 goes as (cos z)^(3/5) and a raw value describes where you pointed
        // rather than the atmosphere.
        std::snprintf(buf, sizeof(buf), "%.2f\" +/- %.2f",
                      st.result.fwhmZenithArcsec, st.result.sigmaArcsec);
        field("SEEING (zenith, 500nm)", buf, good, 200.0f);

        // What the instrument actually saw, along its own line of sight.
        std::snprintf(buf, sizeof(buf), "%.2f\" @ X%.2f",
                      st.result.fwhmArcsec, st.result.airmassUsed);
        field("DIMM LOS", buf, dim, 130.0f);

        // The DIMM points at its own star, so its line of sight is not the
        // science instrument's. This projects the zenith value onto the science
        // target's altitude, which is the number a paired dataset wants.
        if (st.result.fwhmAtScienceArcsec > 0.0) {
            std::snprintf(buf, sizeof(buf), "%.2f\" @ X%.2f",
                          st.result.fwhmAtScienceArcsec, st.result.scienceAirmass);
            field("AT TARGET", buf, good, 130.0f);
        }

        std::snprintf(buf, sizeof(buf), "%.1f cm", st.result.r0Zenith * 100.0);
        field("r0", buf, good, 85.0f);
    } else {
        field("SEEING (zenith, 500nm)", "--", dim, 200.0f);
        field("DIMM LOS", "--", dim, 130.0f);
        field("r0", "--", dim, 85.0f);
    }

    // Deliberately not labelled tau_0. What a DIMM measures directly is how
    // fast the IMAGE MOTION decorrelates; tau_0 concerns phase and is shorter,
    // because tilt is dominated by large spatial scales that persist longer
    // than the speckle pattern.
    if (st.haveMotionTau) {
        std::snprintf(buf, sizeof(buf), "%.1f ms", st.motionTauMs);
        field("MOTION DECORR", buf, good, 120.0f);
    } else if (st.frameIntervalMs > 0.0) {
        std::snprintf(buf, sizeof(buf), "< %.1f ms", st.frameIntervalMs);
        field("MOTION DECORR", buf, dim, 120.0f);
    } else {
        field("MOTION DECORR", "--", dim, 120.0f);
    }

    if (have && st.interleaving && st.exposureCorrection > 1.0) {
        std::snprintf(buf, sizeof(buf), "x%.2f", st.exposureCorrection);
        field("EXP CORR", buf, st.exposureCorrection > 1.3 ? warn : dim, 80.0f);
    }

    std::snprintf(buf, sizeof(buf), "%d", st.nAccepted);
    field("FRAMES", buf, dim, 70.0f);

    field("STATE", toString(st.state),
          st.state == DimmState::Measuring ? good : dim, 90.0f);

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "SEEING is zenith-corrected at 500 nm -- the DIMM convention, and\n"
            "the only form comparable across nights, sites and instruments.\n"
            "DIMM LOS is the raw measurement along the DIMM's own pointing.\n"
            "AT TARGET projects the zenith value onto the science instrument's\n"
            "altitude; the DIMM observes a different star, so its own line of\n"
            "sight is not the science target's.\n\n"
            "MOTION DECORR is the lag-1 decorrelation time of the differential\n"
            "image motion. It is NOT the atmospheric coherence time tau_0:\n"
            "tilt persists longer than phase. Use it to judge whether frames\n"
            "are statistically independent at the current cadence.");
    }
}

#ifdef OPENMEI_ALPACA
static void AlpacaPanel(MeiApp& app) {
    if (!app.showAlpacaPanel) return;
    ImGui::SetNextWindowSize(ImVec2(430, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Alpaca server", &app.showAlpacaPanel)) { ImGui::End(); return; }

    const AlpacaStats st = app.alpaca.stats();

    ImGui::TextWrapped(
        "Publishes seeing as ObservingConditions.StarFWHM. Clients that already "
        "speak observing conditions -- N.I.N.A., SGP, ACP, Voyager -- can consume "
        "it with no work on their side.");
    ImGui::Separator();

    ImGui::BeginDisabled(st.running);
    ImGui::SetNextItemWidth(110);
    ImGui::InputInt("Port", &app.alpacaCfg.port);
    ImGui::Checkbox("UDP discovery (port 32227)", &app.alpacaCfg.discovery);
    ImGui::SetItemTooltip("Without this, clients must be given the address by "
                          "hand, which is where most people give up.");
    ImGui::EndDisabled();

    if (!st.running) {
        if (ImGui::Button("Start", ImVec2(100, 0))) {
            std::string err;
            if (app.alpaca.start(app.alpacaCfg, err))
                app.setStatus(Severity::Info,
                              "Alpaca server listening on port " +
                              std::to_string(app.alpacaCfg.port) + ".");
            else
                app.setStatus(Severity::Error, "Alpaca start failed: " + err);
        }
    } else {
        if (ImGui::Button("Stop", ImVec2(100, 0))) {
            app.alpaca.stop();
            app.setStatus(Severity::Info, "Alpaca server stopped.");
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.50f, 1.0f),
                           "listening on :%d", st.port);
    }

    ImGui::Separator();
    ImGui::Text("Bursts published: %llu", (unsigned long long)st.publishedBursts);
    if (st.publishedBursts > 0) {
        ImGui::SameLine();
        ImGui::TextColored(st.lastPublishAgeS > 300.0
                               ? ImVec4(0.90f, 0.70f, 0.25f, 1.0f)
                               : ImVec4(0.60f, 0.60f, 0.65f, 1.0f),
                           "(last %.0f s ago)", st.lastPublishAgeS);
    }
    ImGui::Text("Requests served: %llu", (unsigned long long)st.requests);
    ImGui::Text("Discovery replies: %llu", (unsigned long long)st.discoveryReplies);
    ImGui::Text("Client Connected: %s", st.clientConnected ? "yes" : "no");
    if (!st.lastClient.empty()) ImGui::TextDisabled("last client: %s", st.lastClient.c_str());
    if (!st.lastError.empty())
        ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "%s", st.lastError.c_str());

    ImGui::Separator();
    ImGui::TextDisabled("Unique ID (persist this with your configuration):");
    ImGui::TextWrapped("%s", app.alpacaCfg.uniqueId.c_str());
    ImGui::SetItemTooltip("Clients key their saved device selection on this. "
                          "Regenerating it makes them lose the device.");

    ImGui::End();
}
#endif

// PHD2-style spot markers drawn over the live view. Deliberately drawn from
// the processor's own measurements rather than re-detected here, so what you
// see is exactly what is being measured -- if the box sits off the star, the
// centroider is wrong, and that should be visible.
static void SpotOverlay(MeiApp& app) {
    if (!app.overlayShow || !app.viewport.hasDrawn()) return;

    const DimmStatus st = app.processor.status();
    if (!st.haveLast) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float spp = app.viewport.screenPerFramePx();
    const float half = float(app.dimm.centroid.windowRadiusPx) * spp;

    const ImU32 colOk   = IM_COL32(235,  60,  60, 230);
    const ImU32 colSat  = IM_COL32(255, 170,  40, 240);
    const ImU32 colBad  = IM_COL32(140, 140, 145, 180);
    const ImU32 colAxis = IM_COL32( 90, 170, 235, 160);

    auto marker = [&](const SpotMeasurement& s, const char* label) {
        if (!s.valid) return;
        ImVec2 c;
        if (!app.viewport.mapFrameToScreen(s.x, s.y, c)) return;

        const ImU32 col = s.saturated ? colSat
                        : (st.last.accepted ? colOk : colBad);

        if (app.overlayWindows) {
            const float h = std::max(4.0f, half);
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h),
                        col, 0.0f, 0, 1.5f);
        }
        // Gapped crosshair: the centre stays clear so the marker never hides
        // the pixels the measurement came from.
        const float g = 3.0f, len = 9.0f;
        dl->AddLine(ImVec2(c.x - g - len, c.y), ImVec2(c.x - g, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x + g, c.y), ImVec2(c.x + g + len, c.y), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y - g - len), ImVec2(c.x, c.y - g), col, 1.5f);
        dl->AddLine(ImVec2(c.x, c.y + g), ImVec2(c.x, c.y + g + len), col, 1.5f);
        dl->AddCircle(c, 2.5f, col, 0, 1.5f);

        if (app.overlayLabels) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s  %.2f\" ", label,
                          s.fwhmPx * app.dimm.calibration.arcsecPerPixel);
            dl->AddText(ImVec2(c.x + std::max(4.0f, half) + 4.0f,
                               c.y - std::max(4.0f, half)), col, buf);
        }
    };

    marker(st.last.a, "A");
    marker(st.last.b, "B");

    if (app.overlayAxis && st.last.a.valid && st.last.b.valid) {
        ImVec2 pa, pb;
        if (app.viewport.mapFrameToScreen(st.last.a.x, st.last.a.y, pa) &&
            app.viewport.mapFrameToScreen(st.last.b.x, st.last.b.y, pb)) {
            // Separation vector: the WEDGE DEVIATION axis.
            dl->AddLine(pa, pb, IM_COL32(235, 60, 60, 90), 1.0f);

            // Longitudinal axis: set by the mask BASELINE, which is why it need
            // not lie along the separation. Drawn through the midpoint so a
            // mis-set wedge orientation is visible rather than inferred.
            const float mx = 0.5f * (pa.x + pb.x), my = 0.5f * (pa.y + pb.y);
            const float th = float(st.axisAngleDeg) * 3.14159265f / 180.0f;
            // Same length as the separation, so the indicator cannot be read as
            // a claim about distance.
            const float L  = 0.5f * std::sqrt((pb.x - pa.x) * (pb.x - pa.x) +
                                              (pb.y - pa.y) * (pb.y - pa.y));
            const float ux = std::cos(th), uy = std::sin(th);
            dl->AddLine(ImVec2(mx - ux * L, my - uy * L),
                        ImVec2(mx + ux * L, my + uy * L), colAxis, 1.5f);
            dl->AddLine(ImVec2(mx + uy * L * 0.5f, my - ux * L * 0.5f),
                        ImVec2(mx - uy * L * 0.5f, my + ux * L * 0.5f),
                        IM_COL32(90, 170, 235, 90), 1.0f);
        }
    }
}

// Placeholder for the central work area. The live view, strip charts and
// control panels will dock in here.
static void CentralArea(MeiApp& app) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (ImGui::Begin("##Central", nullptr, flags)) {
        ImGui::TextDisabled("OpenMei");
        ImGui::Separator();
        if (!app.cameras.connected()) {
            ImGui::TextWrapped("No camera connected.  Camera > Connect...");
            ImGui::Spacing();
            ImGui::BulletText("CAL: %s", app.calibrated ? "ready" : "required before measuring");
            ImGui::BulletText("DRK: %s", app.haveDarks  ? "loaded" : "not loaded");
            ImGui::End();
            return;
        }

        const ICamera* cam = app.cameras.camera();
        const Caps&    c   = cam->caps();

        SeeingReadout(app);
        ImGui::Spacing();

        // --- toolbar ---------------------------------------------------------
        ImGui::Text("%s", app.cameras.connectedDesc().displayName().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("| %d x %d, %.2f um, %d-bit, %s | %s cam on %s port",
                            c.maxWidth, c.maxHeight, c.pixelSizeUm, c.adcBits,
                            c.isColor ? toString(c.bayer) : "mono",
                            toString(c.cameraUsb), toString(c.hostUsb));
        if (c.cameraUsb == UsbSpeed::USB3 && c.hostUsb == UsbSpeed::USB2) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f),
                               "USB3 camera in a USB2 port");
        }
        if (c.binningIsSoftware && app.want.bin > 1) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f), "(sw bin)");
            ImGui::SetItemTooltip(
                "Binning is done on the host, so a binned ROI still reads out "
                "and transfers at full sensor resolution. Frame rate is set by "
                "the UNBINNED row count -- shrink the ROI height to go faster.");
        }

        if (!app.stream.running()) {
            if (ImGui::Button("Start stream")) startStream(app);
        } else {
            if (ImGui::Button("Stop stream")) { app.processor.stopMeasuring(); app.stream.stop(); }
        }
        ImGui::SameLine();

        // Exposure and gain are live-adjustable while streaming.
        // Range comes from the camera, not from a guess. Sub-millisecond
        // exposures are desirable here -- exposure-time bias is the largest
        // single systematic in a DIMM, and it shrinks as exposure does -- so
        // the lower bound must be the hardware minimum, not a round number.
        const double minMs = c.exposureUs.supported
                                 ? double(c.exposureUs.min) / 1000.0 : 0.032;
        const double maxMs = c.exposureUs.supported
                                 ? std::min(double(c.exposureUs.max) / 1000.0, 1000.0)
                                 : 1000.0;
        ImGui::SetNextItemWidth(150);
        float expMs = float(app.want.exposureUs) / 1000.0f;
        if (ImGui::SliderFloat("Exp (ms)", &expMs, float(minMs), float(maxMs),
                               "%.3f",
                               ImGuiSliderFlags_Logarithmic |
                               ImGuiSliderFlags_AlwaysClamp)) {
            std::string err;
            app.want.exposureUs = int64_t(double(expMs) * 1000.0);
            if (app.stream.running()) {
                app.cameras.camera()->setExposureUs(app.want.exposureUs, err);
                // Drivers clamp; adopt what was actually applied so the slider
                // and the status bar agree with the hardware.
                app.want.exposureUs = app.cameras.camera()->config().exposureUs;
            }
        }
        ImGui::SetItemTooltip("Ctrl+click to type an exact value. "
                              "Hardware range: %.3f - %.1f ms.", minMs, maxMs);

        // Standard DIMM exposures. The t/2t pairs matter: the bias correction
        // interleaves an exposure and its double.
        ImGui::SameLine();
        for (double ms : { 1.0, 2.0, 5.0, 10.0 }) {
            char lbl[16];
            std::snprintf(lbl, sizeof(lbl), "%.0fms", ms);
            if (ms >= minMs && ms <= maxMs) {
                if (ImGui::SmallButton(lbl)) {
                    std::string err;
                    app.want.exposureUs = int64_t(ms * 1000.0);
                    if (app.stream.running()) {
                        app.cameras.camera()->setExposureUs(app.want.exposureUs, err);
                        app.want.exposureUs = app.cameras.camera()->config().exposureUs;
                    }
                }
                ImGui::SameLine();
            }
        }

        // Below the readout interval, shortening exposure costs signal and buys
        // no cadence -- worth saying, because the fps number stops responding
        // and that looks like a bug.
        {
            const StreamStats es = app.stream.stats();
            if (app.stream.running() && es.measuredIntervalMs > 0.0 &&
                double(app.want.exposureUs) / 1000.0 < es.measuredIntervalMs * 0.6) {
                ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.0f),
                                   "readout-limited (%.2f ms/frame)",
                                   es.measuredIntervalMs);
                ImGui::SetItemTooltip(
                    "Exposure is well under the frame interval, so cadence is "
                    "set by readout. Shorter exposures still reduce exposure-time "
                    "bias, but cost SNR without gaining frames.");
                ImGui::SameLine();
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150);
        float gain = float(app.want.gain);
        if (ImGui::SliderFloat("Gain", &gain,
                               float(c.gain.min), float(c.gain.max), "%.0f")) {
            std::string err;
            app.want.gain = gain;
            if (app.stream.running()) {
                app.cameras.camera()->setGain(gain, err);
                app.want.gain = app.cameras.camera()->config().gain;
            }
        }
        ImGui::SameLine();

        // Changing pixel format needs a reconfigure, so it is only offered
        // while stopped.
        ImGui::BeginDisabled(app.stream.running());
        ImGui::SetNextItemWidth(110);
        const bool is16 = bytesPerPixel(app.want.format) == 2;
        int depthIdx = is16 ? 1 : 0;
        if (ImGui::Combo("Depth", &depthIdx, "8-bit\0" "16-bit\0")) {
            const bool want16 = (depthIdx == 1);
            const PixelFormat target = c.isColor
                ? (want16 ? PixelFormat::Bayer16 : PixelFormat::Bayer8)
                : (want16 ? PixelFormat::Mono16  : PixelFormat::Mono8);
            for (auto f : c.formats) if (f == target) app.want.format = f;
            app.want.highSpeed = !want16;
        }
        ImGui::EndDisabled();
        if (is16 && app.want.highSpeed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f), "(!)");
            ImGui::SetItemTooltip("High-speed mode reads out at reduced ADC "
                                  "depth; combined with 16-bit output the extra "
                                  "bits are not real.");
        }

        ImGui::SameLine();
        ImGui::Checkbox("Auto stretch", &app.viewport.autoStretch);
        ImGui::SameLine();
        ImGui::Checkbox("Overlay", &app.overlayShow);
        if (ImGui::BeginPopupContextItem("##overlayopts")) {
            ImGui::Checkbox("Window boxes", &app.overlayWindows);
            ImGui::Checkbox("Axes",         &app.overlayAxis);
            ImGui::Checkbox("Labels",       &app.overlayLabels);
            ImGui::EndPopup();
        }
        ImGui::SetItemTooltip("Right-click for overlay options.");

        if (ImGui::TreeNode("ROI / binning")) {
            ImGui::BeginDisabled(app.stream.running());
            ImGui::TextDisabled("Applied on next stream start.");

            if (!c.bins.empty()) {
                std::string items;
                int cur = 0;
                for (size_t i = 0; i < c.bins.size(); ++i) {
                    items += "bin" + std::to_string(c.bins[i]);
                    items.push_back('\0');
                    if (c.bins[i] == app.want.bin) cur = int(i);
                }
                items.push_back('\0');
                ImGui::SetNextItemWidth(90);
                if (ImGui::Combo("Binning", &cur, items.c_str())) {
                    app.want.bin = c.bins[size_t(cur)];
                    app.want.monoBin = c.isColor && c.hasMonoBin && app.want.bin > 1;
                    // ROI is in binned coordinates, so it must be re-derived.
                    app.want.width  = std::min(app.want.width,  c.maxWidth  / app.want.bin);
                    app.want.height = std::min(app.want.height, c.maxHeight / app.want.bin);
                    app.want.x = (c.maxWidth  / app.want.bin - app.want.width)  / 2;
                    app.want.y = (c.maxHeight / app.want.bin - app.want.height) / 2;
                }
            }
            if (c.isColor) {
                ImGui::SameLine();
                ImGui::BeginDisabled(!c.hasMonoBin || app.want.bin < 2);
                ImGui::Checkbox("Mono bin", &app.want.monoBin);
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Sums each 2x2 CFA block on the sensor so "
                                      "the colour filter response cancels. "
                                      "Required for unbiased centroids on a "
                                      "colour camera. Needs bin >= 2.");
            }

            int wh[2] = { app.want.width, app.want.height };
            ImGui::SetNextItemWidth(160);
            if (ImGui::InputInt2("Size (binned px)", wh)) {
                app.want.width  = std::clamp(wh[0], 8, c.maxWidth  / app.want.bin);
                app.want.height = std::clamp(wh[1], 2, c.maxHeight / app.want.bin);
                app.want.x = (c.maxWidth  / app.want.bin - app.want.width)  / 2;
                app.want.y = (c.maxHeight / app.want.bin - app.want.height) / 2;
            }

            // Rolling-shutter readout scales with ROWS, so height is the lever
            // for frame rate. Width barely matters.
            ImGui::TextDisabled("Presets (height drives frame rate):");
            const int presets[][2] = { {1920,1080}, {800,600}, {400,200}, {256,128} };
            for (const auto& p : presets) {
                char lbl[24];
                std::snprintf(lbl, sizeof(lbl), "%dx%d", p[0], p[1]);
                if (ImGui::SmallButton(lbl)) {
                    app.want.width  = std::min(p[0], c.maxWidth  / app.want.bin);
                    app.want.height = std::min(p[1], c.maxHeight / app.want.bin);
                    app.want.x = (c.maxWidth  / app.want.bin - app.want.width)  / 2;
                    app.want.y = (c.maxHeight / app.want.bin - app.want.height) / 2;
                }
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::EndDisabled();
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Link")) {
            ImGui::BeginDisabled(app.stream.running());
            if (c.hasHardwareBin) {
                // On a colour sensor these conflict: hardware binning sums
                // same-colour pixels and keeps the CFA, mono binning sums
                // across it and removes it. Mono binning is the one a DIMM
                // needs, so it takes precedence.
                const bool blocked = c.isColor && app.want.monoBin;
                ImGui::BeginDisabled(blocked);
                if (ImGui::Checkbox("Hardware binning", &app.want.hardwareBin)) {}
                ImGui::EndDisabled();
                ImGui::SetItemTooltip(
                    blocked
                    ? "Unavailable while mono binning is on. Sensor binning sums "
                      "same-colour pixels, so the colour filter array survives "
                      "and reintroduces the centroid bias mono binning removes."
                    : "On-sensor summing: readout time and USB payload both drop "
                      "by bin^2. Host-side binning shrinks neither.");
            } else {
                ImGui::TextDisabled("Hardware binning not offered by this camera");
            }
            ImGui::EndDisabled();

            if (c.hasUsbBandwidth) {
                ImGui::SetNextItemWidth(200);
                int bw = app.want.usbBandwidth < 0 ? 100 : app.want.usbBandwidth;
                if (ImGui::SliderInt("USB bandwidth %", &bw, 40, 100)) {
                    app.want.usbBandwidth = bw;
                    // Takes effect on next configure; harmless to set live too.
                }
            }

            const StreamStats ls = app.stream.stats();
            ImGui::TextDisabled("frame %zu bytes  |  delivered %.0f MB/s  |  link %.0f MB/s%s",
                                ls.frameBytes, ls.mbPerSec, ls.linkMbPerSec,
                                c.binningIsSoftware && app.want.bin > 1
                                    ? "  (host-side binning)" : "");
            ImGui::TreePop();
        }

        // --- live view -------------------------------------------------------
        app.stream.withLatest([&app](const Frame& f) { app.viewport.upload(f); });

        const StreamStats st = app.stream.stats();
        ImGui::TextDisabled(
            "%.1f fps (%.2f ms)  grabbed %llu  display-skipped %llu  sdk-dropped %d%s",
            st.fps, st.measuredIntervalMs,
            static_cast<unsigned long long>(st.framesGrabbed),
            static_cast<unsigned long long>(st.framesSkipped),
            st.sdkDropped,
            app.viewport.lastSaturated() ? "   [SATURATED]" : "");

        ImGui::Separator();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        if (!app.viewport.draw(avail)) {
            ImGui::TextDisabled("Waiting for first frame...");
        } else {
            SpotOverlay(app);
        }
    }
    ImGui::End();
}

} // namespace mei::ui

// =============================================================================
//  Entry point
// =============================================================================

static void fatal(const char* what) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %s", what, SDL_GetError());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenMei", what, nullptr);
}

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { fatal("SDL_Init failed"); return 1; }

    // Request 3.3 core -- enough for ImGui's GL3 backend, and what GLEW will
    // resolve against. Attributes must be set before context creation.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

    const SDL_WindowFlags winFlags =
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    SDL_Window* window = SDL_CreateWindow("OpenMei", 1280, 800, winFlags);
    if (!window) { fatal("SDL_CreateWindow failed"); SDL_Quit(); return 1; }
    SDL_SetWindowMinimumSize(window, 800, 500);

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) { fatal("SDL_GL_CreateContext failed"); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);   // vsync; this is a monitoring UI, not a game

    // GLEW must be initialised after a context is current. glewExperimental is
    // required for core profiles or glewInit reports functions as unavailable.
    glewExperimental = GL_TRUE;
    if (GLenum err = glewInit(); err != GLEW_OK) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "glewInit failed: %s",
                     reinterpret_cast<const char*>(glewGetErrorString(err)));
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    // glewInit() can leave a spurious GL_INVALID_ENUM on core profiles. Clear it
    // so it does not surface later and look like our own bug.
    glGetError();

    SDL_Log("GL %s | GLSL %s | %s",
            reinterpret_cast<const char*>(glGetString(GL_VERSION)),
            reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)),
            reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // --- Dear ImGui ----------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "openmei_layout.ini";

    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    mei::MeiApp app;
    {
        std::string perr;
        if (!mei::loadSettings(app.prefs, perr)) {
            app.setStatus(mei::Severity::Warning,
                          "Settings could not be loaded, using defaults: " + perr);
        }
        // Alpaca's UniqueID identifies a device INSTANCE. Generated once here
        // and persisted: regenerating it per launch loses the client's saved
        // selection, and a compile-time constant would make two installations
        // on one network indistinguishable.
        if (app.prefs.installationId.empty()) {
            app.prefs.installationId = mei::AlpacaServer::generateUniqueId();
            app.prefsDirty = true;
        }
        app.prefs.alpaca.uniqueId = app.prefs.installationId;
        if (app.prefs.haveStream) app.want = app.prefs.stream;
        SDL_Log("settings: %s", mei::settingsPath().c_str());
    }

    // --- Main loop -----------------------------------------------------------
    while (app.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT) app.running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                ev.window.windowID == SDL_GetWindowID(window)) {
                app.running = false;
            }
        }

        // Minimised: skip rendering but keep the event pump alive.
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Order matters: the menu bar and status bar each shrink the viewport
        // work area, so the central region must be built after both.
        mei::ui::MenuBar(app);
        mei::ui::StatusBar(app);
        mei::ui::CentralArea(app);
        mei::ui::SyntheticPanel(app);
#ifdef OPENMEI_ALPACA
        mei::ui::AlpacaPanel(app);
        {
            // Republish on each completed burst. Keyed on the burst counter,
            // not on the measured values: a steady atmosphere produces
            // near-identical results burst after burst, and frame counts are
            // the same every time by construction, so anything derived from the
            // numbers themselves silently stops updating.
            const mei::DimmStatus ds = app.processor.status();
            if (ds.haveResult && ds.result.burstSequence != app.lastPublishedBurst) {
                app.lastPublishedBurst = ds.result.burstSequence;
                app.alpaca.publish(ds.result, app.dimm);
            }
        }
#endif
        {
            const mei::DimmConfig before = app.dimm;
            app.settings.draw(app.dimm);
            // Cheap structural comparison is not available, so key on the one
            // field that gates everything plus a coarse checksum of geometry.
            if (before.calibration.arcsecPerPixel != app.dimm.calibration.arcsecPerPixel ||
                before.calibration.method != app.dimm.calibration.method ||
                before.optics.subApertureMm != app.dimm.optics.subApertureMm ||
                before.optics.baselineMm != app.dimm.optics.baselineMm ||
                before.burst.framesPerBurst != app.dimm.burst.framesPerBurst) {
                app.prefsDirty = true;
            }
        }
        app.processor.setConfig(app.dimm);
        mei::ui::OverlayOptions ov;
        ov.show    = &app.overlayShow;
        ov.windows = &app.overlayWindows;
        ov.axis    = &app.overlayAxis;
        ov.labels  = &app.overlayLabels;
        mei::ui::DimmPanel(&app.showDimmPanel, app.processor, app.dimm,
                           app.stream.running(), app.isSynthetic(), ov);
        app.cameraDialog.draw(app.cameras,
            [&app](bool ok, std::string msg) {
                app.setStatus(ok ? mei::Severity::Info : mei::Severity::Error,
                              std::move(msg));
            });
        if (app.showDemo) ImGui::ShowDemoWindow(&app.showDemo);

        // Debounced save. Writing on every frame would hammer the disk; writing
        // only at exit would lose everything on a crash, and a crash mid-session
        // is exactly when a freshly measured plate scale is most valuable.
        if (app.prefsDirty) {
            const Uint64 nowMs = SDL_GetTicks();
            if (nowMs - app.lastPrefsSaveMs > 2000) {
                app.prefs.stream = app.want;
                app.prefs.haveStream = app.want.width > 0;
                if (app.cameras.connected())
                    app.prefs.lastCameraKey = app.cameras.connectedDesc().uniqueKey();
                std::string perr;
                if (!mei::saveSettings(app.prefs, perr))
                    app.setStatus(mei::Severity::Warning, "Could not save settings: " + perr);
                app.prefsDirty = false;
                app.lastPrefsSaveMs = nowMs;
            }
        }

        ImGui::Render();

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);   // pixels, not points -- HiDPI
        glViewport(0, 0, w, h);
        glClearColor(0.09f, 0.09f, 0.11f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // --- Teardown ------------------------------------------------------------
    // Order matters: join the grab thread and drop the GL texture while the
    // context is still current, before ImGui or SDL go away.
    {
        app.prefs.stream = app.want;
        app.prefs.haveStream = app.want.width > 0;
        std::string perr;
        mei::saveSettings(app.prefs, perr);
    }
#ifdef OPENMEI_ALPACA
    app.alpaca.stop();
#endif
    app.processor.stopMeasuring();
    app.stream.stop();
    app.viewport.release();
    app.cameras.disconnect();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
