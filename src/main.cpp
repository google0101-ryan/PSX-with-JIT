#include <Application.h>
#include <util/log.h>

#define MODULE "Main"

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		log("Usage: %s <bios>\n", argv[0]);
		return 0;
	}

	Application::Init(argv[1]);
	Application::Run();
}