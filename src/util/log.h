#pragma once

#include <cstdio>
#include <cstdlib>

#define MODULE ""

#define log(x, ...) printf("[%s]: ", MODULE); printf(x, ##__VA_ARGS__)
#define panic(x, ...) log(x, ##__VA_ARGS__); exit(1)

#undef MODULE