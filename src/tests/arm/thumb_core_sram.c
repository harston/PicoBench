/*
 * thumb_core.c built for the board: code and dispatch table in SRAM, at -O2 (set in
 * CMakeLists.txt). thumb_core.c itself is a verbatim copy of the PicoA10400
 * source (tools/arm_carts/thumb/) and is not compiled on its own.
 */
#include "pico.h"

#define THUMB_HOT __not_in_flash("thumb")
// An explicit .data section: without it GCC puts the never-written dispatch
// table in .rodata, i.e. flash (checked with nm on the first build).
#define THUMB_DATA __attribute__((section(".data.thumb_dispatch")))
#include "thumb_core.c"
