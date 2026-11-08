#include <stdint.h>
#define	ntohll(x)	__builtin_bswap64(x)
#define	htonll(x)	__builtin_bswap64(x)
