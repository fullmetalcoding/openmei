// dynlib.h -- runtime loading of vendor SDKs.
//
// Uses SDL3's loader, so there is no extra dependency and no platform #ifdef in
// backend code. SDL_FunctionPointer also avoids the object-pointer-to-function-
// pointer conversion that raw dlsym forces and that is formally UB in C++.
//
// Vendor SDKs are never linked. Their headers are compiled against; the binary
// is found at runtime, so the project builds with no SDKs installed and a
// missing SDK disables one backend instead of breaking the build.

#pragma once

#include <SDL3/SDL_loadso.h>

#include <string>
#include <vector>

namespace mei {

class DynLib {
public:
    DynLib() = default;
    ~DynLib() { close(); }
    DynLib(const DynLib&)            = delete;
    DynLib& operator=(const DynLib&) = delete;

    bool open(const std::vector<std::string>& candidates, std::string& err) {
        close();
        for (const auto& name : candidates) {
            if ((h_ = SDL_LoadObject(name.c_str()))) {
                path_ = name;
                return true;
            }
        }
        err = "none of the candidate libraries could load (last error: ";
        err += SDL_GetError();
        err += ")";
        return false;
    }

    void close() {
        if (h_) { SDL_UnloadObject(h_); h_ = nullptr; }
        path_.clear();
    }

    template <typename Fn>
    bool bind(const char* symbol, Fn& out) const {
        SDL_FunctionPointer p = h_ ? SDL_LoadFunction(h_, symbol) : nullptr;
        out = reinterpret_cast<Fn>(p);
        return p != nullptr;
    }

    bool               isOpen() const { return h_ != nullptr; }
    const std::string& path() const { return path_; }

private:
    SDL_SharedObject* h_ = nullptr;
    std::string       path_;
};

// Candidate names for a bare library stem, user-configured directory first.
// The versioned .so entries matter: several vendors ship only libFoo.so.1.x
// with no unversioned symlink, so a single-name lookup silently fails on Linux.
inline std::vector<std::string> libCandidates(const std::string& stem,
                                              const std::string& userDir = {}) {
    std::vector<std::string> bare;
#if defined(_WIN32)
    bare = { stem + ".dll" };
    const char sep = '\\';
#elif defined(__APPLE__)
    bare = { "lib" + stem + ".dylib", stem + ".dylib" };
    const char sep = '/';
#else
    bare = { "lib" + stem + ".so", "lib" + stem + ".so.1", "lib" + stem + ".so.2" };
    const char sep = '/';
#endif
    if (userDir.empty()) return bare;

    std::vector<std::string> out;
    out.reserve(bare.size() * 2);
    for (const auto& n : bare) out.push_back(userDir + sep + n);
    for (const auto& n : bare) out.push_back(n);   // then the system search path
    return out;
}

} // namespace mei
