# =============================================================================
#  OpenMei -- differential image motion monitor
#
#  Configure (Visual Studio 2022):
#      cmake -B build -G "Visual Studio 17 2022" -A x64
#      cmake --build build --config Release
#  or just open the folder in VS, which picks up CMakePresets/CMakeLists.
#
#  Nothing here requires a vendor camera SDK to be installed. Vendor SDKs are
#  loaded at runtime via SDL_LoadObject; only their HEADERS are used at compile
#  time, and each backend compiles only if its header is present.
# =============================================================================

cmake_minimum_required(VERSION 3.21)   # 3.21 for $<TARGET_RUNTIME_DLLS:...>

project(OpenMei
    VERSION 0.1.0
    DESCRIPTION "Open-source DIMM seeing monitor"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Multi-config generators (VS) ignore CMAKE_BUILD_TYPE -- always use $<CONFIG>.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# -----------------------------------------------------------------------------
#  Options
# -----------------------------------------------------------------------------

option(OPENMEI_BUILD_ALPACA  "Build the ASCOM Alpaca ObservingConditions server" ON)
option(OPENMEI_BUILD_TESTS   "Build unit tests"                                  OFF)

# The escape hatch discussed for appliance builds: link a vendor SDK directly
# instead of loading it at runtime. Off by default -- see docs/linking.md.
option(OPENMEI_STATIC_ASI    "Statically link the ZWO SDK instead of dlopen"     OFF)


# Bundle vendor camera DLLs into the installer. OFF by default and deliberately
# so: shipping them is redistribution of someone else's binaries, and the terms
# are mostly unstated rather than permissive. Turn this on only once you have
# written permission from each vendor -- see THIRD_PARTY.md.
option(OPENMEI_BUNDLE_VENDOR_DLLS "Include vendor camera DLLs in the package" OFF)
set(OPENMEI_VENDOR_DLL_DIR "${CMAKE_SOURCE_DIR}/../camdlls"
    CACHE PATH "Directory holding vendor camera DLLs, outside the repository")
 

# -----------------------------------------------------------------------------
#  Dependency versions -- bump deliberately, not automatically.
#  Pinned tags rather than branches so a clean clone is reproducible.
# -----------------------------------------------------------------------------

set(SDL3_TAG      "release-3.2.0")
set(GLEW_TAG      "glew-cmake-2.2.0")
set(IMGUI_TAG     "v1.91.5-docking")
set(IMPLOT_TAG    "v0.16")
set(HTTPLIB_TAG   "v0.18.3")
set(JSON_TAG      "v3.11.3")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- SDL3 ---------------------------------------------------------------------
# Also provides SDL_LoadObject/SDL_LoadFunction, which is how every camera
# backend reaches its vendor SDK. No separate dynamic-loading dependency.
set(SDL_SHARED   ON  CACHE BOOL "" FORCE)
set(SDL_STATIC   OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL  OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        ${SDL3_TAG}
    GIT_SHALLOW    TRUE)

# --- GLEW ---------------------------------------------------------------------
# Perlmint's fork rather than upstream: upstream's repo needs a code-generation
# step before it will build, while this one is FetchContent-friendly.
# Target is libglew_static.
set(glew-cmake_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(glew-cmake_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(ONLY_LIBS               ON  CACHE BOOL "" FORCE)
FetchContent_Declare(glew
    GIT_REPOSITORY https://github.com/Perlmint/glew-cmake.git
    GIT_TAG        ${GLEW_TAG}
    GIT_SHALLOW    TRUE)

# --- Dear ImGui + ImPlot ------------------------------------------------------
# Neither ships a CMakeLists; we declare the targets ourselves below.
# Docking branch: useful for a monitoring UI with detachable panels.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${IMGUI_TAG}
    GIT_SHALLOW    TRUE)

FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        ${IMPLOT_TAG}
    GIT_SHALLOW    TRUE)

# --- cpp-httplib (Alpaca server) ----------------------------------------------
set(HTTPLIB_COMPILE      OFF CACHE BOOL "" FORCE)   # header-only mode
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        ${HTTPLIB_TAG}
    GIT_SHALLOW    TRUE)

# --- nlohmann/json ------------------------------------------------------------
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${JSON_TAG}
    GIT_SHALLOW    TRUE)

# CMake 4 removed compatibility with projects declaring cmake_minimum_required
# below 3.5, and glew-cmake still declares an older minimum. This is the
# sanctioned escape hatch, scoped to the dependency build so our own project
# keeps its real minimum. Unknown and harmless on CMake < 3.31.
set(_mei_saved_policy_min "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_MakeAvailable(SDL3 glew imgui implot httplib json)

if(_mei_saved_policy_min STREQUAL "")
    unset(CMAKE_POLICY_VERSION_MINIMUM)
else()
    set(CMAKE_POLICY_VERSION_MINIMUM "${_mei_saved_policy_min}")
endif()

# -----------------------------------------------------------------------------
#  ImGui / ImPlot targets
# -----------------------------------------------------------------------------

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLEW)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

# -----------------------------------------------------------------------------
#  Vendor camera SDK headers
#
#  Drop the vendor's header into third_party/vendor_headers/<vendor>/ and that
#  backend compiles. Nothing is linked; the .dll/.so is found at runtime.
#  This keeps vendor binaries out of the repo and lets contributors build the
#  whole project with no SDKs installed at all.
# -----------------------------------------------------------------------------

set(VENDOR_HEADERS "${CMAKE_SOURCE_DIR}/third_party/vendor_headers")

function(mei_detect_vendor NAME HEADER_SUBPATH DEFINE_NAME OUT_SOURCES)
    if(EXISTS "${VENDOR_HEADERS}/${HEADER_SUBPATH}")
        message(STATUS "  ${NAME}: header found -- backend enabled")
        set(${OUT_SOURCES} TRUE PARENT_SCOPE)
    else()
        message(STATUS "  ${NAME}: header absent -- backend skipped")
        set(${OUT_SOURCES} FALSE PARENT_SCOPE)
    endif()
endfunction()

message(STATUS "Camera backends:")
mei_detect_vendor("ZWO ASI"    "asi/ASICamera2.h"           HAVE_ASI       HAVE_ASI)
mei_detect_vendor("SVBony"     "svbony/SVBCameraSDK.h"      HAVE_SVBONY    HAVE_SVBONY)
mei_detect_vendor("Touptek"    "toupcam/toupcam.h"          HAVE_TOUPTEK   HAVE_TOUPTEK)
mei_detect_vendor("QHYCCD"     "qhy/qhyccd.h"               HAVE_QHY       HAVE_QHY)
mei_detect_vendor("Player One" "playerone/PlayerOneCamera.h" HAVE_PLAYERONE HAVE_PLAYERONE)

# -----------------------------------------------------------------------------
#  Sources
# -----------------------------------------------------------------------------

set(OPENMEI_SOURCES
    src/main.cpp
    src/app/settings_store.cpp
    src/app/uuid.h
    src/app/settings_store.h
    src/app/seeing_history.cpp

    # --- camera abstraction -------------------------------------------------
    src/camera/camera.h
    src/camera/camera.cpp
    src/camera/dynlib.h
    src/camera/registry.cpp
    src/camera/registry.h
    src/camera/stream.h
    src/camera/stream.cpp
 
    # Always available -- no hardware required. Build these first; they are how
    # you validate the measurement chain against known ground truth.
    src/camera/backends/synthetic_backend.h
    src/camera/backends/synthetic_backend.cpp
   # src/camera/backends/ser_replay_backend.cpp  # replay a SER capture

    # --- measurement --------------------------------------------------------
    src/dimm/centroid.cpp        # background subtraction, thresholding, 1st moment
    src/dimm/centroid.h
    #src/dimm/statistics.cpp      # differential variance, noise-bias subtraction
    #src/dimm/statistics.h
    src/dimm/seeing.cpp          # variance -> r0 -> FWHM, airmass correction
    src/dimm/seeing.h
    src/dimm/config.h
    src/dimm/processor.cpp
    src/dimm/processor.h

    #src/dimm/calibration.cpp     # plate scale, exposure-time extrapolation
    #src/dimm/calibration.h

    # --- ui -----------------------------------------------------------------
    src/ui/camera_dialog.h
    src/ui/camera_dialog.cpp
    src/ui/viewport.h
    src/ui/viewport.cpp
    src/ui/settings_dialog.h
    src/ui/settings_dialog.cpp
    src/ui/dimm_panel.cpp
    src/ui/dimm_panel.h
    src/ui/history_panel.cpp
    src/ui/history_panel.h
   
)

if(HAVE_ASI)
    list(APPEND OPENMEI_SOURCES src/camera/backends/asi_backend.cpp)
endif()
if(HAVE_SVBONY)
    list(APPEND OPENMEI_SOURCES src/camera/backends/svbony_backend.cpp)
endif()
if(HAVE_TOUPTEK)
    # Touptek is push/callback rather than pull -- this backend runs an adapter
    # thread that feeds callback frames into the common buffer pool.
    list(APPEND OPENMEI_SOURCES src/camera/backends/toupcam_backend.cpp)
endif()
if(HAVE_QHY)
    list(APPEND OPENMEI_SOURCES src/camera/backends/qhy_backend.cpp)
endif()
if(HAVE_PLAYERONE)
    list(APPEND OPENMEI_SOURCES src/camera/backends/playerone_backend.cpp)
endif()

if(OPENMEI_BUILD_ALPACA)
    list(APPEND OPENMEI_SOURCES
        src/net/alpaca_server.cpp    # ObservingConditions: StarFWHM + Actions
        src/net/alpaca_server.h
#        src/net/discovery.cpp        # UDP 32227 responder
#        src/net/telemetry_socket.cpp # newline-delimited JSON push feed
    )
endif()


add_executable(openmei ${OPENMEI_SOURCES})

target_include_directories(openmei PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${VENDOR_HEADERS})

if(HAVE_ASI)
    target_include_directories(openmei PRIVATE ${VENDOR_HEADERS}/asi)
endif()
if(HAVE_SVBONY)
    target_include_directories(openmei PRIVATE ${VENDOR_HEADERS}/svbony)
endif()
if(HAVE_TOUPTEK)
    target_include_directories(openmei PRIVATE ${VENDOR_HEADERS}/toupcam)
endif()
if(HAVE_QHY)
    target_include_directories(openmei PRIVATE ${VENDOR_HEADERS}/qhy)
endif()
if(HAVE_PLAYERONE)
    target_include_directories(openmei PRIVATE ${VENDOR_HEADERS}/playerone)
endif()

target_link_libraries(openmei PRIVATE
    SDL3::SDL3
    libglew_static
    imgui
    implot
    nlohmann_json::nlohmann_json)

if(OPENMEI_BUILD_ALPACA)
    target_link_libraries(openmei PRIVATE httplib::httplib)
    target_compile_definitions(openmei PRIVATE OPENMEI_ALPACA)
endif()

foreach(V ASI SVBONY TOUPTEK QHY PLAYERONE)
    if(HAVE_${V})
        target_compile_definitions(openmei PRIVATE MEI_HAVE_${V})
    endif()
endforeach()

find_package(OpenGL REQUIRED)
target_link_libraries(openmei PRIVATE OpenGL::GL)

if(WIN32)
    target_link_libraries(openmei PRIVATE ws2_32)   # UDP discovery socket
else()
    find_package(Threads REQUIRED)
    target_link_libraries(openmei PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# -----------------------------------------------------------------------------
#  Compiler settings
# -----------------------------------------------------------------------------

if(MSVC)
    target_compile_options(openmei PRIVATE
        /W4 /permissive- /utf-8 /MP
        /wd4100        # unreferenced formal parameter -- noisy with callbacks
    )
    target_compile_definitions(openmei PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(openmei PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# -----------------------------------------------------------------------------
#  Visual Studio conveniences
# -----------------------------------------------------------------------------

if(MSVC)
    # F5 works out of the box instead of launching ALL_BUILD.
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT openmei)

    # Run from the output dir so config files and shaders resolve.
    set_target_properties(openmei PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:openmei>")

    # Mirror the on-disk layout in Solution Explorer.
    source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src
                 PREFIX "src" FILES ${OPENMEI_SOURCES})

    # Keep FetchContent's targets out of the way.
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
    foreach(T SDL3-shared SDL_uclibc libglew_static imgui implot)
        if(TARGET ${T})
            set_target_properties(${T} PROPERTIES FOLDER "third_party")
        endif()
    endforeach()
endif()

# Copy SDL3.dll (and anything else shared) next to the exe. Requires 3.21+.
#
# Windows only, and not merely as an optimisation: TARGET_RUNTIME_DLLS is empty
# on platforms without DLLs, so the command collapses to
#     cmake -E copy_if_different <destination>
# with no sources. That is invalid usage, and because it runs POST_BUILD the
# link succeeds first -- producing a build that reports success and then dies
# printing cmake's help text.
#
# Nothing is needed on Linux or macOS: SDL3.so is found through the RPATH CMake
# already sets on the executable.
if(WIN32)
    add_custom_command(TARGET openmei POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:openmei> $<TARGET_FILE_DIR:openmei>
        COMMAND_EXPAND_LISTS)
endif()

# -----------------------------------------------------------------------------
#  Vendor DLLs
#
#  Kept outside the repository (default ../camdlls) so that cloning OpenMEI
#  never brings someone else's binaries with it. Architecture subdirectories are
#  preferred when present, because an x86 DLL in an x64 build fails at
#  SDL_LoadObject with an error that looks like "SDK not installed".
# -----------------------------------------------------------------------------
 
set(OPENMEI_VENDOR_DLLS "")
if(OPENMEI_BUNDLE_VENDOR_DLLS)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_arch_dir "${OPENMEI_VENDOR_DLL_DIR}/x64")
    else()
        set(_arch_dir "${OPENMEI_VENDOR_DLL_DIR}/x86")
    endif()
    if(EXISTS "${_arch_dir}")
        file(GLOB OPENMEI_VENDOR_DLLS "${_arch_dir}/*.dll")
    elseif(EXISTS "${OPENMEI_VENDOR_DLL_DIR}")
        file(GLOB OPENMEI_VENDOR_DLLS "${OPENMEI_VENDOR_DLL_DIR}/*.dll")
    endif()
 
    if(OPENMEI_VENDOR_DLLS)
        list(LENGTH OPENMEI_VENDOR_DLLS _n)
        message(STATUS "Bundling ${_n} vendor DLL(s) from ${OPENMEI_VENDOR_DLL_DIR}")
        # Also stage them next to the executable so F5 in Visual Studio finds a
        # camera without a separate install step.
        add_custom_command(TARGET openmei POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${OPENMEI_VENDOR_DLLS} $<TARGET_FILE_DIR:openmei>
            COMMAND_EXPAND_LISTS)
    else()
        message(WARNING
            "OPENMEI_BUNDLE_VENDOR_DLLS is ON but no DLLs were found under "
            "${OPENMEI_VENDOR_DLL_DIR}. Note file(GLOB) runs at configure time, "
            "so adding DLLs later needs a reconfigure.")
    endif()
endif()
 
# -----------------------------------------------------------------------------
#  Install and package
# -----------------------------------------------------------------------------
 
install(TARGETS openmei RUNTIME DESTINATION .)
 
# SDL3.dll and anything else CMake knows we link. Vendor SDKs are invisible here
# by design -- they are loaded at runtime, so CMake has no dependency edge to
# them and they must be handled separately above.
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:openmei> DESTINATION . OPTIONAL)
else()
    # TARGET_RUNTIME_DLLS is Windows-only, but SDL3 is a shared library here too.
    # Install the target itself so CMake resolves the versioned soname and the
    # symlink chain -- copying $<TARGET_FILE:SDL3::SDL3> alone gives you
    # libSDL3.so.0.2.0 with nothing pointing at it.
    install(IMPORTED_RUNTIME_ARTIFACTS SDL3::SDL3
            LIBRARY DESTINATION lib COMPONENT openmei)
endif()

if(NOT WIN32 AND NOT APPLE)
    set_target_properties(openmei PROPERTIES
        # $ORIGIN is resolved at load time relative to the executable, so the
        # tree relocates anywhere. The build-tree RPATH CMake adds by default is
        # an absolute path into the build directory and is useless once shipped.
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_WITH_INSTALL_RPATH FALSE)
endif()
 
if(OPENMEI_VENDOR_DLLS)
    install(FILES ${OPENMEI_VENDOR_DLLS} DESTINATION .)
endif()
 
install(FILES
    "${CMAKE_SOURCE_DIR}/README.md"
    "${CMAKE_SOURCE_DIR}/LICENSE"
    DESTINATION . OPTIONAL)
if(EXISTS "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md")
    install(FILES "${CMAKE_SOURCE_DIR}/THIRD_PARTY.md" DESTINATION .)
endif()
 
if(MSVC)
    # The VC++ runtime, so the installer does not depend on the user already
    # having the redistributable.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    include(InstallRequiredSystemLibraries)
endif()
 
set(CPACK_PACKAGE_NAME              "OpenMEI")
set(CPACK_PACKAGE_VENDOR            "OpenMEI Project")
set(CPACK_PACKAGE_VERSION           "${OPENMEI_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open-source DIMM atmospheric seeing monitor")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OpenMEI")
set(CPACK_PACKAGE_FILE_NAME
    "OpenMEI-${OPENMEI_VERSION_FULL}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()
 
if(WIN32)
    # ZIP first: it always works, needs no external tooling, and is what CI can
    # produce unconditionally. NSIS is added only when makensis is present.
    set(CPACK_GENERATOR "ZIP")
    find_program(MAKENSIS_EXECUTABLE NAMES makensis)
    if(MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_PACKAGE_NAME       "OpenMEI ${OPENMEI_VERSION}")
        set(CPACK_NSIS_DISPLAY_NAME       "OpenMEI Seeing Monitor")
        set(CPACK_NSIS_INSTALLED_ICON_NAME "openmei.exe")
        set(CPACK_NSIS_MODIFY_PATH        OFF)
        set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
        set(CPACK_NSIS_CREATE_ICONS_EXTRA
            "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\OpenMEI.lnk' '$INSTDIR\\\\openmei.exe'")
        set(CPACK_NSIS_DELETE_ICONS_EXTRA
            "Delete '$SMPROGRAMS\\\\$START_MENU\\\\OpenMEI.lnk'")
    else()
        message(STATUS "makensis not found -- ZIP package only")
    endif()
else()
    set(CPACK_GENERATOR "TGZ")
endif()
 
include(CPack)
 
# -----------------------------------------------------------------------------
#  Summary
# -----------------------------------------------------------------------------

message(STATUS "")
message(STATUS "OpenMEI ${OPENMEI_VERSION_FULL}")
message(STATUS "  Vendor DLLs   : ${OPENMEI_BUNDLE_VENDOR_DLLS} (${OPENMEI_VENDOR_DLL_DIR})")
message(STATUS "  Package       : ${CPACK_GENERATOR}")
message(STATUS "  Alpaca server : ${OPENMEI_BUILD_ALPACA}")
message(STATUS "  Vendor SDKs   : loaded at runtime (no link-time dependency)")
message(STATUS "  Output        : ${CMAKE_BINARY_DIR}/bin/<config>")
message(STATUS "")
