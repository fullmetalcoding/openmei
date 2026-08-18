// uuid.h -- installation identity.
//
// Lives here rather than on AlpacaServer because it is not an Alpaca concept:
// it identifies this installation of OpenMEI, and Alpaca merely happens to be
// one consumer. Putting it on the server also meant the identity disappeared
// when Alpaca was compiled out, which is how it came to be referenced from
// outside its own #ifdef.
//
// Header-only and dependency-free so both the settings store and the network
// layer can use it without either including the other.

#pragma once

#include <chrono>
#include <random>
#include <string>

namespace mei {

    // RFC 4122 version 4, formatted canonically. Generated once per installation
    // and persisted: Alpaca's UniqueID identifies a device INSTANCE, so it must be
    // stable across restarts or clients lose their saved selection, while still
    // differing between two installations on one network or clients confuse them.
    inline std::string generateUuid() {
        const auto seed = static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        std::random_device rd;
        std::mt19937_64 gen(static_cast<uint64_t>(rd()) ^ seed);
        std::uniform_int_distribution<int> nybble(0, 15);

        static const char* kHex = "0123456789abcdef";
        std::string s;
        s.reserve(36);
        for (int i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) { s += '-'; continue; }
            if (i == 14) { s += '4'; continue; }                    // version
            if (i == 19) { s += kHex[(nybble(gen) & 0x3) | 0x8]; continue; }  // variant
            s += kHex[nybble(gen)];
        }
        return s;
    }

} // namespace mei