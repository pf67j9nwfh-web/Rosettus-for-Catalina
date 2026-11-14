/* r2native.h - i386 side of the hostcall bridge (libr2native.dylib, built on Snow Leopard) */
#ifndef R2NATIVE_H
#define R2NATIVE_H
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>

typedef struct { float x, y, w, h; } r2_rect32;
typedef struct { float x, y; } r2_pt32;
typedef struct { float w, h; } r2_size32;
typedef struct { float a, b, c, d, tx, ty; } r2_aff32;

unsigned int r2_hostcall(unsigned int id, unsigned int *blk);
CFDataRef r2_cf_out(CFTypeRef obj);                    /* binary property list of obj (or 0) */
CFTypeRef r2_cf_in(unsigned int addr, unsigned int len);
CFArrayRef r2_ids_in(unsigned int addr, unsigned int len);
int r2_available(void);                                /* 1 when running on the Rosettus 2 host */
CFTypeRef r2_cfconst(const char *name);                /* generated stubs used by r2native.c */
void r2_release(void *h);
void r2_retain(void *h);
#define R2_IS_HANDLE(p) ((unsigned int)(p) - 0x52000001u < 0x01000000u)
void *r2_lookup(const char *name, int *deny);          /* the bridged function NAME, or 0 (*deny: do not use this Mac's i386 one) */
#endif
