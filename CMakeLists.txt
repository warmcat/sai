project(sai C)
cmake_minimum_required(VERSION 2.8)
find_package(libwebsockets CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH ${LWS_CMAKE_DIR})
include(CheckCSourceCompiles)
include(LwsCheckRequirements)

set(CMAKE_SKIP_RPATH 1)

find_package(Git)
if(GIT_EXECUTABLE)
        execute_process(
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                COMMAND "${GIT_EXECUTABLE}" describe --tags --always
                OUTPUT_VARIABLE GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                )
endif()


set(PACKAGE "sai")
set(CPACK_PACKAGE_NAME "${PACKAGE}")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "2")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_RELEASE 1)
if (WIN32 OR MSVC)
	set(CPACK_GENERATOR "7Z")
else()
	set(CPACK_GENERATOR "RPM")
endif()
set(CPACK_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
set(LWS_BUILD_HASH ${CPACK_PACKAGE_VERSION}-${GIT_HASH})
message("Git commit hash: ${LWS_BUILD_HASH}")

set(CPACK_PACKAGE_VENDOR "andy@warmcat.com")
set(CPACK_PACKAGE_CONTACT "andy@warmcat.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PACKAGE} ${PACKAGE_VERSION}")
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${PROJECT_SOURCE_DIR}/cmake/Description")

#set(CPACK_DEBIAN_PACKAGE_RELEASE_DIST 1)
set(CPACK_DEB_COMPONENT_INSTALL 1)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_COMPONENT 1)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_SERVER_PACKAGE_NAME "sai-server")
set(CPACK_DEBIAN_BUILDER_PACKAGE_NAME "sai-builder")

option(SAI_SERVER "Build the server + web part" ON)
option(SAI_BUILDER "Build the builder part" ON)

set(LWS_OPENSSL_LIBRARIES CACHE PATH "Path to the OpenSSL library")
set(LWS_OPENSSL_INCLUDE_DIRS CACHE PATH "Path to the OpenSSL include directory")

if (NOT SAI_SERVER AND NOT SAI_BUILDER)
	message(FATAL_ERROR "Have to build both or one or the other")
endif()

set(LIB_DIR       lib     CACHE PATH "Install dir for libraries")
set(BIN_DIR       bin     CACHE PATH "Install dir for executables")
set(INCLUDE_DIR   include CACHE PATH "Install dir for header files")
set(DATA_DIR      share   CACHE PATH "Install dir for data files")

set(SAI_LWS_INC_PATH CACHE PATH "Path to lws headers dir")
set(SAI_LWS_LIB_PATH CACHE PATH "Path to lws library dir")
set(SAI_EXT_PTHREAD_INCLUDE_DIR CACHE PATH "Path to an external pthreads include directory")
set(SAI_EXT_PTHREAD_LIBRARIES CACHE PATH "Path to an external pthreads library")

if (NOT SAI_LWS_INC_PATH)
	find_path(XSAI_LWS_INC_PATH "libwebsockets.h")
	set(SAI_LWS_INC_PATH ${XSAI_LWS_INC_PATH})
	if (NOT XSAI_LWS_INC_PATH)
		message(FATAL_ERROR "Can't find lws headers")
	endif()
endif()

list(APPEND CMAKE_REQUIRED_INCLUDES ${SAI_LWS_INC_PATH})
message("Required incs: ${CMAKE_REQUIRED_INCLUDES}")

if (NOT SAI_LWS_LIB_PATH)
	find_library(XSAI_LWS_LIB_PATH NAMES "websockets")
	set(SAI_LWS_LIB_PATH ${XSAI_LWS_LIB_PATH})
	if (NOT SAI_LWS_LIB_PATH)
		message(FATAL_ERROR "Can't find lws lib")
	endif()
endif()

if (SAI_EXT_PTHREAD_INCLUDE_DIR)
	list(APPEND CMAKE_REQUIRED_INCLUDES ${SAI_EXT_PTHREAD_INCLUDE_DIR})
	include_directories(${SAI_EXT_PTHREAD_INCLUDE_DIR})

	set(CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS} " -DHAVE_STRUCT_TIMESPEC=1")
endif()

message("LWS: header path ${SAI_LWS_INC_PATH}, lib path ${SAI_LWS_LIB_PATH}")

#
# let the code know where the assets were installed
#
if (NOT MSVC)
	set(CMAKE_C_FLAGS "-pthread ${CMAKE_C_FLAGS}")
endif()

if (APPLE)
	set(CMAKE_C_FLAGS "-I/usr/local/include ${CMAKE_C_FLAGS}")
	link_directories("/usr/local/lib")
endif()

if (LWS_OPENSSL_INCLUDE_DIRS)
	set(CMAKE_C_FLAGS "-I\"${LWS_OPENSSL_INCLUDE_DIRS}\" ${CMAKE_C_FLAGS}")
endif()


set(CMAKE_C_FLAGS "-DBUILD_INFO=\\\"${LWS_BUILD_HASH}\\\" ${CMAKE_C_FLAGS}")

CHECK_C_SOURCE_COMPILES("#include <libwebsockets.h>\nint
	main(void) {\n#if defined(LWS_HAVE_LIBCAP)\n return
		0;\n#else\n fail;\n#endif\n return 0;\n}\n" HAS_LIBCAP)


set(requirements 1)
require_lws_config(LWS_ROLE_H1				1 requirements)
require_lws_config(LWS_ROLE_WS				1 requirements)
require_lws_config(LWS_WITH_JOSE			1 requirements)

if (requirements)

	if (CMAKE_COMPILER_IS_GNUCC OR
	    CMAKE_COMPILER_IS_GNUCXX OR
	    (CMAKE_C_COMPILER_ID MATCHES "Clang") OR
	    (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
		set(CMAKE_C_FLAGS "-Wall -Wsign-compare -Wignored-qualifiers -Wtype-limits -Wuninitialized -Werror -Wundef ${CMAKE_C_FLAGS}" )
	endif()

	if (SAI_SERVER)
		add_subdirectory(src/server)
		add_subdirectory(src/web)
	endif()
	if (SAI_BUILDER)
		add_subdirectory(src/builder)
		if (NOT MSVC AND NOT WIN32)
			add_subdirectory(src/device)
			add_subdirectory(src/expect)
			if (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
				add_subdirectory(src/jig)
			endif()
		endif()
	endif()
	
endif(requirements)
include(CPack)

