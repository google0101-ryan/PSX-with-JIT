#pragma once

#include <cstdint>
#include <string>
#include <util/log.h>

#define MODULE "Bus"

namespace Bus
{
	inline uint8_t ram[0x200000];
	inline uint8_t bios[0x80000];

	inline uint32_t mask_region(uint32_t addr) {
		constexpr uint32_t region_mask[8] = {
			0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,  // KUSEG: 2048MB, already physaddr, no need to mask
			0x7FFFFFFF,                                      // KSEG0: 512MB, mask top bit
			0x1FFFFFFF,                                      // KSEG1: 512MB, mask top 3 bits
			0xFFFFFFFF, 0xFFFFFFFF                           // KSEG1: 1024MB, already physaddr, no need to mask
		};
		// Use addr's top 3 bits to determine the region and index into region_map
		return addr & region_mask[addr >> 29];
	}
	void Bus(std::string biosFile);

	template<typename T>
	T read(uint32_t addr)
	{
		addr = mask_region(addr);

		if (addr >= 0x1fc00000 && addr < 0x1fc80000)
			return *(T*)&bios[addr - 0x1fc00000];

		panic("Couldn't read from addr 0x%08x\n", addr);
	}

	template<typename T>
	void write(uint32_t addr, T data)
	{
		if (addr >= 0x1f801000 && addr <= 0x1f801020)
			return; // Ignore timing/base address values
		if (addr == 0x1f801060)
			return; // Ignore RAM size val
		if (addr == 0xfffe0130)
			return; // Ignore cache control

		panic("Couldn't write to addr 0x%08x\n", addr);
	}

	inline static void Write32(uint32_t addr, uint32_t data) {write<uint32_t>(addr, data);}
};

#undef MODULE