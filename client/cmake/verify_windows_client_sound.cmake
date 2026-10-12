foreach (required ATRINIK_READOBJ ATRINIK_PYTHON ATRINIK_RUNTIME_HELPER
        ATRINIK_CLIENT_EXECUTABLE)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif ()
endforeach ()

execute_process(
    COMMAND "${ATRINIK_PYTHON}" "${ATRINIK_RUNTIME_HELPER}"
        --readobj "${ATRINIK_READOBJ}" --inspect-only
        --require-import SDL3_mixer.dll "${ATRINIK_CLIENT_EXECUTABLE}"
    RESULT_VARIABLE inspector_result
    OUTPUT_VARIABLE inspector_output
    ERROR_VARIABLE inspector_error)
if (NOT "${inspector_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Could not verify Windows client sound imports: ${inspector_error}")
endif ()
message(STATUS "Windows client sound import verified")
