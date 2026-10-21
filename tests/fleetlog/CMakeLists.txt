#
# How fleet log lines are tagged with the build they're about, and how the
# tags are read back, see test-fleetlog.c.  Run with ctest.
#

include_directories(BEFORE "${SAI_LWS_INC_PATH}")

add_executable(test-fleetlog
	test-fleetlog.c
	../../src/common/c-fleetlog.c
)

target_link_libraries(test-fleetlog ${SAI_LWS_LIB_PATH} ${LIBWEBSOCKETS_DEP_LIBS})

if (LWS_OPENSSL_LIBRARIES)
	target_link_libraries(test-fleetlog ${LWS_OPENSSL_LIBRARIES})
endif()

if (SAI_EXT_PTHREAD_LIBRARIES)
	target_link_libraries(test-fleetlog ${SAI_EXT_PTHREAD_LIBRARIES})
endif()

add_test(NAME fleetlog COMMAND test-fleetlog)
