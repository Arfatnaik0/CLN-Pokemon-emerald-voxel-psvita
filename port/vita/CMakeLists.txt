cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VITASDK})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE FILEPATH "Vita toolchain")
    else()
        message(FATAL_ERROR "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE=<VITASDK>/share/vita.toolchain.cmake")
    endif()
endif()

# Default to Release build type if none was specified.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    message(WARNING "CMAKE_BUILD_TYPE was not specified; defaulting to 'Release' (-O3). High optimization is required for Vita real-time overworld rendering.")
endif()

project(pokeemerald_vita C CXX ASM)

# Enforce optimization floor: Vita CPU (Cortex-A9 @ 333MHz) requires optimization (-O2/-O3).
option(VITA_ALLOW_UNOPTIMIZED "Allow building without optimization (-O0)" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "" OR CMAKE_BUILD_TYPE MATCHES "^(Debug|None)$")
    if(NOT VITA_ALLOW_UNOPTIMIZED)
        message(FATAL_ERROR
            "Vita builds must be optimized. Building with CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' "
            "results in severe frame rate degradation (-O0). "
            "Pass -DCMAKE_BUILD_TYPE=Release or explicitly set -DVITA_ALLOW_UNOPTIMIZED=ON to override.")
    endif()
endif()

if(NOT DEFINED VITASDK)
    if(DEFINED ENV{VITASDK})
        set(VITASDK "$ENV{VITASDK}")
    else()
        get_filename_component(VITASDK "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
        get_filename_component(VITASDK "${VITASDK}/.." ABSOLUTE)
    endif()
endif()
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(GAME_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(GAME_ROOT "${GAME_ROOT}" ABSOLUTE)
set(VITA_BUILD_HELPERS "${CMAKE_CURRENT_LIST_DIR}/cmake")

# A changed source generator input must rerun the freshness check during CMake
# reconfiguration, or the previously generated M0 output could be reused.
file(GLOB_RECURSE M0_GENERATOR_INPUTS CONFIGURE_DEPENDS
    "${GAME_ROOT}/data/*.json"
    "${GAME_ROOT}/data/*.json.txt"
    "${GAME_ROOT}/src/data/*.json"
    "${GAME_ROOT}/src/data/*.json.txt"
    "${GAME_ROOT}/include/constants/*.json"
    "${GAME_ROOT}/include/constants/*.json.txt"
    "${GAME_ROOT}/graphics/*.png"
    "${GAME_ROOT}/graphics/*.pal"
    "${GAME_ROOT}/sound/*.aif"
    "${GAME_ROOT}/sound/*.mid"
    "${GAME_ROOT}/sound/songs/midi/midi.cfg")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${M0_GENERATOR_INPUTS})

set(VITA_APP_NAME "Pokemon Emerald Voxel")
set(VITA_TITLEID "PKEV00001")
set(VITA_VERSION "01.00")
option(VITA_ENABLE_PERF "Include Vita frame timing and perf.log instrumentation" ON)
option(VITA_PIN_RENDER_THREADS
    "Pin renderer, game and EASY draw workers to Vita user cores 0/1/2" ON)
option(VITA_ENABLE_HBLANK_WIN0_PARALLEL
    "Replay the supported WIN0H HBlank DMA before parallel PPU rendering" ON)
option(VITA_PRESENT_EACH_SERVICED_TICK
    "Present each serviced Vita game tick instead of once per main-loop iteration" ON)
option(VITA_BUILD_LIVEAREA_DIAGNOSTIC
    "Build a second VPK under PKEV00002 to diagnose LiveArea registration" OFF)
option(VITA_ENABLE_FOLLOWER "Include local follower assets and runtime support" ON)
option(VITA_PACKAGE_FOLLOWER_ASSETS "Embed local follower assets into VPK (otherwise read ux0:data/pokeemerald/followers/)" ON)
option(VITA_ENABLE_VOXEL "Build 3D voxel mode and package local voxel assets" ON)
option(VITA_PACKAGE_VOXEL_ASSETS "Embed local voxel assets into VPK (otherwise read ux0:data/pokeemerald/voxel/)" ON)
set(FOLLOWER_ASSETS_DIR "$ENV{FOLLOWER_ASSETS_DIR}" CACHE PATH
    "External local follower asset pack (never copied into the source tree)")
set(VOXEL_ASSETS_DIR "$ENV{VOXEL_ASSETS_DIR}" CACHE PATH
    "External local voxel asset directory (never copied into the source tree)")
set(VITAGL_ROOT "$ENV{VITAGL_ROOT}" CACHE PATH
    "Prefix containing vitaGL headers and static libraries (include/vitaGL.h, lib/libvitaGL.a)")
set(VITA_LIVEAREA_DIAGNOSTIC_TITLEID "PKEV00002")

set(VITA_SDL2_ROOT "" CACHE PATH "Prefix containing a Vita-target SDL2 CMake package")
if(VITA_SDL2_ROOT AND EXISTS "${VITA_SDL2_ROOT}/lib/cmake/SDL2/SDL2Config.cmake")
    if(NOT SDL2_DIR OR SDL2_DIR MATCHES "-NOTFOUND$")
        set(SDL2_DIR "${VITA_SDL2_ROOT}/lib/cmake/SDL2" CACHE PATH "Vita SDL2 CMake package" FORCE)
    endif()
endif()
find_package(SDL2 2.30.7 EXACT CONFIG REQUIRED)
if(NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "The selected SDL2 package does not export SDL2::SDL2")
endif()

set(FOLLOWER_PACK_FILES)
if(VITA_ENABLE_FOLLOWER AND VITA_PACKAGE_FOLLOWER_ASSETS)
    if(NOT FOLLOWER_ASSETS_DIR)
        set(FOLLOWER_ASSETS_DIR "${GAME_ROOT}/assets/followers" CACHE PATH
            "External local follower asset pack (never copied into the source tree)" FORCE)
    endif()
    if(NOT IS_DIRECTORY "${FOLLOWER_ASSETS_DIR}")
        message(FATAL_ERROR
            "Follower assets are enabled, but FOLLOWER_ASSETS_DIR is not a directory: ${FOLLOWER_ASSETS_DIR}. "
            "Set FOLLOWER_ASSETS_DIR to your local FMN1 pack, configure -DVITA_PACKAGE_FOLLOWER_ASSETS=OFF, or -DVITA_ENABLE_FOLLOWER=OFF.")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/check_follower_assets.py" "${FOLLOWER_ASSETS_DIR}"
        RESULT_VARIABLE FOLLOWER_ASSET_CHECK_RESULT
        OUTPUT_VARIABLE FOLLOWER_ASSET_CHECK_OUTPUT
        ERROR_VARIABLE FOLLOWER_ASSET_CHECK_ERROR)
    if(NOT FOLLOWER_ASSET_CHECK_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Follower asset validation failed for ${FOLLOWER_ASSETS_DIR}:\n${FOLLOWER_ASSET_CHECK_OUTPUT}${FOLLOWER_ASSET_CHECK_ERROR}")
    endif()
    file(GLOB_RECURSE FOLLOWER_PACK_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
        "${FOLLOWER_ASSETS_DIR}/*")
    if(NOT FOLLOWER_PACK_FILES)
        message(FATAL_ERROR "Follower asset pack is empty: ${FOLLOWER_ASSETS_DIR}")
    endif()
    message(STATUS "Validated external follower pack: ${FOLLOWER_ASSETS_DIR}")
elseif(VITA_ENABLE_FOLLOWER)
    message(STATUS "Follower runtime support enabled; assets omitted from VPK (expecting ux0:data/pokeemerald/followers/)")
endif()

if(NOT VITAGL_ROOT)
    set(VITAGL_ROOT "/home/arfat00/.local/share/galaxy-vita/vitagl/nhustdx" CACHE PATH
        "Prefix containing vitaGL headers and static libraries (include/vitaGL.h, lib/libvitaGL.a)" FORCE)
endif()
if(NOT EXISTS "${VITAGL_ROOT}/include/vitaGL.h" OR NOT EXISTS "${VITAGL_ROOT}/lib/libvitaGL.a")
    message(FATAL_ERROR
        "vitaGL is required for Vita display/voxel builds, but was not found under VITAGL_ROOT=${VITAGL_ROOT}. "
        "Expected include/vitaGL.h and lib/libvitaGL.a.")
endif()
message(STATUS "Using vitaGL prefix: ${VITAGL_ROOT}")

if(VITA_ENABLE_VOXEL AND VITA_PACKAGE_VOXEL_ASSETS)
    if(NOT VOXEL_ASSETS_DIR)
        get_filename_component(REPO_ROOT_GUESS "${GAME_ROOT}/.." ABSOLUTE)
        set(VOXEL_ASSETS_DIR "${REPO_ROOT_GUESS}/local-output/assets/voxel" CACHE PATH
            "External local voxel asset directory (never copied into the source tree)" FORCE)
    endif()
    if(NOT IS_DIRECTORY "${VOXEL_ASSETS_DIR}")
        message(FATAL_ERROR
            "Voxel 3D mode is enabled, but VOXEL_ASSETS_DIR is not a directory: ${VOXEL_ASSETS_DIR}. "
            "Set VOXEL_ASSETS_DIR to your local voxel assets, configure -DVITA_PACKAGE_VOXEL_ASSETS=OFF, or -DVITA_ENABLE_VOXEL=OFF.")
    endif()
    foreach(VOXEL_REQUIRED_FILE IN ITEMS buildings.bin regions.bin relief.bin signposts.bin trees.rgba5551)
        if(NOT EXISTS "${VOXEL_ASSETS_DIR}/${VOXEL_REQUIRED_FILE}")
            message(FATAL_ERROR "Missing required voxel asset ${VOXEL_ASSETS_DIR}/${VOXEL_REQUIRED_FILE}")
        endif()
    endforeach()
    message(STATUS "Validated external voxel pack: ${VOXEL_ASSETS_DIR}")
elseif(VITA_ENABLE_VOXEL)
    message(STATUS "Voxel 3D runtime support enabled; assets omitted from VPK (expecting ux0:data/pokeemerald/voxel/)")
endif()

find_program(VITA_AS NAMES arm-vita-eabi-as HINTS "${VITASDK}/bin" REQUIRED)
find_program(VITA_LD NAMES arm-vita-eabi-ld HINTS "${VITASDK}/bin" REQUIRED)
find_program(VITA_OBJCOPY NAMES arm-vita-eabi-objcopy HINTS "${VITASDK}/bin" REQUIRED)
find_program(HOST_CPP NAMES cpp REQUIRED)
find_program(PC_MAKE NAMES make gmake REQUIRED)

set(PREPROC "${GAME_ROOT}/tools/preproc/preproc")
if(NOT EXISTS "${PREPROC}")
    message(FATAL_ERROR "Host preprocessor is missing: ${PREPROC}. Run `make -C '${GAME_ROOT}' -f make_tools.mk` first.")
endif()
set(SCANINC "${GAME_ROOT}/tools/scaninc/scaninc")
if(NOT EXISTS "${SCANINC}")
    message(FATAL_ERROR "Host dependency scanner is missing: ${SCANINC}. Run `make -C '${GAME_ROOT}' -f make_tools.mk tools` first.")
endif()

set(VITA_M0_DOCKER_IMAGE "" CACHE STRING "Optional container image with the 32-bit M0 host build dependencies")
set(VITA_M0_BUILD_JOBS "2" CACHE STRING "Parallel jobs for refreshing M0 assets inside Docker")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DGAME_ROOT=${GAME_ROOT}"
        "-DSCANINC=${SCANINC}"
        "-DPC_MAKE=${PC_MAKE}"
        "-DVITA_M0_DOCKER_IMAGE=${VITA_M0_DOCKER_IMAGE}"
        "-DVITA_M0_BUILD_JOBS=${VITA_M0_BUILD_JOBS}"
        -P "${VITA_BUILD_HELPERS}/CheckM0Assets.cmake"
    RESULT_VARIABLE M0_CHECK_RESULT
    OUTPUT_VARIABLE M0_CHECK_OUTPUT
    ERROR_VARIABLE M0_CHECK_ERROR)
if(NOT M0_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR "M0 asset freshness check failed:\n${M0_CHECK_OUTPUT}\n${M0_CHECK_ERROR}")
endif()
message(STATUS "M0-generated inputs are fresh (scaninc + Make)")

set(M0_GENERATED_FILES
    "${GAME_ROOT}/data/layouts/layouts.inc"
    "${GAME_ROOT}/data/layouts/layouts_table.inc"
    "${GAME_ROOT}/data/maps/headers.inc"
    "${GAME_ROOT}/data/maps/groups.inc"
    "${GAME_ROOT}/data/maps/connections.inc"
    "${GAME_ROOT}/data/maps/events.inc"
)
foreach(GENERATED_FILE IN LISTS M0_GENERATED_FILES)
    if(NOT EXISTS "${GENERATED_FILE}")
        message(FATAL_ERROR
            "Required M0-generated asset is missing: ${GENERATED_FILE}. Build the Linux host target first with `make -C '${GAME_ROOT}' -f Makefile_pc linux -j`.")
    endif()
endforeach()

file(GLOB MIDI_INPUTS CONFIGURE_DEPENDS "${GAME_ROOT}/sound/songs/midi/*.mid")
foreach(MIDI_INPUT IN LISTS MIDI_INPUTS)
    get_filename_component(MIDI_NAME "${MIDI_INPUT}" NAME_WE)
    if(NOT EXISTS "${GAME_ROOT}/sound/songs/midi/${MIDI_NAME}.s")
        message(FATAL_ERROR
            "MIDI assembly output is missing for ${MIDI_INPUT}. Build the Linux host target first so tools/mid2agb generates sound/songs/midi/${MIDI_NAME}.s.")
    endif()
endforeach()

set(GAME_DEFINES PLATFORM_VITA PLATFORM_SDL2 RENDERER_EASY_DRAW NONMATCHING PORTABLE MODERN=1 UBFIX)
if(VITA_PRESENT_EACH_SERVICED_TICK)
    list(APPEND GAME_DEFINES VITA_PRESENT_EACH_SERVICED_TICK=1)
else()
    list(APPEND GAME_DEFINES VITA_PRESENT_EACH_SERVICED_TICK=0)
endif()
if(NOT VITA_ENABLE_PERF)
    list(APPEND GAME_DEFINES VITA_PERF_DISABLE)
endif()
if(VITA_PIN_RENDER_THREADS)
    list(APPEND GAME_DEFINES VITA_PIN_RENDER_THREADS=1)
endif()
if(VITA_ENABLE_HBLANK_WIN0_PARALLEL)
    list(APPEND GAME_DEFINES VITA_ENABLE_HBLANK_WIN0_PARALLEL=1)
endif()
if(VITA_ENABLE_FOLLOWER)
    list(APPEND GAME_DEFINES FOLLOWER_MON_ENABLED=1)
endif()
if(VITA_ENABLE_VOXEL)
    list(APPEND GAME_DEFINES VOXEL_SUPPORTED)
endif()
set(GAME_DEFINE_ARGS)
foreach(DEFINE IN LISTS GAME_DEFINES)
    list(APPEND GAME_DEFINE_ARGS "-D${DEFINE}")
endforeach()

get_target_property(SDL2_IMPORTED_INCLUDE_DIRS SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
if(NOT SDL2_IMPORTED_INCLUDE_DIRS)
    if(DEFINED SDL2_INCLUDE_DIRS)
        set(SDL2_IMPORTED_INCLUDE_DIRS "${SDL2_INCLUDE_DIRS}")
    elseif(DEFINED SDL2_INCLUDE_DIR)
        set(SDL2_IMPORTED_INCLUDE_DIRS "${SDL2_INCLUDE_DIR}")
    endif()
endif()
set(PREPROCESS_INCLUDE_ARGS)
foreach(INCLUDE_DIR IN LISTS SDL2_IMPORTED_INCLUDE_DIRS)
    if(NOT INCLUDE_DIR MATCHES "^\\$<")
        list(APPEND PREPROCESS_INCLUDE_ARGS "-I${INCLUDE_DIR}")
    endif()
endforeach()
list(APPEND PREPROCESS_INCLUDE_ARGS "-I${GAME_ROOT}/src/platform" "-I${VITAGL_ROOT}/include")

file(GLOB_RECURSE GAME_C CONFIGURE_DEPENDS "${GAME_ROOT}/src/*.c" "${GAME_ROOT}/gflib/*.c")
list(FILTER GAME_C EXCLUDE REGEX "\\.inc\\.c$")
list(FILTER GAME_C EXCLUDE REGEX "/src/platform/voxel/")
if(VITA_ENABLE_VOXEL)
    # Explicit active voxel unit list so legacy voxel_*.c files in older baselines are never compiled.
    list(APPEND GAME_C
        "${GAME_ROOT}/src/platform/voxel/3ds_field_view.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_arena.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_atlas.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_battle.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_building.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_camera.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_entities.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_grade.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_lighting.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_mesh_builder.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_regions.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_relief.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_renderer.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_sign.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_sprite_slots.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_swoop.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_tree.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_vita_bridge.c"
        "${GAME_ROOT}/src/platform/voxel/voxel_world.c"
    )
else()
    list(FILTER GAME_C EXCLUDE REGEX "/src/platform/vita/vita_daynight\\.c$")
endif()

set(GENERATED_C)
foreach(SOURCE IN LISTS GAME_C)
    file(RELATIVE_PATH RELATIVE_SOURCE "${GAME_ROOT}" "${SOURCE}")
    set(OUTPUT "${CMAKE_BINARY_DIR}/generated/${RELATIVE_SOURCE}")
    get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
    add_custom_command(
        OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}"
            "-DC_COMPILER=${CMAKE_C_COMPILER}"
            "-DPREPROC=${PREPROC}"
            "-DROOT=${GAME_ROOT}"
            "-DSOURCE=${SOURCE}"
            "-DOUTPUT=${OUTPUT}"
            "-DDEPFILE=${OUTPUT}.d"
            "-DCHARMAP=${GAME_ROOT}/charmap.txt"
            "-DGAME_DEFINE_ARGS=${GAME_DEFINE_ARGS}"
            "-DPREPROCESS_INCLUDE_ARGS=${PREPROCESS_INCLUDE_ARGS}"
            -P "${VITA_BUILD_HELPERS}/PreprocessC.cmake"
        DEPENDS "${SOURCE}" "${PREPROC}" "${GAME_ROOT}/charmap.txt" "${VITA_BUILD_HELPERS}/PreprocessC.cmake"
        DEPFILE "${OUTPUT}.d"
        VERBATIM)
    list(APPEND GENERATED_C "${OUTPUT}")
endforeach()

file(GLOB DATA_ASM CONFIGURE_DEPENDS "${GAME_ROOT}/data/*.s")
file(GLOB SONG_ASM CONFIGURE_DEPENDS "${GAME_ROOT}/sound/songs/*.s" "${GAME_ROOT}/sound/songs/midi/*.s")

# The .s files under src/ are the GBA startup/audio routines (crt0, m4a_1,
# ROM header and GameCube multiboot). The SDL platform uses Vita's entry point,
# C audio implementation and src/stub.c, so those cartridge objects must not
# be linked into the native app. asm/ contains only assembler macro includes.

set(GAME_DATA_INPUTS ${DATA_ASM} ${SONG_ASM})
set(GAME_DATA_OBJECTS)
foreach(SOURCE IN LISTS GAME_DATA_INPUTS)
    file(RELATIVE_PATH RELATIVE_SOURCE "${GAME_ROOT}" "${SOURCE}")
    set(OUTPUT "${CMAKE_BINARY_DIR}/generated/${RELATIVE_SOURCE}.o")
    get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
    if(SOURCE MATCHES "/sound/songs/")
        set(ASM_MODE song)
        set(ASM_DEPENDS "${SOURCE}" "${VITA_BUILD_HELPERS}/CompileAsm.cmake")
    else()
        set(ASM_MODE preprocessed)
        set(ASM_DEPENDS "${SOURCE}" "${PREPROC}" "${GAME_ROOT}/charmap.txt" "${CMAKE_BINARY_DIR}/generated/special_ids.inc" "${VITA_BUILD_HELPERS}/CompileAsm.cmake")
    endif()
    add_custom_command(
        OUTPUT "${OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}"
            "-DAS=${VITA_AS}"
            "-DHOST_CPP=${HOST_CPP}"
            "-DPREPROC=${PREPROC}"
            "-DSCANINC=${SCANINC}"
            "-DROOT=${GAME_ROOT}"
            "-DSOURCE=${SOURCE}"
            "-DOUTPUT=${OUTPUT}"
            "-DDEPFILE=${OUTPUT}.d"
            "-DCHARMAP=${GAME_ROOT}/charmap.txt"
            "-DSPECIALS=${CMAKE_BINARY_DIR}/generated/special_ids.inc"
            "-DMODE=${ASM_MODE}"
            "-DDEFINE_ARGS=${GAME_DEFINE_ARGS}"
            -P "${VITA_BUILD_HELPERS}/CompileAsm.cmake"
        DEPENDS ${ASM_DEPENDS} "${SCANINC}"
        DEPFILE "${OUTPUT}.d"
        VERBATIM)
    list(APPEND GAME_DATA_OBJECTS "${OUTPUT}")
endforeach()

set(SPECIAL_IDS "${CMAKE_BINARY_DIR}/generated/special_ids.inc")
add_custom_command(
    OUTPUT "${SPECIAL_IDS}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${GAME_ROOT}/data/specials.inc"
        "-DOUTPUT=${SPECIAL_IDS}"
        -P "${VITA_BUILD_HELPERS}/GenerateSpecialIds.cmake"
    DEPENDS "${GAME_ROOT}/data/specials.inc" "${VITA_BUILD_HELPERS}/GenerateSpecialIds.cmake"
    VERBATIM)

set(GAME_DATA_OBJECT "${CMAKE_BINARY_DIR}/generated/game_data.o")
add_custom_command(
    OUTPUT "${GAME_DATA_OBJECT}"
    COMMAND "${VITA_LD}" -r -o "${GAME_DATA_OBJECT}.tmp" ${GAME_DATA_OBJECTS}
    COMMAND "${VITA_OBJCOPY}" --rename-section .rodata=.data,alloc,load,data,contents "${GAME_DATA_OBJECT}.tmp" "${GAME_DATA_OBJECT}"
    DEPENDS ${GAME_DATA_OBJECTS}
    VERBATIM)
set_source_files_properties("${GAME_DATA_OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_executable(${PROJECT_NAME} ${GENERATED_C} "${GAME_DATA_OBJECT}")
set_target_properties(${PROJECT_NAME} PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
    CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)
target_include_directories(${PROJECT_NAME} PRIVATE "${GAME_ROOT}" "${GAME_ROOT}/include" "${GAME_ROOT}/gflib" "${GAME_ROOT}/sound" "${GAME_ROOT}/src/platform")
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE "${VITAGL_ROOT}/include")
target_compile_definitions(${PROJECT_NAME} PRIVATE ${GAME_DEFINES})
target_compile_options(${PROJECT_NAME} PRIVATE
    -mcpu=cortex-a9
    -mfpu=neon
    -mfloat-abi=hard
    -g
    -fno-builtin
    -fno-strict-aliasing
    -Wno-implicit-function-declaration)
target_link_directories(${PROJECT_NAME} PRIVATE "${VITAGL_ROOT}/lib")
target_link_options(${PROJECT_NAME} PRIVATE
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map"
    "-Wl,-z,max-page-size=0x10000"
    "-Wl,-z,common-page-size=0x10000"
    "-Wl,--defsym=__sce_headroom=0x1000")
target_link_libraries(${PROJECT_NAME} PRIVATE
    SDL2::SDL2
    vitaGL
    vitashark
    mathneon
    SceShaccCgExt
    SceShaccCg_stub
    taihen_stub
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceTouch_stub
    SceAudio_stub SceSysmodule_stub SceCommonDialog_stub SceAppUtil_stub
    SceAppMgr_stub ScePower_stub SceRtc_stub SceIofilemgr_stub SceKernelDmacMgr_stub
    SceKernelThreadMgr_stub
    stdc++ m pthread)

# VitaShell rejects VPKs whose package artwork uses truecolor PNGs (the install
# typically reaches 99% before failing with 0x8010113D). Validate the IHDR
# before packaging so every `sce_sys` image is indexed 8-bit without alpha.
function(vita_assert_package_png png_path)
    if(NOT EXISTS "${png_path}")
        message(FATAL_ERROR "Required Vita package PNG is missing: ${png_path}")
    endif()

    file(READ "${png_path}" png_hex HEX)
    string(TOLOWER "${png_hex}" png_hex)
    string(LENGTH "${png_hex}" png_hex_length)
    if(png_hex_length LESS 52)
        message(FATAL_ERROR "Vita package PNG is truncated: ${png_path}")
    endif()

    string(SUBSTRING "${png_hex}" 0 16 png_signature)
    string(SUBSTRING "${png_hex}" 48 2 png_bit_depth)
    string(SUBSTRING "${png_hex}" 50 2 png_color_type)
    if(NOT png_signature STREQUAL "89504e470d0a1a0a")
        message(FATAL_ERROR "Vita package asset is not a PNG: ${png_path}")
    endif()
    if(NOT png_bit_depth STREQUAL "08" OR NOT png_color_type STREQUAL "03")
        message(FATAL_ERROR
            "Vita package PNG ${png_path} must be 8-bit indexed (IHDR color type 3); "
            "found depth=${png_bit_depth}, color type=${png_color_type}. "
            "Re-encode with `convert input.png -alpha off -colors 256 -type Palette PNG8:output.png`.")
    endif()

    # Indexed PNG transparency is stored in a tRNS chunk rather than the IHDR.
    string(FIND "${png_hex}" "74524e53" transparency_chunk)
    if(NOT transparency_chunk EQUAL -1)
        message(FATAL_ERROR "Vita package PNG must not contain transparency: ${png_path}")
    endif()
endfunction()

function(vita_assert_package_png_dimensions png_path expected_width expected_height)
    file(READ "${png_path}" ihdr_dimensions HEX OFFSET 16 LIMIT 8)
    string(TOLOWER "${ihdr_dimensions}" ihdr_dimensions)
    string(SUBSTRING "${ihdr_dimensions}" 0 8 width_hex)
    string(SUBSTRING "${ihdr_dimensions}" 8 8 height_hex)
    math(EXPR actual_width "0x${width_hex}")
    math(EXPR actual_height "0x${height_hex}")
    if(NOT actual_width EQUAL expected_width OR NOT actual_height EQUAL expected_height)
        message(FATAL_ERROR
            "LiveArea image ${png_path} must be ${expected_width}x${expected_height}; "
            "found ${actual_width}x${actual_height}.")
    endif()
endfunction()

vita_assert_package_png("${CMAKE_CURRENT_LIST_DIR}/sce_sys/icon0.png")
vita_assert_package_png("${CMAKE_CURRENT_LIST_DIR}/sce_sys/pic0.png")
vita_assert_package_png("${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/bg.png")
vita_assert_package_png("${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/startup.png")
vita_assert_package_png_dimensions(
    "${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/bg.png" 840 500)
vita_assert_package_png_dimensions(
    "${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/startup.png" 280 158)

file(READ "${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/template.xml" livearea_template)
foreach(required_livearea_reference
    "<image>bg.png</image>"
    "<startup-image>startup.png</startup-image>")
    string(FIND "${livearea_template}" "${required_livearea_reference}" reference_offset)
    if(reference_offset EQUAL -1)
        message(FATAL_ERROR
            "LiveArea template must reference its packaged image with "
            "${required_livearea_reference}.")
    endif()
endforeach()

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)

# vita_create_vpk is a macro that appends its per-package SFO and file arguments
# to CMake variables. Keep each call in its own function scope so the
# diagnostic VPK gets a distinct title ID and no duplicated package entries.
function(vita_create_pokeemerald_vpk target title_id display_name)
    set(resources ${FOLLOWER_PACK_FILES})
    set(package_files
        FILE "${CMAKE_CURRENT_LIST_DIR}/sce_sys/icon0.png" sce_sys/icon0.png
        FILE "${CMAKE_CURRENT_LIST_DIR}/sce_sys/pic0.png" sce_sys/pic0.png
        FILE "${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/bg.png" sce_sys/livearea/contents/bg.png
        FILE "${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/startup.png" sce_sys/livearea/contents/startup.png
        FILE "${CMAKE_CURRENT_LIST_DIR}/sce_sys/livearea/contents/template.xml" sce_sys/livearea/contents/template.xml)
    if(VITA_ENABLE_FOLLOWER AND VITA_PACKAGE_FOLLOWER_ASSETS)
        list(APPEND package_files FILE "${FOLLOWER_ASSETS_DIR}" followers)
    endif()
    if(VITA_ENABLE_VOXEL AND VITA_PACKAGE_VOXEL_ASSETS)
        list(APPEND package_files FILE "${VOXEL_ASSETS_DIR}" voxel)
    endif()
    vita_create_vpk(${target} ${title_id} eboot.bin
        VERSION ${VITA_VERSION}
        NAME "${display_name}"
        ${package_files})
endfunction()

vita_create_pokeemerald_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} "${VITA_APP_NAME}")
if(VITA_BUILD_LIVEAREA_DIAGNOSTIC)
    if(VITA_TITLEID STREQUAL VITA_LIVEAREA_DIAGNOSTIC_TITLEID)
        message(FATAL_ERROR "The LiveArea diagnostic VPK must use a different TITLE_ID.")
    endif()
    vita_create_pokeemerald_vpk(${PROJECT_NAME}_livearea_diag.vpk
        ${VITA_LIVEAREA_DIAGNOSTIC_TITLEID} "${VITA_APP_NAME} LA Test")
endif()

message(STATUS "Vita build: ${VITA_APP_NAME} (${VITA_TITLEID})")
message(STATUS "SDL2 package: ${SDL2_DIR}")
message(STATUS "Game sources: ${GAME_ROOT}")
