// Faster PhysX 2.8.0.7 cloth collision (see clothfast.cpp).
#pragma once
#include <windows.h>

namespace clothfast {
typedef void (*LogFn)(const char* fmt, ...);
bool install(ULONG_PTR coreBase, LogFn log);   // only call for PhysXCore 2.8.0.7
void stats(long* calls, long* culled);
}
