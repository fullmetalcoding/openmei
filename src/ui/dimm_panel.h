#pragma once

#include "dimm/processor.h"

namespace mei::ui {

// Live measurement readout. Everything a live test needs to see: per-frame
// centroids, the separation, running variance, the inverted seeing, and -- when
// running against the synthetic source -- the residual against ground truth,
// which is the only number that says whether the centroider itself is right.
struct OverlayOptions {
    bool* show    = nullptr;
    bool* windows = nullptr;
    bool* axis    = nullptr;
    bool* labels  = nullptr;
};

void DimmPanel(bool* open, DimmProcessor& proc, const DimmConfig& cfg,
               bool cameraStreaming, bool isSynthetic,
               const OverlayOptions& overlay = {});

} // namespace mei::ui
