# The target directory for plugin DLLs
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})


# On ELF a shared object is a LIBRARY artifact, not a RUNTIME one, so
# CMAKE_RUNTIME_OUTPUT_DIRECTORY alone does not place it -- the module would be
# left in its build subdirectory and Orbiter's scans of Modules/ would find
# nothing. Mirroring the directory here puts .so files where .dll files go on
# Windows.
#
# The prefix is cleared for the same reason: Orbiter refers to a module by bare
# name ("Sun", "ScnEditor"), so it must be Sun.so rather than libSun.so.
if(NOT WIN32)
	set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_PLUGIN_DIR})
	set(CMAKE_SHARED_LIBRARY_PREFIX "")
	set(CMAKE_SHARED_MODULE_PREFIX "")
endif()

# Common routines for inclusion by plugins
set(MODULE_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Common)

# The subdirectories with individual plugin implementations
add_subdirectory(CollisionDetection)
add_subdirectory(ExtMFD)
add_subdirectory(FlightData)
add_subdirectory(Framerate)
add_subdirectory(LaunchpadExtensions)
add_subdirectory(LuaConsole)
add_subdirectory(LuaMFD)
add_subdirectory(Meshdebug)
add_subdirectory(Rcontrol)
add_subdirectory(ScnEditor)
add_subdirectory(ScriptMFD)
add_subdirectory(TrackIR)
add_subdirectory(TransX)
add_subdirectory(Notes)

# ==========================================================================
# Module description and category strings
#
# ModuleTab::RefreshLists identifies a plugin by loading it as a data file and
# reading two strings out of it:
#
#     LoadString(hMod, 1000, ...)  -> description shown in the info pane
#     LoadString(hMod, 1001, ...)  -> category the tree groups the module under
#
# On Windows those come from the STRINGTABLE in each plugin's .rc, compiled
# into the DLL's resource section by rc.exe. ELF has no resource section, so
# the Linux LoadString resolves the two ids to exported symbols instead, and
# rcstrings.py generates them from the same .rc the Windows build uses.
#
# Without this every module falls to ModuleTab's default of "Miscellaneous"
# with no description -- which is also the correct result for the plugins in
# this tree that genuinely have no .rc (ExtMFD, FlightData, Framerate, Notes,
# Rcontrol), so those are simply not listed here.
# ==========================================================================
if(NOT WIN32)
	set(RCSTRINGS ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux/rcstrings.py)

	foreach(mod Meshdebug ScnEditor TrackIR TransX LuaMFD ScriptMFD)
		if(TARGET ${mod})
			set(_rc  ${CMAKE_CURRENT_SOURCE_DIR}/${mod}/${mod}.rc)
			set(_gen ${CMAKE_CURRENT_BINARY_DIR}/${mod}/${mod}_modulestrings.cpp)

			# Generated at CONFIGURE time rather than build time.
			#
			# A build-time custom_command would leave the file absent when
			# target_sources runs, and the GENERATED property that would
			# excuse that is directory-scoped -- it cannot be set from here on
			# a target created in a subdirectory. Since a .rc changes only
			# when the source tree does, running the converter now and adding
			# a real file avoids the whole problem.
			file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${mod})
			execute_process(
				COMMAND python3 ${RCSTRINGS} ${_rc} ${_gen}
				        -I ${CMAKE_CURRENT_SOURCE_DIR}/${mod}
				        -I ${CMAKE_SOURCE_DIR}/Src/Orbiter/Linux
				        -I ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
				RESULT_VARIABLE _rcres
			)
			if(_rcres EQUAL 0)
				target_sources(${mod} PRIVATE ${_gen})
			else()
				message(WARNING "rcstrings failed for ${mod}; it will show "
				                "under Miscellaneous with no description")
			endif()
		endif()
	endforeach()

	# ----------------------------------------------------------------------
	# Plugins with no .rc in this source tree.
	#
	# ExtMFD, FlightData, Framerate, Notes, Rcontrol and LuaConsole ship no
	# resource script here, so there is no STRINGTABLE for rcstrings.py to
	# convert and nothing for LoadString to find -- every one of them fell to
	# ModuleTab's "Miscellaneous" default. The released Windows build was made
	# from a tree that does have those files, which is why it groups them
	# correctly and this one did not.
	#
	# The strings are supplied directly instead. The categories are the ones
	# the Windows Launchpad shows for the same plugins, so the two agree; the
	# descriptions are written to match what each module actually does. This
	# is data the tree is missing, not a behaviour change: the generated file
	# is byte-identical in form to what rcstrings.py emits from a .rc.
	#
	# Notes has no Windows counterpart to compare against -- it is absent from
	# the released module set -- so it is grouped with the other in-sim
	# utility dialogs, which is where it belongs by function.
	# ----------------------------------------------------------------------
	set(_nocat_CollisionDetection "Simulation enhancements|Vessel-to-vessel collision detection. Contact is resolved against the actual mesh triangles of each vessel; no vessel, mesh or add-on file is modified.")
	set(_nocat_ExtMFD     "Tools and dialogs|External MFD window. Opens an MFD in a separate resizable window outside the main Orbiter window.")
	set(_nocat_FlightData "Tools and dialogs|Flight data monitor. Records and graphs vessel flight parameters during a session.")
	set(_nocat_Framerate  "Tools and dialogs|Frame rate monitor. Displays the current frame rate and time step as a graph.")
	set(_nocat_Rcontrol   "Tools and dialogs|Remote vessel control. Operates the main, retro and hover engines and attitude of any vessel.")
	set(_nocat_Notes      "Tools and dialogs|Annotation notepad. Provides a free-text note window over the simulation view.")
	set(_nocat_LuaConsole "Script tools and drivers|Interactive Lua console. Provides a command line for the Orbiter script interpreter.")

	foreach(mod CollisionDetection ExtMFD FlightData Framerate Rcontrol Notes LuaConsole)
		if(TARGET ${mod})
			string(REPLACE "|" ";" _parts "${_nocat_${mod}}")
			list(GET _parts 0 _cat)
			list(GET _parts 1 _desc)

			set(_gen ${CMAKE_CURRENT_BINARY_DIR}/${mod}/${mod}_modulestrings.cpp)
			file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${mod})
			file(WRITE ${_gen}
"// Generated for ${mod}, which has no .rc in this source tree.\n"
"//\n"
"// ModuleTab::RefreshLists reads these with LoadString(hMod, 1000) and\n"
"// LoadString(hMod, 1001). On Windows they would come from a STRINGTABLE;\n"
"// here they are exported symbols, which is what the Linux LoadString\n"
"// resolves by name. See Src/Plugin/CMakeLists.txt.\n"
"\n"
"extern \"C\" const char orbiterModuleDescription[] = \"${_desc}\";\n"
"extern \"C\" const char orbiterModuleCategory[] = \"${_cat}\";\n")
			target_sources(${mod} PRIVATE ${_gen})
		endif()
	endforeach()
endif()

# ==========================================================================
# SDK Sample Installation
# ==========================================================================

set(IgnoreFiles
	PATTERN CMakeLists.txt EXCLUDE
	PATTERN Textures EXCLUDE
	PATTERN Textures2 EXCLUDE
	PATTERN Config EXCLUDE
	PATTERN Meshes EXCLUDE
	PATTERN Thumbnail EXCLUDE
)

install(DIRECTORY 
	LuaConsole
	LuaMFD
	ScriptMFD
	TransX
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/samples ${IgnoreFiles}
)
