/* hostcall.h - host side of the bridge from the 32-bit world to this Mac's own frameworks */
#ifndef HOSTCALL_H
#define HOSTCALL_H
#include "../rhost.h"
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

typedef struct { const char *name; void (*fn)(u32 *b); } hc_entry_t;
extern const hc_entry_t g_hc_table[];
extern const int g_hc_count;

void *hc_sym(const char *name);                 /* this Mac's function (CGS* also as SkyLight's SLS*) */
void hc_missing(const char *name);
u32 hc_handle_put(void *obj);                  /* a native object for the 32-bit world: a small number */
void *hc_handle_get(u32 h);
void hc_handle_free(u32 h);
void hc_handle_ref(u32 h);
int hc_handle_unref(u32 h);                     /* references left; 0: freed */
u32 hc_id_put(void *v);                         /* pointer-sized opaque IDs */
void *hc_id_get(u32 w);
CFTypeRef hc_cf_in(u32 addr, u32 len);          /* a binary property list from the 32-bit world */
void hc_ids_out(CFArrayRef a, u32 *b);
void hc_cf_out(CFTypeRef obj, u32 *b);          /* ... and back: b[0] = address, b[1] = length (in a 32-bit buffer) */
static inline CGFloat hc_f32(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static inline void hc_put_pt(u32 a, CGPoint p) { float t[2] = { (float)p.x, (float)p.y }; memcpy(P32(a), t, 8); }
static inline CGRect hc_get_rect(u32 a) { return CGRectMake(hc_f32(rd32(a)), hc_f32(rd32(a + 4)), hc_f32(rd32(a + 8)), hc_f32(rd32(a + 12))); }
static inline void hc_put_rect(u32 a, CGRect r) {
    float t[4] = { (float)r.origin.x, (float)r.origin.y, (float)r.size.width, (float)r.size.height }; memcpy(P32(a), t, 16);
}
#endif
