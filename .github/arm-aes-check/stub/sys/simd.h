/* Harness stub: userland, NEON always usable, no state to save. */
#include <sys/types.h>
typedef struct { int unused; } kfpu_ctx_t;
#define	kfpu_allowed()		1
#define	kfpu_begin(ctx)		((void) (ctx))
#define	kfpu_end(ctx)		((void) (ctx))
static inline boolean_t zfs_aes_available(void) { return (B_TRUE); }
static inline boolean_t zfs_pmull_available(void) { return (B_TRUE); }
