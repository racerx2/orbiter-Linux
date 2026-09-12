// ==============================================================
// CelSphere.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================
//
// A D3DXHANDLE was one type for both techniques and parameters, which is how a
// technique handle could be passed to SetVector and produce silence. It splits
// here into TECHHANDLE and HANDLE; see VulkanEffect.h.
//
// Vulkan has one buffer type: what a buffer is comes from its usage flag and
// from which bind call it is given to, so both vertex and index buffers are
// VulkanBuffer.
// ==============================================================

#ifndef __VULKANCELSPHERE_H
#define __VULKANCELSPHERE_H

#include "CelSphereAPI.h"
#include "VulkanClient.h"
#include "VulkanUtil.h"
// ID3DXEffect arrived through <d3dx9.h>, which D3D9Client.h pulled in for
// everyone; VulkanEffectFile has its own header.
#include "VulkanEffect.h"


// ==============================================================
// Class CelestialSphere (interface)
// ==============================================================

/**
 * \brief Rendering methods for the background celestial sphere.
 *
 * Loads star and constellation information from data bases and uses them to
 * render the celestial sphere background (stars, constellations, grids,
 * labels, etc.)
 */
class VulkanCelestialSphere : public oapi::CelestialSphere {

public:
	/**
	 * \brief Create a new celestial sphere object.
	 * \param gc pointer to graphics client
	 */
	explicit VulkanCelestialSphere(oapi::VulkanClient *gc, Scene* scene);

	/**
	 * \brief Destructor
	 */
	~VulkanCelestialSphere();

	/**
	 * \brief Notification of in-simulation user option change.
	 * \param cat option category, see \ref optcat
	 * \param item option item, see \ref optitem
	 */
	void OnOptionChanged(DWORD cat, DWORD item);

	/**
	 * \brief Render the celestial sphere background.
	 * \param pDevice pointer to graphics device
	 * \param skyCol sky background colour (atmospheric tint)
	 */
	void Render(VulkanDevice *pDevice, const VECTOR3& skyCol);

	/**
	 * \brief Render stars as pixels on the celestial sphere
	 * \param fx  render effect
	 * \note if a background colour is passed into this function, the rendering
	 *   of stars darker than the background is suppressed.
	 * \note All device parameters are assumed to be set correctly on call.
	 */
	void RenderStars(VulkanEffectFile *fx);

	/**
	 * \brief Render constellation lines on the celestial sphere
	 * \param fx  render effect
	 * \note All device parameters are assumed to be set correctly on call.
	 * \note Suggestion: render additively onto background, so that the lines
	 *   are never darker than the background sky.
	 */
	void RenderConstellationLines(VulkanEffectFile *fx);

	/**
	 * \brief Render constellation boundaries on the celestial sphere
	 * \param dev render device
	 * \note All device parameters are assumed to be set correctly on call.
	 * \note Suggestion: render additively onto background, so that the lines
	 *   are never darker than the background sky.
	 */
	void RenderConstellationBoundaries(VulkanEffectFile *fx);

	/**
	 * \brief Render a great circle on the celestial sphere in a given colour.
	 * \param fx  render effect
	 * \note By default (i.e. for identity world matrix), the circle is
	 *   drawn along the plane of the ecliptic. To render a circle in any
	 *   other orientation, the world matrix must be adjusted accordingly
	 *   before the call.
	 */
	void RenderGreatCircle(VulkanEffectFile *fx);

	/**
	 * \brief Render grid lines on the celestial sphere in a given colour.
	 * \param fx  render effect
	 * \param eqline  indicates if the equator line should be drawn
	 * \note By default (i.e. for identity world matrix), this draws the
	 *   ecliptic grid. To render a grid for any other reference frame,
	 *   the world matrix must be adjusted accordingly before call.
	 * \note if eqline==false, then the latitude=0 line is not drawn.
	 *   this is useful if the line should be drawn in a different colour
	 *   with RenderGreatCircle().
	 */
	void RenderGrid(VulkanEffectFile *fx, bool eqline = true);

	void RenderGridLabels(VulkanEffectFile *FX, int az_idx, const oapi::FVECTOR4& baseCol, const MATRIX3& R, double dphi);

	/**
	 * \brief Render a background image on the celestial sphere.
	 * \param dev render device
	 */
	void RenderBkgImage(VulkanDevice *dev);

	static void VulkanTechInit(VulkanEffectFile *fx);

protected:
	/**
	 * \brief Prepare the star vertex list from the star database.
	 */
	void InitStars ();

	/**
	 * \brief Free the vertex buffers for star pixel rendering
	 */
	void ClearStars();

	/**
	 * \brief Load constellation line data from file
	 */
	void InitConstellationLines ();

	/**
	 * \brief Map constellation boundary database to vertex buffer.
	 */
	void InitConstellationBoundaries();

	/**
	 * \brief Allocate vertex list for rendering grid lines
	 *        (e.g. celestial or ecliptic)
	 */
	void AllocGrids();

	void AllocGridLabels();

	void InitCelestialTransform();

	bool LocalHorizonTransform(MATRIX3& R, FMATRIX4& T);

	/**
	 * \brief Convert a direction into viewport coordinates
	 * \param dir direction in the ecliptic frame provided as a point on the
	 *    celestial sphere.
	 * \param x x-position in the viewport window [pixel]
	 * \param y y-position in the viewport window [pixel]
	 * \return true if point is visible in the viewport, false otherwise.
	 */
	virtual bool EclDir2WindowPos(const VECTOR3& dir, int& x, int& y) const;

	int MapLineBuffer(const std::vector<VECTOR3>& lineVtx, VulkanBuffer *& buf) const;

private:
	oapi::VulkanClient *m_gc;       ///< pointer to graphics client
	CSphereManager* m_bkgImgMgr;    ///< background image manager
	Scene* m_scene;                 ///< pointer to scene object
	VulkanDevice *m_pDevice;        ///< Vulkan device

	/// \brief The chunk size the star vertex buffers are split into.
	///
	///        Was GetHardwareCaps()->MaxPrimitiveCount, a real D3D9 limit on
	///        how many primitives one DrawPrimitive call could submit (65535
	///        on the cards that made this code necessary). Vulkan has no
	///        equivalent -- vkCmdDraw's vertexCount is a uint32_t with no
	///        matching VkPhysicalDeviceLimits entry -- so the chunking is now
	///        a buffer-granularity choice, set large enough that any real star
	///        database fits in one buffer. A smaller value still renders
	///        correctly; it just allocates and draws more buffers.
	static const UINT MAX_STAR_CHUNK = 1u << 20;

	UINT maxNumVertices;            ///< number of vertices to use for one chunk at star-drawing
	DWORD m_nsVtx;                  ///< total number of vertices over all buffers
	std::vector<VulkanBuffer *> m_sVtx; ///< star vertex buffers
	std::array<int, 256> m_starCutoffIdx;  ///< list of star render cutoff indices
	DWORD m_nclVtx;                  ///< number of constellation line vertices
	VulkanBuffer *m_clVtx;           ///< constellation line vertex buffer
	DWORD m_ncbVtx;                  ///< number of constellation boundary vertices
	VulkanBuffer *m_cbVtx;           ///< vertex buffer for constellation boundaries
	VulkanBuffer *m_grdLngVtx, *m_grdLatVtx; ///< vertex buffers for grid lines
	lpSurfNative m_GridLabelTex;     ///< texture for grid labels
	std::array<VulkanBuffer *, 3> m_azGridLabelVtx;  ///< vertex buffers for azimuth grid labels
	VulkanBuffer *m_elGridLabelVtx;  ///< vertex buffer for elevation grid labels
	VulkanBuffer *m_GridLabelIdx;    ///< index list for azimuth/elevation grid labels
	MATRIX3 m_rotCelestial;          ///< rotation matrix for celestial grid rendering
	FMATRIX4 m_transformCelestial;   ///< rotation for celestial grid rendering
	double m_mjdPrecessionChecked;

	static VulkanEffectFile *s_FX;
	static TECHHANDLE s_eStar;
	static TECHHANDLE s_eLine;
	static TECHHANDLE s_eLabel;
	static HANDLE s_eColor;
	static HANDLE s_eWVP;

	/// \brief The grid-label texture parameter, `gTex0` in SceneTech.fx.
	///
	///        On Windows RenderGridLabels binds the label texture with
	///        pDev->SetTexture(0, ...) -- straight to device sampler slot 0,
	///        around the effect, which works only because SceneTech.fx's
	///        `sampler Tex0S : register(s0)` is that same slot. Vulkan has no
	///        numbered device sampler slot: a texture reaches a shader through
	///        a descriptor set BeginPass writes from the effect's own recorded
	///        bindings, so the bind goes through the effect parameter it was
	///        always shadowing.
	static HANDLE s_eTex0;
};

#endif // !__VULKANCELSPHERE_H
