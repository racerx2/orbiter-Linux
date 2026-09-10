# Copyright (c) Martin Schweiger
# Licensed under the MIT License

include(ExternalProject)

add_subdirectory(Date)
add_subdirectory(meshc)
add_subdirectory(Pltex)
add_subdirectory(Shipedit)
add_subdirectory(texpack)
#add_subdirectory(tileedit/qt)

# plsplit and tileedit are the two utilities added here as ExternalProjects,
# and NEITHER IS BUILT ON LINUX.
#
# Note what being an ExternalProject means for them: each runs its OWN cmake
# configure, so it does NOT inherit the root CMakeLists' line
#     include_directories(BEFORE ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux)
# and therefore does not see the Win32 header shim at all. That is why plsplit
# stops at <io.h> and tileedit at <windows.h>, headers the in-tree utilities
# resolve without trouble. Wiring the shim in would be the first step of a
# port, not the whole of one.
#
# plsplit -- read end to end. It CANNOT work on Linux as written, and the
# blocker is not headers:
#
#   * It does not compress anything itself. WriteDDS writes a temporary BMP and
#     then runs the DirectX Texture Tool:
#         _spawnl(_P_WAIT, ".\\dxtex.exe", ... "DXT1", ddsname, NULL)
#     dxtex.exe is a Windows binary shipped beside it (installed by the block
#     below) and there is no Linux build of it. A source port would still have
#     no DXT compressor to call, so the tool would compile and then fail on
#     every tile.
#   * ReadPNG is COM + the Windows Imaging Component (IWICImagingFactory,
#     CoCreateInstance), MakePath is SHCreateDirectoryEx from shell32, and
#     FatalError is _kbhit/_getch from conio.h.
#
# Making it work means replacing its DDS writer with a real compressor, which
# is writing a new tool rather than porting this one.
#
# tileedit -- a Qt5 GUI. Not read; the compiler's account is that fourteen
# translation units stop at <windows.h> (cmap.h, tile.h, ddsread.cpp,
# elv_io.cpp, imagetools.h, ZTreeMgr.h, tilecanvas.h and the moc output).
# Qt5 itself IS available on this machine -- the configure got as far as
# compiling sources -- so the header shim, not Qt, is what it stops on. Depth
# beyond that is unassessed.
#
# Both are guarded the way Date and Shipedit already are: no target, and a
# message that says why.
if(WIN32)
	# We do this as an external project to invoke x64 toolchain
	ExternalProject_Add(plsplit
		PREFIX ${CMAKE_CURRENT_BINARY_DIR}/plsplit
		BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/plsplit
		SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/plsplit
		INSTALL_DIR ${ORBITER_INSTALL_UTILS_DIR}
		CMAKE_ARGS -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX} -DINSTALLDIR=${ORBITER_INSTALL_UTILS_DIR}
		BUILD_COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}/plsplit --config Release
		INSTALL_COMMAND ""
	)
	# Install project output from here instead from inside the external project
	# to make cpack aware of it
	if (EXISTS ${CMAKE_CURRENT_BINARY_DIR}/plsplit/plsplit.exe)
		install(PROGRAMS
			${CMAKE_CURRENT_BINARY_DIR}/plsplit/plsplit.exe
			plsplit/DxTex.exe
			DESTINATION ${ORBITER_INSTALL_UTILS_DIR}
		)
	endif()

	# tileedit is another 64-bit project. This also requires
	# Qt5 so will only be built if that is available
	ExternalProject_Add(tileedit_proj
		PREFIX ${CMAKE_CURRENT_BINARY_DIR}/tileedit/qt
		BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/tileedit/qt
		SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tileedit/qt
		INSTALL_DIR ${ORBITER_INSTALL_UTILS_DIR}
		CMAKE_ARGS -DCMAKE_CFG_INTDIR=Release -DQTDIR:PATH=${Qt5_x64_DIR} -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_INSTALL_PREFIX} -DINSTALLDIR=${ORBITER_INSTALL_UTILS_DIR}
		BUILD_COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR}/tileedit/qt --config Release
		INSTALL_COMMAND ""
	)
	# Install project output from here instead from inside the external project
	# to make cpack aware of it
	if (EXISTS ${CMAKE_CURRENT_BINARY_DIR}/tileedit/qt/src/tmp)
		install(DIRECTORY
			${CMAKE_CURRENT_BINARY_DIR}/tileedit/qt/src/tmp/
			DESTINATION ${ORBITER_INSTALL_UTILS_DIR}
		)
	endif()
else()
	message(STATUS "plsplit: not building (drives the Windows-only dxtex.exe for DXT compression)")
	message(STATUS "tileedit: not building (Qt5 sources need the Win32 header shim an ExternalProject cannot see)")
endif()
