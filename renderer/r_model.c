/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// r_model.c -- model loading and caching

#include "r_local.h"

model_t	*loadmodel;
int		modfilelen;

void *ModChunk_Begin (size_t maxsize);
void *ModChunk_Alloc (size_t size);
size_t ModChunk_End (void);
void ModChunk_Free (void *base);
size_t Mod_GetAllocSizeSprite ();
void Mod_LoadSpriteModel (model_t *mod, void *buffer);
void Mod_LoadBrushModel (model_t *mod, void *buffer);

#ifdef MD2_AS_MD3
size_t Mod_GetAllocSizeMD2New (void *buffer);
void Mod_LoadAliasMD2ModelNew (model_t *mod, void *buffer);
#else
size_t Mod_GetAllocSizeMD2Old (void *buffer);
void Mod_LoadAliasMD2ModelOld (model_t *mod, void *buffer);
#endif

//Harven++ MD3
size_t Mod_GetAllocSizeMD3 (void *buffer);
void Mod_LoadAliasMD3Model (model_t *mod, void *buffer);
//Harven-- MD3

byte	mod_novis[MAX_MAP_LEAFS/8];

#define	MAX_MOD_KNOWN	(MAX_MODELS * 2) // Knightmare- was 512
model_t	mod_known[MAX_MOD_KNOWN];
int		mod_numknown;

// the inline * models from the current map are kept seperate
model_t	mod_inline[MAX_MOD_KNOWN];

int		registration_sequence;
qboolean	registration_active;	// map registration flag

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *Mod_PointInLeaf (vec3_t p, model_t *model)
{
	if (!model || !model->nodes)
		VID_Error (ERR_DROP, "Mod_PointInLeaf: bad model");

	mnode_t *node = model->nodes;
	while (true)
	{
		if (node->contents != -1)
			return (mleaf_t *)node;

		cplane_t *plane = node->plane;
		const float d = DotProduct(p, plane->normal) - plane->dist;

		if (d > 0)
			node = node->children[0];
		else
			node = node->children[1];
	}
}


/*
===================
Mod_DecompressVis
===================
*/
byte *Mod_DecompressVis (byte *in, model_t *model)
{
	static byte	decompressed[MAX_MAP_LEAFS / 8];

	int row = (model->vis->numclusters + 7) >> 3;
	byte *out = decompressed;

	if (!in)
	{
		// no vis info, so make all visible
		while (row)
		{
			*out++ = 0xff;
			row--;
		}

		return decompressed;
	}

	do
	{
		if (*in)
		{
			*out++ = *in++;
			continue;
		}
	
		int c = in[1];
		in += 2;
		while (c)
		{
			*out++ = 0;
			c--;
		}
	} while (out - decompressed < row);
	
	return decompressed;
}

/*
==============
Mod_ClusterPVS
==============
*/
byte *Mod_ClusterPVS (int cluster, model_t *model)
{
	if (cluster == -1 || !model->vis)
		return mod_novis;

	return Mod_DecompressVis((byte *)model->vis + model->vis->bitofs[cluster][DVIS_PVS], model);
}


//===============================================================================

/*
================
Mod_Modellist_f
================
*/
void Mod_Modellist_f (void)
{
	int		i;
	model_t	*mod;

	int total = 0;
	VID_Printf(PRINT_ALL, "Loaded models:\n");
	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		if (!mod->name[0])
			continue;

		VID_Printf(PRINT_ALL, "%8i : %s\n",mod->extradatasize, mod->name);
		total += mod->extradatasize;
	}

	VID_Printf(PRINT_ALL, "Total resident: %i\n", total);
}

/*
===============
Mod_Init
===============
*/
void Mod_Init (void)
{
	memset(mod_novis, 0xff, sizeof(mod_novis));
	registration_active = false; // map registration flag
}



/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *Mod_ForName (char *name, qboolean crash)
{
	model_t *mod;
	int i;
	
	if (!name[0])
		VID_Error(ERR_DROP, "Mod_ForName: NULL name");
		
	//
	// inline models are grabbed only from worldmodel
	//
	if (name[0] == '*')
	{
		i = atoi(name + 1);
		if (i < 1 || !r_worldmodel || i >= r_worldmodel->numsubmodels)
			VID_Error(ERR_DROP, "bad inline model number");

		return &mod_inline[i];
	}

	//
	// search the currently loaded models
	//
	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		if (!mod->name[0])
			continue;

		if (!strcmp(mod->name, name))
			return mod;
	}
	
	//
	// find a free model slot spot
	//
	for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	{
		if (!mod->name[0])
			break;	// free spot
	}

	if (i == mod_numknown)
	{
		if (mod_numknown == MAX_MOD_KNOWN)
			VID_Error(ERR_DROP, "mod_numknown == MAX_MOD_KNOWN");

		mod_numknown++;
	}

	Q_strncpyz(mod->name, name, sizeof(mod->name));
	
	//
	// load the file
	//
	unsigned *buf;
	modfilelen = FS_LoadFile(mod->name, &buf);

	if (!buf)
	{
		if (crash)
			VID_Error(ERR_DROP, "Mod_NumForName: %s not found", mod->name);

		memset(mod->name, 0, sizeof(mod->name));
		return NULL;
	}
	
	loadmodel = mod;

	//
	// fill it in
	//
	switch (LittleLong(*(unsigned *)buf))
	{
	case IDALIASHEADER:
#ifdef MD2_AS_MD3
		loadmodel->extradata = ModChunk_Begin(Mod_GetAllocSizeMD2New(buf));
		Mod_LoadAliasMD2ModelNew(mod, buf);
#else
		loadmodel->extradata = ModChunk_Begin(Mod_GetAllocSizeMD2Old(buf));
		Mod_LoadAliasMD2ModelOld(mod, buf);
#endif // MD2_AS_MD3
		loadmodel->extradatasize = ModChunk_End();
		break;

	//Harven++ MD3
	case IDMD3HEADER:
		loadmodel->extradata = ModChunk_Begin(Mod_GetAllocSizeMD3(buf));
		Mod_LoadAliasMD3Model(mod, buf);
		loadmodel->extradatasize = ModChunk_End();
		break;
	//Harven-- MD3
	
	case IDSPRITEHEADER:
		loadmodel->extradata = ModChunk_Begin(Mod_GetAllocSizeSprite());
		Mod_LoadSpriteModel(mod, buf);
		loadmodel->extradatasize = ModChunk_End();
		break;
	
	case IDBSPHEADER:
		loadmodel->extradata = Hunk_Begin(0x4000000); //mxd. Was 0x1000000
		Mod_LoadBrushModel(mod, buf);
		loadmodel->extradatasize = Hunk_End();
		break;

	default:
		VID_Error(ERR_DROP, "Mod_NumForName: unknown fileid for %s", mod->name);
		break;
	}

	FS_FreeFile(buf);

	return mod;
}

/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

byte *mod_base;

/*
=================
Mod_LoadLighting
=================
*/
void Mod_LoadLighting (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->lightdata = NULL;
		return;
	}

	loadmodel->lightdata = Hunk_Alloc(l->filelen);
	memcpy(loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
void Mod_LoadVisibility (lump_t *l)
{
	if (!l->filelen)
	{
		loadmodel->vis = NULL;
		return;
	}

	loadmodel->vis = Hunk_Alloc(l->filelen);
	memcpy(loadmodel->vis, mod_base + l->fileofs, l->filelen);

	loadmodel->vis->numclusters = LittleLong(loadmodel->vis->numclusters);
	for (int i = 0; i < loadmodel->vis->numclusters; i++)
	{
		loadmodel->vis->bitofs[i][0] = LittleLong(loadmodel->vis->bitofs[i][0]);
		loadmodel->vis->bitofs[i][1] = LittleLong(loadmodel->vis->bitofs[i][1]);
	}
}


/*
=================
Mod_LoadVertexes
=================
*/
void Mod_LoadVertexes (lump_t *l)
{
	dvertex_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadVertexes: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	mvertex_t *out = Hunk_Alloc ( count*sizeof(*out));

	loadmodel->vertexes = out;
	loadmodel->numvertexes = count;

	for (int i = 0; i < count; i++, in++, out++)
		for(int c = 0; c < 3; c++)
			out->position[c] = LittleFloat(in->point[c]);
}

/*
=================
RadiusFromBounds
=================
*/
float RadiusFromBounds (vec3_t mins, vec3_t maxs)
{
	vec3_t	corner;
	for (int i = 0; i < 3; i++)
		corner[i] = (fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]));

	return VectorLength(corner);
}


/*
=================
Mod_LoadSubmodels
=================
*/
void Mod_LoadSubmodels (lump_t *l)
{
	dmodel_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadSubmodels: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	mmodel_t *out = Hunk_Alloc ( count*sizeof(*out));

	// Knightmare- catch submodel overflow
	if (count >= MAX_MOD_KNOWN)
		VID_Error(ERR_DROP, "Mod_LoadSubmodels: too many submodels (%i >= %i) in %s", count, MAX_MOD_KNOWN, loadmodel->name);

	loadmodel->submodels = out;
	loadmodel->numsubmodels = count;

	for (int i = 0; i < count; i++, in++, out++)
	{
		for (int j = 0; j < 3 ; j++)
		{
			// spread the mins / maxs by a pixel
			out->mins[j] = LittleFloat(in->mins[j]) - 1;
			out->maxs[j] = LittleFloat(in->maxs[j]) + 1;
			out->origin[j] = LittleFloat(in->origin[j]);
		}

		out->radius = RadiusFromBounds(out->mins, out->maxs);
		out->headnode = LittleLong(in->headnode);
		out->firstface = LittleLong(in->firstface);
		out->numfaces = LittleLong(in->numfaces);
	}
}

/*
=================
Mod_LoadEdges
=================
*/
void Mod_LoadEdges (lump_t *l)
{
	dedge_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadEdges: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	medge_t *out = Hunk_Alloc((count + 1) * sizeof(*out));

	loadmodel->edges = out;
	loadmodel->numedges = count;

	for (int i = 0; i < count; i++, in++, out++)
	{
		out->v[0] = (unsigned short)LittleShort(in->v[0]);
		out->v[1] = (unsigned short)LittleShort(in->v[1]);
	}
}

//=======================================================

// store the names of last textures that failed to load
#define NUM_FAIL_TEXTURES 256
char lastFailedTexture[NUM_FAIL_TEXTURES][MAX_OSPATH];
long lastFailedTextureHash[NUM_FAIL_TEXTURES];
static unsigned failedTexListIndex;

/*
===============
Mod_InitFailedTexList
===============
*/
void Mod_InitFailedTexList (void)
{
	for (int i = 0; i < NUM_FAIL_TEXTURES; i++)
	{
		Com_sprintf(lastFailedTexture[i], sizeof(lastFailedTexture[i]), "\0");
		lastFailedTextureHash[i] = 0;
	}

	failedTexListIndex = 0;
}

/*
===============
Mod_CheckTexFailed
===============
*/
qboolean Mod_CheckTexFailed (char *name)
{
	const long hash = Com_HashFileName(name, 0, false);
	for (int i = 0; i < NUM_FAIL_TEXTURES; i++)
	{
		if (hash == lastFailedTextureHash[i]) // compare hash first
		{	
			if (lastFailedTexture[i][0] && !strcmp(name, lastFailedTexture[i]))
				return true; // we already tried to load this image, didn't find it
		}
	}

	return false;
}

/*
===============
Mod_AddToFailedTexList
===============
*/
void Mod_AddToFailedTexList (char *name)
{
	Com_sprintf(lastFailedTexture[failedTexListIndex], sizeof(lastFailedTexture[failedTexListIndex]), "%s", name);
	lastFailedTextureHash[failedTexListIndex] = Com_HashFileName(name, 0, false);
	failedTexListIndex++;

	// wrap around to start of list
	if (failedTexListIndex >= NUM_FAIL_TEXTURES)
		failedTexListIndex = 0;
}

/*
===============
Mod_FindTexture
A wrapper function that uses a failed list to speed map load times
===============
*/
image_t	*Mod_FindTexture (char *name, imagetype_t type)
{
	// don't try again to load a texture that just failed
	if (Mod_CheckTexFailed(name))
		return glMedia.notexture;

	image_t *image = R_FindImage(name, type);

	if (!image || (image == glMedia.notexture))
		Mod_AddToFailedTexList(name);

	return image;
}

//=======================================================

// store the names and sizes of size reference .wal files
typedef struct walsize_s
{
	char	name[MAX_OSPATH];
	long	hash;
	int		width;
	int		height;
} walsize_t;

#define NUM_WALSIZES 256
walsize_t walSizeList[NUM_WALSIZES];
static unsigned walSizeListIndex;

/*
===============
Mod_InitWalSizeList
===============
*/
void Mod_InitWalSizeList (void)
{
	for (int i = 0; i < NUM_WALSIZES; i++)
	{
		Com_sprintf(walSizeList[i].name, sizeof(walSizeList[i].name), "\0");
		walSizeList[i].hash = 0;
		walSizeList[i].width = 0;
		walSizeList[i].height = 0;
	}

	walSizeListIndex = 0;
}

/*
===============
Mod_CheckWalSizeList
===============
*/
qboolean Mod_CheckWalSizeList (const char *name, int *width, int *height)
{
	const long hash = Com_HashFileName(name, 0, false);
	for (int i = 0; i < NUM_WALSIZES; i++)
	{
		if (hash == walSizeList[i].hash) // compare hash first
		{	
			if (strlen(walSizeList[i].name) && !strcmp(name, walSizeList[i].name)) //mxd. Address of array 'walSizeList[i].name' will always evaluate to 'true'
			{
				// return size of texture
				if (width)
					*width = walSizeList[i].width;
				if (height)
					*height = walSizeList[i].height;

				return true;
			}
		}
	}

	return false;
}

/*
===============
Mod_AddToWalSizeList
===============
*/
void Mod_AddToWalSizeList (const char *name, int width, int height)
{
	Com_sprintf(walSizeList[walSizeListIndex].name, sizeof(walSizeList[walSizeListIndex].name), "%s", name);
	walSizeList[walSizeListIndex].hash = Com_HashFileName(name, 0, false);
	walSizeList[walSizeListIndex].width = width;
	walSizeList[walSizeListIndex].height = height;
	walSizeListIndex++;

	// wrap around to start of list
	if (walSizeListIndex >= NUM_WALSIZES)
		walSizeListIndex = 0;
}
/*
=================
Mod_GetWalSize
Adapted from Q2E
=================
*/
static void Mod_GetWalSize (const char *name, int *width, int *height)
{
	char path[MAX_QPATH];
	Com_sprintf(path, sizeof(path), "textures/%s.wal", name);

	if (Mod_CheckWalSizeList(name, width, height)) // check if already in list
		return;

	miptex_t *mt;
	FS_LoadFile(path, (void **)&mt); // load .wal file 
	if (!mt)
	{
		// set null value to tell us to get actual size of texture
		*width = *height = -1;
		Mod_AddToWalSizeList(name, *width, *height); // add to list

		return;
	}

	*width = LittleLong(mt->width); // grab size from wal
	*height = LittleLong(mt->height);
	FS_FreeFile((void *)mt); // free the wal

	Mod_AddToWalSizeList(name, *width, *height); // add to list
}


/*
=================
Mod_LoadTexinfo
=================
*/
void Mod_LoadTexinfo (lump_t *l)
{
	char name[MAX_QPATH];

	texinfo_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadTexinfo: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	mtexinfo_t *out = Hunk_Alloc(count*sizeof(*out));

	loadmodel->texinfo = out;
	loadmodel->numtexinfo = count;

	for (int i = 0; i < count; i++, in++, out++)
	{
		for (int j = 0; j < 8; j++)
			out->vecs[0][j] = LittleFloat(in->vecs[0][j]);

		out->flags = LittleLong(in->flags);
		const int next = LittleLong(in->nexttexinfo);
		if (next > 0)
			out->next = loadmodel->texinfo + next;
		else
			out->next = NULL;

		Com_sprintf(name, sizeof(name), "textures/%s.wal", in->texture);
		out->image = Mod_FindTexture(name, it_wall); // was R_FindImage
		if (!out->image)
			out->image = glMedia.notexture; //mxd. Mod_FindTexture will print a warning message if the texture wasn't found

		// Added glow
		Com_sprintf(name, sizeof(name), "textures/%s_glow.wal", in->texture);
		out->glow = Mod_FindTexture(name, it_skin); // was R_FindImage
		if (!out->glow)
			out->glow = glMedia.notexture;
		
		// Q2E HACK: find .wal dimensions for texture coord generation
		// NOTE: Once Q3 map support is added, be sure to disable this
		// for Q3 format maps, because they will be natively textured with hi-res textures.
		Mod_GetWalSize(in->texture, &out->texWidth, &out->texHeight);

		// If no .wal texture was found, use width & height of actual texture
		if (out->texWidth == -1 || out->texHeight == -1)
		{
			out->texWidth = out->image->width;
			out->texHeight = out->image->height;
		}
	}

	// count animation frames
	for (int i = 0; i < count; i++)
	{
		out = &loadmodel->texinfo[i];
		out->numframes = 1;
		for (mtexinfo_t *step = out->next; step && step != out; step = step->next)
			out->numframes++;
	}
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
void CalcSurfaceExtents (msurface_t *s)
{
	float	mins[2], maxs[2];
	mvertex_t	*v;

	mins[0] = mins[1] = 999999;
	maxs[0] = maxs[1] = -99999;

	mtexinfo_t *tex = s->texinfo;
	
	for (int i = 0; i < s->numedges; i++)
	{
		const int e = loadmodel->surfedges[s->firstedge + i];
		if (e >= 0)
			v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
		else
			v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];
		
		for (int j = 0; j < 2; j++)
		{
			/* The following calculation is sensitive to floating-point precision.  It needs to produce the same result that the
			* light compiler does, because R_BuildLightMap uses surf->extents to know the width/height of a surface's lightmap,
			* and incorrect rounding here manifests itself as patches of "corrupted" looking lightmaps.
			* Most light compilers are win32 executables, so they use x87 floating point.  This means the multiplies and adds
			* are done at 80-bit precision, and the result is rounded down to 32-bits and stored in val.
			* Adding the casts to double seems to be good enough to fix lighting glitches when Quakespasm is compiled as x86_64
			* and using SSE2 floating-point.  A potential trouble spot is the hallway at the beginning of mfxsp17.  -- ericw
			*/
			const float val = (double)v->position[0] * (double)tex->vecs[j][0] +
							  (double)v->position[1] * (double)tex->vecs[j][1] +
							  (double)v->position[2] * (double)tex->vecs[j][2] +
													   (double)tex->vecs[j][3];

			mins[j] = min(val, mins[j]); //mxd
			maxs[j] = max(val, maxs[j]); //mxd
		}
	}

	const int lmscale = 1 << gl_lms.lmshift; //mxd

	for (int i = 0; i < 2; i++)
	{	
		const int bmins = floorf(mins[i] / lmscale); //mxd. 16 -> lmscale
		const int bmaxs =  ceilf(maxs[i] / lmscale);

		s->texturemins[i] = bmins * lmscale;
		s->extents[i] = (bmaxs - bmins) * lmscale;
	}
}

void R_BuildPolygonFromSurface (msurface_t *fa);
void R_CreateSurfaceLightmap (msurface_t *surf);
void R_EndBuildingLightmaps (void);
void R_BeginBuildingLightmaps (model_t *m);

/*
=================
Mod_LoadFaces
=================
*/
void Mod_LoadFaces (lump_t *l)
{
	dface_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadFaces: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	msurface_t *out = Hunk_Alloc(count*sizeof(*out));

	loadmodel->surfaces = out;
	loadmodel->numsurfaces = count;

	currentmodel = loadmodel;

	R_BeginBuildingLightmaps(loadmodel);

	for (int surfnum = 0; surfnum < count; surfnum++, in++, out++)
	{
		out->firstedge = LittleLong(in->firstedge);
		out->numedges = LittleShort(in->numedges);
		out->flags = 0;
		out->polys = NULL;

		const int planenum = (unsigned short)LittleShort(in->planenum);
		const int side = LittleShort(in->side);
		if (side)
			out->flags |= SURF_PLANEBACK;

		out->plane = loadmodel->planes + planenum;

		const int ti = LittleShort(in->texinfo);
		if (ti < 0 || ti >= loadmodel->numtexinfo)
			VID_Error(ERR_DROP, "Mod_LoadFaces: bad texinfo number");

		out->texinfo = loadmodel->texinfo + ti;

		CalcSurfaceExtents(out);

	// lighting info
		for (int i = 0; i < MAXLIGHTMAPS; i++)
			out->styles[i] = in->styles[i];

		const int lightofs = LittleLong(in->lightofs);
		if (lightofs == -1)
			out->samples = NULL;
		else
			out->samples = loadmodel->lightdata + lightofs;
		
	// set the drawing flags
		if (out->texinfo->flags & SURF_WARP)
		{
			out->flags |= SURF_DRAWTURB;
			for (int i = 0; i < 2; i++)
			{
				out->extents[i] = 16384;
				out->texturemins[i] = -8192;
			}

			R_SubdivideSurface(out); // cut up polygon for warps
		}
		// Knightmare- Psychospaz's envmapping. Windows get glass (envmap) effect, warp surfaces don't
		else if (out->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
		{
			if (!(out->texinfo->flags & SURF_NOLIGHTENV))
				out->flags |= SURF_ENVMAP;
		}

	// create lightmaps and polygons
		if (!(out->texinfo->flags & (SURF_SKY | SURF_WARP)))
			R_CreateSurfaceLightmap(out);

		if (!(out->texinfo->flags & SURF_WARP))
			R_BuildPolygonFromSurface(out);
	}

	R_EndBuildingLightmaps();
}


/*
=================
Mod_SetParent
=================
*/
void Mod_SetParent (mnode_t *node, mnode_t *parent)
{
	node->parent = parent;
	if (node->contents == -1)
	{
		Mod_SetParent(node->children[0], node);
		Mod_SetParent(node->children[1], node);
	}
}

/*
=================
Mod_LoadNodes
=================
*/
void Mod_LoadNodes (lump_t *l)
{
	dnode_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadNodes: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	mnode_t *out = Hunk_Alloc(count * sizeof(*out));

	loadmodel->nodes = out;
	loadmodel->numnodes = count;

	for (int i = 0; i < count; i++, in++, out++)
	{
		for (int j = 0; j < 3; j++)
		{
			out->minmaxs[j] = LittleShort(in->mins[j]);
			out->minmaxs[j + 3] = LittleShort(in->maxs[j]);
		}
	
		const int p = LittleLong(in->planenum);
		out->plane = loadmodel->planes + p;

		out->firstsurface = (unsigned short)LittleShort(in->firstface);
		out->numsurfaces =  (unsigned short)LittleShort(in->numfaces);
		out->contents = -1;	// differentiate from leafs

		for (int j = 0; j < 2; j++)
		{
			const int childindex = LittleLong(in->children[j]);
			if (childindex >= 0)
				out->children[j] = loadmodel->nodes + childindex;
			else
				out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - childindex));
		}
	}
	
	Mod_SetParent(loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
void Mod_LoadLeafs (lump_t *l)
{
	dleaf_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadLeafs: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	mleaf_t *out = Hunk_Alloc(count * sizeof(*out));

	loadmodel->leafs = out;
	loadmodel->numleafs = count;

	for (int i = 0; i < count; i++, in++, out++)
	{
		for (int j = 0; j < 3; j++)
		{
			out->minmaxs[j] = LittleShort(in->mins[j]);
			out->minmaxs[j + 3] = LittleShort(in->maxs[j]);
		}

		out->contents = LittleLong(in->contents);
		out->cluster = LittleShort(in->cluster);
		out->area = LittleShort(in->area);

		out->firstmarksurface = loadmodel->marksurfaces + (unsigned short)LittleShort(in->firstleafface); // Knightmare- make sure this doesn't turn negative!
		out->nummarksurfaces = (unsigned short)LittleShort(in->numleaffaces);
		
		// underwater flag for caustics
		if (out->contents & (MASK_WATER))
		{	
			unsigned int flag;
			if (out->contents & CONTENTS_LAVA)
				flag = SURF_UNDERLAVA;
			else if (out->contents & CONTENTS_SLIME)
				flag = SURF_UNDERSLIME;
			else
				flag = SURF_UNDERWATER;

			for (int j = 0; j < out->nummarksurfaces; j++)
			{
				out->firstmarksurface[j]->flags |= flag;
				for (glpoly_t *poly = out->firstmarksurface[j]->polys; poly; poly = poly->next)
					poly->flags |= flag;
			}
		}
	}	
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
void Mod_LoadMarksurfaces (lump_t *l)
{
	unsigned short *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadMarksurfaces: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	msurface_t **out = Hunk_Alloc(count * sizeof(*out));

	loadmodel->marksurfaces = out;
	loadmodel->nummarksurfaces = count;

	for (int i = 0; i < count; i++)
	{
		const int j = (unsigned short)LittleShort(in[i]);
		if (j >= loadmodel->numsurfaces)
			VID_Error(ERR_DROP, "Mod_ParseMarksurfaces: bad surface number");

		out[i] = loadmodel->surfaces + j;
	}
}

/*
=================
Mod_LoadSurfedges
=================
*/
void Mod_LoadSurfedges (lump_t *l)
{
	int *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadSurfedges: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	if (count < 1 || count >= MAX_MAP_SURFEDGES)
		VID_Error(ERR_DROP, "Mod_LoadSurfedges: bad surfedges count in %s: %i", loadmodel->name, count);

	int *out = Hunk_Alloc(count * sizeof(*out));

	loadmodel->surfedges = out;
	loadmodel->numsurfedges = count;

	for (int i = 0; i < count ; i++)
		out[i] = LittleLong(in[i]);
}


/*
=================
Mod_LoadPlanes
=================
*/
void Mod_LoadPlanes (lump_t *l)
{
	dplane_t *in = (void *)(mod_base + l->fileofs);
	if (l->filelen % sizeof(*in))
		VID_Error(ERR_DROP, "Mod_LoadPlanes: funny lump size in %s", loadmodel->name);

	const int count = l->filelen / sizeof(*in);
	cplane_t *out = Hunk_Alloc(count * sizeof(*out));
	
	loadmodel->planes = out;
	loadmodel->numplanes = count;

	for (int i = 0; i < count; i++, in++, out++)
	{
		int bits = 0;
		for (int j = 0; j < 3; j++)
		{
			out->normal[j] = LittleFloat(in->normal[j]);
			if (out->normal[j] < 0)
				bits |= 1 << j;
		}

		out->dist = LittleFloat(in->dist);
		out->type = LittleLong(in->type);
		out->signbits = bits;
	}
}


/*
=================
Mod_ParseWorldspawnKey (https://github.com/Shpoike/Quakespasm/blob/843ba06637951fee6b81958b8c36c718900b752c/quakespasm/Quake/gl_model.c#L1017)
This just quickly scans the worldspawn entity for a single key. Returning both _prefixed and non prefixed keys.
(wantkey argument should not have a _prefix.)
Also, blame Spike!
=================
*/
const char *Mod_ParseWorldspawnKey(lump_t *l, const char *wantkey, char *buffer, size_t sizeofbuffer)
{
	// Get entstring...
	char map_entitystring[MAX_MAP_ENTSTRING];
	if (l->filelen + 1 > sizeof(map_entitystring))
		return NULL;

	memcpy(map_entitystring, mod_base + l->fileofs, l->filelen);
	map_entitystring[l->filelen] = '\0';

	// Try to find wantkey among the first entity props...
	char foundkey[128];
	char *data = map_entitystring;
	char *com_token = COM_Parse(&data);

	if (data && com_token[0] == '{')
	{
		while (true)
		{
			com_token = COM_Parse(&data);

			if (!data || com_token[0] == '}') // error or end of worldspawn
				break;

			if (com_token[0] == '_')
				strcpy(foundkey, com_token + 1);
			else
				strcpy(foundkey, com_token);

			com_token = COM_Parse(&data);

			if (!data) // error
				break; 

			if (!strcmp(wantkey, foundkey))
			{
				Q_strncpyz(buffer, com_token, sizeofbuffer);
				return buffer;
			}
		}
	}

	return NULL;
}


/*
=================
Mod_LoadBrushModel
=================
*/
void Mod_LoadBrushModel (model_t *mod, void *buffer)
{
	loadmodel->type = mod_brush;
	if (loadmodel != mod_known)
		VID_Error(ERR_DROP, "Loaded a brush model after the world");

	dheader_t *header = (dheader_t *)buffer;

	const int version = LittleLong (header->version);
	if (version != BSPVERSION)
		VID_Error(ERR_DROP, "Mod_LoadBrushModel: %s has wrong version number (%i should be %i)", mod->name, version, BSPVERSION);

	// swap all the lumps
	mod_base = (byte *)header;

	for (int i = 0; i < sizeof(dheader_t) / 4; i++)
		((int *)header)[i] = LittleLong(((int *)header)[i]);

	//mxd. Get _lightmap_scale form worldspawn and store it in gl_lms.lmshift...
	char scalebuf[16];
	byte lmshift = 4;
	if (Mod_ParseWorldspawnKey(&header->lumps[LUMP_ENTITIES], "lightmap_scale", scalebuf, sizeof(scalebuf)))
	{
		const int value = atoi(scalebuf);
		if(value > 0 && value != 16)
		{
			for (lmshift = 0; (1 << lmshift) < value && lmshift < 254; lmshift++) {}
			VID_Printf(PRINT_DEVELOPER, "Using custom lightmap scale (%i) and shift (%i)\n", value, lmshift);
		}
	}
	gl_lms.lmshift = lmshift;

	// load into heap
	Mod_LoadVertexes(&header->lumps[LUMP_VERTEXES]);
	Mod_LoadEdges(&header->lumps[LUMP_EDGES]);
	Mod_LoadSurfedges(&header->lumps[LUMP_SURFEDGES]);
	Mod_LoadLighting(&header->lumps[LUMP_LIGHTING]);
	Mod_LoadPlanes(&header->lumps[LUMP_PLANES]);
	Mod_LoadTexinfo(&header->lumps[LUMP_TEXINFO]);
	Mod_LoadFaces(&header->lumps[LUMP_FACES]);
	Mod_LoadMarksurfaces(&header->lumps[LUMP_LEAFFACES]);
	Mod_LoadVisibility(&header->lumps[LUMP_VISIBILITY]);
	Mod_LoadLeafs(&header->lumps[LUMP_LEAFS]);
	Mod_LoadNodes(&header->lumps[LUMP_NODES]);
	Mod_LoadSubmodels(&header->lumps[LUMP_MODELS]);
	mod->numframes = 2; // regular and alternate animation
	
	//
	// set up the submodels
	//
	for (int i = 0; i < mod->numsubmodels; i++)
	{
		mmodel_t *bm = &mod->submodels[i];
		model_t *starmod = &mod_inline[i];

		*starmod = *loadmodel;
		
		starmod->firstmodelsurface = bm->firstface;
		starmod->nummodelsurfaces = bm->numfaces;
		starmod->firstnode = bm->headnode;
		if (starmod->firstnode >= loadmodel->numnodes)
			VID_Error(ERR_DROP, "Inline model %i has bad firstnode", i);

		VectorCopy(bm->maxs, starmod->maxs);
		VectorCopy(bm->mins, starmod->mins);
		starmod->radius = bm->radius;
	
		if (i == 0)
			*loadmodel = *starmod;

		starmod->numleafs = bm->visleafs;
	}
}

/*
===============================================================================

					MODEL MEMORY ALLOCATION

===============================================================================
*/

// It turns out that the Hunk_Begin(), Hunk_Alloc(), and Hunk_Free()
// functions used for BSP, alias model, and sprite loading are a wrapper around
// VirtualAlloc()/VirtualFree() on Win32 and they should only be used
// rarely and for large blocks of memory.  After about 185-190 VirtualAlloc()
// reserve calls are made on Win7 x64, the subsequent call fails.  Bad Carmack, bad!

// These ModChunk_ functions are a replacement that wrap around malloc()/free()
// and return pointers into sections aligned on cache lines.
// The only caveat is that the allocation size passed to ModChunk_Begin() is
// immediately allocated, not reserved.  Calling it with a maximum memory size
// for each model type would be hugely wasteful.  So a size equal or greater to
// the total amount requested via ModChunk_Alloc() calls in the model loading
// functions must be calculated first.

int		modChunkCount;

byte	*modChunkMemBase;
size_t	modChunkMaxSize;
size_t	modChunkCurSize;


void *ModChunk_Begin(size_t maxsize)
{
	// alocate a chunk of memory, should be exact size needed!
	modChunkCurSize = 0;
	modChunkMaxSize = maxsize;

	modChunkMemBase = malloc(maxsize);

	if (!modChunkMemBase)
		Sys_Error("ModChunk_Begin: malloc of size %i failed, %i chunks already allocated", maxsize, modChunkCount);

	memset(modChunkMemBase, 0, maxsize);

	return (void *)modChunkMemBase;
}

void *ModChunk_Alloc(size_t size)
{
	// round to cacheline
	size = (size + 31) & ~31;

	modChunkCurSize += size;
	if (modChunkCurSize > modChunkMaxSize)
		Sys_Error("ModChunk_Alloc: overflow");

	return (void *)(modChunkMemBase + modChunkCurSize - size);
}

size_t ModChunk_End(void)
{
	// free the remaining unused virtual memory
	modChunkCount++;
	return modChunkCurSize;
}

void ModChunk_Free(void *base)
{
	if (base)
		free(base);

	modChunkCount--;
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

/*
=================
Mod_ParseBlendMode
md3 skin protoshaders
=================
*/
GLenum Mod_ParseBlendMode (char *name)
{
	if (!name) return -1;

	if (!Q_strcasecmp(name, "gl_zero"))
		return GL_ZERO;

	if (!Q_strcasecmp(name, "gl_one"))
		return GL_ONE;

	if (!Q_strcasecmp(name, "gl_src_color"))
		return GL_SRC_COLOR;

	if (!Q_strcasecmp(name, "gl_one_minus_src_color"))
		return GL_ONE_MINUS_SRC_COLOR;

	if (!Q_strcasecmp(name, "gl_src_alpha"))
		return GL_SRC_ALPHA;

	if (!Q_strcasecmp(name, "gl_one_minus_src_alpha"))
		return GL_ONE_MINUS_SRC_ALPHA;

	if (!Q_strcasecmp(name, "gl_dst_alpha"))
		return GL_DST_ALPHA;

	if (!Q_strcasecmp(name, "gl_one_minus_dst_alpha"))
		return GL_ONE_MINUS_DST_ALPHA;

	if (!Q_strcasecmp(name, "gl_dst_color"))
		return GL_DST_COLOR;

	if (!Q_strcasecmp(name, "gl_one_minus_dst_color"))
		return GL_ONE_MINUS_DST_COLOR;

	if (!Q_strcasecmp(name, "gl_src_alpha_saturate"))
		return GL_SRC_ALPHA_SATURATE;

	if (!Q_strcasecmp(name, "gl_constant_color"))
		return GL_CONSTANT_COLOR;

	if (!Q_strcasecmp(name, "gl_one_minus_constant_color"))
		return GL_ONE_MINUS_CONSTANT_COLOR;

	if (!Q_strcasecmp(name, "gl_constant_alpha"))
		return GL_CONSTANT_ALPHA;

	if (!Q_strcasecmp(name, "gl_one_minus_constant_alpha"))
		return GL_ONE_MINUS_CONSTANT_ALPHA;
	
	return -1;
}

/*
=================
Mod_GetNextParm
md3 skin protoshaders
=================
*/
qboolean Mod_GetNextParm (char **data, char **output)
{
	if (!*data)
		return false;

	*output = COM_ParseExt(data, false);

	if (!*data)
	{
		VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: EOF without closing brace\n");
		return false;
	}

	if (!*output[0] || *output[0] == '}')
		return false;

	return true;
}

/*
=================
Mod_ParseWaveFunc
md3 skin protoshaders
=================
*/
qboolean Mod_ParseWaveFunc (char **data, waveForm_t *out)
{
	char *tok = NULL;

	if (!*data || !out || !Mod_GetNextParm(data, &tok))
		return false;

	if (!Q_strcasecmp(tok, "sin"))
		*out = WAVEFORM_SIN;
	else if (!Q_strcasecmp(tok, "triangle"))
		*out = WAVEFORM_TRIANGLE;
	else if (!Q_strcasecmp(tok, "square"))
		*out = WAVEFORM_SQUARE;
	else if (!Q_strcasecmp(tok, "sawtooth"))
		*out = WAVEFORM_SAWTOOTH;
	else if (!Q_strcasecmp(tok, "inversesawtooth"))
		*out = WAVEFORM_INVERSESAWTOOTH;
	else if (!Q_strcasecmp(tok, "noise"))
		*out = WAVEFORM_NOISE;
	else
	{
		*out = -1;
		return false;
	}

	return true;
}

/*
=================
Mod_ParseFloat
md3 skin protoshaders
=================
*/
qboolean Mod_ParseFloat (char **data, float *outnum, qboolean normalized)
{
	char *token = NULL;

	if (!*data || !outnum || !Mod_GetNextParm(data, &token))
		return false;
	
	if (normalized)
		*outnum = max(min(atof(token), 1.0f), 0.0f);
	else
		*outnum = atof(token);

	return true;
}

/*
=================
Mod_ParseModelScript
skin protoshaders
=================
*/
void Mod_ParseModelScript (maliasskin_t *skin, char **data, char *dataStart, int dataSize, char *meshname, int skinnum, char *scriptname)
{
	renderparms_t *skinParms = &skin->renderparms;

	// get the opening curly brace
	char *token = COM_ParseExt(data, true);
	if (!*data)
	{
		VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: unexpected EOF in %s.%i in %s\n", meshname, skinnum, scriptname);
		return;
	}

	if (token[0] != '{')
	{
		VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: found %s when expecting { in %s.%i in %s\n", token, meshname, skinnum, scriptname);
		return;
	}

	// go through all the parms
	while (*data < dataStart + dataSize)
	{	
		token = COM_ParseExt(data, true);
		if (!*data)
		{
			VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: EOF in %s.%i in %s without closing brace\n", meshname, skinnum, scriptname);
			break;
		}

		if (token[0] == '}')
			break; // end of skin

		if (!Q_strcasecmp(token, "twosided"))
		{
			skinParms->twosided = true;
		}
		else if (!Q_strcasecmp(token, "alphatest"))
		{
			skinParms->alphatest = true;
			skinParms->noshadow = true; // also noshadow
		}
		else if (!Q_strcasecmp(token, "fullbright"))
		{
			skinParms->fullbright = true;
		}
		else if (!Q_strcasecmp(token, "nodraw"))
		{
			skinParms->nodraw = true;
			skinParms->noshadow = true; // also noshadow
		}
		else if (!Q_strcasecmp(token, "noshadow"))
		{
			skinParms->noshadow = true;
		}
		else if (!Q_strcasecmp(token, "nodiffuse"))
		{
			skinParms->nodiffuse = true;
		}
		else if (!Q_strcasecmp(token, "envmap"))
		{
			if (!Mod_ParseFloat(data, &skinParms->envmap, true))
			{
				VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'envmap' in %s.%i in %s\n", meshname, skinnum, scriptname);
				break;
			}
		}
		else if (!Q_strcasecmp(token, "alpha") || !Q_strcasecmp(token, "trans"))
		{
			if (!Mod_ParseFloat(data, &skinParms->basealpha, true))
			{
				VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'alpha' or 'trans' in %s.%i in %s\n", meshname, skinnum, scriptname);
				break;
			}
		}
		else if (!Q_strcasecmp(token, "tcmod"))
		{
			if (!Mod_GetNextParm(data, &token))
			{
				VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'tcmod' in %s.%i in %s\n", meshname, skinnum, scriptname);
				break;
			}

			if (!Q_strcasecmp(token, "translate"))
			{
				if (!Mod_ParseFloat(data, &skinParms->translate_x, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameters for 'tcmod translate' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				if (!Mod_ParseFloat(data, &skinParms->translate_y, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'tcmod translate' in %s.%i in %s\n", meshname, skinnum, scriptname);
					skinParms->translate_x = 0.0f;
					break;
				}
			}
			else if (!Q_strcasecmp(token, "rotate"))
			{
				if (!Mod_ParseFloat(data, &skinParms->rotate, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'tcmod rotate' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}
			}
			else if (!Q_strcasecmp(token, "scale"))
			{
				if (!Mod_ParseFloat(data, &skinParms->scale_x, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameters for 'tcmod scale' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				if (!Mod_ParseFloat(data, &skinParms->scale_y, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'tcmod scale' in %s.%i in %s\n", meshname, skinnum, scriptname);
					skinParms->scale_x = 1.0f;
					break;
				}
			}
			else if (!Q_strcasecmp(token, "stretch"))
			{
				if (!Mod_ParseWaveFunc(data, &skinParms->stretch.type))
				{
					VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing or invalid waveform for 'tcmod stretch' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				for (int i = 0; i < 4; i++)
				{
					if (!Mod_ParseFloat(data, &skinParms->stretch.params[i], false))
					{
						VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameters for 'tcmod stretch' in %s.%i in %s\n", meshname, skinnum, scriptname);
						break;
					}
				}
			}
			else if (!Q_strcasecmp(token, "turb"))
			{
				// first parm (base) is unused, so just read twice
				for (int i = 0; i < 4; i++)
				{
					if (!Mod_ParseFloat(data, &skinParms->turb.params[i], false))
					{
						VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameters for 'tcmod turb' in %s.%i in %s\n", meshname, skinnum, scriptname);
						break;
					}
				}

				skinParms->turb.type = WAVEFORM_SIN;
			}
			else if (!Q_strcasecmp(token, "scroll"))
			{
				if (!Mod_ParseFloat(data, &skinParms->scroll_x, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameters for 'tcmod scroll' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				if (!Mod_ParseFloat(data, &skinParms->scroll_y, false))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter for 'tcmod scroll' in %s.%i in %s\n", meshname, skinnum, scriptname);
					skinParms->scroll_x = 0.0f;
					break;
				}
			}
			else
			{
				VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: unknown type '%s' for 'tcmod' in %s.%i in %s\n", token, meshname, skinnum, scriptname);
				break;
			}
		}
		else if (!Q_strcasecmp(token, "blendfunc"))
		{
			// parse blend parm
			if (!Mod_GetNextParm(data, &token))
			{
				VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameter(s) for 'blendfunc' in %s.%i in %s\n", meshname, skinnum, scriptname);
				break;
			}
			
			if (!Q_strcasecmp(token, "add"))
			{
				skinParms->blendfunc_src = GL_ONE;
				skinParms->blendfunc_dst = GL_ONE;
			}
			else if (!Q_strcasecmp(token, "filter"))
			{
				skinParms->blendfunc_src = GL_DST_COLOR;
				skinParms->blendfunc_dst = GL_ZERO;
			}
			else if (!Q_strcasecmp(token, "blend"))
			{
				skinParms->blendfunc_src = GL_SRC_ALPHA;
				skinParms->blendfunc_dst = GL_ONE_MINUS_SRC_ALPHA;
			}
			else
			{
				// parse 2nd blend parm	
				char *token2 = NULL;
				if (!Mod_GetNextParm(data, &token2))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing 2nd parameter for 'blendfunc' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				skinParms->blendfunc_src = Mod_ParseBlendMode(token);
				if (skinParms->blendfunc_src == -1)
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: invalid src blend func in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				skinParms->blendfunc_dst = Mod_ParseBlendMode(token2);
				if (skinParms->blendfunc_dst == -1)
				{
					VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: invalid dst blend func in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}
			}

			skinParms->blend = true;
		}
		else if (!Q_strcasecmp(token, "glow"))
		{
			if (!Mod_GetNextParm(data, &token)) // glowname
			{
				VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing image name for 'glow' in %s.%i in %s\n", meshname, skinnum, scriptname);
				break;
			}

			char glowname[MD3_MAX_PATH];
			memcpy(glowname, token, MD3_MAX_PATH);

			if (!Mod_GetNextParm(data, &token)) // type
			{
				VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing 'identity' or 'wave' for 'glow' in %s.%i in %s\n", meshname, skinnum, scriptname);
				break;
			}

			if (!Q_strcasecmp(token, "wave"))
			{
				if (!Mod_ParseWaveFunc(data, &skinParms->glow.type))
				{
					VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing or invalid waveform for 'glow <glowskin> wave' in %s.%i in %s\n", meshname, skinnum, scriptname);
					break;
				}

				for (int i = 0; i < 4; i++)
				{
					if (!Mod_ParseFloat(data, &skinParms->glow.params[i], false))
					{
						VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: missing parameters for 'glow <glowskin> wave' in %s.%i in %s\n", meshname, skinnum, scriptname);
						break;
					}
				}
			}
			else if (!Q_strcasecmp(token, "identity"))
			{
				skinParms->glow.type = -1;
			}
			else // only wave or identity
			{
				VID_Printf (PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: unknown type '%s' for 'glow' in %s.%i in %s\n", token, meshname, skinnum, scriptname);
				break;
			}

			skin->glowimage = R_FindImage(glowname, it_skin);
			if (skin->glowimage)
				memcpy(skin->glowname, glowname, MD3_MAX_PATH);
		}
		else // invalid parameter
		{
			VID_Printf(PRINT_ALL, S_COLOR_YELLOW"Mod_ParseModelScript: unknown parameter '%s' in %s.%i in %s\n", token, meshname, skinnum, scriptname);
			break;
		}
	}
}

/*
=================
Mod_SetRenderParmsDefaults
md3 skin protoshaders
=================
*/
void Mod_SetRenderParmsDefaults (renderparms_t *parms)
{
	parms->twosided = false;
	parms->alphatest = false;
	parms->fullbright = false;
	parms->nodraw = false;
	parms->noshadow = false;
	parms->nodiffuse = false;
	parms->envmap = 0.0f;
	parms->basealpha = 1.0f;
	parms->blend = false;
	parms->blendfunc_src = -1;
	parms->blendfunc_dst = -1;
	parms->glow.type = -1;
	parms->translate_x = 0.0f;
	parms->translate_y = 0.0f;
	parms->rotate = 0.0f;
	parms->scale_x = 1.0f;
	parms->scale_y = 1.0f;
	parms->stretch.type = -1;
	parms->turb.type = -1;
	parms->scroll_x = 0.0f;
	parms->scroll_y = 0.0f;
}

/*
=================
Mod_LoadModelScript
md3 skin protoshaders
=================
*/
void Mod_LoadModelScript (model_t *mod, maliasmodel_t *aliasmod)
{
	char scriptname[MAX_QPATH];
	char *buf;

	// set defaults
	for (int i = 0; i < aliasmod->num_meshes; i++)
	{
		for (int j = 0; j < aliasmod->meshes[i].num_skins; j++)
		{
			Mod_SetRenderParmsDefaults(&aliasmod->meshes[i].skins[j].renderparms);
			aliasmod->meshes[i].skins[j].glowimage = NULL;
		}
	}

	COM_StripExtension(mod->name, scriptname);
	Q_strncatz(scriptname, ".script", sizeof(scriptname));
	const int buf_size = FS_LoadFile(scriptname, &buf);

	if (buf_size < 1)
		return;
	
	for (int i = 0; i < aliasmod->num_meshes; i++)
	{
		for (int j = 0; j < aliasmod->meshes[i].num_skins; j++)
		{
			// search the script file for that meshname
			char *parse_data = buf;  // copy data postion
			while (parse_data < (buf + buf_size))
			{
				char *token = COM_Parse(&parse_data);
				if (!parse_data || !token)
					break;
				
				if (!Q_strncasecmp(token, aliasmod->meshes[i].name, strlen(aliasmod->meshes[i].name)) && atoi(COM_FileExtension(token)) == j)
				{
					// load the parms for that skin
					Mod_ParseModelScript(&aliasmod->meshes[i].skins[j], &parse_data, buf, buf_size, aliasmod->meshes[i].name, j, scriptname);
					break;
				}
			}
		}
	}

	FS_FreeFile(buf);

	// check if model has any alpha surfaces for sorting
	for (int i = 0; i < aliasmod->num_meshes; i++)
		for (int j = 0; j < aliasmod->meshes[i].num_skins; j++)
			if (aliasmod->meshes[i].skins[j].renderparms.blend || aliasmod->meshes[i].skins[j].renderparms.basealpha < 1.0f) //TODO: mxd. Shouldn't this also be done if renderparms.add !=0 ?
				mod->hasAlpha = true;
}

#ifndef MD2_AS_MD3

#ifdef PROJECTION_SHADOWS // projection shadows from BeefQuake R6
/*
=================
Mod_FindMD2TriangleWithEdge
=================
*/
signed int Mod_FindMD2TriangleWithEdge (short p1, short p2, dtriangle_t *ignore, dmdl_t *hdr)
{
	dtriangle_t *tris = (dtriangle_t *)((unsigned char*)hdr + hdr->ofs_tris);
	int i, match, count;

	count = 0;
	match = -1;

	for (i=0; i<hdr->num_tris; i++, tris++)
	{
		if ( (tris->index_xyz[0] == p2 && tris->index_xyz[1] == p1)
			|| (tris->index_xyz[1] == p2 && tris->index_xyz[2] == p1)
			|| (tris->index_xyz[2] == p2 && tris->index_xyz[0] == p1) ) {
			if (tris != ignore)
				match = i;
			count++;
		}
		else if ( (tris->index_xyz[0] == p1 && tris->index_xyz[1] == p2)
			|| (tris->index_xyz[1] == p1 && tris->index_xyz[2] == p2)
			|| (tris->index_xyz[2] == p1 && tris->index_xyz[0] == p2) )
			count++;
	}

	if (count > 2)
		match = -1;
	return match;
}

/*
=================
Mod_BuildMD2TriangleNeighbors
=================
*/
void Mod_BuildMD2TriangleNeighbors (model_t *mod)
{
	dmdl_t		*hdr = (dmdl_t *)mod->extradata;
	dtriangle_t *tris = (dtriangle_t *)((unsigned char*)hdr + hdr->ofs_tris);
	int			i, *n;

	for (i=0, n=mod->edge_tri; i<hdr->num_tris; i++, n+=3, tris++)
	{
		n[0] = Mod_FindMD2TriangleWithEdge(tris->index_xyz[0], tris->index_xyz[1], tris, hdr);
		n[1] = Mod_FindMD2TriangleWithEdge(tris->index_xyz[1], tris->index_xyz[2], tris, hdr);
		n[2] = Mod_FindMD2TriangleWithEdge(tris->index_xyz[2], tris->index_xyz[0], tris, hdr);
	}
}
#endif // end projection shadows from BeefQuake R6


/*
=================
Mod_GetAllocSizeMD2Old
Calc exact alloc size for MD2 in memory
=================
*/
size_t Mod_GetAllocSizeMD2Old (void *buffer)
{
	dmdl_t *pinmodel;
	size_t allocSize;

	pinmodel = (dmd2_t *)buffer;
	allocSize = (LittleLong(pinmodel->ofs_end) + 31) & ~31;
	return allocSize;
}


/*
=================
Mod_LoadAliasMD2ModelOld
=================
*/
void Mod_LoadAliasMD2ModelOld (model_t *mod, void *buffer)
{
	int					i, j;
	dmdl_t				*pinmodel, *pheader;
	dstvert_t			*pinst, *poutst;
	dtriangle_t			*pintri, *pouttri;
	daliasframe_t		*pinframe, *poutframe;
	int					*pincmd, *poutcmd;
	int					version;

	pinmodel = (dmdl_t *)buffer;

	version = LittleLong (pinmodel->version);
	if (version != ALIAS_VERSION)
		VID_Error (ERR_DROP, "%s has wrong version number (%i should be %i)",
				 mod->name, version, ALIAS_VERSION);

	pheader = ModChunk_Alloc(LittleLong(pinmodel->ofs_end));
	
	// byte swap the header fields and sanity check
	for (i=0; i<sizeof(dmdl_t)/4; i++)
		((int *)pheader)[i] = LittleLong (((int *)buffer)[i]);

	if (pheader->skinheight > MAX_LBM_HEIGHT)
		VID_Error (ERR_DROP, "model %s has a skin taller than %d", mod->name,
				   MAX_LBM_HEIGHT);

	if (pheader->num_xyz <= 0)
		VID_Error (ERR_DROP, "model %s has no vertices", mod->name);

	if (pheader->num_xyz > MAX_VERTS)
		VID_Error (ERR_DROP, "model %s has too many vertices", mod->name);

	if (pheader->num_st <= 0)
		VID_Error (ERR_DROP, "model %s has no st vertices", mod->name);

	if (pheader->num_tris <= 0)
		VID_Error (ERR_DROP, "model %s has no triangles", mod->name);

	if (pheader->num_frames <= 0)
		VID_Error (ERR_DROP, "model %s has no frames", mod->name);

	//
	// load base s and t vertices (not used in gl version)
	//
	pinst = (dstvert_t *) ((byte *)pinmodel + pheader->ofs_st);
	poutst = (dstvert_t *) ((byte *)pheader + pheader->ofs_st);

	for (i=0; i<pheader->num_st; i++)
	{
		poutst[i].s = LittleShort (pinst[i].s);
		poutst[i].t = LittleShort (pinst[i].t);
	}

	//
	// load triangle lists
	//
	pintri = (dtriangle_t *) ((byte *)pinmodel + pheader->ofs_tris);
	pouttri = (dtriangle_t *) ((byte *)pheader + pheader->ofs_tris);

	for (i=0; i<pheader->num_tris; i++)
	{
		for (j=0; j<3; j++)
		{
			pouttri[i].index_xyz[j] = LittleShort (pintri[i].index_xyz[j]);
			pouttri[i].index_st[j] = LittleShort (pintri[i].index_st[j]);
		}
	}

	//
	// load the frames
	//
	for (i=0; i<pheader->num_frames; i++)
	{
		pinframe = (daliasframe_t *) ((byte *)pinmodel 
			+ pheader->ofs_frames + i * pheader->framesize);
		poutframe = (daliasframe_t *) ((byte *)pheader 
			+ pheader->ofs_frames + i * pheader->framesize);

		memcpy (poutframe->name, pinframe->name, sizeof(poutframe->name));
		for (j=0 ; j<3 ; j++)
		{
			poutframe->scale[j] = LittleFloat (pinframe->scale[j]);
			poutframe->translate[j] = LittleFloat (pinframe->translate[j]);
		}
		// verts are all 8 bit, so no swapping needed
		memcpy (poutframe->verts, pinframe->verts, 
			pheader->num_xyz*sizeof(dtrivertx_t));
	}

	//
	// load the glcmds
	//
	pincmd = (int *) ((byte *)pinmodel + pheader->ofs_glcmds);
	poutcmd = (int *) ((byte *)pheader + pheader->ofs_glcmds);
	for (i=0; i<pheader->num_glcmds; i++)
		poutcmd[i] = LittleLong (pincmd[i]);

	// register all skins
	memcpy ((char *)pheader + pheader->ofs_skins, (char *)pinmodel + pheader->ofs_skins,
		pheader->num_skins*MAX_SKINNAME);
	for (i=0; i<pheader->num_skins; i++)
	{
		mod->skins[0][i] = R_FindImage ((char *)pheader + pheader->ofs_skins + i*MAX_SKINNAME, it_skin);
	}

	mod->mins[0] = -32;
	mod->mins[1] = -32;
	mod->mins[2] = -32;
	mod->maxs[0] = 32;
	mod->maxs[1] = 32;
	mod->maxs[2] = 32;

#ifdef PROJECTION_SHADOWS // projection shadows from BeefQuake R6
	mod->edge_tri = malloc(sizeof(int) * pheader->num_tris * 3);
	Mod_BuildMD2TriangleNeighbors (mod);
#endif // end projection shadows from BeefQuake R6

	mod->hasAlpha = false;
	//Mod_LoadMD2ModelScript (mod, pheader); // md2 skin scripting

	mod->type = mod_md2;
}
#endif // MD2_AS_MD3


//Harven++ MD3

//
// Some Vic code here not fully used
//

/*
===============
Mod_FindTriangleWithEdge
===============
*/
int Mod_FindTriangleWithEdge (maliasmesh_t *mesh, index_t p1, index_t p2, int ignore)
{
	int		i;
	index_t *indexes;

	int count = 0;
	int match = -1;
	
	for (i = 0, indexes = mesh->indexes; i < mesh->num_tris; i++, indexes += 3)
	{
		if (   (indexes[0] == p2 && indexes[1] == p1)
			|| (indexes[1] == p2 && indexes[2] == p1)
			|| (indexes[2] == p2 && indexes[0] == p1) )
		{
			if (i != ignore)
				match = i;

			count++;
		}
		else if ((indexes[0] == p1 && indexes[1] == p2)
			  || (indexes[1] == p1 && indexes[2] == p2)
			  || (indexes[2] == p1 && indexes[0] == p2))
		{
			count++;
		}
	}

	// detect edges shared by three triangles and make them seams
	if (count > 2)
		match = -1;

	return match;
}

/*
===============
Mod_BuildTriangleNeighbors
===============
*/
void Mod_BuildTriangleNeighbors (maliasmesh_t *mesh)
{
	int		i, *n;
	index_t	*index;

	for (i = 0, n = mesh->trneighbors, index = mesh->indexes; i < mesh->num_tris; i++, n += 3, index += 3)
	{
		n[0] = Mod_FindTriangleWithEdge(mesh, index[0], index[1], i);
		n[1] = Mod_FindTriangleWithEdge(mesh, index[1], index[2], i);
		n[2] = Mod_FindTriangleWithEdge(mesh, index[2], index[0], i);
	}
}


#ifdef MD2_AS_MD3
/*
=================
NormalToLatLong
=================
*/
void NormalToLatLong (const vec3_t normal, byte bytes[2])
{
	if (normal[0] == 0 && normal[1] == 0)
	{
		if (normal[2] > 0)
		{
			// Lattitude = 0, Longitude = 0
			bytes[0] = 0;
			bytes[1] = 0;
		}
		else
		{
			// Lattitude = 0, Longitude = 128
			bytes[0] = 128;
			bytes[1] = 0;
		}
	}
	else
	{
		const int lat = RAD2DEG(atan2(normal[1], normal[0])) * (255.0 / 360.0);
		const int lng = RAD2DEG(acos(normal[2])) * (255.0 / 360.0);

		bytes[0] = lng & 0xff;
		bytes[1] = lat & 0xff;
	}
}

int				*md2IndRemap;
unsigned int	*md2TempIndex;
unsigned int	*md2TempStIndex;

/*
=================
Mod_GetAllocSizeMD2New
Calc exact alloc size for MD2 in memory
=================
*/

size_t Mod_GetAllocSizeMD2New(void *buffer)
{
	dmdl_t *pinmodel = (dmdl_t *)buffer;

	const int numFrames = LittleLong(pinmodel->num_frames);
	const int numTris = LittleLong(pinmodel->num_tris);

	// count unique verts
	int numVerts = 0;
	const int numIndex = numTris * 3;
	dtriangle_t *pintri = (dtriangle_t *)((byte *)pinmodel + LittleLong(pinmodel->ofs_tris));

	//mxd. (re)allocate remap arrays...
	static int remaparraysize = 0;
	const int newremaparraysize = numTris * 3 * sizeof(int);

	if(newremaparraysize > remaparraysize)
	{
		remaparraysize = newremaparraysize;
		
		if (md2IndRemap)
			free(md2IndRemap);
		if (md2TempIndex)
			free(md2TempIndex);
		if (md2TempStIndex)
			free(md2TempStIndex);

		md2IndRemap = malloc(remaparraysize);
		md2TempIndex = malloc(remaparraysize);
		md2TempStIndex = malloc(remaparraysize);
	}

	for (int i = 0; i < numTris; i++)
	{
		for(int c = 0; c < 3; c++)
		{
			md2TempIndex[i * 3 + c] = (unsigned)LittleShort(pintri[i].index_xyz[c]);
			md2TempStIndex[i * 3 + c] = (unsigned)LittleShort(pintri[i].index_st[c]);
		}
	}

	memset(md2IndRemap, -1, remaparraysize);

	for (int i = 0; i < numIndex; i++)
	{
		if (md2IndRemap[i] != -1)
			continue;

		for (int j = 0; j < numIndex; j++)
			if (i != j && md2TempIndex[j] == md2TempIndex[i] && md2TempStIndex[j] == md2TempStIndex[i])
				md2IndRemap[j] = i;
	}

	for (int i = 0; i < numIndex; i++)
		if (md2IndRemap[i] == -1)
			numVerts++;

	const int numSkins = max(LittleLong(pinmodel->num_skins), 1);	// hack because player models have no skin refs

	// calc sizes rounded to cacheline
	const size_t headerSize = (sizeof(maliasmodel_t) + 31) & ~31;
	const size_t meshSize = (sizeof(maliasmesh_t) + 31) & ~31;
	const size_t indexSize = ((sizeof(index_t) * numTris * 3) + 31) & ~31;
	const size_t coordSize = ((sizeof(maliascoord_t) * numVerts) + 31) & ~31;
	const size_t frameSize = ((sizeof(maliasframe_t) * numFrames) + 31) & ~31;
	const size_t vertSize = ((numFrames * numVerts * sizeof(maliasvertex_t)) + 31) & ~31;
	const size_t trNeighborsSize = ((sizeof(int) * numTris * 3) + 31) & ~31;
	const size_t skinSize = ((sizeof(maliasskin_t) * numSkins) + 31) & ~31;

	const size_t allocSize = headerSize + meshSize + indexSize + coordSize + frameSize + vertSize + trNeighborsSize + skinSize;

	return allocSize;
}

/*
=================
Mod_LoadAliasMD2ModelNew
Based on md2 loading code in Q2E 0.40

Mod_GetAllocSizeMD2() must be called first to init index and remap arrays.
=================
*/
void Mod_LoadAliasMD2ModelNew (model_t *mod, void *buffer)
{
	char	 name[MD3_MAX_PATH];

	dmdl_t *pinmodel = (dmdl_t *)buffer;
	maliasmodel_t *poutmodel = ModChunk_Alloc(sizeof(maliasmodel_t));

	// byte swap the header fields and sanity check
	const int version = LittleLong(pinmodel->version);
	if (version != ALIAS_VERSION)
		VID_Error(ERR_DROP, "%s has wrong version number (%i should be %i)", mod->name, version, ALIAS_VERSION);

	poutmodel->num_frames = LittleLong(pinmodel->num_frames);
	if (poutmodel->num_frames <= 0)
		VID_Error(ERR_DROP, "model %s has invalid number of frames (%i)", mod->name, poutmodel->num_frames);
	else if(poutmodel->num_frames > MAX_FRAMES)
		VID_Error(ERR_DROP, "model %s has too many frames (%i, maximum is %i)", mod->name, poutmodel->num_frames, MAX_FRAMES);

	poutmodel->num_tags = 0;
	poutmodel->num_meshes = 1;

	const double skinWidth = 1.0 / (double)LittleLong(pinmodel->skinwidth);
	const double skinHeight = 1.0 / (double)LittleLong(pinmodel->skinheight);

	//
	// load mesh info
	//
	maliasmesh_t *poutmesh = poutmodel->meshes = ModChunk_Alloc(sizeof(maliasmesh_t));

	Com_sprintf(poutmesh->name, sizeof(poutmesh->name), "md2mesh"); // mesh name in script must match this

	poutmesh->num_tris = LittleLong(pinmodel->num_tris);
	if (poutmesh->num_tris <= 0)
		VID_Error(ERR_DROP, "model %s has invalid number of triangles (%i)", mod->name, poutmesh->num_tris);

	poutmesh->num_verts = LittleLong(pinmodel->num_xyz);
	if (poutmesh->num_verts <= 0)
		VID_Error(ERR_DROP, "model %s has invalid number of vertices (%i)", mod->name, poutmesh->num_verts);

	poutmesh->num_skins = LittleLong(pinmodel->num_skins);
	if (poutmesh->num_skins < 0)
		Com_Error(ERR_DROP, "model %s has invalid number of skins (%i)", mod->name, poutmesh->num_skins);
	else if(poutmesh->num_skins > MAX_MD2SKINS)
		Com_Error(ERR_DROP, "model %s has too many skins (%i, maximum is %i)", mod->name, poutmesh->num_skins, MAX_MD2SKINS);

	// Knightmare: DO NOT DELETE!!!  Needed if Mod_GetAllocSizeMD2() is removed!
	/*dtriangle_t *pintri = (dtriangle_t *)((byte *)pinmodel + LittleLong(pinmodel->ofs_tris));
	
	for (int i = 0; i < poutmesh->num_tris; i++)
	{
		for (int c = 0; c < 3; c++)
		{
			tempIndex[i * 3 + c] = (unsigned)LittleShort(pintri[i].index_xyz[c]);
			tempStIndex[i * 3 + c] = (unsigned)LittleShort(pintri[i].index_st[c]);
		}
	}*/

	//
	// build list of unique vertices
	//
	const int numIndices = poutmesh->num_tris * 3;
	int numVertices = 0;

	index_t *poutindex = poutmesh->indexes = ModChunk_Alloc(sizeof(index_t) * poutmesh->num_tris * 3);

	// Knightmare: DO NOT DELETE!!!  Needed if Mod_GetAllocSizeMD2() is removed!
	/*memset(indRemap, -1, MAX_TRIANGLES * 3 * sizeof(int));

	for (int i = 0; i < numIndices; i++)
	{
		if (indRemap[i] != -1)
			continue;

		for (int j = 0; j < numIndices; j++)
		{
			if (j == i)
				continue;

			if (tempIndex[j] == tempIndex[i] && tempStIndex[j] == tempStIndex[i])
				indRemap[j] = i;
		}
	}*/

	//
	// count unique vertices
	//
	for (int i = 0; i < numIndices; i++)
	{
		if (md2IndRemap[i] != -1)
			continue;

		poutindex[i] = numVertices++;
		md2IndRemap[i] = i;
	}

	poutmesh->num_verts = numVertices;

	//
	// remap remaining indices
	//
	for (int i = 0; i < numIndices; i++)
	{
		if (md2IndRemap[i] != i)
			poutindex[i] = poutindex[md2IndRemap[i]];
	}

	//
	// load base S and T vertices
	//
	dstvert_t *pincoord = (dstvert_t *)((byte *)pinmodel + LittleLong(pinmodel->ofs_st));
	maliascoord_t *poutcoord = poutmesh->stcoords = ModChunk_Alloc(sizeof(maliascoord_t) * poutmesh->num_verts);

	for (int i = 0; i < numIndices; i++)
	{
		poutcoord[poutindex[i]].st[0] = (float)(((double)LittleShort(pincoord[md2TempStIndex[md2IndRemap[i]]].s) + 0.5) * skinWidth);
		poutcoord[poutindex[i]].st[1] = (float)(((double)LittleShort(pincoord[md2TempStIndex[md2IndRemap[i]]].t) + 0.5) * skinHeight);
	}

	//
	// load the frames
	//
	maliasframe_t *poutframe = poutmodel->frames = ModChunk_Alloc(sizeof(maliasframe_t) * poutmodel->num_frames);
	maliasvertex_t *poutvert = poutmesh->vertexes = ModChunk_Alloc(poutmodel->num_frames * poutmesh->num_verts * sizeof(maliasvertex_t));

	mod->radius = 0;
	ClearBounds(mod->mins, mod->maxs);

	for (int i = 0; i < poutmodel->num_frames; i++, poutframe++, poutvert += numVertices)
	{
		daliasframe_t *pinframe = (daliasframe_t *)((byte *)pinmodel + LittleLong(pinmodel->ofs_frames) + i * LittleLong(pinmodel->framesize));

		for(int c = 0; c < 3; c++)
		{
			poutframe->scale[c] = LittleFloat(pinframe->scale[c]);
			poutframe->translate[c] = LittleFloat(pinframe->translate[c]);
		}

		VectorCopy(poutframe->translate, poutframe->mins);
		VectorMA(poutframe->translate, 255, poutframe->scale, poutframe->maxs);

		poutframe->radius = RadiusFromBounds(poutframe->mins, poutframe->maxs);

		mod->radius = max(mod->radius, poutframe->radius);
		AddPointToBounds(poutframe->mins, mod->mins, mod->maxs);
		AddPointToBounds(poutframe->maxs, mod->mins, mod->maxs);

		//
		// load the vertexes and normals
		//
		for (int j = 0; j < numIndices; j++)
		{
			for (int c = 0; c < 3; c++)
				poutvert[poutindex[j]].xyz[c] = (short)pinframe->verts[md2TempIndex[md2IndRemap[j]]].v[c];

			poutvert[poutindex[j]].lightnormalindex = pinframe->verts[md2TempIndex[md2IndRemap[j]]].lightnormalindex;

			vec3_t normal;
			for (int c = 0; c < 3; c++)
				normal[c] = r_avertexnormals[poutvert[poutindex[j]].lightnormalindex][c];

			NormalToLatLong(normal, poutvert[poutindex[j]].normal);
		}
	}

	//
	// build triangle neighbors
	//
	poutmesh->trneighbors = ModChunk_Alloc(sizeof(int) * poutmesh->num_tris * 3);
	Mod_BuildTriangleNeighbors(poutmesh);

	//
	// register all skins
	//
	if (poutmesh->num_skins <= 0) // hack for player models with no skin refs
	{
		maliasskin_t *poutskin = poutmesh->skins = ModChunk_Alloc(sizeof(maliasskin_t));
		poutmesh->num_skins = 1;
		Com_sprintf(name, sizeof(name), "players/male/grunt.pcx");
		memcpy(poutskin->name, name, MD3_MAX_PATH);
		Com_sprintf(poutskin->glowname, sizeof(poutskin->glowname), "\0"); // set null glowskin
		mod->skins[0][0] = R_FindImage(name, it_skin);
	}
	else
	{
		maliasskin_t *poutskin = poutmesh->skins = ModChunk_Alloc(sizeof(maliasskin_t) * poutmesh->num_skins);
		for (int i = 0; i < poutmesh->num_skins; i++, poutskin++)
		{
			memcpy(name, ((char *)pinmodel + LittleLong(pinmodel->ofs_skins) + i * MAX_SKINNAME), MD3_MAX_PATH);
			memcpy(poutskin->name, name, MD3_MAX_PATH);
			Com_sprintf(poutskin->glowname, sizeof(poutskin->glowname), "\0"); // set null glowskin
			mod->skins[0][i] = R_FindImage(name, it_skin);
		}
	}

	mod->hasAlpha = false;
	Mod_LoadModelScript(mod, poutmodel); // md3 skin scripting

	mod->type = mod_alias;
}
#endif // MD2_AS_MD3


//
// And here some Vic and some my (Harven)
//

/*
=================
Mod_GetAllocSizeMD3
Calc exact alloc size for MD3 in memory
=================
*/
size_t Mod_GetAllocSizeMD3(void *buffer)
{
	dmd3_t *pinmodel = (dmd3_t *)buffer;
	const int numFrames = LittleLong(pinmodel->num_frames);
	const int numTags = LittleLong(pinmodel->num_tags);
	const int numMeshes = LittleLong(pinmodel->num_meshes);

	// calc sizes rounded to cacheline
	const size_t headerSize = (sizeof(maliasmodel_t) + 31) & ~31;
	const size_t frameSize = ((sizeof(maliasframe_t) * numFrames) + 31) & ~31;
	const size_t tagSize = ((sizeof(maliastag_t) * numFrames * numTags) + 31) & ~31;
	const size_t meshSize = ((sizeof(maliasmesh_t) * numMeshes) + 31) & ~31;

	dmd3mesh_t *pinmesh = (dmd3mesh_t *)((byte *)pinmodel + LittleLong(pinmodel->ofs_meshes));
	size_t skinSize = 0, indexSize = 0, coordSize = 0, vertSize = 0, trNeighborsSize = 0;

	for (int i = 0; i < numMeshes; i++)
	{
		const int numSkins = LittleLong(pinmesh->num_skins);
		const int numTris = LittleLong(pinmesh->num_tris);
		const int numVerts = LittleLong(pinmesh->num_verts);

		skinSize += ((sizeof(maliasskin_t) * numSkins) + 31) & ~31;
		indexSize += ((sizeof(index_t) * numTris * 3) + 31) & ~31;
		coordSize += ((sizeof(maliascoord_t) * numVerts) + 31) & ~31;
		vertSize += ((numFrames * numVerts * sizeof(maliasvertex_t)) + 31) & ~31;
		trNeighborsSize += ((sizeof(int) * numTris * 3) + 31) & ~31;

		pinmesh = (dmd3mesh_t *)((byte *)pinmesh + LittleLong(pinmesh->meshsize));
	}

	const size_t allocSize = headerSize + frameSize + tagSize + meshSize + skinSize + indexSize + coordSize + vertSize + trNeighborsSize;

	return allocSize;
}

/*
=================
Mod_LoadAliasMD3Model
=================
*/
void Mod_LoadAliasMD3Model (model_t *mod, void *buffer)
{
	char name[MD3_MAX_PATH];

	dmd3_t *pinmodel = (dmd3_t *)buffer;
	const int version = LittleLong(pinmodel->version);

	if (version != MD3_ALIAS_VERSION)
		VID_Error(ERR_DROP, "%s has wrong version number (%i should be %i)", mod->name, version, MD3_ALIAS_VERSION);

	maliasmodel_t *poutmodel = ModChunk_Alloc(sizeof(maliasmodel_t));

	// byte swap the header fields and sanity check
	poutmodel->num_frames = LittleLong(pinmodel->num_frames);
	poutmodel->num_tags = LittleLong(pinmodel->num_tags);
	poutmodel->num_meshes = LittleLong(pinmodel->num_meshes);

	if (poutmodel->num_frames <= 0)
		VID_Error(ERR_DROP, "model %s has no frames", mod->name);
	else if(poutmodel->num_frames > MD3_MAX_FRAMES)
		VID_Error(ERR_DROP, "model %s has too many frames (%i, maximum is %i)", mod->name, poutmodel->num_frames, MD3_MAX_FRAMES);

	if (poutmodel->num_tags > MD3_MAX_TAGS)
		VID_Error(ERR_DROP, "model %s has too many tags (%i, maximum is %i)", mod->name, poutmodel->num_tags, MD3_MAX_TAGS);
	else if (poutmodel->num_tags < 0)
		VID_Error(ERR_DROP, "model %s has invalid number of tags", mod->name);

	if (poutmodel->num_meshes <= 0)
		VID_Error(ERR_DROP, "model %s has no meshes", mod->name );
	else if (poutmodel->num_meshes > MD3_MAX_MESHES)
		VID_Error(ERR_DROP, "model %s has too many meshes (%i, maximum is %i)", mod->name, poutmodel->num_meshes, MD3_MAX_MESHES);

	//
	// load the frames
	//
	dmd3frame_t *pinframe = (dmd3frame_t *)((byte *)pinmodel + LittleLong (pinmodel->ofs_frames));
	maliasframe_t *poutframe = poutmodel->frames = ModChunk_Alloc(sizeof(maliasframe_t) * poutmodel->num_frames);

	mod->radius = 0;
	ClearBounds(mod->mins, mod->maxs);

	for (int i = 0; i < poutmodel->num_frames; i++, pinframe++, poutframe++)
	{
		for (int j = 0; j < 3; j++)
		{
			poutframe->mins[j] = LittleFloat(pinframe->mins[j]);
			poutframe->maxs[j] = LittleFloat(pinframe->maxs[j]);
			poutframe->scale[j] = MD3_XYZ_SCALE;
			poutframe->translate[j] = LittleFloat(pinframe->translate[j]);
		}

		poutframe->radius = LittleFloat(pinframe->radius);

		mod->radius = max(mod->radius, poutframe->radius);
		AddPointToBounds(poutframe->mins, mod->mins, mod->maxs);
		AddPointToBounds(poutframe->maxs, mod->mins, mod->maxs);
	}

	//
	// load the tags
	//
	dmd3tag_t *pintag = (dmd3tag_t *)((byte *)pinmodel + LittleLong (pinmodel->ofs_tags));
	maliastag_t *pouttag = poutmodel->tags = ModChunk_Alloc(sizeof(maliastag_t) * poutmodel->num_frames * poutmodel->num_tags);

	for (int i = 0; i < poutmodel->num_frames; i++)
	{
		for (int l = 0; l < poutmodel->num_tags; l++, pintag++, pouttag++)
		{
			memcpy(pouttag->name, pintag->name, MD3_MAX_PATH);
			for (int j = 0; j < 3; j++)
			{
				pouttag->orient.origin[j] = LittleFloat(pintag->orient.origin[j]);

				for(int c = 0; c < 3; c++)
					pouttag->orient.axis[c][j] = LittleFloat(pintag->orient.axis[c][j]);
			}
		}
	}

	//
	// load the meshes
	//
	dmd3mesh_t *pinmesh = (dmd3mesh_t *)((byte *)pinmodel + LittleLong(pinmodel->ofs_meshes));
	maliasmesh_t *poutmesh = poutmodel->meshes = ModChunk_Alloc(sizeof(maliasmesh_t)*poutmodel->num_meshes);

	for (int i = 0; i < poutmodel->num_meshes; i++, poutmesh++)
	{
		memcpy(poutmesh->name, pinmesh->name, MD3_MAX_PATH);

		if (strncmp((const char *)pinmesh->id, "IDP3", 4))
			VID_Error(ERR_DROP, "mesh %s in model %s has wrong id (%i should be %i)", poutmesh->name, mod->name, LittleLong((int)pinmesh->id), IDMD3HEADER);

		poutmesh->num_tris =  LittleLong(pinmesh->num_tris);
		poutmesh->num_skins = LittleLong(pinmesh->num_skins);
		poutmesh->num_verts = LittleLong(pinmesh->num_verts);

		if (poutmesh->num_skins <= 0)
			VID_Error(ERR_DROP, "mesh %i in model %s has no skins", i, mod->name);
		else if (poutmesh->num_skins > MD3_MAX_SHADERS)
			VID_Error(ERR_DROP, "mesh %i in model %s has too many skins (%i, maximum is %i)", i, mod->name, poutmesh->num_skins, MD3_MAX_SHADERS);

		if (poutmesh->num_tris <= 0)
			VID_Error(ERR_DROP, "mesh %i in model %s has no triangles", i, mod->name);

		if (poutmesh->num_verts <= 0)
			VID_Error(ERR_DROP, "mesh %i in model %s has no vertices", i, mod->name);

		//
		// register all skins
		//
		dmd3skin_t *pinskin = (dmd3skin_t *)((byte *)pinmesh + LittleLong(pinmesh->ofs_skins));
		maliasskin_t *poutskin = poutmesh->skins = ModChunk_Alloc(sizeof(maliasskin_t) * poutmesh->num_skins);

		for (int j = 0; j < poutmesh->num_skins; j++, pinskin++, poutskin++ )
		{
			memcpy(name, pinskin->name, MD3_MAX_PATH);

			if (name[1] == 'o') //mxd. Dafuk is that?..
				name[0] = 'm';
			if (name[1] == 'l')
				name[0] = 'p';

			memcpy(poutskin->name, name, MD3_MAX_PATH);
			Com_sprintf(poutskin->glowname, sizeof(poutskin->glowname), "\0"); // set null glowskin
			mod->skins[i][j] = R_FindImage(name, it_skin);
		}

		//
		// load the indexes
		//
		index_t *pinindex = (index_t *)((byte *)pinmesh + LittleLong(pinmesh->ofs_tris));
		index_t *poutindex = poutmesh->indexes = ModChunk_Alloc(sizeof(index_t) * poutmesh->num_tris * 3);

		for (int j = 0; j < poutmesh->num_tris; j++, pinindex += 3, poutindex += 3)
			for (int c = 0; c < 3; c++)
				poutindex[c] = (index_t)LittleLong(pinindex[c]);

		//
		// load the texture coordinates
		//
		dmd3coord_t *pincoord = (dmd3coord_t *)((byte *)pinmesh + LittleLong (pinmesh->ofs_tcs));
		maliascoord_t *poutcoord = poutmesh->stcoords = ModChunk_Alloc(sizeof(maliascoord_t) * poutmesh->num_verts);

		for (int j = 0; j < poutmesh->num_verts; j++, pincoord++, poutcoord++)
		{
			poutcoord->st[0] = LittleFloat(pincoord->st[0]);
			poutcoord->st[1] = LittleFloat(pincoord->st[1]);
		}

		//
		// load the vertexes and normals
		//
		dmd3vertex_t *pinvert = (dmd3vertex_t *)((byte *)pinmesh + LittleLong(pinmesh->ofs_verts));
		maliasvertex_t *poutvert = poutmesh->vertexes = ModChunk_Alloc(poutmodel->num_frames * poutmesh->num_verts * sizeof(maliasvertex_t));

		for (int l = 0; l < poutmodel->num_frames; l++)
		{
			for (int j = 0; j < poutmesh->num_verts; j++, pinvert++, poutvert++ )
			{
				for (int c = 0; c < 3; c++)
					poutvert->xyz[c] = LittleShort(pinvert->point[c]);

				poutvert->normal[0] = (LittleShort(pinvert->norm) >> 0) & 0xff;
				poutvert->normal[1] = (LittleShort(pinvert->norm) >> 8) & 0xff;

				float lat = (pinvert->norm >> 8) & 0xff;
				float lng = (pinvert->norm & 0xff);
				
				lat *= M_PI / 128;
				lng *= M_PI / 128;

				vec3_t normal;
				normal[0] = cosf(lat) * sinf(lng);
				normal[1] = sinf(lat) * sinf(lng);
				normal[2] = cosf(lng);

				// use ye olde quantized normals for shading
				float maxdot = -999999.0;
				int maxdotindex = -1;

				for (int k = 0; k < NUMVERTEXNORMALS; k++)
				{
					const float dot = DotProduct(normal, r_avertexnormals[k]);
					if (dot > maxdot)
					{
						maxdot = dot;
						maxdotindex = k;
					}
				}

				poutvert->lightnormalindex = maxdotindex;
			}
		}

		pinmesh = (dmd3mesh_t *)((byte *)pinmesh + LittleLong(pinmesh->meshsize));

		//
		// build triangle neighbors
		//
		poutmesh->trneighbors = ModChunk_Alloc(sizeof(int) * poutmesh->num_tris * 3);
		Mod_BuildTriangleNeighbors(poutmesh);
	}

	mod->hasAlpha = false;
	Mod_LoadModelScript(mod, poutmodel); // md3 skin scripting

	mod->type = mod_alias;
}
//Harven-- MD3


/*
==============================================================================

SPRITE MODELS

==============================================================================
*/

/*
=================
Mod_GetAllocSizeSprite
Calc exact alloc size for sprite in memory
=================
*/
size_t Mod_GetAllocSizeSprite()
{
	// calc memory allocation size
	return (modfilelen + 31) & ~31;
}

/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel (model_t *mod, void *buffer)
{
	dsprite_t *sprin = (dsprite_t *)buffer;
	dsprite_t *sprout = ModChunk_Alloc(modfilelen);

	sprout->ident = LittleLong(sprin->ident);
	sprout->version = LittleLong(sprin->version);
	sprout->numframes = LittleLong(sprin->numframes);

	if (sprout->version != SPRITE_VERSION)
		VID_Error(ERR_DROP, "%s has wrong version number (%i should be %i)", mod->name, sprout->version, SPRITE_VERSION);

	if (sprout->numframes > MAX_MD2SKINS)
		VID_Error(ERR_DROP, "%s has too many frames (%i > %i)", mod->name, sprout->numframes, MAX_MD2SKINS);

	// byte swap everything
	for (int i = 0; i < sprout->numframes; i++)
	{
		sprout->frames[i].width = LittleLong(sprin->frames[i].width);
		sprout->frames[i].height = LittleLong(sprin->frames[i].height);
		sprout->frames[i].origin_x = LittleLong(sprin->frames[i].origin_x);
		sprout->frames[i].origin_y = LittleLong(sprin->frames[i].origin_y);
		memcpy(sprout->frames[i].name, sprin->frames[i].name, MAX_SKINNAME);
		mod->skins[0][i] = R_FindImage(sprout->frames[i].name, it_sprite);
	}

	mod->type = mod_sprite;
}

//=============================================================================

/*
@@@@@@@@@@@@@@@@@@@@@
R_BeginRegistration

Specifies the model that will be used as the world
@@@@@@@@@@@@@@@@@@@@@
*/
void R_BeginRegistration (char *model)
{
	registration_sequence++;
	r_oldviewcluster = -1;		// force markleafs

	Mod_InitFailedTexList();	// clear failed texture list
	Mod_InitWalSizeList();		// clear wal size list

	char fullname[MAX_QPATH];
	Com_sprintf(fullname, sizeof(fullname), "maps/%s.bsp", model);

	// Explicitly free the old map if different. This guarantees that mod_known[0] is the world map
	cvar_t *flushmap = Cvar_Get("flushmap", "0", 0);
	if (strcmp(mod_known[0].name, fullname) || flushmap->value)
	{
		Mod_Free (&mod_known[0]);
		// clear this on map change (case of different server and autodownloading)
		R_InitFailedImgList();
	}

	r_worldmodel = Mod_ForName(fullname, true);
	r_viewcluster = -1;
	registration_active = true;	// map registration flag
}


/*
@@@@@@@@@@@@@@@@@@@@@
R_RegisterModel
@@@@@@@@@@@@@@@@@@@@@
*/
struct model_s *R_RegisterModel (char *name)
{
	// Knightmare- MD3 autoreplace code
	const int len = strlen(name);
	if (!strcmp(name + len - 4, ".md2")) // look if we have a .md2 file
	{
		char s[128];
		Q_strncpyz(s, name, sizeof(s));
		s[len - 1] = '3';
		model_t *mod = R_RegisterModel(s);

		if (mod)
			return mod;
	}
	// end Knightmare

	model_t *mod = Mod_ForName(name, false);
	if (mod)
	{
		mod->registration_sequence = registration_sequence;

		// register any images used by the models
		if (mod->type == mod_sprite)
		{
			dsprite_t *sprout = (dsprite_t *)mod->extradata;
			for (int i = 0; i < sprout->numframes; i++)
				mod->skins[0][i] = R_FindImage(sprout->frames[i].name, it_sprite);
		}
		else if (mod->type == mod_md2)
		{
			dmdl_t *pheader = (dmdl_t *)mod->extradata;

			for (int i = 0; i < pheader->num_skins; i++)
				mod->skins[0][i] = R_FindImage ((char *)pheader + pheader->ofs_skins + i * MAX_SKINNAME, it_skin);

			mod->numframes = pheader->num_frames;
		}
		// Harven++ MD3
		else if (mod->type == mod_alias)
		{
			maliasmodel_t *pheader3 = (maliasmodel_t *)mod->extradata;

			for (int i = 0; i < pheader3->num_meshes; i++)
			{
				for (int k = 0; k < pheader3->meshes[i].num_skins; k++)
				{
					mod->skins[i][k] = R_FindImage(pheader3->meshes[i].skins[k].name, it_skin);

					if (strlen(pheader3->meshes[i].skins[k].glowname))
						pheader3->meshes[i].skins[k].glowimage = R_FindImage(pheader3->meshes[i].skins[k].glowname, it_skin);
				}
			}

			mod->numframes = pheader3->num_frames;
		}
		// Harven-- MD3
		else if (mod->type == mod_brush)
		{
			for (int i = 0; i < mod->numtexinfo; i++)
			{
				mod->texinfo[i].image->registration_sequence = registration_sequence;
				mod->texinfo[i].glow->registration_sequence = registration_sequence;
			}
		}
	}

	return mod;
}


/*
@@@@@@@@@@@@@@@@@@@@@
R_EndRegistration
@@@@@@@@@@@@@@@@@@@@@
*/
void R_EndRegistration (void)
{
	for (int i = 0; i < mod_numknown; i++)
	{
		model_t *mod = mod_known + i;
		if (mod->name[0] && mod->registration_sequence != registration_sequence)
			Mod_Free(mod); // don't need this model
	}

	R_FreeUnusedImages();

	registration_active = false; // map registration flag
}


//=============================================================================


/*
================
Mod_Free
================
*/
void Mod_Free (model_t *mod)
{
	// Knightmare- only brush models use the hunk system now
	switch (mod->type)
	{
	case mod_brush:
		Hunk_Free(mod->extradata);
		break;

	case mod_alias:
	case mod_sprite:
	default:
		ModChunk_Free(mod->extradata);
		break;
	}

#ifdef PROJECTION_SHADOWS // projection shadows from BeefQuake R6
	if (mod->edge_tri)
		free (mod->edge_tri);
#endif // end projection shadows from BeefQuake R6

	memset(mod, 0, sizeof(*mod));
}

/*
================
Mod_FreeAll
================
*/
void Mod_FreeAll (void)
{
	for (int i = 0; i < mod_numknown; i++)
		if (mod_known[i].extradatasize)
			Mod_Free(&mod_known[i]);
}
