find_package(Git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE GIT_RESULT
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GIT_RESULT EQUAL 0)
        # Git failed (e.g. running as root in a user directory)
        return()
    endif()
else()
    set(GIT_HASH "unknown")
endif()

set(NEW_HASH_FILE_CONTENT "#define SAI_BUILD_INFO \"${CPACK_PACKAGE_VERSION}-${GIT_HASH}\"\n")

if(EXISTS "${OUTPUT_FILE}")
    file(READ "${OUTPUT_FILE}" OLD_HASH_FILE_CONTENT)
    if(NOT "${OLD_HASH_FILE_CONTENT}" STREQUAL "${NEW_HASH_FILE_CONTENT}")
        file(WRITE "${OUTPUT_FILE}" "${NEW_HASH_FILE_CONTENT}")
    endif()
else()
    file(WRITE "${OUTPUT_FILE}" "${NEW_HASH_FILE_CONTENT}")
endif()
