cmake_minimum_required(VERSION 2.8)
include(CheckCSourceCompiles)

set(SRCS_M
	src/master/sai-master.c
	src/master/conf.c
	src/master/ws.c
)
set(SRCS_B
	src/builder/sai-builder.c
	src/builder/conf.c
	src/builder/protocol.c
)

set(LIB_DIR       lib     CACHE PATH "Install dir for libraries")
set(BIN_DIR       bin     CACHE PATH "Install dir for executables")
set(INCLUDE_DIR   include CACHE PATH "Install dir for header files")
set(DATA_DIR      share   CACHE PATH "Install dir for data files")

#
# let the code know where the assets were installed
#
set(CMAKE_C_FLAGS "-D LWS_DATA_DIR=\\\"${CMAKE_INSTALL_PREFIX}/${DATA_DIR}\\\" ${CMAKE_C_FLAGS}")


# If we are being built as part of lws, confirm current build config supports
# reqconfig, else skip building ourselves.
#
# If we are being built externally, confirm installed lws was configured to
# support reqconfig, else error out with a helpful message about the problem.
#
MACRO(require_lws_config reqconfig _val result)

	if (DEFINED ${reqconfig})
	if (${reqconfig})
		set (rq 1)
	else()
		set (rq 0)
	endif()
	else()
		set(rq 0)
	endif()

	if (${_val} EQUAL ${rq})
		set(SAME 1)
	else()
		set(SAME 0)
	endif()

	if (LWS_WITH_MINIMAL_EXAMPLES AND NOT ${SAME})
		if (${_val})
			message("${SAMP}: skipping as lws being built without ${reqconfig}")
		else()
			message("${SAMP}: skipping as lws built with ${reqconfig}")
		endif()
		set(${result} 0)
	else()
		if (LWS_WITH_MINIMAL_EXAMPLES)
			set(MET ${SAME})
		else()
			CHECK_C_SOURCE_COMPILES("#include <libwebsockets.h>\nint main(void) {\n#if defined(${reqconfig})\n return 0;\n#else\n fail;\n#endif\n return 0;\n}\n" HAS_${reqconfig})
			if (NOT DEFINED HAS_${reqconfig} OR NOT HAS_${reqconfig})
				set(HAS_${reqconfig} 0)
			else()
				set(HAS_${reqconfig} 1)
			endif()
			if ((HAS_${reqconfig} AND ${_val}) OR (NOT HAS_${reqconfig} AND NOT ${_val}))
				set(MET 1)
			else()
				set(MET 0)
			endif()
		endif()
		if (NOT MET)
			if (${_val})
				message(FATAL_ERROR "This project requires lws must have been configured with ${reqconfig}")
			else()
				message(FATAL_ERROR "Lws configuration of ${reqconfig} is incompatible with this project")
			endif()
		endif()
	
	endif()
ENDMACRO()

set(requirements 1)
require_lws_config(LWS_ROLE_H1 1 requirements)
require_lws_config(LWS_ROLE_WS 1 requirements)
require_lws_config(LWS_WITHOUT_SERVER 0 requirements)
require_lws_config(LWS_WITHOUT_CLIENT 0 requirements)
require_lws_config(LWS_WITH_UNIX_SOCK 1 requirements)

if (requirements)
	add_executable("sai-master" ${SRCS_M})
	add_executable("sai-builder" ${SRCS_B})
	
	if (CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX OR (CMAKE_C_COMPILER_ID MATCHES "Clang") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
		set(CMAKE_C_FLAGS "-Wall -Wsign-compare -Wignored-qualifiers -Wtype-limits -Wuninitialized -Werror -Wundef ${CMAKE_C_FLAGS}" )
	endif()

	if (websockets_shared)
		target_link_libraries("sai-master" websockets_shared)
		add_dependencies("sai-master" websockets_shared)
		
		target_link_libraries("sai-builder" websockets_shared)
		add_dependencies("sai-builder" websockets_shared)
	else()
		target_link_libraries("sai-master" websockets)
		target_link_libraries("sai-builder" websockets)
	endif()

#
# libgit2 paths
#
find_path(   GIT2_INC_PATH NAMES "git2.h")
find_library(GIT2_LIB_PATH NAMES "git2")

if (GIT2_INC_PATH AND GIT2_LIB_PATH)
	include_directories(BEFORE "${GIT2_INC_PATH}")
else()
	message(FATAL_ERROR " Unable to find libgit2")
endif()

target_link_libraries("sai-master"  ${GIT2_LIB_PATH})
target_link_libraries("sai-builder" ${GIT2_LIB_PATH})

	install(TARGETS sai-master sai-builder

		LIBRARY DESTINATION "${LIB_DIR}${LIB_SUFFIX}" COMPONENT libraries
		ARCHIVE DESTINATION "${LIB_DIR}${LIB_SUFFIX}" COMPONENT libraries
		RUNTIME DESTINATION "${BIN_DIR}" COMPONENT libraries
		PUBLIC_HEADER DESTINATION "${INCLUDE_DIR}" COMPONENT dev)

	install(FILES assets/index.html
		      assets/sai.css
		      assets/sai.js
		      assets/lws-common.js
		      assets/sai.svg
		      assets/favicon.ico
		      assets/strict-csp.svg
				DESTINATION "${DATA_DIR}/sai/assets")

endif()
