cmake_minimum_required(VERSION 3.19)

# Single-config generators (Makefiles, Ninja) default CMAKE_BUILD_TYPE to an
# empty string -- meaning NO optimization flags at all (effectively -O0) --
# unless something sets it. Nothing here (or in Min-API's scripts) does, so
# every build was silently unoptimized until this was added. Multi-config
# generators (Xcode) pick Debug/Release via scheme instead and aren't
# affected by CMAKE_BUILD_TYPE, hence the CMAKE_CONFIGURATION_TYPES check.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# min-api/script/min-pretarget.cmake (included below) defaults
# CMAKE_OSX_ARCHITECTURES to "x86_64" only if it's still empty at that point.
# Modern Max runs natively as arm64 on Apple Silicon and won't load an
# x86_64-only external there ("incorrect architecture" at load time) unless
# Max itself is running under Rosetta. Pre-seed a universal build so the
# default `cmake ..` (no args) produces something that loads on both --
# still overridable with -DCMAKE_OSX_ARCHITECTURES=... if you want a single
# -arch build.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")

project(jit.decklink)

# Set path to min-api
set(MIN_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/min-api)

# max-sdk-base's max-pretarget.cmake (included below, via min-pretarget.cmake)
# calls project() again itself, using this folder's name -- overriding
# whatever we passed to project() above -- and from then on uses
# "${PROJECT_NAME}_EXTERN_OUTPUT_NAME" (a CACHE var it initializes, but only
# if not already set) as the actual name of the built external/bundle. Since
# this repo's folder is "jit.decklink" (no tilde -- it's a git repo name, not
# a Max object name, and doesn't match the actual object name below either),
# pre-seed that cache var ourselves so the object comes out named
# "jit.decklink.send~" as intended, mirroring max-pretarget.cmake's own
# "~" -> "_tilde" folder-name convention so this still works if the repo is
# ever cloned under a differently-named folder.
string(REGEX REPLACE "(.*)/" "" _jit_decklink_folder_name "${CMAKE_CURRENT_SOURCE_DIR}")
string(REPLACE "~" "_tilde" _jit_decklink_folder_name "${_jit_decklink_folder_name}")
set("${_jit_decklink_folder_name}_EXTERN_OUTPUT_NAME" "jit.decklink.send~"
    CACHE STRING "The name to give to the external output file/directory")

# Pre-target setup
# ---------------------------------------------------------------------------
# Max package layout
#
# Everything a Max package needs is assembled under build/jit.decklink/, so that one
# directory *is* the package and can be symlinked straight into
# ~/Documents/Max 9/Packages/. Previously the externals landed in a build
# subfolder and the package was wired into Max by hand, which meant a stale
# .mxo was always one forgotten copy away.
#
# Link it into Max with:
#   ln -s "$(pwd)/build/jit.decklink" ~/Documents/"Max 9"/Packages/jit.decklink
# ---------------------------------------------------------------------------
set(C74_PACKAGE_NAME "jit.decklink")
set(C74_PACKAGE_DIR  "${CMAKE_BINARY_DIR}/${C74_PACKAGE_NAME}")
set(C74_EXTERNALS_DIR "${C74_PACKAGE_DIR}/externals")

file(MAKE_DIRECTORY "${C74_EXTERNALS_DIR}")

# Symlink rather than copy the authored folders: a copy goes stale the moment
# a patcher is edited, and these are the files most likely to be edited while
# Max is open.
foreach(_authored patchers docs extras help media init)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_authored}")
        file(CREATE_LINK
            "${CMAKE_CURRENT_SOURCE_DIR}/${_authored}"
            "${C74_PACKAGE_DIR}/${_authored}"
            SYMBOLIC)
    endif()
endforeach()

file(WRITE "${C74_PACKAGE_DIR}/package-info.json"
"{
\t\"name\": \"jit.decklink\",
\t\"version\": \"0.1.0\",
\t\"author\": \"Cameron Johnston\",
\t\"description\": \"Send Jitter video and MC audio to Blackmagic DeckLink devices.\",
\t\"tags\": ["jitter", "video", "audio", "decklink", "blackmagic"]
}
")

set(C74_LIBRARY_OUTPUT_DIRECTORY "${C74_EXTERNALS_DIR}")
include(${MIN_API_DIR}/script/min-pretarget.cmake)

# Source files
# NOTE: the .cpp is named jit.decklink.send_tilde.cpp -- Min-API's
# MIN_EXTERNAL macro derives the Max object name from the source filename,
# substituting a "~" for a "_tilde" suffix, so this produces an object
# named "jit.decklink.send~".
#
# A future jit.decklink.receive~ (input, for capture devices) would be a
# second, separate external -- its own source/jit.decklink.receive_tilde.cpp,
# its own add_library() target below (with its own _EXTERN_OUTPUT_NAME
# override above, keyed off that target's own PROJECT_NAME), not an addition
# to this one. Not built yet; see the README.
set(SOURCE_FILES
    source/jit.decklink.send_tilde.cpp
)

# The Blackmagic DeckLink SDK headers (and the Mac/Linux DeckLinkAPIDispatch.cpp
# shim, which resolves the driver's entry points dynamically at runtime via
# CFBundle/dlopen -- no DeckLink .dylib/.framework is needed at link time).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/DeckLinkAPIDispatch.cpp")
    list(APPEND SOURCE_FILES include/DeckLinkAPIDispatch.cpp)
endif()

# Create the Max external (module)
add_library(${PROJECT_NAME} MODULE ${SOURCE_FILES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${MIN_API_DIR}/include
    ${C74_INCLUDES}
)

# DeckLinkAPIDispatch.cpp resolves the installed DeckLink driver's entry
# points dynamically at runtime (via CFBundle), but still needs to link
# against CoreFoundation itself for that lookup machinery.
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE "-framework CoreFoundation")
endif()

# Post-target setup
include(${MIN_API_DIR}/script/min-posttarget.cmake)
