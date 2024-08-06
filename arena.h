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

// clang-format off
#define New(...)                       ARENA_NEWX(__VA_ARGS__, ARENA_NEW4, ARENA_NEW3, ARENA_NEW2)(__VA_ARGS__)
#define ARENA_NEWX(a, b, c, d, e, ...) e
#define ARENA_NEW2(a, t)               (t *)arena_alloc(a, sizeof(t), alignof(t), 1, 0)
#define ARENA_NEW3(a, t, n)            (t *)arena_alloc(a, sizeof(t), alignof(t), n, 0)
#define ARENA_NEW4(a, t, n, z)         (t *)arena_alloc(a, sizeof(t), alignof(t), n, z)

#define ARENA_OOM(A)                              \
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

#ifdef LOGGING
#  define ARENA_LOG(A)                                                   \
     fprintf(stderr, "%s:%d: Arena " #A "\tbeg=%ld end=%ld diff=%ld\n", \
             __FILE__,                                                  \
             __LINE__,                                                  \
            (uintptr_t)(*(A).beg),                                      \
            (uintptr_t)(A).end,                                         \
            (ssize)((A).end - (*(A).beg)))
#else
#  define ARENA_LOG(A)   ((void)0)
#endif

#ifndef NDEBUG
#  include <assert.h>
#elif defined(__GNUC__) // -fsantiize=undefined -fsanitize-trap=unreachable
#  define assert(c)  do if (!(c)) __builtin_unreachable(); while(0)
#elif defined(_MSC_VER)
#  define assert(c)  do if (!(c)) __debugbreak(); while(0)
#elif defined(__x86_64__)
#  define assert(c)  do if (!(c)) __asm__("int3; nop"); while(0)
#else
#  define assert(c)  do if (!(c)) __builtin_trap(); while(0)
#endif
// clang-format on

static inline Arena newarena(byte **mem, ssize size) {
  Arena a = {0};
  a.beg = mem;
  a.end = *mem ? *mem + size : 0;
  return a;
}

static inline bool isscratch(Arena *a) {
  return !!a->persist;
}

static inline Arena getscratch(Arena *a) {
  if (isscratch(a)) return *a;

  Arena scratch = {0};
  scratch.beg = &a->end;
  scratch.end = *a->beg;
  scratch.oomjmp = a->oomjmp;
  scratch.persist = a;
  return scratch;
}

static inline void *arena_alloc(Arena *a, ssize size, ssize align, ssize count, unsigned flags) {
  byte *ret = 0;

  if (isscratch(a)) {
    byte *newend = *a->persist->beg;
    if (*a->beg > newend) {
      a->end = newend;
    } else {
      goto oomjmp;
    }
  }

  if (*a->beg < a->end) {
    ssize avail = a->end - *a->beg;
    ssize padding = -(uintptr_t)*a->beg & (align - 1);
    if (count > (avail - padding) / size) goto oomjmp;
    ret = *a->beg + padding;
    *a->beg += padding + size * count;
  } else {
    ssize avail = *a->beg - a->end;
    ssize padding = +(uintptr_t)*a->beg & (align - 1);
    if (count > (avail - padding) / size) goto oomjmp;
    *a->beg -= padding + size * count;
    ret = *a->beg;
  }

  return flags & NOINIT ? ret : memset(ret, 0, size * count);

oomjmp:
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

  const int grow = 32;

  if (!replica.cap) {
    replica.cap = grow;
    replica.data = arena_alloc(a, size, align, replica.cap, 0);
  } else if ((*a->beg < a->end) &&
             ((uintptr_t)*a->beg - size * replica.cap == (uintptr_t)replica.data)) {
    // grow in place
    arena_alloc(a, size, 1, grow, 0);
    replica.cap += grow;
  } else {
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

#endif  // ARENA_H
