cmake_minimum_required(VERSION 3.20)
project(Quaton VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    if(MSVC)
        add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
        add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
    endif()
endif()

include(third_party/CMakeLists.txt)


# Source files
set(SOURCES
    src/quaton/quaton.cc
    src/quaton/url.cc
    src/quaton/resource.cc
    src/quaton/configuration/path_config.cc
    src/quaton/base/file_writer.cc
    src/quaton/base/task_queue.cc
    src/quaton/database/connection.cc
    src/quaton/database/transaction.cc
    src/quaton/database/chunk_db.cc
    src/quaton/database/config_db.cc
    src/quaton/database/manifest_db.cc
    src/quaton/database/database_interface.cc
    src/quaton/database/database_manager.cc
    src/quaton/downloader/downloader_utils.cc
    src/quaton/downloader/speed_limiter.cc
    src/quaton/progress_callback.cc
    src/quaton/utils/checksum.cc
    src/quaton/utils/md5.cc
    src/quaton/utils/hpatch.cc
    src/quaton/utils/path_helper.cc
    src/quaton/utils/compression.cc
    src/quaton/thread_pool.cc
    src/quaton/manifest/types.cc
    src/quaton/manifest/manifest_metadata.cc
    src/quaton/manifest/chunk_manifest_pair.cc
    src/quaton/manifest/manifest_processor.cc
    src/quaton/manifest/manifest_processor_patch.cc
    src/quaton/manifest/json_parser.cc
    src/quaton/patch.cc
    src/quaton/http_client.cc
    src/quaton/logger.cc
    src/quaton/data_chunk.cc
    src/quaton/patch_applier.cc
    src/quaton/base/download_item.cc
    src/quaton/base/download_job.cc
    src/quaton/base/download_scheduler.cc
    src/quaton/base/download_service.cc
    src/quaton/base/download_manager_base.cc
    src/quaton/chunk/chunk_download_item.cc
    src/quaton/chunk/chunk_download_job.cc
    src/quaton/chunk/chunk_download_manager.cc
    src/quaton/chunk/chunk_download_service.cc
    src/quaton/patch/patch_download_item.cc
    src/quaton/patch/patch_download_job.cc
    src/quaton/patch/patch_download_manager.cc
    src/quaton/patch/patch_download_service.cc
    ${HDIFFPATCH_SOURCES}
)

# Header files
set(HEADERS
    include/quaton/quaton_global.h
    include/quaton/quaton.h
    include/quaton/url.h
    include/quaton/resource.h
    include/quaton/configuration/configuration.h
    include/quaton/configuration/http_config.h
    include/quaton/configuration/download_config.h
    include/quaton/configuration/database_config.h
    include/quaton/configuration/path_config.h
    include/quaton/base/file_writer.h
    include/quaton/base/task_queue.h
    include/quaton/database/records.h
    include/quaton/database/connection.h
    include/quaton/database/transaction.h
    include/quaton/database/chunk_db.h
    include/quaton/database/config_db.h
    include/quaton/database/manifest_db.h
    include/quaton/database/database_interface.h
    include/quaton/database/database_manager.h
    include/quaton/downloader/downloader_utils.h
    include/quaton/downloader/speed_limiter.h
    include/quaton/progress_callback.h
    include/quaton/thread_pool.h
    include/quaton/manifest/types.h
    include/quaton/manifest/build_api.h
    include/quaton/manifest/patch_api.h
    include/quaton/manifest/json_parser.h
    include/quaton/manifest/manifest_processor.h
    include/quaton/manifest/manifest_processor_patch.h
    include/quaton/manifest/manifest_metadata.h
    include/quaton/manifest/chunk_manifest_pair.h
    include/quaton/patch.h
    include/quaton/logger.h
    include/quaton/data_chunk.h
    include/quaton/patch_applier.h
    include/quaton/protozero_parser.hpp
    include/quaton/http_client.h
    include/quaton/utils/checksum.h
    include/quaton/utils/md5.h
    include/quaton/utils/hpatch.h
    include/quaton/utils/retry_helper.h
    include/quaton/utils/path_helper.h
    include/quaton/utils/compression.h
    include/quaton/base/download_state.h
    include/quaton/base/download_item.h
    include/quaton/base/download_job.h
    include/quaton/base/download_scheduler.h
    include/quaton/base/download_service.h
    include/quaton/base/download_manager_base.h
    include/quaton/chunk/chunk_download_item.h
    include/quaton/chunk/chunk_download_job.h
    include/quaton/chunk/chunk_download_manager.h
    include/quaton/chunk/chunk_download_service.h
    include/quaton/patch/patch_download_item.h
    include/quaton/patch/patch_download_job.h
    include/quaton/patch/patch_download_manager.h
    include/quaton/patch/patch_download_service.h
)

if(QUATON_BUILD_SHARED)
    add_library(Quaton SHARED ${SOURCES} ${HEADERS})
else()
    add_library(Quaton STATIC ${SOURCES} ${HEADERS})
endif()

target_include_directories(Quaton PRIVATE include)
target_include_directories(Quaton PRIVATE ${HDIFFPATCH_INCLUDE_DIRS})

# HDiffPatch multi-threaded patch engine runs on native Windows threads.
target_compile_definitions(Quaton PRIVATE _IS_USED_WIN32THREAD=1)

if(QUATON_BUILD_SHARED)
    target_compile_definitions(Quaton PRIVATE
        QUATON_EXPORTS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    # QUATON_STATIC must propagate to consumers so their compile of
    # quaton_global.h yields no dllimport/dllexport symbols.
    target_compile_definitions(Quaton PUBLIC
        QUATON_STATIC
    )
    target_compile_definitions(Quaton PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

find_path(PROTOZERO_INCLUDE_DIRS "protozero/pbf_reader.hpp")
if(PROTOZERO_INCLUDE_DIRS)
    message(STATUS "Found protozero: ${PROTOZERO_INCLUDE_DIRS}")
    target_include_directories(Quaton PRIVATE ${PROTOZERO_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "protozero not found. Please install it via vcpkg: vcpkg install protozero")
endif()

target_link_libraries(Quaton
    PRIVATE
    ${THIRD_PARTY_LIBRARIES}
)

set_target_properties(Quaton PROPERTIES
    OUTPUT_NAME "Quaton"
)
if(QUATON_BUILD_SHARED)
    set_target_properties(Quaton PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

install(TARGETS Quaton
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/quaton/
    DESTINATION include/quaton
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)


if(WIN32)

    set(VCPKG_BIN_DIR "D:/vcpkg/installed/x64-windows/bin")
    set(LOCAL_VCPKG_BIN_DIR "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin")

    function(find_and_copy_dll DLL_NAME TARGET_DIR COMMENT_PREFIX)

        set(DLL_PATH "")
        if(EXISTS "${LOCAL_VCPKG_BIN_DIR}/${DLL_NAME}")
            set(DLL_PATH "${LOCAL_VCPKG_BIN_DIR}/${DLL_NAME}")
            message(STATUS "Found ${DLL_NAME} in local vcpkg")
        elseif(EXISTS "${VCPKG_BIN_DIR}/${DLL_NAME}")
            set(DLL_PATH "${VCPKG_BIN_DIR}/${DLL_NAME}")
            message(STATUS "Found ${DLL_NAME} in global vcpkg")
        else()
            message(WARNING "DLL not found: ${DLL_NAME}")
            return()
        endif()

        set(COPY_COMMAND
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}"
                "${TARGET_DIR}/${DLL_NAME}"
            PARENT_SCOPE
        )
    endfunction()

    set(REQUIRED_DLLS
        abseil_dll.dll
        libcurl.dll
        sqlite3.dll
        zstd.dll
        zip.dll
        bz2.dll
    )

    # In shared mode, ship the third-party DLLs next to Quaton.dll.
    if(QUATON_BUILD_SHARED)
    foreach(DLL_NAME ${REQUIRED_DLLS})
        if(EXISTS "${LOCAL_VCPKG_BIN_DIR}/${DLL_NAME}")
            set(DLL_PATH "${LOCAL_VCPKG_BIN_DIR}/${DLL_NAME}")
        elseif(EXISTS "${VCPKG_BIN_DIR}/${DLL_NAME}")
            set(DLL_PATH "${VCPKG_BIN_DIR}/${DLL_NAME}")
        else()
            message(WARNING "DLL not found: ${DLL_NAME}")
            continue()
        endif()

        add_custom_command(TARGET Quaton POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DLL_PATH}"
                "$<TARGET_FILE_DIR:Quaton>/${DLL_NAME}"
            COMMENT "Copying ${DLL_NAME}"
            VERBATIM
        )
    endforeach()

    endif()  # QUATON_BUILD_SHARED


    set_directory_properties(PROPERTIES
        ADDITIONAL_CLEAN_FILES ""
    )

    if(QUATON_BUILD_SHARED)
    install(FILES ${VCPKG_BIN_DIR}/abseil_dll.dll
                  ${VCPKG_BIN_DIR}/libcurl.dll
                  ${VCPKG_BIN_DIR}/sqlite3.dll
                  ${VCPKG_BIN_DIR}/zstd.dll
                  ${LOCAL_VCPKG_BIN_DIR}/zip.dll
                  ${LOCAL_VCPKG_BIN_DIR}/bz2.dll
            DESTINATION bin)
    endif()  # QUATON_BUILD_SHARED
endif()

