#include "net/alpaca_server.h"

#include "app/uuid.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <cstring>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#endif

namespace mei {

    namespace {

        // ASCOM error numbers. 0x400-0x4FF are the standard exceptions; 0x500-0xFFF are
        // available for driver-specific ones.
        constexpr int kErrNotImplemented = 0x400;
        constexpr int kErrInvalidValue = 0x401;
        constexpr int kErrValueNotSet = 0x402;
        constexpr int kErrNotConnected = 0x407;
        constexpr int kErrActionNotImplemented = 0x40C;

        constexpr int kDiscoveryPort = 32227;

        std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return char(::tolower(c)); });
            return s;
        }

        std::string jsonEscape(const std::string& s) {
            std::string o;
            o.reserve(s.size() + 8);
            for (char c : s) {
                switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", c);
                        o += b;
                    }
                    else {
                        o += c;
                    }
                }
            }
            return o;
        }

        std::string num(double v) {
            if (!std::isfinite(v)) return "0";
            std::ostringstream os;
            os.precision(10);
            os << v;
            return os.str();
        }

        int64_t nowMs() {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        std::string isoUtcNow() {
            const auto t = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char b[32];
            std::strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return b;
        }

        // Alpaca parameter names are case-insensitive, and clients in the wild really
        // do send ClientID, clientid and ClientId. Exact matching silently drops
        // transaction IDs and makes some clients log errors.
        const std::string* findParam(const httplib::Request& req, const std::string& want) {
            const std::string w = lower(want);
            for (const auto& kv : req.params)
                if (lower(kv.first) == w) return &kv.second;
            return nullptr;
        }

        uint32_t clientTxn(const httplib::Request& req) {
            if (const std::string* p = findParam(req, "ClientTransactionID")) {
                try { return static_cast<uint32_t>(std::stoul(*p)); }
                catch (...) {}
            }
            return 0;
        }

    } // namespace

    // ---------------------------------------------------------------------------

    AlpacaServer::AlpacaServer() = default;

    AlpacaServer::~AlpacaServer() { stop(); }

    void AlpacaServer::publish(const SeeingResult& r, const DimmConfig& c) {
        std::lock_guard<std::mutex> lk(m_);
        snap_.valid = r.valid;
        snap_.result = r;
        snap_.cfg = c;
        snap_.stampMs = nowMs();
        ++stats_.publishedBursts;
    }

    AlpacaStats AlpacaServer::stats() const {
        std::lock_guard<std::mutex> lk(m_);
        AlpacaStats s = stats_;
        s.running = running_.load();
        s.port = cfg_.port;
        s.clientConnected = connected_.load();
        s.lastPublishAgeS = snap_.valid
            ? double(nowMs() - snap_.stampMs) / 1000.0 : 0.0;
        return s;
    }

    bool AlpacaServer::start(const AlpacaConfig& c, std::string& err) {
        if (running_) return true;
        cfg_ = c;
        if (cfg_.uniqueId.empty()) cfg_.uniqueId = generateUuid();

        srv_ = std::make_unique<httplib::Server>();
        installRoutes();

        running_ = true;
        httpThread_ = std::thread([this] {
            if (!srv_->listen("0.0.0.0", cfg_.port)) {
                std::lock_guard<std::mutex> lk(m_);
                stats_.lastError = "listen failed on port " + std::to_string(cfg_.port) +
                    " (already in use?)";
                running_ = false;
            }
            });

        // Give listen() a moment to fail loudly rather than reporting success on a
        // port that was never bound.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        if (!running_) {
            std::lock_guard<std::mutex> lk(m_);
            err = stats_.lastError;
            if (httpThread_.joinable()) httpThread_.join();
            srv_.reset();
            return false;
        }

        if (cfg_.discovery) {
            discoveryRun_ = true;
            discoveryThread_ = std::thread(&AlpacaServer::discoveryLoop, this);
        }
        return true;
    }

    void AlpacaServer::stop() {
        discoveryRun_ = false;
        if (srv_) srv_->stop();
        running_ = false;
        if (httpThread_.joinable())      httpThread_.join();
        if (discoveryThread_.joinable()) discoveryThread_.join();
        srv_.reset();
    }

    // ---------------------------------------------------------------------------

    void AlpacaServer::installRoutes() {
        // Response envelope. Every Alpaca reply carries it, success or failure.
        auto envelope = [this](const std::string& valueJson, uint32_t ctid,
            int errNum, const std::string& errMsg) {
                std::ostringstream os;
                os << "{";
                if (!valueJson.empty()) os << "\"Value\":" << valueJson << ",";
                os << "\"ClientTransactionID\":" << ctid
                    << ",\"ServerTransactionID\":" << ++serverTxn_
                    << ",\"ErrorNumber\":" << errNum
                    << ",\"ErrorMessage\":\"" << jsonEscape(errMsg) << "\"}";
                return os.str();
            };

        auto ok = [this, envelope](const httplib::Request& req, httplib::Response& res,
            const std::string& valueJson) {
                res.status = 200;
                res.set_content(envelope(valueJson, clientTxn(req), 0, ""), "application/json");
            };

        // Device-level failure. Still HTTP 200: the device understood the request
        // and simply cannot action it, which Alpaca signals in the body. Returning
        // 4xx here confuses conformant clients.
        auto deviceError = [this, envelope](const httplib::Request& req,
            httplib::Response& res,
            int errNum, const std::string& msg) {
                res.status = 200;
                res.set_content(envelope("", clientTxn(req), errNum, msg), "application/json");
            };

        // Request-level failure: malformed URL, bad device number, unparseable
        // parameter. These are 400 with a PLAIN TEXT body, not the JSON envelope --
        // clients must branch on status before attempting to parse.
        auto badRequest = [](httplib::Response& res, const std::string& msg) {
            res.status = 400;
            res.set_content(msg, "text/plain");
            };

        auto countRequest = [this](const httplib::Request& req) {
            std::lock_guard<std::mutex> lk(m_);
            ++stats_.requests;
            stats_.lastClient = req.remote_addr;
            };

        auto snapshot = [this] {
            std::lock_guard<std::mutex> lk(m_);
            return snap_;
            };

        // --- management API ------------------------------------------------------
        // Required for discovery to resolve to an actual device; without it a
        // client finds the port and then has nothing to talk to.
        srv_->Get("/management/apiversions",
            [=](const httplib::Request& req, httplib::Response& res) {
                countRequest(req);
                ok(req, res, "[1]");
            });

        srv_->Get("/management/v1/description",
            [=](const httplib::Request& req, httplib::Response& res) {
                countRequest(req);
                std::ostringstream v;
                v << "{\"ServerName\":\"" << jsonEscape(cfg_.serverName) << "\""
                    << ",\"Manufacturer\":\"OpenMEI\""
                    << ",\"ManufacturerVersion\":\"0.1.0\""
                    << ",\"Location\":\"" << jsonEscape(cfg_.location) << "\"}";
                ok(req, res, v.str());
            });

        srv_->Get("/management/v1/configureddevices",
            [=](const httplib::Request& req, httplib::Response& res) {
                countRequest(req);
                std::ostringstream v;
                v << "[{\"DeviceName\":\"OpenMEI Seeing Monitor\""
                    << ",\"DeviceType\":\"ObservingConditions\""
                    << ",\"DeviceNumber\":0"
                    << ",\"UniqueID\":\"" << jsonEscape(cfg_.uniqueId) << "\"}]";
                ok(req, res, v.str());
            });

        // --- setup page ----------------------------------------------------------
        srv_->Get("/setup", [=](const httplib::Request& req, httplib::Response& res) {
            countRequest(req);
            const Snapshot s = snapshot();
            std::ostringstream h;
            h << "<html><head><title>OpenMEI</title></head><body>"
                << "<h2>OpenMEI Seeing Monitor</h2>"
                << "<p>ASCOM Alpaca ObservingConditions, device 0.</p><ul>";
            if (s.valid) {
                h << "<li>StarFWHM (zenith, 500nm): " << num(s.result.fwhmZenithArcsec)
                    << " arcsec</li>"
                    << "<li>r0: " << num(s.result.r0Zenith * 1000.0) << " mm</li>"
                    << "<li>Frames: " << s.result.framesUsed << "</li>";
            }
            else {
                h << "<li>No valid measurement yet.</li>";
            }
            h << "</ul><p>Configuration is set in the OpenMEI application, not here."
                << "</p></body></html>";
            res.set_content(h.str(), "text/html");
            });

        // --- device API ----------------------------------------------------------
        // One handler per verb, dispatched internally, because Alpaca URLs are
        // case-insensitive and registering fixed lowercase paths would reject a
        // client that capitalises them.
        auto handleGet = [=](const httplib::Request& req, httplib::Response& res) {
            countRequest(req);

            const std::string path = lower(req.matches.size() > 1 ? req.matches[1].str() : "");
            // Expect: observingconditions/<n>/<property>
            const size_t s1 = path.find('/');
            const size_t s2 = path.find('/', s1 == std::string::npos ? 0 : s1 + 1);
            if (s1 == std::string::npos || s2 == std::string::npos) {
                badRequest(res, "Malformed device URL");
                return;
            }
            const std::string devType = path.substr(0, s1);
            const std::string devNum = path.substr(s1 + 1, s2 - s1 - 1);
            const std::string prop = path.substr(s2 + 1);

            if (devType != "observingconditions") {
                badRequest(res, "Unsupported device type: " + devType);
                return;
            }
            if (devNum != "0") {
                badRequest(res, "Invalid device number: " + devNum);
                return;
            }

            // --- common members, valid whether or not Connected -------------------
            if (prop == "connected") { ok(req, res, connected_ ? "true" : "false"); return; }
            if (prop == "connecting") { ok(req, res, "false"); return; }
            if (prop == "name") { ok(req, res, "\"OpenMEI Seeing Monitor\""); return; }
            if (prop == "description") { ok(req, res, "\"Differential Image Motion Monitor\""); return; }
            if (prop == "driverinfo") { ok(req, res, "\"OpenMEI (Motion Estimating Imager) Alpaca bridge\""); return; }
            if (prop == "driverversion") { ok(req, res, "\"0.1\""); return; }
            if (prop == "interfaceversion") { ok(req, res, "2"); return; }
            if (prop == "supportedactions") {
                ok(req, res, "[\"OpenMEI:GetMeasurement\",\"OpenMEI:GetCalibration\"]");
                return;
            }

            const Snapshot snap = snapshot();

            if (prop == "devicestate") {
                // Platform 7 batch read. One round trip instead of one per
                // property, which is why it was added.
                std::ostringstream v;
                v << "[";
                if (snap.valid) {
                    v << "{\"Name\":\"StarFWHM\",\"Value\":"
                        << num(snap.result.fwhmZenithArcsec) << "},";
                }
                v << "{\"Name\":\"TimeStamp\",\"Value\":\"" << isoUtcNow() << "\"}]";
                ok(req, res, v.str());
                return;
            }

            if (prop == "averageperiod") { ok(req, res, num(averagePeriodHours_)); return; }

            // --- operational members require a connection -------------------------
            if (!connected_) { deviceError(req, res, kErrNotConnected, "Not connected"); return; }

            if (prop == "starfwhm") {
                if (!snap.valid) {
                    // ValueNotSet rather than zero: a client must be able to tell
                    // "no measurement" from "perfect seeing".
                    deviceError(req, res, kErrValueNotSet,
                        "No valid seeing measurement available");
                    return;
                }
                // Zenith-corrected at 500 nm, per DIMM convention. The line-of-sight
                // value and the science-target projection are available through the
                // custom action.
                ok(req, res, num(snap.result.fwhmZenithArcsec));
                return;
            }

            if (prop == "timesincelastupdate") {
                if (!snap.valid) { deviceError(req, res, kErrValueNotSet, "No measurement yet"); return; }
                ok(req, res, num(double(nowMs() - snap.stampMs) / 1000.0));
                return;
            }

            if (prop == "sensordescription") {
                const std::string* p = findParam(req, "SensorName");
                if (!p) { badRequest(res, "SensorName parameter is required"); return; }
                if (lower(*p) == "starfwhm") {
                    ok(req, res, "\"DIMM: zenith-corrected seeing FWHM at 500nm\"");
                }
                else {
                    deviceError(req, res, kErrNotImplemented,
                        "Sensor '" + *p + "' is not implemented");
                }
                return;
            }

            // Everything else in the interface. Reporting NotImplemented rather
            // than a plausible zero matters: a driver that answers 0 for CloudCover
            // and Humidity will get an observatory roof opened in the rain.
            static const char* kUnimplemented[] = {
                "cloudcover", "dewpoint", "humidity", "pressure", "rainrate",
                "skybrightness", "skyquality", "skytemperature", "temperature",
                "winddirection", "windgust", "windspeed"
            };
            for (const char* u : kUnimplemented) {
                if (prop == u) {
                    deviceError(req, res, kErrNotImplemented,
                        std::string(u) + " is not measured by this device");
                    return;
                }
            }

            badRequest(res, "Unknown property: " + prop);
            };

        auto handlePut = [=](const httplib::Request& req, httplib::Response& res) {
            countRequest(req);

            const std::string path = lower(req.matches.size() > 1 ? req.matches[1].str() : "");
            const size_t s1 = path.find('/');
            const size_t s2 = path.find('/', s1 == std::string::npos ? 0 : s1 + 1);
            if (s1 == std::string::npos || s2 == std::string::npos) {
                badRequest(res, "Malformed device URL");
                return;
            }
            if (path.substr(0, s1) != "observingconditions") {
                badRequest(res, "Unsupported device type");
                return;
            }
            if (path.substr(s1 + 1, s2 - s1 - 1) != "0") {
                badRequest(res, "Invalid device number");
                return;
            }
            const std::string prop = path.substr(s2 + 1);

            if (prop == "connected") {
                const std::string* p = findParam(req, "Connected");
                if (!p) { badRequest(res, "Connected parameter is required"); return; }
                const char c0 = p->empty() ? '\0' : char(::tolower((*p)[0]));
                if (c0 != 't' && c0 != 'f') { badRequest(res, "Connected must be true or false"); return; }
                connected_ = (c0 == 't');
                ok(req, res, "");
                return;
            }
            if (prop == "connect") { connected_ = true;  ok(req, res, ""); return; }
            if (prop == "disconnect") { connected_ = false; ok(req, res, ""); return; }

            if (prop == "averageperiod") {
                const std::string* p = findParam(req, "AveragePeriod");
                if (!p) { badRequest(res, "AveragePeriod parameter is required"); return; }
                double v = 0.0;
                try { v = std::stod(*p); }
                catch (...) { badRequest(res, "AveragePeriod is not a number"); return; }
                if (v < 0.0) {
                    deviceError(req, res, kErrInvalidValue, "AveragePeriod must be >= 0");
                    return;
                }
                averagePeriodHours_ = v;
                ok(req, res, "");
                return;
            }

            if (prop == "refresh") { ok(req, res, ""); return; }   // updates continuously

            if (prop == "action") {
                const std::string* a = findParam(req, "Action");
                if (!a) { badRequest(res, "Action parameter is required"); return; }
                const Snapshot snap = snapshot();
                const std::string act = lower(*a);

                if (act == "openmei:getmeasurement") {
                    // ObservingConditions carries a single scalar, so everything
                    // else goes through Action -- the mechanism the interface
                    // documentation names for exactly this. Bundled into one call
                    // so a client cannot stitch together fields from different
                    // bursts.
                    std::ostringstream j;
                    j << "{"
                        << "\"valid\":" << (snap.valid ? "true" : "false")
                        << ",\"fwhm_zenith_500nm\":" << num(snap.result.fwhmZenithArcsec)
                        << ",\"fwhm_line_of_sight\":" << num(snap.result.fwhmArcsec)
                        << ",\"fwhm_at_science_target\":" << num(snap.result.fwhmAtScienceArcsec)
                        << ",\"sigma_arcsec\":" << num(snap.result.sigmaArcsec)
                        << ",\"r0_zenith_m\":" << num(snap.result.r0Zenith)
                        << ",\"var_long_arcsec2\":" << num(snap.result.varLongRad2 * 206264.806 * 206264.806)
                        << ",\"var_tran_arcsec2\":" << num(snap.result.varTranRad2 * 206264.806 * 206264.806)
                        << ",\"fwhm_long\":" << num(snap.result.fwhmLongArcsec)
                        << ",\"fwhm_tran\":" << num(snap.result.fwhmTranArcsec)
                        << ",\"axis_agreement\":" << num(snap.result.axisAgreementRatio)
                        << ",\"exposure_correction\":" << num(snap.result.exposureCorrectionFactor)
                        << ",\"airmass\":" << num(snap.result.airmassUsed)
                        << ",\"science_airmass\":" << num(snap.result.scienceAirmass)
                        << ",\"frames_used\":" << snap.result.framesUsed
                        << ",\"frames_rejected\":" << snap.result.framesRejected
                        << ",\"age_seconds\":" << num(double(nowMs() - snap.stampMs) / 1000.0)
                        << "}";
                    // Action returns a string, so the payload is escaped inside one.
                    ok(req, res, "\"" + jsonEscape(j.str()) + "\"");
                    return;
                }

                if (act == "openmei:getcalibration") {
                    // Provenance. For a dataset other people will cite, being able
                    // to reconstruct how a number was produced matters as much as
                    // the number.
                    std::ostringstream j;
                    j << "{"
                        << "\"arcsec_per_pixel\":" << num(snap.cfg.calibration.arcsecPerPixel)
                        << ",\"scale_method\":\"" << toString(snap.cfg.calibration.method) << "\""
                        << ",\"calibrated_utc\":\"" << jsonEscape(snap.cfg.calibration.calibratedUtc) << "\""
                        << ",\"fit_residual_px\":" << num(snap.cfg.calibration.fitResidualPx)
                        << ",\"sub_aperture_mm\":" << num(snap.cfg.optics.subApertureMm)
                        << ",\"baseline_mm\":" << num(snap.cfg.optics.baselineMm)
                        << ",\"d_over_D\":" << num(snap.cfg.optics.b())
                        << ",\"wedge_orientation\":\"" << toString(snap.cfg.optics.wedgeOrientation) << "\""
                        << ",\"coefficients\":\"" << toString(snap.cfg.reporting.coefficients) << "\""
                        << ",\"exposure_us\":" << snap.cfg.burst.baseExposureUs
                        << ",\"frames_per_burst\":" << snap.cfg.burst.framesPerBurst
                        << "}";
                    ok(req, res, "\"" + jsonEscape(j.str()) + "\"");
                    return;
                }

                deviceError(req, res, kErrActionNotImplemented, "Unknown action: " + *a);
                return;
            }

            if (prop == "commandblind" || prop == "commandbool" || prop == "commandstring") {
                deviceError(req, res, kErrNotImplemented,
                    "This device has no pass-through command interface");
                return;
            }

            badRequest(res, "Unknown or read-only property: " + prop);
            };

        srv_->Get(R"(/api/v1/(.+))", handleGet);
        srv_->Put(R"(/api/v1/(.+))", handlePut);

        // Anything unexpected in the driver itself becomes a 500 with a plain-text
        // body, per the spec, rather than an unhandled exception killing a thread.
        srv_->set_exception_handler([](const httplib::Request&, httplib::Response& res,
            std::exception_ptr) {
                res.status = 500;
                res.set_content("Internal device error", "text/plain");
            });
    }

    // ---------------------------------------------------------------------------

    namespace {

        // Alpaca discovery: clients broadcast this exact string to UDP 32227 and every
        // device on the network answers with the port its HTTP API is on. Without it a
        // user has to type in an address by hand, which is where most people give up.
        constexpr const char* kDiscoveryProbe = "alpacadiscovery1";
        constexpr size_t      kProbeLen = 16;

        // IPv6 discovery uses a multicast group rather than broadcast.
        constexpr const char* kIpv6Group = "ff12::a1:9aca";

        void closeSock(int s) {
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
        }

        void setReuse(int s) {
            int yes = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef SO_REUSEPORT
            // Required on Linux and macOS for several processes to RECEIVE on the same
            // UDP port. SO_REUSEADDR alone permits the bind but delivers the datagram
            // to only one of them -- and sharing a machine with ASCOM Remote or another
            // Alpaca driver is the normal case, not an exotic one.
            setsockopt(s, SOL_SOCKET, SO_REUSEPORT,
                reinterpret_cast<const char*>(&yes), sizeof(yes));
#endif
        }

        void setRecvTimeout(int s, int ms) {
            // Short timeout so the thread notices a stop request instead of blocking
            // until the next broadcast happens to arrive.
#ifdef _WIN32
            DWORD tv = DWORD(ms);
#else
            timeval tv{ ms / 1000, (ms % 1000) * 1000 };
#endif
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&tv), sizeof(tv));
        }

    } // namespace

    void AlpacaServer::discoveryLoop() {
#ifdef _WIN32
        WSADATA wsa;
        const bool wsaOk = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif

        const std::string reply = "{\"AlpacaPort\":" + std::to_string(cfg_.port) + "}";

        // --- IPv4 broadcast listener --------------------------------------------
        int s4 = static_cast<int>(socket(AF_INET, SOCK_DGRAM, 0));
        if (s4 >= 0) {
            setReuse(s4);
            setRecvTimeout(s4, 300);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = INADDR_ANY;
            a.sin_port = htons(kDiscoveryPort);
            if (bind(s4, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
                std::lock_guard<std::mutex> lk(m_);
                stats_.lastError = "discovery: could not bind UDP " +
                    std::to_string(kDiscoveryPort) +
                    " (another Alpaca device may hold it without "
                    "SO_REUSEPORT)";
                closeSock(s4);
                s4 = -1;
            }
        }

        // --- IPv6 multicast listener, best effort --------------------------------
        // Failure here is not fatal: essentially every amateur setup is IPv4, and a
        // network without IPv6 should not cost us IPv4 discovery.
        int s6 = static_cast<int>(socket(AF_INET6, SOCK_DGRAM, 0));
        if (s6 >= 0) {
            setReuse(s6);
            setRecvTimeout(s6, 300);
            int v6only = 1;
            setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY,
                reinterpret_cast<const char*>(&v6only), sizeof(v6only));

            sockaddr_in6 a6{};
            a6.sin6_family = AF_INET6;
            a6.sin6_addr = in6addr_any;
            a6.sin6_port = htons(kDiscoveryPort);
            if (bind(s6, reinterpret_cast<sockaddr*>(&a6), sizeof(a6)) == 0) {
                ipv6_mreq mreq{};
                if (inet_pton(AF_INET6, kIpv6Group, &mreq.ipv6mr_multiaddr) == 1) {
                    mreq.ipv6mr_interface = 0;   // default interface
                    setsockopt(s6, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                        reinterpret_cast<const char*>(&mreq), sizeof(mreq));
                }
            }
            else {
                closeSock(s6);
                s6 = -1;
            }
        }

        if (s4 < 0 && s6 < 0) {
#ifdef _WIN32
            if (wsaOk) WSACleanup();
#endif
            return;
        }

        auto serve = [&](int s) {
            char buf[512];
            sockaddr_storage from{};
#ifdef _WIN32
            int fromLen = sizeof(from);
#else
            socklen_t fromLen = sizeof(from);
#endif
            const int n = static_cast<int>(recvfrom(s, buf, sizeof(buf) - 1, 0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLen));
            if (n <= 0) return;
            buf[n] = '\0';
            if (size_t(n) < kProbeLen) return;
            if (std::strncmp(buf, kDiscoveryProbe, kProbeLen) != 0) return;

            sendto(s, reply.c_str(), static_cast<int>(reply.size()), 0,
                reinterpret_cast<sockaddr*>(&from), fromLen);
            std::lock_guard<std::mutex> lk(m_);
            ++stats_.discoveryReplies;
            };

        while (discoveryRun_) {
            // select() rather than one thread per socket: both listeners are idle
            // almost all the time, and the receive timeouts keep shutdown prompt.
            fd_set rfds;
            FD_ZERO(&rfds);
            int maxfd = 0;
            if (s4 >= 0) { FD_SET(s4, &rfds); maxfd = std::max(maxfd, s4); }
            if (s6 >= 0) { FD_SET(s6, &rfds); maxfd = std::max(maxfd, s6); }

            timeval tv{ 0, 300000 };
            const int r = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;

            if (s4 >= 0 && FD_ISSET(s4, &rfds)) serve(s4);
            if (s6 >= 0 && FD_ISSET(s6, &rfds)) serve(s6);
        }

        if (s4 >= 0) closeSock(s4);
        if (s6 >= 0) closeSock(s6);
#ifdef _WIN32
        if (wsaOk) WSACleanup();
#endif
    }

} // namespace mei