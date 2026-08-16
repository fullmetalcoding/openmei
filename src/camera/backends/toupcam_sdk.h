// toupcam_sdk.h -- runtime binding to the Touptek SDK and its OEM rebadges.
//
// Touptek licenses the same SDK to a long list of brands with nothing changed
// but the exported symbol prefix and the library name. SVBony asked INDI to add
// a driver for their SC715C and supplied an "svbonycamsdk" package; the INDI
// maintainers found it was the Touptek SDK with the symbols renamed, and that
// it had to coexist with SVBony's own separate SDK for their in-house cameras.
//
// So one implementation covers all of them, provided symbol names are built at
// runtime rather than linked. That is exactly what dynamic loading buys here.
//
// Only toupcam.h is needed at build time: the OEM headers differ solely in
// identifier spelling, and the structs are layout-compatible because they are
// the same compiled code. That does assume matching SDK generations -- a much
// older OEM DLL could have a different struct layout, which is why the version
// string is recorded in diagnostics.

#pragma once

#include "camera/dynlib.h"

#include <toupcam.h>

#include <string>
#include <vector>

// Some SDK generations ship toupcam.h with MAKEFOURCC commented out, on the
// assumption that a Windows system header already provided it. We build with
// WIN32_LEAN_AND_MEAN, so mmsystem.h is not in scope -- define it if absent.
// Little-endian packing, matching how the SDK reports the CFA layout.
#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d)                                     \
    ((static_cast<unsigned>(a)      ) | (static_cast<unsigned>(b) <<  8) | \
     (static_cast<unsigned>(c) << 16) | (static_cast<unsigned>(d) << 24))
#endif

namespace mei {

struct ToupVariant {
    const char* lib;      // library stem, no prefix or extension
    const char* prefix;   // exported symbol prefix
    const char* label;    // shown to the user
};

// Ordered by how likely they are to be present. The first variant that loads
// AND reports a camera wins; enumerating all of them would double-report the
// same hardware, which is why SharpCap and N.I.N.A. can show one camera twice.
inline const std::vector<ToupVariant>& toupVariants() {
    static const std::vector<ToupVariant> v = {
        { "toupcam",      "Toupcam_",      "ToupTek" },
        { "svbonycam",    "Svbonycam_",    "SVBony (ToupTek OEM)" },
        { "altaircam",    "Altaircam_",    "Altair" },
        { "ogmacam",      "Ogmacam_",      "OGMA" },
        { "nncam",        "Nncam_",        "RisingCam / Levenhuk" },
        { "omegonprocam", "Omegonprocam_", "Omegon" },
        { "starshootg",   "Starshootg_",   "Orion StarShoot G" },
        { "bressercam",   "Bressercam_",   "Bresser" },
        { "mallincam",    "Mallincam_",    "Mallincam" },
    };
    return v;
}

// NOTE: these signatures are reconstructed and must be checked against the
// toupcam.h you actually vendor. The X-macro table is deliberately the only
// place they appear, so verification is a single read-through.
#define MEI_TOUP_SYMBOLS(X)                                                                 \
    X(const char*, Version,     (void),                                                true)\
    X(unsigned,    EnumV2,      (ToupcamDeviceV2*),                                    true)\
    X(HToupcam,    Open,        (const wchar_t*),                                      true)\
    X(void,        Close,       (HToupcam),                                            true)\
    X(HRESULT,     StartPullModeWithCallback, (HToupcam, PTOUPCAM_EVENT_CALLBACK, void*), true)\
    X(HRESULT,     PullImageV3, (HToupcam, void*, int, int, int, ToupcamFrameInfoV3*), true)\
    X(HRESULT,     Stop,        (HToupcam),                                            true)\
    X(HRESULT,     put_Option,  (HToupcam, unsigned, int),                             true)\
    X(HRESULT,     get_Option,  (HToupcam, unsigned, int*),                            true)\
    X(HRESULT,     put_Roi,     (HToupcam, unsigned, unsigned, unsigned, unsigned),    true)\
    X(HRESULT,     get_Roi,     (HToupcam, unsigned*, unsigned*, unsigned*, unsigned*),true)\
    X(HRESULT,     put_ExpoTime,(HToupcam, unsigned),                                  true)\
    X(HRESULT,     get_ExpoTime,(HToupcam, unsigned*),                                 true)\
    X(HRESULT,     get_ExpTimeRange, (HToupcam, unsigned*, unsigned*, unsigned*),      true)\
    X(HRESULT,     put_ExpoAGain,(HToupcam, unsigned short),                           true)\
    X(HRESULT,     get_ExpoAGain,(HToupcam, unsigned short*),                          true)\
    X(HRESULT,     get_ExpoAGainRange, (HToupcam, unsigned short*, unsigned short*, unsigned short*), true)\
    X(HRESULT,     put_AutoExpoEnable, (HToupcam, int),                                true)\
    X(HRESULT,     get_Size,    (HToupcam, int*, int*),                                true)\
    X(HRESULT,     get_RawFormat,(HToupcam, unsigned*, unsigned*),                    false)\
    X(HRESULT,     get_SerialNumber, (HToupcam, char*),                               false)\
    X(HRESULT,     get_PixelSize,(HToupcam, unsigned, float*, float*),                false)

struct ToupApi {
    DynLib      lib;
    std::string prefix;
    std::string label;

#define MEI_TOUP_DECL(ret, name, params, req) ret (__stdcall *name) params = nullptr;
    MEI_TOUP_SYMBOLS(MEI_TOUP_DECL)
#undef MEI_TOUP_DECL

    // Binds against one OEM variant. Symbol names are assembled at runtime, so
    // adding a brand is one line in toupVariants().
    bool loadVariant(const ToupVariant& v, const std::string& sdkDir,
                     std::string& err) {
        if (!lib.open(libCandidates(v.lib, sdkDir), err)) return false;
        prefix = v.prefix;
        label  = v.label;

        std::vector<std::string> missing;
#define MEI_TOUP_BIND(ret, name, params, req)                              \
        if (!lib.bind((prefix + #name).c_str(), name) && (req))            \
            missing.emplace_back(prefix + #name);
        MEI_TOUP_SYMBOLS(MEI_TOUP_BIND)
#undef MEI_TOUP_BIND

        if (!missing.empty()) {
            err = "loaded " + lib.path() + " but symbols are missing:";
            for (const auto& m : missing) err += " " + m;
            err += " -- SDK generation may not match the vendored header.";
            lib.close();
            return false;
        }
        return true;
    }

    void unload() { lib.close(); }
    bool ok() const { return lib.isOpen(); }
    std::string version() const {
        if (!ok() || !Version) return "unknown";
        const char* v = Version();
        return v ? v : "unknown";
    }
};

} // namespace mei
