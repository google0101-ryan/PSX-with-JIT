#include <cpu/cpu_recomp_core.h>
#include <cpu/cpu_core.h>

#include <fstream>

#ifdef __linux__
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#include "cpu_recomp_core.h"
#else
#error Please use Windows or Linux
#endif

// Describe an allocated memory block
struct MemBlock
{
	uint32_t size;
	bool free;
	MemBlock* next;
	MemBlock* prev;
	uint32_t magic;
};

#define MEMBLOCK_MAGIC 0x4D454D42

void* CPURecompiler::AllocBlock(uint32_t size)
{
	MemBlock* b = (MemBlock*)base;

	for (; b; b = b->next)
	{
		if (b->free == true && b->size >= size)
		{
			if (b->size >= (size + sizeof(MemBlock) + 4))
			{
				MemBlock* nextBlock = (MemBlock*)(b + size + sizeof(MemBlock));

				nextBlock->free = true;
				nextBlock->size = b->size - size - sizeof(MemBlock);
				nextBlock->prev = b;
				nextBlock->next = b->next;
				if (b->next)
					b->next->prev = nextBlock;
				
				b->next = nextBlock;
			}

			b->free = false;
			b->size = size;
			b->magic = MEMBLOCK_MAGIC;

			return (void*)(b + sizeof(MemBlock));
		}
	}

 	printf("ERROR allocating memory for size %d\n", size);
	exit(1);
}

void CPURecompiler::FreeBlock(void *ptr)
{
	MemBlock* block = (MemBlock*)(ptr - sizeof(MemBlock));

	if (block->free || (block->magic != MEMBLOCK_MAGIC))
	{
		printf("ERROR: Double free detected or invalid ptr\n");
		printf("Block = %lx, Free = %d, Size = %d\n", (uint64_t)block, block->free, block->size);
		exit(1);
	}

	block->free = true;

	if (block->prev && block->prev->free)
	{
		block->prev->next = block->next;
		block->prev->size += block->size + sizeof(MemBlock);

		if (block->next)
			block->next->prev = block->prev;
		
		block = block->prev;
	}

	if (block->next && block->next->free)
	{
		block->size += block->next->size + sizeof(MemBlock);

		block->next = block->next->next;

		if (block->next)
			block->next->prev = block;
	}
}

CPURecompiler::CPURecompiler()
{
#ifdef __linux__
	base = (uint8_t*)mmap(nullptr, 0xffffffff, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (!base)
	{
		printf("ERROR: Couldn't allocate memory for JIT\n");
		exit(1);
	}
#elif defined(_WIN32)
	base = VirtualAlloc(nullptr, 0xffffffff, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

	if (!base)
	{
		printf("ERROR: Couldn't allocate memory for JIT\n");
		exit(1);
	}
#endif

	MemBlock* block = (MemBlock*)base;
	block->free = true;
	block->next = nullptr;
	block->prev = nullptr;
	block->size = 0xffffffff - sizeof(MemBlock);

	cur_size = 25;
}

CPURecompiler::~CPURecompiler()
{
#ifdef __linux__
	if (base)
		munmap(base, 0xffffffff);
#endif
}

struct Opcode
{
    union
    {
        uint32_t full;
        struct
        { /* Used when polling for the opcode */
            uint32_t : 26;
            uint32_t opcode : 6;
        };
        struct
        {
            uint32_t imm : 16;
            uint32_t rt : 5;
            uint32_t rs : 5;
            uint32_t opcode : 6;
        } i_type;
        struct
        {
            uint32_t target : 26;
            uint32_t opcode : 6;
        } j_type;
        struct
        {
            uint32_t func : 6;
            uint32_t sa : 5;
            uint32_t rd : 5;
            uint32_t rt : 5;
            uint32_t rs : 5;
            uint32_t opcode : 6;
        } r_type;
    };
} cur_instr;

void CPURecompiler::EmitPrequel(Xbyak::CodeGenerator& cg)
{
	cg.push(cg.rax);
	cg.push(cg.rbx);
	cg.push(cg.rcx);
	cg.push(cg.rdx);
	cg.push(cg.rdi);
	cg.push(cg.rsi);
	cg.push(cg.rbp);
	
	cg.mov(cg.rbp, reinterpret_cast<size_t>(reinterpret_cast<const void*>(&g_state)));
}

void CPURecompiler::EmitSequel(Xbyak::CodeGenerator &cg)
{
	cg.pop(cg.rbp);
	cg.pop(cg.rsi);
	cg.pop(cg.rdi);
	cg.pop(cg.rdx);
	cg.pop(cg.rcx);
	cg.pop(cg.rbx);
	cg.pop(cg.rax);

	cg.ret();
}

void CPURecompiler::EmitIncPC(Xbyak::CodeGenerator &cg)
{
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, pc)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
	
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.add(cg.dword[cg.rax], 4);
}

void CPURecompiler::EmitHandleLoadDelay(Xbyak::CodeGenerator &cg)
{
	cg.mov(cg.rax, reinterpret_cast<uint64_t>(HandleLoadDelay));
	cg.call(cg.rax);
}

void CPURecompiler::EmitJ(Xbyak::CodeGenerator &cg)
{
	printf("j 0x%08x\n", (g_state.next_pc & 0xf0000000) | (cur_instr.j_type.target << 2));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.and_(cg.ebx, 0xf0000000);
	cg.mov(cg.ecx, cur_instr.j_type.target << 2);
	cg.or_(cg.ebx, cg.ecx);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitJAL(Xbyak::CodeGenerator &cg)
{
	printf("jal 0x%08x (0x%08x)\n", (g_state.next_pc & 0xf0000000) | (cur_instr.j_type.target << 2), g_state.pc);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (31 * 4)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.and_(cg.ebx, 0xf0000000);
	cg.mov(cg.ecx, cur_instr.j_type.target << 2);
	cg.or_(cg.ebx, cg.ecx);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitBEQ(Xbyak::CodeGenerator &cg)
{
	printf("bne %s, %s, 0x%08x\n", GetRegName(cur_instr.i_type.rt),  GetRegName(cur_instr.i_type.rs), (g_state.next_pc + (int32_t)(cur_instr.i_type.imm << 2)));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * sizeof(uint32_t))]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * sizeof(uint32_t))]);
	cg.mov(cg.ecx, cg.dword[cg.rax]);
	
	cg.cmp(cg.ecx, cg.ebx);

	Xbyak::Label not_equal;
	cg.jne(not_equal);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.add(cg.ebx, (int32_t)(int16_t)(cur_instr.i_type.imm << 2));
	cg.mov(cg.dword[cg.rax], cg.ebx);

	cg.L(not_equal);
}

void CPURecompiler::EmitBNE(Xbyak::CodeGenerator &cg)
{
	printf("bne %s, %s, 0x%08x\n", GetRegName(cur_instr.i_type.rt),  GetRegName(cur_instr.i_type.rs), (g_state.next_pc + (int32_t)(cur_instr.i_type.imm << 2)));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * sizeof(uint32_t))]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * sizeof(uint32_t))]);
	cg.mov(cg.ecx, cg.dword[cg.rax]);
	
	cg.cmp(cg.ecx, cg.ebx);

	Xbyak::Label equal;
	cg.je(equal);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.add(cg.ebx, (int32_t)(int16_t)(cur_instr.i_type.imm << 2));
	cg.mov(cg.dword[cg.rax], cg.ebx);

	cg.L(equal);
}

void CPURecompiler::EmitAddiu(Xbyak::CodeGenerator &cg)
{
	printf("%s %s, %s, 0x%04x\n", cur_instr.opcode == 0x08 ? "addi" : "addiu", GetRegName(cur_instr.i_type.rt), GetRegName(cur_instr.i_type.rs), (int32_t)(int16_t)cur_instr.i_type.imm);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * sizeof(uint32_t))]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.mov(cg.ecx, static_cast<int32_t>(static_cast<int16_t>(cur_instr.i_type.imm)));
	cg.add(cg.ebx, cg.ecx);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * sizeof(uint32_t))]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitLUI(Xbyak::CodeGenerator &cg)
{
	printf("lui %s, 0x%04x\n", GetRegName(cur_instr.i_type.rt), cur_instr.i_type.imm);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
	cg.mov(cg.dword[cg.rax], static_cast<uint32_t>(cur_instr.i_type.imm << 16));
}

void CPURecompiler::EmitLB(Xbyak::CodeGenerator &cg)
{
	printf("lb %s, %d(%s)\n", GetRegName(cur_instr.i_type.rt), cur_instr.i_type.imm, GetRegName(cur_instr.i_type.rs));

	// Grab the register and add the offset to it
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.edi, cg.dword[cg.rax]);
	cg.add(cg.edi, (int32_t)(int16_t)cur_instr.i_type.imm);

	// Call Read32 with the address in edi
	cg.mov(cg.rax, reinterpret_cast<uint64_t>(Bus::Read8));
	cg.call(cg.rax);

	// Save the result
	cg.mov(cg.rbx, cg.rax);
	
	cg.push(cg.rbp);

	// Set up load delay slot
	cg.mov(cg.rbp, reinterpret_cast<uint64_t>(&next_load_delay));
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(LoadDelaySlot, reg)]);
	cg.mov(cg.dword[cg.rax], cur_instr.i_type.rt);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(LoadDelaySlot, data)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);

	// Restore rbp
	cg.pop(cg.rbp);
}

void CPURecompiler::EmitLW(Xbyak::CodeGenerator &cg)
{
	printf("lw %s, %d(%s)\n", GetRegName(cur_instr.i_type.rt), cur_instr.i_type.imm, GetRegName(cur_instr.i_type.rs));

	// Grab the register and add the offset to it
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.edi, cg.dword[cg.rax]);
	cg.add(cg.edi, (int32_t)(int16_t)cur_instr.i_type.imm);

	// Call Read32 with the address in edi
	cg.mov(cg.rax, reinterpret_cast<uint64_t>(Bus::Read32));
	cg.call(cg.rax);

	// Save the result
	cg.mov(cg.rbx, cg.rax);
	
	cg.push(cg.rbp);

	// Set up load delay slot
	cg.mov(cg.rbp, reinterpret_cast<uint64_t>(&next_load_delay));
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(LoadDelaySlot, reg)]);
	cg.mov(cg.dword[cg.rax], cur_instr.i_type.rt);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(LoadDelaySlot, data)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);

	// Restore rbp
	cg.pop(cg.rbp);
}

void CPURecompiler::EmitSB(Xbyak::CodeGenerator &cg)
{
	printf("sb %s, %d(%s)\n", GetRegName(cur_instr.i_type.rt), cur_instr.i_type.imm, GetRegName(cur_instr.i_type.rs));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.add(cg.ebx, (int32_t)((int16_t)cur_instr.i_type.imm));
	cg.mov(cg.rdi, cg.rbx);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
	cg.mov(cg.esi, cg.dword[cg.rax]);

	Xbyak::Label skip_cache;
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, cop0) + (12*4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.and_(cg.ebx, (1 << 16));
	cg.jnz(skip_cache);

	cg.mov(cg.rax, reinterpret_cast<size_t>(Bus::Write8));
	cg.call(cg.rax);

	cg.L(skip_cache);
}

void CPURecompiler::EmitSH(Xbyak::CodeGenerator &cg)
{
	printf("sh %s, %d(%s)\n", GetRegName(cur_instr.i_type.rt), cur_instr.i_type.imm, GetRegName(cur_instr.i_type.rs));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.add(cg.ebx, (int32_t)((int16_t)cur_instr.i_type.imm));
	cg.mov(cg.rdi, cg.rbx);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
	cg.mov(cg.esi, cg.dword[cg.rax]);

	Xbyak::Label skip_cache;
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, cop0) + (12*4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.and_(cg.ebx, (1 << 16));
	cg.jnz(skip_cache);

	cg.mov(cg.rax, reinterpret_cast<size_t>(Bus::Write16));
	cg.call(cg.rax);

	cg.L(skip_cache);
}

void CPURecompiler::EmitSW(Xbyak::CodeGenerator &cg)
{
	printf("sw %s, %d(%s)\n", GetRegName(cur_instr.i_type.rt), cur_instr.i_type.imm, GetRegName(cur_instr.i_type.rs));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.add(cg.ebx, (int32_t)((int16_t)cur_instr.i_type.imm));
	cg.mov(cg.rdi, cg.rbx);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
	cg.mov(cg.esi, cg.dword[cg.rax]);

	Xbyak::Label skip_cache;
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, cop0) + (12*4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.and_(cg.ebx, (1 << 16));
	cg.jnz(skip_cache);

	cg.mov(cg.rax, reinterpret_cast<size_t>(Bus::Write32));
	cg.call(cg.rax);

	cg.L(skip_cache);
}

void CPURecompiler::EmitANDI(Xbyak::CodeGenerator &cg)
{
	printf("andi %s, %s, 0x%04x\n", GetRegName(cur_instr.i_type.rt), GetRegName(cur_instr.i_type.rs), cur_instr.i_type.imm);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.and_(cg.ebx, cur_instr.i_type.imm);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitORI(Xbyak::CodeGenerator& cg)
{
	printf("ori %s, %s, 0x%04x\n", GetRegName(cur_instr.i_type.rt), GetRegName(cur_instr.i_type.rs), cur_instr.i_type.imm);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.or_(cg.ebx, cur_instr.i_type.imm);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitJR(Xbyak::CodeGenerator &cg)
{
	printf("jr %s (0x%08x)\n", GetRegName(cur_instr.i_type.rs), g_state.regs[cur_instr.i_type.rs]);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitADDU(Xbyak::CodeGenerator &cg)
{
	printf("addu %s, %s, %s\n", GetRegName(cur_instr.r_type.rd), GetRegName(cur_instr.r_type.rt), GetRegName(cur_instr.r_type.rs));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rt * 4)]);
	cg.mov(cg.ecx, cg.dword[cg.rax]);
	
	cg.add(cg.ebx, cg.ecx);
	
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rd * 4)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitOr(Xbyak::CodeGenerator &cg)
{
	printf("or %s, %s, %s\n", GetRegName(cur_instr.r_type.rd), GetRegName(cur_instr.r_type.rt), GetRegName(cur_instr.r_type.rs));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rt * 4)]);
	cg.mov(cg.ecx, cg.dword[cg.rax]);
	
	cg.or_(cg.ebx, cg.ecx);
	
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rd * 4)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

void CPURecompiler::EmitSLTU(Xbyak::CodeGenerator &cg)
{
	printf("sltu %s, %s, %s\n", GetRegName(cur_instr.r_type.rd), GetRegName(cur_instr.r_type.rt), GetRegName(cur_instr.r_type.rs));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rs * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rt * 4)]);
	cg.mov(cg.ecx, cg.dword[cg.rax]);
	cg.cmp(cg.ebx, cg.ecx);

	Xbyak::Label not_less_than;
	Xbyak::Label end;

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rd * 4)]);
	cg.jae(not_less_than);
	cg.mov(cg.dword[cg.rax], 1);
	cg.jmp(end);

	cg.L(not_less_than);
	cg.mov(cg.dword[cg.rax], 0);
	cg.L(end);
}

void CPURecompiler::EmitMTC0(Xbyak::CodeGenerator &cg)
{
	printf("mtc0 r%d, %s\n", cur_instr.r_type.rd, GetRegName(cur_instr.r_type.rt));

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.r_type.rt * 4)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, cop0) + (cur_instr.r_type.rd * 4)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
}

HostFunc CPURecompiler::CompileBlock()
{
	printf("-----------------------------------\n");

	CheckCacheFull();

	for (auto b : blockCache)
	{
		if (b->guest_addr == g_state.pc)
		{
			b->hits++;
			return b->entry;
		}
	}

	void* buffer = AllocBlock(cur_size);
	Xbyak::CodeGenerator cg(cur_size, buffer);

	CodeBlock* block = new CodeBlock;
	block->entry = (HostFunc)buffer;
	block->Start = (uint8_t*)buffer;
	block->guest_addr = g_state.pc;

	blockCache.push_back(block);

	EmitPrequel(cg);

	for (auto i : cur_instrs)
	{
		cur_instr.full = i;

		EmitIncPC(cg);

		if (cur_instr.full == 0)
		{
			printf("nop\n");
			cg.nop();
		}
		else
		{
			switch (cur_instr.opcode)
			{
			case Instructions::special:
			{
				switch (cur_instr.r_type.func)
				{
				case SpecialInstructions::jr:
					EmitJR(cg);
					break;
				case SpecialInstructions::addu:
					EmitADDU(cg);
					break;
				case SpecialInstructions::or_:
					EmitOr(cg);
					break;
				case SpecialInstructions::sltiu:
					EmitSLTU(cg);
					break;
				default:
					printf("Unknown special instruction 0x%02x (0x%08x)\n", cur_instr.r_type.func, cur_instr.full);
					exit(1);
				}
				break;
			}
			case Instructions::jal:
				EmitJAL(cg);
				break;
			case Instructions::j:
				EmitJ(cg);
				break;
			case Instructions::beq:
				EmitBEQ(cg);
				break;
			case Instructions::bne:
				EmitBNE(cg);
				break;
			case Instructions::addi:
			case Instructions::addiu:
				EmitAddiu(cg);
				break;
			case Instructions::andi:
				EmitANDI(cg);
				break;
			case Instructions::ori:
				EmitORI(cg);
				break;
			case Instructions::lui:
				EmitLUI(cg);
				break;
			case Instructions::cop0:
			{
				switch (cur_instr.r_type.rs)
				{
				case Cop0Instructions::mtc0:
					EmitMTC0(cg);
					break;
				default:
					printf("Unknown cop0 instruction 0x%02x (0x%08x)\n", cur_instr.r_type.rs, cur_instr.full);
					exit(1);
				}
				break;
			}
			case Instructions::lb:
				EmitLB(cg);
				break;
			case Instructions::lw:
				EmitLW(cg);
				break;
			case Instructions::sb:
				EmitSB(cg);
				break;
			case Instructions::sh:
				EmitSH(cg);
				break;
			case Instructions::sw:
				EmitSW(cg);
				break;
			default:
				printf("Unknown instruction 0x%02x (0x%08x)\n", cur_instr.opcode, cur_instr.full);
				exit(1);
			}
		}

		EmitHandleLoadDelay(cg);
	}

	EmitSequel(cg);

	std::ofstream file("out.bin");

	for (int i = 0; i < cur_size; i++)
	{
		file << cg.getCode()[i];
	}

	cur_size = 25;
	cur_instrs.clear();

	return block->entry;
}

bool CPURecompiler::ModifiesPC(uint32_t i)
{
	Opcode o;
	o.full = i;

	switch (o.opcode)
	{
	case Instructions::special:
		switch (o.r_type.func)
		{
		case SpecialInstructions::jr:
			return true;
		default:
			return false;
		}
		break;
	case Instructions::j:
	case Instructions::jal:
	case Instructions::beq:
	case Instructions::bne:
		return true;
	default:
		return false;
	}
}

void CPURecompiler::CheckCacheFull()
{
	if (blockCache.size() >= 32) // 32 blocks max
	{
		printf("Flushing block cache\n");
		int leastUsed = -1;
		int leastUsedAmount = INT32_MAX;
		for (int i = 0; i < blockCache.size(); i++)
		{
			auto b = blockCache[i];
			if (b->hits < leastUsedAmount)
			{
				leastUsedAmount = b->hits;
				leastUsed = i;
			}
		}

		FreeBlock(blockCache[leastUsed]->Start);
		delete blockCache[leastUsed];
		blockCache.erase(blockCache.begin() + leastUsed);
	}
}

bool CPURecompiler::EmitInstruction(uint32_t opcode)
{
	cur_instr.full = opcode;

	cur_size += 30; // For the increment PC
	cur_size += 15; // For the load delay handler

	if (!opcode)
	{
		cur_size += 1;
	}
	else
	{
		switch (cur_instr.opcode)
		{
		case Instructions::special:
		{
			switch (cur_instr.r_type.func)
			{
			case SpecialInstructions::jr:
				cur_size += 15;
				break;
			case SpecialInstructions::addu:
			case SpecialInstructions::or_:
				cur_size += 20;
				break;
			case SpecialInstructions::sltiu:
				cur_size += 34;
				break;
			default:
				printf("Unknown special instruction 0x%02x (0x%08x)\n", cur_instr.r_type.func, cur_instr.full);
				exit(1);
			}
			break;
		}
		case Instructions::jal:
			cur_size += 36;
			break;
		case Instructions::j:
			cur_size += 24;
			break;
		case Instructions::beq:
		case Instructions::bne:
			cur_size += 27;
			break;
		case Instructions::addiu:
		case Instructions::addi: // Eventually we should maybe handle addi exceptions
			cur_size += 19;
			break;
		case Instructions::lui:
			cur_size += 10;
			break;
		case Instructions::cop0:
		{
			switch (cur_instr.r_type.rs)
			{
			case Cop0Instructions::mtc0:
				cur_size += 12;
				break;
			default:
				printf("Unknown cop0 instruction 0x%02x (0x%08x)\n", cur_instr.r_type.rs, cur_instr.full);
				exit(1);
			}
			break;
		}
		case Instructions::andi:
			cur_size += 20;
			break;
		case Instructions::ori:
			cur_size += 20;
			break;
		case Instructions::sb:
		case Instructions::sh:
		case Instructions::sw:
			cur_size += 47;
			break;
		case Instructions::lb:
		case Instructions::lw:
			cur_size += 55;
			break;
		default:
			printf("Unknown instruction 0x%02x (0x%08x)\n", cur_instr.opcode, cur_instr.full);
			exit(1);
		}
	}

	cur_instrs.push_back(opcode);

	return !ModifiesPC(opcode);
}