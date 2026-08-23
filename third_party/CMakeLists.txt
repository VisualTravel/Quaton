# Third-party libraries configuration
# All packages are resolved through the vcpkg toolchain, which automatically
# adds the build directory's vcpkg_installed/<triplet> to the search path, so
# the same file works for both static and shared builds in any build tree.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "D:/vcpkg/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(CURL REQUIRED)
find_package(zstd REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(xxhash CONFIG REQUIRED)

# SQLite3 (may be exposed as official or unofficial config)
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    find_package(unofficial-sqlite3 CONFIG QUIET)
    if(unofficial-sqlite3_FOUND)
        set(SQLite3_FOUND TRUE)
        set(SQLite3_LIBRARIES unofficial::sqlite3::sqlite3)
    endif()
endif()

if(NOT SQLite3_FOUND)
    message(STATUS "SQLite3 not found via find_package, will use vcpkg")
    set(SQLite3_LIBRARIES sqlite3)
endif()

# zstd exposes libzstd_static for the static triplet and libzstd_shared for
# the dynamic triplet. Pick whichever the installed build provides.
if(TARGET zstd::libzstd_static)
    set(ZSTD_TARGET zstd::libzstd_static)
else()
    set(ZSTD_TARGET zstd::libzstd_shared)
endif()

set(THIRD_PARTY_LIBRARIES
    ZLIB::ZLIB
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${ZSTD_TARGET}
    libzip::zip
    xxHash::xxhash
    ${SQLite3_LIBRARIES}
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

# ---------------------------------------------------------------------------
# HDiffPatch: vendored third_party/HDiffPatch (MIT license). Provides the
# patch-apply routines previously invoked through the external hpatchz.exe, so
# Quaton becomes self-contained. patch.c references the multi-threaded symbols
# even in single-threaded use, so the whole hpatch_mt set plus the platform
# thread abstraction (libParallel/parallel_import_c.c) must be compiled.
# _IS_USED_WIN32THREAD picks the native Windows thread backend.
# ---------------------------------------------------------------------------
set(HDIFFPATCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/HDiffPatch")
set(HDIFFPATCH_SOURCES
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/patch.c"
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/hpatch_mt.c"
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hcache_old_mt.c"
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hcache_window_old_mt.c"
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hinput_mt.c"
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_houtput_mt.c"
    "${HDIFFPATCH_ROOT}/libHDiffPatch/HPatch/hpatch_mt/_hpatch_mt.c"
    "${HDIFFPATCH_ROOT}/libParallel/parallel_import_c.c"
    "${HDIFFPATCH_ROOT}/file_for_patch.c"
)
set(HDIFFPATCH_INCLUDE_DIRS
    "${HDIFFPATCH_ROOT}"
    "${HDIFFPATCH_ROOT}/libHDiffPatch"
    "${HDIFFPATCH_ROOT}/libParallel"
)