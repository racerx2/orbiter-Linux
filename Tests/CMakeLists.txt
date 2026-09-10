Include(FetchContent)

# v3.0.0-preview3 (2021) sizes its alternate signal stack with a constexpr
# expression over MINSIGSTKSZ. glibc 2.34 made MINSIGSTKSZ a call to sysconf(),
# so that expression is no longer a constant and the file will not compile on
# any modern Linux. Catch2 fixed this in v3.1.0; v3.5.2 is the pinned stable
# release here. Windows is unaffected either way.
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2
)

FetchContent_MakeAvailable(Catch2)

# Utility function
function(add_test_file test_name)
	add_executable(${test_name} "${test_name}.cpp")

	set_target_properties( ${test_name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${ORBITER_BINARY_ROOT_DIR}" )

	target_include_directories(${test_name}
		PRIVATE ${ORBITER_SOURCE_SDK_INCLUDE_DIR}
		PRIVATE ${MODULE_COMMON_DIR}
		PRIVATE ${ORBITER_SOURCE_ROOT_DIR}/Src/Module/LuaScript/LuaInterpreter
	)

	target_link_libraries(${test_name}
		${ORBITER_LIB}
		${ORBITER_SDK_LIB}
		lua::lib
		${LUAINTERPRETER_LIB}
		Catch2::Catch2WithMain
	)

	add_dependencies(${test_name}
		${OrbiterTgt}
		Orbitersdk
		LuaInterpreter
	)

	add_test( 
		NAME ${test_name} 
		COMMAND $<TARGET_FILE:${test_name}>
		WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	)
	set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
endfunction()

# Register unit tests
#
# Lua.Interpreter is a standalone EXECUTABLE that links LuaInterpreter, and on
# ELF that cannot be linked at all. It is not an omission or a missing library;
# it follows from the core-linkage model described at ORBITER_LIB in the root
# CMakeLists.txt:
#
#   On Windows, building Orbiter.exe also produces orbiter.lib, and
#   ${ORBITER_LIB} resolves to it. That import library supplies VESSEL::*, the
#   oapi entry points and the Linux-shim symbols this test needs.
#
#   ELF has no import libraries. A module simply leaves the core's symbols
#   undefined and the dynamic linker binds them to the Orbiter executable at
#   dlopen time, which is why ${ORBITER_LIB} is empty here. That works for
#   anything Orbiter loads and cannot work for a separate executable: nothing
#   will ever bind those symbols, and the link fails on both halves of it --
#
#     LuaInterpreter.so: undefined reference to VESSEL::GetAnimation(...)
#     libOrbitersdk.a(Orbitersdk.cpp.o): undefined reference to
#                                        orbiter_WrapDlHandle
#
#   the second because the global -Wl,-u,orbiter_module_attach pulls the SDK's
#   module-attach object into every link, including this one, and its shim
#   references are satisfied by the executable only when it IS Orbiter.
#
# Making this build means factoring Orbiter's core out of the executable into a
# library the test could link -- a real change to the tree's shape, not a build
# setting -- so the target is not registered here. It has never built on Linux;
# it was simply never reached, because nothing had run a full `ninja`.
#
# The scenario tests below are unaffected: they run the Orbiter_server BINARY
# rather than linking against it.
if(WIN32)
	add_test_file(Lua.Interpreter)
else()
	message(STATUS "Lua.Interpreter unit test: not built (ELF has no import library for the Orbiter core)")
endif()

if (BUILD_ORBITER_SERVER)

	# Sanity check for scenario tests
	add_test(
		NAME "Scenario.SanityCheck"
		COMMAND $<TARGET_FILE:Orbiter_server> "--scenariox=${CMAKE_SOURCE_DIR}/Scenarios/Delta-glider/Smack!.scn" "--maxframes=60"
		WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
	)
	set_tests_properties(Scenario.SanityCheck PROPERTIES TIMEOUT 60)

	# Register scenario tests
	file(GLOB TestScenarios "${CMAKE_SOURCE_DIR}/Scenarios/Tests/*.scn")
	foreach(Scenario ${TestScenarios})
		get_filename_component(test_name ${Scenario} NAME_WE)
		add_test(
			NAME "Scenario.${test_name}"
			COMMAND $<TARGET_FILE:Orbiter_server> "--scenariox=${Scenario}"
			WORKING_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
		)
		set_tests_properties(Scenario.${test_name} PROPERTIES TIMEOUT 60)
	endforeach()

endif()
