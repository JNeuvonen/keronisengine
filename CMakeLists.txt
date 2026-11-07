cmake_minimum_required(VERSION 3.24)

project(keronis_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Prices and P&L go through floating point: keep results identical across
# compilers and optimization levels.
add_compile_options(-fno-fast-math -fno-strict-aliasing -ffp-contract=off
                    -frounding-math -fno-omit-frame-pointer)

option(KERONIS_BUILD_TESTS "Build unit tests" ON)
option(KERONIS_DISK_LOGGING "Write engine logs to LOG_DIRECTORY" OFF)
set(KERONIS_STRATEGY_DIRS "" CACHE STRING
    "Extra directories of strategy and state-machine sources, ;-separated")
set(ONNXRUNTIME_ROOT "" CACHE PATH
    "ONNX Runtime install: include/ and lib/ (a release tarball works)")

if(NOT ONNXRUNTIME_ROOT)
    message(FATAL_ERROR "Set ONNXRUNTIME_ROOT to an ONNX Runtime release "
                        "(see scripts/fetch_onnxruntime.sh)")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(date CONFIG REQUIRED)
find_package(libsecp256k1 CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
find_package(ArrowDataset CONFIG REQUIRED)
find_package(ArrowCompute CONFIG REQUIRED)
find_path(ASIO_INCLUDE_DIR NAMES asio.hpp REQUIRED)
find_path(CONCURRENTQUEUE_INCLUDE_DIR NAMES concurrentqueue.h
          PATH_SUFFIXES concurrentqueue concurrentqueue/moodycamel REQUIRED)
# MPFR and GMP are LGPL: link them as shared libraries, never statically.
find_library(MPFR_LIBRARY NAMES libmpfr.so libmpfr.dylib REQUIRED)
find_library(GMP_LIBRARY NAMES libgmp.so libgmp.dylib REQUIRED)
find_library(ZMQ_LIBRARY NAMES libzmq.a zmq REQUIRED)
find_library(NATS_LIBRARY NAMES libnats_static.a nats REQUIRED)
find_library(SODIUM_LIBRARY NAMES libsodium.a sodium REQUIRED)
find_library(PGCOMMON_LIBRARY NAMES pgcommon)
find_library(PGPORT_LIBRARY NAMES pgport)
find_library(ARROW_COMPUTE_ARCHIVE NAMES libarrow_compute.a REQUIRED)
find_library(ARROW_DATASET_ARCHIVE NAMES libarrow_dataset.a REQUIRED)
find_library(ARROW_ACERO_ARCHIVE NAMES libarrow_acero.a REQUIRED)
find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
             PATHS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}
             NO_DEFAULT_PATH REQUIRED)

# Hyperliquid order signing: a Rust cdylib built from source with cargo.
set(HL_SIGNER_DIR ${CMAKE_SOURCE_DIR}/src/rust/hyperliquid_signer)
set(HL_SIGNER_TARGET_DIR ${CMAKE_BINARY_DIR}/hyperliquid_signer)
set(HL_SIGNER_LIB ${HL_SIGNER_TARGET_DIR}/release/libhyperliquid_signer.so)
add_custom_command(
    OUTPUT ${HL_SIGNER_LIB}
    COMMAND cargo build --release --manifest-path ${HL_SIGNER_DIR}/Cargo.toml
            --target-dir ${HL_SIGNER_TARGET_DIR}
    DEPENDS ${HL_SIGNER_DIR}/Cargo.toml ${HL_SIGNER_DIR}/src/lib.rs
    COMMENT "Building Hyperliquid signer")
add_custom_target(hyperliquid_signer_build DEPENDS ${HL_SIGNER_LIB})
add_library(hyperliquid_signer SHARED IMPORTED GLOBAL)
set_target_properties(hyperliquid_signer PROPERTIES
    IMPORTED_LOCATION ${HL_SIGNER_LIB}
    IMPORTED_NO_SONAME TRUE)
add_dependencies(hyperliquid_signer hyperliquid_signer_build)

set(PKG ${CMAKE_SOURCE_DIR}/src/cpp/packages)

file(GLOB KERONIS_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${PKG}/common/src/*.cpp
    ${PKG}/common/src/trading/*.cpp
    ${PKG}/trading_engine/src/*.cpp
    ${PKG}/trading_engine/src/dtypes/*.cpp
    ${PKG}/market_data/src/*.cpp
    ${PKG}/live_infra/src/*.cpp
    ${PKG}/live_infra/src/exchange_specific/*.cpp
    ${PKG}/historical_feature_gen/src/*.cpp
    ${PKG}/historical_feature_gen/src/transformations/*.cpp
    ${PKG}/strategies/*/*.cpp)
foreach(dir IN LISTS KERONIS_STRATEGY_DIRS)
    file(GLOB_RECURSE extra CONFIGURE_DEPENDS ${dir}/*.cpp)
    list(APPEND KERONIS_ENGINE_SOURCES ${extra})
endforeach()
list(FILTER KERONIS_ENGINE_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

file(GLOB ECS_SOURCES CONFIGURE_DEPENDS ${PKG}/engine_control_server/src/*.cpp)
add_library(engine_control_server STATIC ${ECS_SOURCES})
target_include_directories(engine_control_server PUBLIC
    ${PKG}/engine_control_server/include
    ${PKG}/trading_engine/include
    ${PKG}/market_data/include
    ${PKG}/common/include
    ${CMAKE_SOURCE_DIR}/src/cpp/headeronlylibs)
target_link_libraries(engine_control_server PUBLIC nlohmann_json::nlohmann_json)

add_library(keronis_engine STATIC ${KERONIS_ENGINE_SOURCES})
target_compile_definitions(keronis_engine PUBLIC
    ASIO_STANDALONE DISABLE_VISUALIZATION
    $<$<BOOL:${KERONIS_DISK_LOGGING}>:DISK_LOGGING_ENABLED=1>)
target_include_directories(keronis_engine PUBLIC
    ${PKG}
    ${PKG}/common/include
    ${PKG}/trading_engine/include
    ${PKG}/market_data/include
    ${PKG}/live_infra/include
    ${PKG}/historical_feature_gen/include
    ${PKG}/realtime_dashboard/include
    ${PKG}/engine_control_server/include
    ${CMAKE_SOURCE_DIR}/src/cpp/headeronlylibs
    ${ASIO_INCLUDE_DIR}
    ${CONCURRENTQUEUE_INCLUDE_DIR}
    ${ONNXRUNTIME_ROOT}/include
    ${ONNXRUNTIME_ROOT}/include/onnxruntime/core/session)
target_link_libraries(keronis_engine PUBLIC
    engine_control_server
    hyperliquid_signer
    OpenSSL::SSL OpenSSL::Crypto
    CURL::libcurl
    SQLite::SQLite3
    PostgreSQL::PostgreSQL
    $<$<BOOL:${PGCOMMON_LIBRARY}>:${PGCOMMON_LIBRARY}>
    $<$<BOOL:${PGPORT_LIBRARY}>:${PGPORT_LIBRARY}>
    nlohmann_json::nlohmann_json
    simdjson::simdjson
    date::date-tz
    libsecp256k1::secp256k1
    TBB::tbb
    ${MPFR_LIBRARY} ${GMP_LIBRARY}
    ${ZMQ_LIBRARY} ${NATS_LIBRARY} ${SODIUM_LIBRARY}
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,${ARROW_COMPUTE_ARCHIVE},${ARROW_DATASET_ARCHIVE},${ARROW_ACERO_ARCHIVE}>"
    arrow_static arrow_dataset_static parquet_static
    ${ONNXRUNTIME_LIBRARY}
    ZLIB::ZLIB
    Threads::Threads)

# Strategies and state machines register themselves from static
# initializers that nothing references, so every binary takes the whole
# archive; a plain static link would silently drop them.
function(keronis_link_engine target)
    target_link_libraries(${target} PRIVATE
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,keronis_engine>")
endfunction()

add_executable(live_infra ${PKG}/live_infra/src/main.cpp)
keronis_link_engine(live_infra)

add_executable(export_strategy_config ${PKG}/live_infra/tools/export_strategy_config.cpp)
keronis_link_engine(export_strategy_config)

file(GLOB BACKTEST_RUNNER_SOURCES CONFIGURE_DEPENDS ${PKG}/backtest_runner/src/*.cpp)
add_executable(backtest_runner ${BACKTEST_RUNNER_SOURCES})
target_include_directories(backtest_runner PRIVATE ${PKG}/backtest_runner/src/include)
keronis_link_engine(backtest_runner)

add_executable(historical_feature_gen ${PKG}/historical_feature_gen/src/main.cpp)
keronis_link_engine(historical_feature_gen)

if(KERONIS_BUILD_TESTS)
    enable_testing()
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

    file(GLOB TRADING_ENGINE_TESTS CONFIGURE_DEPENDS ${PKG}/trading_engine/tests/*.cpp)
    add_executable(trading_engine_tests ${TRADING_ENGINE_TESTS})
    keronis_link_engine(trading_engine_tests)
    target_link_libraries(trading_engine_tests PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(trading_engine_tests)

    file(GLOB LIVE_INFRA_TESTS CONFIGURE_DEPENDS ${PKG}/live_infra/tests/*.cpp)
    add_executable(live_infra_tests ${LIVE_INFRA_TESTS})
    target_compile_definitions(live_infra_tests PRIVATE
        SOURCE_ROOT="${CMAKE_SOURCE_DIR}/src/cpp")
    keronis_link_engine(live_infra_tests)
    target_link_libraries(live_infra_tests PRIVATE Catch2::Catch2WithMain)
    catch_discover_tests(live_infra_tests)
endif()
