#
# How sai-web cooks the x-lws-login-* headers the front-end's lws-login
# stamps into who may do what, see test-auth.c.  Run with ctest.
#

include_directories(BEFORE "${SAI_LWS_INC_PATH}")

add_executable(test-auth
	test-auth.c
	../../src/web/w-auth.c
)

target_link_libraries(test-auth ${SAI_LWS_LIB_PATH} ${LIBWEBSOCKETS_DEP_LIBS})

if (LWS_OPENSSL_LIBRARIES)
	target_link_libraries(test-auth ${LWS_OPENSSL_LIBRARIES})
endif()

if (SAI_EXT_PTHREAD_LIBRARIES)
	target_link_libraries(test-auth ${SAI_EXT_PTHREAD_LIBRARIES})
endif()

add_test(NAME auth COMMAND test-auth)
