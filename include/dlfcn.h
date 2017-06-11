#ifndef _DLFCN_H_
#define _DLFCN_H_

/* Flags for dlopen(). */
#define    RTLD_LAZY     0x1     /* Lazy relocation. */
#define    RTLD_NOW      0x2     /* Immediate relocations. */
#define    RTLD_GLOBAL   0x4     /* Symbols are globally available. */
#define    RTLD_LOCAL    0       /* Opposite of RTLD_GLOBAL, and the default. */

/* Predefined handles. */
#define    RTLD_DEFAULT  NULL    /* Global library. */

#ifdef __cplusplus
extern "C" {
#endif

/* POSIX standard functions. */
int     dlclose(void *);
char    *dlerror(void);
void    *dlopen(const char *, int);
void    *dlsym(void * __restrict, const char * __restrict);

/* Non-standard functions. */
int     dlinit();
void    dlfree();
int     dldbadd(const char *);    /* NID database initialization. */
int     dldbfreeall(void);        /* NID database destruction. */

#ifdef __cplusplus
}
#endif

#endif
