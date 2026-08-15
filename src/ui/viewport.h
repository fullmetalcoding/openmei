// viewport.h -- uploads camera frames to a GL texture and draws them.

#pragma once

#include "camera/camera.h"

#include <cstdint>
#include <vector>

struct ImVec2;

namespace mei::ui {

    class Viewport {
    public:
        ~Viewport();

        // Converts to 8-bit for display and uploads. Applies FrameMeta::sampleShift
        // so a 12-bit sensor does not render as a dim smear; this is display-only,
        // the measurement path never sees the converted data.
        void upload(const Frame& f);

        // Draws the texture aspect-fitted into `avail`. Returns false if nothing
        // has been uploaded yet.
        bool draw(const ImVec2& avail);

        void release();

        bool  autoStretch = true;
        float blackPoint = 0.0f;   // 0..1, used when autoStretch is off
        float whitePoint = 1.0f;
        bool  showCrosshair = true;

        // Sum each 2x2 CFA block for display when a frame carries a Bayer pattern.
        // Halves the displayed size but removes the mosaic texture. This is display
        // only -- the analysis path must do its own summing.
        bool  sumBayer2x2 = true;

        int  width()  const { return w_; }
        int  height() const { return h_; }
        // Percentiles from the last uploaded frame, in raw ADU.
        int  lastMin() const { return lastMin_; }
        int  lastMax() const { return lastMax_; }
        bool lastSaturated() const { return saturated_; }
        // Real bits behind the container, from FrameMeta. A 16-bit frame from a
        // 12-bit sensor reports 12 here.
        int  lastSignificantBits() const { return sigBits_; }

    private:
        void uploadBayerSummed(const Frame& f);
        void uploadGray(int width, int height);

        unsigned int         tex_ = 0;     // GLuint
        int                  w_ = 0, h_ = 0;
        std::vector<uint8_t> scratch_;
        int  lastMin_ = 0, lastMax_ = 0;
        int  sigBits_ = 8;
        bool saturated_ = false;
    };

} // namespace mei::ui