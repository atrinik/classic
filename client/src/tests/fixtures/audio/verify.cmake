file(SHA256 "${FIXTURE}" ACTUAL_SHA256)
if(NOT ACTUAL_SHA256 STREQUAL EXPECTED_SHA256)
    message(FATAL_ERROR
        "Audio fixture checksum mismatch: expected ${EXPECTED_SHA256}, got ${ACTUAL_SHA256}")
endif()
