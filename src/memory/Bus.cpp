#include <memory/Bus.h>
#include <fstream>

#define MODULE "Bus"

void Bus::Bus(std::string biosFile)
{
	std::ifstream file(biosFile, std::ios::binary | std::ios::ate);

	size_t size = file.tellg();

	if (size < 0x80000)
	{
		panic("Bios is incorrect size!\n");
	}

	file.seekg(0, std::ios::beg);
	file.read((char*)bios, size);
}