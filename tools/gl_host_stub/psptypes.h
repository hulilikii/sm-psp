/* Host stand-in for <psptypes.h>: only what pspgu.h, pspge.h and the PSPSDK libgu sources need. Used by tools/test_gl_bounds.sh. */
#ifndef GL_HOST_PSPTYPES_H
#define GL_HOST_PSPTYPES_H
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef unsigned int SceSize;
typedef int SceUID;
typedef struct ScePspFVector3 { float x, y, z; } ScePspFVector3;
typedef struct ScePspFVector4 { float x, y, z, w; } ScePspFVector4;
typedef struct ScePspIVector4 { int x, y, z, w; } ScePspIVector4;
typedef struct ScePspFMatrix4 { ScePspFVector4 x, y, z, w; } ScePspFMatrix4;
typedef struct ScePspIMatrix4 { ScePspIVector4 x, y, z, w; } ScePspIMatrix4;
typedef struct ScePspFMatrix3 { ScePspFVector3 x, y, z; } ScePspFMatrix3;
#endif
