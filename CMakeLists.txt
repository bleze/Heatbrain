cmake_minimum_required(VERSION 3.25)
project(heatbrain VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HEATBRAIN_BUILD_TESTS "Build unit tests" ON)
option(HEATBRAIN_DEV_DEFAULTS "Use <source>/data as default data dir (local development)" OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# --- Pure logic: no IO, no network. Everything here is unit-testable. ---
add_library(heatbrain_core STATIC
  src/core/time.cpp
  src/core/tariff.cpp
  src/core/schedule.cpp
  src/core/price.cpp
  src/core/failsafe.cpp
  src/core/planner.cpp
  src/core/alerts.cpp
  src/core/cost.cpp
  src/core/version.cpp
)
target_include_directories(heatbrain_core PUBLIC src)

# --- IO: HTTP, providers, config ---
add_library(heatbrain_io STATIC
  src/net/http.cpp
  src/providers/energidataservice.cpp
  src/providers/basic_components.cpp
  src/providers/price_model.cpp
  src/plugs/tasmota.cpp
  src/plugs/plugs.cpp
  src/sensors/netatmo.cpp
  src/sensors/sensors.cpp
  src/storage/history.cpp
  src/notify/notifier.cpp
  src/web/server.cpp
  src/app/config.cpp
  src/app/rooms.cpp
  src/app/controller.cpp
  src/app/config_service.cpp
  src/app/cost_report.cpp
  src/app/digest.cpp
)
target_link_libraries(heatbrain_io PUBLIC
  heatbrain_core
  httplib::httplib
  nlohmann_json::nlohmann_json
  OpenSSL::SSL OpenSSL::Crypto
  unofficial::sqlite3::sqlite3
)
target_compile_definitions(heatbrain_io PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)

add_executable(heatbrain src/app/main.cpp src/app/control.cpp)
target_link_libraries(heatbrain PRIVATE heatbrain_io)
if(HEATBRAIN_DEV_DEFAULTS)
  target_compile_definitions(heatbrain PRIVATE HEATBRAIN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
install(TARGETS heatbrain RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION config)
install(DIRECTORY web/ DESTINATION web)

if(HEATBRAIN_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(heatbrain_tests
    tests/test_time.cpp
    tests/test_tariff.cpp
    tests/test_schedule.cpp
    tests/test_price.cpp
    tests/test_energidataservice.cpp
    tests/test_http_cache.cpp
    tests/test_failsafe.cpp
    tests/test_tasmota.cpp
    tests/test_http_client.cpp
    tests/test_netatmo.cpp
    tests/test_planner.cpp
    tests/test_controller.cpp
    tests/test_alerts.cpp
    tests/test_config_service.cpp
    tests/test_cost.cpp
    tests/test_version.cpp
    tests/test_digest.cpp
  )
  target_link_libraries(heatbrain_tests PRIVATE heatbrain_io Catch2::Catch2WithMain)
  target_compile_definitions(heatbrain_tests PRIVATE
    HEATBRAIN_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  include(Catch)
  catch_discover_tests(heatbrain_tests)
endif()
