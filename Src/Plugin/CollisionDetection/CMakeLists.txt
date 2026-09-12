# Licensed under the MIT License

add_library(CollisionDetection SHARED
	CollisionDetection.cpp
)

target_include_directories(CollisionDetection
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
	PUBLIC ${MODULE_COMMON_DIR}
)

target_link_libraries(CollisionDetection
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

add_dependencies(CollisionDetection
	${OrbiterTgt}
	Orbitersdk
)

set_target_properties(CollisionDetection
	PROPERTIES
	FOLDER Modules
)

# Installation.
#
# The other plugins in this tree install with RUNTIME only, which is correct on
# Windows where a module is a .dll. On ELF a shared object is a LIBRARY
# artifact, so a RUNTIME-only rule installs nothing -- the same distinction the
# parent CMakeLists already handles for the build tree by mirroring
# CMAKE_LIBRARY_OUTPUT_DIRECTORY. Both destinations are given here so the
# module installs on either platform.
install(TARGETS CollisionDetection
	RUNTIME DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
	LIBRARY DESTINATION ${ORBITER_INSTALL_PLUGIN_DIR}
)

# NOTHING ELSE IS INSTALLED, and that is a constraint on the design rather than
# an omission. Breaking a piece off as a real Orbiter vessel would need a class
# config in Config/Vessels -- oapiCreateVessel resolves a class name only from
# $CONFIG/Vessels/ and $CONFIG/ (Vessel::OpenConfigFile), with no plugin-local
# path, and a miss calls TerminateOnError(). That is a file outside the module
# plus a fatal failure mode if it ever goes missing. The debris is therefore
# owned by the module and drawn out of the parent's own mesh; see ColDamage.h.
