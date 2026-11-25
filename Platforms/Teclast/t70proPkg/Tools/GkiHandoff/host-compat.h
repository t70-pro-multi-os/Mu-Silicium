/* Host tools only: retain pre-C23 GNU string prototypes with glibc 2.43. */
#include <features.h>
#if defined(__GLIBC__) && defined(__GLIBC_USE_ISOC23)
#undef __GLIBC_USE_ISOC23
#define __GLIBC_USE_ISOC23 0
#endif
