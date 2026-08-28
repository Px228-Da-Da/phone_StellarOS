#ifndef TYPES_H
#define TYPES_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long       u64;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long         s64;
typedef unsigned long       size_t;
typedef unsigned long       uintptr_t;

#define NULL ((void *)0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#endif
