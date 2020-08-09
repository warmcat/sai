set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-expect")

set(SRCS
	e-sai.c
	e-serial.c
	../common/ss-client-logproxy.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_SPAWN		1 requirements)
require_lws_config(LWS_WITH_STRUCT_JSON		1 requirements)
require_lws_config(LWS_WITH_SECURE_STREAMS	1 requirements)
require_lws_config(LWS_WITH_DIR			1 requirements)

if (requirements AND NOT MSVC)

	CHECK_C_SOURCE_COMPILES("#include <libwebsockets.h>\nint
	main(void) {\n#if defined(LWS_HAVE_LIBCAP)\n return
		0;\n#else\n fail;\n#endif\n return 0;\n}\n" HAS_LIBCAP)
	if (HAS_LIBCAP)
		find_library(CAP_LIB_PATH NAMES "cap")
	endif()

	add_executable("sai-expect" ${SRCS})
	if (APPLE)
		set_property(TARGET sai-expect PROPERTY MACOSX_RPATH YES)
	endif()
	if (SAI_LWS_INC_PATH)
		target_include_directories("sai-expect" PRIVATE ${SAI_LWS_INC_PATH})
	endif()

	target_link_libraries("sai-expect" ${SAI_LWS_LIB_PATH})
	
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries("sai-expect" ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (SAI_EXT_PTHREAD_LIBRARIES AND NOT MSVC)
		target_link_libraries("sai-expect" ${SAI_EXT_PTHREAD_LIBRARIES})
	endif()
	if (HAS_LIBCAP)
		target_link_libraries("sai-expect" ${CAP_LIB_PATH})
	endif()

	if (MSVC OR WIN32)
		target_link_libraries("sai-expect" ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()
	
	install(TARGETS "sai-expect"
		RUNTIME DESTINATION "${BIN_DIR}" COMPONENT expect)

endif(requirements AND NOT MSVC)
include(CPack)
