if(NOT DEFINED GAME_ROOT OR NOT DEFINED SCANINC OR NOT DEFINED PC_MAKE)
    message(FATAL_ERROR "CheckM0Assets.cmake requires GAME_ROOT, SCANINC and PC_MAKE")
endif()

if(VITA_M0_DOCKER_IMAGE)
    find_program(DOCKER_COMMAND NAMES docker)
    if(NOT DOCKER_COMMAND)
        message(FATAL_ERROR "VITA_M0_DOCKER_IMAGE is set, but docker is not available")
    endif()
    if(NOT DEFINED VITA_M0_BUILD_JOBS OR VITA_M0_BUILD_JOBS STREQUAL "")
        set(VITA_M0_BUILD_JOBS 2)
    endif()
    execute_process(COMMAND id -u OUTPUT_VARIABLE HOST_UID OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND id -g OUTPUT_VARIABLE HOST_GID OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT HOST_UID MATCHES "^[0-9]+$" OR NOT HOST_GID MATCHES "^[0-9]+$")
        message(FATAL_ERROR "Could not determine the host UID/GID for the M0 Docker refresh")
    endif()
    execute_process(
        COMMAND "${DOCKER_COMMAND}" run --rm
            --user "${HOST_UID}:${HOST_GID}"
            --volume "${GAME_ROOT}:/workspace"
            --workdir /workspace
            --env "VITA_M0_BUILD_JOBS=${VITA_M0_BUILD_JOBS}"
            "${VITA_M0_DOCKER_IMAGE}"
            make -f Makefile_pc linux "-j${VITA_M0_BUILD_JOBS}"
        RESULT_VARIABLE DOCKER_RESULT
        OUTPUT_VARIABLE DOCKER_OUTPUT
        ERROR_VARIABLE DOCKER_ERROR)
    if(NOT DOCKER_RESULT EQUAL 0)
        message(FATAL_ERROR
            "The M0 refresh failed in Docker image '${VITA_M0_DOCKER_IMAGE}'.\n${DOCKER_OUTPUT}\n${DOCKER_ERROR}")
    endif()
endif()

file(GLOB DATA_ASM "${GAME_ROOT}/data/*.s")
file(GLOB SONG_ASM "${GAME_ROOT}/sound/songs/*.s" "${GAME_ROOT}/sound/songs/midi/*.s")
file(GLOB MIDI_INPUTS "${GAME_ROOT}/sound/songs/midi/*.mid")
file(GLOB_RECURSE C_SOURCES "${GAME_ROOT}/src/*.c" "${GAME_ROOT}/gflib/*.c")
list(FILTER C_SOURCES EXCLUDE REGEX "\\.inc\\.c$")
set(SCAN_SOURCES ${DATA_ASM} ${SONG_ASM} ${C_SOURCES})
set(ASSET_TARGETS generated)

foreach(MIDI_INPUT IN LISTS MIDI_INPUTS)
    get_filename_component(MIDI_NAME "${MIDI_INPUT}" NAME_WE)
    set(MIDI_ASM "${GAME_ROOT}/sound/songs/midi/${MIDI_NAME}.s")
    if(NOT EXISTS "${MIDI_ASM}")
        message(FATAL_ERROR
            "Generated MIDI assembly is missing: ${MIDI_ASM}. Set VITA_M0_DOCKER_IMAGE to refresh assets, or run the M0 host build in its prepared container.")
    endif()
    file(RELATIVE_PATH MIDI_TARGET "${GAME_ROOT}" "${MIDI_ASM}")
    list(APPEND ASSET_TARGETS "${MIDI_TARGET}")
endforeach()

foreach(SOURCE IN LISTS SCAN_SOURCES)
    execute_process(
        COMMAND "${SCANINC}"
            "-I${GAME_ROOT}/include"
            "-I${GAME_ROOT}"
            "-I${GAME_ROOT}/gflib"
            "-I${GAME_ROOT}/sound"
            "${SOURCE}"
        WORKING_DIRECTORY "${GAME_ROOT}"
        OUTPUT_VARIABLE SCAN_OUTPUT
        RESULT_VARIABLE SCAN_RESULT
        ERROR_VARIABLE SCAN_ERROR)
    if(NOT SCAN_RESULT EQUAL 0)
        message(FATAL_ERROR "scaninc failed while checking ${SOURCE}: ${SCAN_ERROR}")
    endif()

    string(REPLACE "\r" "" SCAN_OUTPUT "${SCAN_OUTPUT}")
    string(REPLACE "\n" ";" SCAN_DEPENDENCIES "${SCAN_OUTPUT}")
    foreach(DEPENDENCY IN LISTS SCAN_DEPENDENCIES)
        if(DEPENDENCY STREQUAL "")
            continue()
        endif()
        if(IS_ABSOLUTE "${DEPENDENCY}")
            get_filename_component(DEPENDENCY_ABSOLUTE "${DEPENDENCY}" ABSOLUTE)
        else()
            get_filename_component(DEPENDENCY_ABSOLUTE "${GAME_ROOT}/${DEPENDENCY}" ABSOLUTE)
        endif()
        if(NOT EXISTS "${DEPENDENCY_ABSOLUTE}")
            message(FATAL_ERROR
                "M0-generated input is missing: ${DEPENDENCY_ABSOLUTE}. Set VITA_M0_DOCKER_IMAGE to refresh assets, or run the M0 host build in its prepared container.")
        endif()
        get_filename_component(DEPENDENCY_EXTENSION "${DEPENDENCY_ABSOLUTE}" LAST_EXT)
        if(DEPENDENCY_EXTENSION MATCHES "^\\.(inc|h|bin|1bpp|4bpp|8bpp|gbapal|lz|rl|latfont|hwjpnfont|fwjpnfont)$")
            file(RELATIVE_PATH DEPENDENCY_TARGET "${GAME_ROOT}" "${DEPENDENCY_ABSOLUTE}")
            list(APPEND ASSET_TARGETS "${DEPENDENCY_TARGET}")
        endif()
    endforeach()
endforeach()

list(REMOVE_DUPLICATES ASSET_TARGETS)
execute_process(
    COMMAND "${PC_MAKE}" --no-print-directory -q -f Makefile_pc NATIVE_LINUX=1 NODEP=0 ${ASSET_TARGETS}
    WORKING_DIRECTORY "${GAME_ROOT}"
    RESULT_VARIABLE FRESHNESS_RESULT
    OUTPUT_VARIABLE FRESHNESS_OUTPUT
    ERROR_VARIABLE FRESHNESS_ERROR)
if(FRESHNESS_RESULT EQUAL 0)
    message("M0-generated headers, maps, MIDI and binary assets are current")
elseif(FRESHNESS_RESULT EQUAL 1)
    message(FATAL_ERROR
        "M0-generated inputs are stale. Refresh them with the prepared M0 container, then rerun this build. Configure -DVITA_M0_DOCKER_IMAGE=<image> to refresh automatically.\n${FRESHNESS_OUTPUT}\n${FRESHNESS_ERROR}")
else()
    message(FATAL_ERROR
        "Could not check M0-generated input freshness (make exited ${FRESHNESS_RESULT}).\n${FRESHNESS_OUTPUT}\n${FRESHNESS_ERROR}")
endif()
