#pragma once

#include <cstdint>
#include <string>
#include <util/log.h>
#include <cpu/cpu_core.h>
#include <cpu/cpu_recomp_core.h>

#define MODULE "Bus"

namespace Bus
{
	inline CPURecompiler* recomp;

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

		if (addr < 0x00200000)
			return *(T*)&ram[addr];
		if (addr >= 0x1f000000 && addr < 0x1f080000)
			return 0xff;
		if (addr >= 0x1fc00000 && addr < 0x1fc80000)
			return *(T*)&bios[addr - 0x1fc00000];

		panic("Couldn't read from addr 0x%08x\n", addr);
	}

	template<typename T>
	void write(uint32_t addr, T data)
	{
		addr = mask_region(addr);

		recomp->MarkBlockDirty(addr);

		if (addr == 0xC0)
		{
			printf("Writing 0x%08x to 0x%08x (0x%08x)\n", data, addr, recomp->GetPC());
		}

		if (addr < 0x00200000)
		{
			*(T*)&ram[addr] = data;
			return;
		}

		if (addr >= 0x1f801000 && addr <= 0x1f801020)
			return; // Ignore timing/base address values
		if (addr == 0x1f801060)
			return; // Ignore RAM size val
		if (addr >= 0x1f801c00 && addr <= 0x1F801D7E)
			return; // Ignore the voices
		if (addr >= 0x1f801d80 && addr <= 0x1f801dbc)
			return; // Ignore SPU control registers
		if (addr == 0xfffe0130)
			return; // Ignore cache control

		switch (addr)
		{
		case 0x1f802041:
			printf("TraceStep(0x%x)\n", data);
			return;
		}

		panic("Couldn't write to addr 0x%08x\n", addr);
	}

	inline static void Write32(uint32_t addr, uint32_t data) {write<uint32_t>(addr, data);}
	inline static void Write16(uint32_t addr, uint16_t data) {write<uint16_t>(addr, data);}
	inline static void Write8(uint32_t addr, uint8_t data) {write<uint8_t>(addr, data);}
	inline static uint32_t Read32(uint32_t addr) {return read<uint32_t>(addr);}
	inline static uint8_t Read8(uint32_t addr) {return read<uint8_t>(addr);}
};

#undef MODULE
