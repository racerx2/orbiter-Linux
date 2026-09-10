
AUTOBUILDER
=============
A new script allows any Windows 10/11 environment to automatically get the dependencies required to compile Orbiter from scratch. It then automatically compiles and creates an `/install` folder containing everything needed to run the resulting build. Tested with Python v3.13+.

The script also allows you to build existing local repositories (as well as pull different Orbiter repositories from remote sources). This is useful if you plan to use this script in perpetuity instead of Visual Studio.

Simply download or copy the Python script located in the root of the repository, titled `compileOrbiter.py`, save it to the directory where you'd like your new repository to go, and run it.

Upon running the script, you'll be presented with a simple CLI menu that will guide you through the process:
1. If you wish to clone/pull and compile this master repository, choose option 1.
2. If you have previously forked the Orbiter repository and wish to clone/pull and compile it, choose option 2.
3. If you already have a local copy of an Orbiter repository and would like to compile it, choose option 3.

If the script fails for you, please open an issue on GitHub and provide a copy of the script's output in full.

If you prefer to manually set up dependencies and build Orbiter on your own instead, refer to the setup guide below.


PREREQUISITES
=============
**For a native Linux build, skip to [BUILDING ON LINUX](#building-on-linux).** Everything from here to that
section is the Windows toolchain. Note that the `linux-x64-*` CMake presets are *not* it: those are
`winegcc`/`wineg++` and cross-build a Windows binary to run under Wine.

To build Orbiter from its sources manually, you need a C++ compiler capable of creating Windows binaries.
The recommended compiler is bundled with **[Microsoft Visual Studio](https://visualstudio.microsoft.com/)** (tested with 2017, 2019, 2022, and 2026).

To install the Visual Studio components required for development, launch the Visual Studio Installer and select the **Desktop development with C++** workload. Then, go to the **Individual Components** tab to ensure that **C++ MFC** is selected.

If you don't already have it installed, you'll also need the [DirectX SDK (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=6812). 
- **Note:** If the installer fails with an S1023 error, this is a known bug. The files are usually still extracted correctly to `C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)`.

Having [Git for Windows](https://git-scm.com/install/windows) is recommended but not required, as you can just download the repository as a .zip file.

If you want to build the Orbiter documentation, you need LaTeX. Multiple LaTeX distributions for Windows are available, such as [MiKTeX](https://miktex.org/download).

To build the code-level documentation, you need [Doxygen](https://www.doxygen.nl/index.html).


BUILDING ORBITER
================
After cloning the Orbiter Git repository, you can either:

- Load the Orbiter download directory as a local directory into VS2017 or later.
  
- Select Project | CMake Settings for Orbiter
  to check the build settings and make sure that all required components are found.
  
- Select Project | Configure Orbiter
  This will configure the CMake build files.

- Select Build | Build All
  to build Orbiter and all its components.
  
Or, run CMake externally:

- Create a build directory separate from the Orbiter source directory.

- Run CMake, and select the correct source and build directories.

- Select Configure, and pick the Win32 platform.

- Edit options as required.

- Select Generate, then close CMake.

- This should have generated a solution file (`Orbiter.sln`) in the build directory.
  Load this into Visual Studio, and Build All.
  
  
BUILDING ON LINUX
=================
This builds a **native Linux binary**. Direct3D 9 has no Linux implementation, so `D3D9Client` is not
built; the renderer is `OVP/VulkanClient`, a Vulkan port of it, and the Win32 API the core is written
against is served by a shim in `Src/Orbiter/Linux`. Both are selected automatically — `cmake` needs no
extra flags.

Do **not** use the `linux-x64-debug` / `linux-x64-release` presets for this. Those predate the native
port: they set `CMAKE_CXX_COMPILER=wineg++` and build a Windows binary to be run under Wine.

Toolchain
---------
| Component | Notes |
|---|---|
| GCC | C++17. Verified with 15.2.0 |
| CMake | Verified with 4.2.3. `Extern/zlib` pins madler/zlib v1.2.11, whose `cmake_minimum_required(2.4.4)` CMake 4 rejects; the subproject sets `CMAKE_POLICY_VERSION_MINIMUM 3.5` for itself, so no workaround is needed on the command line |
| Ninja | The presets use it |
| Python 3 | Required, not optional. `Src/Orbiter/Linux/rc2cpp.py` converts `Orbiter.rc` into a C++ dialog-template table at build time, because ELF has no resource section |

Libraries
---------
Everything else — zlib, ImGui, VSG, Lua, Catch2 — is fetched or vendored by the build.

| Needed for | pkg-config / CMake package | Ubuntu package |
|---|---|---|
| The renderer | `Vulkan` | `libvulkan-dev` |
| Shader compilation | `glslang` | `glslang-dev`, `glslang-tools` |
| Window and input | `glfw3` | `libglfw3-dev` |
| | `x11` | `libx11-dev` |
| Sketchpad font lookup | `fontconfig` | `libfontconfig-dev` |
| XRSound | `ALSA` | `libasound2-dev` |

On Ubuntu 26.04, in one line:

```
sudo apt install build-essential cmake ninja-build python3 pkg-config \
     libvulkan-dev glslang-dev glslang-tools libglfw3-dev libx11-dev \
     libfontconfig-dev libasound2-dev
```

Build
-----
```
cmake --preset linux-native-release
cmake --build --preset linux-native-release
```

Then run it from the build tree, which is laid out exactly like an installation:

```
cd out/build/linux-native-release && ./Orbiter
```

`cmake --install out/build/linux-native-release --prefix <dir>` writes a self-contained tree under
`<dir>/Orbiter`, and `cpack` in the build directory produces `OpenOrbiter-<version>-Linux.tar.gz`
of the same content. (`CPACK_GENERATOR` is `WIX;ZIP` on Windows and `TGZ` here; WIX has no Linux
implementation.)

There is also a `linux-native-debug` preset. `ORBITER_BUILD_VULKANCLIENT=OFF` still builds, and gives
the console session with no graphics client — useful for isolating a renderer problem from a core one.

What does not get built, and why
--------------------------------
Each of these prints its own reason during `cmake` configure:

- **`Date`, `Shipedit`** — MFC. Windows-only by construction.
- **`pltex`** — stops at `conio.h`, then an unassessed amount of DirectDraw.
- **`plsplit`** — shells out to the Windows binary `dxtex.exe` for DXT compression.
- **`tileedit`** — a Qt5 GUI added as an `ExternalProject`, which runs its own `cmake` configure and so
  cannot see the Win32 header shim on the include path.
- **`Lua.Interpreter`** unit test — needs the Orbiter core factored out of the executable into a
  library; ELF has no import library to link a test against.

`texpack` **does** build, and packs and unpacks planet texture trees.


PLANETARY TEXTURES
==================
The Orbiter Git repository does not include the planetary texture files for most
celestial bodies. You need to install these separately (e.g., by installing Orbiter
2016 and optionally downloading high-res texture packs from the Orbiter website).

Set the environment variable `ORBITER_PLANET_TEXTURE_INSTALL_DIR` in your profile so that
the Orbiter build correctly configures the reference in the configuration. 

Assuming Orbiter 2016 is installed in `C:\orbiter2016`:

```
setx ORBITER_PLANET_TEXTURE_INSTALL_DIR C:\orbiter2016\Textures
```

Alternatively, you can specify the location of the texture files as a CMake variable:
```
cmake --preset windows-x64-debug -DORBITER_PLANET_TEXTURE_INSTALL_DIR=C:\orbiter2016\Textures
```
You can also set the planetary texture directory after building
Orbiter by setting the `PlanetTexDir` entry in `Orbiter.cfg`.


TROUBLESHOOTING
===============
* If you get errors during the build, in particular when building documentation (PDF from
LaTeX sources), try disabling multithreaded build support (limit to a single
thread). Some of the document converters/compilers you are using may not be
thread-safe. 

* CMake errors during build (cannot find system include files, etc.). This may happen
when using the Ninja generator. You may need to install and configure vcpkg to allow
Ninja to find the VS2019 toolset (https://github.com/microsoft/vcpkg).

* LaTeX build components not found: If using MiKTeX, make sure you install it for all
users instead of locally for a single user. CMake won't automatically find the 
single-user installation, so you would have to specify the paths to all components
manually.

* Problems launching Orbiter from the build directory: If you use the VS2019
generator, it puts binaries in configuration-dependent subdirectories (Debug/Release).
This means that Orbiter may not find plugin DLLs. You need to run Orbiter from
the install directory. The Ninja generator separates the Debug and Release builds
at the top level and doesn't have that problem.
