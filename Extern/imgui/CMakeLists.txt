project(imgui)

Include(FetchContent)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.5-docking
  PATCH_COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/imconfig.h .
  UPDATE_DISCONNECTED 1
)
FetchContent_MakeAvailable(imgui)

install(FILES ${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf fa-solid-900.ttf ${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf Lekton-Bold.ttf architext.regular.ttf
	DESTINATION ${ORBITER_INSTALL_ROOT_DIR}/Fonts
)

# THE SAME FIVE FONTS INTO THE BUILD TREE, not only into the install package.
#
# install() above runs at `cmake --install` / CPack time. Orbiter is RUN from
# the build tree, and that tree had no Fonts/ directory at all, so every one
# of these was missing at runtime. DialogManager::InitImGui says so in the
# log, once per font, and then carries on with a fallback:
#
#   ImGui font not found: 'Fonts/Roboto-Medium.ttf' -- using the built-in face.
#   ImGui font not found: 'Fonts/Cousine-Regular.ttf' -- using the built-in face.
#   ImGui font not found: 'Fonts/Lekton-Bold.ttf' -- using the built-in face.
#   ImGui font not found: 'Fonts/architext.regular.ttf' -- using the built-in face.
#
# So every ImGui dialog in the program -- Options, Custom functions, the MFD
# windows, Flight Data -- drew in ImGui's built-in face rather than Roboto
# Medium, at a different size and different metrics from the reference build.
#
# fa-solid-900.ttf is worse than a fallback, because it has none. It is merged
# into the atlas only `if (std::filesystem::exists(...))`, and the captions are
# built as ICON_FA_PUZZLE_PIECE " Orbiter: Custom functions" -- so with the
# file absent every FontAwesome glyph in the UI renders as a missing-glyph box
# instead of its icon.
#
# On Windows the installer lays these down beside the executable, which is why
# the reference build shows the right face and the icons.
#
# Configure-time copy, in the same style as the imgui.h copy below: these files
# come from a pinned tag and a checked-in directory, and never change.
file(COPY
		${imgui_SOURCE_DIR}/misc/fonts/Roboto-Medium.ttf
		${imgui_SOURCE_DIR}/misc/fonts/Cousine-Regular.ttf
		${CMAKE_CURRENT_SOURCE_DIR}/fa-solid-900.ttf
		${CMAKE_CURRENT_SOURCE_DIR}/Lekton-Bold.ttf
		${CMAKE_CURRENT_SOURCE_DIR}/architext.regular.ttf
	DESTINATION ${CMAKE_BINARY_DIR}/Fonts
)

install(FILES ${imgui_SOURCE_DIR}/imgui.h ${imgui_SOURCE_DIR}/imconfig.h
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/include
)

add_library(imgui INTERFACE)

set(IMGUI_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
file(COPY ${imgui_SOURCE_DIR}/imgui.h ${imgui_SOURCE_DIR}/imconfig.h DESTINATION ${IMGUI_INCLUDE_DIR})
target_include_directories(imgui INTERFACE ${IMGUI_INCLUDE_DIR})

FetchContent_Declare(
  implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG 3da8bd3
#  UPDATE_DISCONNECTED 1
)
FetchContent_MakeAvailable(implot)
add_library(implot INTERFACE)
install(FILES ${implot_SOURCE_DIR}/implot.h
	DESTINATION ${ORBITER_INSTALL_SDK_DIR}/include
)
set(IMPLOT_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
file(COPY ${implot_SOURCE_DIR}/implot.h DESTINATION ${IMPLOT_INCLUDE_DIR})
target_include_directories(implot INTERFACE ${IMPLOT_INCLUDE_DIR})
