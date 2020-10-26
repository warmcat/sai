set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-builder")

set(SRCS
	b-sai.c
	b-conf.c
	b-comms.c
	b-mirror.c
	b-nspawn.c
	b-task.c
	b-artifacts.c
	b-logproxy.c
)

set(requirements 1)
require_lws_config(LWS_WITH_CLIENT		1 requirements)
require_lws_config(LWS_WITH_SPAWN		1 requirements)
require_lws_config(LWS_WITH_STRUCT_JSON		1 requirements)
require_lws_config(LWS_WITH_SECURE_STREAMS	1 requirements)
require_lws_config(LWS_WITH_DIR			1 requirements)

if (requirements)
	add_executable("sai-builder" ${SRCS})
	if (APPLE)
		set_property(TARGET sai-builder PROPERTY MACOSX_RPATH YES)
	endif()

	#
	# libgit2 paths (builder)
	#
	if (SAI_BUILDER)
	       #
	       # libgit2 paths
	       #
	       find_path(   SAI_GIT2_INC_PATH NAMES "git2.h")
	       find_library(SAI_GIT2_LIB_PATH NAMES "git2")
	       
	       if (SAI_GIT2_INC_PATH AND SAI_GIT2_LIB_PATH)
		       set(SAI_DEPLIBS "${SAI_GIT2_LIB_PATH}" ${SAI_DEPLIBS})
		       include_directories(BEFORE "${SAI_GIT2_INC_PATH}")
	       else()
		       message(FATAL_ERROR " Unable to find libgit2")
	       endif()
	       
	       target_link_libraries("sai-builder" ${SAI_GIT2_LIB_PATH})
	endif()

	set(CMAKE_REQUIRED_LIBRARIES ${SAI_GIT2_LIB_PATH})
	
	CHECK_C_SOURCE_COMPILES("#include <git2.h>
        int main(void) {
               const git_error *e = git_error_last();
                return !!e->message;
        }" SAI_HAVE_LIBGIT2_GIT_ERROR)
        
        if (SAI_HAVE_LIBGIT2_GIT_ERROR)
        	set(CMAKE_C_FLAGS "-DSAI_HAVE_LIBGIT2_GIT_ERROR ${CMAKE_C_FLAGS}")
        endif()

	CHECK_C_SOURCE_COMPILES("
                #include <git2.h>
		int main(int argc, char **argv) {
			git_proxy_options o;
			git_proxy_options_init(&o, GIT_PROXY_OPTIONS_VERSION);
			return 0;
	}" LIBGIT2_HAVE_GIT_PROXY_OPTIONS_INIT)

	if (LIBGIT2_HAVE_GIT_PROXY_OPTIONS_INIT)
        	set(CMAKE_C_FLAGS "-DLIBGIT2_HAVE_GIT_PROXY_OPTIONS_INIT ${CMAKE_C_FLAGS}")
        endif()

	#configure_file("${PROJECT_SOURCE_DIR}/cmake/sai_config_private.h.in"
	#	       "${PROJECT_BINARY_DIR}/sai_config_private.h")
	#
	#target_include_directories("sai-builder" PRIVATE "${PROJECT_BINARY_DIR}")



	if (SAI_LWS_INC_PATH)
		target_include_directories("sai-builder" PRIVATE ${SAI_LWS_INC_PATH})
	endif()

	if (HAS_LIBCAP)
		find_library(CAP_LIB_PATH NAMES "cap")
	endif()

	target_link_libraries("sai-builder" ${SAI_LWS_LIB_PATH})
	
	if (LWS_OPENSSL_LIBRARIES)
		target_link_libraries("sai-builder" ${LWS_OPENSSL_LIBRARIES})
	endif()
	
	if (SAI_EXT_PTHREAD_LIBRARIES)
		target_link_libraries("sai-builder" ${SAI_EXT_PTHREAD_LIBRARIES})
	endif()
	if (HAS_LIBCAP)
		target_link_libraries("sai-builder" ${CAP_LIB_PATH})
	endif()

	if (MSVC OR WIN32)
		target_link_libraries("sai-builder" ws2_32.lib userenv.lib psapi.lib iphlpapi.lib)
	endif()

	install(TARGETS "sai-builder"
		RUNTIME DESTINATION "${BIN_DIR}" COMPONENT builder)

endif(requirements)
include(CPack)
