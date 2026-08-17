#include "app/settings_store.h"

#include <SDL3/SDL_filesystem.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mei {

    using nlohmann::json;

    namespace {

        // Read with a default rather than throwing on a missing key: a config written
        // by an older build must still load, or every added field becomes a breaking
        // change for anyone who upgrades.
        template <typename T>
        T get(const json& j, const char* key, T fallback) {
            auto it = j.find(key);
            if (it == j.end() || it->is_null()) return fallback;
            try { return it->get<T>(); }
            catch (...) { return fallback; }
        }

        const json& sub(const json& j, const char* key) {
            static const json empty = json::object();
            auto it = j.find(key);
            return (it != j.end() && it->is_object()) ? *it : empty;
        }

    } // namespace

    std::string settingsPath() {
        // Explicit override first: useful for portable installs, for running two
        // instruments from one machine, and for tests that must not touch the
        // user's real configuration.
        if (const char* env = std::getenv("OPENMEI_CONFIG")) {
            if (*env) return env;
        }

        // SDL_GetPrefPath is portable and creates the directory: on Windows
        // %APPDATA%/OpenMEI/OpenMEI/, on Linux $XDG_DATA_HOME (usually
        // ~/.local/share), on macOS ~/Library/Application Support. It always
        // returns a trailing separator, so plain concatenation is correct.
        //
        // Strictly, XDG puts configuration under ~/.config rather than
        // ~/.local/share; SDL offers no equivalent, and a hand-rolled path per
        // platform is not worth the divergence for one file.
        char* p = SDL_GetPrefPath("OpenMEI", "OpenMEI");
        std::string dir = p ? p : "";
        if (p) SDL_free(p);
        if (dir.empty()) dir = "./";
        return dir + "settings.json";
    }

    namespace {

        // Write `content` to `path` such that the file is never observed truncated:
        // write a temporary, flush it all the way to the platter, then replace.
        //
        // The sync must happen on the handle we WROTE through. Reopening the file
        // read-only and calling _commit() on that descriptor is invalid -- MSVCRT's
        // invalid-parameter handler fires and, by default, terminates the process.
        bool writeAtomic(const std::string& path, const std::string& content,
            std::string& err) {
            const std::string tmp = path + ".tmp";

            FILE* f = nullptr;
#ifdef _WIN32
            if (fopen_s(&f, tmp.c_str(), "wb") != 0) f = nullptr;
#else
            f = std::fopen(tmp.c_str(), "wb");
#endif
            if (!f) { err = "cannot open " + tmp + " for writing"; return false; }

            const size_t wrote = std::fwrite(content.data(), 1, content.size(), f);
            if (wrote != content.size()) {
                std::fclose(f);
                std::remove(tmp.c_str());
                err = "short write to " + tmp;
                return false;
            }
            std::fflush(f);

            // Best effort: get the bytes out of the page cache before the rename.
            // Without it the directory entry can land while the data has not, leaving a
            // valid-looking empty file -- worse than no file, because it reads as valid
            // empty configuration rather than a first run.
#ifdef _WIN32
            _commit(_fileno(f));
#else
            ::fsync(::fileno(f));
#endif
            std::fclose(f);

#ifdef _WIN32
            // MSVCRT's rename() refuses an existing destination, so it must be removed
            // first. That briefly leaves neither file present; MoveFileExW with
            // MOVEFILE_REPLACE_EXISTING is the genuinely atomic upgrade if it matters.
            std::remove(path.c_str());
#endif
            // POSIX rename() replaces atomically -- removing the destination first,
            // as Windows requires, would defeat the entire point of the temporary.
            if (std::rename(tmp.c_str(), path.c_str()) != 0) {
                std::remove(tmp.c_str());
                err = "cannot replace " + path;
                return false;
            }
            return true;
        }

    } // namespace

    bool loadSettings(AppSettings& s, std::string& err) {
        const std::string path = settingsPath();
        std::ifstream in(path);
        if (!in) return true;   // first run

        json j;
        try {
            in >> j;
        }
        catch (const std::exception& e) {
            err = "settings file at " + path + " could not be parsed: " + e.what();
            return false;
        }

        s.installationId = get<std::string>(j, "installationId", s.installationId);
        s.lastCameraKey = get<std::string>(j, "lastCameraKey", "");
        s.autoStartAlpaca = get<bool>(j, "autoStartAlpaca", false);

        // --- optics -------------------------------------------------------------
        {
            const json& o = sub(j, "optics");
            OpticsConfig& c = s.dimm.optics;
            c.subApertureMm = get<double>(o, "subApertureMm", c.subApertureMm);
            c.baselineMm = get<double>(o, "baselineMm", c.baselineMm);
            c.wedgeOrientation =
                WedgeOrientation(get<int>(o, "wedgeOrientation", int(c.wedgeOrientation)));
            c.wedgeAngleFromSeparationDeg =
                get<double>(o, "wedgeAngleDeg", c.wedgeAngleFromSeparationDeg);
            c.haveWedgeSpec = get<bool>(o, "haveWedgeSpec", c.haveWedgeSpec);
            c.wedgeApexArcmin = get<double>(o, "wedgeApexArcmin", c.wedgeApexArcmin);
            c.wedgeIndex = get<double>(o, "wedgeIndex", c.wedgeIndex);
            c.wedgeAbbe = get<double>(o, "wedgeAbbe", c.wedgeAbbe);
            c.separationTolerancePct =
                get<double>(o, "separationTolerancePct", c.separationTolerancePct);
            c.wedgeOnSpotB = get<bool>(o, "wedgeOnSpotB", c.wedgeOnSpotB);
        }

        // --- calibration: the reason this file exists ---------------------------
        {
            const json& o = sub(j, "calibration");
            CalibrationConfig& c = s.dimm.calibration;
            c.arcsecPerPixel = get<double>(o, "arcsecPerPixel", c.arcsecPerPixel);
            c.method = ScaleMethod(get<int>(o, "method", int(c.method)));
            c.calibratedUtc = get<std::string>(o, "calibratedUtc", c.calibratedUtc);
            c.fitResidualPx = get<double>(o, "fitResidualPx", c.fitResidualPx);
            c.declinationDeg = get<double>(o, "declinationDeg", c.declinationDeg);
            c.driftSeconds = get<double>(o, "driftSeconds", c.driftSeconds);
            c.notes = get<std::string>(o, "notes", c.notes);
            c.focalLengthMm = get<double>(o, "focalLengthMm", c.focalLengthMm);
            c.pixelSizeUm = get<double>(o, "pixelSizeUm", c.pixelSizeUm);
            c.binning = get<int>(o, "binning", c.binning);
        }

        {
            const json& o = sub(j, "acquisition");
            AcquisitionConfig& c = s.dimm.acquisition;
            c.detectThresholdSigma = get<double>(o, "detectThresholdSigma", c.detectThresholdSigma);
            c.searchWindowPx = get<int>(o, "searchWindowPx", c.searchWindowPx);
            c.minSpotSeparationPx = get<double>(o, "minSpotSeparationPx", c.minSpotSeparationPx);
            c.fluxRatioTolerance = get<double>(o, "fluxRatioTolerance", c.fluxRatioTolerance);
            c.expectedFluxRatio = get<double>(o, "expectedFluxRatio", c.expectedFluxRatio);
            c.requireUniquePair = get<bool>(o, "requireUniquePair", c.requireUniquePair);
        }

        {
            const json& o = sub(j, "centroid");
            CentroidConfig& c = s.dimm.centroid;
            c.windowRadiusPx = get<int>(o, "windowRadiusPx", c.windowRadiusPx);
            c.background = BackgroundMethod(get<int>(o, "background", int(c.background)));
            c.annulusInnerPx = get<int>(o, "annulusInnerPx", c.annulusInnerPx);
            c.annulusOuterPx = get<int>(o, "annulusOuterPx", c.annulusOuterPx);
            c.fixedBackground = get<double>(o, "fixedBackground", c.fixedBackground);
            c.thresholdSigma = get<double>(o, "thresholdSigma", c.thresholdSigma);
            c.iterations = get<int>(o, "iterations", c.iterations);
            c.subtractDark = get<bool>(o, "subtractDark", c.subtractDark);
            c.maskHotPixels = get<bool>(o, "maskHotPixels", c.maskHotPixels);
        }

        {
            const json& o = sub(j, "burst");
            BurstConfig& c = s.dimm.burst;
            c.framesPerBurst = get<int>(o, "framesPerBurst", c.framesPerBurst);
            c.publishIntervalS = get<double>(o, "publishIntervalS", c.publishIntervalS);
            c.interleaveExposures = get<bool>(o, "interleaveExposures", c.interleaveExposures);
            c.baseExposureUs = get<int64_t>(o, "baseExposureUs", c.baseExposureUs);
            c.pairing = ExposurePairing(get<int>(o, "pairing", int(c.pairing)));
            c.interleaveBlockFrames = get<int>(o, "interleaveBlockFrames", c.interleaveBlockFrames);
            c.interleaveSettleFrames = get<int>(o, "interleaveSettleFrames", c.interleaveSettleFrames);
            c.subtractNoiseBias = get<bool>(o, "subtractNoiseBias", c.subtractNoiseBias);
            c.minFrameSpacingMs = get<double>(o, "minFrameSpacingMs", c.minFrameSpacingMs);
        }

        {
            const json& o = sub(j, "rejection");
            RejectionConfig& c = s.dimm.rejection;
            c.maxSeparationDeviationPct = get<double>(o, "maxSeparationDeviationPct", c.maxSeparationDeviationPct);
            c.maxCentroidExcursionPx = get<double>(o, "maxCentroidExcursionPx", c.maxCentroidExcursionPx);
            c.minPerFrameSnr = get<double>(o, "minPerFrameSnr", c.minPerFrameSnr);
            c.rejectSaturated = get<bool>(o, "rejectSaturated", c.rejectSaturated);
            c.maxRejectedFraction = get<double>(o, "maxRejectedFraction", c.maxRejectedFraction);
            c.rejectElongated = get<bool>(o, "rejectElongated", c.rejectElongated);
            c.maxSpotEllipticity = get<double>(o, "maxSpotEllipticity", c.maxSpotEllipticity);
        }

        {
            const json& o = sub(j, "site");
            SiteConfig& c = s.dimm.site;
            c.latitudeDeg = get<double>(o, "latitudeDeg", c.latitudeDeg);
            c.longitudeDeg = get<double>(o, "longitudeDeg", c.longitudeDeg);
            c.elevationM = get<double>(o, "elevationM", c.elevationM);
            c.name = get<std::string>(o, "name", c.name);
            c.useMountAltitude = get<bool>(o, "useMountAltitude", c.useMountAltitude);
            c.manualAltitudeDeg = get<double>(o, "manualAltitudeDeg", c.manualAltitudeDeg);
            c.haveScienceTarget = get<bool>(o, "haveScienceTarget", c.haveScienceTarget);
            c.scienceAltitudeDeg = get<double>(o, "scienceAltitudeDeg", c.scienceAltitudeDeg);
        }

        {
            const json& o = sub(j, "reporting");
            ReportingConfig& c = s.dimm.reporting;
            c.wavelengthNm = get<double>(o, "wavelengthNm", c.wavelengthNm);
            c.coefficients = CoefficientModel(get<int>(o, "coefficients", int(c.coefficients)));
            c.manualKLong = get<double>(o, "manualKLong", c.manualKLong);
            c.manualKTran = get<double>(o, "manualKTran", c.manualKTran);
            c.zenithCorrect = get<bool>(o, "zenithCorrect", c.zenithCorrect);
            c.sqlitePath = get<std::string>(o, "sqlitePath", c.sqlitePath);
            c.logRawCentroids = get<bool>(o, "logRawCentroids", c.logRawCentroids);
        }

        {
            const json& o = sub(j, "alpaca");
            s.alpaca.port = get<int>(o, "port", s.alpaca.port);
            s.alpaca.discovery = get<bool>(o, "discovery", s.alpaca.discovery);
            s.alpaca.serverName = get<std::string>(o, "serverName", s.alpaca.serverName);
            s.alpaca.location = get<std::string>(o, "location", s.alpaca.location);
        }

        {
            const json& o = sub(j, "stream");
            if (!o.empty()) {
                StreamConfig& c = s.stream;
                c.x = get<int>(o, "x", c.x);
                c.y = get<int>(o, "y", c.y);
                c.width = get<int>(o, "width", c.width);
                c.height = get<int>(o, "height", c.height);
                c.bin = get<int>(o, "bin", c.bin);
                c.format = PixelFormat(get<int>(o, "format", int(c.format)));
                c.exposureUs = get<int64_t>(o, "exposureUs", c.exposureUs);
                c.gain = get<double>(o, "gain", c.gain);
                c.highSpeed = get<bool>(o, "highSpeed", c.highSpeed);
                c.usbBandwidth = get<int>(o, "usbBandwidth", c.usbBandwidth);
                c.monoBin = get<bool>(o, "monoBin", c.monoBin);
                c.hardwareBin = get<bool>(o, "hardwareBin", c.hardwareBin);
                c.conversionGain = get<int>(o, "conversionGain", c.conversionGain);
                s.haveStream = true;
            }
        }

        return true;
    }

    bool saveSettings(const AppSettings& s, std::string& err) {
        const DimmConfig& d = s.dimm;
        json j;

        j["version"] = 1;
        j["installationId"] = s.installationId;
        j["lastCameraKey"] = s.lastCameraKey;
        j["autoStartAlpaca"] = s.autoStartAlpaca;

        j["optics"] = {
            {"subApertureMm", d.optics.subApertureMm},
            {"baselineMm", d.optics.baselineMm},
            {"wedgeOrientation", int(d.optics.wedgeOrientation)},
            {"wedgeAngleDeg", d.optics.wedgeAngleFromSeparationDeg},
            {"haveWedgeSpec", d.optics.haveWedgeSpec},
            {"wedgeApexArcmin", d.optics.wedgeApexArcmin},
            {"wedgeIndex", d.optics.wedgeIndex},
            {"wedgeAbbe", d.optics.wedgeAbbe},
            {"separationTolerancePct", d.optics.separationTolerancePct},
            {"wedgeOnSpotB", d.optics.wedgeOnSpotB},
        };

        j["calibration"] = {
            {"arcsecPerPixel", d.calibration.arcsecPerPixel},
            {"method", int(d.calibration.method)},
            {"calibratedUtc", d.calibration.calibratedUtc},
            {"fitResidualPx", d.calibration.fitResidualPx},
            {"declinationDeg", d.calibration.declinationDeg},
            {"driftSeconds", d.calibration.driftSeconds},
            {"notes", d.calibration.notes},
            {"focalLengthMm", d.calibration.focalLengthMm},
            {"pixelSizeUm", d.calibration.pixelSizeUm},
            {"binning", d.calibration.binning},
        };

        j["acquisition"] = {
            {"detectThresholdSigma", d.acquisition.detectThresholdSigma},
            {"searchWindowPx", d.acquisition.searchWindowPx},
            {"minSpotSeparationPx", d.acquisition.minSpotSeparationPx},
            {"fluxRatioTolerance", d.acquisition.fluxRatioTolerance},
            {"expectedFluxRatio", d.acquisition.expectedFluxRatio},
            {"requireUniquePair", d.acquisition.requireUniquePair},
        };

        j["centroid"] = {
            {"windowRadiusPx", d.centroid.windowRadiusPx},
            {"background", int(d.centroid.background)},
            {"annulusInnerPx", d.centroid.annulusInnerPx},
            {"annulusOuterPx", d.centroid.annulusOuterPx},
            {"fixedBackground", d.centroid.fixedBackground},
            {"thresholdSigma", d.centroid.thresholdSigma},
            {"iterations", d.centroid.iterations},
            {"subtractDark", d.centroid.subtractDark},
            {"maskHotPixels", d.centroid.maskHotPixels},
        };

        j["burst"] = {
            {"framesPerBurst", d.burst.framesPerBurst},
            {"publishIntervalS", d.burst.publishIntervalS},
            {"interleaveExposures", d.burst.interleaveExposures},
            {"baseExposureUs", d.burst.baseExposureUs},
            {"pairing", int(d.burst.pairing)},
            {"interleaveBlockFrames", d.burst.interleaveBlockFrames},
            {"interleaveSettleFrames", d.burst.interleaveSettleFrames},
            {"subtractNoiseBias", d.burst.subtractNoiseBias},
            {"minFrameSpacingMs", d.burst.minFrameSpacingMs},
        };

        j["rejection"] = {
            {"maxSeparationDeviationPct", d.rejection.maxSeparationDeviationPct},
            {"maxCentroidExcursionPx", d.rejection.maxCentroidExcursionPx},
            {"minPerFrameSnr", d.rejection.minPerFrameSnr},
            {"rejectSaturated", d.rejection.rejectSaturated},
            {"maxRejectedFraction", d.rejection.maxRejectedFraction},
            {"rejectElongated", d.rejection.rejectElongated},
            {"maxSpotEllipticity", d.rejection.maxSpotEllipticity},
        };

        j["site"] = {
            {"latitudeDeg", d.site.latitudeDeg},
            {"longitudeDeg", d.site.longitudeDeg},
            {"elevationM", d.site.elevationM},
            {"name", d.site.name},
            {"useMountAltitude", d.site.useMountAltitude},
            {"manualAltitudeDeg", d.site.manualAltitudeDeg},
            {"haveScienceTarget", d.site.haveScienceTarget},
            {"scienceAltitudeDeg", d.site.scienceAltitudeDeg},
        };

        j["reporting"] = {
            {"wavelengthNm", d.reporting.wavelengthNm},
            {"coefficients", int(d.reporting.coefficients)},
            {"manualKLong", d.reporting.manualKLong},
            {"manualKTran", d.reporting.manualKTran},
            {"zenithCorrect", d.reporting.zenithCorrect},
            {"sqlitePath", d.reporting.sqlitePath},
            {"logRawCentroids", d.reporting.logRawCentroids},
        };

        j["alpaca"] = {
            {"port", s.alpaca.port},
            {"discovery", s.alpaca.discovery},
            {"serverName", s.alpaca.serverName},
            {"location", s.alpaca.location},
        };

        if (s.haveStream) {
            j["stream"] = {
                {"x", s.stream.x}, {"y", s.stream.y},
                {"width", s.stream.width}, {"height", s.stream.height},
                {"bin", s.stream.bin},
                {"format", int(s.stream.format)},
                {"exposureUs", s.stream.exposureUs},
                {"gain", s.stream.gain},
                {"highSpeed", s.stream.highSpeed},
                {"usbBandwidth", s.stream.usbBandwidth},
                {"monoBin", s.stream.monoBin},
                {"hardwareBin", s.stream.hardwareBin},
                {"conversionGain", s.stream.conversionGain},
            };
        }

        std::string err2;
        if (!writeAtomic(settingsPath(), j.dump(2) + "\n", err2)) {
            err = err2;
            return false;
        }
        return true;
    }

} // namespace mei