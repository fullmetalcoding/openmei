// alpaca_server.h -- ASCOM Alpaca ObservingConditions endpoint.
//
// Publishes seeing as IObservingConditions.StarFWHM, which is documented as
// "Seeing (FWHM in arc-sec) at the observatory". Any client that already speaks
// observing conditions -- N.I.N.A., SGP, ACP, Voyager -- can then consume this
// instrument with no work on their side, which is a far better adoption story
// than asking anyone to implement a bespoke protocol.
//
// Alpaca is RPC over HTTP rather than REST, and the conventions bite:
//   * device-level errors return HTTP 200 with ErrorNumber set in the body
//   * only malformed requests return 400/500, and those bodies are PLAIN TEXT
//   * parameter names are case-insensitive
//   * unimplemented properties must say so, never return a plausible zero

#pragma once

#include "dimm/config.h"
#include "dimm/seeing.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; }

namespace mei {

    struct AlpacaConfig {
        bool        enabled = false;
        int         port = 11111;   // conventional Alpaca default
        bool        discovery = true;    // UDP responder on 32227
        std::string serverName = "OpenMEI";
        std::string location;
        // Stable per installation, not per build. Clients key their saved device
        // selection on this.
        std::string uniqueId = "";
    };

    struct AlpacaStats {
        bool        running = false;
        int         port = 0;
        uint64_t    requests = 0;
        uint64_t    discoveryReplies = 0;
        uint64_t    publishedBursts = 0;
        double      lastPublishAgeS = 0.0;
        bool        clientConnected = false;
        std::string lastClient;
        std::string lastError;
    };

    class AlpacaServer {
    public:
        AlpacaServer();
        ~AlpacaServer();
        AlpacaServer(const AlpacaServer&) = delete;
        AlpacaServer& operator=(const AlpacaServer&) = delete;

        bool start(const AlpacaConfig&, std::string& err);
        void stop();
        bool running() const { return running_.load(); }

        // Called whenever a burst completes. Everything the endpoint reports comes
        // from this snapshot, so a client can never observe a half-updated result.
        void publish(const SeeingResult&, const DimmConfig&);

        AlpacaStats stats() const;

    private:
        void installRoutes();
        void discoveryLoop();

        struct Snapshot {
            bool         valid = false;
            SeeingResult result;
            DimmConfig   cfg;
            int64_t      stampMs = 0;
        };

        std::unique_ptr<httplib::Server> srv_;
        std::thread                      httpThread_;
        std::thread                      discoveryThread_;
        std::atomic<bool>                running_{ false };
        std::atomic<bool>                discoveryRun_{ false };

        AlpacaConfig cfg_;

        mutable std::mutex m_;
        Snapshot           snap_;
        AlpacaStats        stats_;

        // ASCOM clients expect Connected to be a real, settable piece of state.
        std::atomic<bool>     connected_{ false };
        std::atomic<uint32_t> serverTxn_{ 0 };
        std::atomic<double>   averagePeriodHours_{ 0.0 };
    };

} // namespace mei