// settings_store.h -- persistence for everything that should survive a restart.
//
// The plate scale is the motivating case. It is a measurement, taken once at
// commissioning, and losing it on exit means re-entering it every session --
// which in practice means people stop calibrating and start guessing.
//
// The installation UUID lives here for the same reason: Alpaca's UniqueID
// identifies a device INSTANCE, so it must be stable across restarts (or
// clients lose their saved selection) while still differing between two
// installations on one network (or clients confuse them for each other). Only
// generate-once-and-persist satisfies both; a compile-time constant fails the
// second and changes on every upgrade.

#pragma once

#include "camera/camera.h"
#include "dimm/config.h"
#include "net/alpaca_server.h"

#include <string>

namespace mei {

    // Pure view preferences for the history plot. Deliberately not part of
    // DimmConfig: nothing here changes a measurement, and mixing presentation into
    // the measurement configuration makes the provenance record noisier for no
    // benefit.
    struct HistoryView {
        // Data arrives at the right edge, so a legend anchored there sits on top of
        // exactly the part being watched.
        int   legendCorner = 0;      // 0 = TL, 1 = TR, 2 = BL, 3 = BR
        bool  legendOutside = false;
        bool  legendHidden = false;

        // Rescale to fit on every frame. Off lets the user zoom and keep it.
        bool  autoFitY = true;

        float smoothingMinutes = 2.0f;
        bool  showBand = true;
        bool  showR0 = false;
        bool  showLos = false;
    };

    struct AppSettings {
        DimmConfig   dimm;
        AlpacaConfig alpaca;
        HistoryView  historyView;

        // Stream defaults, so a session resumes where the last one left off.
        StreamConfig stream;
        bool         haveStream = false;

        // Reconnect to the same physical camera rather than whatever enumerates
        // first. Matches CameraDesc::uniqueKey().
        std::string lastCameraKey;

        // Generated on first run and never regenerated.
        std::string installationId;

        bool autoStartAlpaca = false;
    };

    // Per-user writable location, from SDL_GetPrefPath. On Windows that is under
    // AppData/Roaming rather than next to the executable, which matters because
    // Program Files is not writable.
    std::string settingsPath();

    // Missing file is not an error -- it is a first run, and defaults apply.
    // Returns false only on a file that exists but cannot be parsed, so a corrupt
    // config is reported rather than silently discarded.
    bool loadSettings(AppSettings&, std::string& err);
    bool saveSettings(const AppSettings&, std::string& err);

} // namespace mei