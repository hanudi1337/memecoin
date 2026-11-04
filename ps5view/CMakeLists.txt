# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#
# ps5view - View-only PS5 Remote Play client built on libchiaki (chiaki-ng).

cmake_minimum_required(VERSION 3.16)

project(ps5view VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PS5VIEW_SOURCE_URL "https://github.com/hanudi1337/memecoin/tree/main/ps5view"
	CACHE STRING "Where the corresponding source code of this build can be obtained (AGPL-3.0)")
option(PS5VIEW_ENABLE_TESTS "Build unit and integration tests" ON)

set(CHIAKI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/chiaki-ng")
if(NOT EXISTS "${CHIAKI_DIR}/lib/CMakeLists.txt")
	message(FATAL_ERROR
		"chiaki-ng submodule missing. Run:\n"
		"  git submodule update --init ps5view/third-party/chiaki-ng\n"
		"  git -C ps5view/third-party/chiaki-ng submodule update --init third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl")
endif()
foreach(_sub nanopb jerasure gf-complete curl)
	if(NOT EXISTS "${CHIAKI_DIR}/third-party/${_sub}/.git" AND NOT EXISTS "${CHIAKI_DIR}/third-party/${_sub}/CMakeLists.txt" AND NOT EXISTS "${CHIAKI_DIR}/third-party/${_sub}/src")
		message(FATAL_ERROR "chiaki-ng submodule third-party/${_sub} missing. Run:\n"
			"  git -C ps5view/third-party/chiaki-ng submodule update --init third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl")
	endif()
endforeach()

# ---------------------------------------------------------------------------
# libchiaki: only the library, none of the chiaki-ng front ends.
# Cache variables are forced so chiaki-ng's own option() defaults cannot win.
# ---------------------------------------------------------------------------
set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_ANDROID OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_BOREALIS OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
set(CHIAKI_ENABLE_FFMPEG_DECODER ON CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
set(CHIAKI_ENABLE_SPEEX OFF CACHE STRING "" FORCE)
set(CHIAKI_LIB_ENABLE_OPUS ON CACHE BOOL "" FORCE)
set(CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER OFF CACHE BOOL "" FORCE)
add_subdirectory("${CHIAKI_DIR}" chiaki-ng EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Our own dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)  # chiaki-lib does not link libcrypto itself on WIN32
find_package(SDL2 CONFIG REQUIRED)

# Reuse chiaki-ng's find modules for FFmpeg and Opus, but only for these calls,
# so its other modules (FindSDL2, FindCURL, ...) do not shadow anything here.
set(_pv_saved_module_path "${CMAKE_MODULE_PATH}")
list(APPEND CMAKE_MODULE_PATH "${CHIAKI_DIR}/cmake")
find_package(FFMPEG REQUIRED COMPONENTS avcodec avutil swscale)
find_package(Opus REQUIRED)
set(CMAKE_MODULE_PATH "${_pv_saved_module_path}")

pkg_search_module(PV_JSONC REQUIRED json-c IMPORTED_TARGET)

if(TARGET SDL2::SDL2)
	set(PV_SDL2_TARGET SDL2::SDL2)
elseif(TARGET SDL2::SDL2-static)
	set(PV_SDL2_TARGET SDL2::SDL2-static)
else()
	message(FATAL_ERROR "SDL2 found but no SDL2::SDL2 target available")
endif()

# ---------------------------------------------------------------------------
# Version / build info
# ---------------------------------------------------------------------------
find_package(Git QUIET)
set(PV_CHIAKI_COMMIT "unknown")
set(PV_GIT_COMMIT "unknown")
if(GIT_FOUND)
	execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CHIAKI_DIR}" rev-parse HEAD
		OUTPUT_VARIABLE _out RESULT_VARIABLE _res ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(_res EQUAL 0)
		set(PV_CHIAKI_COMMIT "${_out}")
	endif()
	execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short HEAD
		OUTPUT_VARIABLE _out RESULT_VARIABLE _res ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(_res EQUAL 0)
		set(PV_GIT_COMMIT "${_out}")
	endif()
endif()
configure_file(src/pv_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/pv_version.h" @ONLY)

# ---------------------------------------------------------------------------
# ps5view
# ---------------------------------------------------------------------------
set(PV_CORE_SOURCES
	src/app.c
	src/args.c
	src/audio.c
	src/cmd_account.c
	src/cmd_check.c
	src/cmd_discover.c
	src/cmd_list_audio.c
	src/cmd_profiles.c
	src/cmd_register.c
	src/cmd_stream.c
	src/config.c
	src/diag.c
	src/discover.c
	src/errors.c
	src/log.c
	src/netif.c
	src/oauth.c
	src/util.c
	src/video.c
)

add_library(ps5view-core STATIC ${PV_CORE_SOURCES})
target_include_directories(ps5view-core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(ps5view-core PRIVATE ${Opus_INCLUDE_DIRS})
target_compile_definitions(ps5view-core PUBLIC SDL_MAIN_HANDLED)
target_link_libraries(ps5view-core PUBLIC
	chiaki-lib
	CURL::libcurl
	PkgConfig::PV_JSONC
	FFMPEG::avcodec FFMPEG::avutil FFMPEG::swscale
	${PV_SDL2_TARGET}
	${Opus_LIBRARIES}
	OpenSSL::Crypto
	Threads::Threads)
if(UNIX)
	target_link_libraries(ps5view-core PUBLIC m)
endif()
if(WIN32)
	target_compile_definitions(ps5view-core PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
	target_link_libraries(ps5view-core PUBLIC iphlpapi ws2_32 shell32 ole32 bcrypt crypt32)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(ps5view-core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

add_executable(ps5view src/main.c)
target_link_libraries(ps5view PRIVATE ps5view-core)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(ps5view PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# Note: ps5view is deliberately a console application (no -mwindows), so all
# diagnostics are visible. Unicode arguments are fetched via GetCommandLineW().

install(TARGETS ps5view RUNTIME DESTINATION .)
install(FILES COPYING README.md DESTINATION .)
install(FILES "${CHIAKI_DIR}/LICENSES/AGPL-3.0-only-OpenSSL.txt" DESTINATION licenses)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(PS5VIEW_ENABLE_TESTS)
	enable_testing()
	add_executable(ps5view-tests tests/test_main.c)
	target_link_libraries(ps5view-tests PRIVATE ps5view-core)
	target_include_directories(ps5view-tests PRIVATE ${Opus_INCLUDE_DIRS})
	target_compile_definitions(ps5view-tests PRIVATE
		PV_CHIAKI_SOURCE_DIR="${CHIAKI_DIR}"
		PV_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
	add_test(NAME unit COMMAND ps5view-tests)

	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(Python3_FOUND)
		add_test(NAME integration
			COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py" "$<TARGET_FILE:ps5view>")
		set_tests_properties(integration PROPERTIES TIMEOUT 240)
	endif()
endif()
