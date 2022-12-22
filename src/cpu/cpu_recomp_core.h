#pragma once

#include <xbyak/xbyak.h>
#include <cpu/cpu_ops.h>

#include <vector>

using HostFunc = void (*)();

class CPURecompiler
{
private:
	uint8_t* base;
	uint8_t* entry; // Keep track of the current block's entry
	uint32_t cur_size;

	std::vector<uint32_t> cur_instrs;

	void* AllocBlock(uint32_t size);

	void EmitPrequel(Xbyak::CodeGenerator& cg);
	void EmitSequel(Xbyak::CodeGenerator& cg);
	void EmitIncPC(Xbyak::CodeGenerator& cg);

	void EmitJ(Xbyak::CodeGenerator& cg); // 0x02
	void EmitBNE(Xbyak::CodeGenerator& cg); // 0x05
	void EmitAddiu(Xbyak::CodeGenerator& cg); // 0x09
	void EmitORI(Xbyak::CodeGenerator& cg); // 0x0D
	void EmitLUI(Xbyak::CodeGenerator& cg); // 0x0F
	void EmitSW(Xbyak::CodeGenerator& cg); // 0x2B

	// Special opcodes
	void EmitOr(Xbyak::CodeGenerator& cg); // 0x25

	// Cop0 opcodes
	void EmitMTC0(Xbyak::CodeGenerator& cg); // 0x04

	typedef struct
	{
		uint8_t* Start;
		HostFunc entry;
		uint32_t guest_addr;
	} CodeBlock;

	std::vector<CodeBlock*> blockCache;

	bool ModifiesPC(uint32_t i);
public:
	CPURecompiler();
	~CPURecompiler();

	bool EmitInstruction(uint32_t opcode);
	HostFunc CompileBlock();
};