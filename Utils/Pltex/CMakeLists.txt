# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# pltex builds the format-1 <planet>.tex / <planet>_lmask.tex texture archives
# for surface levels 1-8 -- the files the legacy TileManager reads.
#
# NOT BUILT ON LINUX. Its includes are
#   <io.h> <direct.h> <process.h> <windows.h>   -- all present in the shim
#   <conio.h>                                   -- ABSENT (_kbhit, _getch)
#   <ddraw.h>                                   -- present, but see below
#
# conio.h is where the compile stops today; that much is measured. It is also
# the shallow part: _kbhit/_getch are used for "press a key to continue" and a
# termios equivalent is a few lines. The unmeasured part is DirectDraw --
# Pltex writes DDS surface descriptors into the .tex archive, so how far the
# shim's ddraw.h carries it is not known, and nobody should assume "add
# conio.h and it builds" without trying it.
#
# Not attempted here, so the target is not created, following the pattern Date
# and Shipedit already use.
if(WIN32)
	add_executable(pltex
		Pltex.cpp
	)

	set_target_properties(pltex
		PROPERTIES
		FOLDER Tools
	)

	# Installation
	install(TARGETS
		pltex
		RUNTIME
		DESTINATION ${ORBITER_INSTALL_UTILS_DIR}
	)
else()
	message(STATUS "pltex: not building (needs conio.h in the Linux shim; DirectDraw usage unassessed)")
endif()