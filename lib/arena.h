/*
 * arena.h - fxsh segmented nested arena allocator
 *
 * Design:
 *   - Segmented: linked list of fixed-size chunks, grows on demand
 *   - Nested: parent/child arenas for scope-based lifetime
 *   - No GC: scripts are short-lived; OS reclaims everything on exit
 *   - Mark/restore: used by type unifier to backtrack on failure
 */

#ifndef FXSH_ARENA_H
#define FXSH_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*=============================================================================
 * Configuration
 *=============================================================================*/

#ifndef ARENA_DEFAULT_SEG_SIZE
#define ARENA_DEFAULT_SEG_SIZE (64 * 1024) /* 64 KB per segment */
#endif

#define ARENA_ALIGN      8
#define ARENA_ALIGN_UP(n) (((size_t)(n) + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1))

/*=============================================================================
 * Segment (internal)
 *=============================================================================*/

typedef struct fxsh_arena_seg {
    struct fxsh_arena_seg *next; /* linked list of segments */
    size_t                 cap;  /* usable bytes in this segment */
    size_t                 used; /* bytes allocated so far */
    char                   data[]; /* flexible array: actual memory */
} fxsh_arena_seg_t;

/*=============================================================================
 * Arena Control Block
 *=============================================================================*/

typedef struct fxsh_arena {
    fxsh_arena_seg_t *head;     /* current (newest) active segment */
    struct fxsh_arena *parent;  /* parent arena for nested scopes */
    size_t             seg_size; /* default new segment size */
    size_t             total;    /* total bytes allocated (stats) */
    size_t             n_segs;   /* number of segments (stats) */
} fxsh_arena_t;

/*=============================================================================
 * Mark/Restore (for unification backtracking)
 *=============================================================================*/

typedef struct {
    fxsh_arena_seg_t *seg;   /* segment at time of mark */
    size_t            used;  /* used bytes at time of mark */
} fxsh_arena_mark_t;

/*=============================================================================
 * API (forward declarations)
 *=============================================================================*/

static inline void *arena_alloc(fxsh_arena_t *a, size_t size);

/* Create a new arena. Pass NULL for parent if top-level. */
static inline fxsh_arena_t *arena_create(fxsh_arena_t *parent, size_t seg_size) {
    if (seg_size == 0) seg_size = ARENA_DEFAULT_SEG_SIZE;

    /* Allocate the control block from the OS (or parent) */
    fxsh_arena_t *a;
    if (parent) {
        /* Embed control block in parent arena to avoid malloc */
        a = (fxsh_arena_t *)arena_alloc(parent, sizeof(fxsh_arena_t));
    } else {
        a = (fxsh_arena_t *)malloc(sizeof(fxsh_arena_t));
    }
    if (!a) return NULL;

    a->head     = NULL;
    a->parent   = parent;
    a->seg_size = seg_size;
    a->total    = 0;
    a->n_segs   = 0;
    return a;
}

/* Allocate a new segment of at least `min_size` bytes */
static inline fxsh_arena_seg_t *arena_new_seg(fxsh_arena_t *a, size_t min_size) {
    size_t cap = min_size > a->seg_size ? min_size : a->seg_size;
    fxsh_arena_seg_t *seg = (fxsh_arena_seg_t *)malloc(sizeof(fxsh_arena_seg_t) + cap);
    if (!seg) return NULL;
    seg->cap  = cap;
    seg->used = 0;
    seg->next = a->head;
    a->head   = seg;
    a->n_segs++;
    return seg;
}

/* Core allocation (no zeroing) */
static inline void *arena_alloc(fxsh_arena_t *a, size_t size) {
    if (!a) return malloc(size); /* fallback for NULL arena */
    size = ARENA_ALIGN_UP(size);
    if (size == 0) size = ARENA_ALIGN;

    /* Try current segment */
    fxsh_arena_seg_t *seg = a->head;
    if (seg && seg->used + size <= seg->cap) {
        void *ptr  = seg->data + seg->used;
        seg->used += size;
        a->total  += size;
        return ptr;
    }

    /* Need a new segment */
    seg = arena_new_seg(a, size);
    if (!seg) return NULL;
    void *ptr  = seg->data;
    seg->used  = size;
    a->total  += size;
    return ptr;
}

/* Zeroed allocation */
static inline void *arena_alloc_zero(fxsh_arena_t *a, size_t size) {
    void *p = arena_alloc(a, size);
    if (p) memset(p, 0, size);
    return p;
}

/* Aligned allocation (power-of-2 alignment) */
static inline void *arena_alloc_align(fxsh_arena_t *a, size_t size, size_t align) {
    /* Pad current usage to requested alignment, then alloc */
    if (!a) return malloc(size);
    fxsh_arena_seg_t *seg = a->head;
    size_t pad = 0;
    if (seg) {
        size_t cur = (size_t)(seg->data + seg->used);
        pad = (align - (cur & (align - 1))) & (align - 1);
    }
    /* Allocate pad + size */
    void *p = arena_alloc(a, pad + size);
    if (!p) return NULL;
    return (char *)p + pad;
}

/* Duplicate string into arena */
static inline char *arena_strdup(fxsh_arena_t *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char  *p   = (char *)arena_alloc(a, len);
    if (p) memcpy(p, s, len);
    return p;
}

static inline char *arena_strndup(fxsh_arena_t *a, const char *s, size_t n) {
    if (!s) return NULL;
    char *p = (char *)arena_alloc(a, n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

/* Mark current position (for backtracking) */
static inline fxsh_arena_mark_t arena_mark(fxsh_arena_t *a) {
    fxsh_arena_mark_t m;
    m.seg  = a->head;
    m.used = a->head ? a->head->used : 0;
    return m;
}

/* Restore to a previous mark (frees segments allocated after mark) */
static inline void arena_restore(fxsh_arena_t *a, fxsh_arena_mark_t m) {
    /* Free segments newer than the marked segment */
    while (a->head && a->head != m.seg) {
        fxsh_arena_seg_t *dead = a->head;
        a->head = dead->next;
        a->n_segs--;
        free(dead);
    }
    /* Restore used bytes in the marked segment */
    if (a->head) {
        a->head->used = m.used;
    }
}

/* Reset arena (keep all segments, just reset used counters) */
static inline void arena_reset(fxsh_arena_t *a) {
    for (fxsh_arena_seg_t *s = a->head; s; s = s->next) {
        s->used = 0;
    }
    a->total = 0;
}

/* Destroy arena and free all its segments */
static inline void arena_destroy(fxsh_arena_t *a) {
    if (!a) return;
    fxsh_arena_seg_t *seg = a->head;
    while (seg) {
        fxsh_arena_seg_t *next = seg->next;
        free(seg);
        seg = next;
    }
    a->head   = NULL;
    a->n_segs = 0;
    a->total  = 0;
    /* Only free control block if it was malloc'd (no parent) */
    if (!a->parent) {
        free(a);
    }
}

/* Statistics */
static inline size_t arena_used(fxsh_arena_t *a) {
    return a ? a->total : 0;
}

static inline size_t arena_total_segs(fxsh_arena_t *a) {
    return a ? a->n_segs : 0;
}

/*=============================================================================
 * Thread-local current arena (used by fxsh_alloc macro)
 *=============================================================================*/

#ifdef __GNUC__
static __thread fxsh_arena_t *fxsh_current_arena = NULL;
#else
static fxsh_arena_t *fxsh_current_arena = NULL;
#endif

/* Push/pop current arena (use in RAII style) */
#define ARENA_PUSH(a) \
    fxsh_arena_t *_fxsh_prev_arena = fxsh_current_arena; \
    fxsh_current_arena = (a)

#define ARENA_POP() \
    fxsh_current_arena = _fxsh_prev_arena

/* Allocation macros that use current arena */
#define fxsh_alloc(size)       arena_alloc(fxsh_current_arena, (size))
#define fxsh_alloc0(size)      arena_alloc_zero(fxsh_current_arena, (size))
#define fxsh_strdup(s)         arena_strdup(fxsh_current_arena, (s))
#define fxsh_strndup(s, n)     arena_strndup(fxsh_current_arena, (s), (n))
#define fxsh_new(T)            ((T*)arena_alloc_zero(fxsh_current_arena, sizeof(T)))
#define fxsh_new_n(T, n)       ((T*)arena_alloc_zero(fxsh_current_arena, sizeof(T) * (n)))

#endif /* FXSH_ARENA_H */
