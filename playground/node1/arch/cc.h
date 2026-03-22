/* arch/cc.h — compiler/platform glue for LwIP on Linux/GCC */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uintptr_t mem_ptr_t;

#define U16_F "hu"
#define S16_F "d"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    fprintf(stderr, "Assertion failed: %s:%d\n", __FILE__, __LINE__); \
    abort(); } while(0)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#endif /* LWIP_ARCH_CC_H */
// ```

// Your `node1/` directory should now look like this:
// ```
// node1/
// ├── arch/
// │   └── cc.h
// ├── CMakeLists.txt
// ├── Dockerfile
// ├── entrypoint.sh
// ├── lwipopts.h
// ├── main.c
// └── sys_arch_stub.c