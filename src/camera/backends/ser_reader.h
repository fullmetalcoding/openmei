// ser_reader.h -- SER v3 capture files.
//
// The format is simple: a 178-byte header, tightly packed frames, and an
// optional trailer of one 64-bit UTC timestamp per frame. Two fields are
// unreliable in the wild and both are handled here rather than trusted:
//
//   * LittleEndian. The v3 spec says 0 means the 16-bit data is BIG-endian,
//     but enough capture software wrote it inverted that PIPP, AutoStakkert and
//     SER Player all sniff the data instead. So do we.
//   * PixelDepthPerPlane. Frequently written as 16 when the sensor is 12-bit,
//     which throws off the display stretch and the saturation gate.

#pragma once

#include "camera/camera.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mei {

    enum class SerColorId : int32_t {
        Mono = 0,
        BayerRGGB = 8,
        BayerGRBG = 9,
        BayerGBRG = 10,
        BayerBGGR = 11,
        BayerCYYM = 16,
        BayerYCMY = 17,
        BayerYMCY = 18,
        BayerMYYC = 19,
        RGB = 100,
        BGR = 101,
    };

    struct SerHeaderInfo {
        std::string fileId;          // expected "LUCAM-RECORDER"
        int32_t     luId = 0;
        SerColorId  colorId = SerColorId::Mono;
        bool        headerSaysLittleEndian = false;
        int32_t     width = 0;
        int32_t     height = 0;
        int32_t     pixelDepthPerPlane = 8;
        int32_t     frameCount = 0;
        std::string observer, instrument, telescope;
        int64_t     dateTimeTicks = 0;      // .NET ticks, local
        int64_t     dateTimeUtcTicks = 0;

        int planes = 1;
        int bytesPerSample = 1;
        size_t frameBytes = 0;
    };

    // What the reader concluded, as distinct from what the file claimed. Surfaced
    // so the UI can show both and let the user override.
    struct SerDetection {
        bool   endiannessOverridden = false;   // sniffed value differs from header
        bool   dataIsLittleEndian = true;
        bool   depthOverridden = false;
        int    detectedSignificantBits = 8;
        double smoothnessRatio = 1.0;          // >1 means the sniff was confident
        bool   haveTimestamps = false;
        double medianIntervalMs = 0.0;
    };

    class SerReader {
    public:
        ~SerReader() { close(); }

        bool open(const std::string& path, std::string& err);
        void close();
        bool isOpen() const { return f_ != nullptr; }

        const SerHeaderInfo& header() const { return h_; }
        const SerDetection& detection() const { return det_; }
        const std::string& path() const { return path_; }

        uint64_t frameCount() const { return uint64_t(h_.frameCount); }

        // Reads frame `index` into `dst`, which must hold at least frameBytes().
        // Byte-swaps in place when the data is big-endian.
        bool readFrame(uint64_t index, uint8_t* dst, std::string& err);

        // Unix seconds for a frame, from the trailer. Falls back to the header
        // start time plus the median interval when no trailer is present.
        double frameTimeUnix(uint64_t index) const;

        // What the frames should be labelled as, after detection.
        PixelFormat  pixelFormat() const;
        BayerPattern bayerPattern() const;
        int          significantBits() const { return det_.detectedSignificantBits; }

        // Applied by the caller: SER stores raw sensor values right-aligned, unlike
        // the left-aligned convention several camera SDKs use.
        int sampleShift() const { return 0; }

    private:
        bool parseHeader(std::string& err);
        void detectEndianness();
        void detectDepth();
        void loadTimestamps();

        std::FILE* f_ = nullptr;
        std::string path_;
        SerHeaderInfo h_;
        SerDetection  det_;
        std::vector<int64_t> stamps_;   // .NET ticks, UTC
    };

} // namespace mei