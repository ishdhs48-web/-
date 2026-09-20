#pragma once
#define NOMINMAX
#include <Windows.h>
#include <objidl.h>
#include <propidl.h>
#include <algorithm>  // std::max / std::min — needed because NOMINMAX disables the macros
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
