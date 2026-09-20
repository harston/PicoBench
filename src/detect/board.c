#include "detect/board.h"

#include "core/platform.h"
#include "core/sequencer.h"
#include "storage/flash_layout.h"

#define MB (1024u * 1024u)

static const board_profile_t s_weact = {
    "WeAct Studio RP2350B Core", "RP2350B (QFN80) with 16 MB flash", 25, 23, 0};
static const board_profile_t s_rp2350b = {
    "RP2350B board (not the WeAct flash size)", "RP2350B, flash size does not match a known board", 25, 23, 0};
static const board_profile_t s_pico2 = {
    "Raspberry Pi Pico 2", "RP2350A (QFN60) with 4 MB flash", 25, -1, -1};
static const board_profile_t s_rp2350a = {
    "Pico 2 compatible board / clone (RP2350A)", "RP2350A with a flash size other than the Pico 2's 4 MB",
    25, -1, -1};
static const board_profile_t s_pico = {
    "Raspberry Pi Pico", "RP2040 with 2 MB flash", 25, -1, -1};
static const board_profile_t s_rp2040_big = {
    "RP2040 board with large flash (e.g. purple Pico clone)", "RP2040 with more than 2 MB flash", 25, -1, -1};

const board_profile_t *board_profile(void) {
    uint32_t flash = flash_layout()->flash_size;
    switch (platform_chip()) {
    case CHIP_RP2350B: return flash == 16u * MB ? &s_weact : &s_rp2350b;
    case CHIP_RP2350A: return flash == 4u * MB ? &s_pico2 : &s_rp2350a;
    default: return flash <= 2u * MB ? &s_pico : &s_rp2040_big;
    }
}
