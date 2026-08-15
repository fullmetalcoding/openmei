#include "camera/camera.h"

#include <chrono>

namespace mei {


    void FramePool::reset(size_t count, size_t bytesEach) {
        std::lock_guard<std::mutex> lk(m_);
        free_ = {};
        ready_ = {};
        bytesEach_ = bytesEach;
        for (size_t i = 0; i < count; ++i) {
            Frame f;
            f.pixels.resize(bytesEach);
            free_.push(std::move(f));
        }
    }

    bool FramePool::acquire(Frame& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (free_.empty()) return false;
        out = std::move(free_.front());
        free_.pop();
        if (out.pixels.size() != bytesEach_) out.pixels.resize(bytesEach_);
        return true;
    }

    void FramePool::recycle(Frame&& f) {
        std::lock_guard<std::mutex> lk(m_);
        f.meta = FrameMeta{};
        free_.push(std::move(f));
    }

    void FramePool::publish(Frame&& f) {
        {
            std::lock_guard<std::mutex> lk(m_);
            ready_.push(std::move(f));
        }
        cv_.notify_one();
    }

    bool FramePool::consume(Frame& out, int timeoutMs) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
            [this] { return !ready_.empty(); })) {
            return false;
        }
        out = std::move(ready_.front());
        ready_.pop();
        return true;
    }

    void FramePool::wake() { cv_.notify_all(); }

    size_t FramePool::freeCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return free_.size();
    }

} // namespace mei