cmake_minimum_required(VERSION 3.16)
project(EnhancedPhysXLoader CXX RC)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Configure for 32-bit: cmake -S . -B build -A Win32 (Mirror's Edge is a 32-bit game)")
endif()

add_library(PhysXLoader SHARED
  src/proxy.cpp src/fluid.cpp src/clothfast.cpp src/clothspy.cpp src/version.rc src/PhysXLoader.def)
target_link_libraries(PhysXLoader PRIVATE version)

if(MSVC)
  # Static CRT: no extra runtime DLL next to the game.
  set_property(TARGET PhysXLoader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  target_compile_options(PhysXLoader PRIVATE /W3 /GS- /EHs-c- /GR- /O2 /arch:SSE2 /fp:fast)
  target_compile_definitions(PhysXLoader PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
