#pragma once

enum Instructions
{
	special = 0x00,
	j = 0x02,
	bne = 0x05,
	addiu = 0x09,
	ori = 0x0d,
	lui = 0x0f,
	cop0 = 0x10,
	sw = 0x2b,
};

enum Cop0Instructions
{
	mtc0 = 0x04,
};

enum SpecialInstructions
{
	or_ = 0x25
};