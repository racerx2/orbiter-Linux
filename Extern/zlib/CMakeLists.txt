project(zlib)

Include(FetchContent)

set(SKIP_INSTALL_ALL ON)

# zlib v1.2.11 declares cmake_minimum_required(VERSION 2.4.4), which CMake 4
# refuses outright. The tag is pinned below, so rather than bump zlib we grant
# the old policy behaviour to this subproject only. FetchContent propagates
# this into the fetched CMakeLists; it is deliberately not set project-wide.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(
  zlib
  GIT_REPOSITORY https://github.com/madler/zlib.git
  GIT_TAG v1.2.11
)
FetchContent_MakeAvailable(zlib)
target_include_directories(zlib INTERFACE ${zlib_SOURCE_DIR})
target_include_directories(zlib INTERFACE ${zlib_BINARY_DIR})

set_target_properties(zlib PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${ORBITER_BINARY_ROOT_DIR}
)
install(TARGETS zlib RUNTIME DESTINATION ${ORBITER_INSTALL_ROOT_DIR})