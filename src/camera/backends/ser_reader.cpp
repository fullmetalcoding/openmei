#include "camera/backends/ser_reader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mei {

    namespace {

        constexpr size_t kHeaderBytes = 178;
        // .NET ticks (100 ns since 0001-01-01) at the Unix epoch.
        constexpr int64_t kTicksAtUnixEpoch = 621355968000000000LL;

        int32_t rd32(const uint8_t* p) {
            int32_t v;
            std::memcpy(&v, p, 4);
            return v;   // SER header fields are always little-endian
        }
        int64_t rd64(const uint8_t* p) {
            int64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }
        std::string rdStr(const uint8_t* p, size_t n) {
            size_t len = 0;
            while (len < n && p[len] != '\0') ++len;
            std::string s(reinterpret_cast<const char*>(p), len);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        }

    } // namespace

    bool SerReader::open(const std::string& path, std::string& err) {
        close();
#ifdef _WIN32
        if (fopen_s(&f_, path.c_str(), "rb") != 0) f_ = nullptr;
#else
        f_ = std::fopen(path.c_str(), "rb");
#endif
        if (!f_) { err = "cannot open " + path; return false; }
        path_ = path;

        if (!parseHeader(err)) { close(); return false; }
        detectEndianness();
        detectDepth();
        loadTimestamps();
        return true;
    }

    void SerReader::close() {
        if (f_) { std::fclose(f_); f_ = nullptr; }
        stamps_.clear();
        h_ = SerHeaderInfo{};
        det_ = SerDetection{};
        path_.clear();
    }

    bool SerReader::parseHeader(std::string& err) {
        uint8_t buf[kHeaderBytes];
        if (std::fread(buf, 1, kHeaderBytes, f_) != kHeaderBytes) {
            err = "file is shorter than a SER header";
            return false;
        }

        h_.fileId = rdStr(buf, 14);
        if (h_.fileId.rfind("LUCAM-RECORDER", 0) != 0) {
            err = "not a SER file (FileID is '" + h_.fileId + "')";
            return false;
        }

        h_.luId = rd32(buf + 14);
        h_.colorId = SerColorId(rd32(buf + 18));
        h_.headerSaysLittleEndian = rd32(buf + 22) != 0;
        h_.width = rd32(buf + 26);
        h_.height = rd32(buf + 30);
        h_.pixelDepthPerPlane = rd32(buf + 34);
        h_.frameCount = rd32(buf + 38);
        h_.observer = rdStr(buf + 42, 40);
        h_.instrument = rdStr(buf + 82, 40);
        h_.telescope = rdStr(buf + 122, 40);
        h_.dateTimeTicks = rd64(buf + 162);
        h_.dateTimeUtcTicks = rd64(buf + 170);

        if (h_.width <= 0 || h_.height <= 0 || h_.frameCount <= 0) {
            err = "SER header has degenerate dimensions or frame count";
            return false;
        }
        if (h_.pixelDepthPerPlane < 1 || h_.pixelDepthPerPlane > 16) {
            err = "SER pixel depth out of range: " +
                std::to_string(h_.pixelDepthPerPlane);
            return false;
        }

        h_.planes = (h_.colorId == SerColorId::RGB || h_.colorId == SerColorId::BGR) ? 3 : 1;
        h_.bytesPerSample = (h_.pixelDepthPerPlane <= 8) ? 1 : 2;
        h_.frameBytes = size_t(h_.width) * size_t(h_.height) *
            size_t(h_.planes) * size_t(h_.bytesPerSample);

        det_.dataIsLittleEndian = h_.headerSaysLittleEndian;
        det_.detectedSignificantBits = h_.pixelDepthPerPlane;
        return true;
    }

    // The header flag is unreliable, so decide from the data. Byte-swapped 16-bit
    // values look like noise: adjacent pixels in a real image are correlated, and
    // swapping destroys that. Comparing mean absolute adjacent-pixel difference for
    // both interpretations picks the right one decisively on any real frame.
    void SerReader::detectEndianness() {
        if (h_.bytesPerSample != 2 || h_.frameCount < 1) return;

        const size_t sampleRow = size_t(h_.width) * h_.planes;
        std::vector<uint8_t> row(sampleRow * 2);

        // A row from the middle of the middle frame: avoids any header oddity and
        // any dark first frame.
        const long off = long(kHeaderBytes) +
            long(h_.frameBytes) * (h_.frameCount / 2) +
            long(sampleRow * 2) * (h_.height / 2);
        if (std::fseek(f_, off, SEEK_SET) != 0) return;
        if (std::fread(row.data(), 1, row.size(), f_) != row.size()) return;

        auto roughness = [&](bool swap) {
            double sum = 0.0;
            int    n = 0;
            int    prev = -1;
            for (size_t i = 0; i + 1 < row.size(); i += 2) {
                const int v = swap ? (row[i] << 8) | row[i + 1]
                    : (row[i + 1] << 8) | row[i];
                if (prev >= 0) { sum += std::fabs(double(v - prev)); ++n; }
                prev = v;
            }
            return n ? sum / n : 0.0;
            };

        const double asLittle = roughness(false);
        const double asBig = roughness(true);
        if (asLittle <= 0.0 && asBig <= 0.0) return;

        const bool little = asLittle <= asBig;
        det_.smoothnessRatio = little
            ? (asLittle > 0.0 ? asBig / asLittle : 1.0)
            : (asBig > 0.0 ? asLittle / asBig : 1.0);

        det_.dataIsLittleEndian = little;
        det_.endiannessOverridden = (little != h_.headerSaysLittleEndian);
    }

    // PixelDepthPerPlane is often written as 16 for a 12-bit sensor. Sampling the
    // actual maximum tells us how many bits are really used, which matters for the
    // display stretch and for the saturation gate.
    void SerReader::detectDepth() {
        if (h_.bytesPerSample != 2) {
            det_.detectedSignificantBits = 8;
            return;
        }

        const size_t sampleRow = size_t(h_.width) * h_.planes;
        std::vector<uint8_t> row(sampleRow * 2);
        int maxVal = 0;

        // Several rows across several frames: one row could easily miss the star.
        for (int fi = 0; fi < std::min(h_.frameCount, 5); ++fi) {
            const long frame = long(h_.frameCount) * fi / std::min(h_.frameCount, 5);
            for (int r = 0; r < 8; ++r) {
                const long rowIdx = long(h_.height) * r / 8;
                const long off = long(kHeaderBytes) + long(h_.frameBytes) * frame +
                    long(sampleRow * 2) * rowIdx;
                if (std::fseek(f_, off, SEEK_SET) != 0) continue;
                if (std::fread(row.data(), 1, row.size(), f_) != row.size()) continue;
                for (size_t i = 0; i + 1 < row.size(); i += 2) {
                    const int v = det_.dataIsLittleEndian ? (row[i + 1] << 8) | row[i]
                        : (row[i] << 8) | row[i + 1];
                    maxVal = std::max(maxVal, v);
                }
            }
        }

        if (maxVal <= 0) return;
        int bits = 1;
        while (bits < 16 && (1 << bits) - 1 < maxVal) ++bits;

        // Only override downward, and only by a whole number of bits. Claiming
        // fewer bits than the data uses would clip; claiming more merely wastes
        // range, so a wrong guess in that direction is the safe one.
        if (bits < h_.pixelDepthPerPlane) {
            det_.detectedSignificantBits = bits;
            det_.depthOverridden = true;
        }
        else {
            det_.detectedSignificantBits = h_.pixelDepthPerPlane;
        }
    }

    void SerReader::loadTimestamps() {
        const long trailerOff = long(kHeaderBytes) +
            long(h_.frameBytes) * h_.frameCount;
        if (std::fseek(f_, 0, SEEK_END) != 0) return;
        const long size = std::ftell(f_);
        const long need = long(sizeof(int64_t)) * h_.frameCount;
        if (size < trailerOff + need) return;   // no trailer: legal, just absent

        stamps_.resize(size_t(h_.frameCount));
        if (std::fseek(f_, trailerOff, SEEK_SET) != 0) { stamps_.clear(); return; }
        if (std::fread(stamps_.data(), sizeof(int64_t), stamps_.size(), f_) !=
            stamps_.size()) {
            stamps_.clear();
            return;
        }
        det_.haveTimestamps = true;

        // Median interval, used to pace playback at the original cadence and as a
        // fallback when a frame's stamp is missing.
        if (stamps_.size() > 2) {
            std::vector<double> d;
            d.reserve(stamps_.size() - 1);
            for (size_t i = 1; i < stamps_.size(); ++i)
                d.push_back(double(stamps_[i] - stamps_[i - 1]) / 10000.0);   // ticks -> ms
            std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
            det_.medianIntervalMs = d[d.size() / 2];
        }
    }

    bool SerReader::readFrame(uint64_t index, uint8_t* dst, std::string& err) {
        if (!f_) { err = "no file open"; return false; }
        if (index >= uint64_t(h_.frameCount)) { err = "frame index past end"; return false; }

        const long off = long(kHeaderBytes) + long(h_.frameBytes) * long(index);
        if (std::fseek(f_, off, SEEK_SET) != 0) { err = "seek failed"; return false; }
        if (std::fread(dst, 1, h_.frameBytes, f_) != h_.frameBytes) {
            err = "short read -- file may be truncated";
            return false;
        }

        // Normalise to host order. Everything downstream assumes little-endian
        // 16-bit samples, so a big-endian file is swapped once here rather than
        // being special-cased in the centroider and the display.
        if (h_.bytesPerSample == 2 && !det_.dataIsLittleEndian) {
            for (size_t i = 0; i + 1 < h_.frameBytes; i += 2)
                std::swap(dst[i], dst[i + 1]);
        }
        return true;
    }

    double SerReader::frameTimeUnix(uint64_t index) const {
        if (det_.haveTimestamps&& index < stamps_.size()) {
            return double(stamps_[index] - kTicksAtUnixEpoch) / 1e7;
        }
        const double base = double(h_.dateTimeUtcTicks - kTicksAtUnixEpoch) / 1e7;
        const double step = det_.medianIntervalMs > 0.0 ? det_.medianIntervalMs / 1000.0 : 0.0;
        return base + step * double(index);
    }

    PixelFormat SerReader::pixelFormat() const {
        const bool wide = h_.bytesPerSample == 2;
        switch (h_.colorId) {
        case SerColorId::RGB:
        case SerColorId::BGR:
            return PixelFormat::RGB24;
        case SerColorId::Mono:
            return wide ? PixelFormat::Mono16 : PixelFormat::Mono8;
        default:
            return wide ? PixelFormat::Bayer16 : PixelFormat::Bayer8;
        }
    }

    BayerPattern SerReader::bayerPattern() const {
        switch (h_.colorId) {
        case SerColorId::BayerRGGB: return BayerPattern::RGGB;
        case SerColorId::BayerGRBG: return BayerPattern::GRBG;
        case SerColorId::BayerGBRG: return BayerPattern::GBRG;
        case SerColorId::BayerBGGR: return BayerPattern::BGGR;
        default:                    return BayerPattern::None;
        }
    }

} // namespace mei