#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <taihen.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include "dlfcn.h"
#include "uthash.h"

#define SYMBOL_LENGTH_MAX 59
#define MODNAME_LENGTH_MAX 59
#define DLERRMSG_LENGTH_MAX 127

#define DL_MODULE_TYPE_SYS 's'
#define DL_MODULE_TYPE_FILE 'f'
#define DL_MODULE_TYPE_PRELOADED 'p'

#define DLERR(...) \
    do { \
        snprintf(_dl_errmsg, DLERRMSG_LENGTH_MAX+1, __VA_ARGS__); \
        _dl_err = 1; \
    } while (0)

typedef struct {
    char name[SYMBOL_LENGTH_MAX+1];
    int nid;
    UT_hash_handle hh;
} dl_symbol_info;

typedef struct {
    int type;
    int sid;
    int uid;
    int refcount;
    char name[MODNAME_LENGTH_MAX+1];
    dl_symbol_info *dl_symbols;
    UT_hash_handle hh;
} dl_module_info;

typedef struct dl_handle {
    char name[MODNAME_LENGTH_MAX+1];
    SceUID reserved;
} dl_handle;

SceUID sceKernelCreateRWLock(const char *, SceUInt32, void *);
SceUID sceKernelDeleteRWLock(SceUInt32);
SceInt32 sceKernelLockWriteRWLock(SceUID, SceUInt32 *);
SceInt32 sceKernelUnlockWriteRWLock(SceUID);
SceInt32 sceKernelLockReadRWLock(SceUID, SceUInt32 *);
SceInt32 sceKernelUnlockReadRWLock(SceUID);

static const char *_dl_default_module[] = {"SceLibKernel"};

static int _dl_err = 0;
static char *_dl_errmsg = NULL;
static dl_module_info *_dl_module_db = NULL;
static SceUID _dl_dbhash_rwlock;

static char *basename(const char *filename)
{
    char *p = strrchr(filename, '/');
    return p ? p + 1 : (char *)filename;
}

static int free_module(dl_module_info *module)
{
    switch(module->type)
    {
        case DL_MODULE_TYPE_FILE:
        {
            return sceKernelStopUnloadModule(module->uid, 0, NULL, 0, NULL, NULL);
        }
        case DL_MODULE_TYPE_SYS:
        {
            return sceSysmoduleUnloadModule(module->sid);
        }
        default:
        {
            return -1;
        }
    }
}

static int load_module(dl_module_info *module)
{
    switch(module->type)
    {
        case DL_MODULE_TYPE_FILE:
        {
            int ret = sceKernelLoadStartModule(module->name, 0, NULL, 0, NULL, NULL);
            if (ret >=0 ) module->uid = ret;
            return ret;
        }
        case DL_MODULE_TYPE_SYS:
        {
            return sceSysmoduleLoadModule(module->sid);
        }
        default:
        {
            return -1;
        }
    }
}

static void *symbol_lookup(const char * __restrict module, const char * __restrict symbol)
{
    if (sceKernelLockReadRWLock(_dl_dbhash_rwlock, NULL) < 0)
    {
        DLERR("Error: failed to acquire read lock\n");
        return NULL;
    }
    void *func;
    dl_module_info *module_info = NULL;
    dl_symbol_info *symbol_info = NULL;
    HASH_FIND_STR(_dl_module_db, module, module_info);
    if (!module_info)
    {
        DLERR("Error: failed to find module %s in database\n", module);
        func = NULL;
        goto end;
    }
    HASH_FIND_STR(module_info->dl_symbols, symbol, symbol_info);
    if (!symbol_info)
    {
        func = NULL;
        goto end;
    }
    int ret;
    if (module_info->type == DL_MODULE_TYPE_FILE)
    {
        char *name = strdup(basename(module));
        *strrchr(name, '.') = 0;
        ret = taiGetModuleExportFunc(name, TAI_ANY_LIBRARY, symbol_info->nid, (uintptr_t *)&func);
        free(name);
    }
    else
    {
        ret = taiGetModuleExportFunc(module, TAI_ANY_LIBRARY, symbol_info->nid, (uintptr_t *)&func);
    }
    func = !ret ? func : NULL;
end:
    sceKernelUnlockReadRWLock(_dl_dbhash_rwlock);
    return func;
}

int dlinit()
{
    _dl_dbhash_rwlock = sceKernelCreateRWLock("dldbhashrwlock", 0x2, NULL);
    if (_dl_dbhash_rwlock < 0)
    {
        fprintf(stderr, "Error: failed to create dbhash rwlock\n");
        return -1;
    }
    _dl_errmsg = calloc(1, DLERRMSG_LENGTH_MAX+1);
    if (!_dl_errmsg)
    {
        fprintf(stderr, "Error: no memory\n");
        dlfree();
        return -1;
    }
    return 0;
}

void dlfree()
{
    dldbfreeall();
    memset(_dl_errmsg, 0, DLERRMSG_LENGTH_MAX+1);
    free(_dl_errmsg);
    sceKernelDeleteRWLock(_dl_dbhash_rwlock);
}

int dldbadd(const char *path)
{
    if (sceKernelLockWriteRWLock(_dl_dbhash_rwlock, NULL) < 0)
    {
        DLERR("Error: failed to acquire write lock\n");
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
    {
        DLERR("Error: could not open %s\n", path);
        sceKernelUnlockWriteRWLock(_dl_dbhash_rwlock);
        return -1;
    }
    int failed = 0;
    char *linebuf = NULL;
    size_t bufsize = 0;
    int linenum = 1;
    dl_module_info *current_module = NULL;
    while(__getline(&linebuf, &bufsize, fp) > 0)
    {
        linebuf[strcspn(linebuf, "\r\n")] = 0;    /* Removing trailing newline. */
        switch(linebuf[0])
        {
            /* Module SID. */
            case '#':
            {
                if(!current_module)
                {
                    DLERR("Error: no modules found in %s\n", path);
                    failed = 1;
                    goto fail;
                }
                if (sscanf(linebuf, "#0x%X", &current_module->sid) != 1)
                {
                    DLERR("Error: could not parse %s at line %d\n", path, linenum);
                    failed = 1;
                    goto fail;
                }
                break;
            }
            /* Exports NID entry. */
            case '*':
            {
                if(!current_module)
                {
                    DLERR("Error: no modules found in %s\n", path);
                    failed = 1;
                    goto fail;
                }
                dl_symbol_info *symbol = calloc(1, sizeof(dl_symbol_info));
                if (!symbol)
                {
                    DLERR("Error: no memory\n");
                    failed = 1;
                    goto fail;
                }
                if (sscanf(linebuf, "*%s 0x%X", symbol->name, &symbol->nid) != 2)
                {
                    DLERR("Error: could not parse %s at line %d\n", path, linenum);
                    memset(symbol, 0, sizeof(dl_symbol_info));
                    free(symbol);
                    failed = 1;
                    goto fail;
                }
                dl_symbol_info *duplicated_symbol;
                HASH_REPLACE_STR(current_module->dl_symbols, name, symbol, duplicated_symbol);
                if (duplicated_symbol)
                {
                    fprintf(stderr, "Warning: duplicated symbol %s at line %d\n", duplicated_symbol->name, linenum);
                    memset(duplicated_symbol, 0, sizeof(dl_symbol_info));
                    free(duplicated_symbol);
                }
                break;
            }
            /* Module type [name | path]. */
            case '$':
            {
                HASH_FIND_STR(_dl_module_db, linebuf+3, current_module);
                if (!current_module)
                {
                    current_module = calloc(1, sizeof(dl_module_info));
                    if (!current_module)
                    {
                        DLERR("Error: no memory\n");
                        failed = 1;
                        goto fail;
                    }
                    char module_type;
                    if (sscanf(linebuf, "$%c %s", &module_type, current_module->name) != 2)
                    {
                        DLERR("Error: could not parse %s at line %d\n", path, linenum);
                        memset(current_module, 0, sizeof(dl_module_info));
                        free(current_module);
                        failed = 1;
                        goto fail;
                    }
                    current_module->type = module_type;
                    HASH_ADD_STR(_dl_module_db, name, current_module);
                }
                break;
            }
        }
        linenum++;
    }
fail:
    if (failed) dldbfreeall();
    fclose(fp);
    memset(linebuf, 0, bufsize);
    free(linebuf);
    sceKernelUnlockWriteRWLock(_dl_dbhash_rwlock);
    return 0;
}

int dldbfreeall()
{
    if (sceKernelLockWriteRWLock(_dl_dbhash_rwlock, NULL) < 0)
    {
        DLERR("Error: failed to acquire write lock\n");
        return -1;
    }
    dl_module_info *module, *tmp;
    HASH_ITER(hh, _dl_module_db, module, tmp)
    {
        free_module(module);
        HASH_DEL(_dl_module_db, module);
        memset(module, 0, sizeof(dl_module_info));
        free(module);
    }
    sceKernelUnlockWriteRWLock(_dl_dbhash_rwlock);
    return 0;
}

void *dlopen(const char *filename, int flag)
{
    dl_module_info *module = NULL;
    dl_handle *new_handle = NULL;
    HASH_FIND_STR(_dl_module_db, filename, module);
    if (!module)
    {
        DLERR("Error: failed to find module %s in database\n", filename);
        return NULL;
    }
    if (sceKernelLockWriteRWLock(_dl_dbhash_rwlock, NULL) < 0)
    {
        DLERR("Error: failed to acquire write lock\n");
        return NULL;
    }
    if (!module->refcount)
    {
        if (load_module(module) < 0)
        {
            DLERR("Error: failed to load module %s\n", module->name);
            goto end;
        }
    }
    new_handle = calloc(1, sizeof(dl_handle));
    if (!new_handle)
    {
        DLERR("Error: no memory\n");
        if (!module->refcount)
        {
            free_module(module);
        }
        goto end;
    }
    module->refcount++;
    strcpy(new_handle->name, module->name);
end:
    sceKernelUnlockWriteRWLock(_dl_dbhash_rwlock);
    return new_handle;
}

char *dlerror()
{
    if (_dl_err)
    {
        _dl_err = 0;
        return _dl_errmsg;
    }
    return NULL;
}

int dlclose(void *_handle)
{
    dl_handle *handle = _handle;
    dl_module_info *module = NULL;
    HASH_FIND_STR(_dl_module_db, handle->name, module);
    if (sceKernelLockWriteRWLock(_dl_dbhash_rwlock, NULL) < 0)
    {
        DLERR("Error: failed to acquire write lock\n");
        return -1;
    }
    if (1 == module->refcount)
    {
        free_module(module);
    }
    module->refcount--;
    sceKernelUnlockWriteRWLock(_dl_dbhash_rwlock);
    memset(handle, 0, sizeof(dl_handle));
    free(handle);
    return 0;
}

void *dlsym(void * __restrict _handle, const char * __restrict symbol)
{
    dl_handle *handle = _handle;
    void *ret = NULL;
    if (!handle)
    {
        int i;
        for(i = 0; i < sizeof(_dl_default_module) / sizeof(_dl_default_module[0]); i++)
        {
            ret = symbol_lookup(_dl_default_module[i], symbol);
            if (ret) return ret;
        }
        if (!_dl_err) DLERR("Error: failed to find symbol %s in default modules\n", symbol);
        return NULL;
    }
    ret = symbol_lookup(handle->name, symbol);
    if (!ret && !_dl_err) DLERR("Error: failed to find symbol %s in module %s\n", symbol, handle->name);
    return ret;
}

