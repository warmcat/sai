set(SUB "sai-server")

set(CPACK_DEBIAN_SERVER_PACKAGE_NAME ${SUB})

set(SRCS
	s-sai.c
	s-conf.c
	s-notification.c
	s-comms.c
	s-ws-builder.c
	s-task.c
	s-central.c
	s-websrv.c
	s-resource.c
)

set(requirements 1)
require_lws_config(LWS_WITH_SERVER		1 requirements)
require_lws_config(LWS_WITH_GENCRYPTO		1 requirements)
require_lws_config(LWS_WITH_UNIX_SOCK		1 requirements)
require_lws_config(LWS_WITH_STRUCT_SQLITE3	1 requirements)

if (requirements)
	add_executable(${SUB} ${SRCS})
	if (APPLE)
		set_property(TARGET sai-server PROPERTY MACOSX_RPATH YES)
	endif()

	#
	# sqlite3 paths (server)
	#

	find_path(   SQLITE3_INC_PATH NAMES "sqlite3.h")
	find_library(SQLITE3_LIB_PATH NAMES "sqlite3")
	
	if (SQLITE3_INC_PATH AND SQLITE3_LIB_PATH)
		include_directories(BEFORE "${SQLITE3_INC_PATH}")
	else()
		message(FATAL_ERROR " Unable to find sqlite3")
	endif()

	target_link_libraries(${SUB} websockets ${SQLITE3_LIB_PATH})

       	include_directories(BEFORE "${SAI_LWS_INC_PATH}")

	CHECK_C_SOURCE_COMPILES("#include <libwebsockets.h>\nint
	main(void) {\n#if defined(LWS_HAVE_LIBCAP)\n return
		0;\n#else\n fail;\n#endif\n return 0;\n}\n" HAS_LIBCAP)
	if (HAS_LIBCAP)
		find_library(CAP_LIB_PATH NAMES "cap")
	endif()

	target_link_libraries(${SUB} ${SAI_LWS_LIB_PATH})
	
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries(${SUB} ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (SAI_EXT_PTHREAD_LIBRARIES)
		target_link_libraries(${SUB} ${SAI_EXT_PTHREAD_LIBRARIES})
	endif()

	if (HAS_LIBCAP)
		target_link_libraries(${SUB} ${CAP_LIB_PATH})
	endif()

	if (MSVC)
		target_link_libraries(${SUB} ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()
	
	install(TARGETS ${SUB} RUNTIME DESTINATION "${BIN_DIR}" COMPONENT server)

endif(requirements)

