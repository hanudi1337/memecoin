# SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#
# Builds libchiaki's lib/src/streamconnection.c from a patched, build-local copy.
# The chiaki-ng submodule itself stays untouched (same approach chiaki-ng uses
# for its bundled curl).
#
# The patch adds a single switch, `bool ps5view_announce_controller` (default
# true = libchiaki's normal behaviour). When ps5view sets it to false before a
# session starts (ps5view stream --controller none), the session
#   - does not send the Takion CONTROLLER_CONNECTION message, and
#   - does not start the feedback sender, i.e. sends no controller state at all.
# libchiaki only sends CONTROLLER_CONNECTION since Nov 2022 (commit 74d39e6,
# for DualSense haptics); earlier versions streamed from PS4/PS5 without it.
#
# Every anchor must occur exactly once in the pinned source; otherwise the
# configure step stops, so a libchiaki update cannot silently drop the patch.

set(_pv_src "${CHIAKI_DIR}/lib/src/streamconnection.c")
set(_pv_dst_dir "${CMAKE_CURRENT_BINARY_DIR}/chiaki-patched")
set(_pv_dst "${_pv_dst_dir}/streamconnection.c")

file(READ "${_pv_src}" _pv_code)

macro(pv_patch_once anchor replacement)
	string(FIND "${_pv_code}" "${anchor}" _pv_first)
	string(FIND "${_pv_code}" "${anchor}" _pv_last REVERSE)
	if(_pv_first EQUAL -1)
		message(FATAL_ERROR "PatchLibchiaki: anchor not found in ${_pv_src}:\n  ${anchor}\nlibchiaki changed - update ps5view/cmake/PatchLibchiaki.cmake")
	endif()
	if(NOT _pv_first EQUAL _pv_last)
		message(FATAL_ERROR "PatchLibchiaki: anchor occurs more than once in ${_pv_src}:\n  ${anchor}")
	endif()
	string(REPLACE "${anchor}" "${replacement}" _pv_code "${_pv_code}")
endmacro()

# 1. the switch itself, right after the forward declaration
pv_patch_once(
	"static ChiakiErrorCode stream_connection_send_controller_connection(ChiakiStreamConnection *stream_connection);"
	"static ChiakiErrorCode stream_connection_send_controller_connection(ChiakiStreamConnection *stream_connection);\n// ps5view patch (ps5view/cmake/PatchLibchiaki.cmake): false = announce no controller, send no controller data\nCHIAKI_EXPORT bool ps5view_announce_controller = true;")

# 2. no CONTROLLER_CONNECTION message
pv_patch_once(
	"ChiakiErrorCode err = stream_connection_send_controller_connection(stream_connection);"
	"ChiakiErrorCode err = ps5view_announce_controller ? stream_connection_send_controller_connection(stream_connection) : CHIAKI_ERR_SUCCESS;")

# 3. no feedback sender (controller state / button history packets)
pv_patch_once(
	"err = chiaki_feedback_sender_init(&stream_connection->feedback_sender, &stream_connection->takion);"
	"err = ps5view_announce_controller ? chiaki_feedback_sender_init(&stream_connection->feedback_sender, &stream_connection->takion) : CHIAKI_ERR_SUCCESS;")
pv_patch_once(
	"stream_connection->feedback_sender_active = true;"
	"stream_connection->feedback_sender_active = ps5view_announce_controller;")
pv_patch_once(
	"chiaki_feedback_sender_set_controller_state(&stream_connection->feedback_sender, &session->controller_state);"
	"if(ps5view_announce_controller) chiaki_feedback_sender_set_controller_state(&stream_connection->feedback_sender, &session->controller_state);")
pv_patch_once(
	"chiaki_feedback_sender_fini(&stream_connection->feedback_sender);"
	"if(ps5view_announce_controller) chiaki_feedback_sender_fini(&stream_connection->feedback_sender);")

# Write only if the content changed (avoids needless rebuilds).
file(MAKE_DIRECTORY "${_pv_dst_dir}")
file(WRITE "${_pv_dst}.tmp" "${_pv_code}")
configure_file("${_pv_dst}.tmp" "${_pv_dst}" COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pv_src}")

# Swap the source in chiaki-lib.
get_target_property(_pv_sources chiaki-lib SOURCES)
list(FIND _pv_sources "src/streamconnection.c" _pv_idx)
if(_pv_idx EQUAL -1)
	message(FATAL_ERROR "PatchLibchiaki: src/streamconnection.c not among chiaki-lib's sources")
endif()
list(REMOVE_AT _pv_sources ${_pv_idx})
list(INSERT _pv_sources ${_pv_idx} "${_pv_dst}")
set_property(TARGET chiaki-lib PROPERTY SOURCES "${_pv_sources}")
# the copy still includes "utils.h" etc. from libchiaki's source directory
target_include_directories(chiaki-lib PRIVATE "${CHIAKI_DIR}/lib/src")
message(STATUS "ps5view: libchiaki streamconnection.c patched (view-only switch)")
