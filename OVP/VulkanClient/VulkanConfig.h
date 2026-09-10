// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Config.h, read end to end (118 lines),
// against D3D9Config.cpp read end to end (310 lines).
//
// NO DIRECT3D IN EITHER FILE -- it is config-file I/O through the Orbiter
// SDK, which is already cross-platform. Beyond the D3D9Config -> VulkanConfig
// rename, the only change is that FOUR SETTINGS ARE GONE, because what they
// controlled does not exist on this platform. Each was checked for its call
// sites first, and in every case those sites are code already removed by the
// conversion of the file that held them:
//
//   Enable9On12 ("EnableDX12Wrapper")   selected pDirect3DCreate9On12 over
//       Direct3DCreate9 (D3D9Client.cpp:3222, :3235, :5938). Running D3D9 on
//       top of DX12 has no meaning where there is no D3D9 and no DX12.
//
//   DisableDriverManagement ("DisableDrvMgm")   set
//       D3DCREATE_DISABLE_DRIVER_MANAGEMENT on CreateDevice
//       (D3D9Frame.cpp:8004). Vulkan has no driver-managed resource pool to
//       disable -- the application owns every allocation -- and there is no
//       CreateDevice call left to pass a flag to.
//
//   NVPerfHUD   selected D3DDEVTYPE_REF on adapter 1 (D3D9Frame.cpp:8005,
//       :8466) for NVIDIA's PerfHUD, a Direct3D-only tool that was already
//       discontinued on Windows.
//
//   PresentLocation   chose whether PresentScene() ran at clbkDisplayFrame or
//       at clbkRenderScene (D3D9Client.cpp:4064, :4153). The client does not
//       present at all here: UIHost.cpp's renderFrame owns the only command
//       buffer that reaches the swapchain and presentFrame the only
//       vkQueuePresentKHR, so there is no location to choose between.
//
// A setting is dropped only when it is inexpressible, never merely unused --
// SceneAntialias, Anisotrophy and DisableVisualHelperReadout all still mean
// something here and all stay.
//
// THE CONFIG FILE IS NOW VulkanClient.cfg. It has to be: this is a different
// client with a different set of keys, and writing the four dropped keys back
// into a user's D3D9Client.cfg -- or silently discarding them from it -- would
// corrupt the settings of a D3D9Client they may still run under WINE.
// ==============================================================

#ifndef __VULKANCONFIG_H
#define __VULKANCONFIG_H
#include <map>
#include <string>

extern class VulkanConfig *Config;

/**
 * \brief Configuration Manager
 *
 * This class provides access to config-parameters that were read from the
 * config file.
 */
class VulkanConfig {
public:

	VulkanConfig ();
	~VulkanConfig ();

	void Reset();
	bool ReadParams();
	void WriteParams();
	int  MaxLights();

	/// Bit flags for "LabelDisplayFlags" parameter.
	static const int LABEL_DISPLAY_RECORD = 0x1; ///< Display label "Record" on active recording session
	static const int LABEL_DISPLAY_REPLAY = 0x2; ///< Display label "Replay" on active playback session

	int PlanetPreloadMode;			///< Planet preload mode setting (0=load on demand, 1=preload)
	int PlanetLoadFrequency;		///< Load frequency for on-demand textures \[Hz\] (1...1000)
	int Anisotrophy;				///< Anisotropic filtering setting \[factor\] (1...16)
	int SceneAntialias;				///< Antialiasing setting \[factor\] (0...)
	int DisableVisualHelperReadout;	///< Disable the hooking of the visual helper windows, to allow access to config parameter that Orbiter core doesn't provide (0=normal mode, 1=disable any hooking)
	int NearClipPlane;				///< Near clip plane mode (0,1)
	int DebugBreak;					///< Enable Debug Break
	int PreLBaseVis;				///< Preload base visuals (0=load on demand, 1=preload)
	int DebugFontSize;				///< Debug font height \[pixel\] (default=18px)
	int UseNormalMap;				///< Enable normal mapping (0,1)
	int SketchpadFont;				///< Sketchpad Font (0=Crisp, 1=Default, 2=Cleartype, 3=Proof Quality)
	int RwyLightAnimate;			///< Runway light animate (0,1)
	double RwyLightAngle;			///< Runway light angle \[deg\] (10...180)
	double RwyBrightness;			///< Runway light brightness (0.3...3.0)
	double VCNearPlane;				///< Virtual cockpit near clip-plane distance \[m\] (-1.0...1.0, default=0.1)
	double Convergence;				///< StereoScopic 3D convergence distance \[m\] (0.05...1.0, default=0.2)
	double Separation;				///< StereoScopic 3D depth of field separation \[m\] (10.0...100.0, default=65)
	double SunAngle;				///< Sun-angle above horizon when night-lights set it \[deg\] (0.1...20.0, default=10)
	double BumpAmp;					///< Bump map amplification setting (0.1...10.0, default=1)
	float PlanetGlow;				///< Intensity of planet glow effect (0.01...2.0, default=0.7)
	double FrameRate;				///< Frame-rate limiter
	double OrbitalShadowMult;		///< Multiplier for cloud shadows for Orbital flight
	int EnableLimiter;				///< Enable frame-rate limiter
	int DebugLvl;					///< Level of debug output 'verbosity'. Higher values create more detailed output (0...4, default=1)
	int LabelDisplayFlags;			///< Label display option flags. For example the "Record" and "Replay" labels (0=all disabled, 1=show record label, 2=show replay label, 3=show both \[default\])
	int LightConfig;				///< Light emitter configuration
	int EnvMapSize;					///< Environment map size (64...512)
	int EnvMapMode;					///< Environment map mode (0=disabled, 1=planet only, 2=full scene)
	int EnvMapFaces;				///< Number of environment map faces render per frame (1..6, default=1)
	int EnableGlass;				///< Enable improved glass shading (Fresnel reflection)
	int EnableMeshDbg;				///< Enable mesh debugger
	int ShadowMapMode;				///< Shadow Mapping Mode
	int ShadowFilter;				///< Shadow Mapping Filter
	int ShadowMapSize;				///< Shadow Map size
	int TerrainShadowing;			///< Terrain Shadowing mode (0=None, 1=Stencil, 2=Projected, default=1)
	int CustomCamMode;				///< Custom Camera Mode
	int TileMipmaps;				///< Enable surface tile mipmaps
	int ShaderDebug;				///< Shader Debug Logging enable flag (0=disabled, 1=enabled)
	double LODBias;					///< 3D Terrain resolution bias
	int MeshRes;					///< Tile patch mesh resolution
	int MaxTiles;
	int TileDebug;					///< Enable tile debugger
	int TextureMips;				///< Texture mipmap auto-gen policy
	int PostProcess;				///< Enable post processing effects
	int MicroMode;					///< Surface micro textures enable flag (0=disabled, 1=enabled)
	int MicroFilter;				///< Surface micro texture filter mode (0=Point, 1=Linear ,2=Anisotropic 2x ,3=Anisotropic 4x, 4=Anisotropic 8x, Anisotropic 16x)
	int BlendMode;					///< Surface micro texture light blend mode (0=Soft, 1=Normal, 2=Hard)
	int ShaderCacheUse;				///< Shader cache usage flag (0=disabled, 1=enabled)
	int MicroBias;					///< Mipmap LOD Bias for surface micro textures
	int CloudMicro;					///< Cloud layer micro textures
	int PlanetTileLoadFlags;		///< Planet Tile Load Flags (0x1=load tiles from directory tree, 0x2=load tiles from compressed archive, 0x3=both \[try directory tree first, then archive\])
	int GDIOverlay;					///< GDI Overlay
	int gcGUIMode;					///< gcGUI Operation Mode
	int bAbsAnims;					///< Absolute animations
	int bCloudNormals;				///< Felix24's Cloud normals implementation test
	int bFlats;						///< Face's terrain flattening
	int bGlares;
	int bLocalGlares;
	int bIrradiance;
	int bAtmoQuality;
	int NoPlanetAA;					///< Disable planet surface anti-aliasing to prevent white pixels at horizon 
	char *DebugFont;				///< Font face for debug lines (default="Fixed")
	char *SolCfg;					///< Solar system to use (default="Sol")
	float GFXIntensity;			///< Post Processing | Light glow intensity (0.0...1.0, default=0.5)
	float GFXDistance;				///< Post Processing | Light glow distance (0.0...1.0, default=0.8)
	float GFXThreshold;			///< Post Processing | Glow threshold (0.5...2.0, default=1.1)
	float GFXGamma;				///< Post Processing | Gamma (0.3...2.5, default=1.0)
	float GFXSunIntensity;			///< Light Configuration| Sunlight Intensity (0.5...2.5, default=1.2)
	float GFXLocalMax;				///< Light Configuration| Local Lights Max (0.001...1.0, default=0.5)
	float GFXGlare;				///< Sun glare intensity| (0.001...1.0, default=0.5)

	std::map<std::string, std::string> AtmoCfg;

private:

};

#endif // !__VULKANCONFIG_H
