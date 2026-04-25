find_package(Git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
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
