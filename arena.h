#ifndef ARENA_H
#define ARENA_H

/** Credit:
    https://nullprogram.com/blog/2023/09/27/
    https://nullprogram.com/blog/2023/10/05/
    https://www.chiark.greenend.org.uk/~sgtatham/quasiblog/c11-generic/#inline
*/

#include <memory.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ALIGN _Alignof(max_align_t)

#if __has_include("debug.h")
#include "debug.h"
#else
#include <assert.h>
#endif

#ifdef __GNUC__
static void autofree_impl(void *p) {
  free(*((void **)p));
}
#define autofree __attribute__((__cleanup__(autofree_impl)))
#else
#error "autofree is not supported"
#define autofree
#endif

#if defined(__GNUC__) && !defined(__APPLE__)
#undef setjmp
#define setjmp  __builtin_setjmp
#define longjmp __builtin_longjmp
#define _JBLEN  5
#else
#include <setjmp.h>
#endif

typedef ptrdiff_t isize;
typedef uint8_t byte;

typedef struct Arena Arena;
struct Arena {
  byte *beg;
  byte *end;
  void **jmpbuf;
};

enum {
  _NO_INIT = 1u << 0,   // don't `zero` alloced memory
  _OOM_NULL = 1u << 1,  // return NULL on OOM
  _PUSH_END = 1u << 2   // push a new arena at the end
};

typedef struct {
  unsigned mask;
} ArenaFlag;

const ArenaFlag NO_INIT = {_NO_INIT};
const ArenaFlag OOM_NULL = {_OOM_NULL};

/** Usage:

  enum { ARENA_SIZE = 1 << 20 };
  void *mem = malloc(ARENA_SIZE);
  Arena arena = NewArena(mem, ARENA_SIZE);

  if (ArenaOOM(&arena)) {
    abort();
  }

  {
    Shadow(arena);

    Ty *x = New(arena, Ty);

    // x pointer cannot escape this block

  }

  Ty a[] = New(&arena, Ty, 10);
  {
    Arena tmp[] = {PushArena(arena)};

    for (int i = 0; i < 10; ++i) {
      Ty *x = foo(tmp);
      // must move x *value* into a[i]
      a[i] = *x;
    }

    PopArena(arena, tmp);
  }


*/

#define New(...)                       ARENA_NEWX(__VA_ARGS__, ARENA_NEW4, ARENA_NEW3, ARENA_NEW2)(__VA_ARGS__)
#define ARENA_NEWX(a, b, c, d, e, ...) e
#define ARENA_NEW2(a, t)               (t *)arena_alloc(a, sizeof(t), _Alignof(t), 1, (ArenaFlag){0})
#define ARENA_NEW3(a, t, n)            (t *)arena_alloc(a, sizeof(t), _Alignof(t), n, (ArenaFlag){0})
#define ARENA_NEW4(a, t, n, z)                                       \
  (t *)_Generic((z), t *: arena_alloc_init, ArenaFlag: arena_alloc)( \
      a, sizeof(t), _Alignof(t), n, _Generic((z), t *: z, ArenaFlag: z))

#define ArenaOOM(A)                                 \
  ({                                                \
    Arena *a_ = (A);                                \
    a_->jmpbuf = New(a_, void *, _JBLEN, OOM_NULL); \
    !a_->jmpbuf || setjmp((void *)a_->jmpbuf);      \
  })

#define CONCAT0(a, b) a##b
#define CONCAT(a, b)  CONCAT0(a, b)

#define ARENA_PTR CONCAT(_arena_, __LINE__)
#define Shadow(arena)           \
  Arena *ARENA_PTR = arena;     \
  Arena arena[] = {*ARENA_PTR}; \
  LogArena(*arena)

#ifdef LOGGING
#define LogArena(A)                                                                      \
  fprintf(stderr, "%s:%d: Arena " #A "\tbeg=%ld end=%ld diff=%ld\n", __FILE__, __LINE__, \
          (uintptr_t)((A).beg), (uintptr_t)((A).end), (isize)((A).end - ((A).beg)))
#else
#define LogArena(A) ((void)A)
#endif

static inline Arena NewArena(byte *mem, isize size) {
  Arena a = {0};
  a.beg = mem;
  a.end = mem ? mem + size : 0;
  return a;
}

static inline void *arena_alloc(Arena *arena, isize size, isize align, isize count,
                                ArenaFlag flags) {
  assert(arena != NULL && "arena cannot be NULL");
  assert(count >= 0 && "count must be positive");

  byte *current = arena->beg;
  isize avail = arena->end - current;
  isize padding = -(uintptr_t)current & (align - 1);
  if (count > (avail - padding) / size) {
    goto handle_oom;
  }

  isize total_size = size * count;
  if (flags.mask & _PUSH_END) {
    arena->end -= total_size;
    current = arena->end;
  } else {
    arena->beg += padding + total_size;
    current += padding;
  }
  return flags.mask & _NO_INIT ? current : memset(current, 0, total_size);

handle_oom:
  if (flags.mask & _OOM_NULL || !arena->jmpbuf)
    return NULL;
#ifndef OOM_DIE
  longjmp((void *)arena->jmpbuf, 1);
#else
  assert(!OOM_DIE);
  abort();
#endif
}

static inline void *arena_alloc_init(Arena *arena, isize size, isize align, isize count,
                                     const void *const initptr) {
  assert(initptr != NULL && "initptr cannot be NULL");
  void *ptr = arena_alloc(arena, size, align, count, NO_INIT);
  return memmove(ptr, initptr, size * count);
}

static inline Arena PushArena(Arena *arena) {
  Arena tail = *arena;
  tail.beg = New(arena, byte, (arena->end - arena->beg) / 2, (ArenaFlag){_NO_INIT | _PUSH_END});
  return tail;
}

static inline void PopArena(Arena *head, Arena *tail) {
  assert(head->end <= tail->beg && "Invalid arena chain");
  head->end = tail->end;
}

#define Push(S, A)                                                             \
  ({                                                                           \
    __typeof__(S) s_ = (S);                                                    \
    if (s_->len >= s_->cap) {                                                  \
      slice_grow(s_, sizeof(*s_->data), _Alignof(__typeof__(*s_->data)), (A)); \
    }                                                                          \
    s_->data + s_->len++;                                                      \
  })

static inline void slice_grow(void *slice, isize size, isize align, Arena *a) {
  struct {
    void *data;
    isize len;
    isize cap;
  } slicemeta;
  memcpy(&slicemeta, slice, sizeof(slicemeta));
  assert(slicemeta.len >= 0 && "slice.len must be non-negative");
  assert(slicemeta.cap >= slicemeta.len && "slice.cap cannot be less than slice.len");

  const int grow = MAX_ALIGN;

  if (!slicemeta.cap) {
    // first alloc
    slicemeta.cap = grow;
    slicemeta.data = arena_alloc(a, size, align, slicemeta.cap, NO_INIT);
  } else if ((uintptr_t)slicemeta.data == (uintptr_t)a->beg - size * slicemeta.cap) {
    // grow inplace
    arena_alloc(a, size, 1, grow, NO_INIT);
    slicemeta.cap += grow;
  } else {
    slicemeta.cap += slicemeta.cap / 2;  // grow by 1.5
    void *dest = arena_alloc(a, size, align, slicemeta.cap, NO_INIT);
    void *src = slicemeta.data;
    isize len = size * slicemeta.len;
    // allow slice to be moved between overlapping arenas
    memmove(dest, src, len);
    slicemeta.data = dest;
  }

  memcpy(slice, &slicemeta, sizeof(slicemeta));
}

// Arena owned str (aka astr)
typedef struct astr {
  char *data;
  ptrdiff_t len;
} astr;

// string literal only!
#define astr(s) (astr){s, sizeof(s) - 1}

// printf("%.*s", S(s))
#define S(s) (int)(s).len, (s).data

static inline astr astrclone(Arena *arena, astr s) {
  astr s2 = s;
  // Early return if string is empty or already at arena boundary
  if (!s.len || s.data + s.len == (char *)arena->beg)
    return s2;

  s2.data = New(arena, char, s.len, NO_INIT);
  if (s2.data >= s.data + s.len || s2.data + s2.len <= s.data) {
    memcpy(s2.data, s.data, s.len);
  } else {
    memmove(s2.data, s.data, s.len);
  }
  return s2;
}

static inline astr astrconcat(Arena *arena, astr head, astr tail) {
  astr ret = head;
  // Ignore empty head
  if (head.len == 0) {
    // If tail is at arena tip, return it directly; otherwise duplicate
    return tail.len && tail.data + tail.len == (char *)arena->beg ? tail : astrclone(arena, tail);
  }
  // If head isn't at arena tip, duplicate it
  if (head.data + head.len != (char *)arena->beg) {
    ret = astrclone(arena, head);
  }
  // Now head is guaranteed to be at arena tip, clone tail and append it right after
  ret.len += astrclone(arena, tail).len;
  return ret;
}

static inline astr astrcopy(Arena *arena, const void *bytes, size_t nbytes) {
  return astrclone(arena, (astr){(char *)bytes, nbytes});
}

static inline astr astrappend(Arena *arena, astr head, const void *bytes, size_t nbytes) {
  return astrconcat(arena, head, (astr){(char *)bytes, nbytes});
}

static inline astr astrcpy(Arena *arena, const char *str) {
  return astrcopy(arena, str, strlen(str));
}

static inline astr astrcat(Arena *arena, astr head, const char *str) {
  return astrappend(arena, head, str, strlen(str));
}

static inline astr astrfmt(Arena *arena, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int nbytes = vsnprintf(NULL, 0, format, args);
  va_end(args);
  assert(nbytes >= 0);
  void *data = New(arena, char, nbytes + 1, NO_INIT);
  va_start(args, format);
  int nbytes2 = vsnprintf(data, nbytes + 1, format, args);
  va_end(args);
  assert(nbytes2 == nbytes);
  arena->beg--;
  return (astr){.data = data, .len = nbytes};
}

static inline bool astrcmp(astr a, astr b) {
  if (a.len != b.len)
    return false;

  return !a.len || !memcmp(a.data, b.data, a.len);
}

static uint64_t astrhash(astr key) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (isize i = 0; i < key.len; i++)
    hash = ((unsigned char)key.data[i] ^ hash) * 0x100000001b3ull;

  return hash;
}

#if __has_include("cc.h")
#include "cc.h"
#define CC_CMPR astr, return strncmp(val_1.data, val_2.data, val_1.len);
#define CC_HASH astr, return astrhash(val);
#endif

#endif  // ARENA_H
