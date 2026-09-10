## XRSound_dll
add_library(XRSound_dll SHARED
	DefaultSoundGroupPreSteps.cpp
	ModuleXRSoundEngine.cpp
	SoundPreSteps.cpp
	VesselXRSoundEngine.cpp
	XRSoundConfigFileParser.cpp
	XRSoundDLL.cpp
	XRSoundEngine.cpp
	XRSoundEngine30.cpp
	Resource.rc
	Utils/ConfigFileParser.cpp
	Utils/FileList.cpp
)

# ALSA output: irrKlang mixes, this writes the mixed buffer to the device,
# because irrKlang's own ALSA backend crackles on PipeWire systems. Windows
# uses DirectSound8 and needs none of this.
if(NOT WIN32)
	find_package(ALSA REQUIRED)
	target_sources(XRSound_dll PRIVATE AlsaOutput.cpp)
	target_link_libraries(XRSound_dll ALSA::ALSA)
endif()

set_target_properties(XRSound_dll PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR}
	OUTPUT_NAME XRSound
	COMPILE_DEFINITIONS "XRSOUND_DLL_BUILD;_CRT_SECURE_NO_WARNINGS;_USRDLL;XRSOUNDDLL_EXPORTS"
)

# Resource.rc is listed above as a source: rc.exe compiles it into the module on
# Windows, and Linux ignores it, so anything it declares -- including the
# version and category strings the Launchpad's Modules tab reads -- was absent.
# Registered under the OUTPUT_NAME, not the target name: an HINSTANCE resolves
# to the loaded file, which is XRSound.so. See orbiter_module_resources in the
# top-level CMakeLists and Src/Orbiter/Linux/ResourceRegistry.cpp.
orbiter_module_resources(XRSound_dll ${CMAKE_CURRENT_SOURCE_DIR}/Resource.rc)

# On ELF a shared object is a LIBRARY artifact, not a RUNTIME one, so
# RUNTIME_OUTPUT_DIRECTORY alone leaves it in the build subdirectory where
# Orbiter's scan of Modules/Plugin will not find it. The prefix is cleared for
# the same reason as every other module: Orbiter names modules bare, so it must
# be XRSound.so and not libXRSound.so.
if(NOT WIN32)
	set_target_properties(XRSound_dll PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR}
		PREFIX ""
	)
endif()

target_include_directories(XRSound_dll PUBLIC
	${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/Utils
	${IRRKLANG_DIR}/include
)

target_link_libraries(XRSound_dll
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
	${IRRKLANG_LIB}
)

add_dependencies(XRSound_dll
	${OrbiterTgt}
	Orbitersdk
)

install(TARGETS XRSound_dll
	RUNTIME DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)

## XRSound_lib
add_library(XRSound_lib STATIC
	XRSound.cpp
	XRSoundImpl.cpp
)

set_target_properties(XRSound_lib
	PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY ${ORBITER_BINARY_SDK_DIR}/$<1:XRSound>
	OUTPUT_NAME XRSound
)

target_include_directories(XRSound_lib PUBLIC
	${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

install(TARGETS XRSound_lib
	ARCHIVE DESTINATION ${ORBITER_INSTALL_SDK_DIR}/XRSound
)
