#include <sys/types.h>
#include <sys/byteorder.h>
typedef void (*gcm_mul_f)(uint64_t *, uint64_t *, uint64_t *);
typedef boolean_t (*gcm_will_work_f)(void);
typedef struct gcm_impl_ops {
	gcm_mul_f mul;
	gcm_will_work_f is_supported;
	char name[16];
} gcm_impl_ops_t;
