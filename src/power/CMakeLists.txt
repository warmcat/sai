set(SUB "sai-power")
set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-power")

set(SRCS
	p-sai.c
	p-conf.c
	p-comms.c
	p-smartplug.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_SPAWN		1 requirements)
require_lws_config(LWS_WITH_STRUCT_JSON		1 requirements)
require_lws_config(LWS_WITH_SECURE_STREAMS	1 requirements)
require_lws_config(LWS_WITH_DIR			1 requirements)
require_lws_config(LWS_WITH_WOL			1 requirements)


if (requirements)
	add_executable(${SUB} ${SRCS})
	if (APPLE)
		set_property(TARGET ${SUB} PROPERTY MACOSX_RPATH YES)
	endif()

	if (SAI_LWS_INC_PATH)
		target_include_directories(${SUB} PRIVATE ${SAI_LWS_INC_PATH})
	endif()

	if (HAS_LIBCAP)
		find_library(CAP_LIB_PATH NAMES "cap")
	endif()

	target_link_libraries(${SUB} websockets ${SAI_LWS_LIB_PATH})
	
	message("LWS_OPENSSL_LIBRARIES ${SUB} '${LWS_OPENSSL_LIBRARIES}'")
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries(${SUB} ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (SAI_EXT_PTHREAD_LIBRARIES)
		target_link_libraries(${SUB} ${SAI_EXT_PTHREAD_LIBRARIES})
	endif()
	if (HAS_LIBCAP)
		target_link_libraries(${SUB} ${CAP_LIB_PATH})
	endif()

	if (MSVC OR WIN32)
		target_link_libraries(${SUB} ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()

	install(TARGETS "sai-power"
		RUNTIME DESTINATION "${BIN_DIR}" COMPONENT power)

endif(requirements)
include(CPack)
