# Copyright (c) Martin Schweiger
# Licensed under the MIT License

set(VESSEL_NAME Atlantis_Tank)
set(INSTALL_SUBDIR Atlantis)

set(MESH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Meshes)
set(TEXTURE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Textures)
set(CONFIG_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Config)
set(THUMBNAIL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Thumbnail)

add_library(${VESSEL_NAME} SHARED
	Atlantis_Tank.cpp
)

set_target_properties(${VESSEL_NAME}
	PROPERTIES
	FOLDER Vessels/${VESSEL_NAME}
)

target_include_directories(${VESSEL_NAME}
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../Atlantis
	PUBLIC ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
)

target_link_libraries(${VESSEL_NAME}
	${ORBITER_LIB}
	${ORBITER_SDK_LIB}
)

# Atlantis_SRB is linked as a CMake TARGET on ELF, not by full path.
#
# THIS COMMENT USED TO SAY THE OPPOSITE, and the opposite is what broke every
# first configure of a fresh build directory. The measured behaviour, from an
# isolated two-target project built both ways:
#
#   full path -> -L<dir> -lAtlantis_SRB   (searches for libAtlantis_SRB.so)
#   target    -> Modules/Atlantis_SRB.so  (records NEEDED [Atlantis_SRB.so])
#
# CMake reduces a full-path link item to -L/-l when it cannot read an SONAME
# out of the file (cmComputeLinkInformation::CheckSharedLibNoSOName), and on a
# first configure the file does not exist yet. The module is deliberately
# Atlantis_SRB.so with no prefix -- Orbiter refers to modules by bare name --
# so the reconstructed -l cannot resolve:
#     cannot find -lAtlantis_SRB: No such file or directory
# The full path only ever worked because the file already existed from an
# earlier build, so a second cmake run repaired it and no established build
# directory ever showed the fault. Same fix applied to Vsop87, Galsat, Satsat
# and LuaInterpreter.
#
# The target form also supplies the build-order edge, so add_dependencies is no
# longer needed for it (the one at the bottom of this file already names it).
# The rpath set at the top level lets the loader find it at run time.
if(WIN32)
	target_link_libraries(${VESSEL_NAME} $<TARGET_LINKER_FILE:Atlantis_SRB>)
else()
	target_link_libraries(${VESSEL_NAME} Atlantis_SRB)
endif()

add_custom_command(
	TARGET ${VESSEL_NAME} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${CONFIG_SOURCE_DIR}/ ${CMAKE_BINARY_DIR}/Config/Vessels
)

add_custom_command(
	TARGET ${VESSEL_NAME} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${MESH_SOURCE_DIR}/ ${ORBITER_BINARY_MESH_DIR}/${INSTALL_SUBDIR}
)

add_custom_command(
	TARGET ${VESSEL_NAME} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${TEXTURE_SOURCE_DIR}/ ${ORBITER_BINARY_TEXTURES_DIR}/${INSTALL_SUBDIR}
)

add_custom_command(
	TARGET ${VESSEL_NAME} PRE_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory ${THUMBNAIL_SOURCE_DIR}/ ${CMAKE_BINARY_DIR}/Images/Vessels/Default
)

add_dependencies(${VESSEL_NAME}
	${OrbiterTgt}
	Orbitersdk
	Atlantis_SRB
)

install(TARGETS ${VESSEL_NAME}
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_MODULE_DIR}
)
install(DIRECTORY ${CONFIG_SOURCE_DIR}/
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Config/Vessels
)
install(DIRECTORY ${MESH_SOURCE_DIR}/
	DESTINATION ${ORBITER_INSTALL_MESH_DIR}/${INSTALL_SUBDIR}
)
install(DIRECTORY ${TEXTURE_SOURCE_DIR}/
	DESTINATION ${ORBITER_INSTALL_TEXTURES_DIR}/${INSTALL_SUBDIR}
)
install(DIRECTORY ${THUMBNAIL_SOURCE_DIR}/
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Images/Vessels/Default
)
