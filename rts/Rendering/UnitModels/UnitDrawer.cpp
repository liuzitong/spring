/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "StdAfx.h"
#include "mmgr.h"

#include "UnitDrawer.h"

#include "Game/Game.h"
#include "Game/Camera.h"
#include "Game/GameHelper.h"
#include "Game/GameSetup.h"
#include "Game/SelectedUnits.h"
#include "Game/CameraHandler.h"
#include "Game/UI/MiniMap.h"
#include "Lua/LuaMaterial.h"
#include "Lua/LuaUnitMaterial.h"
#include "Lua/LuaRules.h"
#include "Map/BaseGroundDrawer.h"
#include "Map/Ground.h"
#include "Map/MapInfo.h"
#include "Map/ReadMap.h"

#include "Rendering/Env/BaseWater.h"
#include "Rendering/Env/CubeMapHandler.h"
#include "Rendering/FarTextureHandler.h"
#include "Rendering/glFont.h"
#include "Rendering/GL/glExtra.h"
#include "Rendering/GL/VertexArray.h"
#include "Rendering/GroundDecalHandler.h"
#include "Rendering/IconHandler.h"
#include "Rendering/ShadowHandler.h"
#include "Rendering/Shaders/ShaderHandler.hpp"
#include "Rendering/Shaders/Shader.hpp"
#include "Rendering/Textures/Bitmap.h"
#include "Rendering/Textures/3DOTextureHandler.h"
#include "Rendering/Textures/S3OTextureHandler.h"
#include "Rendering/UnitModels/WorldObjectModelRenderer.h"

#include "Sim/Units/Groups/Group.h"
#include "Sim/Features/Feature.h"
#include "Sim/Misc/CollisionVolume.h"
#include "Sim/Misc/LosHandler.h"
#include "Sim/Misc/RadarHandler.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Units/CommandAI/BuilderCAI.h"
#include "Sim/Units/UnitDef.h"
#include "Sim/Units/UnitDefHandler.h"
#include "Sim/Units/Unit.h"
#include "Sim/Units/UnitHandler.h"
#include "Sim/Units/UnitTypes/Building.h"
#include "Sim/Units/UnitTypes/TransportUnit.h"
#include "Sim/Weapons/Weapon.h"

#include "System/myMath.h"
#include "System/LogOutput.h"
#include "System/ConfigHandler.h"
#include "System/EventHandler.h"
#include "System/GlobalUnsynced.h"

#ifdef USE_GML
#include "lib/gml/gmlsrv.h"
extern gmlClientServer<void, int, CUnit*> *gmlProcessor;
#endif

#define UNIT_SHADOW_ALPHA_MASKING

CUnitDrawer* unitDrawer;

static bool luaDrawing = false; // FIXME

static float GetLODFloat(const string& name, float def)
{
	// NOTE: the inverse of the value is used
	char buf[64];
	SNPRINTF(buf, sizeof(buf), "%.3f", def);
	const string valueStr = configHandler->GetString(name, buf);
	char* end;
	float value = (float)strtod(valueStr.c_str(), &end);
	if ((end == valueStr.c_str()) || (value <= 0.0f)) {
		return (1.0f / def);
	}
	return (1.0f / value);
}



CUnitDrawer::CUnitDrawer(): CEventClient("[CUnitDrawer]", 271828, false)
{
	eventHandler.AddClient(this);

	if (texturehandler3DO == 0) { texturehandler3DO = new C3DOTextureHandler; }
	if (texturehandlerS3O == 0) { texturehandlerS3O = new CS3OTextureHandler; }

	SetUnitDrawDist((float)configHandler->Get("UnitLodDist",  200));
	SetUnitIconDist((float)configHandler->Get("UnitIconDist", 200));

	LODScale           = GetLODFloat("LODScale",           1.0f);
	LODScaleShadow     = GetLODFloat("LODScaleShadow",     1.0f);
	LODScaleReflection = GetLODFloat("LODScaleReflection", 1.0f);
	LODScaleRefraction = GetLODFloat("LODScaleRefraction", 1.0f);

	CBitmap white;
	white.Alloc(1, 1);
	for (int a = 0; a < 4; ++a) {
		white.mem[a] = 255;
	}

	whiteTex = white.CreateTexture(false);

	unitAmbientColor = mapInfo->light.unitAmbientColor;
	unitSunColor = mapInfo->light.unitSunColor;
	unitShadowDensity = mapInfo->light.unitShadowDensity;

	advFade = GLEW_NV_vertex_program2;
	advShading = (LoadModelShaders() && cubeMapHandler->Init());

	cloakAlpha  = std::max(0.11f, std::min(1.0f, 1.0f - configHandler->Get("UnitTransparency", 0.7f)));
	cloakAlpha1 = std::min(1.0f, cloakAlpha + 0.1f);
	cloakAlpha2 = std::min(1.0f, cloakAlpha + 0.2f);
	cloakAlpha3 = std::min(1.0f, cloakAlpha + 0.4f);

	showHealthBars = !!configHandler->Get("ShowHealthBars", 1);

#ifdef USE_GML
	multiThreadDrawUnit = configHandler->Get("MultiThreadDrawUnit", 1);
	multiThreadDrawUnitShadow = configHandler->Get("MultiThreadDrawUnitShadow", 1);
#endif


	ghostBuildings.resize(MODELTYPE_OTHER);

	opaqueModelRenderers.resize(MODELTYPE_OTHER, NULL);
	cloakedModelRenderers.resize(MODELTYPE_OTHER, NULL);

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		opaqueModelRenderers[modelType] = IWorldObjectModelRenderer::GetInstance(modelType);
		cloakedModelRenderers[modelType] = IWorldObjectModelRenderer::GetInstance(modelType);
	}
}

CUnitDrawer::~CUnitDrawer(void)
{
	eventHandler.RemoveClient(this);

	glDeleteTextures(1, &whiteTex);

	shaderHandler->ReleaseProgramObjects("[UnitDrawer]");
	cubeMapHandler->Free();

	std::list<GhostBuilding*>::iterator gbi;

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		for (gbi = ghostBuildings[modelType].begin(); gbi != ghostBuildings[modelType].end(); /* no inc */) {
			if ((*gbi)->decal) {
				(*gbi)->decal->gbOwner = 0;
			}
			delete *gbi;
			gbi = ghostBuildings[modelType].erase(gbi);
		}
	}

#ifdef USE_GML
	configHandler->Set("MultiThreadDrawUnit", multiThreadDrawUnit);
	configHandler->Set("MultiThreadDrawUnitShadow", multiThreadDrawUnitShadow);
#endif


	ghostBuildings.clear();

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		delete opaqueModelRenderers[modelType];
		delete cloakedModelRenderers[modelType];
	}

	opaqueModelRenderers.clear();
	cloakedModelRenderers.clear();
}



bool CUnitDrawer::LoadModelShaders()
{
	S3ODefShader = shaderHandler->CreateProgramObject("[UnitDrawer]", "S3OShaderDefARB", true);
	S3OAdvShader = S3ODefShader;
	S3OCurShader = S3ODefShader;

	if (!GLEW_ARB_fragment_program) {
		// not possible to do (ARB) shader-based model rendering
		logOutput.Print("[LoadModelShaders] GLEW_ARB_fragment_program OpenGL extension missing");
		return false;
	}
	if (!(!!configHandler->Get("AdvUnitShading", 1))) {
		// not allowed to do shader-based model rendering
		return false;
	}

	// with advFade, submerged transparent objects are clipped against GL_CLIP_PLANE3
	const char* vertexProgNameARB = (advFade)? "ARB/units3o2.vp": "ARB/units3o.vp";

	S3ODefShader->AttachShaderObject(shaderHandler->CreateShaderObject(vertexProgNameARB, "", GL_VERTEX_PROGRAM_ARB));
	S3ODefShader->AttachShaderObject(shaderHandler->CreateShaderObject("ARB/units3o.fp", "", GL_FRAGMENT_PROGRAM_ARB));
	S3ODefShader->Link();

	if (shadowHandler->canUseShadows) {
		if (!gu->haveGLSL) {
			S3OAdvShader = shaderHandler->CreateProgramObject("[UnitDrawer]", "S3OShaderAdvARB", true);
			S3OAdvShader->AttachShaderObject(shaderHandler->CreateShaderObject(vertexProgNameARB, "", GL_VERTEX_PROGRAM_ARB));
			S3OAdvShader->AttachShaderObject(shaderHandler->CreateShaderObject("ARB/units3o_shadow.fp", "", GL_FRAGMENT_PROGRAM_ARB));
			S3OAdvShader->Link();
		} else {
			S3OAdvShader = shaderHandler->CreateProgramObject("[UnitDrawer]", "S3OShaderAdvGLSL", false);
			S3OAdvShader->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/S3OVertProg.glsl", "", GL_VERTEX_SHADER));
			S3OAdvShader->AttachShaderObject(shaderHandler->CreateShaderObject("GLSL/S3OFragProg.glsl", "", GL_FRAGMENT_SHADER));
			S3OAdvShader->Link();
			S3OAdvShader->SetUniformLocation("diffuseTex");        // idx  0 (t1: diffuse + team-color)
			S3OAdvShader->SetUniformLocation("shadingTex");        // idx  1 (t2: spec/refl + self-illum)
			S3OAdvShader->SetUniformLocation("shadowTex");         // idx  2
			S3OAdvShader->SetUniformLocation("reflectTex");        // idx  3 (cube)
			S3OAdvShader->SetUniformLocation("specularTex");       // idx  4 (cube)
			S3OAdvShader->SetUniformLocation("lightDir");          // idx  5
			S3OAdvShader->SetUniformLocation("cameraPos");         // idx  6
			S3OAdvShader->SetUniformLocation("cameraMatInv");      // idx  7
			S3OAdvShader->SetUniformLocation("unitTeamColor");     // idx  8
			S3OAdvShader->SetUniformLocation("unitAmbientColor");  // idx  9
			S3OAdvShader->SetUniformLocation("unitDiffuseColor");  // idx 10
			S3OAdvShader->SetUniformLocation("unitShadowDensity"); // idx 11
			S3OAdvShader->SetUniformLocation("shadowMat");         // idx 12
			S3OAdvShader->SetUniformLocation("shadowParams");      // idx 13

			S3OAdvShader->Enable();
			S3OAdvShader->SetUniform1i(0, 0); // diffuseTex  (idx 0, texunit 0)
			S3OAdvShader->SetUniform1i(1, 1); // shadingTex  (idx 1, texunit 1)
			S3OAdvShader->SetUniform1i(2, 2); // shadowTex   (idx 2, texunit 2)
			S3OAdvShader->SetUniform1i(3, 3); // reflectTex  (idx 3, texunit 3)
			S3OAdvShader->SetUniform1i(4, 4); // specularTex (idx 4, texunit 4)
			S3OAdvShader->SetUniform4fv(5, const_cast<float*>(&mapInfo->light.sunDir[0]));
			S3OAdvShader->SetUniform3fv(9, &unitAmbientColor[0]);
			S3OAdvShader->SetUniform3fv(10, &unitSunColor[0]);
			S3OAdvShader->SetUniform1f(11, unitShadowDensity);
			S3OAdvShader->Disable();
		}

		S3OCurShader = S3OAdvShader;
	}

	return true;
}



void CUnitDrawer::SetUnitDrawDist(float dist)
{
	unitDrawDist = dist;
	unitDrawDistSqr = dist * dist;
}

void CUnitDrawer::SetUnitIconDist(float dist)
{
	unitIconDist = dist;
	iconLength = 750 * unitIconDist * unitIconDist;
}



void CUnitDrawer::Update(void)
{
	{
		GML_STDMUTEX_LOCK(temp); // Update

		while (!tempDrawUnits.empty() && tempDrawUnits.begin()->first < gs->frameNum - 1) {
			tempDrawUnits.erase(tempDrawUnits.begin());
		}
		while (!tempTransparentDrawUnits.empty() && tempTransparentDrawUnits.begin()->first <= gs->frameNum) {
			tempTransparentDrawUnits.erase(tempTransparentDrawUnits.begin());
		}
	}

	{
		GML_RECMUTEX_LOCK(unit); // Update

		for (std::set<CUnit*>::iterator usi = unsortedUnits.begin(); usi != unsortedUnits.end(); ++usi) {
			UpdateDrawPos(*usi);
		}
	}

	useDistToGroundForIcons = camHandler->GetCurrentController().GetUseDistToGroundForIcons();
	if (useDistToGroundForIcons) {
		const float3& camPos = camera->pos;
		// use the height at the current camera position
		//const float groundHeight = ground->GetHeight(camPos.x, camPos.z);
		// use the middle between the highest and lowest position on the map as average
		const float groundHeight = (readmap->currMinHeight + readmap->currMaxHeight) / 2;
		const float overGround = camPos.y - groundHeight;
		sqCamDistToGroundForIcons = overGround * overGround;
	}
}






//! only called by DoDrawUnit
inline bool CUnitDrawer::DrawUnitLOD(CUnit* unit)
{
	if (unit->lodCount > 0) {
		if (unit->isCloaked) {
			const LuaMatType matType = (water->drawReflection)?
				LUAMAT_ALPHA_REFLECT: LUAMAT_ALPHA;
			LuaUnitMaterial& unitMat = unit->luaMats[matType];
			const unsigned lod = unit->CalcLOD(unitMat.GetLastLOD());
			unit->currentLOD = lod;
			LuaUnitLODMaterial* lodMat = unitMat.GetMaterial(lod);

			if ((lodMat != NULL) && lodMat->IsActive()) {
				lodMat->AddUnit(unit); return true;
			}
		} else {
			const LuaMatType matType =
				(water->drawReflection) ? LUAMAT_OPAQUE_REFLECT : LUAMAT_OPAQUE;
			LuaUnitMaterial& unitMat = unit->luaMats[matType];
			const unsigned lod = unit->CalcLOD(unitMat.GetLastLOD());
			unit->currentLOD = lod;
			LuaUnitLODMaterial* lodMat = unitMat.GetMaterial(lod);

			if ((lodMat != NULL) && lodMat->IsActive()) {
				lodMat->AddUnit(unit); return true;
			}
		}
	}

	return false;
}

inline void CUnitDrawer::DoDrawUnit(CUnit* unit, const CUnit* excludeUnit, bool drawReflection, bool drawRefraction)
{
	if (unit == excludeUnit) {
		return;
	}
	if (unit->noDraw) {
		return;
	}
	if (!camera->InView(unit->drawMidPos, unit->radius + 30)) {
		return;
	}

	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	if ((losStatus & LOS_INLOS) || gu->spectatingFullView) {
		if (drawReflection) {
			float3 zeroPos;

			if (unit->drawMidPos.y < 0.0f) {
				zeroPos = unit->drawMidPos;
			} else {
				const float dif = unit->drawMidPos.y - camera->pos.y;
				zeroPos = camera->pos  * (unit->drawMidPos.y / dif) +
					unit->drawMidPos * (-camera->pos.y / dif);
			}
			if (ground->GetApproximateHeight(zeroPos.x, zeroPos.z) > unit->radius) {
				return;
			}
		}
		else if (drawRefraction) {
			if (unit->pos.y > 0.0f) {
				return;
			}
		}
#ifdef USE_GML
		else
			unit->lastDrawFrame = gs->frameNum;
#endif

		const float sqDist = (unit->pos - camera->pos).SqLength();
		unit->isIcon = DrawAsIcon(*unit, sqDist);

		if (unit->isIcon) {
			drawIcon.push_back(unit);
		} else {
			const float farLength = unit->sqRadius * unitDrawDistSqr;

			if (sqDist > farLength) {
				drawFar.push_back(unit);
			} else {
				if (!DrawUnitLOD(unit)) {
					SetTeamColour(unit->team);
					DrawUnitNow(unit);
				}
			}

			if (showHealthBars && (sqDist < (unitDrawDistSqr * 500))) {
				drawStat.push_back(unit);
			}
		}
	} else if (losStatus & LOS_PREVLOS) {
		if ((!gameSetup || gameSetup->ghostedBuildings) && !(unit->mobility)) {
			// it's a building we've had LOS on once,
			// add it to the vector of cloaked units
			const float sqDist = (unit->pos-camera->pos).SqLength();

			unit->isIcon = DrawAsIcon(*unit, sqDist);
		}
		if (losStatus & LOS_INRADAR) {
			if (!(losStatus & LOS_CONTRADAR)) {
				drawRadarIcon.push_back(unit);
			} else if (unit->isIcon) {
				// this prevents us from drawing icons on top of ghosted buildings
				drawIcon.push_back(unit);
			}
		}
	} else if (losStatus & LOS_INRADAR) {
		// if only in radar coverage, mark for drawing as radar icon
		drawRadarIcon.push_back(unit);
	}
}



void CUnitDrawer::Draw(bool drawReflection, bool drawRefraction)
{
	drawFar.clear();
	drawStat.clear();
	drawIcon.clear();
	drawRadarIcon.clear();

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	if (gu->drawFog) {
		glFogfv(GL_FOG_COLOR, mapInfo->atmosphere.fogColor);
		glEnable(GL_FOG);
	}

	if (drawReflection) {
		CUnit::SetLODFactor(LODScale * LODScaleReflection);
	} else if (drawRefraction) {
		CUnit::SetLODFactor(LODScale * LODScaleRefraction);
	} else {
		CUnit::SetLODFactor(LODScale);
	}

	camNorm = camera->forward;
	camNorm.y = -0.1f;
	camNorm.ANormalize();

	SetupForUnitDrawing();

	const CUnit* excludeUnit = drawReflection? NULL: gu->directControl;

	// lock on the bins
	GML_RECMUTEX_LOCK(unit); // Draw

#ifdef USE_GML
	/*
	// FIXME: this code-path is now broken
	if (multiThreadDrawUnit) {
		mt_drawReflection = drawReflection; // these member vars will be accessed by DoDrawUnitMT
		mt_drawRefraction = drawRefraction;
		mt_excludeUnit = excludeUnit;
		gmlProcessor->Work(
			NULL, NULL, &CUnitDrawer::DoDrawUnitMT, this, gmlThreadCount,
			FALSE, &unsortedUnitsGML, unsortedUnitsGML.size(), 50, 100, TRUE
		);
	}
	*/
#endif

	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		DrawOpaqueUnits(modelType, excludeUnit, drawReflection, drawRefraction);
	}

	CleanUpUnitDrawing();

	DrawOpaqueShaderUnits();
	DrawFarTextures();
	DrawUnitIcons(drawReflection);

	glDisable(GL_FOG);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_2D);
}



void CUnitDrawer::DrawOpaqueUnits(int modelType, const CUnit* excludeUnit, bool drawReflection, bool drawRefraction)
{
	const std::map<int, std::set<CUnit*> >& opaqueUnits =
		opaqueModelRenderers[modelType]->GetUnitBin();

	std::map<int, std::set<CUnit*> >::const_iterator binIt;
	std::set<CUnit*>::const_iterator setIt;

	#define DRAW_UNIT_SET(units)                                                          \
		for (binIt = units.begin(); binIt != units.end(); binIt++) {                      \
			if (modelType == MODELTYPE_S3O) {                                             \
				texturehandlerS3O->SetS3oTexture(binIt->first);                           \
			}                                                                             \
                                                                                          \
			const std::set<CUnit*>& opaqueUnitSet = binIt->second;                        \
                                                                                          \
			for (setIt = opaqueUnitSet.begin(); setIt != opaqueUnitSet.end(); setIt++) {  \
				DoDrawUnit(*setIt, excludeUnit, drawReflection, drawRefraction);          \
			}                                                                             \
		}

	switch (modelType) {
		case MODELTYPE_3DO: {
			SetupFor3DO();

			DRAW_UNIT_SET(opaqueUnits);

			DrawOpaqueAIUnits();
			CleanUp3DO();
		} break;

		case MODELTYPE_S3O: {
			DRAW_UNIT_SET(opaqueUnits);

			// draw non-unit opaque S3O cruft
			// (ie. features and projectiles)
			DrawQuedS3O();
		} break;

		default: {
		} break;
	}

	#undef DRAW_UNIT_SET
}

void CUnitDrawer::DrawOpaqueAIUnits()
{
	GML_STDMUTEX_LOCK(temp);

	// non-cloaked AI unit ghosts (FIXME: s3o's + teamcolor)
	for (std::multimap<int, TempDrawUnit>::iterator ti = tempDrawUnits.begin(); ti != tempDrawUnits.end(); ++ti) {
		if (camera->InView(ti->second.pos, 100)) {
			glPushMatrix();
			glTranslatef3(ti->second.pos);
			glRotatef(ti->second.rotation * 180 / PI, 0, 1, 0);

			const UnitDef* udef = ti->second.unitdef;
			const S3DModel* model = udef->LoadModel();

			model->DrawStatic();
			glPopMatrix();
		}
	}
}

void CUnitDrawer::DrawFarTextures()
{
	if (drawFar.empty()) {
		return;
	}

	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.5f);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, farTextureHandler->GetTextureID());
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glNormal3fv((const GLfloat*) &camNorm.x);

	if (gu->drawFog) {
		glFogfv(GL_FOG_COLOR, mapInfo->atmosphere.fogColor);
		glEnable(GL_FOG);
	}

	va = GetVertexArray();
	va->Initialize();
	va->EnlargeArrays(drawFar.size() * 4, 0, VA_SIZE_T);
	for (GML_VECTOR<CUnit*>::iterator it = drawFar.begin(); it != drawFar.end(); it++) {
		farTextureHandler->DrawFarTexture(camera, (*it)->model, (*it)->drawPos, (*it)->radius, (*it)->heading, va);
	}

	va->DrawArrayT(GL_QUADS);
}

void CUnitDrawer::DrawUnitIcons(bool drawReflection)
{
	if (!drawReflection) {
		// Draw unit icons and radar blips.
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.5f);

		GML_VECTOR<CUnit*>::iterator ui;

		for (ui = drawIcon.begin(); ui != drawIcon.end(); ++ui) {
			DrawIcon(*ui, false);
		}
		for (ui = drawRadarIcon.begin(); ui != drawRadarIcon.end(); ++ui) {
			DrawIcon(*ui, true);
		}

		glDisable(GL_TEXTURE_2D);
		glDisable(GL_ALPHA_TEST);
		for (ui = drawStat.begin(); ui != drawStat.end(); ++ui) {
			DrawUnitStats(*ui);
		}
	}
}





/******************************************************************************/
/******************************************************************************/

static void DrawBins(LuaMatType type)
{
	const LuaMatBinSet& bins = luaMatHandler.GetBins(type);
	if (bins.empty()) {
		return;
	}

	luaDrawing = true;

	glPushAttrib(GL_TEXTURE_BIT | GL_ENABLE_BIT | GL_TRANSFORM_BIT);
	if (type==LUAMAT_ALPHA || type==LUAMAT_ALPHA_REFLECT) {
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.1f);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}else{
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GREATER, 0.5f);
	}

	const LuaMaterial* currMat = &LuaMaterial::defMat;

	LuaMatBinSet::const_iterator it;
	for (it = bins.begin(); it != bins.end(); ++it) {
		LuaMatBin* bin = *it;
		bin->Execute(*currMat);
		currMat = (LuaMaterial*)bin;

		const GML_VECTOR<CUnit*>& units = bin->GetUnits();
		const int count = (int)units.size();
		for (int i = 0; i < count; i++) {
			CUnit* unit = units[i];
			const LuaUnitMaterial& unitMat = unit->luaMats[type];
			const LuaUnitLODMaterial* lodMat = unitMat.GetMaterial(unit->currentLOD);

			lodMat->uniforms.Execute(unit);

			unitDrawer->SetTeamColour(unit->team);
			unitDrawer->DrawUnitWithLists(unit, lodMat->preDisplayList, lodMat->postDisplayList);
		}
	}

	LuaMaterial::defMat.Execute(*currMat);
	luaMatHandler.ClearBins(type);

	glPopAttrib();

	luaDrawing = false;
}


/******************************************************************************/

static void SetupShadowDrawing()
{
	//FIXME setup face culling for s3o?

	glColor3f(1.0f, 1.0f, 1.0f);
	glDisable(GL_TEXTURE_2D);

	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);

	Shader::IProgramObject* po =
		shadowHandler->GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);
	po->Enable();
}


static void CleanUpShadowDrawing()
{
	Shader::IProgramObject* po =
		shadowHandler->GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);

	po->Disable();
	glDisable(GL_POLYGON_OFFSET_FILL);
}


/******************************************************************************/

static void SetupOpaque3DO() { unitDrawer->SetupForUnitDrawing();  unitDrawer->SetupFor3DO(); }
static void ResetOpaque3DO() { unitDrawer->CleanUp3DO();   unitDrawer->CleanUpUnitDrawing();  }
static void SetupOpaqueS3O() { unitDrawer->SetupForUnitDrawing();  }
static void ResetOpaqueS3O() { unitDrawer->CleanUpUnitDrawing();   }

static void SetupAlpha3DO()  { unitDrawer->SetupForGhostDrawing(); unitDrawer->SetupFor3DO(); }
static void ResetAlpha3DO()  { unitDrawer->CleanUp3DO();   unitDrawer->CleanUpGhostDrawing(); }
static void SetupAlphaS3O()  { unitDrawer->SetupForGhostDrawing();    }
static void ResetAlphaS3O()  { unitDrawer->CleanUpGhostDrawing();     }

static void SetupShadow3DO() { SetupShadowDrawing();   }
static void ResetShadow3DO() { CleanUpShadowDrawing(); }
static void SetupShadowS3O() { SetupShadowDrawing();   }
static void ResetShadowS3O() { CleanUpShadowDrawing(); }


/******************************************************************************/

void CUnitDrawer::DrawOpaqueShaderUnits()
{
	luaMatHandler.setup3doShader = SetupOpaque3DO;
	luaMatHandler.reset3doShader = ResetOpaque3DO;
	luaMatHandler.setupS3oShader = SetupOpaqueS3O;
	luaMatHandler.resetS3oShader = ResetOpaqueS3O;

	const LuaMatType matType =
		(water->drawReflection) ? LUAMAT_OPAQUE_REFLECT : LUAMAT_OPAQUE;

	DrawBins(matType);
}


void CUnitDrawer::DrawCloakedShaderUnits()
{
	luaMatHandler.setup3doShader = SetupAlpha3DO;
	luaMatHandler.reset3doShader = ResetAlpha3DO;
	luaMatHandler.setupS3oShader = SetupAlphaS3O;
	luaMatHandler.resetS3oShader = ResetAlphaS3O;

	const LuaMatType matType =
		(water->drawReflection) ? LUAMAT_ALPHA_REFLECT : LUAMAT_ALPHA;

	DrawBins(matType);
}


void CUnitDrawer::DrawShadowShaderUnits()
{
	luaMatHandler.setup3doShader = SetupShadow3DO;
	luaMatHandler.reset3doShader = ResetShadow3DO;
	luaMatHandler.setupS3oShader = SetupShadowS3O;
	luaMatHandler.resetS3oShader = ResetShadowS3O;

	DrawBins(LUAMAT_SHADOW);
}



/******************************************************************************/
/******************************************************************************/


inline void CUnitDrawer::DoDrawUnitShadow(CUnit* unit) {
	const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

	// do shadow alpha-masking for S3O units only
	// (3DO's need more setup than it is worth)
	#ifdef UNIT_SHADOW_ALPHA_MASKING
		#define S3O_TEX(model) \
			texturehandlerS3O->GetS3oTex(model->textureType)
		#define PUSH_SHADOW_TEXTURE_STATE(model)                    \
			if (model->type == MODELTYPE_S3O) {                     \
				glActiveTexture(GL_TEXTURE0);                       \
				glEnable(GL_TEXTURE_2D);                            \
				glBindTexture(GL_TEXTURE_2D, S3O_TEX(model)->tex2); \
			}
		#define POP_SHADOW_TEXTURE_STATE(model)   \
			if (model->type == MODELTYPE_S3O) {   \
				glBindTexture(GL_TEXTURE_2D, 0);  \
				glDisable(GL_TEXTURE_2D);         \
				glActiveTexture(GL_TEXTURE0);     \
			}
	#else
		#define PUSH_SHADOW_TEXTURE_STATE(model)
		#define POP_SHADOW_TEXTURE_STATE(model)
	#endif

	// FIXME: test against the shadow projection intersection
	if (!(((losStatus & LOS_INLOS) || gu->spectatingFullView) &&
		camera->InView(unit->drawMidPos, unit->radius + 700))) {
		return;
	}

	const float sqDist = (unit->pos - camera->pos).SqLength();
	const float farLength = unit->sqRadius * unitDrawDistSqr;

	if (sqDist >= farLength) { return; }
	if (unit->isCloaked) { return; }
	if (DrawAsIcon(*unit, sqDist)) { return; }

	if (unit->lodCount <= 0) {
		PUSH_SHADOW_TEXTURE_STATE(unit->model);
		DrawUnitNow(unit);
		POP_SHADOW_TEXTURE_STATE(unit->model);
	} else {
		LuaUnitMaterial& unitMat = unit->luaMats[LUAMAT_SHADOW];
		const unsigned lod = unit->CalcLOD(unitMat.GetLastLOD());
		unit->currentLOD = lod;
		LuaUnitLODMaterial* lodMat = unitMat.GetMaterial(lod);

		if ((lodMat != NULL) && lodMat->IsActive()) {
			lodMat->AddUnit(unit);
		} else {
			PUSH_SHADOW_TEXTURE_STATE(unit->model);
			DrawUnitNow(unit);
			POP_SHADOW_TEXTURE_STATE(unit->model);
		}
	}

	#undef PUSH_SHADOW_TEXTURE_STATE
	#undef POP_SHADOW_TEXTURE_STATE
}

void CUnitDrawer::DrawShadowPass(void)
{
	glColor3f(1.0f, 1.0f, 1.0f);
	glPolygonOffset(1.0f, 1.0f);
	glEnable(GL_POLYGON_OFFSET_FILL);

	#ifdef UNIT_SHADOW_ALPHA_MASKING
	glAlphaFunc(GL_GREATER, 0.5f);
	glEnable(GL_ALPHA_TEST);
	#endif

	Shader::IProgramObject* po =
		shadowHandler->GetShadowGenProg(CShadowHandler::SHADOWGEN_PROGRAM_MODEL);
	po->Enable();

	CUnit::SetLODFactor(LODScale * LODScaleShadow);


	GML_RECMUTEX_LOCK(unit); // DrawShadowPass


#ifdef USE_GML
	if (multiThreadDrawUnitShadow) {
		gmlProcessor->Work(
			NULL, NULL, &CUnitDrawer::DoDrawUnitShadowMT, this, gmlThreadCount,
			FALSE, &unsortedUnitsGML, unsortedUnitsGML.size(), 50, 100, TRUE
		);
	}
	else
#endif
	{
		for (std::set<CUnit*>::iterator usi = unsortedUnits.begin(); usi != unsortedUnits.end(); ++usi) {
			DoDrawUnitShadow(*usi);
		}
	}

	po->Disable();

	#ifdef UNIT_SHADOW_ALPHA_MASKING
	glDisable(GL_ALPHA_TEST);
	#endif

	glDisable(GL_POLYGON_OFFSET_FILL);

	DrawShadowShaderUnits();
}



void CUnitDrawer::DrawIcon(CUnit * unit, bool asRadarBlip)
{
	// If the icon is to be drawn as a radar blip, we want to get the default icon.
	const CIconData* iconData;
	if (asRadarBlip) {
		iconData = iconHandler->GetDefaultIconData();
	} else {
		iconData = unit->unitDef->iconType.GetIconData();
	}

	// Calculate the icon size. It scales with:
	//  * The square root of the camera distance.
	//  * The mod defined 'iconSize' (which acts a multiplier).
	//  * The unit radius, depending on whether the mod defined 'radiusadjust' is true or false.
	float3 pos;
	if (gu->spectatingFullView) {
		pos = unit->drawMidPos;
	} else {
		pos = helper->GetUnitErrorPos(unit, gu->myAllyTeam);
	}
	float dist = fastmath::sqrt2(fastmath::sqrt2((pos - camera->pos).SqLength()));
	float scale = 0.4f * iconData->GetSize() * dist;
	if (iconData->GetRadiusAdjust() && !asRadarBlip) {
		// I take the standard unit radius to be 30
		// ... call it an educated guess. (Teake Nutma)
		scale *= (unit->radius / 30);
	}

	unit->iconRadius = scale; // store the icon size so that we don't have to calculate it again

	// Is the unit selected? Then draw it white.
	if (unit->commandAI->selected) {
		glColor3ub(255, 255, 255);
	} else {
		glColor3ubv(teamHandler->Team(unit->team)->color);
	}

	// If the icon is partly under the ground, move it up.
	const float h = ground->GetHeight(pos.x, pos.z);
	if (pos.y < (h + scale)) {
		pos.y = (h + scale);
	}

	// calculate the vertices
	const float3 dy = camera->up    * scale;
	const float3 dx = camera->right * scale;
	const float3 vn = pos - dx;
	const float3 vp = pos + dx;
	const float3 vnn = vn - dy;
	const float3 vpn = vp - dy;
	const float3 vnp = vn + dy;
	const float3 vpp = vp + dy;

	// Draw the icon.
	iconData->Draw(vnn, vpn, vnp, vpp);
}






void CUnitDrawer::SetupForGhostDrawing() const
{
	glEnable(GL_LIGHTING); // Give faded objects same appearance as regular
	glLightfv(GL_LIGHT1, GL_POSITION, mapInfo->light.sunDir);
	glEnable(GL_LIGHT1);

	SetupBasicS3OTexture0();
	SetupBasicS3OTexture1(); // This also sets up the transparency

	static const float cols[] = {1.0f, 1.0f, 1.0f, 1.0f};
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, cols);
	glColor3f(1.0f, 1.0f, 1.0f);

	glActiveTextureARB(GL_TEXTURE0_ARB);
	glEnable(GL_TEXTURE_2D);

	glPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GREATER, 0.1f);
	glDepthMask(GL_FALSE);
}

void CUnitDrawer::CleanUpGhostDrawing() const
{
	glPopAttrib();
	glDisable(GL_TEXTURE_2D);

	// clean up s3o drawing stuff
	// reset texture1 state
	CleanupBasicS3OTexture1();

	// reset texture0 state
	CleanupBasicS3OTexture0();

	glDisable(GL_LIGHTING);
	glDisable(GL_LIGHT1);
}



void CUnitDrawer::DrawCloakedUnits(bool submerged, bool disableAdvShading)
{
	const bool oldAdvShading = advShading;

	{
		// don't use shaders if shadows are enabled
		advShading = advShading && !disableAdvShading;

		if (advShading) {
			SetupForUnitDrawing();
			glDisable(GL_ALPHA_TEST);
		} else {
			SetupForGhostDrawing();
		}


		const double plane[4] = {0.0f, submerged? -1.0f: 1.0f, 0.0f, 0.0f};

		glClipPlane(GL_CLIP_PLANE3, plane);
		glColor4f(1.0f, 1.0f, 1.0f, cloakAlpha);

		GML_RECMUTEX_LOCK(unit); // DrawCloakedUnits

		DrawCloakedUnitsHelper();

		if (advShading) {
			CleanUpUnitDrawing();
			glEnable(GL_ALPHA_TEST);
		} else {
			CleanUpGhostDrawing();
		}

		advShading = oldAdvShading;
	}

	// shader rendering
	DrawCloakedShaderUnits();
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

void CUnitDrawer::DrawCloakedUnitsHelper()
{
	for (int modelType = MODELTYPE_3DO; modelType < MODELTYPE_OTHER; modelType++) {
		if (modelType == MODELTYPE_3DO) {
			SetupFor3DO();
			DrawCloakedAIUnits();
		}

		{
			typedef std::map<int, std::set<CUnit*> > UnitRenderBin;
			typedef std::map<int, std::set<CUnit*> >::const_iterator UnitRenderBinIt;

			const UnitRenderBin& cloakedUnitSets =
				cloakedModelRenderers[modelType]->GetUnitBin();

			// cloaked units and living ghosted buildings
			for (UnitRenderBinIt it = cloakedUnitSets.begin(); it != cloakedUnitSets.end(); it++) {
				if (modelType == MODELTYPE_S3O) {
					texturehandlerS3O->SetS3oTexture(it->first);
				}

				DrawCloakedUnitsSet(it->second, modelType);
			}
		}

		DrawGhostedBuildings(modelType);

		if (modelType == MODELTYPE_3DO) {
			CleanUp3DO();
		}
	}
}

void CUnitDrawer::DrawCloakedUnitsSet(const std::set<CUnit*>& cloakedUnits, int modelType) {
	for (std::set<CUnit*>::const_iterator ui = cloakedUnits.begin(); ui != cloakedUnits.end(); ++ui) {
		CUnit* unit = *ui;

		#if defined(USE_GML) && GML_ENABLE_SIM
		if (unit == NULL) { continue; }
		#endif

		const unsigned short losStatus = unit->losStatus[gu->myAllyTeam];

		if ((losStatus & LOS_INLOS) || gu->spectatingFullView) {
			SetTeamColour(unit->team, cloakAlpha);
			DrawUnitNow(unit);
		} else {
			// check for decoy models
			const UnitDef* decoyDef = unit->unitDef->decoyDef;
			const S3DModel* model = NULL;

			if (decoyDef == NULL) {
				model = unit->model;
			} else {
				model = decoyDef->LoadModel();
			}


			// FIXME: needs a second pass
			if (model->type != modelType) {
				continue;
			}


			// ghosted enemy units
			if (losStatus & LOS_CONTRADAR) {
				glColor4f(0.9f, 0.9f, 0.9f, cloakAlpha2);
			} else {
				glColor4f(0.6f, 0.6f, 0.6f, cloakAlpha1);
			}

			glPushMatrix();
			glTranslatef3(unit->pos);
			glRotatef(unit->buildFacing * 90.0f, 0, 1, 0);

			SetTeamColour(unit->team, (losStatus & LOS_CONTRADAR) ? cloakAlpha2 : cloakAlpha1);
			model->DrawStatic();
			glPopMatrix();

			glColor4f(1.0f, 1.0f, 1.0f, cloakAlpha);
		}
	}
}

void CUnitDrawer::DrawCloakedAIUnits()
{
	GML_STDMUTEX_LOCK(temp);

	// cloaked AI unit ghosts (FIXME: S3O's need different state)
	for (std::multimap<int, TempDrawUnit>::iterator ti = tempTransparentDrawUnits.begin(); ti != tempTransparentDrawUnits.end(); ++ti) {
		if (camera->InView(ti->second.pos, 100)) {
			glPushMatrix();
			glTranslatef3(ti->second.pos);
			glRotatef(ti->second.rotation * 180 / PI, 0, 1, 0);

			const UnitDef* udef = ti->second.unitdef;
			const S3DModel* model = udef->LoadModel();

			SetTeamColour(ti->second.team, cloakAlpha);

			model->DrawStatic();
			glPopMatrix();
		}
		if (ti->second.drawBorder) {
			float3 pos = ti->second.pos;
			const UnitDef* unitdef = ti->second.unitdef;

			SetTeamColour(ti->second.team, cloakAlpha3);

			BuildInfo bi(unitdef, pos, ti->second.facing);
			pos = helper->Pos2BuildPos(bi);

			const float xsize = bi.GetXSize() * 4;
			const float zsize = bi.GetZSize() * 4;

			glColor4f(0.2f, 1, 0.2f, cloakAlpha3);
			glDisable(GL_TEXTURE_2D);
			glBegin(GL_LINE_STRIP);
				glVertexf3(pos + float3( xsize, 1.0f,  zsize));
				glVertexf3(pos + float3(-xsize, 1.0f,  zsize));
				glVertexf3(pos + float3(-xsize, 1.0f, -zsize));
				glVertexf3(pos + float3( xsize, 1.0f, -zsize));
				glVertexf3(pos + float3( xsize, 1.0f,  zsize));
			glEnd();
			glColor4f(1.0f, 1.0f, 1.0f, cloakAlpha);
			glEnable(GL_TEXTURE_2D);
		}
	}
}

void CUnitDrawer::DrawGhostedBuildings(int modelType)
{
	std::list<GhostBuilding*>& ghostedBuildings = ghostBuildings[modelType];

	// buildings that died but were still ghosted
	glColor4f(0.6f, 0.6f, 0.6f, cloakAlpha1);
	for (std::list<GhostBuilding*>::iterator gbi = ghostedBuildings.begin(); gbi != ghostedBuildings.end(); ) {
		if (loshandler->InLos((*gbi)->pos, gu->myAllyTeam)) {
			if ((*gbi)->decal)
				(*gbi)->decal->gbOwner = 0;

			delete *gbi;
			gbi = ghostedBuildings.erase(gbi);
		} else {
			if (camera->InView((*gbi)->pos, (*gbi)->model->radius * 2)) {
				glPushMatrix();
				glTranslatef3((*gbi)->pos);
				glRotatef((*gbi)->facing * 90.0f, 0, 1, 0);

				if (modelType == MODELTYPE_S3O) {
					texturehandlerS3O->SetS3oTexture((*gbi)->model->textureType);
				}

				SetTeamColour((*gbi)->team, cloakAlpha1);
				(*gbi)->model->DrawStatic();
				glPopMatrix();
			}
			++gbi;
		}
	}
}





void CUnitDrawer::SetupForUnitDrawing(void)
{
	glCullFace(GL_BACK);
	glEnable(GL_CULL_FACE);

	glAlphaFunc(GL_GREATER, 0.5f);
	glEnable(GL_ALPHA_TEST);

	if (advShading && !water->drawReflection) {
		const float4 shadowParams = float4(shadowHandler->xmid, shadowHandler->ymid, shadowHandler->p17, shadowHandler->p18);

		// ARB standard does not seem to support
		// vertex program + clipplanes (used for
		// reflective pass) at once ==> not true,
		// but needs option ARB_position_invariant
		S3OCurShader = (shadowHandler->drawShadows)? S3OAdvShader: S3ODefShader;
		S3OCurShader->Enable();

		if (gu->haveGLSL && shadowHandler->drawShadows) {
			S3OCurShader->SetUniform3fv(6, &camera->pos[0]);
			S3OCurShader->SetUniformMatrix4fv(7, false, (float*) &camera->modelviewInverse[0]);
			S3OCurShader->SetUniformMatrix4fv(12, false, &shadowHandler->shadowMatrix.m[0]);
			S3OCurShader->SetUniform4fv(13, const_cast<float*>(&shadowParams[0]));
		} else {
			S3OCurShader->SetUniformTarget(GL_VERTEX_PROGRAM_ARB);
			S3OCurShader->SetUniform4f(10, mapInfo->light.sunDir.x, mapInfo->light.sunDir.y ,mapInfo->light.sunDir.z, 0.0f);
			S3OCurShader->SetUniform4f(11, unitSunColor.x, unitSunColor.y, unitSunColor.z, 0.0f);
			S3OCurShader->SetUniform4f(12, unitAmbientColor.x, unitAmbientColor.y, unitAmbientColor.z, 1.0f); //!
			S3OCurShader->SetUniform4f(13, camera->pos.x, camera->pos.y, camera->pos.z, 0.0f);
			S3OCurShader->SetUniformTarget(GL_FRAGMENT_PROGRAM_ARB);
			S3OCurShader->SetUniform4f(10, 0.0f, 0.0f, 0.0f, unitShadowDensity);
			S3OCurShader->SetUniform4f(11, unitAmbientColor.x, unitAmbientColor.y, unitAmbientColor.z, 1.0f);

			glMatrixMode(GL_MATRIX0_ARB);
			glLoadMatrixf(shadowHandler->shadowMatrix.m);
		}

		glActiveTextureARB(GL_TEXTURE0_ARB);
		glEnable(GL_TEXTURE_2D);

		glActiveTextureARB(GL_TEXTURE1_ARB);
		glEnable(GL_TEXTURE_2D);

		if (shadowHandler->drawShadows) {
			glActiveTextureARB(GL_TEXTURE2_ARB);
			glBindTexture(GL_TEXTURE_2D, shadowHandler->shadowTexture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC_ARB, GL_LEQUAL);
			glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE_ARB, GL_LUMINANCE);
			glEnable(GL_TEXTURE_2D);
		}

		glActiveTextureARB(GL_TEXTURE3_ARB);
		glEnable(GL_TEXTURE_CUBE_MAP_ARB);
		glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, cubeMapHandler->GetReflectionTextureID());

		glActiveTextureARB(GL_TEXTURE4_ARB);
		glEnable(GL_TEXTURE_CUBE_MAP_ARB);
		glBindTexture(GL_TEXTURE_CUBE_MAP_ARB, cubeMapHandler->GetSpecularTextureID());

		glActiveTextureARB(GL_TEXTURE0_ARB);

		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glMatrixMode(GL_PROJECTION);
		glPushMatrix();
		glMultMatrixd(camera->GetModelview());
		glMatrixMode(GL_MODELVIEW);
		glPushMatrix();
		glLoadIdentity();
	} else {
		glEnable(GL_LIGHTING);
		glLightfv(GL_LIGHT1, GL_POSITION, mapInfo->light.sunDir);
		glEnable(GL_LIGHT1);

		SetupBasicS3OTexture1();
		SetupBasicS3OTexture0();

		// Set material color
		static const float cols[] = {1.0f, 1.0f, 1.0, 1.0f};
		glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, cols);
		glColor4fv(cols);
	}
}

void CUnitDrawer::CleanUpUnitDrawing(void) const
{
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);

	if (advShading && !water->drawReflection) {
		S3OCurShader->Disable();

		glActiveTextureARB(GL_TEXTURE1_ARB);
		glDisable(GL_TEXTURE_2D);

		glActiveTextureARB(GL_TEXTURE2_ARB);
		glDisable(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_NONE);
		glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE_ARB, GL_LUMINANCE);

		glActiveTextureARB(GL_TEXTURE3_ARB);
		glDisable(GL_TEXTURE_CUBE_MAP_ARB);

		glActiveTextureARB(GL_TEXTURE4_ARB);
		glDisable(GL_TEXTURE_CUBE_MAP_ARB);

		glActiveTextureARB(GL_TEXTURE0_ARB);

		glMatrixMode(GL_PROJECTION);
		glPopMatrix();
		glMatrixMode(GL_MODELVIEW);
		glPopMatrix();
	} else {
		glDisable(GL_LIGHTING);
		glDisable(GL_LIGHT1);

		CleanupBasicS3OTexture1();
		CleanupBasicS3OTexture0();
	}
}



void CUnitDrawer::SetTeamColour(int team, float alpha) const
{
	if (advShading && !water->drawReflection) {
		CTeam* t = teamHandler->Team(team);
		float4 c = float4(t->color[0] / 255.0f, t->color[1] / 255.0f, t->color[2] / 255.0f, alpha);

		if (gu->haveGLSL && shadowHandler->drawShadows) {
			S3OCurShader->SetUniform4fv(8, &c[0]);
		} else {
			S3OCurShader->SetUniformTarget(GL_FRAGMENT_PROGRAM_ARB);
			S3OCurShader->SetUniform4fv(14, &c[0]);
		}

		if (luaDrawing) {// FIXME?
			SetBasicTeamColour(team, alpha);
		}
	} else {
		// non-shader case via texture combiners
		SetBasicTeamColour(team, alpha);
	}
}

void CUnitDrawer::SetBasicTeamColour(int team, float alpha) const
{
	const unsigned char* col = teamHandler->Team(team)->color;
	const float texConstant[] = {col[0] / 255.0f, col[1] / 255.0f, col[2] / 255.0f, alpha};
	const float matConstant[] = {1.0f, 1.0f, 1.0f, alpha};

	glActiveTexture(GL_TEXTURE0);
	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, texConstant);
	glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, matConstant);
}



/**
 * Binds the 3do texture atlas and deactivates face culling
 */
void CUnitDrawer::SetupFor3DO() const
{
	texturehandler3DO->Set3doAtlases();
	glPushAttrib(GL_POLYGON_BIT);
	glDisable(GL_CULL_FACE);
}

/**
 * Reset face culling after 3do drawing
 */
void CUnitDrawer::CleanUp3DO() const
{
	glPopAttrib();
}


/**
 * Set up the texture environment in texture unit 0
 * to give an S3O texture its team-colour.
 *
 * Also:
 * - call SetBasicTeamColour to set the team colour to transform to.
 * - Replace the output alpha channel. If not, only the team-coloured bits will show, if that. Or something.
 */
void CUnitDrawer::SetupBasicS3OTexture0(void) const
{
	glActiveTextureARB(GL_TEXTURE0_ARB);
	glEnable(GL_TEXTURE_2D);

	// RGB = Texture * (1 - Alpha) + Teamcolor * Alpha
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_INTERPOLATE_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_CONSTANT_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE2_RGB_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB_ARB, GL_ONE_MINUS_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);

	// ALPHA = Ignore
}

/**
 * This sets the first texture unit to GL_MODULATE the colours from the
 * first texture unit with the current glColor.
 *
 * Normal S3O drawing sets the color to full white; translucencies
 * use this setup to 'tint' the drawn model.
 *
 * - Leaves glActivateTextureARB at the first unit.
 * - This doesn't tinker with the output alpha, either.
 */
void CUnitDrawer::SetupBasicS3OTexture1(void) const
{
	glActiveTextureARB(GL_TEXTURE1_ARB);
	glEnable(GL_TEXTURE_2D);

	// RGB = Primary Color * Previous
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB_ARB, GL_PRIMARY_COLOR_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);

	// ALPHA = Current alpha * Alpha mask
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA_ARB, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_ALPHA_ARB, GL_PRIMARY_COLOR_ARB);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA_ARB, GL_SRC_ALPHA);
}


void CUnitDrawer::CleanupBasicS3OTexture1(void) const
{
	// reset texture1 state
	glActiveTextureARB(GL_TEXTURE1_ARB);
	glDisable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE1_ALPHA_ARB, GL_PREVIOUS_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE0_RGB_ARB, GL_TEXTURE);
	glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
}

void CUnitDrawer::CleanupBasicS3OTexture0(void) const
{
	// reset texture0 state
	glActiveTextureARB(GL_TEXTURE0_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE1_RGB_ARB, GL_PREVIOUS_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE2_RGB_ARB, GL_CONSTANT_ARB);
	glTexEnvi(GL_TEXTURE_ENV,GL_OPERAND2_RGB_ARB, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_RGB_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
}




/**
 * The companion to UnitDrawingTexturesOff(), re-enables the texture units
 * needed for drawing a model.
 *
 * Does *not* restore the texture bindings.
 */
void CUnitDrawer::UnitDrawingTexturesOn()
{
	// XXX FIXME GL_VERTEX_PROGRAM_ARB is very slow on ATIs here for some reason
	// if clip planes are enabled
	// check later after driver updates
	if (advShading && !water->drawReflection) {
		glEnable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE1_ARB);
		glEnable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE2_ARB);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_COMPARE_R_TO_TEXTURE);
		glEnable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE3_ARB);
		glEnable(GL_TEXTURE_CUBE_MAP_ARB);
		glActiveTextureARB(GL_TEXTURE4_ARB);
		glEnable(GL_TEXTURE_CUBE_MAP_ARB);
		glActiveTextureARB(GL_TEXTURE0_ARB);
	} else {
		glEnable(GL_LIGHTING);
		glColor3f(1.0f, 1.0f, 1.0f);
		glEnable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE1_ARB);
		glEnable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE0_ARB);
	}
}

/**
 * Between a pair of SetupFor/CleanUpUnitDrawing,
 * temporarily turns off textures and shaders.
 *
 * Used by CUnit::Draw() for drawing a unit under construction.
 *
 * Unfortunately, it doesn't work! With advanced shading on, the green
 * is darker than usual; with shadows as well, it's almost black. -- krudat
 */
void CUnitDrawer::UnitDrawingTexturesOff()
{
	/* If SetupForUnitDrawing is changed, this may need tweaking too. */
	if (advShading && !water->drawReflection) {
		glActiveTextureARB(GL_TEXTURE1_ARB); //! 'Shiny' texture.
		glDisable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE2_ARB); //! Shadows.
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE_ARB, GL_NONE);
		glDisable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE3_ARB); //! reflectionTex
		glDisable(GL_TEXTURE_CUBE_MAP_ARB);
		glActiveTextureARB(GL_TEXTURE4_ARB); //! specularTex
		glDisable(GL_TEXTURE_CUBE_MAP_ARB);
		glActiveTextureARB(GL_TEXTURE0_ARB);
		glDisable(GL_TEXTURE_2D); //! albedo + teamcolor
	} else {
		glDisable(GL_LIGHTING);
		glDisable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE1_ARB); //! GL lighting, I think.
		glDisable(GL_TEXTURE_2D);
		glActiveTextureARB(GL_TEXTURE0_ARB);
		glDisable(GL_TEXTURE_2D); //! albedo + teamcolor
	}
}






void CUnitDrawer::QueS3ODraw(CWorldObject* object, int textureType)
{
#ifdef USE_GML
	quedS3Os.acquire(textureType).push_back(object);
	quedS3Os.release();
#else
	const size_t signedTextureType = (textureType < 0) ? 0 : textureType;
	while (quedS3Os.size() <= signedTextureType) {
		quedS3Os.push_back(GML_VECTOR<CWorldObject*>());
	}
	quedS3Os[textureType].push_back(object);
	usedS3OTextures.insert(textureType);
#endif
}

void CUnitDrawer::DrawQuedS3O(void)
{
#ifdef USE_GML
	const int sz = quedS3Os.size();
	for (int tex = 0; tex < sz; ++tex) {
		if (quedS3Os[tex].size() > 0) {
			texturehandlerS3O->SetS3oTexture(tex);

			for (GML_VECTOR<CWorldObject*>::iterator ui = quedS3Os[tex].begin(); ui != quedS3Os[tex].end(); ++ui) {
				// for unit and feature objects, this calls back
				// to DrawUnitS3O() and to DrawFeatureStatic()
				// respectively
				if (*ui) { (*ui)->DrawS3O(); }
			}

			quedS3Os[tex].clear();
		}
	}
#else
	for (std::set<int>::iterator uti = usedS3OTextures.begin(); uti != usedS3OTextures.end(); ++uti) {
		const int tex = *uti;
		texturehandlerS3O->SetS3oTexture(tex);

		for (GML_VECTOR<CWorldObject*>::iterator ui = quedS3Os[tex].begin(); ui != quedS3Os[tex].end(); ++ui) {
			if (*ui) { (*ui)->DrawS3O(); }
		}

		quedS3Os[tex].clear();
	}
	usedS3OTextures.clear();
#endif
}



/**
 * Draw one unit.
 *
 * Used for drawing the view of the controlled unit.
 *
 * Note: does all the GL state setting for that one unit, so you might want
 * something else for drawing many units.
 */
void CUnitDrawer::DrawIndividual(CUnit* unit)
{
	const bool origDebug = gu->drawdebug;
	gu->drawdebug = false;

	LuaUnitLODMaterial* lodMat = NULL;

	if (unit->lodCount > 0) {
		const LuaMatType matType = (water->drawReflection)?
			LUAMAT_OPAQUE_REFLECT : LUAMAT_OPAQUE;
		LuaUnitMaterial& unitMat = unit->luaMats[matType];
		lodMat = unitMat.GetMaterial(unit->currentLOD);
	}

	if (lodMat && lodMat->IsActive()) {
		CUnit::SetLODFactor(LODScale);

		luaMatHandler.setup3doShader = SetupOpaque3DO;
		luaMatHandler.reset3doShader = ResetOpaque3DO;
		luaMatHandler.setupS3oShader = SetupOpaqueS3O;
		luaMatHandler.resetS3oShader = ResetOpaqueS3O;

		const LuaMaterial& mat = (LuaMaterial&) *lodMat->matref.GetBin();

		mat.Execute(LuaMaterial::defMat);

		lodMat->uniforms.Execute(unit);

		SetTeamColour(unit->team);
		DrawUnitRawWithLists(unit, lodMat->preDisplayList, lodMat->postDisplayList);

		LuaMaterial::defMat.Execute(mat);
	}
	else {
		/* 3DO + S3O */
		SetupForUnitDrawing();
		if (unit->model->type == MODELTYPE_3DO) {
			texturehandler3DO->Set3doAtlases();
		} else {
			texturehandlerS3O->SetS3oTexture(unit->model->textureType);
		}

		SetTeamColour(unit->team);
		DrawUnitRaw(unit);
		CleanUpUnitDrawing();
	}

	gu->drawdebug = origDebug;
}


/**
 * Draw one unit,
 * - with depth-buffering(!) and lighting off,
 * - 'tinted' by the current glColor, *including* alpha.
 *
 * Used for drawing building orders.
 *
 * Note: does all the GL state setting for that one unit, so you might want
 * something else for drawing many translucent units.
 */
void CUnitDrawer::DrawBuildingSample(const UnitDef* unitdef, int side, float3 pos, int facing)
{
	S3DModel* model = unitdef->LoadModel();

	/* From SetupForGhostDrawing. */
	glPushAttrib (GL_TEXTURE_BIT | GL_ENABLE_BIT);

	/* *No* GL lighting. */

	/* Get the team-coloured texture constructed by unit 0. */
	SetBasicTeamColour(side);
	SetupBasicS3OTexture0();
	if (model->type == MODELTYPE_3DO) {
		texturehandler3DO->Set3doAtlases();
	} else {
		texturehandlerS3O->SetS3oTexture(model->textureType);
	}
	SetupBasicS3OTexture1();

	/* Use the alpha given by glColor for the outgoing alpha.
	   (Might need to change this if we ever have transparent bits on units?)
	 */
	glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_ALPHA_ARB,GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE0_ALPHA_ARB,GL_PRIMARY_COLOR_ARB);

	glActiveTextureARB(GL_TEXTURE0_ARB);

	/* From SetupForGhostDrawing. */
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE); /* Leave out face culling, as 3DO and 3DO translucents does. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

	/* Push out the polygons. */
	glPushMatrix();
	glTranslatef3(pos);
	glRotatef(facing * 90.0f, 0, 1, 0);

	model->DrawStatic();
	glPopMatrix();

	// reset texture1 state
	CleanupBasicS3OTexture1();

	/* Also reset the alpha generation. */
	glTexEnvi(GL_TEXTURE_ENV,GL_COMBINE_ALPHA_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV,GL_SOURCE0_ALPHA_ARB, GL_TEXTURE);

	// reset texture0 state
	CleanupBasicS3OTexture0();

	/* From CleanUpGhostDrawing. */
	glPopAttrib();
	glDisable(GL_TEXTURE_2D);
	glDepthMask(GL_TRUE);
}



//! used by LuaOpenGL::DrawUnitShape only
void CUnitDrawer::DrawUnitDef(const UnitDef* unitDef, int team)
{
	S3DModel* model = unitDef->LoadModel();

	glPushAttrib (GL_TEXTURE_BIT | GL_ENABLE_BIT);
	glEnable(GL_TEXTURE_2D);

	// get the team-coloured texture constructed by unit 0
	SetBasicTeamColour(team);
	SetupBasicS3OTexture0();
	if (model->type == MODELTYPE_3DO) {
		texturehandler3DO->Set3doAtlases();
	} else {
		texturehandlerS3O->SetS3oTexture(model->textureType);
	}
	// tint it with the current glColor in unit 1
	SetupBasicS3OTexture1();

	// use the alpha given by glColor for the outgoing alpha.
	// (might need to change this if we ever have transparent bits on units?)
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_REPLACE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_PRIMARY_COLOR_ARB);

	glActiveTextureARB(GL_TEXTURE0_ARB);
	model->DrawStatic();

	// reset texture1 state
	CleanupBasicS3OTexture1();

	// also reset the alpha generation
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA_ARB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_ALPHA_ARB, GL_TEXTURE);

	// reset texture0 state
	CleanupBasicS3OTexture0();

	glPopAttrib();
}



void DrawCollisionVolume(const CollisionVolume* vol, GLUquadricObj* q)
{
	switch (vol->GetVolumeType()) {
		case COLVOL_TYPE_FOOTPRINT:
			// fall through, this is too hard to render correctly so just render sphere :)
		case COLVOL_TYPE_SPHERE:
			// fall through, sphere is special case of ellipsoid
		case COLVOL_TYPE_ELLIPSOID: {
			// scaled sphere: radius, slices, stacks
			glTranslatef(vol->GetOffset(0), vol->GetOffset(1), vol->GetOffset(2));
			glScalef(vol->GetHScale(0), vol->GetHScale(1), vol->GetHScale(2));
			gluSphere(q, 1.0f, 20, 20);
		} break;
		case COLVOL_TYPE_CYLINDER: {
			// scaled cylinder: base-radius, top-radius, height, slices, stacks
			//
			// (cylinder base is drawn at unit center by default so add offset
			// by half major axis to visually match the mathematical situation,
			// height of the cylinder equals the unit's full major axis)
			switch (vol->GetPrimaryAxis()) {
				case COLVOL_AXIS_X: {
					glTranslatef(-(vol->GetHScale(0)), 0.0f, 0.0f);
					glTranslatef(vol->GetOffset(0), vol->GetOffset(1), vol->GetOffset(2));
					glScalef(vol->GetScale(0), vol->GetHScale(1), vol->GetHScale(2));
					glRotatef( 90.0f, 0.0f, 1.0f, 0.0f);
				} break;
				case COLVOL_AXIS_Y: {
					glTranslatef(0.0f, -(vol->GetHScale(1)), 0.0f);
					glTranslatef(vol->GetOffset(0), vol->GetOffset(1), vol->GetOffset(2));
					glScalef(vol->GetHScale(0), vol->GetScale(1), vol->GetHScale(2));
					glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);
				} break;
				case COLVOL_AXIS_Z: {
					glTranslatef(0.0f, 0.0f, -(vol->GetHScale(2)));
					glTranslatef(vol->GetOffset(0), vol->GetOffset(1), vol->GetOffset(2));
					glScalef(vol->GetHScale(0), vol->GetHScale(1), vol->GetScale(2));
				} break;
			}

			gluCylinder(q, 1.0f, 1.0f, 1.0f, 20, 20);
		} break;
		case COLVOL_TYPE_BOX: {
			// scaled cube: length, width, height
			glTranslatef(vol->GetOffset(0), vol->GetOffset(1), vol->GetOffset(2));
			glScalef(vol->GetScale(0), vol->GetScale(1), vol->GetScale(2));
			gluMyCube(1.0f);
		} break;
	}
}


void DrawUnitDebugPieceTree(const LocalModelPiece* p, const LocalModelPiece* lap, int lapf, CMatrix44f mat, GLUquadricObj* q)
{
	mat.Translate(p->pos.x, p->pos.y, p->pos.z);
	mat.RotateY(-p->rot[1]);
	mat.RotateX(-p->rot[0]);
	mat.RotateZ(-p->rot[2]);

	glPushMatrix();
		glMultMatrixf(mat.m);

		if (p->visible && !p->colvol->IsDisabled()) {
			if ((p == lap) && (lapf > 0 && ((gs->frameNum - lapf) < 150))) {
				glLineWidth(2.0f);
				glColor3f((1.0f - ((gs->frameNum - lapf) / 150.0f)), 0.0f, 0.0f);
			}

			DrawCollisionVolume(p->colvol, q);

			if ((p == lap) && (lapf > 0 && ((gs->frameNum - lapf) < 150))) {
				glLineWidth(1.0f);
				glColor3f(0.0f, 0.0f, 0.0f);
			}
		}
	glPopMatrix();

	for (unsigned int i = 0; i < p->childs.size(); i++) {
		DrawUnitDebugPieceTree(p->childs[i], lap, lapf, mat, q);
	}
}


inline void CUnitDrawer::DrawUnitDebug(CUnit* unit)
{
	if (gu->drawdebug) {
		if (!shadowHandler->inShadowPass && !water->drawReflection) {
			S3OCurShader->Disable();
		}

		glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT);
			glDisable(GL_LIGHTING);
			glDisable(GL_LIGHT0);
			glDisable(GL_LIGHT1);
			glDisable(GL_CULL_FACE);
			glDisable(GL_TEXTURE_2D);
			glDisable(GL_BLEND);
			glDisable(GL_ALPHA_TEST);
			glDisable(GL_FOG);
			glDisable(GL_CLIP_PLANE0);
			glDisable(GL_CLIP_PLANE1);

			UnitDrawingTexturesOff();

			const float3 midPosOffset =
				(unit->frontdir * unit->relMidPos.z) +
				(unit->updir    * unit->relMidPos.y) +
				(unit->rightdir * unit->relMidPos.x);

			glPushMatrix();
				glTranslatef3(midPosOffset);

				GLUquadricObj* q = gluNewQuadric();

				// draw the aimpoint
				glColor3f(1.0f, 1.0f, 1.0f);
				gluQuadricDrawStyle(q, GLU_FILL);
				gluSphere(q, 2.0f, 20, 20);

				glColor3f(0.0f, 0.0f, 0.0f);
				gluQuadricDrawStyle(q, GLU_LINE);

				if (unit->unitDef->usePieceCollisionVolumes) {
					// draw only the piece volumes for less clutter
					CMatrix44f mat(-midPosOffset);
					DrawUnitDebugPieceTree(unit->localmodel->pieces[0], unit->lastAttackedPiece, unit->lastAttackedPieceFrame, mat, q);
				} else {
					if (!unit->collisionVolume->IsDisabled()) {
						if (unit->lastAttack > 0 && ((gs->frameNum - unit->lastAttack) < 150)) {
							glLineWidth(2.0f);
							glColor3f((1.0f - ((gs->frameNum - unit->lastAttack) / 150.0f)), 0.0f, 0.0f);
						}

						DrawCollisionVolume(unit->collisionVolume, q);

						if (unit->lastAttack > 0 && ((gs->frameNum - unit->lastAttack) < 150)) {
							glLineWidth(1.0f);
							glColor3f(0.0f, 0.0f, 0.0f);
						}
					}
				}

				gluDeleteQuadric(q);
			glPopMatrix();

			UnitDrawingTexturesOn();
		glPopAttrib();

		if (!shadowHandler->inShadowPass && !water->drawReflection) {
			S3OCurShader->Enable();
		}
	}
}



void CUnitDrawer::DrawUnitBeingBuilt(CUnit* unit)
{
	if (shadowHandler->inShadowPass) {
		if (unit->buildProgress > 0.66f) {
			DrawUnitModel(unit);
		}
		return;
	}

	const float start  = std::max(unit->model->miny, -unit->model->height);
	const float height = unit->model->height;

	glEnable(GL_CLIP_PLANE0);
	glEnable(GL_CLIP_PLANE1);

	const float col = fabs(128.0f - ((gs->frameNum * 4) & 255)) / 255.0f + 0.5f;
	const unsigned char* tcol = teamHandler->Team(unit->team)->color;
	// frame line-color
	const float3 fc = (!gu->teamNanospray)?
		unit->unitDef->nanoColor:
		float3(tcol[0] / 255.0f, tcol[1] / 255.0f, tcol[2] / 255.0f);

	glColorf3(fc * col);

	//! render wireframe with FFP
	if (advShading && !water->drawReflection) {
		S3OCurShader->Disable();
	}

	unitDrawer->UnitDrawingTexturesOff();

	// first stage: wireframe model
	//
	// Both clip planes move up. Clip plane 0 is the upper bound of the model,
	// clip plane 1 is the lower bound. In other words, clip plane 0 makes the
	// wireframe/flat color/texture appear, and clip plane 1 then erases the
	// wireframe/flat color laten on.

	const double plane0[4] = {0, -1, 0,  start + height * (unit->buildProgress *      3)};
	const double plane1[4] = {0,  1, 0, -start - height * (unit->buildProgress * 10 - 9)};
	glClipPlane(GL_CLIP_PLANE0, plane0);
	glClipPlane(GL_CLIP_PLANE1, plane1);

	if (!gu->atiHacks) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		DrawUnitModel(unit);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	} else {
		//! some ATi mobility cards/drivers dont like clipping wireframes...
		glDisable(GL_CLIP_PLANE0);
		glDisable(GL_CLIP_PLANE1);

		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
		DrawUnitModel(unit);
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

		glEnable(GL_CLIP_PLANE0);
		glEnable(GL_CLIP_PLANE1);
	}

	// Flat-colored model
	if (unit->buildProgress > 0.33f) {
		glColorf3(fc * (1.5f - col));
		const double plane0[4] = {0, -1, 0,  start + height * (unit->buildProgress * 3 - 1)};
		const double plane1[4] = {0,  1, 0, -start - height * (unit->buildProgress * 3 - 2)};
		glClipPlane(GL_CLIP_PLANE0, plane0);
		glClipPlane(GL_CLIP_PLANE1, plane1);

		DrawUnitModel(unit);
	}

	glDisable(GL_CLIP_PLANE1);
	unitDrawer->UnitDrawingTexturesOn();

	if (advShading && !water->drawReflection) {
		S3OCurShader->Enable();
	}

	// second stage: texture-mapped model
	//
	// XXX FIXME
	// ATI has issues with textures, clip planes and shader programs at once - very low performance
	// FIXME: This may work now I added OPTION ARB_position_invariant to the ARB programs.
	if (unit->buildProgress > 0.66f) {
		if (gu->atiHacks) {
			glDisable(GL_CLIP_PLANE0);

			glPolygonOffset(1.0f, 1.0f);
			glEnable(GL_POLYGON_OFFSET_FILL);
				DrawUnitModel(unit);
			glDisable(GL_POLYGON_OFFSET_FILL);
		} else {
			const double plane0[4] = {0, -1, 0 , start + height * (unit->buildProgress * 3 - 2)};
			glClipPlane(GL_CLIP_PLANE0, plane0);

			glPolygonOffset(1.0f, 1.0f);
			glEnable(GL_POLYGON_OFFSET_FILL);
				DrawUnitModel(unit);
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
	}

	glDisable(GL_CLIP_PLANE0);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}



void CUnitDrawer::ApplyUnitTransformMatrix(CUnit* unit)
{
	const CMatrix44f& m = unit->GetTransformMatrix();
	glMultMatrixf(m);
}


inline void CUnitDrawer::DrawUnitModel(CUnit* unit) {
	if (unit->luaDraw && luaRules && luaRules->DrawUnit(unit->id)) {
		return;
	}

	if (unit->lodCount <= 0) {
		unit->localmodel->Draw();
	} else {
		unit->localmodel->DrawLOD(unit->currentLOD);
	}
}


void CUnitDrawer::DrawUnitNow(CUnit* unit)
{
	/*
	// this interferes with Lua material management
	if (unit->alphaThreshold != 0.1f) {
		glPushAttrib(GL_COLOR_BUFFER_BIT);
		glAlphaFunc(GL_GREATER, unit->alphaThreshold);
	}
	*/

	glPushMatrix();
	ApplyUnitTransformMatrix(unit);

	if (!unit->beingBuilt || !unit->unitDef->showNanoFrame) {
		DrawUnitModel(unit);
	} else {
		DrawUnitBeingBuilt(unit);
	}
#ifndef USE_GML
	DrawUnitDebug(unit);
#endif
	glPopMatrix();

	/*
	if (unit->alphaThreshold != 0.1f) {
		glPopAttrib();
	}
	*/
}


void CUnitDrawer::DrawUnitWithLists(CUnit* unit, unsigned int preList, unsigned int postList)
{
	glPushMatrix();
	ApplyUnitTransformMatrix(unit);

	if (preList != 0) {
		glCallList(preList);
	}

	if (!unit->beingBuilt || !unit->unitDef->showNanoFrame) {
		DrawUnitModel(unit);
	} else {
		DrawUnitBeingBuilt(unit);
	}

	if (postList != 0) {
		glCallList(postList);
	}

#ifndef USE_GML
	DrawUnitDebug(unit);
#endif
	glPopMatrix();
}


void CUnitDrawer::DrawUnitRaw(CUnit* unit)
{
	glPushMatrix();
	ApplyUnitTransformMatrix(unit);
	DrawUnitModel(unit);
	glPopMatrix();
}


void CUnitDrawer::DrawUnitRawModel(CUnit* unit)
{
	if (unit->lodCount <= 0) {
		unit->localmodel->Draw();
	} else {
		unit->localmodel->DrawLOD(unit->currentLOD);
	}
}


void CUnitDrawer::DrawUnitRawWithLists(CUnit* unit, unsigned int preList, unsigned int postList)
{
	glPushMatrix();
	ApplyUnitTransformMatrix(unit);

	if (preList != 0) {
		glCallList(preList);
	}

	DrawUnitModel(unit);

	if (postList != 0) {
		glCallList(postList);
	}

	glPopMatrix();
}


void CUnitDrawer::DrawUnitStats(CUnit* unit)
{
	if ((gu->myAllyTeam != unit->allyteam) &&
	    !gu->spectatingFullView && unit->unitDef->hideDamage) {
		return;
	}

	float3 interPos = unit->drawPos;
	interPos.y += unit->model->height + 5.0f;

	// setup the billboard transformation
	glPushMatrix();
	glTranslatef(interPos.x, interPos.y, interPos.z);
	glCallList(CCamera::billboardList);

	if (unit->health < unit->maxHealth) {
		// black background for healthbar
		glColor3f(0.0f, 0.0f, 0.0f);
		glRectf(-5.0f, 4.0f, +5.0f, 6.0f);

		// healthbar
		const float hpp = std::max(0.0f, unit->health / unit->maxHealth);
		const float hEnd = hpp * 10.0f;

		if (unit->stunned) {
			glColor3f(0.0f, 0.0f, 1.0f);
		} else {
			if (hpp > 0.5f) {
				glColor3f(1.0f - ((hpp - 0.5f) * 2.0f), 1.0f, 0.0f);
			} else {
				glColor3f(1.0f, hpp * 2.0f, 0.0f);
			}
		}

		glRectf(-5.0f, 4.0f, hEnd - 5.0f, 6.0f);
	}

	// stun level
	if (!unit->stunned && (unit->paralyzeDamage > 0.0f)) {
		const float pEnd = (unit->paralyzeDamage / unit->maxHealth) * 10.0f;
		glColor3f(0.0f, 0.0f, 1.0f);
		glRectf(-5.0f, 4.0f, pEnd - 5.0f, 6.0f);
	}

	// skip the rest of the indicators if it isn't a local unit
	if ((gu->myTeam != unit->team) && !gu->spectatingFullView) {
		glPopMatrix();
		return;
	}

	// experience bar
	const float eEnd = (unit->limExperience * 0.8f) * 10.0f;
	glColor3f(1.0f, 1.0f, 1.0f);
	glRectf(6.0f, -2.0f, 8.0f, eEnd - 2.0f);

	if (unit->beingBuilt) {
		const float bEnd = (unit->buildProgress * 0.8f) * 10.0f;
		glColor3f(1.0f, 0.0f, 0.0f);
		glRectf(-8.0f, -2.0f, -6.0f, bEnd - 2.0f);
	}
	else if (unit->stockpileWeapon) {
		const float sEnd = (unit->stockpileWeapon->buildPercent * 0.8f) * 10.0f;
		glColor3f(1.0f, 0.0f, 0.0f);
		glRectf(-8.0f, -2.0f, -6.0f, sEnd - 2.0f);
	}

	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	if (unit->group) {
		font->glFormat(8.0f, 0.0f, 10.0f, FONT_BASELINE, "%i", unit->group->id);
	}

	glPopMatrix();
}

void CUnitDrawer::UpdateDrawPos(CUnit* u) {
	const CTransportUnit* trans = u->GetTransporter();

#if defined(USE_GML) && GML_ENABLE_SIM
	if (trans) {
		u->drawPos = u->pos + (trans->speed * ((float)gu->lastFrameStart - (float)u->lastUnitUpdate) * gu->weightedSpeedFactor);
	} else {
		u->drawPos = u->pos + (u->speed * ((float)gu->lastFrameStart - (float)u->lastUnitUpdate) * gu->weightedSpeedFactor);
	}
#else
	if (trans) {
		u->drawPos = u->pos + (trans->speed * gu->timeOffset);
	} else {
		u->drawPos = u->pos + (u->speed * gu->timeOffset);
	}
#endif
	u->drawMidPos = u->drawPos + (u->midPos - u->pos);
}



void CUnitDrawer::DrawFeatureStatic(CFeature* feature)
{
	glPushMatrix();
	glMultMatrixf(feature->transMatrix.m);

	SetTeamColour(feature->team, feature->tempalpha);

	feature->model->DrawStatic();
	glPopMatrix();
}



bool CUnitDrawer::DrawAsIcon(const CUnit& unit, const float sqUnitCamDist) const {

	const float sqIconDistMult = unit.unitDef->iconType->GetDistanceSqr();
	const float realIconLength = iconLength * sqIconDistMult;
	bool asIcon = false;

	if (useDistToGroundForIcons) {
		asIcon = (sqCamDistToGroundForIcons > realIconLength);
	} else {
		asIcon = (sqUnitCamDist > realIconLength);
	}

	return asIcon;
}




//! visualize if a unit can be built at specified position
int CUnitDrawer::ShowUnitBuildSquare(const BuildInfo& buildInfo)
{
	return ShowUnitBuildSquare(buildInfo, std::vector<Command>());
}

int CUnitDrawer::ShowUnitBuildSquare(const BuildInfo& buildInfo, const std::vector<Command>& commands)
{
	glDisable(GL_DEPTH_TEST );
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_TEXTURE_2D);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	CFeature* feature = NULL;

	std::vector<float3> canbuildpos;
	std::vector<float3> featurepos;
	std::vector<float3> nobuildpos;

	const float3& pos = buildInfo.pos;
	const int x1 = (int) (pos.x - (buildInfo.GetXSize() * 0.5f * SQUARE_SIZE));
	const int x2 = x1 + (buildInfo.GetXSize() * SQUARE_SIZE);
	const int z1 = (int) (pos.z - (buildInfo.GetZSize() * 0.5f * SQUARE_SIZE));
	const int z2 = z1 + (buildInfo.GetZSize() * SQUARE_SIZE);
	const float h = uh->GetBuildHeight(pos, buildInfo.def);

	const int canBuild = uh->TestUnitBuildSquare(
		buildInfo,
		feature,
		-1,
		&canbuildpos,
		&featurepos,
		&nobuildpos,
		&commands
	);

	if (canBuild) {
		glColor4f(0, 0.8f, 0.0f, 1.0f);
	} else {
		glColor4f(0.5f, 0.5f, 0.0f, 1.0f);
	}

	CVertexArray* va = GetVertexArray();
	va->Initialize();
	va->EnlargeArrays(canbuildpos.size() * 4, 0, VA_SIZE_0);

	for (unsigned int i = 0; i < canbuildpos.size(); i++) {
		va->AddVertexQ0(canbuildpos[i]                                      );
		va->AddVertexQ0(canbuildpos[i] + float3(SQUARE_SIZE, 0,           0));
		va->AddVertexQ0(canbuildpos[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE));
		va->AddVertexQ0(canbuildpos[i] + float3(          0, 0, SQUARE_SIZE));
	}
	va->DrawArray0(GL_QUADS);


	glColor4f(0.5f, 0.5f, 0.0f, 1.0f);
	va->Initialize();
	va->EnlargeArrays(featurepos.size() * 4, 0, VA_SIZE_0);

	for (unsigned int i = 0; i < featurepos.size(); i++) {
		va->AddVertexQ0(featurepos[i]                                      );
		va->AddVertexQ0(featurepos[i] + float3(SQUARE_SIZE, 0,           0));
		va->AddVertexQ0(featurepos[i] + float3(SQUARE_SIZE, 0, SQUARE_SIZE));
		va->AddVertexQ0(featurepos[i] + float3(          0, 0, SQUARE_SIZE));
	}
	va->DrawArray0(GL_QUADS);


	glColor4f(0.8f, 0.0f, 0.0f, 1.0f);
	va->Initialize();
	va->EnlargeArrays(nobuildpos.size(), 0, VA_SIZE_0);

	for (unsigned int i = 0; i < nobuildpos.size(); i++) {
		va->AddVertexQ0(nobuildpos[i]);
		va->AddVertexQ0(nobuildpos[i]+float3(SQUARE_SIZE, 0,           0));
		va->AddVertexQ0(nobuildpos[i]+float3(SQUARE_SIZE, 0, SQUARE_SIZE));
		va->AddVertexQ0(nobuildpos[i]+float3(          0, 0, SQUARE_SIZE));
	}
	va->DrawArray0(GL_QUADS);


	if (h < 0.0f) {
		const unsigned char s[4] = { 0,   0, 255, 128 }; // start color
		const unsigned char e[4] = { 0, 128, 255, 255 }; // end color

		va = GetVertexArray();
		va->Initialize();
		va->EnlargeArrays(8, 0, VA_SIZE_C);
		va->AddVertexQC(float3(x1, h, z1), s); va->AddVertexQC(float3(x1, 0.f, z1), e);
		va->AddVertexQC(float3(x1, h, z2), s); va->AddVertexQC(float3(x1, 0.f, z2), e);
		va->AddVertexQC(float3(x2, h, z2), s); va->AddVertexQC(float3(x2, 0.f, z2), e);
		va->AddVertexQC(float3(x2, h, z1), s); va->AddVertexQC(float3(x2, 0.f, z1), e);
		va->DrawArrayC(GL_LINES);

		va->Initialize();
		va->AddVertexQC(float3(x1, 0.0f, z1), e);
		va->AddVertexQC(float3(x1, 0.0f, z2), e);
		va->AddVertexQC(float3(x2, 0.0f, z2), e);
		va->AddVertexQC(float3(x2, 0.0f, z1), e);
		va->DrawArrayC(GL_LINE_LOOP);
	}

	glEnable(GL_DEPTH_TEST );
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	// glDisable(GL_BLEND);

	return canBuild;
}



void CUnitDrawer::UnitCreated(const CUnit* u, const CUnit*) {
	// this MUST block the renderer thread or there will be trouble
	GML_STDMUTEX_LOCK(unit);

	CUnit* unit = const_cast<CUnit*>(u);
	CBuilding* building = dynamic_cast<CBuilding*>(unit);

	if (building != NULL) {
		if (building->unitDef->useBuildingGroundDecal) {
			groundDecals->AddBuilding(building);
		}
	}


	if (u->model) {
		if (u->isCloaked) {
			cloakedModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		} else {
			opaqueModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		}
	}

	unsortedUnits.insert(const_cast<CUnit*>(u));
}

void CUnitDrawer::UnitDestroyed(const CUnit* u, const CUnit*) {
	GML_STDMUTEX_LOCK(unit);

	CUnit* unit = const_cast<CUnit*>(u);
	CBuilding* building = dynamic_cast<CBuilding*>(unit);

	if (building != NULL) {
		GhostBuilding* gb = NULL;

		if (!gameSetup || gameSetup->ghostedBuildings) {
			if (!(building->losStatus[gu->myAllyTeam] & (LOS_INLOS | LOS_CONTRADAR)) &&
				(building->losStatus[gu->myAllyTeam] & (LOS_PREVLOS)) &&
				!gu->spectatingFullView) {

				// FIXME -- adjust decals for decoys? gets weird?
				const UnitDef* decoyDef = building->unitDef->decoyDef;
				S3DModel* gbModel = (decoyDef == NULL) ? building->model : decoyDef->LoadModel();

				gb = new GhostBuilding();
				gb->pos    = building->pos;
				gb->model  = gbModel;
				gb->decal  = building->buildingDecal;
				gb->facing = building->buildFacing;
				gb->team   = building->team;

				ghostBuildings[gbModel->type].push_back(gb);
			}
		}

		if (groundDecals && building->buildingDecal) {
			groundDecals->RemoveBuilding(building, gb);
		}
	}


	if (u->model) {
		if (u->isCloaked) {
			cloakedModelRenderers[MDL_TYPE(u)]->DelUnit(u);
		} else {
			opaqueModelRenderers[MDL_TYPE(u)]->DelUnit(u);
		}
	}

	unsortedUnits.erase(const_cast<CUnit*>(u));


	#if defined(USE_GML)
	for (std::list<CUnit*>::iterator usi = unsortedUnitsGML.begin(); usi != unsortedUnitsGML.end(); ++usi) {
		if (*usi == unit) {
			unsortedUnitsGML.erase(usi);
			break;
		}
	}
	#endif
}


void CUnitDrawer::UnitCloaked(const CUnit* u) {
	GML_STDMUTEX_LOCK(unit);

	if (u->model) {
		cloakedModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		opaqueModelRenderers[MDL_TYPE(u)]->DelUnit(u);
	}
}

void CUnitDrawer::UnitDecloaked(const CUnit* u) {
	GML_STDMUTEX_LOCK(unit);

	if (u->model) {
		opaqueModelRenderers[MDL_TYPE(u)]->AddUnit(u);
		cloakedModelRenderers[MDL_TYPE(u)]->DelUnit(u);
	}
}
