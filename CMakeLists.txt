cmake_minimum_required(VERSION 3.16)
project(psvita_usb_audio_midi C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(PSVITA_USB_AUDIO_MIDI_VITA "Build Vita kernel/client/diagnostic artifacts" OFF)

set(PSVITA_USB_AUDIO_MIDI_SHARED_SOURCES
	src/common/audio_control.c
	src/common/audio_core.c
	src/common/midi_core.c
	src/common/usb_descriptors.c
	src/common/midi_timing.c
	src/common/owner_guard.c
	src/common/stock_state.c
	src/common/takeover.c
)

add_library(psvita_usb_audio_midi_core STATIC
	${PSVITA_USB_AUDIO_MIDI_SHARED_SOURCES}
	src/common/audio_diagnostics.c
	src/common/scope_text.c
)
target_include_directories(psvita_usb_audio_midi_core PUBLIC include src/common)
target_compile_options(psvita_usb_audio_midi_core PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(psvita_usb_audio_midi_core_tests tests/core_tests.c)
target_link_libraries(psvita_usb_audio_midi_core_tests PRIVATE psvita_usb_audio_midi_core)
target_compile_options(psvita_usb_audio_midi_core_tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME psvita_usb_audio_midi_core_tests COMMAND psvita_usb_audio_midi_core_tests)

if(PSVITA_USB_AUDIO_MIDI_VITA)
	if(NOT DEFINED VITASDK AND DEFINED ENV{VITASDK})
		set(VITASDK "$ENV{VITASDK}")
	endif()
	if(NOT DEFINED VITASDK)
		message(FATAL_ERROR "Set VITASDK before enabling PSVITA_USB_AUDIO_MIDI_VITA")
	endif()
	include("${VITASDK}/share/vita.cmake" REQUIRED)

	add_executable(psvita_usb_audio_midi_kernel
		src/kernel/kernel.c
		src/kernel/usb_driver.c
		${PSVITA_USB_AUDIO_MIDI_SHARED_SOURCES}
	)
	target_include_directories(psvita_usb_audio_midi_kernel PRIVATE include src/common src/kernel)
	target_compile_options(psvita_usb_audio_midi_kernel PRIVATE
		-Wall -Wextra -Werror -O2 -fno-builtin)
	target_link_libraries(psvita_usb_audio_midi_kernel PRIVATE
		SceSysclibForDriver_stub SceThreadmgrForDriver_stub SceSysmemForDriver_stub
		SceUdcdForDriver_stub SceProcEventForDriver_stub SceProcessmgrForDriver_stub
		SceCpuForDriver_stub
	)
	set_target_properties(psvita_usb_audio_midi_kernel PROPERTIES LINK_FLAGS "-nostdlib")
	set(VITA_ELF_CREATE_FLAGS "")
	set(VITA_MAKE_FSELF_FLAGS "")
	vita_create_self(psvita_usb_audio_midi.skprx psvita_usb_audio_midi_kernel
		CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/exports.yml UNSAFE)
	vita_create_stubs(kernel_stubs psvita_usb_audio_midi_kernel
		${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/exports.yml KERNEL)

	add_executable(psvita_usb_audio_midi_client src/client/client.c)
	target_include_directories(psvita_usb_audio_midi_client PRIVATE include)
	target_compile_options(psvita_usb_audio_midi_client PRIVATE
		-Wall -Wextra -Werror -O2)
	target_link_directories(psvita_usb_audio_midi_client PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
	target_link_libraries(psvita_usb_audio_midi_client PRIVATE
		psvita_usb_audio_midi_k_stub SceLibKernel_stub)
	add_dependencies(psvita_usb_audio_midi_client kernel_stubs)
	set_target_properties(psvita_usb_audio_midi_client PROPERTIES
		COMPILE_FLAGS "-D__PSP2_USER__" LINK_FLAGS "-nostdlib")
	set(VITA_ELF_CREATE_FLAGS "")
	set(VITA_MAKE_FSELF_FLAGS "")
	vita_create_self(psvita_usb_audio_midi_client.suprx psvita_usb_audio_midi_client
		CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/src/client/exports.yml UNSAFE)
	vita_create_stubs(client_stubs psvita_usb_audio_midi_client
		${CMAKE_CURRENT_SOURCE_DIR}/src/client/exports.yml)

	find_package(SDL2 REQUIRED)
	add_executable(audio-midi-scope src/audio-midi-scope/main.c
		src/common/audio_diagnostics.c src/common/scope_text.c)
	target_include_directories(audio-midi-scope PRIVATE include src/common)
	target_compile_options(audio-midi-scope PRIVATE -Wall -Wextra -Werror -O2)
	target_link_directories(audio-midi-scope PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
	target_link_libraries(audio-midi-scope PRIVATE
		psvita_usb_audio_midi_c_stub SDL2::SDL2 SceDisplay_stub SceCtrl_stub
		SceIofilemgr_stub SceLibKernel_stub)
	add_dependencies(audio-midi-scope client_stubs)
	set(VITA_ELF_CREATE_FLAGS "")
	set(VITA_MAKE_FSELF_FLAGS "")
	vita_create_self(audio-midi-scope.self audio-midi-scope)
	vita_create_vpk(audio-midi-scope.vpk AUMSCOPE1 audio-midi-scope.self
		VERSION 01.00 NAME "USB Audio + MIDI Scope"
		FILE ${CMAKE_CURRENT_BINARY_DIR}/psvita_usb_audio_midi_client.suprx
		     psvita_usb_audio_midi_client.suprx)

	# Vita module names are limited to 26 characters. The exports use compact
	# internal names; keep descriptive archive names for applications to link.
	add_custom_target(public_stub_aliases ALL
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_k_stub.a
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_kernel_stub.a
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_k_stub_weak.a
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_kernel_stub_weak.a
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_c_stub.a
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_client_stub.a
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_c_stub_weak.a
			${CMAKE_CURRENT_BINARY_DIR}/libpsvita_usb_audio_midi_client_stub_weak.a
		DEPENDS kernel_stubs client_stubs
		COMMENT "Creating public USB audio/MIDI stub archive names")
endif()
