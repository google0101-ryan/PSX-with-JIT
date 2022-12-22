#include <Application.h>
#include <util/log.h>

#include <memory/Bus.h>
#include <cpu/cpu_core.h>

#define MODULE "Application"

CPU* cpu;

bool Application::Init(std::string bios_path)
{
	log("Initializing emulator\n");

	Bus::Bus(bios_path);
	cpu = new CPU();

	return true;
}

void Application::Run()
{
	while (1)
		cpu->Clock(32);
}
