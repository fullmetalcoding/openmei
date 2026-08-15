#include "camera/stream.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>

namespace mei {

    bool StreamController::start(ICamera* cam, std::string& err) {
        if (running_) return true;
        if (!cam) { err = "no camera"; return false; }
        if (!cam->start(err)) return false;

        cam_ = cam;
        stats_ = StreamStats{};
        running_ = true;
        th_ = std::thread(&StreamController::threadMain, this);
        return true;
    }

    void StreamController::stop() {
        if (!running_) return;
        running_ = false;
        if (th_.joinable()) th_.join();

        if (cam_) {
            {
                std::lock_guard<std::mutex> lk(m_);
                if (hasPending_) { cam_->recycle(std::move(pending_)); hasPending_ = false; }
            }
            cam_->stop();
        }
        cam_ = nullptr;
    }

    void StreamController::threadMain() {
        // ZWO's own samples use exposure*2 + 500 ms. Too short gives spurious
        // timeouts on long subs; too long stalls shutdown.
        auto timeoutFor = [this] {
            const int64_t us = cam_->config().exposureUs;
            return static_cast<int>(std::clamp<int64_t>(us / 1000 * 2 + 500, 100, 5000));
            };

        int64_t  lastNs = static_cast<int64_t>(SDL_GetTicksNS());
        int64_t  windowNs = 0;
        uint64_t windowFrames = 0;
        uint64_t windowBytes = 0;

        while (running_) {
            Frame f;
            std::string err;
            if (!cam_->nextFrame(f, timeoutFor(), err)) {
                if (!err.empty()) {
                    std::lock_guard<std::mutex> lk(m_);
                    stats_.lastError = err;
                }
                continue;   // a bare timeout is normal, not an error
            }

            const int64_t now = static_cast<int64_t>(SDL_GetTicksNS());
            windowNs += now - lastNs;
            lastNs = now;
            ++windowFrames;
            windowBytes += f.byteCount();

            std::lock_guard<std::mutex> lk(m_);
            ++stats_.framesGrabbed;

            // Single-slot mailbox: the display only ever wants the newest frame.
            // The analysis stage will need EVERY frame -- that gets its own queue
            // rather than sharing this one, because dropping frames for display is
            // correct and dropping them for measurement is not.
            if (hasPending_) {
                cam_->recycle(std::move(pending_));
                ++stats_.framesSkipped;
            }
            pending_ = std::move(f);
            hasPending_ = true;

            if (windowNs > 500'000'000) {          // recompute fps twice a second
                stats_.fps = double(windowFrames) * 1e9 / double(windowNs);
                stats_.measuredIntervalMs = double(windowNs) / double(windowFrames) / 1e6;
                stats_.mbPerSec = double(windowBytes) * 1e9 / double(windowNs) / 1048576.0;
                // With host-side binning the sensor still reads out and transfers
                // bin^2 times as many pixels as we receive, so the delivered rate
                // understates the real link load by that factor.
                const int    b = cam_->config().bin;
                const double mult = cam_->caps().binningIsSoftware && b > 1
                    ? double(b) * double(b) : 1.0;
                stats_.linkMbPerSec = stats_.mbPerSec * mult;
                stats_.frameBytes = size_t(double(windowBytes) / double(windowFrames));
                windowNs = 0;
                windowFrames = 0;
                windowBytes = 0;
                stats_.sdkDropped = cam_->droppedFrames();
            }
        }
    }

    bool StreamController::withLatest(const std::function<void(const Frame&)>& fn) {
        std::lock_guard<std::mutex> lk(m_);
        if (!hasPending_ || !cam_) return false;
        fn(pending_);
        cam_->recycle(std::move(pending_));
        hasPending_ = false;
        ++stats_.framesDisplayed;
        return true;
    }

    StreamStats StreamController::stats() const {
        std::lock_guard<std::mutex> lk(m_);
        return stats_;
    }

} // namespace mei