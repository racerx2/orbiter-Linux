// ==============================================================
// OapiExtension.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012 - 2018 Peter Schneider (Kuddel)
// ==============================================================
//
// LogD3D9Modules() is gone. It walked the process's loaded modules
// (EnumProcessModules / GetModuleBaseName) and read their file versions
// (GetFileVersionInfo / VerQueryValue) so the log would say which Direct3D
// runtime was actually loaded. There are no such modules to find,
// Src/Orbiter/Linux/psapi.h supplies GetProcessMemoryInfo and nothing else,
// and ELF has no version resource to query. The question it existed to answer
// -- which graphics runtime is in use, for a bug report -- is answered by
// CVulkanFramework::Initialize out of VkPhysicalDeviceProperties, in the same
// log position the Windows client used for its adapter identifier.
//
// The default directories are POSIX now. ".\\Config\\" and friends are the
// client's own hardcoded fallbacks, used when Orbiter_NG.cfg does not
// override them, and they reach fopen unchanged: a Windows-spelled path fails
// to open, the caller reads the missing file as an empty data set, and it
// surfaces as a missing visual somewhere far away with nothing reported. The
// values read from Orbiter_NG.cfg are left exactly as read -- the core's
// Config::ConfigPath already translates separators for those.
// ==============================================================

#include <algorithm>
#include "VulkanUtil.h"
#include "OapiExtension.h"
#include "VulkanConfig.h"
#include "OrbiterAPI.h"
#include <stdlib.h>
#include <limits.h>


// ===========================================================================
// Class statics initialization

DWORD OapiExtension::elevationMode = 0;
// Orbiters default directories
std::string OapiExtension::configDir("./Config/");
std::string OapiExtension::meshDir("./Meshes/");
std::string OapiExtension::textureDir("./Textures/");
std::string OapiExtension::hightexDir("./Textures2/");
std::string OapiExtension::scenarioDir("./Scenarios/");

std::string OapiExtension::startupScenario = OapiExtension::ScanCommandLine();

bool OapiExtension::configParameterRead = OapiExtension::GetConfigParameter();

// 2010       100606
// 2010-P1    100830
// 2010-P2    110822
// 2010-P2.1  110824
bool OapiExtension::isOrbiter2010 = (oapiGetOrbiterVersion() <= 110824 && oapiGetOrbiterVersion() >= 100606);

bool OapiExtension::orbiterSound40 = false;
bool OapiExtension::tileLoadThread = true;
// Native ELF module, native Linux Orbiter. There is no WINE underneath.
bool OapiExtension::runsUnderWINE = false;
bool OapiExtension::runsSpacecraftDll = false;


// ===========================================================================
// Construction
//
OapiExtension::OapiExtension(void) {
}

// ===========================================================================
// Destruction
//
OapiExtension::~OapiExtension(void)
{
}


/*
------------------------------------------------------------------------------
	PUBLIC INTERFACE METHODS
------------------------------------------------------------------------------
*/

// ===========================================================================
// Initialization
//
void OapiExtension::GlobalInit(const VulkanConfig &Config)
{
}

// ===========================================================================
// Same functionality than 'official' GetConfigParam, but for non-provided
// config parameters
//
const void *OapiExtension::GetConfigParam (DWORD paramtype)
{
	switch (paramtype) {
		case CFGPRM_ELEVATIONINTERPOLATION	: return (void*)&elevationMode;
		case CFGPRM_TILELOADTHREAD          : return (void*)&tileLoadThread;
		default                             : return NULL;
	}
}

/*
------------------------------------------------------------------------------
	PRIVATE METHODS
------------------------------------------------------------------------------
*/

// ===========================================================================
// Tries to get the initial settings from Orbiter_NG.cfg file
//
bool OapiExtension::GetConfigParameter(void)
{
	char *pLine;
	bool orbiterSoundModuleEnabled = false;

	FILEHANDLE f = oapiOpenFile("Orbiter_NG.cfg", FILE_IN_ZEROONFAIL, ROOT);
	if (f) {
		char  string[MAX_PATH];
		DWORD flags;

		// General check for OrbiterSound module enabled
		while (oapiReadScenario_nextline(f, pLine)) {
			if (NULL != strstr(pLine, "OrbiterSound")) {
				orbiterSoundModuleEnabled = true;
				break;
			}
		}

		if (oapiReadItem_string(f, (char*)"ElevationMode", string)) {
			if (1 == sscanf_s(string, "%u", &flags)) {
				elevationMode = flags;
			}
		}

		// Get planet rendering parameters
		oapiReadItem_bool(f, (char*)"TileLoadThread", tileLoadThread);

		// Get directory config
		if (oapiReadItem_string(f, (char*)"ConfigDir", string)) {
			configDir = string;
		}
		if (oapiReadItem_string(f, (char*)"MeshDir", string)) {
			meshDir = string;
		}
		if (oapiReadItem_string(f, (char*)"TextureDir", string)) {
			textureDir = string;
		}
		if (oapiReadItem_string(f, (char*)"HightexDir", string)) {
			hightexDir = string;
		}
		if (oapiReadItem_string(f, (char*)"ScenarioDir", string)) {
			scenarioDir = string;
		}

		oapiCloseFile(f, FILE_IN_ZEROONFAIL);

		// Log directory config
		auto logPath = [](const char *name, const std::string &path) {
			char buff[PATH_MAX];
			// realpath() requires the path to exist, where GetFullPathName
			// resolved the name either way. A failure takes the same branch
			// the Windows code took for a missing directory, printing the
			// path as given rather than dropping the line entirely.
			if (realpath(path.c_str(), buff)) {
				DWORD ftyp = GetFileAttributes(buff);
				auto result = (ftyp == INVALID_FILE_ATTRIBUTES || !(ftyp & FILE_ATTRIBUTE_DIRECTORY) ? " [[DIR NOT FOUND!]]" : "");
				oapiWriteLogV("%-11s: %s%s", name, buff, result);
			}
			else {
				oapiWriteLogV("%-11s: %s [[DIR NOT FOUND!]]", name, path.c_str());
			}
		};
		oapiWriteLog((char*)"---------------------------------------------------------------");
		logPath("BaseDir"    , "./");
		logPath("ConfigDir"  , configDir);
		logPath("MeshDir"    , meshDir);
		logPath("TextureDir" , textureDir);
		logPath("HightexDir" , hightexDir);
		logPath("ScenarioDir", scenarioDir);
		oapiWriteLog((char*)"---------------------------------------------------------------");
		// LogD3D9Modules() stood here; CVulkanFramework::Initialize reports
		// the graphics runtime instead.
		oapiWriteLog((char*)"---------------------------------------------------------------");

	}

	// Check for the OrbiterSound version
	if (orbiterSoundModuleEnabled)  {
		orbiterSound40 = false;

		f = oapiOpenFile("Sound/version.txt", FILE_IN_ZEROONFAIL, ROOT);
		while (f && oapiReadScenario_nextline(f, pLine)) {
			if (NULL != strstr(pLine, "OrbiterSound 4.0 (3D)")) {
				orbiterSound40 = true;
				break;
			}
		}
		oapiCloseFile(f, FILE_IN_ZEROONFAIL);
	}

	// The wine_get_version probe of ntdll.dll stood here; runsUnderWINE is
	// false by construction.

	return true;
}

// ===========================================================================
// Try to read a startup scenario given by "-s" command line parameter
//
std::string OapiExtension::ScanCommandLine (void)
{
	// Was GetCommandLine(). Linux keeps no single command-line string;
	// /proc/self/cmdline holds the arguments NUL-separated and is not
	// seekable, so it is read in a loop and joined with spaces into the one
	// string the search below expects.
	std::string commandLine;
	if (FILE *f = fopen("/proc/self/cmdline", "rb")) {
		char buf[512];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			for (size_t i = 0; i < n; i++) commandLine += buf[i] ? buf[i] : ' ';
		}
		fclose(f);
	}

	// Is there a "-s <scenario_name>" option at all?
	size_t pos = rfind_ci(commandLine, "-s");
	if (pos != std::string::npos)
	{
		std::string scenarioName = commandLine.substr(pos+2, std::string::npos);
		trim(scenarioName);

		// Remove (optional) quotes
		std::replace(scenarioName.begin(), scenarioName.end(), '"', ' ');

		// Build the path (like "./Scenarios/(Current State).scn"
		//startupScenario = GetScenarioDir() + trim(scenarioName) + ".scn";
		return GetScenarioDir() + trim(scenarioName) + ".scn";
	}
	return "";
}

// --- eof ---
