// stream.h -- grab thread and display tap.

#pragma once

#include "camera/camera.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mei {

    struct StreamStats {
        uint64_t framesGrabbed = 0;
        uint64_t framesDisplayed = 0;
        uint64_t framesSkipped = 0;   // newer frame arrived before UI drew the last
        int      sdkDropped = 0;
        double   fps = 0.0;
        double   mbPerSec = 0.0;   // delivered frame bytes per second
        double   linkMbPerSec = 0.0;   // what actually crosses USB (see below)
        size_t   frameBytes = 0;
        double   measuredIntervalMs = 0.0;
        std::string lastError;
    };

    class StreamController {
    public:
        ~StreamController() { stop(); }

        bool start(ICamera* cam, std::string& err);
        void stop();
        bool running() const { return running_.load(); }

        // Borrow the newest undelivered frame. The callback runs under the mailbox
        // lock and the frame is recycled immediately afterwards, so no ownership
        // escapes -- which means disconnecting mid-draw cannot leave the UI holding
        // a buffer whose pool has been destroyed.
        bool withLatest(const std::function<void(const Frame&)>& fn);

        StreamStats stats() const;

    private:
        void threadMain();

        ICamera* cam_ = nullptr;
        std::thread       th_;
        std::atomic<bool> running_{ false };

        mutable std::mutex m_;
        Frame              pending_;
        bool               hasPending_ = false;
        StreamStats        stats_;
    };

} // namespace mei