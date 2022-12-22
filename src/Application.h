#pragma once

#include <cstdint>
#include <string>

class Application
{
public:
	static bool Init(std::string bios_path);

	static void Run();
};