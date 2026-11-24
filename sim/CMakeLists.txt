cmake_minimum_required(VERSION 3.16)
project(brew_sim C CXX)

# Host build of the same libraries the firmware uses (lib/*), against desktop LVGL:
#   brew_sim    headless scripted session -> PNG frames
#   brew_bench  redraw cost per interaction
#   brew_desk   interactive SDL window (only if SDL2 is found)
#   brew_tests  unit tests for lib/core, the chip drivers and the UI model (ctest)
#   harness_check  smoke test of the test harness (ctest)
#   brew_web    the test harness as a web page (Emscripten builds only; see sim/web/README.md)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Panel diameter: 480 = Elecrow CrowPanel 2.1", 360 = Waveshare 1.8" knob
set(BREW_DISP_SIZE 480 CACHE STRING "Round panel diameter in px (needs lib/fonts/<size>)")
add_compile_definitions(BREW_UI_DISP_SIZE=${BREW_DISP_SIZE})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
    add_compile_options(/utf-8 /D_CRT_SECURE_NO_WARNINGS)
    set(BREW_WARN /W3)
else()
    set(BREW_WARN -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- architecture / wire-format rules (same check PlatformIO runs before every build) ----
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    add_custom_target(check_rules ALL
        COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_rules.py
        COMMENT "Checking BrewDial layering and wire-format rules")
endif()

# ---- SDL2 (optional): enables the interactive `brew_desk` window ----
if(NOT EMSCRIPTEN)
    find_package(SDL2 QUIET)
endif()
if(SDL2_FOUND)
    message(STATUS "SDL2 found: building brew_desk (interactive)")
    add_compile_definitions(BREW_SIM_SDL=1)
    # LVGL includes <SDL2/SDL.h>, so add the parent of SDL2's include dir as well.
    list(GET SDL2_INCLUDE_DIRS 0 _sdl_inc)
    get_filename_component(_sdl_parent "${_sdl_inc}" DIRECTORY)
    include_directories(${SDL2_INCLUDE_DIRS} ${_sdl_parent})
else()
    message(STATUS "SDL2 not found: brew_desk skipped (brew_sim, brew_bench, brew_tests still build)")
endif()

# ---- LVGL ----
set(BREW_LV_CONF "${ROOT}/config/lv_conf.h" CACHE FILEPATH "lv_conf.h to build LVGL with")
set(LV_CONF_PATH ${BREW_LV_CONF} CACHE STRING "" FORCE)
set(LV_BUILD_CONF_PATH ${BREW_LV_CONF} CACHE PATH "lv_conf.h for LVGL" FORCE)
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
get_filename_component(_conf_dir "${BREW_LV_CONF}" DIRECTORY)
include_directories(${_conf_dir})
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Use ../lvgl if you've cloned it (handy for reading or patching LVGL);
# otherwise download the pinned release automatically (needs git on PATH).
set(BREW_LVGL_DIR "${ROOT}/lvgl" CACHE PATH "Local LVGL checkout (fetched if missing)")
if(EXISTS "${BREW_LVGL_DIR}/CMakeLists.txt")
    message(STATUS "Using local LVGL: ${BREW_LVGL_DIR}")
    add_subdirectory(${BREW_LVGL_DIR} ${CMAKE_BINARY_DIR}/lvgl)
else()
    message(STATUS "No ../lvgl folder; fetching LVGL v9.3.0 (first configure only)")
    include(FetchContent)
    FetchContent_Declare(lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG        v9.3.0
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(lvgl)
endif()
if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
        target_link_libraries(lvgl PUBLIC SDL2::SDL2)
    else()
        target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})
    endif()
endif()
if(NOT MSVC)
    set(BREW_LIBM m)
endif()

# ---- libraries (same sources PlatformIO builds from lib/) ----
add_library(brew_hal INTERFACE)
target_include_directories(brew_hal INTERFACE ${ROOT}/lib/drivers/interfaces/include)

file(GLOB BREW_CORE_SRC ${ROOT}/lib/core/src/*.cpp)
add_library(brew_core STATIC ${BREW_CORE_SRC})
target_include_directories(brew_core PUBLIC ${ROOT}/lib/core/include)
target_link_libraries(brew_core PUBLIC brew_hal)
target_compile_options(brew_core PRIVATE ${BREW_WARN})

file(GLOB BREW_NET_SRC ${ROOT}/lib/network/src/*.cpp)
add_library(brew_network STATIC ${BREW_NET_SRC})
target_include_directories(brew_network PUBLIC ${ROOT}/lib/network/include)
target_link_libraries(brew_network PUBLIC brew_hal)
target_compile_options(brew_network PRIVATE ${BREW_WARN})

file(GLOB BREW_DRV_SRC ${ROOT}/lib/drivers/chips/src/*.cpp)
add_library(brew_drivers STATIC ${BREW_DRV_SRC})
target_include_directories(brew_drivers PUBLIC ${ROOT}/lib/drivers/chips/include)
target_link_libraries(brew_drivers PUBLIC brew_hal)
target_compile_options(brew_drivers PRIVATE ${BREW_WARN})

# The UI model (state + rules, no LVGL): shared by every layout, unit-tested on its own.
add_library(brew_ui_model STATIC ${ROOT}/lib/ui/src/brew_model.c)
target_include_directories(brew_ui_model PUBLIC ${ROOT}/lib/ui/src)
target_compile_options(brew_ui_model PRIVATE ${BREW_WARN})
target_link_libraries(brew_ui_model PUBLIC ${BREW_LIBM})

file(GLOB FONT_SRC ${ROOT}/lib/fonts/${BREW_DISP_SIZE}/src/*.c)
add_library(brew_ui STATIC
    ${ROOT}/lib/ui/src/brew_ui.c
    ${ROOT}/lib/ui/src/layout_kit.c
    ${ROOT}/lib/ui/src/layout_round.c
    ${ROOT}/lib/ui/src/layout_square.c
    ${ROOT}/lib/ui/src/brew_bg.c
    ${FONT_SRC})
target_include_directories(brew_ui PUBLIC ${ROOT}/lib/ui/src)
target_compile_options(brew_ui PRIVATE ${BREW_WARN})
target_link_libraries(brew_ui PUBLIC brew_ui_model lvgl ${BREW_LIBM})

# portable half of lib/app (the ESP task runtime is firmware-only)
add_library(brew_app STATIC
    ${ROOT}/lib/app/src/ui_bridge.cpp
    ${ROOT}/lib/app/src/lvgl_port.cpp)
target_include_directories(brew_app PUBLIC ${ROOT}/lib/app/include)
target_link_libraries(brew_app PUBLIC brew_core brew_ui)
target_compile_options(brew_app PRIVATE ${BREW_WARN})

# ---- test harness: firmware display + controller on simulated hardware (sim/harness) ----
add_library(brew_harness STATIC harness/harness.cpp)
target_include_directories(brew_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/harness)
target_link_libraries(brew_harness PUBLIC brew_app brew_network)
target_compile_options(brew_harness PRIVATE ${BREW_WARN})

if(EMSCRIPTEN)
    # Web harness: `emcmake cmake -S sim -B sim/build-web` then build; open sim/build-web/web/index.html.
    # web_api.cpp is compiled into the executable (not the library) so its exports are kept.
    add_executable(brew_web harness/web_api.cpp)
    target_link_libraries(brew_web PRIVATE brew_harness)
    set_target_properties(brew_web PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/web)
    target_link_options(brew_web PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_NAME=createBrewDial
        -sALLOW_MEMORY_GROWTH=1
        -sSINGLE_FILE=1            # wasm embedded in the .js: index.html works from file://
        -sENVIRONMENT=web,node     # node: lets harness/web_smoke.js run it headless
        "-sEXPORTED_RUNTIME_METHODS=['UTF8ToString','HEAPU8','ccall']")
    # Always re-copy the page (a target with no outputs runs every build), so editing
    # index.html / app.js / style.css shows up even when no C++ changed.
    add_custom_target(brew_web_page ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/web
        COMMENT "Copying the harness page next to brew_web.js")
    if(TARGET check_rules)
        add_dependencies(brew_web check_rules)
    endif()
    return()   # the native tools below don't make sense in a browser
endif()

# ---- executables ----
add_executable(brew_sim main.cpp)
target_link_libraries(brew_sim PRIVATE brew_app)

add_executable(brew_bench bench.c)
target_link_libraries(brew_bench PRIVATE brew_ui)

if(SDL2_FOUND)
    add_executable(brew_desk desk.cpp)
    target_compile_options(brew_desk PRIVATE ${BREW_WARN})
    target_link_libraries(brew_desk PRIVATE brew_app)
endif()

add_executable(harness_check harness/harness_check.cpp harness/web_api.cpp)
target_link_libraries(harness_check PRIVATE brew_harness)
target_compile_options(harness_check PRIVATE ${BREW_WARN})

# ---- unit tests ----
file(GLOB TEST_SRC ${ROOT}/test/*.cpp)
add_executable(brew_tests ${TEST_SRC})
target_link_libraries(brew_tests PRIVATE brew_core brew_drivers brew_network brew_ui_model)
target_compile_options(brew_tests PRIVATE ${BREW_WARN})
enable_testing()
add_test(NAME brew_tests COMMAND brew_tests)
add_test(NAME harness_check COMMAND harness_check)
if(TARGET check_rules)
    add_dependencies(brew_tests check_rules)
endif()
