# Case-insensitive aliases for the script tree.  Non-Windows build only.
#
# Scenarios and vessels name scripts in whatever case their author used, while
# the shipped folders use another -- the Delta-glider asks for "dg/aap" and the
# directory is Script/DG.  Lua opens the path with fopen and never touches the
# Win32 shim, so there is no lookup point to hook; the aliases are made in the
# build tree instead.  The source tree is untouched.
#
# Symlinks, not copies: the alias must follow the real file if it is rebuilt,
# and a copy would silently go stale.

if(NOT IS_DIRECTORY "${SCRIPTDIR}")
	return()
endif()

file(GLOB entries RELATIVE "${SCRIPTDIR}" "${SCRIPTDIR}/*")

foreach(e ${entries})
	string(TOLOWER "${e}" lower)
	if(NOT "${e}" STREQUAL "${lower}")
		if(NOT EXISTS "${SCRIPTDIR}/${lower}")
			execute_process(
				COMMAND ${CMAKE_COMMAND} -E create_symlink "${e}" "${SCRIPTDIR}/${lower}"
				RESULT_VARIABLE rc)
			if(rc EQUAL 0)
				message(STATUS "script alias: ${lower} -> ${e}")
			endif()
		endif()
	endif()
endforeach()
