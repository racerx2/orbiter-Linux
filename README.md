![Orbiter logo](./Src/Orbiter/Bitmaps/banner.png)

# Orbiter for Linux — a native Vulkan port

This is a fork of [orbitersim/orbiter](https://github.com/orbitersim/orbiter) that runs
**natively on Linux**. No Wine, no compatibility layer.

Two things had to be built for that:

- **`OVP/VulkanClient`** — the D3D9 graphics client ported to Vulkan, carried across function by
  function rather than rewritten. Its 23 `.fx`/`.hlsl` shaders became 14 GLSL modules carrying 123
  entry points, with the technique tables kept as data in `.tech` files — glslang understands the
  shader half of an `.fx` and nothing of the technique half.
- **`Src/Orbiter/Linux`** — the Win32 API the simulation core is written against, served by a shim:
  windows and dialogs, GDI, DirectInput, module and process services, and the host that owns the
  GLFW window, the Vulkan device and swapchain, and the ImGui frame.

Both are selected automatically. `cmake` needs no extra flags.

**This is a 64-bit build** — `x86-64`, and the only one. Upstream Orbiter for Windows is 32-bit, so
any add-on binary built against it will not load here; it has to be rebuilt from source.

That difference is not cosmetic. Windows x64 is LLP64, where `long` stays 32 bits; Linux is LP64,
where it is 64. Every `DWORD`, `LONG` and `uLongf` in the original had to be checked rather than
assumed — `uncompress()` taking a `uLongf*` is the clearest case, and it is handled by keeping the
Windows line under `#ifdef _WIN32` and using a correctly typed temporary here, so the original source
reads the same character for character.

**Come talk about it on Discord: https://discord.gg/fnxQYTKPFK**

---

## Status

It flies. You can launch a scenario, fly the Delta-glider from the 2D panel or the virtual cockpit,
use the MFDs, hear the engines, and land somewhere else.

**Working:** the planet and terrain renderers (both tile formats), atmospheric scattering and haze,
clouds and cloud shadows, meshes and the PBR path, engine exhaust and particle streams, the virtual
cockpit with live instruments, 2D panels, MFDs, the HUD, the Launchpad and every in-game dialog,
scenario editor, flight recording and playback, quicksave, screenshots, and audio through XRSound on
ALSA.

**Known open — this is a port in progress, not a release:**

| Issue | State |
|---|---|
| `vkQueueSubmit: VK_ERROR_DEVICE_LOST` at session start | Intermittent, measured at **1 in 58 runs**. GPU checkpoints are armed so the next occurrence names the last marker the GPU passed |
| A surface use-after-free | The virtual cockpit can hold a `SURFHANDLE` the client has released. It now reports itself in the log instead of crashing; the release itself is still being hunted |
| An occasional freeze | The picture stops while the simulation keeps running. Every path in the frame pump that skips a present now logs itself with a duration, so the next occurrence names which one |
| Joystick | The code is there; no device has ever been attached to it |
| `pltex`, `tileedit`, `plsplit` | Not ported. `conio.h` + DirectDraw, a Qt5 GUI, and a shell-out to `dxtex.exe` respectively |

If you hit something else, the log is `Orbiter.log` in the build tree — it is verbose on purpose and
most failures now name themselves in it.

## Building

Full detail, including what does not get built and why, is in **[COMPILE.md](./COMPILE.md)**.

### Prerequisites

Everything else — zlib, ImGui, VSG, Lua, Catch2 — is fetched or vendored by the build. On Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build python3 pkg-config \
     libvulkan-dev glslang-dev glslang-tools libglfw3-dev libx11-dev \
     libfontconfig-dev libasound2-dev
```

Python 3 is **required, not optional**: ELF has no resource section, so `Src/Orbiter/Linux/rc2cpp.py`
converts `Orbiter.rc` into a C++ dialog-template table at build time.

### Build

```bash
git clone --recursive https://github.com/racerx2/orbiter-Linux.git
cd orbiter-Linux
cmake --preset linux-native-release
cmake --build --preset linux-native-release
```

> **Use the `linux-native-*` presets.** The `linux-x64-*` presets predate this port — they set
> `CMAKE_CXX_COMPILER=wineg++` and cross-build a *Windows* binary to run under Wine, which is not
> what you want here.

There is also a `linux-native-debug` preset. `-DORBITER_BUILD_VULKANCLIENT=OFF` still builds and
gives you the console session with no graphics client, which is useful for telling a renderer problem
apart from a core one.

### Run

The build tree is laid out exactly like an installation, so run it from there:

```bash
cd out/build/linux-native-release
./Orbiter
```

`cmake --install out/build/linux-native-release --prefix <dir>` writes a self-contained tree under
`<dir>/Orbiter`, and `cpack` in the build directory produces `OpenOrbiter-<version>-Linux.tar.gz`.

## Planet textures

**The repository does not include the planetary surface textures.** Without them Orbiter runs, but
most bodies have no surface and the log fills with `Surface textures are missing for <planet>`.

Install an [Orbiter release](https://github.com/orbitersim/orbiter/releases) somewhere separate from
your clone, then point the build at its `Textures` directory — either set
`ORBITER_PLANET_TEXTURE_INSTALL_DIR` when you configure, or set `PlanetTexDir` in `Orbiter.cfg`
afterwards. High-resolution texture packs from the Orbiter website work the same way.

## Display server

Orbiter picks the GLFW backend itself, which means **Wayland** wherever a compositor is present.
If your compositor misbehaves, force XWayland:

```bash
ORBITER_GLFW_PLATFORM=x11 ./Orbiter
```

## Tested on

| | |
|---|---|
| Target | `x86-64` (64-bit), LP64 |
| CPU | Intel Core i9-13900K |
| GPU | NVIDIA GeForce RTX 5070 Ti, driver 615.71.09 |
| RAM | 32 GB |
| OS | Kubuntu 26.04.1 LTS, KDE Plasma on Wayland, kernel 7.0.0-31 |
| Vulkan | 1.4.351 |
| Toolchain | GCC 15.2.0, CMake 4.2.3, Ninja 1.13.2, Python 3.14.4 |

This is the only configuration the port has been run on. Reports from other hardware — especially
AMD and Intel GPUs, and non-KDE desktops — are genuinely useful; bring them to
[Discord](https://discord.gg/fnxQYTKPFK) or open an issue.

---

## About Orbiter

Orbiter is a spaceflight simulator based on Newtonian mechanics. Its playground is our solar system
with many of its major bodies – the sun, planets and moons. You take control of a spacecraft – either
historic, hypothetical, or purely science fiction. Orbiter is unlike most commercial computer games
with a space theme – there are no predefined missions to complete (except the ones you set yourself),
no aliens to destroy and no goods to trade. Instead, you will get a pretty good idea about what is
involved in real space flight – how to plan an ascent into orbit, how to rendezvous with a space
station, or how to fly to another planet. It is more difficult, but also more of a challenge. Some
people get hooked, others get bored. Finding out for yourself is easy – simply give it a try.

## Help

The in-game help is on the "Help" button of the Launchpad, or Alt-F1 while running. The user manual
is in the `Doc` subfolder if you built the documentation.

For Orbiter itself, the community is at [orbiter-forum.com](https://www.orbiter-forum.com).
For **this Linux port**, use [Discord](https://discord.gg/fnxQYTKPFK) or the issue tracker.

## License

Orbiter is published under the MIT License — see [LICENSE](./LICENSE).

`OVP/VulkanClient` is a port of `OVP/D3D9Client` and inherits its license: **LGPL**, see
[OVP/D3D9Client/LGPL.txt](./OVP/D3D9Client/LGPL.txt). The original D3D9 sources are kept alongside
the converted ones as the reference the conversion was made against.
