#
# The problems parser and summary, against the logs in this dir, see
# test-problems.c.  Run with ctest.
#

include_directories(BEFORE "${SAI_LWS_INC_PATH}")

add_executable(test-problems
	test-problems.c
	../../src/common/c-problems.c
	../../src/common/c-problems-summary.c
)

target_link_libraries(test-problems ${SAI_LWS_LIB_PATH} ${LIBWEBSOCKETS_DEP_LIBS})

if (LWS_OPENSSL_LIBRARIES)
	target_link_libraries(test-problems ${LWS_OPENSSL_LIBRARIES})
endif()

if (SAI_EXT_PTHREAD_LIBRARIES)
	target_link_libraries(test-problems ${SAI_EXT_PTHREAD_LIBRARIES})
endif()

add_test(NAME problems COMMAND test-problems ${CMAKE_CURRENT_SOURCE_DIR})
