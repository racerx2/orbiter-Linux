# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# texpack packs a planet texture layer directory tree into one deflated
# <layer>.tree archive -- the counterpart of the core's ZTreeMgr reader.
#
# IT NOW BUILDS ON LINUX. What it needed, read end to end, was:
#
#   <Shlwapi.h>   PathFileExists            -> Src/Orbiter/Linux/shlwapi.h,
#                                              one static inline over access()
#   <windows.h>   WIN32_FIND_DATA, FindFirstFile, FindNextFile, FindClose
#                 CreateFile, GetFileSizeEx, ReadFile, INVALID_HANDLE_VALUE
#                                           -> declared in the windows.h shim,
#                                              implemented in Handles.cpp
#   LARGE_INTEGER.LowPart -- the SDK declares the Low/High pair twice, once
#                 anonymously and once as the member `u`; the shim had only
#                 the second, and this file uses the first
#   path separators -- every path here was built with a literal backslash,
#                 the same defect class fixed four times over in the core.
#                 Converted with ORB_SEP, the convention those settled on.
#
# The one structural problem, and why the fix is where it is: CloseHandle used
# to live in Src/Orbiter/Linux/Platform.cpp, which no standalone tool can link
# -- it also carries module loading, the registry, the console and the file
# choosers, and leaves InitLib and ExitLib undefined for the executable to
# supply (measured with nm -u; those two are its only non-libc externals). A
# file handle has to be the same tagged object every other HANDLE in the shim
# is, so the handle object and CloseHandle were moved out to Handles.cpp,
# whose only dependencies are libc and pthreads. That single file is what this
# target links; nothing else of the shim is pulled in.
add_executable(texpack
	texpack.cpp
)

if(WIN32)
	target_link_libraries(texpack
		Shlwapi.lib
		zlib
	)
else()
	# The shim's HANDLE object, CloseHandle, and the Win32 file and
	# directory-enumeration API. See Src/Orbiter/Linux/Handles.h.
	target_sources(texpack PRIVATE
		${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/Handles.cpp
	)
	target_link_libraries(texpack
		zlib
		pthread
	)
endif()

set_target_properties(texpack
	PROPERTIES
	FOLDER Tools
)

# Installation
install(TARGETS
	texpack
	RUNTIME
	DESTINATION ${ORBITER_INSTALL_UTILS_DIR}
)
