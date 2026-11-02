/* gcc 3.3 for C33 has no <stdint.h>; mini-libc's <inttypes.h> has the types */
#ifndef ZW_STDINT_SHIM_H
#define ZW_STDINT_SHIM_H
#include <inttypes.h>
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffu
#endif
#ifndef INT32_MAX
#define INT32_MAX 0x7fffffff
#endif
#ifndef UINT16_MAX
#define UINT16_MAX 0xffff
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffULL
#endif
#ifndef INT64_MAX
#define INT64_MAX 0x7fffffffffffffffLL
#endif
#ifndef SIZE_MAX
#define SIZE_MAX 0xffffffffu
#endif
#ifndef UINTPTR_MAX
#define UINTPTR_MAX 0xffffffffu
#endif
#endif
