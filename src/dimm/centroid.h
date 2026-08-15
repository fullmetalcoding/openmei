// centroid.h -- spot detection and sub-pixel centroiding.
//
// The centroid is the measurement. Everything downstream is arithmetic on
// these numbers, so the systematics that matter live here: threshold choice,
// window size, background estimation, and pixel-phase bias.

#pragma once

#include "camera/camera.h"
#include "dimm/config.h"

#include <cstring>
#include <vector>

namespace mei {

// Raw ADU at a pixel, with the container shift undone. Out of bounds returns 0.
inline double pixelAt(const Frame& f, int x, int y) {
    const FrameMeta& m = f.meta;
    if (x < 0 || y < 0 || x >= m.width || y >= m.height) return 0.0;
    const size_t i = size_t(y) * m.width + x;
    if (bytesPerPixel(m.format) == 2) {
        uint16_t v;
        std::memcpy(&v, &f.pixels[i * 2], 2);
        return double(v >> m.sampleShift);
    }
    return double(f.pixels[i]);
}

inline double frameFullScale(const Frame& f) {
    return double((1 << f.meta.significantBits) - 1);
}

struct SpotMeasurement {
    bool   valid = false;
    double x = 0.0, y = 0.0;      // frame coordinates, sub-pixel
    double flux = 0.0;            // background-subtracted, ADU
    double peak = 0.0;            // above background
    double background = 0.0;
    double backgroundSigma = 0.0;
    double fwhmPx = 0.0;          // from second moments
    double ellipticity = 0.0;     // 1 - minor/major
    double snr = 0.0;
    int    pixelsUsed = 0;
    bool   saturated = false;
};

// Background level and its scatter from an annulus around (cx, cy). Median and
// MAD rather than mean and stddev: a field star or a cosmic ray in the annulus
// should not move the estimate.
void estimateBackground(const Frame&, double cx, double cy,
                        const CentroidConfig&, double& level, double& sigma);

// Thresholded intensity-weighted first moment, recentred `iterations` times.
//
// The threshold is a bias knob in both directions -- too high clips the wings
// asymmetrically, too low lets noise pull the result -- and the window size
// changes the measured response, so both belong in the provenance record.
SpotMeasurement measureSpot(const Frame&, double guessX, double guessY,
                            const CentroidConfig&);

struct Detection {
    double x = 0.0, y = 0.0;
    double peak = 0.0;
    double flux = 0.0;
};

// Full-frame search for local maxima above threshold. Used only at acquisition;
// once a pair is locked, tracking measures fixed windows.
std::vector<Detection> detectSpots(const Frame&, const AcquisitionConfig&,
                                   const CentroidConfig&, int maxCount = 8);

// Pick the DIMM pair from a detection list. Returns false when the choice is
// ambiguous and the config says to refuse rather than guess -- silently
// selecting the wrong pair produces plausible seeing values indefinitely.
bool selectPair(const std::vector<Detection>&, const DimmConfig&,
                double expectedSepPx, Detection& a, Detection& b,
                std::string& why);

} // namespace mei
