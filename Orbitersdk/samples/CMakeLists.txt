# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# On ELF a shared object is a LIBRARY artifact, and Orbiter refers to a module
# by bare name, so the lib prefix must be cleared -- otherwise AscentMFD is
# built as libAscentMFD.so and the Modules tab lists it as "libAscentMFD".
# Every other module directory carries this block; this one was missed.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

add_subdirectory(AscentMFD)
add_subdirectory(DialogTemplate)
add_subdirectory(LaunchpadParamTemplate)
add_subdirectory(MFDTemplate)
add_subdirectory(Lua)

# Module description and category strings; see Src/Plugin/CMakeLists.txt.
if(NOT WIN32 AND TARGET AscentMFD)
	set(_gen ${CMAKE_CURRENT_BINARY_DIR}/AscentMFD_modulestrings.cpp)
	execute_process(
		COMMAND python3 ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/rcstrings.py
		        ${CMAKE_CURRENT_SOURCE_DIR}/AscentMFD/AscentMFD.rc ${_gen}
		        -I ${CMAKE_CURRENT_SOURCE_DIR}/AscentMFD
		        -I ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux
		        -I ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
		RESULT_VARIABLE _rcres
	)
	if(_rcres EQUAL 0)
		target_sources(AscentMFD PRIVATE ${_gen})
	endif()
endif()

set(Exclude
	PATTERN CMakeLists.txt EXCLUDE
)

install(DIRECTORY ${ORBITER_SOURCE_SDK_DIR}/samples/AscentMFD/ DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Orbitersdk/samples/AscentMFD ${Exclude})
install(DIRECTORY ${ORBITER_SOURCE_SDK_DIR}/samples/DialogTemplate/ DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Orbitersdk/samples/DialogTemplate ${Exclude})
install(DIRECTORY ${ORBITER_SOURCE_SDK_DIR}/samples/LaunchpadParamTemplate/ DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Orbitersdk/samples/LaunchpadParamTemplate ${Exclude})
install(DIRECTORY ${ORBITER_SOURCE_SDK_DIR}/samples/MFDTemplate/ DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Orbitersdk/samples/MFDTemplate ${Exclude})
install(DIRECTORY ${ORBITER_SOURCE_SDK_DIR}/samples/Lua/ DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Orbitersdk/samples/Lua ${Exclude})

