// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// R3DData.h
//
// The level-of-detail table a viewport points to (in culling RAM): 128
// entries, each four (delete size, blend factor) float pairs, one per LOD.

#ifndef INCLUDED_NEW3D_R3DDATA_H
#define INCLUDED_NEW3D_R3DDATA_H

struct LOD
{
  float deleteSize;   // smallest on-screen size at which this LOD is drawn
  float blendFactor;  // how fast it fades in above that size
};

struct LODFeatureType
{
  LOD lod[4];
};

struct LODBlendTable
{
  LODFeatureType table[128];
};

static_assert(sizeof(LODBlendTable) == 128 * 4 * 8, "LOD table layout is fixed by the hardware");

#endif  // INCLUDED_NEW3D_R3DDATA_H
