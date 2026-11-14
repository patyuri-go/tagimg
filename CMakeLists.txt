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
find_package(SOCI 4.1 COMPONENTS PostgreSQL CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_library(tagimg_ai STATIC
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
    src/common/uuid.cpp
    src/common/validation.cpp
)
target_include_directories(tagimg_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(tagimg_db STATIC
    src/db/entity_repository.cpp
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

add_executable(tagimg_entity_lookup
    tools/entity_lookup.cpp
)
target_link_libraries(tagimg_entity_lookup PRIVATE
    tagimg_db
    tagimg_config
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

add_library(tagimg_job STATIC
    src/job/add_job.cpp
    src/job/job.cpp
    src/job/job_worker.cpp
    src/job/scan_job.cpp
    src/job/scan_service.cpp
)
target_include_directories(tagimg_job PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_job PUBLIC
    tagimg_ai
    tagimg_db
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
    tagimg_job
    Drogon::Drogon
    nlohmann_json::nlohmann_json
)

add_library(tagimg_graphql STATIC
    src/graphql/ArtistObject.cpp
    src/graphql/ArtworkConnectionObject.cpp
    src/graphql/ArtworkObject.cpp
    src/graphql/DeleteArtworkResultObject.cpp
    src/graphql/MutationObject.cpp
    src/graphql/OrphanImageConnectionObject.cpp
    src/graphql/OrphanImageObject.cpp
    src/graphql/QueryObject.cpp
    src/graphql/TagMatchObject.cpp
    src/graphql/TagObject.cpp
    src/graphql/TagPathObject.cpp
    src/graphql/TagimgSchema.cpp
    src/graphql/graphql_service.cpp
)
target_include_directories(tagimg_graphql PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(tagimg_graphql PUBLIC
    tagimg_job
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

message(STATUS "tagimg: C++ dependencies resolved")
