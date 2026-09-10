// ===========================================================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2013-2026 Jarmo Nikkanen
// ===========================================================================================
//
// CONVERTED FROM OVP/D3D9Client/MaterialMgr.cpp, read end to end (513 lines).
//
// A TEXT FILE READER AND WRITER. Not one graphics call in it on either
// platform: it parses `<config>/GC/<class>.cfg`, fills VulkanMatExt records
// and hands them to VulkanMesh. The conversion is types and one path:
//
//   D3D9Mesh -> VulkanMesh, D3D9MatExt -> VulkanMatExt,
//   D3D9MATEX_* -> VULKANMATEX_*, D3D9Client -> VulkanClient,
//   D3DXVECTOR2/3/4 -> FVECTOR2/3/4.
//
//   `"%sGC\\%s.cfg"` -> `"%sGC/%s.cfg"`, in all three places it appears.
//   THIS IS THE FIFTH INSTANCE OF THE DEFECT CLASS in
//   the porting notes, and the symptom is the usual one: a
//   backslash is a legal filename character on Linux, so the open does not
//   fail as a bad path -- it asks for one file named `GC\DeltaGlider.cfg`,
//   misses, and returns the same "no custom configuration for this vessel"
//   that a vessel without one gives. Every material override would silently
//   stop being applied, and SaveConfiguration would write the file to a
//   name nothing ever reads back.
//
// TWO MORE THINGS CHANGE, AND BOTH ARE WORTH READING:
//
//   `vessel->GetClassNameA()` becomes `GetClassName()`. There is no
//   GetClassNameA in VesselAPI.h and there never was: `GetClassName` is a
//   WIN32 MACRO (windows.h defines it as GetClassNameA under !UNICODE), so
//   the preprocessor rewrote the CALL SITE before the compiler ever saw the
//   member name. The shim does not, and should not, define that macro -- so
//   the call is spelled as the SDK declares it.
//
//   `sscanf_s(cbuf, "MESH %s", meshname, 64)` becomes
//   `sscanf_s(cbuf, "MESH %63s", meshname)`. The shim maps sscanf_s to
//   sscanf, which HAS NO SUCH ARGUMENT: the 64 would be read as the next
//   conversion's target, and there is no next conversion, so it is silently
//   dropped (GCC reports it as -Wformat-extra-args). The width that
//   protected the 64-byte buffer on Windows therefore protected nothing
//   here. Moving it into the format string restores exactly that protection
//   in a form plain sscanf honours -- 63 characters plus the terminator.
//   This is the same defect as finding 8 with a benign argument instead of
//   a pointer; there it was a wild store, here it was an unbounded %s.
// ===========================================================================================


#include "MaterialMgr.h"
#include "VulkanSurface.h"
#include "OapiExtension.h"
#include "VVessel.h"



// ===========================================================================================
//
MatMgr::MatMgr(class vObject *v, class VulkanClient *_gc)
{
	gc = _gc;
	vObj = v;

	pCamera = new ENVCAMREC[1];

	ResetCamera(0);

	Shaders.push_back(SHADER("PBR-Old",SHADER_NULL));
	Shaders.push_back(SHADER("Metalness", SHADER_METALNESS));
}


// ===========================================================================================
//
MatMgr::~MatMgr()
{
	MeshConfig.clear();

	if (pCamera) {
		if (pCamera[0].pOmitAttc) delete[] pCamera[0].pOmitAttc;
		if (pCamera[0].pOmitDock) delete[] pCamera[0].pOmitDock;
		delete[] pCamera;
	}
}


// ===========================================================================================
//
ENVCAMREC * MatMgr::GetCamera(DWORD idx)
{
	return &pCamera[0];
}


// ===========================================================================================
//
DWORD MatMgr::CameraCount()
{
	return 1;
}


// ===========================================================================================
//
void MatMgr::ResetCamera(DWORD idx)
{
	pCamera[idx].near_clip = 0.25f;
	pCamera[idx].lPos = FVECTOR3(0.0f, 0.0f, 0.0f);
	pCamera[idx].nAttc = 0;
	pCamera[idx].nDock = 0;
	pCamera[idx].flags = ENVCAM_OMIT_ATTC;
	pCamera[idx].pOmitAttc = NULL;
	pCamera[idx].pOmitDock = NULL;
}
	

// ===========================================================================================
//
void MatMgr::RegisterMaterialChange(VulkanMesh *pMesh, DWORD midx, const VulkanMatExt *pM)
{
	if (!pMesh || !pM) return;
	MeshConfig[pMesh->GetName()].material[midx] = *pM;
}

// ===========================================================================================
//
void MatMgr::RegisterShaderChange(VulkanMesh *pMesh, WORD id)
{
	if (!pMesh) return;
	for (auto y : Shaders) if (y.id == id) {
		MeshConfig[pMesh->GetName()].shader = id;
		break;
	}
}


// ===========================================================================================
//
void MatMgr::ApplyConfiguration(VulkanMesh *pMesh)
{
	if (pMesh==NULL) return;

	const char *name = pMesh->GetName();

	LogAlw("Applying custom configuration to a mesh (%s)",name);

	if (MeshConfig.count(name)) 
	{
		pMesh->SetDefaultShader(MeshConfig[name].shader);

		for (auto x : MeshConfig[name].material) 
		{
			// `auto rec = x.second;` stood here and was never read -- four
			// lines below, `auto RecMat = x.second;` makes the same copy and
			// is the one used. Dropped rather than silenced: it is a copy of
			// a 124-byte struct that nothing looks at.

			if (x.first >= int(pMesh->GetMaterialCount())) {
				LogErr("MatMgr::ApplyConfiguration: Matrial Idx out of range [%s.msh]", name);
				continue;
			}

			VulkanMatExt Mat;
			auto RecMat = x.second;
			DWORD flags = RecMat.ModFlags;

			if (!pMesh->GetMaterial(&Mat, x.first)) continue;

			if (flags&VULKANMATEX_AMBIENT) Mat.Ambient = RecMat.Ambient;
			if (flags&VULKANMATEX_DIFFUSE) Mat.Diffuse = RecMat.Diffuse;
			if (flags&VULKANMATEX_EMISSIVE) Mat.Emissive = RecMat.Emissive;
			if (flags&VULKANMATEX_REFLECT) Mat.Reflect = RecMat.Reflect;
			if (flags&VULKANMATEX_SPECULAR) Mat.Specular = RecMat.Specular;
			if (flags&VULKANMATEX_FRESNEL) Mat.Fresnel = RecMat.Fresnel;
			if (flags&VULKANMATEX_EMISSION2) Mat.Emission2 = RecMat.Emission2;
			if (flags&VULKANMATEX_ROUGHNESS) Mat.Roughness = RecMat.Roughness;
			if (flags&VULKANMATEX_METALNESS) Mat.Metalness = RecMat.Metalness;

			Mat.ModFlags = flags;

			pMesh->SetMaterial(&Mat, x.first);

			LogBlu("Material %u setup applied to mesh (%s) Flags=0x%X", x.first, name, flags);
		}
	}
}

// ===========================================================================================
//
bool MatMgr::HasMesh(const char *name)
{
	if (MeshConfig.count(name)) return true;
	return false;
}

// ===========================================================================================
//
void parse_vessel_classname(char *lbl)
{
	int i = -1;
	while (lbl[++i]!=0) if (lbl[i]=='/' || lbl[i]=='\\') lbl[i]='_';
}

// ===========================================================================================
//
bool MatMgr::LoadConfiguration(bool bAppend)
{
	_TRACE;

	char cbuf[256];
	char path[256];
	char classname[256];
	char meshname[64];
	char shadername[64];

	OBJHANDLE hObj = vObj->GetObjectA();

	if (oapiGetObjectType(hObj)!=OBJTP_VESSEL) return false; 

	const char *cfgdir = OapiExtension::GetConfigDir();

	VESSEL *vessel = oapiGetVesselInterface(hObj);
	strcpy_s(classname, 256, vessel->GetClassName());
	parse_vessel_classname(classname);

	AutoFile file;

	if (file.IsInvalid()) {
		// Was "%sGC\\%s.cfg". See the file header: a backslash here does not
		// fail as a bad path on Linux, it silently names a file that does not
		// exist, and every material override stops being applied.
		sprintf_s(path, 256, "%sGC/%s.cfg", cfgdir, classname);
		fopen_s(&file.pFile, path, "r");	
	}

	if (file.IsInvalid()) return true;

	LogAlw("Reading a custom configuration file for a vessel %s (%s)", vessel->GetName(), vessel->GetClassName());
	
	DWORD n = 0;
	int mat_idx = -1;

	while (fgets2(cbuf, 256, file.pFile, 0x0A)>=0) 
	{	
		float a, b, c, d;
		
		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "MESH", 4)) {
			mat_idx = -1;
			if (sscanf_s(cbuf, "MESH %63s", meshname)!=1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			if (strncmp(meshname, "???", 3) == 0) meshname[0] = 0;
			if (HasMesh(meshname) && bAppend) meshname[0] = 0; // Mesh is loaded already skip all entries related to it.
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (meshname[0] == 0) continue;  // Do not continue without a valid mesh

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "SHADER", 6)) {
			MeshConfig[meshname].shader = SHADER_NULL;
			if (sscanf_s(cbuf, "SHADER %63s", shadername) != 1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			for (auto x : Shaders)
				if (std::string(shadername) == x.name) {
					MeshConfig[meshname].shader = x.id;
					LogOapi("NewShader [%s]=%hX", meshname, x.id);
				}
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "MATERIAL", 8)) {
			if (sscanf_s(cbuf, "MATERIAL %d", &mat_idx)!=1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (mat_idx == -1) continue;  // Do not continue without a valid material idx

		auto &Mat = MeshConfig[meshname].material[mat_idx];

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "SPECULAR", 8)) {
			if (sscanf_s(cbuf, "SPECULAR %f %f %f %f", &a, &b, &c, &d)!=4) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Specular = FVECTOR4(a, b, c, d);
			Mat.ModFlags |= VULKANMATEX_SPECULAR;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "DIFFUSE", 7)) {
			if (sscanf_s(cbuf, "DIFFUSE %f %f %f %f", &a, &b, &c, &d)!=4) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Diffuse = FVECTOR4(a, b, c, d);
			Mat.ModFlags |= VULKANMATEX_DIFFUSE;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "EMISSIVE", 8)) {
			if (sscanf_s(cbuf, "EMISSIVE %f %f %f", &a, &b, &c)!=3) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Emissive = FVECTOR3(a, b, c);
			Mat.ModFlags |= VULKANMATEX_EMISSIVE;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "EMISSION2", 9)) {
			if (sscanf_s(cbuf, "EMISSION2 %f %f %f", &a, &b, &c) != 3) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Emission2 = FVECTOR3(a, b, c);
			Mat.ModFlags |= VULKANMATEX_EMISSION2;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "AMBIENT", 7)) {
			if (sscanf_s(cbuf, "AMBIENT %f %f %f", &a, &b, &c)!=3) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Ambient = FVECTOR3(a, b, c);
			Mat.ModFlags |= VULKANMATEX_AMBIENT;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "REFLECT", 7)) {
			if (sscanf_s(cbuf, "REFLECT %f %f %f", &a, &b, &c) != 3) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Reflect = FVECTOR3(a, b, c);
			Mat.ModFlags |= VULKANMATEX_REFLECT;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "FRESNEL", 7)) {
			if (sscanf_s(cbuf, "FRESNEL %f %f %f", &a, &b, &c) != 3) LogErr("Invalid Line in (%s): %s", path, cbuf);
			if (b < 10.0f) b = 1024.0f;
			// The b and c are exchanged deliberately, and SaveConfiguration
			// writes them back the same way round. Carried over verbatim.
			Mat.Fresnel = FVECTOR3(a, c, b);
			Mat.ModFlags |= VULKANMATEX_FRESNEL;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "ROUGHNESS", 9)) {
			int cnt = sscanf_s(cbuf, "ROUGHNESS %f %f", &a, &b);
			if (cnt == 1) Mat.Roughness = FVECTOR2(a, 1.0f);
			else if (cnt == 2)  Mat.Roughness = FVECTOR2(a, b);
			else LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.ModFlags |= VULKANMATEX_ROUGHNESS;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "SMOOTHNESS", 10)) {
			int cnt = sscanf_s(cbuf, "SMOOTHNESS %f %f", &a, &b);
			if (cnt == 1) Mat.Roughness = FVECTOR2(a, 1.0f);
			else if (cnt == 2)  Mat.Roughness = FVECTOR2(a, b);
			else LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.ModFlags |= VULKANMATEX_ROUGHNESS;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "METALNESS", 9)) {
			if (sscanf_s(cbuf, "METALNESS %f", &a) != 1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			Mat.Metalness = a;
			Mat.ModFlags |= VULKANMATEX_METALNESS;
			continue;
		}
	}

	(void)n;	// declared and never used on Windows too

	return true;
}


// ===========================================================================================
//
bool MatMgr::SaveConfiguration()
{
	_TRACE;
	bool bIfStatement = false;

	char path[256];
	char classname[256];
	
	
	OBJHANDLE hObj = vObj->GetObjectA();

	if (oapiGetObjectType(hObj)!=OBJTP_VESSEL) return false; 

	VESSEL *vessel = oapiGetVesselInterface(hObj);
	const char *cfgdir = OapiExtension::GetConfigDir();

	strcpy_s(classname, 256, vessel->GetClassName());
	parse_vessel_classname(classname);

	AutoFile file;
	sprintf_s(path, 256, "%sGC/%s.cfg", cfgdir, classname);		// was "GC\\"
	
	// If the target file contains configurations those are not loaded into the editor,
	// Load them before overwriting the file
	LoadConfiguration(true);

	fopen_s(&file.pFile, path, "w");

	if (file.IsInvalid()) {
		LogErr("Failed to write a file");
		return false;
	}

	fprintf(file.pFile, "CONFIG_VERSION 3\n");

	for (auto x : MeshConfig) 
	{		
		std::string current = x.first;

		fprintf(file.pFile,"; =============================================\n");
		fprintf(file.pFile, "MESH %s\n", current.c_str());

		for (auto y : Shaders) if (y.id == x.second.shader) fprintf(file.pFile, "SHADER %s\n", y.name.c_str());
		
		for (auto rec : x.second.material) 
		{		
			DWORD flags = rec.second.ModFlags;
			VulkanMatExt *pM = &rec.second;

			if (flags==0) continue;

			fprintf(file.pFile,"; ---------------------------------------------\n");
			fprintf(file.pFile,"MATERIAL %u\n", rec.first);
					
			if (flags&VULKANMATEX_AMBIENT)  fprintf(file.pFile,"AMBIENT %f %f %f\n", pM->Ambient.x, pM->Ambient.y, pM->Ambient.z);
			if (flags&VULKANMATEX_DIFFUSE)  fprintf(file.pFile,"DIFFUSE %f %f %f %f\n", pM->Diffuse.x, pM->Diffuse.y, pM->Diffuse.z, pM->Diffuse.w);
			if (flags&VULKANMATEX_SPECULAR) fprintf(file.pFile,"SPECULAR %f %f %f %f\n", pM->Specular.x, pM->Specular.y, pM->Specular.z, pM->Specular.w);
			if (flags&VULKANMATEX_EMISSIVE) fprintf(file.pFile,"EMISSIVE %f %f %f\n", pM->Emissive.x, pM->Emissive.y, pM->Emissive.z);
			if (flags&VULKANMATEX_REFLECT)  fprintf(file.pFile,"REFLECT %f %f %f\n", pM->Reflect.x, pM->Reflect.y, pM->Reflect.z);
			if (flags&VULKANMATEX_FRESNEL)  fprintf(file.pFile,"FRESNEL %f %f %f\n", pM->Fresnel.x, pM->Fresnel.z, pM->Fresnel.y);
			if (flags&VULKANMATEX_EMISSION2) fprintf(file.pFile, "EMISSION2 %f %f %f\n", pM->Emission2.x, pM->Emission2.y, pM->Emission2.z);
			if (flags&VULKANMATEX_ROUGHNESS) fprintf(file.pFile, "SMOOTHNESS %f %f\n", pM->Roughness.x, pM->Roughness.y);
			if (flags&VULKANMATEX_METALNESS) fprintf(file.pFile, "METALNESS %f\n", pM->Metalness);		
		}
	}

	(void)bIfStatement;	// declared and never used on Windows too

	return true;
}


// ===========================================================================================
//
bool MatMgr::LoadCameraConfig()
{
	_TRACE;

	char cbuf[256];
	char path[256];
	char classname[256];

	OBJHANDLE hObj = vObj->GetObjectA();

	if (oapiGetObjectType(hObj)!=OBJTP_VESSEL) return false; 

	const char *cfgdir = OapiExtension::GetConfigDir();
	
	VESSEL *vessel = oapiGetVesselInterface(hObj);
	strcpy_s(classname, 256, vessel->GetClassName());
	parse_vessel_classname(classname);

	AutoFile file;

	sprintf_s(path, 256, "%sGC/%s_ecam.cfg", cfgdir, classname);	// was "GC\\"
	fopen_s(&file.pFile, path, "r");	
	
	if (file.IsInvalid()) return true;

	LogAlw("Reading a camera configuration file for a vessel %s (%s)", vessel->GetName(), vessel->GetClassName());
	
	DWORD iattc = 0;
	DWORD idock = 0;
	DWORD camera = 0;

	BYTE attclist[256];
	BYTE docklist[256];

	while(fgets2(cbuf, 256, file.pFile, 0x08)>=0) 
	{	
		float a, b, c;
		DWORD id;

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "END_CAMERA", 10)) {

			if (iattc) pCamera[camera].pOmitAttc = new BYTE[iattc];
			if (idock) pCamera[camera].pOmitDock = new BYTE[idock];
			
			if (iattc) memcpy(pCamera[camera].pOmitAttc, attclist, iattc); 
			if (idock) memcpy(pCamera[camera].pOmitDock, docklist, idock); 
			
			pCamera[camera].nAttc = WORD(iattc);
			pCamera[camera].nDock = WORD(idock);
			
			continue;
		}
		
		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "BEGIN_CAMERA", 12)) {
			if (sscanf_s(cbuf, "BEGIN_CAMERA %u", &camera)!=1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			camera = 0; // For now just one camera
			pCamera[camera].flags = 0; // Clear default flags
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "LPOS", 4)) {
			if (sscanf_s(cbuf, "LPOS %g %g %g", &a, &b, &c)!=3) LogErr("Invalid Line in (%s): %s", path, cbuf);
			pCamera[camera].lPos = FVECTOR3(a,b,c);
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "OMITATTC", 8)) {
			if (sscanf_s(cbuf, "OMITATTC %u", &id)!=1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			attclist[iattc++] = BYTE(id);
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "OMITDOCK", 8)) {
			if (sscanf_s(cbuf, "OMITDOCK %u", &id)!=1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			docklist[idock++] = BYTE(id);
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "CLIPDIST", 8)) {
			if (sscanf_s(cbuf, "CLIPDIST %g", &a)!=1) LogErr("Invalid Line in (%s): %s", path, cbuf);
			pCamera[camera].near_clip = a;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "OMIT_ALL_ATTC", 13)) {
			pCamera[camera].flags |= ENVCAM_OMIT_ATTC;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "DO_NOT_OMIT_FOCUS", 17)) {
			pCamera[camera].flags |= ENVCAM_FOCUS;
			continue;
		}

		// --------------------------------------------------------------------------------------------
		if (!strncmp(cbuf, "OMIT_ALL_DOCKS", 14)) {
			pCamera[camera].flags |= ENVCAM_OMIT_DOCKS;
			continue;
		}

		if (cbuf[0]!=';') LogErr("Invalid Line in (%s): %s", path, cbuf);
	}

	return true;
}
