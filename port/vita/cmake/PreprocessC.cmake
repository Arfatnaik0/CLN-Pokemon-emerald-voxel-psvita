if(NOT DEFINED C_COMPILER OR NOT DEFINED PREPROC OR NOT DEFINED SOURCE OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "PreprocessC.cmake requires C_COMPILER, PREPROC, SOURCE and OUTPUT")
endif()

set(CPP_OUTPUT "${OUTPUT}.i")
execute_process(
    COMMAND "${C_COMPILER}" -E -MMD -MF "${DEPFILE}" -MT "${OUTPUT}" -std=gnu99
        "-iquote${ROOT}/include" "-iquote${ROOT}/gflib"
        ${GAME_DEFINE_ARGS}
        ${PREPROCESS_INCLUDE_ARGS}
        "${SOURCE}" -o "${CPP_OUTPUT}"
    WORKING_DIRECTORY "${ROOT}"
    RESULT_VARIABLE CPP_RESULT
    ERROR_VARIABLE CPP_ERROR)
if(NOT CPP_RESULT EQUAL 0)
    message(FATAL_ERROR "C preprocessing failed for ${SOURCE}: ${CPP_ERROR}")
endif()

execute_process(
    COMMAND "${PREPROC}" "${CPP_OUTPUT}" "${CHARMAP}"
    WORKING_DIRECTORY "${ROOT}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE PREPROC_RESULT
    ERROR_VARIABLE PREPROC_ERROR)
if(NOT PREPROC_RESULT EQUAL 0)
    message(FATAL_ERROR "Game preprocessor failed for ${SOURCE}: ${PREPROC_ERROR}")
endif()
