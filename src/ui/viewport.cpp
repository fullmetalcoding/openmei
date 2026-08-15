#include "ui/viewport.h"

#include <GL/glew.h>
#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace mei::ui {

Viewport::~Viewport() { release(); }

void Viewport::release() {
    if (tex_) { GLuint t = tex_; glDeleteTextures(1, &t); tex_ = 0; }
    w_ = h_ = 0;
}

void Viewport::upload(const Frame& f) {
    const FrameMeta& m = f.meta;
    if (m.width <= 0 || m.height <= 0) return;

    // Raw CFA data: sum 2x2 blocks so the mosaic texture cancels rather than
    // showing up as a grid. Produces a half-size mono image.
    sigBits_ = m.significantBits;
    texPerFrame_ = 1.0f;
    const bool doSum = sumBayer2x2 && m.bayer != BayerPattern::None &&
                       m.width >= 2 && m.height >= 2;
    if (doSum) { texPerFrame_ = 0.5f; uploadBayerSummed(f); return; }

    const size_t px = static_cast<size_t>(m.width) * m.height;
    scratch_.resize(px);

    const int bpp = bytesPerPixel(m.format);
    int lo = 0, hi = 255;

    if (bpp == 2) {
        // Undo the container shift to get raw ADU. A 12-bit ZWO frame arrives
        // left-shifted by 4; without this the histogram is combed and the
        // stretch is wrong.
        const int shift = m.sampleShift;
        const uint16_t* src = reinterpret_cast<const uint16_t*>(f.pixels.data());

        int mn = 65535, mx = 0;
        for (size_t i = 0; i < px; ++i) {
            const int v = src[i] >> shift;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        lo = mn; hi = std::max(mx, mn + 1);
        const int full = (1 << m.significantBits) - 1;
        saturated_ = (mx >= full);

        if (!autoStretch) {
            lo = int(blackPoint * full);
            hi = std::max(int(whitePoint * full), lo + 1);
        }
        const float scale = 255.0f / float(hi - lo);
        for (size_t i = 0; i < px; ++i) {
            const int v = (src[i] >> shift) - lo;
            scratch_[i] = static_cast<uint8_t>(std::clamp(int(v * scale), 0, 255));
        }
    } else if (bpp == 1) {
        const uint8_t* src = f.pixels.data();
        int mn = 255, mx = 0;
        for (size_t i = 0; i < px; ++i) { mn = std::min<int>(mn, src[i]); mx = std::max<int>(mx, src[i]); }
        lo = mn; hi = std::max(mx, mn + 1);
        saturated_ = (mx >= 255);

        if (!autoStretch) {
            lo = int(blackPoint * 255.0f);
            hi = std::max(int(whitePoint * 255.0f), lo + 1);
        }
        const float scale = 255.0f / float(hi - lo);
        for (size_t i = 0; i < px; ++i)
            scratch_[i] = static_cast<uint8_t>(std::clamp(int((src[i] - lo) * scale), 0, 255));
    } else {
        return;   // RGB24 not needed for a DIMM; add if a colour source appears
    }

    lastMin_ = lo;
    lastMax_ = hi;
    uploadGray(m.width, m.height);
}

void Viewport::uploadGray(int width, int height) {
    if (!tex_ || w_ != width || h_ != height) {
        release();
        GLuint t = 0;
        glGenTextures(1, &t);
        tex_ = t;
        glBindTexture(GL_TEXTURE_2D, t);
        // Nearest, not linear: at pixel-level zoom you want to see actual
        // samples, not an interpolated guess about where the spot centre is.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Single channel broadcast to RGB via swizzle -- GL_LUMINANCE is gone
        // from core profiles, and this avoids expanding to RGBA on the CPU.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0,
                     GL_RED, GL_UNSIGNED_BYTE, nullptr);
        w_ = width; h_ = height;
    } else {
        glBindTexture(GL_TEXTURE_2D, tex_);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   // rows are not 4-byte aligned
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RED, GL_UNSIGNED_BYTE, scratch_.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Viewport::uploadBayerSummed(const Frame& f) {
    const FrameMeta& m = f.meta;
    const int W = m.width / 2, H = m.height / 2;
    const int bpp = bytesPerPixel(m.format);

    std::vector<int> sums(static_cast<size_t>(W) * H);
    int mn = INT32_MAX, mx = 0;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int s = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const size_t i = size_t(y * 2 + dy) * m.width + (x * 2 + dx);
                    if (bpp == 2) {
                        uint16_t v;
                        std::memcpy(&v, &f.pixels[i * 2], 2);
                        s += (v >> m.sampleShift);
                    } else {
                        s += f.pixels[i];
                    }
                }
            }
            sums[size_t(y) * W + x] = s;
            mn = std::min(mn, s);
            mx = std::max(mx, s);
        }
    }

    const int full = ((bpp == 2) ? ((1 << m.significantBits) - 1) : 255) * 4;
    saturated_ = (mx >= full);

    int lo = mn, hi = std::max(mx, mn + 1);
    if (!autoStretch) {
        lo = int(blackPoint * full);
        hi = std::max(int(whitePoint * full), lo + 1);
    }
    lastMin_ = lo;
    lastMax_ = hi;

    scratch_.resize(static_cast<size_t>(W) * H);
    const float scale = 255.0f / float(hi - lo);
    for (size_t i = 0; i < sums.size(); ++i)
        scratch_[i] = static_cast<uint8_t>(std::clamp(int((sums[i] - lo) * scale), 0, 255));

    uploadGray(W, H);
}

bool Viewport::mapFrameToScreen(double fx, double fy, ImVec2& out) const {
    if (!drawn_) return false;
    out.x = lastOrigin_.x + float(fx) * texPerFrame_ * lastScale_;
    out.y = lastOrigin_.y + float(fy) * texPerFrame_ * lastScale_;
    return true;
}

bool Viewport::draw(const ImVec2& avail) {
    drawn_ = false;
    if (!tex_ || w_ <= 0 || h_ <= 0) return false;

    const float sx = avail.x / float(w_);
    const float sy = avail.y / float(h_);
    const float s  = std::min(sx, sy);
    const ImVec2 size(float(w_) * s, float(h_) * s);

    // Centre it.
    const ImVec2 cur = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(cur.x + (avail.x - size.x) * 0.5f,
                               cur.y + (avail.y - size.y) * 0.5f));

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Image(static_cast<ImTextureID>(static_cast<intptr_t>(tex_)), size);

    lastOrigin_ = origin;
    lastScale_  = s;
    drawn_      = true;

    if (showCrosshair) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = IM_COL32(90, 160, 220, 90);
        const ImVec2 c(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);
        dl->AddLine(ImVec2(origin.x, c.y), ImVec2(origin.x + size.x, c.y), col);
        dl->AddLine(ImVec2(c.x, origin.y), ImVec2(c.x, origin.y + size.y), col);
    }
    return true;
}

} // namespace mei::ui
