/*
 * Size class allocation cache for FFmpeg's av_malloc / av_realloc / av_free (Windows).
 * Included by the mem.c that thirdparty/CMakeLists.txt generates from libavutil/mem.c.
 *
 * XMA audio decoding (WMA Pro) does about 10 av_malloc and 19 av_free per decoded frame. In this
 * process each heap call costs several microseconds, which measured as 86% of FFmpeg's time and,
 * under effect heavy scenes, slowed every game thread (the BO2 MP recompile: bo2-bisect
 * perf/0010, 16-35 -> 89-149 fps in a stress scene). Freed blocks of 64 B to 64 KB are kept per
 * power of two size class (up to 512 each, SRW locked stacks) and reused.
 *
 * Every block carries a 64 byte header (class, capacity, magic), which keeps FFmpeg's alignment,
 * so blocks can always be told apart. runtime.ffmpeg_alloc_cache switches only the reuse of freed
 * blocks (rex_ffmpeg_alloc_cache_enable), so it can change while blocks are live.
 */
#if defined(_WIN32) && HAVE_ALIGNED_MALLOC
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define FFC_HDR 64
#define FFC_MAGIC 0x46464341u /* 'FFCA' */
#define FFC_MIN_LOG2 6        /* 64 B */
#define FFC_MAX_LOG2 16       /* 64 KB */
#define FFC_CLASSES (FFC_MAX_LOG2 - FFC_MIN_LOG2 + 1)
#define FFC_DEPTH 512
typedef struct { uint32_t magic; int32_t cls; size_t cap; } ffc_hdr;
static SRWLOCK ffc_lock[FFC_CLASSES] = { SRWLOCK_INIT };
static void *ffc_stack[FFC_CLASSES][FFC_DEPTH];
static int ffc_top[FFC_CLASSES];
static volatile long ffc_enabled = 0;

void rex_ffmpeg_alloc_cache_enable(int enabled)
{
    InterlockedExchange(&ffc_enabled, enabled ? 1 : 0);
}

static int ffc_class(size_t size)
{
    int l = FFC_MIN_LOG2;
    while (l <= FFC_MAX_LOG2 && ((size_t)1 << l) < size)
        l++;
    return l > FFC_MAX_LOG2 ? -1 : l - FFC_MIN_LOG2;
}

static void *ffc_alloc(size_t size)
{
    int cls = ffc_class(size);
    size_t cap = cls >= 0 ? ((size_t)1 << (cls + FFC_MIN_LOG2)) : size;
    char *raw = NULL;
    ffc_hdr *h;
    if (cls >= 0) {
        AcquireSRWLockExclusive(&ffc_lock[cls]);
        if (ffc_top[cls] > 0)
            raw = ffc_stack[cls][--ffc_top[cls]];
        ReleaseSRWLockExclusive(&ffc_lock[cls]);
    }
    if (!raw) {
        raw = _aligned_malloc(cap + FFC_HDR, FFC_HDR);
        if (!raw)
            return NULL;
    }
    h = (ffc_hdr *)raw;
    h->magic = FFC_MAGIC;
    h->cls = cls;
    h->cap = cap;
    return raw + FFC_HDR;
}

static void ffc_free(void *ptr)
{
    char *raw;
    ffc_hdr *h;
    int cls;
    if (!ptr)
        return;
    raw = (char *)ptr - FFC_HDR;
    h = (ffc_hdr *)raw;
    cls = h->cls;
    if (cls >= 0 && ffc_enabled) {
        AcquireSRWLockExclusive(&ffc_lock[cls]);
        if (ffc_top[cls] < FFC_DEPTH) {
            ffc_stack[cls][ffc_top[cls]++] = raw;
            ReleaseSRWLockExclusive(&ffc_lock[cls]);
            return;
        }
        ReleaseSRWLockExclusive(&ffc_lock[cls]);
    }
    h->magic = 0;
    _aligned_free(raw);
}

static void *ffc_realloc(void *ptr, size_t size)
{
    ffc_hdr *h;
    void *n;
    if (!ptr)
        return ffc_alloc(size);
    h = (ffc_hdr *)((char *)ptr - FFC_HDR);
    if (size <= h->cap)
        return ptr;
    n = ffc_alloc(size);
    if (!n)
        return NULL;
    memcpy(n, ptr, h->cap);
    ffc_free(ptr);
    return n;
}
#define FFC_ENABLED 1
#else
void rex_ffmpeg_alloc_cache_enable(int enabled)
{
    (void)enabled;
}
#endif
