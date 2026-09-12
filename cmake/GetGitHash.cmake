# Run in cmake script mode (-P) by the sai_git_hash target.
#
# Note: in script mode CMAKE_SOURCE_DIR is forced to the current working
# directory, so the caller must pass the checkout under SAI_SOURCE_DIR.

set(GIT_HASH "unknown")

if(NOT SAI_SOURCE_DIR)
    set(SAI_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
endif()

find_package(Git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always
        WORKING_DIRECTORY "${SAI_SOURCE_DIR}"
        RESULT_VARIABLE GIT_RESULT
        OUTPUT_VARIABLE GIT_OUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    # Git can fail (e.g. running as root in a user directory, or not a
    # checkout); fall through with "unknown" so the header always exists.
    if(GIT_RESULT EQUAL 0 AND NOT "${GIT_OUT}" STREQUAL "")
        set(GIT_HASH "${GIT_OUT}")
    endif()
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
