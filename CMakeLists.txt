cmake_minimum_required(VERSION 3.20)

# vcpkg 在增量重新生成构建文件时不会继承 CMakePresets 里的环境变量，
# 这里显式设置，避免它尝试下载自带的 CMake 而失败。
set(ENV{VCPKG_FORCE_SYSTEM_BINARIES} 1)

project(tagimg LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cppgraphqlgen CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
# Drogon 的 FindSQLite3.cmake 只创建 SQLite3_lib，而 SOCI 的 SQLite3
# 组件要求 SQLite::SQLite3。这里先解析并补齐该 target。
find_package(SQLite3 REQUIRED)
if(NOT TARGET SQLite::SQLite3)
    find_library(TAGIMG_SQLITE3_LIBRARY NAMES sqlite3 REQUIRED)
    add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
    set_target_properties(SQLite::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${TAGIMG_SQLITE3_LIBRARY}")
endif()
find_package(SOCI 4.1 COMPONENTS PostgreSQL SQLite3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# vcpkg 的 libvips 只安装 pkg-config 文件，没有 CMake config。
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(ENV{PKG_CONFIG_PATH}
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(VIPS REQUIRED IMPORTED_TARGET vips)

# 动图 AVIF 序列编码需要直接调用 libheif（vips 的 heifsave 不写帧时长/循环）。
find_package(libheif CONFIG REQUIRED)

add_library(tagimg_ai STATIC
    src/ai/clip_embedder.cpp
    src/ai/health.cpp
    src/ai/sidecar_process.cpp
    src/ai/tagger.cpp
)
target_include_directories(tagimg_ai PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_ai PUBLIC
    Drogon::Drogon
    nlohmann_json::nlohmann_json
)

add_executable(tagimg_tag_smoke
    tools/tag_smoke.cpp
)
target_link_libraries(tagimg_tag_smoke PRIVATE
    tagimg_ai
)

add_executable(tagimg_clip_smoke
    tools/clip_smoke.cpp
)
target_link_libraries(tagimg_clip_smoke PRIVATE
    tagimg_ai
)

add_executable(tagimg_clip_db_smoke
    tools/clip_db_smoke.cpp
)
target_link_libraries(tagimg_clip_db_smoke PRIVATE
    tagimg_config
    tagimg_db
)

add_executable(tagimg_rescan_db_smoke
    tools/rescan_db_smoke.cpp
)
target_link_libraries(tagimg_rescan_db_smoke PRIVATE
    tagimg_config
    tagimg_db
)

add_library(tagimg_config STATIC
    src/common/config.cpp
)
target_include_directories(tagimg_config PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_config PUBLIC
    nlohmann_json::nlohmann_json
)

add_library(tagimg_common STATIC
    src/common/image_size.cpp
    src/common/storage.cpp
    src/common/uuid.cpp
    src/common/validation.cpp
)
target_include_directories(tagimg_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(tagimg_request STATIC
    src/http/request_context.cpp
)
target_include_directories(tagimg_request PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_request PUBLIC
    Drogon::Drogon
)

add_library(tagimg_db STATIC
    src/db/tagimg_repository.cpp
)
target_include_directories(tagimg_db PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_db PUBLIC
    tagimg_common
    SOCI::Core
    SOCI::PostgreSQL
)

add_library(tagimg_io STATIC
    src/io/byte_stream.cpp
    src/io/temp_file.cpp
)
target_include_directories(tagimg_io PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_io PUBLIC
    CURL::libcurl
)

add_library(tagimg_image STATIC
    src/image/animated_avif.cpp
    src/image/image_metadata.cpp
    src/image/image_processor.cpp
)
target_include_directories(tagimg_image PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_image PUBLIC
    tagimg_common
    PkgConfig::VIPS
    heif
)

add_library(tagimg_cache STATIC
    src/cache/image_cache_key.cpp
    src/cache/image_cache_keys.cpp
    src/cache/image_cache_object.cpp
    src/cache/image_cache_store.cpp
)
target_include_directories(tagimg_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_cache PUBLIC
    tagimg_common
    tagimg_config
    tagimg_image
    SOCI::Core
    SOCI::SQLite3
    # SOCI 的组件配置在 SQLite::SQLite3 已存在时不会把它追加进
    # SOCI::SQLite3 的链接接口，这里显式补上。
    SQLite::SQLite3
    OpenSSL::Crypto
    Threads::Threads
)

add_library(tagimg_job STATIC
    src/job/add_job.cpp
    src/job/ai_preflight.cpp
    src/job/cache_maintenance.cpp
    src/job/job.cpp
    src/job/job_worker.cpp
    src/job/rescan_artwork_job.cpp
    src/job/scan_job.cpp
    src/job/service_runtime.cpp
)
target_include_directories(tagimg_job PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_job PUBLIC
    tagimg_ai
    tagimg_cache
    tagimg_db
    tagimg_image
    tagimg_io
    Threads::Threads
)

add_executable(tagimg_scan_smoke
    tools/scan_smoke.cpp
)
target_link_libraries(tagimg_scan_smoke PRIVATE
    tagimg_config
    tagimg_job
)

add_executable(tagimg_job_smoke
    tools/job_smoke.cpp
)
target_link_libraries(tagimg_job_smoke PRIVATE
    tagimg_config
    tagimg_job
)

add_library(tagimg_http STATIC
    src/http/api_service.cpp
    src/http/image_stream.cpp
    src/http/json_helpers.cpp
)
target_include_directories(tagimg_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_http PUBLIC
    tagimg_cache
    tagimg_image
    tagimg_job
    tagimg_request
    Drogon::Drogon
    nlohmann_json::nlohmann_json
)

add_library(tagimg_graphql STATIC
    src/graphql/ArtistObject.cpp
    src/graphql/ArtworkConnectionObject.cpp
    src/graphql/ArtworkObject.cpp
    src/graphql/DeleteArtworkResultObject.cpp
    src/graphql/ImageArtworkObject.cpp
    src/graphql/ImageConnectionObject.cpp
    src/graphql/ImageObject.cpp
    src/graphql/MutationObject.cpp
    src/graphql/OrphanImageConnectionObject.cpp
    src/graphql/OrphanImageObject.cpp
    src/graphql/QueryObject.cpp
    src/graphql/TagMatchObject.cpp
    src/graphql/TagObject.cpp
    src/graphql/TagPathObject.cpp
    src/graphql/TagTreeMatchObject.cpp
    src/graphql/TagTreeNodeObject.cpp
    src/graphql/TagimgSchema.cpp
    src/graphql/graphql_service.cpp
)
target_include_directories(tagimg_graphql PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_graphql PUBLIC
    tagimg_job
    tagimg_request
    cppgraphqlgen::graphqlservice
    cppgraphqlgen::graphqljson
    nlohmann_json::nlohmann_json
)

add_executable(tagimg_server
    src/server/main.cpp
)
target_link_libraries(tagimg_server PRIVATE
    tagimg_graphql
    tagimg_http
    tagimg_config
)

add_executable(tagimg_vips_smoke
    tools/vips_smoke.cpp
)
target_link_libraries(tagimg_vips_smoke PRIVATE
    tagimg_image
)

add_executable(tagimg_animated_avif_smoke
    tools/animated_avif_smoke.cpp
)
target_link_libraries(tagimg_animated_avif_smoke PRIVATE
    tagimg_image
)

add_executable(tagimg_cache_smoke
    tools/cache_smoke.cpp
)
target_link_libraries(tagimg_cache_smoke PRIVATE
    tagimg_cache
    tagimg_config
)

message(STATUS "tagimg: C++ dependencies resolved")
