#pragma once

#define WIN32_LEAN_AND_MEAN

#include <vector>

#include <Windows.h>

namespace Gdi
{
	namespace Palette
	{
		PALETTEENTRY* getDefaultPalette();
		std::vector<PALETTEENTRY> getHardwarePalette();
		std::vector<PALETTEENTRY> getSystemPalette();
		void installHooks();
		void setHardwarePalette(PALETTEENTRY* entries);
		void setSystemPalette(PALETTEENTRY* entries, DWORD count, bool forceBackground);
	}
}
