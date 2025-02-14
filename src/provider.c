/* Copyright (C) 2022 Simo Sorce <simo@redhat.com>
   SPDX-License-Identifier: Apache-2.0 */

#include "provider.h"
#include <pthread.h>
#include <dlfcn.h>
#include <string.h>

struct quirk;

struct p11prov_ctx {

    enum {
        P11PROV_UNINITIALIZED,
        P11PROV_INITIALIZED,
        P11PROV_IN_ERROR,
    } status;

    pthread_rwlock_t rwlock;

    /* Provider handles */
    const OSSL_CORE_HANDLE *handle;
    OSSL_LIB_CTX *libctx;

    /* Configuration */
    const char *module;
    const char *init_args;
    char *pin;
    /* TODO: ui_method */
    /* TODO: fork id */

    /* module handles and data */
    void *dlhandle;
    CK_FUNCTION_LIST *fns;

    int nslots;
    struct p11prov_slot *slots;

    OSSL_ALGORITHM *op_kdf;
    OSSL_ALGORITHM *op_keymgmt;
    OSSL_ALGORITHM *op_exchange;
    OSSL_ALGORITHM *op_signature;
    OSSL_ALGORITHM *op_asym_cipher;
    OSSL_ALGORITHM *op_encoder;
    OSSL_ALGORITHM *op_store;

    struct quirk *quirks;
    int nquirks;
};

struct quirk {
    CK_SLOT_ID id;
    char *name;
    union {
        void *ptr;
        CK_ULONG ulong;
    } data;
    CK_ULONG size;
};

CK_RV p11prov_ctx_get_quirk(P11PROV_CTX *ctx, CK_SLOT_ID id, const char *name,
                            void **data, CK_ULONG *size)
{
    int lock;
    CK_RV ret;

    lock = pthread_rwlock_rdlock(&ctx->rwlock);
    if (lock != 0) {
        ret = CKR_CANT_LOCK;
        P11PROV_raise(ctx, ret, "Failure to rdlock! (%d)", errno);
        return ret;
    }

    for (int i = 0; i < ctx->nquirks; i++) {
        if (id != ctx->quirks[i].id) {
            continue;
        }
        if (strcmp(name, ctx->quirks[i].name) != 0) {
            continue;
        }
        /* return only if requested and if ancillary data exists */
        if (data && ctx->quirks[i].size > 0) {
            if (*size == 0) {
                *data = OPENSSL_malloc(ctx->quirks[i].size);
                if (!*data) {
                    ret = CKR_HOST_MEMORY;
                    goto done;
                }
            } else {
                if (*size < ctx->quirks[i].size) {
                    ret = CKR_BUFFER_TOO_SMALL;
                    goto done;
                }
            }
            if (ctx->quirks[i].size > sizeof(CK_ULONG)) {
                memcpy(*data, ctx->quirks[i].data.ptr, ctx->quirks[i].size);
            } else {
                memcpy(*data, &ctx->quirks[i].data.ulong, ctx->quirks[i].size);
            }
            *size = ctx->quirks[i].size;
        }
        break;
    }

    ret = CKR_OK;

done:
    lock = pthread_rwlock_unlock(&ctx->rwlock);
    if (lock != 0) {
        P11PROV_raise(ctx, CKR_CANT_LOCK, "Failure to unlock! (%d)", errno);
        /* we do not return an error in this case, as we got the info */
    }
    return ret;
}

#define DATA_SWAP(t, d, s) \
    do { \
        t _tmp = d; \
        d = s; \
        s = _tmp; \
    } while (0)
#define QUIRK_ALLOC 4
CK_RV p11prov_ctx_set_quirk(P11PROV_CTX *ctx, CK_SLOT_ID id, const char *name,
                            void *data, CK_ULONG size)
{
    char *_name = NULL;
    void *_data = NULL;
    CK_ULONG _ulong = 0;
    CK_ULONG _size = size;
    int lock;
    CK_RV ret;
    bool found = false;
    int i;

    /* do potentially costly memory allocation operations before locking */
    _name = OPENSSL_strdup(name);
    if (!_name) {
        ret = CKR_HOST_MEMORY;
        P11PROV_raise(ctx, ret, "Failure to copy name");
        return ret;
    }
    if (_size > 0) {
        if (_size > sizeof(CK_ULONG)) {
            _data = OPENSSL_malloc(_size);
            if (!_data) {
                ret = CKR_HOST_MEMORY;
                P11PROV_raise(ctx, ret, "Failure to allocate for data");
                goto failed;
            }
        } else {
            _data = &_ulong;
        }
        memcpy(_data, data, _size);
    }

    lock = pthread_rwlock_wrlock(&ctx->rwlock);
    if (lock != 0) {
        ret = CKR_CANT_LOCK;
        P11PROV_raise(ctx, ret, "Failure to wrlock! (%d)", errno);
        goto failed;
    }

    /* first see if we are replacing quirk data */
    for (i = 0; i < ctx->nquirks; i++) {
        if (id != ctx->quirks[i].id) {
            continue;
        }
        if (strcmp(_name, ctx->quirks[i].name) != 0) {
            continue;
        }

        found = true;
        /* free previous data */
        break;
    }

    if (!found) {
        if ((ctx->nquirks % QUIRK_ALLOC) == 0) {
            size_t asize = sizeof(struct quirk) * (ctx->nquirks + QUIRK_ALLOC);
            struct quirk *q = OPENSSL_realloc(ctx->quirks, asize);
            if (!q) {
                ret = CKR_HOST_MEMORY;
                goto done;
            }
            ctx->quirks = q;
            memset(&ctx->quirks[ctx->nquirks], 0, asize);
            i = ctx->nquirks;
            ctx->nquirks++;
        }
    }

    ctx->quirks[i].id = id;
    /* swap so that we free the old data at fn exit, where
     * precopied data is also freed in case of error */
    DATA_SWAP(char *, ctx->quirks[i].name, _name);
    if (_size > sizeof(CK_ULONG)) {
        DATA_SWAP(void *, ctx->quirks[i].data.ptr, _data);
    } else {
        ctx->quirks[i].data.ulong = _ulong;
        _data = NULL;
    }
    DATA_SWAP(CK_ULONG, ctx->quirks[i].size, _size);
    ret = CKR_OK;

done:
    P11PROV_debug("Set quirk '%s' of size %lu", name, size);
    lock = pthread_rwlock_unlock(&ctx->rwlock);
    if (lock != 0) {
        P11PROV_raise(ctx, CKR_CANT_LOCK, "Failure to unlock! (%d)", errno);
        /* we do not return an error in this case, as we got the info */
    }
failed:
    OPENSSL_free(_name);
    if (_data != &_ulong) {
        OPENSSL_clear_free(_data, _size);
    }
    return ret;
}

int p11prov_ctx_get_slots(P11PROV_CTX *ctx, struct p11prov_slot **slots)
{
    if (ctx->status != P11PROV_INITIALIZED) {
        return RET_OSSL_ERR;
    }

    *slots = ctx->slots;
    return ctx->nslots;
}

OSSL_LIB_CTX *p11prov_ctx_get_libctx(P11PROV_CTX *ctx)
{
    if (ctx->status != P11PROV_INITIALIZED) {
        return NULL;
    }
    return ctx->libctx;
}

CK_RV p11prov_ctx_status(P11PROV_CTX *ctx, CK_FUNCTION_LIST **fns)
{
    switch (ctx->status) {
    case P11PROV_UNINITIALIZED:
        P11PROV_raise(ctx, CKR_GENERAL_ERROR, "Module uninitialized!");
        return CKR_GENERAL_ERROR;
    case P11PROV_INITIALIZED:
        break;
    case P11PROV_IN_ERROR:
        P11PROV_raise(ctx, CKR_GENERAL_ERROR, "Module in error state!");
        return CKR_GENERAL_ERROR;
    }
    if (ctx->fns == NULL) {
        P11PROV_raise(ctx, CKR_GENERAL_ERROR,
                      "Failed to fetch PKCS#11 Function List");
        ctx->status = P11PROV_IN_ERROR;
        return CKR_GENERAL_ERROR;
    }
    if (fns) {
        *fns = ctx->fns;
    }
    return CKR_OK;
}

CK_UTF8CHAR_PTR p11prov_ctx_pin(P11PROV_CTX *ctx)
{
    if (ctx->status != P11PROV_INITIALIZED) {
        return NULL;
    }
    return (CK_UTF8CHAR_PTR)ctx->pin;
}

static void p11prov_ctx_free(P11PROV_CTX *ctx)
{
    int ret;

    if (ctx->status != P11PROV_UNINITIALIZED) {
        ret = pthread_rwlock_wrlock(&ctx->rwlock);
        if (ret != 0) {
            P11PROV_raise(ctx, CKR_CANT_LOCK,
                          "Failure to wrlock! Data corruption may happen (%d)",
                          errno);
        }
    }

    OSSL_LIB_CTX_free(ctx->libctx);

    if (ctx->dlhandle) {
        if (ctx->slots) {
            for (int i = 0; i < ctx->nslots; i++) {
                (void)p11prov_session_pool_free(ctx->slots[i].pool);
                OPENSSL_free(ctx->slots[i].mechs);
            }
            OPENSSL_free(ctx->slots);
            ctx->slots = NULL;
            ctx->nslots = 0;
        }

        ctx->fns->C_Finalize(NULL);
        dlclose(ctx->dlhandle);
    }

    if (ctx->pin) {
        OPENSSL_clear_free(ctx->pin, strlen(ctx->pin));
    }

    if (ctx->quirks) {
        for (int i = 0; i < ctx->nquirks; i++) {
            OPENSSL_free(ctx->quirks[i].name);
            if (ctx->quirks[i].size > sizeof(CK_ULONG)) {
                OPENSSL_clear_free(ctx->quirks[i].data.ptr,
                                   ctx->quirks[i].size);
            }
        }
        OPENSSL_free(ctx->quirks);
    }

    if (ctx->status != P11PROV_UNINITIALIZED) {
        ret = pthread_rwlock_unlock(&ctx->rwlock);
        if (ret != 0) {
            P11PROV_raise(ctx, CKR_CANT_LOCK,
                          "Failure to unlock! Data corruption may happen (%d)",
                          errno);
        }
    }
    ret = pthread_rwlock_destroy(&ctx->rwlock);
    if (ret != 0) {
        P11PROV_raise(ctx, CKR_CANT_LOCK,
                      "Failure to free lock! Data corruption may happen (%d)",
                      errno);
    }
    OPENSSL_clear_free(ctx, sizeof(P11PROV_CTX));
}

static void p11prov_teardown(void *ctx)
{
    p11prov_ctx_free((P11PROV_CTX *)ctx);
}

static OSSL_FUNC_core_get_params_fn *core_get_params = NULL;
static OSSL_FUNC_core_new_error_fn *core_new_error = NULL;
static OSSL_FUNC_core_set_error_debug_fn *core_set_error_debug = NULL;
static OSSL_FUNC_core_vset_error_fn *core_vset_error = NULL;

static void p11prov_get_core_dispatch_funcs(const OSSL_DISPATCH *in)
{
    const OSSL_DISPATCH *iter_in;

    for (iter_in = in; iter_in->function_id != 0; iter_in++) {
        switch (iter_in->function_id) {
        case OSSL_FUNC_CORE_GET_PARAMS:
            core_get_params = OSSL_FUNC_core_get_params(iter_in);
            break;
        case OSSL_FUNC_CORE_NEW_ERROR:
            core_new_error = OSSL_FUNC_core_new_error(iter_in);
            break;
        case OSSL_FUNC_CORE_SET_ERROR_DEBUG:
            core_set_error_debug = OSSL_FUNC_core_set_error_debug(iter_in);
            break;
        case OSSL_FUNC_CORE_VSET_ERROR:
            core_vset_error = OSSL_FUNC_core_vset_error(iter_in);
            break;
        default:
            /* Just ignore anything we don't understand */
            continue;
        }
    }
}

void p11prov_raise(P11PROV_CTX *ctx, const char *file, int line,
                   const char *func, int errnum, const char *fmt, ...)
{
    va_list args;

    if (!core_new_error || !core_vset_error) {
        return;
    }

    va_start(args, fmt);
    core_new_error(ctx->handle);
    core_set_error_debug(ctx->handle, file, line, func);
    core_vset_error(ctx->handle, errnum, fmt, args);
    va_end(args);
}

/* Parameters we provide to the core */
static const OSSL_PARAM p11prov_param_types[] = {
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_NAME, OSSL_PARAM_UTF8_PTR, NULL, 0),
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_VERSION, OSSL_PARAM_UTF8_PTR, NULL, 0),
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_BUILDINFO, OSSL_PARAM_UTF8_PTR, NULL, 0),
    OSSL_PARAM_DEFN(OSSL_PROV_PARAM_STATUS, OSSL_PARAM_INTEGER, NULL, 0),
    OSSL_PARAM_END,
};

static const OSSL_PARAM *p11prov_gettable_params(void *provctx)
{
    return p11prov_param_types;
}

static int p11prov_get_params(void *provctx, OSSL_PARAM params[])
{
    OSSL_PARAM *p;
    int ret;

    p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_NAME);
    if (p != NULL) {
        ret = OSSL_PARAM_set_utf8_ptr(p, "PKCS#11 Provider");
        if (ret == 0) {
            return RET_OSSL_ERR;
        }
    }
    p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_VERSION);
    if (p != NULL) {
        /* temporarily return the OpenSSL build version */
        ret = OSSL_PARAM_set_utf8_ptr(p, OPENSSL_VERSION_STR);
        if (ret == 0) {
            return RET_OSSL_ERR;
        }
    }
    p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_BUILDINFO);
    if (p != NULL) {
        /* temporarily return the OpenSSL build version */
        ret = OSSL_PARAM_set_utf8_ptr(p, OPENSSL_FULL_VERSION_STR);
        if (ret == 0) {
            return RET_OSSL_ERR;
        }
    }
    p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_STATUS);
    if (p != NULL) {
        /* return 1 for now,
         * return 0 in future if there are module issues? */
        ret = OSSL_PARAM_set_int(p, 1);
        if (ret == 0) {
            return RET_OSSL_ERR;
        }
    }
    return RET_OSSL_OK;
}

/* TODO: this needs to be made dynamic,
 * based on what the pkcs11 module supports */
#define ALGOS_ALLOC 4
static CK_RV alg_set_op(OSSL_ALGORITHM **op, int idx, OSSL_ALGORITHM *alg)
{
    if (idx % ALGOS_ALLOC == 0) {
        OSSL_ALGORITHM *tmp =
            OPENSSL_realloc(*op, sizeof(OSSL_ALGORITHM) * (idx + ALGOS_ALLOC));
        if (!tmp) {
            return CKR_HOST_MEMORY;
        }
        *op = tmp;
    }
    (*op)[idx] = *alg;
    return CKR_OK;
}

#define ADD_ALGO_EXT_INT(NAME, operation, prop, func) \
    do { \
        CK_RV alg_ret; \
        OSSL_ALGORITHM alg = { P11PROV_NAMES_##NAME, prop, func, \
                               P11PROV_DESCS_##NAME }; \
        alg_ret = alg_set_op(&ctx->op_##operation, operation##_idx, &alg); \
        if (alg_ret != CKR_OK) { \
            P11PROV_raise(ctx, alg_ret, "Failed to store mech algo"); \
            return RET_OSSL_ERR; \
        } \
        operation##_idx++; \
    } while (0);

#define ADD_ALGO_EXT(NAME, operation, prop, func) \
    do { \
        ADD_ALGO_EXT_INT(NAME, operation, prop, func); \
        ADD_ALGO_EXT_INT(PKCS11_##NAME, operation, prop, func); \
    } while (0)

#define ADD_ALGO(NAME, name, operation) \
    ADD_ALGO_EXT(NAME, operation, P11PROV_DEFAULT_PROPERTIES, \
                 p11prov_##name##_##operation##_functions)

#define TERM_ALGO(operation) \
    do { \
        CK_RV alg_ret; \
        OSSL_ALGORITHM alg = { NULL, NULL, NULL, NULL }; \
        alg_ret = alg_set_op(&ctx->op_##operation, operation##_idx, &alg); \
        if (alg_ret != CKR_OK) { \
            P11PROV_raise(ctx, alg_ret, "Failed to terminate mech algo"); \
            return RET_OSSL_ERR; \
        } \
    } while (0);

#define RSA_SIG_MECHS \
    CKM_RSA_PKCS, CKM_SHA1_RSA_PKCS, CKM_SHA224_RSA_PKCS, CKM_SHA256_RSA_PKCS, \
        CKM_SHA384_RSA_PKCS, CKM_SHA512_RSA_PKCS, CKM_SHA3_224_RSA_PKCS, \
        CKM_SHA3_256_RSA_PKCS, CKM_SHA3_384_RSA_PKCS, CKM_SHA3_512_RSA_PKCS

#define RSAPSS_SIG_MECHS \
    CKM_RSA_PKCS_PSS, CKM_SHA1_RSA_PKCS_PSS, CKM_SHA224_RSA_PKCS_PSS, \
        CKM_SHA256_RSA_PKCS_PSS, CKM_SHA384_RSA_PKCS_PSS, \
        CKM_SHA512_RSA_PKCS_PSS, CKM_SHA3_224_RSA_PKCS_PSS, \
        CKM_SHA3_256_RSA_PKCS_PSS, CKM_SHA3_384_RSA_PKCS_PSS, \
        CKM_SHA3_512_RSA_PKCS_PSS

#define RSA_ENC_MECHS \
    CKM_RSA_PKCS, CKM_RSA_PKCS_OAEP, CKM_RSA_X_509, CKM_RSA_X9_31

#define ECDSA_SIG_MECHS \
    CKM_ECDSA, CKM_ECDSA_SHA1, CKM_ECDSA_SHA224, CKM_ECDSA_SHA256, \
        CKM_ECDSA_SHA384, CKM_ECDSA_SHA512, CKM_ECDSA_SHA3_224, \
        CKM_ECDSA_SHA3_256, CKM_ECDSA_SHA3_384, CKM_ECDSA_SHA3_512

static void alg_rm_mechs(CK_ULONG *checklist, CK_ULONG *rmlist, int *clsize,
                         int rmsize)
{
    CK_ULONG tmplist[*clsize];
    int t = 0;

    for (int i = 0; i < *clsize; i++) {
        tmplist[t] = checklist[i];
        for (int j = 0; j < rmsize; j++) {
            if (tmplist[t] == rmlist[j]) {
                tmplist[t] = CK_UNAVAILABLE_INFORMATION;
                break;
            }
        }
        if (tmplist[t] != CK_UNAVAILABLE_INFORMATION) {
            t++;
        }
    }
    memcpy(checklist, tmplist, t * sizeof(CK_ULONG));
    *clsize = t;
}

#define UNCHECK_MECHS(...) \
    do { \
        CK_ULONG rmlist[] = { __VA_ARGS__ }; \
        int rmsize = sizeof(rmlist) / sizeof(CK_ULONG); \
        alg_rm_mechs(checklist, rmlist, &cl_size, rmsize); \
    } while (0);

static int p11prov_operations_init(P11PROV_CTX *ctx)
{
    CK_ULONG checklist[] = { CKM_RSA_PKCS_KEY_PAIR_GEN,
                             RSA_SIG_MECHS,
                             RSAPSS_SIG_MECHS,
                             RSA_ENC_MECHS,
                             CKM_EC_KEY_PAIR_GEN,
                             ECDSA_SIG_MECHS,
                             CKM_ECDH1_DERIVE,
                             CKM_ECDH1_COFACTOR_DERIVE,
                             CKM_HKDF_DERIVE };
    bool add_rsasig = false;
    bool add_rsaenc = false;
    bool keymgmt_rsa = false;
    bool keymgmt_rsapss = false;
    bool keymgmt_ec = false;
    bool keymgmt_hkdf = false;
    int cl_size = sizeof(checklist) / sizeof(CK_ULONG);
    int kdf_idx = 0;
    int keymgmt_idx = 0;
    int exchange_idx = 0;
    int signature_idx = 0;
    int asym_cipher_idx = 0;
    int encoder_idx = 0;

    for (int ns = 0; ns < ctx->nslots; ns++) {
        for (CK_ULONG ms = 0; ms < ctx->slots->mechs_num; ms++) {
            CK_ULONG mech = CK_UNAVAILABLE_INFORMATION;
            if (cl_size == 0) {
                /* we are done*/
                break;
            }
            for (int cl = 0; cl < cl_size; cl++) {
                if (ctx->slots->mechs[ms] == checklist[cl]) {
                    mech = ctx->slots->mechs[ms];
                    /* found */
                    break;
                }
            }
            if (mech == CK_UNAVAILABLE_INFORMATION) {
                continue;
            }
            switch (mech) {
            case CKM_RSA_PKCS_KEY_PAIR_GEN:
                keymgmt_rsa = true;
                UNCHECK_MECHS(CKM_RSA_PKCS_KEY_PAIR_GEN);
                break;
            case CKM_RSA_PKCS:
                keymgmt_rsa = true;
                add_rsasig = true;
                add_rsaenc = true;
                UNCHECK_MECHS(CKM_RSA_PKCS_KEY_PAIR_GEN, RSA_SIG_MECHS);
                UNCHECK_MECHS(CKM_RSA_PKCS_KEY_PAIR_GEN, RSA_ENC_MECHS);
                break;
            case CKM_SHA1_RSA_PKCS:
            case CKM_SHA224_RSA_PKCS:
            case CKM_SHA256_RSA_PKCS:
            case CKM_SHA384_RSA_PKCS:
            case CKM_SHA512_RSA_PKCS:
            case CKM_SHA3_224_RSA_PKCS:
            case CKM_SHA3_256_RSA_PKCS:
            case CKM_SHA3_384_RSA_PKCS:
            case CKM_SHA3_512_RSA_PKCS:
                keymgmt_rsa = true;
                add_rsasig = true;
                UNCHECK_MECHS(CKM_RSA_PKCS_KEY_PAIR_GEN, RSA_SIG_MECHS);
                break;
            case CKM_RSA_PKCS_PSS:
            case CKM_SHA1_RSA_PKCS_PSS:
            case CKM_SHA224_RSA_PKCS_PSS:
            case CKM_SHA256_RSA_PKCS_PSS:
            case CKM_SHA384_RSA_PKCS_PSS:
            case CKM_SHA512_RSA_PKCS_PSS:
            case CKM_SHA3_224_RSA_PKCS_PSS:
            case CKM_SHA3_256_RSA_PKCS_PSS:
            case CKM_SHA3_384_RSA_PKCS_PSS:
            case CKM_SHA3_512_RSA_PKCS_PSS:
                keymgmt_rsapss = true;
                add_rsasig = true;
                UNCHECK_MECHS(CKM_RSA_PKCS_KEY_PAIR_GEN, RSAPSS_SIG_MECHS);
                break;
            case CKM_RSA_PKCS_OAEP:
            case CKM_RSA_X_509:
            case CKM_RSA_X9_31:
                keymgmt_rsa = true;
                add_rsaenc = true;
                UNCHECK_MECHS(CKM_RSA_PKCS_KEY_PAIR_GEN, RSA_ENC_MECHS);
                break;
            case CKM_EC_KEY_PAIR_GEN:
                keymgmt_ec = true;
                UNCHECK_MECHS(CKM_EC_KEY_PAIR_GEN);
                break;
            case CKM_ECDSA:
            case CKM_ECDSA_SHA1:
            case CKM_ECDSA_SHA224:
            case CKM_ECDSA_SHA256:
            case CKM_ECDSA_SHA384:
            case CKM_ECDSA_SHA512:
            case CKM_ECDSA_SHA3_224:
            case CKM_ECDSA_SHA3_256:
            case CKM_ECDSA_SHA3_384:
            case CKM_ECDSA_SHA3_512:
                keymgmt_ec = true;
                ADD_ALGO(ECDSA, ecdsa, signature);
                UNCHECK_MECHS(CKM_EC_KEY_PAIR_GEN, ECDSA_SIG_MECHS);
                break;
            case CKM_ECDH1_DERIVE:
            case CKM_ECDH1_COFACTOR_DERIVE:
                keymgmt_ec = true;
                ADD_ALGO(ECDH, ecdh, exchange);
                UNCHECK_MECHS(CKM_EC_KEY_PAIR_GEN, CKM_ECDH1_DERIVE,
                              CKM_ECDH1_COFACTOR_DERIVE);
                break;
            case CKM_HKDF_DERIVE:
                keymgmt_hkdf = true;
                ADD_ALGO(HKDF, hkdf, kdf);
                ADD_ALGO(HKDF, hkdf, exchange);
                UNCHECK_MECHS(CKM_HKDF_DERIVE);
                break;
            default:
                P11PROV_raise(ctx, CKR_GENERAL_ERROR,
                              "Unhandled mechianism %lu", mech);
                break;
            }
        }
    }

    /* keymgmt */
    if (keymgmt_rsa) {
        ADD_ALGO(RSA, rsa, keymgmt);
    }
    if (keymgmt_rsapss) {
        ADD_ALGO(RSAPSS, rsapss, keymgmt);
    }
    if (keymgmt_ec) {
        ADD_ALGO(EC, ec, keymgmt);
    }
    if (keymgmt_hkdf) {
        ADD_ALGO(HKDF, hkdf, keymgmt);
    }

    if (add_rsasig) {
        ADD_ALGO(RSA, rsa, signature);
    }
    if (add_rsaenc) {
        ADD_ALGO(RSA, rsa, asym_cipher);
    }
    /* terminations */
    if (kdf_idx > 0) {
        TERM_ALGO(keymgmt);
    }
    if (keymgmt_idx > 0) {
        TERM_ALGO(keymgmt);
    }
    if (exchange_idx > 0) {
        TERM_ALGO(exchange);
    }
    if (signature_idx > 0) {
        TERM_ALGO(signature);
    }
    if (asym_cipher_idx > 0) {
        TERM_ALGO(asym_cipher);
    }

    /* encoder/decoder */
    ADD_ALGO_EXT(RSA, encoder, "provider=pkcs11,output=text",
                 p11prov_rsa_encoder_text_functions);
    ADD_ALGO_EXT(RSA, encoder, "provider=pkcs11,output=der,structure=pkcs1",
                 p11prov_rsa_encoder_pkcs1_der_functions);
    ADD_ALGO_EXT(RSA, encoder, "provider=pkcs11,output=pem,structure=pkcs1",
                 p11prov_rsa_encoder_pkcs1_pem_functions);
    TERM_ALGO(encoder);

    return RET_OSSL_OK;
}

static const OSSL_ALGORITHM p11prov_store[] = {
    {
        "pkcs11",
        P11PROV_DEFAULT_PROPERTIES,
        p11prov_store_functions,
        P11PROV_DESCS_URI,
    },
    { NULL, NULL, NULL, NULL },
};

static const OSSL_ALGORITHM *
p11prov_query_operation(void *provctx, int operation_id, int *no_cache)
{
    P11PROV_CTX *ctx = (P11PROV_CTX *)provctx;
    *no_cache = 0;
    switch (operation_id) {
    case OSSL_OP_KDF:
        return ctx->op_kdf;
    case OSSL_OP_KEYMGMT:
        return ctx->op_keymgmt;
    case OSSL_OP_KEYEXCH:
        return ctx->op_exchange;
    case OSSL_OP_SIGNATURE:
        return ctx->op_signature;
    case OSSL_OP_ASYM_CIPHER:
        return ctx->op_asym_cipher;
    case OSSL_OP_ENCODER:
        return ctx->op_encoder;
    case OSSL_OP_STORE:
        return p11prov_store;
    }
    return NULL;
}

static int p11prov_get_capabilities(void *provctx, const char *capability,
                                    OSSL_CALLBACK *cb, void *arg)
{
    /* TODO: deal with TLS-GROUP */

    return RET_OSSL_ERR;
}

static const OSSL_ITEM *p11prov_get_reason_strings(void *provctx)
{
#define C(str) (void *)(str)
    static const OSSL_ITEM reason_strings[] = {
        { CKR_HOST_MEMORY, C("Host out of memory error") },
        { CKR_SLOT_ID_INVALID, C("The specified slot ID is not valid") },
        { CKR_GENERAL_ERROR, C("General Error") },
        { CKR_FUNCTION_FAILED,
          C("The requested function could not be performed") },
        { CKR_ARGUMENTS_BAD,
          C("Invalid or improper arguments were provided to the "
            "invoked function") },
        { CKR_ATTRIBUTE_READ_ONLY,
          C("Attempted to set or modify an attribute that is Read "
            "Only for applications") },
        { CKR_ATTRIBUTE_TYPE_INVALID,
          C("Invalid attribute type specified in a template") },
        { CKR_ATTRIBUTE_VALUE_INVALID,
          C("Invalid value specified for attribute in a template") },
        { CKR_DATA_INVALID, C("The plaintext input data to a cryptographic "
                              "operation is invalid") },
        { CKR_DATA_LEN_RANGE,
          C("The size of plaintext input data to a cryptographic "
            "operation is invalid (Out of range)") },
        { CKR_DEVICE_ERROR,
          C("Some problem has occurred with the token and/or slot") },
        { CKR_DEVICE_MEMORY,
          C("The token does not have sufficient memory to perform "
            "the requested function") },
        { CKR_DEVICE_REMOVED,
          C("The token was removed from its slot during the "
            "execution of the function") },
        { CKR_FUNCTION_CANCELED,
          C("The function was canceled in mid-execution") },
        { CKR_KEY_HANDLE_INVALID, C("The specified key handle is not valid") },
        { CKR_KEY_SIZE_RANGE,
          C("Unable to handle the specified key size (Out of range)") },
        { CKR_KEY_TYPE_INCONSISTENT,
          C("The specified key is not the correct type of key to "
            "use with the specified mechanism") },
        { CKR_KEY_FUNCTION_NOT_PERMITTED,
          C("The key attributes do not allow this operation to "
            "be executed") },
        { CKR_MECHANISM_INVALID, C("An invalid mechanism was specified to the "
                                   "cryptographic operation") },
        { CKR_MECHANISM_PARAM_INVALID,
          C("Invalid mechanism parameters were supplied") },
        { CKR_OPERATION_ACTIVE,
          C("There is already an active operation that prevents "
            "executing the requested function") },
        { CKR_OPERATION_NOT_INITIALIZED,
          C("There is no active operation of appropriate type "
            "in the specified session") },
        { CKR_PIN_INCORRECT, C("The specified PIN is incorrect") },
        { CKR_PIN_EXPIRED, C("The specified PIN has expired") },
        { CKR_PIN_LOCKED,
          C("The specified PIN is locked, and cannot be used") },
        { CKR_SESSION_CLOSED, C("Session is already closed") },
        { CKR_SESSION_COUNT, C("Too many sessions open") },
        { CKR_SESSION_HANDLE_INVALID, C("Invalid Session Handle") },
        { CKR_SESSION_PARALLEL_NOT_SUPPORTED,
          C("Parallel sessions not supported") },
        { CKR_SESSION_READ_ONLY, C("Session is Read Only") },
        { CKR_SESSION_EXISTS, C("Session already exists") },
        { CKR_SESSION_READ_ONLY_EXISTS,
          C("A read-only session already exists") },
        { CKR_SESSION_READ_WRITE_SO_EXISTS,
          C("A read/write SO session already exists") },
        { CKR_TEMPLATE_INCOMPLETE,
          C("The template to create an object is incomplete") },
        { CKR_TEMPLATE_INCONSISTENT,
          C("The template to create an object has conflicting attributes") },
        { CKR_TOKEN_NOT_PRESENT,
          C("The token was not present in its slot when the "
            "function was invoked") },
        { CKR_TOKEN_NOT_RECOGNIZED,
          C("The token in the slot is not recognized") },
        { CKR_TOKEN_WRITE_PROTECTED,
          C("Action denied because the token is write-protected") },
        { CKR_TOKEN_WRITE_PROTECTED,
          C("Can't perform action because the token is write-protected") },
        { CKR_USER_NOT_LOGGED_IN,
          C("The desired action cannot be performed because an "
            "appropriate user is not logged in") },
        { CKR_USER_PIN_NOT_INITIALIZED, C("The user PIN is not initialized") },
        { CKR_USER_TYPE_INVALID, C("An invalid user type was specified") },
        { CKR_USER_ANOTHER_ALREADY_LOGGED_IN,
          C("Another user is already logged in") },
        { CKR_USER_TOO_MANY_TYPES,
          C("Attempted to log in more users than the token can support") },
        { CKR_OPERATION_CANCEL_FAILED, C("The operation cannot be cancelled") },
        { CKR_DOMAIN_PARAMS_INVALID,
          C("Invalid or unsupported domain parameters were "
            "supplied to the function") },
        { CKR_CURVE_NOT_SUPPORTED,
          C("The specified curve is not supported by this token") },
        { CKR_BUFFER_TOO_SMALL,
          C("The output of the function is too large to fit in "
            "the supplied buffer") },
        { CKR_SAVED_STATE_INVALID,
          C("The supplied saved cryptographic operations state is invalid") },
        { CKR_STATE_UNSAVEABLE,
          C("The cryptographic operations state of the specified "
            "session cannot be saved") },
        { CKR_CRYPTOKI_NOT_INITIALIZED,
          C("PKCS11 Module has not been initialized yet") },
        { 0, NULL },
    };

    return reason_strings;
#undef C
}

/* Functions we provide to the core */
static const OSSL_DISPATCH p11prov_dispatch_table[] = {
    { OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void))p11prov_teardown },
    { OSSL_FUNC_PROVIDER_GETTABLE_PARAMS,
      (void (*)(void))p11prov_gettable_params },
    { OSSL_FUNC_PROVIDER_GET_PARAMS, (void (*)(void))p11prov_get_params },
    { OSSL_FUNC_PROVIDER_QUERY_OPERATION,
      (void (*)(void))p11prov_query_operation },
    { OSSL_FUNC_PROVIDER_GET_CAPABILITIES,
      (void (*)(void))p11prov_get_capabilities },
    { OSSL_FUNC_PROVIDER_GET_REASON_STRINGS,
      (void (*)(void))p11prov_get_reason_strings },
    { 0, NULL },
};

static int get_slot_profiles(P11PROV_CTX *ctx, struct p11prov_slot *slot)
{
    CK_SESSION_HANDLE session;
    CK_BBOOL token = CK_TRUE;
    CK_OBJECT_CLASS class = CKO_PROFILE;

    CK_ATTRIBUTE template[2] = {
        { CKA_TOKEN, &token, sizeof(token) },
        { CKA_CLASS, &class, sizeof(class) },
    };
    CK_OBJECT_HANDLE object[5];
    CK_ULONG objcount;
    int index = 0;
    int ret;

    ret = ctx->fns->C_OpenSession(slot->id, CKF_SERIAL_SESSION, NULL, NULL,
                                  &session);
    if (ret != CKR_OK) {
        P11PROV_debug("OpenSession failed %d", ret);
        return ret;
    }

    ret = ctx->fns->C_FindObjectsInit(session, template, 2);
    if (ret != CKR_OK) {
        P11PROV_debug("C_FindObjectsInit failed %d", ret);
        (void)ctx->fns->C_CloseSession(session);
        return ret;
    }

    /* at most 5 objects as there are 5 profiles for now */
    ret = ctx->fns->C_FindObjects(session, object, 5, &objcount);
    if (ret != CKR_OK) {
        P11PROV_debug("C_FindObjects failed %d", ret);
        goto done;
    }

    if (objcount == 0) {
        P11PROV_debug("No profiles for slot %lu", slot->id);
        goto done;
    }

    for (size_t i = 0; i < objcount; i++) {
        CK_ULONG value = CK_UNAVAILABLE_INFORMATION;
        CK_ATTRIBUTE profileid = { CKA_PROFILE_ID, &value, sizeof(value) };

        ret = ctx->fns->C_GetAttributeValue(session, object[i], &profileid, 1);
        if (ret != CKR_OK || value == CK_UNAVAILABLE_INFORMATION) {
            P11PROV_debug("C_GetAttributeValue failed %d", ret);
            continue;
        }

        slot->profiles[index] = value;
        index++;
    }

done:
    (void)ctx->fns->C_FindObjectsFinal(session);
    (void)ctx->fns->C_CloseSession(session);
    return ret;
}

static void get_slot_mechanisms(P11PROV_CTX *ctx, struct p11prov_slot *slot)
{
    int ret;

    ret = ctx->fns->C_GetMechanismList(slot->id, NULL, &slot->mechs_num);
    if (ret != CKR_OK) {
        P11PROV_raise(ctx, ret, "GetMechanismList(NULL) failed");
        return;
    }

    slot->mechs = OPENSSL_malloc(slot->mechs_num * sizeof(CK_MECHANISM_TYPE));
    if (!slot->mechs) {
        P11PROV_raise(ctx, CKR_HOST_MEMORY, "Failed to alloc for mech list");
        slot->mechs_num = 0;
        return;
    }

    ret = ctx->fns->C_GetMechanismList(slot->id, slot->mechs, &slot->mechs_num);
    if (ret != CKR_OK) {
        P11PROV_raise(ctx, ret, "GetMechanismList(%lu) failed",
                      slot->mechs_num);
        OPENSSL_free(slot->mechs);
        slot->mechs_num = 0;
        return;
    }

    P11PROV_debug("Slot(%lu) mechs found: %lu", slot->id, slot->mechs_num);
}

static CK_RV get_slots(P11PROV_CTX *ctx)
{
    CK_ULONG nslots;
    CK_SLOT_ID *slotid;
    struct p11prov_slot *slots;
    int ret;

    ret = ctx->fns->C_GetSlotList(CK_FALSE, NULL, &nslots);
    if (ret) {
        return ret;
    }

    /* arbitrary number from libp11 */
    if (nslots > 0x10000) {
        return CKR_GENERAL_ERROR;
    }

    slotid = OPENSSL_malloc(nslots * sizeof(CK_SLOT_ID));
    if (slotid == NULL) {
        return CKR_HOST_MEMORY;
    }

    ret = ctx->fns->C_GetSlotList(CK_FALSE, slotid, &nslots);
    if (ret) {
        OPENSSL_free(slotid);
        return ret;
    }

    slots = OPENSSL_zalloc(nslots * sizeof(struct p11prov_slot));
    if (slots == NULL) {
        OPENSSL_free(slotid);
        return CKR_HOST_MEMORY;
    }

    for (size_t i = 0; i < nslots; i++) {
        slots[i].id = slotid[i];
        ret = ctx->fns->C_GetSlotInfo(slotid[i], &slots[i].slot);
        if (ret == CKR_OK && slots[i].slot.flags & CKF_TOKEN_PRESENT) {
            ret = ctx->fns->C_GetTokenInfo(slotid[i], &slots[i].token);
        }
        if (ret) {
            goto done;
        }

        ret = p11prov_session_pool_init(ctx, &slots[i].token, &(slots[i].pool));
        if (ret) {
            goto done;
        }

        (void)get_slot_profiles(ctx, &slots[i]);
        get_slot_mechanisms(ctx, &slots[i]);

        P11PROV_debug_slot(ctx, &slots[i]);
    }

done:
    if (ret != CKR_OK) {
        for (size_t i = 0; i < nslots; i++) {
            p11prov_session_pool_free(slots[i].pool);
        }
        OPENSSL_free(slots);
    } else {
        ctx->slots = slots;
        ctx->nslots = nslots;
    }
    OPENSSL_free(slotid);
    return ret;
}

#if !defined(RTLD_DEEPBIND)
#define RTLD_DEEPBIND 0
#endif

static int p11prov_module_init(P11PROV_CTX *ctx)
{
    CK_RV (*c_get_function_list)(CK_FUNCTION_LIST_PTR_PTR);
    CK_C_INITIALIZE_ARGS args = {
        .flags = CKF_OS_LOCKING_OK,
        .pReserved = (void *)ctx->init_args,
    };
    CK_INFO ck_info = { 0 };
    int ret;

    if (ctx->status != P11PROV_UNINITIALIZED) {
        return 0;
    }

    P11PROV_debug("PKCS#11: Initializing the module: %s", ctx->module);

    dlerror();
    ctx->dlhandle = dlopen(ctx->module, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (ctx->dlhandle == NULL) {
        char *err = dlerror();
        P11PROV_debug("dlopen() failed: %s", err);
        return -ENOENT;
    }

    c_get_function_list = dlsym(ctx->dlhandle, "C_GetFunctionList");
    if (c_get_function_list) {
        ret = c_get_function_list(&ctx->fns);
    } else {
        ret = CKR_GENERAL_ERROR;
    }
    if (ret != CKR_OK) {
        char *err = dlerror();
        P11PROV_debug("dlsym() failed: %s", err);
        dlclose(ctx->dlhandle);
        ctx->dlhandle = NULL;
        return -ENOENT;
    }

    ret = ctx->fns->C_Initialize(&args);
    if (ret && ret != CKR_CRYPTOKI_ALREADY_INITIALIZED) {
        P11PROV_debug("init failed: %d (%s:%d)", ret, __FILE__, __LINE__);
        return -EFAULT;
    }

    ctx->status = P11PROV_INITIALIZED;

    ret = pthread_rwlock_init(&ctx->rwlock, NULL);
    if (ret != 0) {
        ret = errno;
        P11PROV_debug("rwlock init failed (%d)", ret);
        return -ret;
    }

    ret = ctx->fns->C_GetInfo(&ck_info);
    if (ret) {
        return -EFAULT;
    }
    P11PROV_debug("Module Info: ck_ver:%d.%d lib: '%s' '%s' ver:%d.%d",
                  (int)ck_info.cryptokiVersion.major,
                  (int)ck_info.cryptokiVersion.minor, ck_info.manufacturerID,
                  ck_info.libraryDescription, (int)ck_info.libraryVersion.major,
                  (int)ck_info.libraryVersion.minor);

    ret = get_slots(ctx);
    if (ret) {
        return -EFAULT;
    }

    ret = p11prov_operations_init(ctx);
    if (ret != RET_OSSL_OK) {
        return -EFAULT;
    }

    return 0;
}

int OSSL_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in,
                       const OSSL_DISPATCH **out, void **provctx)
{
    OSSL_PARAM core_params[4] = { 0 };
    char *pin = NULL;
    P11PROV_CTX *ctx;
    int ret;

    *provctx = NULL;

    p11prov_get_core_dispatch_funcs(in);

    ctx = OPENSSL_zalloc(sizeof(P11PROV_CTX));
    if (ctx == NULL) {
        return RET_OSSL_ERR;
    }
    ctx->handle = handle;

    ctx->libctx = OSSL_LIB_CTX_new_from_dispatch(handle, in);
    if (ctx->libctx == NULL) {
        OPENSSL_free(ctx);
        return RET_OSSL_ERR;
    }

    /* get module path */
    core_params[0] = OSSL_PARAM_construct_utf8_ptr(
        P11PROV_PKCS11_MODULE_PATH, (char **)&ctx->module, sizeof(ctx->module));
    core_params[1] = OSSL_PARAM_construct_utf8_ptr(
        P11PROV_PKCS11_MODULE_INIT_ARGS, (char **)&ctx->init_args,
        sizeof(ctx->init_args));
    core_params[2] = OSSL_PARAM_construct_utf8_ptr(
        P11PROV_PKCS11_MODULE_TOKEN_PIN, &pin, sizeof(pin));
    core_params[3] = OSSL_PARAM_construct_end();
    ret = core_get_params(handle, core_params);
    if (ret != RET_OSSL_OK) {
        ERR_raise(ERR_LIB_PROV, PROV_R_FAILED_TO_GET_PARAMETER);
        p11prov_ctx_free(ctx);
        return ret;
    }

    ret = p11prov_module_init(ctx);
    if (ret != CKR_OK) {
        ERR_raise(ERR_LIB_PROV, PROV_R_IN_ERROR_STATE);
        p11prov_ctx_free(ctx);
        return RET_OSSL_ERR;
    }

    if (pin != NULL) {
        ret = p11prov_get_pin(pin, &ctx->pin);
        if (ret != 0) {
            ERR_raise(ERR_LIB_PROV, PROV_R_IN_ERROR_STATE);
            p11prov_ctx_free(ctx);
            return RET_OSSL_ERR;
        }
    }

    *out = p11prov_dispatch_table;
    *provctx = ctx;
    return RET_OSSL_OK;
}
