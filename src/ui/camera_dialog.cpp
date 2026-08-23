#include "ui/camera_dialog.h"

#include "camera/backends/ser_reader.h"


#include "imgui.h"

#include <SDL3/SDL_dialog.h>

#include <cstdio>
#include <cstring>

namespace mei::ui {

    namespace {

        // SDL_ShowOpenFileDialog is asynchronous: the callback fires later, on the
        // main thread during event pumping. So it stores the path and the next
        // rescan picks it up, rather than returning a result inline.
        void SDLCALL onSerFileChosen(void* userdata, const char* const* files, int) {
            auto* rescan = static_cast<bool*>(userdata);
            if (files && files[0]) {
                setSerReplayFile(files[0]);
                if (rescan) *rescan = true;
            }
        }

        void capsPanel(const CameraDesc& d, const CameraRegistry& reg) {
            ImGui::TextDisabled("Selected");
            ImGui::TextUnformatted(d.displayName().c_str());
            ImGui::TextDisabled("key: %s", d.uniqueKey().c_str());
            ImGui::Spacing();
            ImGui::TextWrapped(
                "Full capabilities are read when the camera is opened; opening runs "
                "the vendor init sequence, which can take a moment.");
            (void)reg;
        }

    } // namespace

    void CameraDialog::draw(CameraRegistry& reg, const ResultFn& onResult) {
        if (requestOpen_) {
            ImGui::OpenPopup("Connect Camera");
            requestOpen_ = false;
            refreshed_ = false;
        }

        ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
        if (!ImGui::BeginPopupModal("Connect Camera", nullptr,
            ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }

        if (!refreshed_) {
            reg.refresh();
            refreshed_ = true;
            selected_ = reg.cameras().empty() ? -1 : 0;
        }

        // --- Backend availability -------------------------------------------------
        ImGui::TextDisabled("Backends");
        if (ImGui::BeginTable("##backends", 3,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Detail");
            ImGui::TableHeadersRow();

            for (const auto& b : reg.backends()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(b.displayName.c_str());

                ImGui::TableNextColumn();
                if (b.loaded) {
                    ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.45f, 1.0f), "loaded");
                }
                else {
                    ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.25f, 1.0f), "absent");
                }

                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", b.message.c_str());
                if (!b.diagnostics.empty()) {
                    ImGui::PushID(b.id.c_str());
                    if (ImGui::TreeNode("details")) {
                        ImGui::TextUnformatted(b.diagnostics.c_str());
                        if (ImGui::SmallButton("Copy"))
                            ImGui::SetClipboardText(b.diagnostics.c_str());
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        if (reg.backends().empty()) {
            ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.25f, 1.0f),
                "No camera backends were compiled in. Place the vendor "
                "headers under third_party/vendor_headers and reconfigure.");
        }

        // --- SER replay -----------------------------------------------------------
        // Choosing a file is the replay backend's equivalent of a USB scan, which is
        // why it lives here beside the cameras rather than under File > Open: both
        // answer the same question, which is where frames come from.
        {
            const std::string cur = serReplayFile();
            ImGui::Spacing();
            ImGui::TextDisabled("SER file replay");
            if (ImGui::Button("Choose file...")) {
                static const SDL_DialogFileFilter filters[] = {
                    { "SER captures", "ser" },
                    { "All files",    "*" },
                };
                static bool rescanFlag = false;
                rescanFlag = false;
                SDL_ShowOpenFileDialog(onSerFileChosen, &rescanFlag, nullptr,
                    filters, 2, nullptr, false);
                refreshed_ = false;   // pick up the choice on the next frame
            }
            ImGui::SameLine();
            if (cur.empty()) {
                ImGui::TextDisabled("(none selected)");
            }
            else {
                ImGui::TextWrapped("%s", cur.c_str());
                if (ImGui::SmallButton("Clear")) {
                    setSerReplayFile("");
                    refreshed_ = false;
                }

                // Read the header here rather than relying on the backend's
                // diagnostics string: what a capture contains decides whether it can
                // be measured at all, and that should be visible before connecting
                // rather than buried under a details node.
                SerReader probe;
                std::string perr;
                if (!probe.open(cur, perr)) {
                    ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.35f, 1.0f), "%s", perr.c_str());
                }
                else {
                    const SerHeaderInfo& h = probe.header();
                    const SerDetection& det = probe.detection();

                    ImGui::Indent();
                    ImGui::Text("%d x %d, %d frames, %d-bit",
                        h.width, h.height, h.frameCount, h.pixelDepthPerPlane);

                    if (det.haveTimestamps && det.medianIntervalMs > 0.0) {
                        const double fps = 1000.0 / det.medianIntervalMs;
                        const double durS = det.medianIntervalMs * h.frameCount / 1000.0;
                        ImGui::Text("%.2f ms between frames (%.1f fps), %.0f s total",
                            det.medianIntervalMs, fps, durS);

                        // SER has no field for exposure time. The interval is the
                        // only timing the file carries, so the duty cycle -- which
                        // governs how well a synthesized 2t leg approximates a real
                        // one -- cannot be determined from the file alone.
                        ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f),
                            "Exposure is not recorded by the SER format.");
                        ImGui::TextWrapped(
                            "Set it to match the capture in Settings > Statistics. If the "
                            "exposure is much shorter than the %.1f ms interval, the "
                            "duty cycle is low and the synthesized 2t leg of the "
                            "exposure-bias correction will be unreliable.",
                            det.medianIntervalMs);
                    }
                    else {
                        ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f),
                            "No timestamp trailer -- frame timing is unknown and a "
                            "fixed rate will be assumed.");
                    }

                    if (!h.instrument.empty())
                        ImGui::TextDisabled("Instrument: %s", h.instrument.c_str());
                    if (!h.observer.empty())
                        ImGui::TextDisabled("Observer: %s", h.observer.c_str());

                    // Both of these mean the file's own header was wrong, which is
                    // common enough to be worth stating plainly rather than fixing
                    // silently.
                    if (det.endiannessOverridden) {
                        ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f),
                            "Byte order taken from the data (%s), not the header; "
                            "confidence %.1fx.",
                            det.dataIsLittleEndian ? "little-endian" : "big-endian",
                            det.smoothnessRatio);
                    }
                    if (det.depthOverridden) {
                        ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.25f, 1.0f),
                            "Header declares %d bits but the data occupies %d.",
                            h.pixelDepthPerPlane, det.detectedSignificantBits);
                    }
                    ImGui::Unindent();
                }
            }
        }

        ImGui::Spacing();

        ImGui::Spacing();

        // --- SDK path override ----------------------------------------------------
        if (ImGui::TreeNode("SDK search path")) {
            ImGui::TextWrapped(
                "Vendor libraries are loaded at runtime. If a backend shows as "
                "absent, point it at the folder containing the vendor DLL or shared "
                "object -- normally installed with the vendor's camera driver.");
            for (const auto& b : reg.backends()) {
                ImGui::PushID(b.id.c_str());
                std::snprintf(sdkDirBuf_, sizeof(sdkDirBuf_), "%s",
                    reg.sdkDir(b.id).c_str());
                if (ImGui::InputText(b.displayName.c_str(), sdkDirBuf_, sizeof(sdkDirBuf_))) {
                    reg.setSdkDir(b.id, sdkDirBuf_);
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Rescan")) refreshed_ = false;
            ImGui::TreePop();
        }

        ImGui::Separator();

        // --- Detected cameras -----------------------------------------------------
        ImGui::TextDisabled("Detected cameras");
        const auto& cams = reg.cameras();

        ImGui::BeginChild("##cams", ImVec2(0, 140), ImGuiChildFlags_Border);
        if (cams.empty()) {
            ImGui::TextDisabled("None. Check connections, then Rescan.");
        }
        for (int i = 0; i < static_cast<int>(cams.size()); ++i) {
            const bool sel = (i == selected_);
            if (ImGui::Selectable(cams[i].displayName().c_str(), sel)) selected_ = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndChild();

        if (selected_ >= 0 && selected_ < static_cast<int>(cams.size())) {
            capsPanel(cams[selected_], reg);
        }

        ImGui::Separator();

        // --- Actions --------------------------------------------------------------
        if (ImGui::Button("Rescan", ImVec2(90, 0))) refreshed_ = false;
        ImGui::SameLine();

        const bool canConnect =
            selected_ >= 0 && selected_ < static_cast<int>(cams.size());
        ImGui::BeginDisabled(!canConnect);
        if (ImGui::Button("Connect", ImVec2(110, 0))) {
            std::string err;
            const CameraDesc d = cams[selected_];
            if (reg.connect(d, err)) {
                if (onResult) onResult(true, "Connected to " + d.displayName());
                ImGui::CloseCurrentPopup();
            }
            else {
                if (onResult) onResult(false, "Connect failed: " + err);
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

} // namespace mei::ui