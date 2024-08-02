#ifndef ARENA_H
#define ARENA_H

/** Credit:
    https://nullprogram.com/blog/2023/09/27/
    https://lists.sr.ht/~skeeto/public-inbox/%3C20231015233305.sssrgorhqu2qo5jr%40nullprogram.com%3E
    https://nullprogram.com/blog/2023/10/05/
*/

#include <memory.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef ptrdiff_t ssize;
typedef unsigned char byte;

typedef struct Arena Arena;
struct Arena {
  byte **beg;
  byte *end;
  void **oomjmp;
  Arena *persist;
};

enum {
  SOFTFAIL = 1 << 0,
  NOINIT = 1 << 1,
};

/** Usage:

  ssize cap = 1 << 20;
  void *heap = malloc(cap);
  Arena global = newarena(&(byte *){heap}, cap);

  if (ARENA_OOM(&global)) {
    abort();
  }

  Arena scratch = getscratch(&global);

  thing *x = New(&global, thing);
  thing *y = New(&scratch, thing);
  thing *z = helper(scratch);

  free(buffer);

*/
#define New(...)                       ARENA_NEWX(__VA_ARGS__, ARENA_NEW4, ARENA_NEW3, ARENA_NEW2)(__VA_ARGS__)
#define ARENA_NEWX(a, b, c, d, e, ...) e
#define ARENA_NEW2(a, t)               (t *)arena_alloc(a, sizeof(t), alignof(t), 1, 0)
#define ARENA_NEW3(a, t, n)            (t *)arena_alloc(a, sizeof(t), alignof(t), n, 0)
#define ARENA_NEW4(a, t, n, z)         (t *)arena_alloc(a, sizeof(t), alignof(t), n, z)

#define ArenaOOM(A)                              \
  ({                                             \
    Arena *a_ = (A);                             \
    a_->oomjmp = New(a_, void *, 5, SOFTFAIL);   \
    !a_->oomjmp || __builtin_setjmp(a_->oomjmp); \
  })

#define Push(S, A)                                               \
  ({                                                             \
    typeof(S) s_ = (S);                                          \
    if (s_->len >= s_->cap) {                                    \
      slice_grow(s_, sizeof(*s_->data), sizeof(*s_->data), (A)); \
    }                                                            \
    s_->data + s_->len++;                                        \
  })

// clang-format off
#ifdef NDEBUG
#  define LogArena(A)
#else
#  define LogArena(A)                                                   \
     fprintf(stderr, "%s:%d: Arena " #A "\tbeg=%ld end=%ld diff=%ld\n", \
             __FILE__,                                                  \
             __LINE__,                                                  \
            (uintptr_t)(*(A).beg),                                      \
            (uintptr_t)(A).end,                                         \
            (ssize)((A).end - (*(A).beg)))
#endif

#if defined(__GNUC__) && !defined(__COSMOCC__)
#  define assert(c)  while (!(c)) __builtin_unreachable()
#elif defined(NDEBUG) && defined(_MSC_VER)
#  define assert(c)  do if (!(c)) __debugbreak(); while (0)
#else
#  include <assert.h>
#endif

static inline Arena newarena(byte **mem, ssize size) {
  Arena a = {0};
  a.beg = mem;
  a.end = *mem ? *mem + size : 0;
  return a;
}

__attribute((malloc, alloc_size(2, 4), alloc_align(3))) static inline
void* arena_alloc(Arena *a, ssize size, ssize align, ssize count, unsigned flags) {
  // clang-format on
  byte *r = 0;
  // sync [2]
  if (a->persist) {
    byte *beg = *a->persist->beg;
    if (*a->beg < a->end) {
      a->end = beg < a->end ? beg : a->end;
      if (*a->beg > a->end) goto OOM_EXIT;
    } else {
      a->end = a->end < beg ? beg : a->end;
      if (*a->beg < a->end) goto OOM_EXIT;
    }
  }

  if (*a->beg < a->end) {
    ssize avail = a->end - *a->beg;
    ssize padding = -(uintptr_t)*a->beg & (align - 1);
    if (count > (avail - padding) / size) goto OOM_EXIT;
    r = *a->beg + padding;
    *a->beg += padding + size * count;
  } else {
    ssize avail = *a->beg - a->end;
    ssize padding = +(uintptr_t)*a->beg & (align - 1);
    if (count > (avail - padding) / size) goto OOM_EXIT;
    *a->beg -= padding + size * count;
    r = *a->beg;
  }

  return flags & NOINIT ? r : memset(r, 0, size * count);

OOM_EXIT:
  if (flags & SOFTFAIL || !a->oomjmp) return NULL;
#ifndef OOM
  assert(a->oomjmp);
  __builtin_longjmp(a->oomjmp, 1);
#else
  assert(!OOM);
#endif
}

static inline void slice_grow(void *slice, ssize size, ssize align, Arena *a) {
  struct {
    void *data;
    ssize len;
    ssize cap;
  } replica;
  memcpy(&replica, slice, sizeof(replica));

  int grow = 16;

  if (!replica.data) {
    replica.cap = grow;
    replica.data = arena_alloc(a, size, align, replica.cap, 0);
  } else if ((*a->beg < a->end &&
              ((uintptr_t)*a->beg - size * replica.cap == (uintptr_t)replica.data))) {
    // grow in place
    arena_alloc(a, size, 1, grow, 0);
    replica.cap += grow;
  } else {
    // grow in move
    replica.cap += grow;
    void *data = arena_alloc(a, size, align, replica.cap, 0);
    void *src = replica.data;
    void *dest = data;
    ssize len = size * replica.len;
    memcpy(dest, src, len);
    replica.data = data;
  }

  memcpy(slice, &replica, sizeof(replica));
}

static inline bool isscratch(Arena *a) {
  return *a->beg > a->end;
}

static inline Arena getscratch(Arena *a) {
  if (isscratch(a)) return *a;  // guard [2]

  Arena scratch = {0};
  scratch.beg = &a->end;
  scratch.end = *a->beg;
  scratch.oomjmp = a->oomjmp;
  scratch.persist = a;
  return scratch;
}

#endif  // ARENA_H
