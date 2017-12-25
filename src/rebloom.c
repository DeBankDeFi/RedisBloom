#include "redismodule.h"
#include "sb.h"
#include "cf.h"
#include "version.h"

#include <assert.h>
#include <strings.h> // strncasecmp
#include <string.h>
#include <ctype.h>

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Redis Commands                                                           ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
static RedisModuleType *BFType;
static RedisModuleType *CFType;
static double BFDefaultErrorRate = 0.01;
static size_t BFDefaultInitCapacity = 100;
static size_t CFDefaultInitCapacity = 1000;
static int rsStrcasecmp(const RedisModuleString *rs1, const char *s2);

typedef enum { SB_OK = 0, SB_MISSING, SB_EMPTY, SB_MISMATCH } lookupStatus;

static int getValue(RedisModuleKey *key, RedisModuleType *expType, void **sbout) {
    *sbout = NULL;
    if (key == NULL) {
        return SB_MISSING;
    }
    int type = RedisModule_KeyType(key);
    if (type == REDISMODULE_KEYTYPE_EMPTY) {
        return SB_EMPTY;
    } else if (type == REDISMODULE_KEYTYPE_MODULE &&
               RedisModule_ModuleTypeGetType(key) == expType) {
        *sbout = RedisModule_ModuleTypeGetValue(key);
        return SB_OK;
    } else {
        return SB_MISMATCH;
    }
}

static int bfGetChain(RedisModuleKey *key, SBChain **sbout) {
    return getValue(key, BFType, (void **)sbout);
}

static int cfGetFilter(RedisModuleKey *key, CuckooFilter **cfout) {
    return getValue(key, CFType, (void **)cfout);
}

static const char *statusStrerror(int status) {
    switch (status) {
    case SB_MISSING:
    case SB_EMPTY:
        return "ERR not found";
    case SB_MISMATCH:
        return REDISMODULE_ERRORMSG_WRONGTYPE;
    case SB_OK:
        return "ERR item exists";
    default:
        return "Unknown error";
    }
}

/**
 * Common function for adding one or more items to a bloom filter.
 * capacity and error rate must not be 0.
 */
static SBChain *bfCreateChain(RedisModuleKey *key, double error_rate, size_t capacity) {
    SBChain *sb = SB_NewChain(capacity, error_rate, 0);
    if (sb != NULL) {
        RedisModule_ModuleTypeSetValue(key, BFType, sb);
    }
    return sb;
}

static CuckooFilter *cfCreate(RedisModuleKey *key, size_t capacity) {
    CuckooFilter *cf = RedisModule_Calloc(1, sizeof(*cf));
    if (CuckooFilter_Init(cf, capacity) != 0) {
        RedisModule_Free(cf);
        cf = NULL;
    }
    RedisModule_ModuleTypeSetValue(key, CFType, cf);
    return cf;
}

/**
 * Reserves a new empty filter with custom parameters:
 * BF.RESERVE <KEY> <ERROR_RATE (double)> <INITIAL_CAPACITY (int)>
 */
static int BFReserve_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);

    if (argc != 4) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    double error_rate;
    if (RedisModule_StringToDouble(argv[2], &error_rate) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR bad error rate");
    }

    long long capacity;
    if (RedisModule_StringToLongLong(argv[3], &capacity) != REDISMODULE_OK ||
        capacity >= UINT32_MAX) {
        return RedisModule_ReplyWithError(ctx, "ERR bad capacity");
    }

    if (error_rate == 0 || capacity == 0) {
        return RedisModule_ReplyWithError(ctx, "ERR capacity and error must not be 0");
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    SBChain *sb;
    int status = bfGetChain(key, &sb);
    if (status != SB_EMPTY) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    if (bfCreateChain(key, error_rate, capacity) == NULL) {
        RedisModule_ReplyWithSimpleString(ctx, "ERR could not create filter");
    } else {
        RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
    return REDISMODULE_OK;
}

static int isMulti(const RedisModuleString *rs) {
    size_t n;
    const char *s = RedisModule_StringPtrLen(rs, &n);
    return s[3] == 'm' || s[3] == 'M';
}

/**
 * Check for the existence of an item
 * BF.CHECK <KEY>
 * Returns true or false
 */
static int BFCheck_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    int is_multi = isMulti(argv[0]);

    if ((is_multi == 0 && argc != 3) || (is_multi && argc < 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    SBChain *sb;
    int status = bfGetChain(key, &sb);

    int is_empty = 0;
    if (status != SB_OK) {
        is_empty = 1;
    }

    // Check if it exists?
    if (is_multi) {
        RedisModule_ReplyWithArray(ctx, argc - 2);
    }

    for (size_t ii = 2; ii < argc; ++ii) {
        if (is_empty == 1) {
            RedisModule_ReplyWithLongLong(ctx, 0);
        } else {
            size_t n;
            const char *s = RedisModule_StringPtrLen(argv[ii], &n);
            int exists = SBChain_Check(sb, s, n);
            RedisModule_ReplyWithLongLong(ctx, exists);
        }
    }

    return REDISMODULE_OK;
}

/**
 * Adds items to an existing filter. Creates a new one on demand if it doesn't exist.
 * BF.ADD <KEY> ITEMS...
 * Returns an array of integers. The nth element is either 1 or 0 depending on whether it was newly
 * added, or had previously existed, respectively.
 */
static int BFAdd_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);

    int is_multi = isMulti(argv[0]);

    if ((is_multi && argc < 3) || (is_multi == 0 && argc != 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    SBChain *sb;
    int status = bfGetChain(key, &sb);
    if (status == SB_EMPTY) {
        sb = bfCreateChain(key, BFDefaultErrorRate, BFDefaultInitCapacity);
        if (sb == NULL) {
            return RedisModule_ReplyWithError(ctx, "ERR could not create filter");
        }
    } else if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    if (is_multi) {
        RedisModule_ReplyWithArray(ctx, argc - 2);
    }

    for (size_t ii = 2; ii < argc; ++ii) {
        size_t n;
        const char *s = RedisModule_StringPtrLen(argv[ii], &n);
        int rv = SBChain_Add(sb, s, n);
        RedisModule_ReplyWithLongLong(ctx, !!rv);
    }
    return REDISMODULE_OK;
}

/**
 * BF.DEBUG KEY
 * returns some information about the bloom filter.
 */
static int BFInfo_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 2) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    const SBChain *sb = NULL;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int status = bfGetChain(key, (SBChain **)&sb);
    if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    // Start writing info
    RedisModule_ReplyWithArray(ctx, 1 + sb->nfilters);

    RedisModuleString *info_s = RedisModule_CreateStringPrintf(ctx, "size:%llu", sb->size);
    RedisModule_ReplyWithString(ctx, info_s);
    RedisModule_FreeString(ctx, info_s);

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        const SBLink *lb = sb->filters + ii;
        info_s = RedisModule_CreateStringPrintf(
            ctx, "bytes:%llu bits:%llu hashes:%u capacity:%u size:%lu ratio:%g", lb->inner.bytes,
            lb->inner.bits ? lb->inner.bits : 1LLU << lb->inner.n2, lb->inner.hashes,
            lb->inner.entries, lb->size, lb->inner.error);
        RedisModule_ReplyWithString(ctx, info_s);
        RedisModule_FreeString(ctx, info_s);
    }

    return REDISMODULE_OK;
}

#define MAX_SCANDUMP_SIZE 10485760 // 10MB

/**
 * BF.SCANDUMP <KEY> <ITER>
 * Returns an (iterator,data) pair which can be used for LOADCHUNK later on
 */
static int BFScanDump_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    const SBChain *sb = NULL;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int status = bfGetChain(key, (SBChain **)&sb);
    if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    long long iter;
    if (RedisModule_StringToLongLong(argv[2], &iter) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Second argument must be numeric");
    }

    RedisModule_ReplyWithArray(ctx, 2);

    if (iter == 0) {
        size_t hdrlen;
        char *hdr = SBChain_GetEncodedHeader(sb, &hdrlen);
        RedisModule_ReplyWithLongLong(ctx, SB_CHUNKITER_INIT);
        RedisModule_ReplyWithStringBuffer(ctx, (const char *)hdr, hdrlen);
        SB_FreeEncodedHeader(hdr);
    } else {
        size_t bufLen = 0;
        const char *buf = SBChain_GetEncodedChunk(sb, &iter, &bufLen, MAX_SCANDUMP_SIZE);
        RedisModule_ReplyWithLongLong(ctx, iter);
        RedisModule_ReplyWithStringBuffer(ctx, buf, bufLen);
    }
    return REDISMODULE_OK;
}

/**
 * BF.LOADCHUNK <KEY> <ITER> <DATA>
 * Incrementally loads a bloom filter.
 */
static int BFLoadChunk_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);

    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    long long iter;
    if (RedisModule_StringToLongLong(argv[2], &iter) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "ERR Second argument must be numeric");
    }

    size_t bufLen;
    const char *buf = RedisModule_StringPtrLen(argv[3], &bufLen);

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    SBChain *sb;
    int status = bfGetChain(key, &sb);
    if (status == SB_EMPTY && iter == 1) {
        const char *errmsg;
        SBChain *sb = SB_NewChainFromHeader(buf, bufLen, &errmsg);
        if (!sb) {
            return RedisModule_ReplyWithError(ctx, errmsg);
        } else {
            RedisModule_ModuleTypeSetValue(key, BFType, sb);
            return RedisModule_ReplyWithSimpleString(ctx, "OK");
        }
    } else if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    assert(sb);

    const char *errMsg;
    if (SBChain_LoadEncodedChunk(sb, iter, buf, bufLen, &errMsg) != 0) {
        return RedisModule_ReplyWithError(ctx, errMsg);
    } else {
        return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
}

/** CF.RESERVE <KEY> <CAPACITY> */
static int CFReserve_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);
    //
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    long long capacity;
    if (RedisModule_StringToLongLong(argv[2], &capacity)) {
        return RedisModule_ReplyWithError(ctx, "Bad capacity");
    }

    CuckooFilter *cf;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int status = cfGetFilter(key, &cf);
    if (status != SB_EMPTY) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    cf = cfCreate(key, capacity);
    if (cf == NULL) {
        return RedisModule_ReplyWithError(ctx, "Couldn't create Cuckoo Filter");
    } else {
        return RedisModule_ReplyWithSimpleString(ctx, "OK");
    }
}
/**
 * CF.ADD <KEY> <ELEM> [CAPACITY]
 *
 * Adds an item to a cuckoo filter, potentially creating a new cuckoo filter
 */
static int CFAdd_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);

    size_t cmdlen;
    const char *cmdstr = RedisModule_StringPtrLen(argv[0], &cmdlen);
    int isNX = tolower(cmdstr[cmdlen - 1]) == 'x';
    if (argc != 3 && argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    CuckooFilter *cf = NULL;
    int status = cfGetFilter(key, &cf);

    if (status == SB_EMPTY) {
        long long capacity = CFDefaultInitCapacity;
        if (argc == 4) {
            if (RedisModule_StringToLongLong(argv[3], &capacity) != REDISMODULE_OK) {
                return RedisModule_ReplyWithError(ctx, "CAPACITY must be a number");
            }
        }
        if ((cf = cfCreate(key, capacity)) == NULL) {
            return RedisModule_ReplyWithError(ctx, "Could not create filter");
        }
    } else if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    // See if we can add the element
    size_t elemlen;
    const char *elem = RedisModule_StringPtrLen(argv[2], &elemlen);
    CuckooHash hash = CUCKOO_GEN_HASH(elem, elemlen);

    CuckooInsertStatus insStatus;
    if (isNX) {
        insStatus = CuckooFilter_InsertUnique(cf, hash);
    } else {
        insStatus = CuckooFilter_Insert(cf, hash);
    }
    switch (insStatus) {
    case CuckooInsert_Inserted:
        return RedisModule_ReplyWithLongLong(ctx, 1);
    case CuckooInsert_Exists:
        return RedisModule_ReplyWithLongLong(ctx, 0);
    case CuckooInsert_NoSpace:
        return RedisModule_ReplyWithError(ctx, "Filter is full");
    }
}

static int isCount(RedisModuleString *s) {
    size_t n;
    const char *ss = RedisModule_StringPtrLen(s, &n);
    return toupper(ss[n - 1]) == 'T';
}

/**
 * Copy-paste from BFCheck :'(
 */
static int CFCheck_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);

    int is_multi = isMulti(argv[0]);
    int is_count = isCount(argv[0]);

    if ((is_multi == 0 && argc != 3) || (is_multi && argc < 3)) {
        RedisModule_WrongArity(ctx);
        return REDISMODULE_ERR;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    CuckooFilter *cf;
    int status = cfGetFilter(key, &cf);

    int is_empty = 0;
    if (status != SB_OK) {
        is_empty = 1;
    }

    // Check if it exists?
    if (is_multi) {
        RedisModule_ReplyWithArray(ctx, argc - 2);
    }

    for (size_t ii = 2; ii < argc; ++ii) {
        if (is_empty == 1) {
            RedisModule_ReplyWithLongLong(ctx, 0);
        } else {
            size_t n;
            const char *s = RedisModule_StringPtrLen(argv[ii], &n);
            CuckooHash hash = CUCKOO_GEN_HASH(s, n);
            long long rv;
            if (is_count) {
                rv = CuckooFilter_Count(cf, hash);
            } else {
                rv = CuckooFilter_Check(cf, hash);
            }
            RedisModule_ReplyWithLongLong(ctx, rv);
        }
        return REDISMODULE_OK;
    }
    return REDISMODULE_OK;
}

static int CFDel_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    RedisModule_ReplicateVerbatim(ctx);

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    CuckooFilter *cf;
    int status = cfGetFilter(key, &cf);
    if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, "Not found");
    }

    size_t elemlen;
    const char *elem = RedisModule_StringPtrLen(argv[2], &elemlen);
    CuckooHash hash = CUCKOO_GEN_HASH(elem, elemlen);
    return RedisModule_ReplyWithLongLong(ctx, CuckooFilter_Delete(cf, hash));
}

static int CFScanDump_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }

    long long pos;
    if (RedisModule_StringToLongLong(argv[2], &pos) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Invalid position");
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    CuckooFilter *cf;
    int status = cfGetFilter(key, &cf);
    if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    RedisModule_ReplyWithArray(ctx, 2);
    if (!cf->numItems) {
        RedisModule_ReplyWithLongLong(ctx, -1);
        RedisModule_ReplyWithNull(ctx);
        return REDISMODULE_OK;
    }

    size_t chunkLen;
    const char *chunk = CF_GetEncodedChunk(cf, &pos, &chunkLen, MAX_SCANDUMP_SIZE);
    if (chunk == NULL) {
        RedisModule_ReplyWithLongLong(ctx, 0);
        RedisModule_ReplyWithNull(ctx);
    } else {
        RedisModule_ReplyWithLongLong(ctx, pos);
        RedisModule_ReplyWithStringBuffer(ctx, chunk, chunkLen);
    }
    return REDISMODULE_OK;
}

static int CFLoadHeader_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    CuckooFilter *cf;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    int status = cfGetFilter(key, &cf);
    if (status != SB_EMPTY) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }
    size_t n;
    const char *s = RedisModule_StringPtrLen(argv[2], &n);
    if (n != sizeof(CFHeader)) {
        return RedisModule_ReplyWithError(ctx, "Invalid header");
    }
    cf = CFHeader_Load((CFHeader *)s);
    if (cf == NULL) {
        return RedisModule_ReplyWithError(ctx, "Couldn't create filter!");
    }
    RedisModule_ModuleTypeSetValue(key, CFType, cf);
    return REDISMODULE_OK;
}

static int CFLoadChunk_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);

    if (argc != 4) {
        return RedisModule_WrongArity(ctx);
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    CuckooFilter *cf;
    int status = cfGetFilter(key, &cf);
    if (status != SB_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    // Pos, blob
    long long pos;
    if (RedisModule_StringToLongLong(argv[2], &pos) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Invalid position");
    }
    size_t bloblen;
    const char *blob = RedisModule_StringPtrLen(argv[3], &bloblen);

    if (CF_LoadEncodedChunk(cf, pos, blob, bloblen) != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, "Couldn't load chunk!");
    }
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int CFInfo_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModule_AutoMemory(ctx);
    if (argc != 2) {
        return RedisModule_WrongArity(ctx);
    }

    CuckooFilter *cf;
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ);
    int status = cfGetFilter(key, &cf);
    if (status != REDISMODULE_OK) {
        return RedisModule_ReplyWithError(ctx, statusStrerror(status));
    }

    RedisModuleString *resp = RedisModule_CreateStringPrintf(
        ctx, "bktsize:%lu buckets:%lu items:%lu deletes:%lu filters:%lu", CUCKOO_BKTSIZE,
        cf->numBuckets, cf->numItems, cf->numDeletes, cf->numFilters);
    return RedisModule_ReplyWithString(ctx, resp);
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
/// Datatype Functions                                                       ///
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
#define BF_ENCODING_VERSION 1

static void BFRdbSave(RedisModuleIO *io, void *obj) {
    // Save the setting!
    SBChain *sb = obj;

    RedisModule_SaveUnsigned(io, sb->size);
    RedisModule_SaveUnsigned(io, sb->nfilters);

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        const SBLink *lb = sb->filters + ii;
        const struct bloom *bm = &lb->inner;

        RedisModule_SaveUnsigned(io, bm->entries);
        RedisModule_SaveDouble(io, bm->error);
        RedisModule_SaveUnsigned(io, bm->hashes);
        RedisModule_SaveDouble(io, bm->bpe);
        RedisModule_SaveUnsigned(io, bm->bits);
        RedisModule_SaveUnsigned(io, bm->n2);
        RedisModule_SaveStringBuffer(io, (const char *)bm->bf, bm->bytes);

        // Save the number of actual entries stored thus far.
        RedisModule_SaveUnsigned(io, lb->size);
    }
}

static void *BFRdbLoad(RedisModuleIO *io, int encver) {
    if (encver > BF_ENCODING_VERSION) {
        return NULL;
    }

    // Load our modules
    SBChain *sb = RedisModule_Calloc(1, sizeof(*sb));
    sb->size = RedisModule_LoadUnsigned(io);
    sb->nfilters = RedisModule_LoadUnsigned(io);

    // Sanity:
    assert(sb->nfilters < 1000);
    sb->filters = RedisModule_Calloc(sb->nfilters, sizeof(*sb->filters));

    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        SBLink *lb = sb->filters + ii;
        struct bloom *bm = &lb->inner;

        bm->entries = RedisModule_LoadUnsigned(io);
        bm->error = RedisModule_LoadDouble(io);
        bm->hashes = RedisModule_LoadUnsigned(io);
        bm->bpe = RedisModule_LoadDouble(io);
        if (encver == 0) {
            bm->bits = (double)bm->entries * bm->bpe;
        } else {
            bm->bits = RedisModule_LoadUnsigned(io);
            bm->n2 = RedisModule_LoadUnsigned(io);
        }
        size_t sztmp;
        bm->bf = (unsigned char *)RedisModule_LoadStringBuffer(io, &sztmp);
        bm->bytes = sztmp;
        lb->size = RedisModule_LoadUnsigned(io);
    }

    return sb;
}

static void BFAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *value) {
    SBChain *sb = value;
    size_t len;
    char *hdr = SBChain_GetEncodedHeader(sb, &len);
    RedisModule_EmitAOF(aof, "BF.LOADCHUNK", "slb", key, 0, hdr, len);
    SB_FreeEncodedHeader(hdr);

    long long iter = SB_CHUNKITER_INIT;
    const char *chunk;
    while ((chunk = SBChain_GetEncodedChunk(sb, &iter, &len, MAX_SCANDUMP_SIZE)) != NULL) {
        RedisModule_EmitAOF(aof, "BF.LOADCHUNK", "slb", key, iter, chunk, len);
    }
}

static void BFFree(void *value) { SBChain_Free(value); }

static size_t BFMemUsage(const void *value) {
    const SBChain *sb = value;
    size_t rv = sizeof(*sb);
    for (size_t ii = 0; ii < sb->nfilters; ++ii) {
        rv += sizeof(*sb->filters);
        rv += sb->filters[ii].inner.bytes;
    }
    return rv;
}

static void CFFree(void *value) {
    CuckooFilter_Free(value);
    RedisModule_Free(value);
}

static void CFRdbSave(RedisModuleIO *io, void *obj) {
    CuckooFilter *cf = obj;
    RedisModule_SaveUnsigned(io, cf->numFilters);
    RedisModule_SaveUnsigned(io, cf->numBuckets);
    RedisModule_SaveUnsigned(io, cf->numItems);
    for (size_t ii = 0; ii < cf->numFilters; ++ii) {
        RedisModule_SaveStringBuffer(io, (char *)cf->filters[ii],
                                     cf->numBuckets * sizeof(*cf->filters[ii]));
    }
}

static void *CFRdbLoad(RedisModuleIO *io, int encver) {
    if (encver > BF_ENCODING_VERSION) {
        return NULL;
    }

    CuckooFilter *cf = RedisModule_Calloc(1, sizeof(*cf));
    cf->numFilters = RedisModule_LoadUnsigned(io);
    cf->numBuckets = RedisModule_LoadUnsigned(io);
    cf->numItems = RedisModule_LoadUnsigned(io);
    cf->filters = RedisModule_Calloc(cf->numFilters, sizeof(*cf->filters));
    for (size_t ii = 0; ii < cf->numFilters; ++ii) {
        size_t lenDummy = 0;
        cf->filters[ii] = (CuckooBucket *)RedisModule_LoadStringBuffer(io, &lenDummy);
        assert(cf->filters[ii] != NULL && lenDummy == sizeof(CuckooBucket) * cf->numBuckets);
    }
    return cf;
}

static size_t CFMemUsage(const void *value) {
    const CuckooFilter *cf = value;
    return sizeof(*cf) + sizeof(CuckooBucket) * cf->numBuckets * cf->numFilters;
}

static void CFAofRewrite(RedisModuleIO *aof, RedisModuleString *key, void *obj) {
    CuckooFilter *cf = obj;
    // First get the header
    CFHeader header = {.numItems = cf->numItems,
                       .numBuckets = cf->numBuckets,
                       .numDeletes = cf->numDeletes,
                       .numFilters = cf->numFilters};
    RedisModule_EmitAOF(aof, "CF.LOADHDR", "sb", key, (const char *)&header, sizeof(header));
    const char *chunk;
    size_t nchunk;
    long long pos = 0;
    while ((chunk = CF_GetEncodedChunk(cf, &pos, &nchunk, MAX_SCANDUMP_SIZE))) {
        RedisModule_EmitAOF(aof, "CF.LOADCHUNK", "slb", key, pos, chunk, nchunk);
    }
}

static int rsStrcasecmp(const RedisModuleString *rs1, const char *s2) {
    size_t n1 = strlen(s2);
    size_t n2;
    const char *s1 = RedisModule_StringPtrLen(rs1, &n2);
    if (n1 != n2) {
        return -1;
    }
    return strncasecmp(s1, s2, n1);
}

#define BAIL(s, ...)                                                                               \
    do {                                                                                           \
        RedisModule_Log(ctx, "warning", s, ##__VA_ARGS__);                                         \
        return REDISMODULE_ERR;                                                                    \
    } while (0);

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    if (RedisModule_Init(ctx, "bf", REBLOOM_MODULE_VERSION, REDISMODULE_APIVER_1) !=
        REDISMODULE_OK) {
        return REDISMODULE_ERR;
    }

    if (argc == 1) {
        RedisModule_Log(ctx, "notice", "Found empty string. Assuming ramp-packer validation");
        // Hack for ramp-packer which gives us an empty string.
        size_t tmp;
        RedisModule_StringPtrLen(argv[0], &tmp);
        if (tmp == 0) {
            argc = 0;
        }
    }

    if (argc % 2) {
        BAIL("Invalid number of arguments passed");
    }

    for (int ii = 0; ii < argc; ii += 2) {
        if (!rsStrcasecmp(argv[ii], "initial_size")) {
            long long v;
            if (RedisModule_StringToLongLong(argv[ii + 1], &v) == REDISMODULE_ERR) {
                BAIL("Invalid argument for 'INITIAL_SIZE'");
            }
            if (v > 0) {
                BFDefaultInitCapacity = v;
            } else {
                BAIL("INITIAL_SIZE must be > 0");
            }
        } else if (!rsStrcasecmp(argv[ii], "error_rate")) {
            double d;
            if (RedisModule_StringToDouble(argv[ii + 1], &d) == REDISMODULE_ERR) {
                BAIL("Invalid argument for 'ERROR_RATE'");
            } else if (d <= 0) {
                BAIL("ERROR_RATE must be > 0");
            } else {
                BFDefaultErrorRate = d;
            }
        } else {
            BAIL("Unrecognized option");
        }
    }

#define CREATE_CMD(name, tgt, attr)                                                                \
    do {                                                                                           \
        if (RedisModule_CreateCommand(ctx, name, tgt, attr, 1, 1, 1) != REDISMODULE_OK) {          \
            return REDISMODULE_ERR;                                                                \
        }                                                                                          \
    } while (0)
#define CREATE_WRCMD(name, tgt) CREATE_CMD(name, tgt, "write deny-oom")
#define CREATE_ROCMD(name, tgt) CREATE_CMD(name, tgt, "readonly fast")

    CREATE_WRCMD("BF.RESERVE", BFReserve_RedisCommand);
    CREATE_WRCMD("BF.ADD", BFAdd_RedisCommand);
    CREATE_WRCMD("BF.MADD", BFAdd_RedisCommand);
    CREATE_ROCMD("BF.EXISTS", BFCheck_RedisCommand);
    CREATE_ROCMD("BF.MEXISTS", BFCheck_RedisCommand);

    // Bloom - Debug
    CREATE_ROCMD("BF.DEBUG", BFInfo_RedisCommand);
    // Bloom - AOF
    CREATE_ROCMD("BF.SCANDUMP", BFScanDump_RedisCommand);
    CREATE_WRCMD("BF.LOADCHUNK", BFLoadChunk_RedisCommand);

    // Cuckoo Filter commands
    CREATE_WRCMD("CF.RESERVE", CFReserve_RedisCommand);
    CREATE_WRCMD("CF.ADD", CFAdd_RedisCommand);
    CREATE_WRCMD("CF.ADDNX", CFAdd_RedisCommand);
    CREATE_ROCMD("CF.EXISTS", CFCheck_RedisCommand);
    CREATE_ROCMD("CF.MEXISTS", CFCheck_RedisCommand);
    CREATE_ROCMD("CF.COUNT", CFCheck_RedisCommand);

    // Technically a write command, but doesn't change memory profile
    CREATE_CMD("CF.DEL", CFDel_RedisCommand, "write fast");

    // AOF:
    CREATE_ROCMD("CF.SCANDUMP", CFScanDump_RedisCommand);
    CREATE_WRCMD("CF.LOADCHUNK", CFLoadChunk_RedisCommand);
    CREATE_WRCMD("CF.LOADHDR", CFLoadHeader_RedisCommand);

    CREATE_ROCMD("CF.DEBUG", CFInfo_RedisCommand);

    static RedisModuleTypeMethods typeprocs = {.version = REDISMODULE_TYPE_METHOD_VERSION,
                                               .rdb_load = BFRdbLoad,
                                               .rdb_save = BFRdbSave,
                                               .aof_rewrite = BFAofRewrite,
                                               .free = BFFree,
                                               .mem_usage = BFMemUsage};
    BFType = RedisModule_CreateDataType(ctx, "MBbloom--", BF_ENCODING_VERSION, &typeprocs);
    if (BFType == NULL) {
        return REDISMODULE_ERR;
    }

    static RedisModuleTypeMethods cfTypeProcs = {.version = REDISMODULE_TYPE_METHOD_VERSION,
                                                 .rdb_load = CFRdbLoad,
                                                 .rdb_save = CFRdbSave,
                                                 .aof_rewrite = CFAofRewrite,
                                                 .free = CFFree,
                                                 .mem_usage = CFMemUsage};
    CFType = RedisModule_CreateDataType(ctx, "MBbloomCF", BF_ENCODING_VERSION, &cfTypeProcs);
    if (CFType == NULL) {
        return REDISMODULE_ERR;
    }
    return REDISMODULE_OK;
}
