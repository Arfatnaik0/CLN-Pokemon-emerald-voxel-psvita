if(NOT DEFINED AS OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT OR NOT DEFINED MODE
        OR NOT DEFINED SCANINC OR NOT DEFINED DEPFILE)
    message(FATAL_ERROR "CompileAsm.cmake requires AS, SOURCE, OUTPUT, MODE, SCANINC and DEPFILE")
endif()

execute_process(
    COMMAND "${SCANINC}"
        "-I${ROOT}/include"
        "-I${ROOT}"
        "-I${ROOT}/gflib"
        "-I${ROOT}/sound"
        "${SOURCE}"
    WORKING_DIRECTORY "${ROOT}"
    OUTPUT_VARIABLE SCAN_OUTPUT
    RESULT_VARIABLE SCAN_RESULT
    ERROR_VARIABLE SCAN_ERROR)
if(NOT SCAN_RESULT EQUAL 0)
    message(FATAL_ERROR "scaninc failed for ${SOURCE}: ${SCAN_ERROR}")
endif()

function(escape_make_path OUTPUT_VARIABLE PATH_VALUE)
    set(ESCAPED_PATH "${PATH_VALUE}")
    string(REPLACE "\\" "\\\\" ESCAPED_PATH "${ESCAPED_PATH}")
    string(REPLACE " " "\\ " ESCAPED_PATH "${ESCAPED_PATH}")
    string(REPLACE "#" "\\#" ESCAPED_PATH "${ESCAPED_PATH}")
    string(REPLACE "$" "$$" ESCAPED_PATH "${ESCAPED_PATH}")
    set(${OUTPUT_VARIABLE} "${ESCAPED_PATH}" PARENT_SCOPE)
endfunction()

escape_make_path(DEPFILE_TARGET "${OUTPUT}")
set(DEPFILE_LINE "${DEPFILE_TARGET}:")
string(REPLACE "\r" "" SCAN_OUTPUT "${SCAN_OUTPUT}")
string(REPLACE "\n" ";" SCAN_DEPENDENCIES "${SCAN_OUTPUT}")
foreach(DEPENDENCY IN LISTS SCAN_DEPENDENCIES)
    if(DEPENDENCY STREQUAL "")
        continue()
    endif()
    if(IS_ABSOLUTE "${DEPENDENCY}")
        get_filename_component(DEPENDENCY_ABSOLUTE "${DEPENDENCY}" ABSOLUTE)
    else()
        get_filename_component(DEPENDENCY_ABSOLUTE "${ROOT}/${DEPENDENCY}" ABSOLUTE)
    endif()
    if(NOT EXISTS "${DEPENDENCY_ABSOLUTE}")
        message(FATAL_ERROR "Assembly input dependency is missing: ${DEPENDENCY_ABSOLUTE}")
    endif()
    escape_make_path(DEPENDENCY_ESCAPED "${DEPENDENCY_ABSOLUTE}")
    string(APPEND DEPFILE_LINE " ${DEPENDENCY_ESCAPED}")
endforeach()
file(WRITE "${DEPFILE}" "${DEPFILE_LINE}\n")

set(ASSEMBLY_FILE "${OUTPUT}.s")
if(MODE STREQUAL "preprocessed")
    set(PREPROCESSED_ASSEMBLY "${OUTPUT}.preproc")
    set(CPP_ASSEMBLY "${OUTPUT}.cpp")
    set(ENUM_ASSEMBLY "${OUTPUT}.enum")
    execute_process(
        COMMAND "${PREPROC}" "${SOURCE}" "${CHARMAP}"
        WORKING_DIRECTORY "${ROOT}"
        OUTPUT_FILE "${PREPROCESSED_ASSEMBLY}"
        RESULT_VARIABLE PREPROC_RESULT
        ERROR_VARIABLE PREPROC_ERROR)
    if(NOT PREPROC_RESULT EQUAL 0)
        message(FATAL_ERROR "Game assembly preprocessor failed for ${SOURCE}: ${PREPROC_ERROR}")
    endif()

    execute_process(
        COMMAND "${HOST_CPP}" -x c "-I${ROOT}/include" ${DEFINE_ARGS}
            "${PREPROCESSED_ASSEMBLY}" -o "${CPP_ASSEMBLY}"
        WORKING_DIRECTORY "${ROOT}"
        RESULT_VARIABLE CPP_RESULT
        ERROR_VARIABLE CPP_ERROR)
    if(NOT CPP_RESULT EQUAL 0)
        message(FATAL_ERROR "Host C preprocessor failed for ${SOURCE}: ${CPP_ERROR}")
    endif()

    execute_process(
        COMMAND "${PREPROC}" -ie "${SOURCE}" "${CHARMAP}"
        WORKING_DIRECTORY "${ROOT}"
        INPUT_FILE "${CPP_ASSEMBLY}"
        OUTPUT_FILE "${ENUM_ASSEMBLY}"
        RESULT_VARIABLE ENUM_RESULT
        ERROR_VARIABLE ENUM_ERROR)
    if(NOT ENUM_RESULT EQUAL 0)
        message(FATAL_ERROR "Enum/string preprocessor failed for ${SOURCE}: ${ENUM_ERROR}")
    endif()
    file(READ "${ENUM_ASSEMBLY}" ASSEMBLY_OUTPUT)
elseif(MODE STREQUAL "song")
    file(READ "${SOURCE}" ASSEMBLY_OUTPUT)
elseif(MODE STREQUAL "direct")
    file(READ "${SOURCE}" ASSEMBLY_OUTPUT)
else()
    message(FATAL_ERROR "Unsupported assembly mode '${MODE}' for ${SOURCE}")
endif()

string(REPLACE ".4byte" ".word" ASSEMBLY_OUTPUT "${ASSEMBLY_OUTPUT}")
string(REPLACE ".2byte" ".short" ASSEMBLY_OUTPUT "${ASSEMBLY_OUTPUT}")

if(MODE STREQUAL "preprocessed" AND DEFINED SPECIALS AND EXISTS "${SPECIALS}")
    file(WRITE "${ASSEMBLY_FILE}" ".include \"${SPECIALS}\"\n${ASSEMBLY_OUTPUT}")
else()
    file(WRITE "${ASSEMBLY_FILE}" "${ASSEMBLY_OUTPUT}")
endif()

execute_process(
    COMMAND "${AS}" -march=armv7-a "-I${ROOT}" "-I${ROOT}/sound" -o "${OUTPUT}" "${ASSEMBLY_FILE}"
    WORKING_DIRECTORY "${ROOT}"
    RESULT_VARIABLE AS_RESULT
    ERROR_VARIABLE AS_ERROR)
if(NOT AS_RESULT EQUAL 0)
    message(FATAL_ERROR "Vita assembler failed for ${SOURCE}: ${AS_ERROR}")
endif()
