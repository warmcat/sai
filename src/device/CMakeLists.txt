set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-device")

set(SRCS
	d-sai.c
	d-conf.c
	../common/ss-client-logproxy.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_SPAWN		1 requirements)
require_lws_config(LWS_WITH_STRUCT_JSON		1 requirements)
require_lws_config(LWS_WITH_SECURE_STREAMS	1 requirements)
require_lws_config(LWS_WITH_DIR			1 requirements)

if (requirements)

	add_executable("sai-device" ${SRCS})
	if (APPLE)
		set_property(TARGET sai-device PROPERTY MACOSX_RPATH YES)
	endif()

	target_link_libraries("sai-device" ${SAI_LWS_LIB_PATH})
	
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries("sai-device" ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (SAI_EXT_PTHREAD_LIBRARIES)
		target_link_libraries("sai-device" ${SAI_EXT_PTHREAD_LIBRARIES})
	endif()
	if (HAS_LIBCAP)
		target_link_libraries("sai-device" ${CAP_LIB_PATH})
	endif()

	if (MSVC OR WIN32)
		target_link_libraries("sai-device" ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()
	
	install(TARGETS "sai-device"
	RUNTIME DESTINATION "${BIN_DIR}" COMPONENT device)

endif(requirements)
include(CPack)
