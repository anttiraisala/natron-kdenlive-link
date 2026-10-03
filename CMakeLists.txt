cmake_minimum_required(VERSION 3.20)
project(natron_kdenlive_link VERSION 0.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # nkb_base is linked into the MLT module (a shared object)

option(NKB_BUILD_TESTS "Build and register the tests" ON)
# Command that runs a Natron Python script, e.g. /path/to/NatronRenderer or "snap run natron". If set, the
# Natron worker end-to-end test is registered.
set(NATRON_COMMAND "" CACHE STRING "Command that runs 'natron -t script.py' (enables the natron_e2e test)")
# Prefix of an MLT install (headers + libmlt-7). If set, the MLT filter module is built.
set(MLT_ROOT "" CACHE PATH "Prefix of the MLT install the filter is built against")

include(GNUInstallDirs)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)

# Protocol, sockets, cache, config. No spdlog here, so the MLT filter can link it.
add_library(nkb_base STATIC
  src/protocol.cpp src/net.cpp src/cache.cpp src/config.cpp src/client.cpp)
target_include_directories(nkb_base PUBLIC include)
target_link_libraries(nkb_base PUBLIC Threads::Threads)

# Logging (spdlog) for the daemon and tools.
add_library(nkb_log STATIC src/log.cpp)
target_link_libraries(nkb_log PUBLIC nkb_base spdlog::spdlog)

add_executable(natron-kdenlive-daemon src/daemon.cpp)
target_link_libraries(natron-kdenlive-daemon PRIVATE nkb_log)
# Where an installed daemon finds natron/nkb_gui_open.py ("Open in Natron").
target_compile_definitions(natron-kdenlive-daemon PRIVATE NKB_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}"
                                                     NKB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_executable(natron-kdenlive-cache tools/cache_tool.cpp)
target_link_libraries(natron-kdenlive-cache PRIVATE nkb_log)

add_executable(natron-kdenlive-doctor tools/doctor.cpp)
target_link_libraries(natron-kdenlive-doctor PRIVATE nkb_log)

add_executable(nkb-mock-worker tools/mock_worker.cpp)
target_link_libraries(nkb-mock-worker PRIVATE nkb_log)

add_executable(nkb-mock-filter tools/mock_filter.cpp)
target_link_libraries(nkb-mock-filter PRIVATE nkb_log)

install(TARGETS natron-kdenlive-daemon natron-kdenlive-cache natron-kdenlive-doctor
        RUNTIME DESTINATION bin)
install(FILES natron/nkb_natron_worker.py natron/nkb_gui_open.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/natron-kdenlive-link/natron)

# ---- MLT filter module (milestone 2) -------------------------------------
if(MLT_ROOT)
  find_library(MLT_LIB NAMES mlt-7 PATHS ${MLT_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
  find_path(MLT_INCLUDE NAMES framework/mlt.h PATHS ${MLT_ROOT}/include/mlt-7 NO_DEFAULT_PATH REQUIRED)
  message(STATUS "MLT: ${MLT_LIB} (headers ${MLT_INCLUDE})")

  # libmltnatron.so: MLT loads every libmlt*.so found in its module directory.
  add_library(mltnatron MODULE src/filter/natron_link.cpp)
  target_include_directories(mltnatron SYSTEM PRIVATE ${MLT_INCLUDE})
  target_link_libraries(mltnatron PRIVATE nkb_base ${MLT_LIB})
  # Static C++ runtime: the module must load into the MLT/Qt of Kdenlive's snap or AppImage.
  target_link_options(mltnatron PRIVATE -static-libstdc++ -static-libgcc)
  set_target_properties(mltnatron PROPERTIES PREFIX "lib" SKIP_BUILD_RPATH ON)

  if(NKB_BUILD_TESTS)
    add_executable(nkb-filter-check tests/filter_check.c)
    target_include_directories(nkb-filter-check SYSTEM PRIVATE ${MLT_INCLUDE})
    target_link_libraries(nkb-filter-check PRIVATE ${MLT_LIB})
    set_target_properties(nkb-filter-check PROPERTIES SKIP_BUILD_RPATH ON)
  endif()
endif()

if(NKB_BUILD_TESTS)
  enable_testing()
  add_executable(nkb-unit-tests tests/unit_tests.cpp)
  target_link_libraries(nkb-unit-tests PRIVATE nkb_base)
  add_test(NAME unit COMMAND nkb-unit-tests)
  add_test(NAME e2e COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e.sh $<TARGET_FILE_DIR:natron-kdenlive-daemon>)
  set_tests_properties(e2e PROPERTIES TIMEOUT 180)
  if(NATRON_COMMAND)
    separate_arguments(NATRON_COMMAND_LIST UNIX_COMMAND "${NATRON_COMMAND}")
    add_test(NAME natron_e2e COMMAND bash ${CMAKE_SOURCE_DIR}/tests/natron_e2e.sh
             $<TARGET_FILE_DIR:natron-kdenlive-daemon> ${NATRON_COMMAND_LIST})
    set_tests_properties(natron_e2e PROPERTIES TIMEOUT 240)
  endif()
  if(MLT_ROOT)
    add_test(NAME filter_e2e COMMAND bash ${CMAKE_SOURCE_DIR}/tests/filter_e2e.sh
             $<TARGET_FILE_DIR:natron-kdenlive-daemon> $<TARGET_FILE:mltnatron> ${MLT_ROOT})
    set_tests_properties(filter_e2e PROPERTIES TIMEOUT 180)
  endif()
endif()

# "make uninstall" removes everything recorded by "make install".
if(NOT TARGET uninstall)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
                 ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
  add_custom_target(uninstall COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
