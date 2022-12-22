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
};

void* CPURecompiler::AllocBlock(uint32_t size)
{
	MemBlock* b = (MemBlock*)base;

	for (; b; b = b->next)
	{
		if (b->free == true && b->size >= size)
		{
			if (b->size == size)
				return (void*)(b + sizeof(MemBlock));
			
			MemBlock* new_block = (MemBlock*)(b + sizeof(MemBlock) + size);

			new_block->size = b->size - size;
			new_block->free = true;
			new_block->next = b->next;
			new_block->prev = b;
			if (b->next)
				b->next->prev = new_block;
			b->next = new_block;

			b->size = size;
			b->free = false;

			return (void*)(b + sizeof(MemBlock));
		}
	}

	printf("ERROR allocating memory for size %d\n", size);
	exit(1);
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
	block->size = 0xffffffff;

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
	cg.pop(cg.rbx);
	cg.pop(cg.rcx);
	cg.pop(cg.rbx);
	cg.pop(cg.rax);

	cg.ret();
}

void CPURecompiler::EmitIncPC(Xbyak::CodeGenerator &cg)
{
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.rbx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, pc)]);
	cg.mov(cg.dword[cg.rax], cg.ebx);
	
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.add(cg.dword[cg.rax], 4);
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

void CPURecompiler::EmitBNE(Xbyak::CodeGenerator &cg)
{
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * sizeof(uint32_t))]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * sizeof(uint32_t))]);
	cg.mov(cg.ecx, cg.dword[cg.rax]);
	
	cg.cmp(cg.ecx, cg.ebx);

	Xbyak::Label not_equal;
	cg.jne(not_equal);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, next_pc)]);
	cg.mov(cg.ebx, cg.dword[cg.rax]);
	cg.add(cg.ebx, (int32_t)(int16_t)(cur_instr.i_type.imm << 2));
	cg.mov(cg.dword[cg.rax], cg.ebx);

	cg.L(not_equal);
}

void CPURecompiler::EmitAddiu(Xbyak::CodeGenerator &cg)
{
	printf("addiu %s, %s, 0x%04x\n", GetRegName(cur_instr.i_type.rt), GetRegName(cur_instr.i_type.rs), (int32_t)(int16_t)cur_instr.i_type.imm);

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

void CPURecompiler::EmitORI(Xbyak::CodeGenerator& cg)
{
	printf("ori %s, %s, 0x%04x\n", GetRegName(cur_instr.i_type.rt), GetRegName(cur_instr.i_type.rs), cur_instr.i_type.imm);

	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rs * 4)]);
	cg.mov(cg.rbx, cg.dword[cg.rax]);
	cg.or_(cg.rbx, cur_instr.i_type.imm);
	cg.lea(cg.rax, cg.ptr[cg.rbp + offsetof(CPUState, regs) + (cur_instr.i_type.rt * 4)]);
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
	printf("New block\n");

	for (auto b : blockCache)
	{
		if (b->guest_addr == g_state.pc)
			return b->entry;
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
				case SpecialInstructions::or_:
					EmitOr(cg);
					break;
				default:
					printf("Unknown special instruction 0x%02x (0x%08x)\n", cur_instr.r_type.func, cur_instr.full);
					exit(1);
				}
				break;
			}
			case Instructions::j:
				EmitJ(cg);
				break;
			case Instructions::bne:
				EmitBNE(cg);
				break;
			case Instructions::addiu:
				EmitAddiu(cg);
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
			case Instructions::sw:
				EmitSW(cg);
				break;
			default:
				printf("Unknown instruction 0x%02x (0x%08x)\n", cur_instr.opcode, cur_instr.full);
				exit(1);
			}
		}
	}

	EmitSequel(cg);

	std::ofstream file("out.bin");

	for (int i = 0; i < cur_size; i++)
	{
		file << cg.getCode()[i];
	}

	cur_size = 25;
	cur_instrs.clear();

	return (HostFunc)cg.getCode();
}

bool CPURecompiler::ModifiesPC(uint32_t i)
{
	Opcode o;
	o.full = i;

	switch (o.opcode)
	{
	case Instructions::j:
		return true;
	default:
		return false;
	}
}

bool CPURecompiler::EmitInstruction(uint32_t opcode)
{
	cur_instr.full = opcode;

	cur_size += 30;

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
			case SpecialInstructions::or_:
				cur_size += 20;
				break;
			default:
				printf("Unknown special instruction 0x%02x (0x%08x)\n", cur_instr.r_type.func, cur_instr.full);
				exit(1);
			}
			break;
		}
		case Instructions::j:
			cur_size += 24;
			break;
		case Instructions::bne:
			cur_size += 27;
			break;
		case Instructions::addiu:
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
		case Instructions::ori:
			cur_size += 20;
			break;
		case Instructions::sw:
			cur_size += 47;
			break;
		default:
			printf("Unknown instruction 0x%02x (0x%08x)\n", cur_instr.opcode, cur_instr.full);
			exit(1);
		}
	}

	cur_instrs.push_back(opcode);

	return !ModifiesPC(opcode);
}