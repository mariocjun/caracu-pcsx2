# SPDX-FileCopyrightText: 2026 Caracu contributors
# SPDX-License-Identifier: GPL-3.0+
add_executable(caracu-ps2d Main.cpp HostUI.cpp Host.manifest)
execute_process(COMMAND git status --porcelain --untracked-files=normal
	WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE CARACU_DIRTY_OUTPUT
	RESULT_VARIABLE CARACU_GIT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE)
if(CARACU_GIT_STATUS EQUAL 0 AND CARACU_DIRTY_OUTPUT STREQUAL "")
	target_compile_definitions(caracu-ps2d PRIVATE CARACU_SOURCE_DIRTY=0)
else()
	target_compile_definitions(caracu-ps2d PRIVATE CARACU_SOURCE_DIRTY=1)
endif()
target_include_directories(caracu-ps2d PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2")
target_link_libraries(caracu-ps2d PRIVATE PCSX2_FLAGS PCSX2 ole32)
add_custom_command(TARGET caracu-ps2d POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_directory
		"${CMAKE_SOURCE_DIR}/bin/resources" "$<TARGET_FILE_DIR:caracu-ps2d>/resources"
	VERBATIM)
