/* psprecomp — static constructor discovery and execution. See src/ctors.c. */
#ifndef PSPRECOMP_CTORS_H
#define PSPRECOMP_CTORS_H

#include <stdint.h>

/* Search [lo, hi) for the longest null-terminated run of pointers into
 * [code_lo, code_hi). Returns 0 and fills the outputs on success, -1 if no
 * candidate was found. Candidates are proposals, not proof: vtables have the
 * same shape, and only the caller knows whether the entries have callers. */
int psp_ctors_find(uint32_t lo, uint32_t hi, uint32_t code_lo, uint32_t code_hi,
                   uint32_t *out_addr, int *out_count);

/* Call every entry in a null-terminated constructor table. Returns the number
 * run. Must happen before the module uses any global object. */
int psp_ctors_run(uint32_t table);

/* As psp_ctors_run, for a table whose entries are `stride` bytes apart with
 * the function pointer as the first word of each. A zero pointer ends it.
 *
 * Not every toolchain emits a packed array of pointers. The Sony PSP SDK
 * puts C++ global constructors in `.cplinit` as (function, flags) pairs,
 * stride 8, so a stride-4 walk reads a flags word as its second entry -- and
 * because that word is usually zero, the walk stops after one. Stride 4 is
 * the GNU `.ctors` / `.init_array` shape. */
int psp_ctors_run_strided(uint32_t table, uint32_t stride);

#endif