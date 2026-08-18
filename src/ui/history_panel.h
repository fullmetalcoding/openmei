#pragma once

#include "app/seeing_history.h"
#include "app/settings_store.h"
#include "dimm/config.h"

namespace mei::ui {

    // Rolling plot of completed bursts. Window length comes from the config so the
    // setting lives in one place; the panel only reads it.
    void SeeingHistoryPanel(bool* open, SeeingHistory&, const DimmConfig&,
        HistoryView&, bool measuring);

} // namespace mei::ui