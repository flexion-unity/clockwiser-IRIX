cmake_minimum_required(VERSION 3.16)
if(APPLE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version for Clockwiser")
    endif()
endif()
project(Clockwiser LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Make Release explicitly -O3 (the optimisation the scalers rely on).
if(MSVC)
    set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG")
else()
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
endif()

# -----------------------------------------------------------------------------
# Emscripten (web) build
#
# SDL2 / SDL2_image / SDL2_mixer come from Emscripten ports. The entire data/
# folder is preloaded into MEMFS, and ASYNCIFY lets the main loop yield to the
# browser without source changes.
#
# Build:
#   source /path/to/emsdk/emsdk_env.sh
#   emcmake cmake -B build-web -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-web -j
#   # serve build-web and open clockwiser.html
# -----------------------------------------------------------------------------
if(EMSCRIPTEN)
    set(SOURCES
        src/m68k_runtime.c
        src/amigaHostSdl.c
        src/amigaHostMusic.c
        src/clockagaHostFiles.c
        src/clockwiser_state.c
        src/clockwiserDrawText.c
        src/clockwiser_movie_player.c
        src/clockaga_transpiled.c
        src/clockwiser_main.c
    )

    add_executable(clockwiser ${SOURCES})

    target_compile_definitions(clockwiser PRIVATE
        NOTSTANDALONE
        AMIGA_HOST_HAVE_SDL2_MIXER
    )
    target_compile_options(clockwiser PRIVATE
        -Wall -Wextra
        "SHELL:-sUSE_SDL=2"
        "SHELL:-sUSE_SDL_IMAGE=2"
        "SHELL:-sUSE_SDL_MIXER=2"
        "SHELL:-sSDL2_IMAGE_FORMATS=png,gif"
        "SHELL:-sSDL2_MIXER_FORMATS=ogg"
    )
    target_link_options(clockwiser PRIVATE
        "SHELL:-sUSE_SDL=2"
        "SHELL:-sUSE_SDL_IMAGE=2"
        "SHELL:-sUSE_SDL_MIXER=2"
        "SHELL:-sSDL2_IMAGE_FORMATS=png,gif"
        "SHELL:-sSDL2_MIXER_FORMATS=ogg"
        "SHELL:-sASYNCIFY=1"
        "SHELL:-sALLOW_MEMORY_GROWTH=1"
        "SHELL:-sINITIAL_MEMORY=64MB"
        "SHELL:-sSTACK_SIZE=1MB"
        "SHELL:-sEXIT_RUNTIME=0"
        "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/data@/data"
    )

    set_source_files_properties(
        src/clockaga_transpiled.c
        PROPERTIES COMPILE_FLAGS "-w"
    )

    set_target_properties(clockwiser PROPERTIES SUFFIX ".html")

    return()
endif()

# -----------------------------------------------------------------------------
# IRIX: link the bundled static SDL2 from IRIX-SDL2/ instead of a system one.
# X11/GL are dlopen'd at runtime; libaudio (dmedia) and pthreads are linked.
# -----------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME MATCHES "^IRIX")
    set(CLOCKWISER_IRIX_SDL2_DIR "${CMAKE_SOURCE_DIR}/IRIX-SDL2")
    find_package(Threads REQUIRED)
    add_library(SDL2::SDL2 STATIC IMPORTED)
    set_target_properties(SDL2::SDL2 PROPERTIES
        IMPORTED_LOCATION "${CLOCKWISER_IRIX_SDL2_DIR}/lib32/libSDL2.a"
        INTERFACE_INCLUDE_DIRECTORIES "${CLOCKWISER_IRIX_SDL2_DIR}/include;${CLOCKWISER_IRIX_SDL2_DIR}/include/SDL2"
        INTERFACE_LINK_LIBRARIES "audio;Threads::Threads;m"
    )
else()
    find_package(SDL2 REQUIRED CONFIG)
endif()

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2_IMAGE_PC QUIET IMPORTED_TARGET SDL2_image)
endif()
if(TARGET PkgConfig::SDL2_image)
    set(CLOCKWISER_SDL2_IMAGE_LIB PkgConfig::SDL2_image)
else()
    find_package(SDL2_image CONFIG QUIET)
endif()
if(TARGET SDL2_image::SDL2_image)
    set(CLOCKWISER_SDL2_IMAGE_LIB SDL2_image::SDL2_image)
elseif(NOT CLOCKWISER_SDL2_IMAGE_LIB)
    find_path(CLOCKWISER_SDL2_IMAGE_INCLUDE_DIR SDL2/SDL_image.h
        PATHS /opt/homebrew/include /usr/local/include /usr/include
    )
    find_library(CLOCKWISER_SDL2_IMAGE_LIBRARY NAMES SDL2_image
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib
    )
    if(CLOCKWISER_SDL2_IMAGE_INCLUDE_DIR AND CLOCKWISER_SDL2_IMAGE_LIBRARY)
        add_library(SDL2_image_manual UNKNOWN IMPORTED)
        set_target_properties(SDL2_image_manual PROPERTIES
            IMPORTED_LOCATION "${CLOCKWISER_SDL2_IMAGE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${CLOCKWISER_SDL2_IMAGE_INCLUDE_DIR}"
        )
        set(CLOCKWISER_SDL2_IMAGE_LIB SDL2_image_manual)
    else()
        message(FATAL_ERROR "SDL2_image not found (install sdl2_image / libsdl2-image-dev, or set SDL2_image_DIR)")
    endif()
endif()

set(HAVE_SDL2_MIXER OFF)
find_package(SDL2_mixer CONFIG QUIET)
if(SDL2_mixer_FOUND)
    set(HAVE_SDL2_MIXER ON)
endif()

set(CLOCKWISER_MACOS_ICON "${CMAKE_SOURCE_DIR}/cmake/AppIcon.icns")
set(CLOCKWISER_WINDOWS_ICON_RC "${CMAKE_SOURCE_DIR}/cmake/clockwiser.rc")
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/cmake/AppIcon.ico" AND EXISTS "${CLOCKWISER_WINDOWS_ICON_RC}")
    enable_language(RC)
    set(CLOCKWISER_WINDOWS_SOURCES "${CLOCKWISER_WINDOWS_ICON_RC}")
endif()
if(APPLE AND EXISTS "${CLOCKWISER_MACOS_ICON}")
    set_source_files_properties("${CLOCKWISER_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
endif()

set(SOURCES
    src/m68k_runtime.c
    src/amigaHostSdl.c
    src/amigaHostMusic.c
    src/clockagaHostFiles.c
    src/clockwiser_state.c
    src/clockwiserDrawText.c
    src/clockwiser_movie_player.c
    src/clockaga_transpiled.c
    src/clockwiser_main.c
)

if(APPLE)
    if(EXISTS "${CLOCKWISER_MACOS_ICON}")
        add_executable(clockwiser MACOSX_BUNDLE ${SOURCES} "${CLOCKWISER_MACOS_ICON}")
    else()
        add_executable(clockwiser MACOSX_BUNDLE ${SOURCES})
    endif()
    set(_clockwiserBundleProps MACOSX_BUNDLE_BUNDLE_NAME "Clockwiser")
    if(EXISTS "${CLOCKWISER_MACOS_ICON}")
        list(APPEND _clockwiserBundleProps MACOSX_BUNDLE_ICON_FILE "AppIcon")
    endif()
    set_target_properties(clockwiser PROPERTIES ${_clockwiserBundleProps})
else()
    add_executable(clockwiser ${SOURCES} ${CLOCKWISER_WINDOWS_SOURCES})
endif()

target_compile_definitions(clockwiser PRIVATE NOTSTANDALONE)
if(MSVC)
    target_compile_options(clockwiser PRIVATE /W4)
else()
    target_compile_options(clockwiser PRIVATE -Wall -Wextra)
endif()

if(TARGET SDL2::SDL2main)
    target_link_libraries(clockwiser PRIVATE SDL2::SDL2main SDL2::SDL2 ${CLOCKWISER_SDL2_IMAGE_LIB})
else()
    target_link_libraries(clockwiser PRIVATE SDL2::SDL2 ${CLOCKWISER_SDL2_IMAGE_LIB})
endif()

if(HAVE_SDL2_MIXER)
    target_compile_definitions(clockwiser PRIVATE AMIGA_HOST_HAVE_SDL2_MIXER)
    target_link_libraries(clockwiser PRIVATE SDL2_mixer::SDL2_mixer)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(clockwiser PRIVATE m)
endif()

if(MSVC)
    set(clockwiserTranspiledWarnFlags "/w")
else()
    set(clockwiserTranspiledWarnFlags "-w")
endif()
set_source_files_properties(
    src/clockaga_transpiled.c
    PROPERTIES COMPILE_FLAGS "${clockwiserTranspiledWarnFlags}"
)

if(APPLE)
    add_custom_command(TARGET clockwiser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            $<TARGET_FILE_DIR:clockwiser>/data
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data
            $<TARGET_BUNDLE_CONTENT_DIR:clockwiser>/Resources/data
        COMMENT "Placing data/ into clockwiser.app/Contents/Resources for local testing and signing"
    )
else()
    add_custom_command(TARGET clockwiser POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/data
            $<TARGET_FILE_DIR:clockwiser>/data
        COMMENT "Copying data/ to build output"
    )
endif()

if(APPLE)
    install(TARGETS clockwiser
        BUNDLE DESTINATION .
        RUNTIME DESTINATION .
    )
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/data
        DESTINATION "clockwiser.app/Contents/Resources"
    )
else()
    install(TARGETS clockwiser
        RUNTIME DESTINATION .
    )
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/data
        DESTINATION .
    )
endif()

set(CLOCKWISER_MAC_CODESIGN_IDENTITY "$ENV{CLOCKWISER_MAC_CODESIGN_IDENTITY}" CACHE STRING
    "macOS code-sign identity for packaged binaries (example: Developer ID Application: Company (TEAMID))")
if(CLOCKWISER_MAC_CODESIGN_IDENTITY STREQUAL "" AND NOT "$ENV{HOI_MAC_CODESIGN_IDENTITY}" STREQUAL "")
    set(CLOCKWISER_MAC_CODESIGN_IDENTITY "$ENV{HOI_MAC_CODESIGN_IDENTITY}" CACHE STRING
        "macOS code-sign identity for packaged binaries (example: Developer ID Application: Company (TEAMID))"
        FORCE)
endif()
set(HOI_MAC_CODESIGN_IDENTITY "$ENV{HOI_MAC_CODESIGN_IDENTITY}" CACHE STRING
    "Deprecated; use -DCLOCKWISER_MAC_CODESIGN_IDENTITY=...")
if(CLOCKWISER_MAC_CODESIGN_IDENTITY STREQUAL "" AND NOT HOI_MAC_CODESIGN_IDENTITY STREQUAL "")
    set(CLOCKWISER_MAC_CODESIGN_IDENTITY "${HOI_MAC_CODESIGN_IDENTITY}" CACHE STRING
        "macOS code-sign identity for packaged binaries (example: Developer ID Application: Company (TEAMID))"
        FORCE)
endif()

if(APPLE)
    set(CLOCKWISER_MAC_CPACK_NOTARIZE OFF CACHE BOOL
        "After CPack ZIP: submit to Apple notarytool and staple (notary build dirs only)")
else()
    set(CLOCKWISER_MAC_CPACK_NOTARIZE OFF)
endif()

if(APPLE)
    set(CLOCKWISER_MAC_NOTARIZE OFF CACHE STRING "Deprecated; use -DCLOCKWISER_MAC_CPACK_NOTARIZE=ON.")
    string(TOUPPER "${CLOCKWISER_MAC_NOTARIZE}" _clockwiserLegacyNotarizeUpper)
    if(_clockwiserLegacyNotarizeUpper STREQUAL "ON"
            OR _clockwiserLegacyNotarizeUpper STREQUAL "1"
            OR _clockwiserLegacyNotarizeUpper STREQUAL "TRUE"
            OR _clockwiserLegacyNotarizeUpper STREQUAL "YES")
        message(STATUS "Clockwiser: CLOCKWISER_MAC_NOTARIZE=ON maps to CLOCKWISER_MAC_CPACK_NOTARIZE=ON")
        set(CLOCKWISER_MAC_CPACK_NOTARIZE ON CACHE BOOL
            "After CPack ZIP: submit to Apple notarytool and staple (notary build dirs only)"
            FORCE)
        set(CLOCKWISER_MAC_NOTARIZE OFF CACHE STRING "Deprecated; use -DCLOCKWISER_MAC_CPACK_NOTARIZE=ON." FORCE)
    endif()
    set(HOI_MAC_CPACK_NOTARIZE OFF CACHE STRING "Deprecated; use -DCLOCKWISER_MAC_CPACK_NOTARIZE=ON.")
    string(TOUPPER "${HOI_MAC_CPACK_NOTARIZE}" _hoiLegacyNotarizeUpper)
    if(_hoiLegacyNotarizeUpper STREQUAL "ON"
            OR _hoiLegacyNotarizeUpper STREQUAL "1"
            OR _hoiLegacyNotarizeUpper STREQUAL "TRUE"
            OR _hoiLegacyNotarizeUpper STREQUAL "YES")
        message(STATUS "Clockwiser: HOI_MAC_CPACK_NOTARIZE=ON maps to CLOCKWISER_MAC_CPACK_NOTARIZE=ON")
        set(CLOCKWISER_MAC_CPACK_NOTARIZE ON CACHE BOOL
            "After CPack ZIP: submit to Apple notarytool and staple (notary build dirs only)"
            FORCE)
        set(HOI_MAC_CPACK_NOTARIZE OFF CACHE STRING "Deprecated; use -DCLOCKWISER_MAC_CPACK_NOTARIZE=ON." FORCE)
    endif()
endif()

set(CLOCKWISER_MAC_NOTARY_APPLE_ID "$ENV{CLOCKWISER_MAC_NOTARY_APPLE_ID}" CACHE STRING
    "Apple ID email for notarytool")
if(CLOCKWISER_MAC_NOTARY_APPLE_ID STREQUAL "" AND NOT "$ENV{HOI_MAC_NOTARY_APPLE_ID}" STREQUAL "")
    set(CLOCKWISER_MAC_NOTARY_APPLE_ID "$ENV{HOI_MAC_NOTARY_APPLE_ID}" CACHE STRING
        "Apple ID email for notarytool" FORCE)
endif()
set(HOI_MAC_NOTARY_APPLE_ID "$ENV{HOI_MAC_NOTARY_APPLE_ID}" CACHE STRING
    "Deprecated; use -DCLOCKWISER_MAC_NOTARY_APPLE_ID=...")
if(CLOCKWISER_MAC_NOTARY_APPLE_ID STREQUAL "" AND NOT HOI_MAC_NOTARY_APPLE_ID STREQUAL "")
    set(CLOCKWISER_MAC_NOTARY_APPLE_ID "${HOI_MAC_NOTARY_APPLE_ID}" CACHE STRING
        "Apple ID email for notarytool" FORCE)
endif()

set(CLOCKWISER_MAC_NOTARY_TEAM_ID "$ENV{CLOCKWISER_MAC_NOTARY_TEAM_ID}" CACHE STRING
    "Apple Team ID for notarytool")
if(CLOCKWISER_MAC_NOTARY_TEAM_ID STREQUAL "" AND NOT "$ENV{HOI_MAC_NOTARY_TEAM_ID}" STREQUAL "")
    set(CLOCKWISER_MAC_NOTARY_TEAM_ID "$ENV{HOI_MAC_NOTARY_TEAM_ID}" CACHE STRING
        "Apple Team ID for notarytool" FORCE)
endif()
set(HOI_MAC_NOTARY_TEAM_ID "$ENV{HOI_MAC_NOTARY_TEAM_ID}" CACHE STRING
    "Deprecated; use -DCLOCKWISER_MAC_NOTARY_TEAM_ID=...")
if(CLOCKWISER_MAC_NOTARY_TEAM_ID STREQUAL "" AND NOT HOI_MAC_NOTARY_TEAM_ID STREQUAL "")
    set(CLOCKWISER_MAC_NOTARY_TEAM_ID "${HOI_MAC_NOTARY_TEAM_ID}" CACHE STRING
        "Apple Team ID for notarytool" FORCE)
endif()

set(CLOCKWISER_MAC_NOTARY_PASSWORD "$ENV{CLOCKWISER_MAC_NOTARY_PASSWORD}" CACHE STRING
    "App-specific password for notarytool")
if(CLOCKWISER_MAC_NOTARY_PASSWORD STREQUAL "" AND NOT "$ENV{HOI_MAC_NOTARY_PASSWORD}" STREQUAL "")
    set(CLOCKWISER_MAC_NOTARY_PASSWORD "$ENV{HOI_MAC_NOTARY_PASSWORD}" CACHE STRING
        "App-specific password for notarytool" FORCE)
endif()
set(HOI_MAC_NOTARY_PASSWORD "$ENV{HOI_MAC_NOTARY_PASSWORD}" CACHE STRING
    "Deprecated; use -DCLOCKWISER_MAC_NOTARY_PASSWORD=... (HOI name kept for old cmake one-liners)")
if(CLOCKWISER_MAC_NOTARY_PASSWORD STREQUAL "" AND NOT HOI_MAC_NOTARY_PASSWORD STREQUAL "")
    set(CLOCKWISER_MAC_NOTARY_PASSWORD "${HOI_MAC_NOTARY_PASSWORD}" CACHE STRING
        "App-specific password for notarytool" FORCE)
endif()

set(CLOCKWISER_RUNTIME_DEP_SEARCH_DIRS "")
if(APPLE)
    list(APPEND CLOCKWISER_RUNTIME_DEP_SEARCH_DIRS /opt/homebrew/lib /usr/local/lib)
elseif(UNIX)
    list(APPEND CLOCKWISER_RUNTIME_DEP_SEARCH_DIRS /usr/local/lib /usr/lib /lib)
endif()

list(REMOVE_DUPLICATES CLOCKWISER_RUNTIME_DEP_SEARCH_DIRS)
string(REPLACE ";" "\n" CLOCKWISER_RUNTIME_DEP_SEARCH_DIRS_LINES "${CLOCKWISER_RUNTIME_DEP_SEARCH_DIRS}")

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/install_runtime_deps.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/install_runtime_deps.cmake"
    @ONLY
)

option(CLOCKWISER_BUNDLE_DEPS "Bundle runtime dependencies into clockwiser.app and rewrite install names" ON)
if(CLOCKWISER_BUNDLE_DEPS)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_runtime_deps.cmake")
endif()

set(CPACK_PACKAGE_NAME "clockwiser")
set(CPACK_PACKAGE_VENDOR "clockwiser-portable")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Portable Clockwiser game package")
set(CPACK_PACKAGE_FILE_NAME "clockwiser-portable-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
else()
    set(CPACK_GENERATOR "ZIP;TGZ")
endif()
if(APPLE AND CLOCKWISER_MAC_CPACK_NOTARIZE)
    set(CPACK_GENERATOR "ZIP")
endif()

if(APPLE)
    set(_clockwiserNbspUtf8 "")
    if(EXISTS "${CMAKE_SOURCE_DIR}/cmake/clockwiser_nbsp.utf8")
        file(READ "${CMAKE_SOURCE_DIR}/cmake/clockwiser_nbsp.utf8" _clockwiserNbspUtf8)
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/cmake/hoi_nbsp.utf8")
        file(READ "${CMAKE_SOURCE_DIR}/cmake/hoi_nbsp.utf8" _clockwiserNbspUtf8)
    endif()
    set(_clockwiserPkgApple "${CLOCKWISER_MAC_NOTARY_APPLE_ID}")
    set(_clockwiserPkgTeam "${CLOCKWISER_MAC_NOTARY_TEAM_ID}")
    set(_clockwiserPkgPass "${CLOCKWISER_MAC_NOTARY_PASSWORD}")
    if(NOT _clockwiserNbspUtf8 STREQUAL "")
        string(REPLACE "${_clockwiserNbspUtf8}" "" _clockwiserPkgApple "${_clockwiserPkgApple}")
        string(REPLACE "${_clockwiserNbspUtf8}" "" _clockwiserPkgTeam "${_clockwiserPkgTeam}")
        string(REPLACE "${_clockwiserNbspUtf8}" "" _clockwiserPkgPass "${_clockwiserPkgPass}")
    endif()
    string(STRIP "${_clockwiserPkgApple}" CLOCKWISER_MAC_NOTARY_APPLE_ID)
    string(STRIP "${_clockwiserPkgTeam}" CLOCKWISER_MAC_NOTARY_TEAM_ID)
    string(STRIP "${_clockwiserPkgPass}" CLOCKWISER_MAC_NOTARY_PASSWORD)
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/cpack_notarize_macos.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cpack_notarize_macos.cmake"
        @ONLY
    )
    if(CLOCKWISER_MAC_CPACK_NOTARIZE)
        set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_CURRENT_BINARY_DIR}/cpack_notarize_macos.cmake")
    endif()
endif()
include(CPack)
