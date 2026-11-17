/* audio.c - capture game global music/sound volume via Fusion PLT hooks.
 *
 * Redirect the game's PLT slots rather than overwriting function bodies: the
 * sound setter is only four bytes long, and both originals must still run.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <string.h>
#include "../so_util.h"
#include "../audio.h"
#include "../error.h"

extern so_module game_mod;

volatile float g_globalMusicGain  = 1.0f;
volatile float g_globalSoundGain  = 1.0f;
volatile int   g_musicHookActive  = 0;
volatile int   g_soundHookActive  = 0;

typedef void (*volume_setter)(float);
static volume_setter s_music_set_volume;
static volume_setter s_sound_set_volume;

static void my_geMusic_SetGlobalVolume(float v)
{
    s_music_set_volume(v);
    g_globalMusicGain = v;
}

static void my_geSound_SetGlobalVolume(float v)
{
    s_sound_set_volume(v);
    g_globalSoundGain = v;
}

static void patch_volume_plt(const char *symbol, uintptr_t replacement)
{
    unsigned patched = 0;
    for (int i = 0; i < game_mod.elf_hdr->e_shnum; ++i) {
        Elf64_Shdr *section = &game_mod.sec_hdr[i];
        if (strcmp(game_mod.shstrtab + section->sh_name, ".rela.plt") != 0)
            continue;
        Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)game_mod.load_base + section->sh_addr);
        size_t count = section->sh_size / sizeof(*rels);
        for (size_t j = 0; j < count; ++j) {
            if (ELF64_R_TYPE(rels[j].r_info) != R_AARCH64_JUMP_SLOT)
                continue;
            Elf64_Sym *sym = &game_mod.syms[ELF64_R_SYM(rels[j].r_info)];
            if (strcmp(game_mod.dynstrtab + sym->st_name, symbol) != 0)
                continue;
            uintptr_t *slot = (uintptr_t *)((uintptr_t)game_mod.load_base + rels[j].r_offset);
            *slot = replacement;
            ++patched;
        }
    }
    if (!patched)
        fatal_error("Could not hook audio function:\n%s", symbol);
}

void patch_audio(void)
{
    const char *music = "_Z23geMusic_SetGlobalVolumef";
    const char *sound = "_Z23geSound_SetGlobalVolumef";
    s_music_set_volume = (volume_setter)so_find_addr_rx(&game_mod, music);
    s_sound_set_volume = (volume_setter)so_find_addr_rx(&game_mod, sound);
    patch_volume_plt(music, (uintptr_t)&my_geMusic_SetGlobalVolume);
    patch_volume_plt(sound, (uintptr_t)&my_geSound_SetGlobalVolume);
    g_musicHookActive = 1;
    g_soundHookActive = 1;
}
