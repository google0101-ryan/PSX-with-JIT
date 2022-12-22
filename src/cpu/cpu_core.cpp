#include "cpu_core.h"
#include <cstring>

CPUState g_state;

void Dump()
{
	for (int i = 0; i < 32; i++)
		printf("%s\t->\t0x%08x\n", GetRegName(i), g_state.regs[i]);
	printf("pc\t->\t0x%08x\n", g_state.pc);
	printf("next_pc\t->\t0x%08x\n", g_state.next_pc);
	printf("IsC: %d\n", ((g_state.cop0[12] >> 16) & 1) == 1);
}

CPU::CPU()
{
	g_state.pc = 0xBFC00000;
	g_state.next_pc = g_state.pc + 4;

	memset(g_state.regs, 0, sizeof(g_state.regs));

	recomp = new CPURecompiler();

	std::atexit(Dump);
}

void CPU::Clock(int cycles)
{
	uint32_t pc = g_state.pc;
	uint32_t next_pc = g_state.next_pc;
	for (int i = 0; i < cycles; i++)
	{
		uint32_t opcode = Bus::read<uint32_t>(pc);
		pc = next_pc;
		next_pc += 4;

		bool shouldContinue = recomp->EmitInstruction(opcode);

		if (!shouldContinue)
		{
			// Add one more opcode for branch delay slot
			opcode = Bus::read<uint32_t>(pc);
			pc = next_pc;
			next_pc += 4;
			recomp->EmitInstruction(opcode);
			break;
		}
	}

	auto func = recomp->CompileBlock();
	func();
}