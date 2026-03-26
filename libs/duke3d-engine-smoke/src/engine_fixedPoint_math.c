// This translation unit pulls in a real Duke3D engine source file so PlatformIO
// compiles and links it into the ESPHome firmware (smoke-test dependency).
//
// We include the original .c file to avoid copying engine code into this repo
// and to keep the dependency obvious.

#include "../../../../components/duke3d/engine/components/Engine/fixedPoint_math.c"

