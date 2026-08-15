#pragma once

#include "dimm/config.h"

namespace mei::ui {

// Edit > Settings. Tabbed, with derived quantities and validity warnings shown
// live -- the point is that a bad configuration should be visible while you are
// entering it, not discovered later from an implausible seeing value.
class SettingsDialog {
public:
    void open() { requestOpen_ = true; }
    void draw(DimmConfig& cfg);

private:
    void drawOptics(DimmConfig&);
    void drawCalibration(DimmConfig&);
    void drawDetection(DimmConfig&);
    void drawStatistics(DimmConfig&);
    void drawSite(DimmConfig&);
    void drawReporting(DimmConfig&);

    bool       requestOpen_ = false;
    DimmConfig backup_{};      // for Cancel
    bool       haveBackup_ = false;
};

} // namespace mei::ui
