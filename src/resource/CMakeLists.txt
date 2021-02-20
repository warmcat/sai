set(SUB "sai-resource")
set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-resource")

set(SRCS
	r-sai.c
	r-comms.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_SPAWN		1 requirements)
require_lws_config(LWS_WITH_STRUCT_JSON		1 requirements)
require_lws_config(LWS_WITH_SECURE_STREAMS	1 requirements)
require_lws_config(LWS_WITH_DIR			1 requirements)

if (requirements AND NOT MSVC)

	add_executable(${SUB} ${SRCS})
	if (APPLE)
		set_property(TARGET sai-resource PROPERTY MACOSX_RPATH YES)
	endif()
	if (SAI_LWS_INC_PATH)
		target_include_directories(${SUB} PRIVATE ${SAI_LWS_INC_PATH})
	endif()

	target_link_libraries(${SUB} websockets ${SAI_LWS_LIB_PATH})
	
	message("LWS_OPENSSL_LIBRARIES ${SUB} '${LWS_OPENSSL_LIBRARIES}'")
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries(${SUB} ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (MSVC OR WIN32)
		target_link_libraries(${SUB} ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()
	
	install(TARGETS ${SUB}
		RUNTIME DESTINATION "${BIN_DIR}" COMPONENT resource)

endif(requirements AND NOT MSVC)
include(CPack)
