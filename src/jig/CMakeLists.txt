set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-jig")

set(SRCS
	j-sai.c
	j-conf.c
	j-server.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_UNIX_SOCK		1 requirements)

if (requirements)

	find_path(   GPIOD_INC_PATH NAMES "gpiod.h")
	find_library(GPIOD_LIB_PATH NAMES "gpiod")
	
	if (NOT GPIOD_INC_PATH OR NOT GPIOD_LIB_PATH)
		message(FATAL_ERROR " Unable to find libgpiod")
	endif()
	include_directories(BEFORE "${GPIOD_INC_PATH}")

	add_executable("sai-jig" ${SRCS})
	target_link_libraries("sai-jig" ${GPIOD_LIB_PATH})

	if (APPLE)
		set_property(TARGET sai-device PROPERTY MACOSX_RPATH YES)
	endif()

	target_link_libraries("sai-jig" ${SAI_LWS_LIB_PATH})
	
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries("sai-jig" ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (SAI_EXT_PTHREAD_LIBRARIES)
		target_link_libraries("sai-jig" ${SAI_EXT_PTHREAD_LIBRARIES})
	endif()
	if (HAS_LIBCAP)
		target_link_libraries("sai-jig" ${CAP_LIB_PATH})
	endif()

	if (MSVC OR WIN32)
		target_link_libraries("sai-jig" ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()
	
	install(TARGETS "sai-jig"
	RUNTIME DESTINATION "${BIN_DIR}" COMPONENT jig)

endif(requirements)
include(CPack)