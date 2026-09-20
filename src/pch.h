#pragma once
// GDI+ requires COM types (IStream, PROPID) that WIN32_LEAN_AND_MEAN strips.
// Include order: full Windows.h first (no LEAN), then COM/OLE headers,
// then GDI+. NOMINMAX is safe to keep.
#define NOMINMAX
#include <Windows.h>
#include <objidl.h>   // IStream, ISequentialStream (COM base)
#include <propidl.h>  // PROPID, PROPVARIANT

// Now GDI+ can see IStream / PROPID / HDC etc.
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

