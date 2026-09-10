# VulkanSceneGraph — the renderer the graphics client is built on.
#
# Orbiter's own graphics client is Direct3D 9, which does not exist on Linux
# and cannot be ported: 47k lines woven through with D3D types, and roughly
# 1,700 references to D3DX9, a Microsoft utility library nobody has
# reimplemented. So the Linux client is written against VSG instead.
#
# Why VSG rather than bare Vulkan or another engine:
#
#   - Double-precision Camera and Transform, "for large database coordinate
#     systems such as whole earth/GIS rendering whilst minimizing precision
#     issues". That is a structural requirement for Orbiter, not a luxury --
#     its global frame runs to ~1e11 m, far past what single-precision floats
#     survive.
#   - Runtime GLSL to SPIR-V through built-in glslang: the direct replacement
#     for the D3DXCreateEffectFromFile shader system that made D3D9Client
#     unportable.
#   - A scene graph, which is the shape Orbiter's world already has -- planets,
#     vessels and meshes in a hierarchy.
#   - Native Linux windowing, MIT licence, plain CMake, and the same lead
#     author as OpenSceneGraph. That last point matters: the one existing
#     non-DirectX Orbiter client, OrbiterSkyboltClient, is built on OSG and is
#     the reference for the Orbiter-side work.
#
# Pinned to a tag rather than a branch so a build is reproducible, matching how
# stb, lua and zlib are handled in this tree.
#
# VSG_SOURCE_DIR overrides the fetch with a local checkout, for working offline
# or against a modified copy.

include(FetchContent)

set(VSG_SOURCE_DIR "" CACHE PATH
    "Local VulkanSceneGraph checkout to build against instead of fetching")

if(VSG_SOURCE_DIR)
	message(STATUS "VulkanSceneGraph: using local source at ${VSG_SOURCE_DIR}")
	FetchContent_Declare(vsg SOURCE_DIR ${VSG_SOURCE_DIR})
else()
	FetchContent_Declare(
	  vsg
	  GIT_REPOSITORY https://github.com/vsg-dev/VulkanSceneGraph.git
	  GIT_TAG        v1.1.16
	)
endif()

# Static: the client is a plugin loaded with dlopen, and a static VSG keeps its
# symbols inside that plugin rather than leaking them into the process where
# they could collide. The same reasoning as every other module here being built
# without a lib prefix and loaded by bare name.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(vsg)
