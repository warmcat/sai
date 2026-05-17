set(SUB "sai-virt")
set(CPACK_DEBIAN_VIRT_PACKAGE_NAME "sai-virt")

set(SRCS
	v-sai.c
	v-ws-server.c
	v-libvirt.c
	v-http-api.c
	../common/c-utils.c
	../common/struct-metadata.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_STRUCT_JSON		1 requirements)
require_lws_config(LWS_WITH_SECURE_STREAMS	1 requirements)

if (requirements)
	add_executable(${SUB} ${SRCS})
add_dependencies(${SUB} sai_git_hash)
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

	install(TARGETS "sai-virt"
		RUNTIME DESTINATION "${BIN_DIR}" COMPONENT virt)

endif(requirements)
include(CPack)
