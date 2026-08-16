
#pragma once

#include <stddef.h>

namespace ModelSets
{
enum eModelSet
{
	MODEL_SET_CLASSIC = 0,
	MODEL_SET_MODERN,
	MODEL_SET_COUNT
};

// The Modern overlay can be mixed by asset category.  The whole set is still
// selected before RenderWare starts, and category changes likewise require a
// restart; no loaded RenderWare object is ever replaced in place.
enum eModelCategory
{
	MODEL_CATEGORY_WORLD = 0,
	MODEL_CATEGORY_VEGETATION,
	MODEL_CATEGORY_VEHICLES,
	MODEL_CATEGORY_PEDS,
	MODEL_CATEGORY_WEAPONS,
	MODEL_CATEGORY_COUNT
};

// The active set is captured before RenderWare and the game assets start
// loading.  Menu changes only update the requested set for the next process.
void InitializeStartup(const char *gameRoot);
eModelSet GetActive();
eModelSet GetRequested();
void SetRequested(eModelSet modelSet);
void CycleRequested(int direction);
bool IsModernActive();
bool IsRestartRequired();
bool IsAvailable(eModelSet modelSet);
const char *GetName(eModelSet modelSet);
// Describes where files are actually read from. "Classic" is a promise made
// by the installation layout, while the base directory can contain any files.
const char *GetSourceName(eModelSet modelSet);

eModelSet GetActiveForCategory(eModelCategory category);
eModelSet GetRequestedForCategory(eModelCategory category);
void SetRequestedForCategory(eModelCategory category, eModelSet modelSet);
void CycleRequestedCategory(eModelCategory category, int direction);
bool IsCategoryModernActive(eModelCategory category);
bool IsCategoryModernRequested(eModelCategory category);
bool IsCategoryRestartRequired(eModelCategory category);
bool IsCategoryAvailable(eModelCategory category);
const char *GetCategoryName(eModelCategory category);

// Vegetation is intentionally manifest-driven.  Model-name heuristics would
// eventually classify a building or prop incorrectly.  The model-set builder
// emits this manifest from the exact vegetation source directories.
bool HasVegetationManifest();
bool IsVegetationModel(const char *modelName);

// The mixed loader keeps the legal base GTA3 archive open and overlays only
// enabled Modern entries from a second image.  These helpers avoid routing the
// base archive through ResolveAssetPath and identify the overlay image later
// while its directory is being registered.
bool GetModernAssetPath(const char *relativePath, char *resolvedPath,
	size_t resolvedPathSize);
bool IsModernAssetPath(const char *path);

// A Modern install mirrors the original relative layout below
// modelsets\modern.  Missing files deliberately fall through to the classic
// game directory.  gta3.img and gta3.dir are selected only as a complete pair.
const char *ResolveAssetPath(const char *originalPath, char *resolvedPath,
	size_t resolvedPathSize);
}


