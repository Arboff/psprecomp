/* psprecomp -- loading and starting a recompiled module. See module.h. */

#include "psprecomp/module.h"
#include "psprecomp/mem.h"
#include "psprecomp/cpu.h"
#include "psprecomp/ctors.h"
#include "psprecomp/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* The constructor table, by section name.
 *
 * Shape alone cannot separate a constructor table from a vtable, and it
 * cannot see a table whose entries are not packed pointers at all. The
 * section headers say outright which is which, and a decrypted PRX still
 * carries them -- so when they are there, they are the better source.
 *
 * `.cplinit` is the Sony PSP SDK's: (function, flags) pairs, stride 8,
 * terminated by a zero function. `.init_array` and `.ctors` are the GNU
 * ones, packed pointers, and GNU `.ctors` opens with a 0xFFFFFFFF sentinel.
 *
 * The module must already be in guest memory: this reads the table through
 * psp_read32 so that a relocated image is read as the game will see it.
 *
 * Returns 0 having filled m->ctors / nctors / ctors_stride, else -1. */
static int find_ctors_by_section(const uint8_t *b, size_t n, psp_module *m) {
    const uint32_t shoff     = rd32(b + 32);
    const uint16_t shentsize = rd16(b + 46);
    const uint16_t shnum     = rd16(b + 48);
    const uint16_t shstrndx  = rd16(b + 50);
    if (!shoff || shentsize < 40 || !shnum || shstrndx >= shnum) return -1;
    if ((size_t)shoff + (size_t)shentsize * shnum > n) return -1;

    const uint8_t *strsh = b + shoff + (size_t)shstrndx * shentsize;
    const uint32_t stroff = rd32(strsh + 16), strsize = rd32(strsh + 20);
    if ((size_t)stroff + strsize > n) return -1;

    static const struct { const char *name; uint32_t stride; } want[] = {
        { ".cplinit",    8 },
        { ".init_array", 4 },
        { ".ctors",      4 },
    };

    /* In preference order: a module with both should use the SDK's. */
    for (size_t w = 0; w < sizeof want / sizeof want[0]; w++) {
        const size_t wlen = strlen(want[w].name);
        for (uint16_t i = 0; i < shnum; i++) {
            const uint8_t *sh = b + shoff + (size_t)i * shentsize;
            const uint32_t nameoff = rd32(sh);
            const uint32_t addr = rd32(sh + 12), size = rd32(sh + 20);
            if (!addr || size < 4) continue;
            if ((size_t)nameoff + wlen + 1 > strsize) continue;
            if (memcmp(b + stroff + nameoff, want[w].name, wlen + 1) != 0) continue;

            const uint32_t stride = want[w].stride;
            uint32_t start = addr, left = size;
            if (psp_read32(start) == 0xFFFFFFFFu && left >= stride) {
                start += stride;
                left  -= stride;
            }
            int count = 0;
            for (uint32_t at = start; left >= stride; at += stride, left -= stride) {
                if (!psp_read32(at)) break;
                count++;
            }
            if (!count) continue;      /* present but empty: keep looking */

            m->ctors = start;
            m->nctors = count;
            m->ctors_stride = stride;
            return 0;
        }
    }
    return -1;
}

int psp_module_load(const char *path, uint32_t span, psp_module *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "psprecomp: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    uint8_t *b = (n > 52) ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(b);
        fprintf(stderr, "psprecomp: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    /* A decrypted PRX is an ELF whose PT_LOAD segments hold the image. The
     * first one's p_paddr is not a physical address: it carries the file
     * offset of the module info. */
    const uint32_t phoff     = rd32(b + 28);
    const uint16_t phentsize = rd16(b + 42);
    const uint16_t phnum     = rd16(b + 44);
    if (memcmp(b, "\x7F" "ELF", 4) != 0 || phentsize < 32 || phnum == 0 ||
        (size_t)phoff + (size_t)phentsize * phnum > (size_t)n) {
        free(b);
        fprintf(stderr, "psprecomp: %s is not a decrypted ELF module\n", path);
        return -1;
    }

    /* Measure every PT_LOAD before mapping anything, because the mapping has
     * to cover all of them.
     *
     * Loading only the first was enough for a module whose entire image is
     * one segment, and quietly wrong for anything else. God of War: Ghost of
     * Sparta has two, and the second holds .cplinit -- its C++ constructor
     * table -- along with 34 KB of .linkonce.d and the .bss. All of it read
     * as zero, so the constructor search found nothing to run and every
     * global object stayed as .bss left it. */
    uint32_t base = 0xFFFFFFFFu, mem_end = 0, file_end = 0;
    uint32_t info_off = 0, info_vaddr = 0, info_paddr = 0;
    int nload = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + (size_t)i * phentsize;
        if (rd32(ph) != 1) continue;                      /* PT_LOAD */
        const uint32_t vaddr  = rd32(ph + 8);
        const uint32_t filesz = rd32(ph + 16);
        const uint32_t memsz  = rd32(ph + 20);
        if (vaddr < base)              base = vaddr;
        if (vaddr + memsz  > mem_end)  mem_end  = vaddr + memsz;
        if (vaddr + filesz > file_end) file_end = vaddr + filesz;
        if (nload == 0) {
            info_off   = rd32(ph + 4);
            info_vaddr = vaddr;
            info_paddr = rd32(ph + 12);
        }
        nload++;
    }
    if (nload == 0) {
        free(b);
        fprintf(stderr, "psprecomp: %s has no PT_LOAD segment\n", path);
        return -1;
    }
    if (mem_end - base > span) {
        free(b);
        fprintf(stderr,
                "psprecomp: %s needs 0x%08X bytes from 0x%08X, span is 0x%08X\n",
                path, mem_end - base, base, span);
        return -1;
    }
    if (psp_mem_map_module(base, span) != 0) {
        free(b);
        fprintf(stderr, "psprecomp: cannot map %s at 0x%08X\n", path, base);
        return -1;
    }
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = b + phoff + (size_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        const uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8);
        const uint32_t filesz = rd32(ph + 16);
        if (filesz == 0) continue;                        /* .bss-only */
        if ((size_t)off + filesz > (size_t)n ||
            psp_mem_write_block(vaddr, b + off, filesz) != 0) {
            free(b);
            fprintf(stderr, "psprecomp: cannot place %s segment %u at 0x%08X\n",
                    path, (unsigned)i, vaddr);
            return -1;
        }
    }

    m->base = base;
    m->image_end = file_end;
    m->entry = rd32(b + 24);
    /* SceModuleInfo: u16 attr, u16 version, char name[28], then gp. */
    const uint32_t info = (info_paddr & 0x7FFFFFFFu) - info_off + info_vaddr;
    m->gp = psp_read32(info + 32);

    /* Named sections beat recognising the table by shape, so try them first. */
    if (find_ctors_by_section(b, (size_t)n, m) != 0) {
        /* No section headers, or none of those names: fall back to shape. The
         * table and the constructors sit together at the end of the image; a
         * vtable elsewhere looks the same, so only that region is searched and
         * the entries must point into it too.
         * ponytail: right for WTF's modules (8 constructors just below .bss);
         * widen the window if another title keeps them elsewhere. */
        const uint32_t lo = file_end > base + 0x10000u ? file_end - 0x10000u : base;
        uint32_t t;
        int count;
        if (psp_ctors_find(lo, file_end, lo, file_end, &t, &count) == 0) {
            m->ctors = t;
            m->nctors = count;
            m->ctors_stride = 4;
        }
    }
    free(b);
    return 0;
}

/* The loader's leftovers. $k0 is the thread pointer: libc reaches its
 * per-thread reent, and through it the heap, at *(k0 + 4). The stack is only
 * module_start's, which returns once it has started the game's threads.
 * ponytail: fixed addresses, proven on WTF; take them from sysmem if a title
 * minds. */
#define START_STACK 0x08F00000u
#define TCB_ADDR    0x08800000u
#define REENT_ADDR  0x08800100u

uint32_t psp_module_start(const psp_module *m) {
    psp_cpu.r[PSP_REG_GP] = m->gp;
    psp_cpu.r[PSP_REG_SP] = START_STACK;
    psp_cpu.r[PSP_REG_A0] = 0;
    psp_cpu.r[PSP_REG_A1] = 0;
    psp_cpu.r[26] = TCB_ADDR;                   /* $k0 */
    psp_write32(TCB_ADDR + 4, REENT_ADDR);
    if (m->ctors) psp_ctors_run_strided(m->ctors, m->ctors_stride);
    psp_call_from_host(m->entry);
    return psp_cpu.r[PSP_REG_V0];
}
