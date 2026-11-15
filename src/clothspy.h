// NxCloth API census (diagnostics). See clothspy.cpp.
#pragma once
namespace clothspy {
typedef void (*LogFn)(const char* fmt, ...);
bool install(void* anyCloth, LogFn log);   // instruments the shared NpCloth vtable (once)
void report(const char* when);
}
