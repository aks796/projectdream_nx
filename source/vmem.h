/* vmem.h -- reserved address space that is committed piece by piece (vmem.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef VMEM_H
#define VMEM_H

#include <stddef.h>
#include <stdint.h>

void vmem_init(void);
// A PROT_NONE reservation of len bytes: address space only. NULL when there
// is no room (the caller then falls back to committed memory).
void *vmem_reserve(size_t len);
// 1 when [addr, addr+len) lies in a reservation.
int vmem_owns(const void *addr, size_t len);
// Make [addr, addr+len) usable (zeroed when new). 0 / -1.
int vmem_commit(void *addr, size_t len);
// Zero what is committed in [addr, addr+len) (MADV_DONTNEED).
void vmem_zero(void *addr, size_t len);
// Give back [addr, addr+len): committed memory is freed, address space
// released when a whole end of the reservation goes.
void vmem_release(void *addr, size_t len);
void vmem_stats(size_t *reserved, size_t *committed);
// Unity's decommit (mprotect PROT_NONE) of committed pages
void vmem_decommit(void *addr, size_t len);
// the granules with nothing committed given back to the heap: bytes freed
size_t vmem_trim(void);

#endif
