# Copyright (c) Martin Schweiger
# Licensed under the MIT License

if(ORBITER_BUILD_D3D9CLIENT)
	add_subdirectory(D3D9Client)
elseif(NOT WIN32)
	# The plugin samples under D3D9Client/samples are ordinary Orbiter
	# modules that merely live in the client's tree -- DrawOrbits, DX9ExtMFD,
	# GenericCamera and TerrainToolKit link only ${OrbiterTgt} and Orbitersdk,
	# and their dependency on D3D9Client is build ordering, not code. Gating
	# them behind ORBITER_BUILD_D3D9CLIENT keeps four modules out of the
	# Modules tab that have no DirectX in them at all.
	#
	# gcCoreAPI.h, which DrawOrbits includes, is the old name for the client's
	# gcCore.h and is aliased in Src/Orbiter/Linux.
	set(INCLUDE_TARGET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/D3D9Client)

	# Same as every other module directory: a .so is a LIBRARY artifact, and
	# Orbiter names modules bare, so the lib prefix must go.
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Modules/Plugin)
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")

	add_subdirectory(D3D9Client/samples/DrawOrbits)
	add_subdirectory(D3D9Client/samples/DX9ExtMFD)
	add_subdirectory(D3D9Client/samples/GenericCamera)
	add_subdirectory(D3D9Client/samples/TerrainToolKit)

	# Module description and category strings, as in Src/Plugin/CMakeLists.txt.
	# The .rc basename does not match the module name for these, so each pair
	# is listed explicitly.
	set(RCSTRINGS ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/rcstrings.py)
	foreach(pair
		"DrawOrbits|Orbits"
		"DX9ExtMFD|ExtMFD"
		"GenericCamera|GenericCamera"
		"TerrainToolKit|ToolKit")

		string(REPLACE "|" ";" _p ${pair})
		list(GET _p 0 mod)
		list(GET _p 1 rcname)

		if(TARGET ${mod})
			set(_dir ${CMAKE_CURRENT_SOURCE_DIR}/D3D9Client/samples/${mod})
			set(_rc  ${_dir}/${rcname}.rc)
			set(_gen ${CMAKE_CURRENT_BINARY_DIR}/${mod}_modulestrings.cpp)

			execute_process(
				COMMAND python3 ${RCSTRINGS} ${_rc} ${_gen}
				        -I ${_dir}
				        -I ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux
				        -I ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
				RESULT_VARIABLE _rcres
			)
			if(_rcres EQUAL 0)
				target_sources(${mod} PRIVATE ${_gen})
			endif()
		endif()
	endforeach()
endif()

# The Vulkan graphics client: the Linux renderer, replacing D3D9Client where
# Direct3D does not exist. ON by default on non-Windows -- see the note at
# ORBITER_BUILD_VULKANCLIENT in the root CMakeLists for why that default
# changed. The NOT WIN32 guard stays: the client is Vulkan on GLFW/X11.
if(NOT WIN32 AND ORBITER_BUILD_VULKANCLIENT)
	add_subdirectory(VulkanClient)
endif()
