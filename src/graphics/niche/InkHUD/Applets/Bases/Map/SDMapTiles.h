#pragma once

#ifdef MESHTASTIC_INCLUDE_INKHUD

#include <stddef.h>
#include <stdint.h>

#include "MapTileUtils.h"

namespace NicheGraphics::Drivers
{
class EInk;
}

namespace NicheGraphics::InkHUD::SDMapTiles
{

void preload(Drivers::EInk *driver = nullptr);
bool hasZoom(int zoom);
MapTileUtils::Format format();
MapTileUtils::TileView readTile(int zoom, int x, int y);

} // namespace NicheGraphics::InkHUD::SDMapTiles

#endif
