#include "ui/camera_dialog.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace mei::ui {

namespace {

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
        refreshed_   = false;
    }

    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal("Connect Camera", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (!refreshed_) {
        reg.refresh();
        refreshed_ = true;
        selected_  = reg.cameras().empty() ? -1 : 0;
    }

    // --- Backend availability -------------------------------------------------
    ImGui::TextDisabled("Backends");
    if (ImGui::BeginTable("##backends", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Vendor",  ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Status",  ImGuiTableColumnFlags_WidthFixed,  70.0f);
        ImGui::TableSetupColumn("Detail");
        ImGui::TableHeadersRow();

        for (const auto& b : reg.backends()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.displayName.c_str());

            ImGui::TableNextColumn();
            if (b.loaded) {
                ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.45f, 1.0f), "loaded");
            } else {
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
        } else {
            if (onResult) onResult(false, "Connect failed: " + err);
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
}

} // namespace mei::ui
