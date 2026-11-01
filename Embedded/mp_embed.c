#define MONOLITHPY_EMBED_BUILD
#define _FILE_OFFSET_BITS 64
#include "mp_embed.h"
#define PATH_MAX 4096
#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
/* Pull in <io.h> so the fallthrough _lseeki64 / _lseek / _open / _close
 * calls below see the real CRT prototypes - returning __int64 in the
 * _lseeki64 case. Without this, MSVC implicitly declares _lseeki64 as
 * int-returning and silently truncates positions past 2 GB, breaking
 * ZipFile.tell() during large wheel writes (e.g. mpy-tool-flang). */
#include <io.h>
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <zstd.h>

#if _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define likely(x)       __builtin_expect(!!(x), 1)
#else
#define unlikely(x)     (x)
#define likely(x)       (x)
#endif

extern const char nuitka_embed_map;
extern const long nuitka_embed_map_len;

extern const char nuitka_embed_data;
extern const long nuitka_embed_data_len;

struct FDMAP_S {    // Virtual File Stream
  int fd;
  EFILE *f;
};
typedef struct FDMAP_S FDMAP;

FDMAP mp_fd2file[512] = {};
bool mp_fd2file_initialized = false;

#if defined(__linux__)
#include <sys/auxv.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
/* Protects slot allocation/free in the handle registries. The
 * std::ifstream threading torture (16 threads x 200 iters of
 * open/read/close) otherwise races on slot reuse. */
static pthread_mutex_t mp_fd2file_mutex = PTHREAD_MUTEX_INITIALIZER;
#define MP_REGISTRY_LOCK()   pthread_mutex_lock(&mp_fd2file_mutex)
#define MP_REGISTRY_UNLOCK() pthread_mutex_unlock(&mp_fd2file_mutex)
#endif

#ifdef _WIN32
static SRWLOCK mp_registry_lock = SRWLOCK_INIT;
#define MP_REGISTRY_LOCK()   AcquireSRWLockExclusive(&mp_registry_lock)
#define MP_REGISTRY_UNLOCK() ReleaseSRWLockExclusive(&mp_registry_lock)

/* Handles of virtual files. A CreateFile handle owns its EFILE; the OS handle
 * behind a virtual fd is fd_owned, and the fd's close releases it. */
struct HNDMAP_S {
    HANDLE h;
    EFILE *f;
    bool fd_owned;
};
typedef struct HNDMAP_S HNDMAP;

HNDMAP mp_handle2file[512] = {};

/* A mapping of a virtual file owns its own decompressed copy, so like a real
 * section object it outlives the file handle it was created from. */
typedef struct VMAPPING_S {
    char *data;
    size_t size;
    int views;
    bool closed;
} VMAPPING;

// A map to associate mapping HANDLEs with our virtual mappings.
struct VMAP_S {   // Virtual File Mapping
    HANDLE h;
    VMAPPING *m;
};
typedef struct VMAP_S VMAP;

// An array to store the mappings for open virtual files.
VMAP mp_mapping2file[512] = {};

// A map to associate view pointers with our virtual mappings
struct VIEWMAP_S { // Virtual File View
    LPVOID view;
    VMAPPING *m;
};
typedef struct VIEWMAP_S VIEWMAP;

VIEWMAP mp_view2file[512] = {};

// For FindFirst/NextFile functionality
typedef struct VIRTUAL_FIND_HANDLE_DATA_S {
    const EMAP* dir_map;        // The directory entry being iterated
    uint32_t next_child;        // Index into dir_map->children of next entry to return
    wchar_t search_pattern[MAX_PATH]; // The file pattern (e.g., "*.txt")
    HANDLE real;                // The same search on disk, or INVALID_HANDLE_VALUE
    bool real_ready;            // real_next holds its next match
    WIN32_FIND_DATAW real_next;
} VIRTUAL_FIND_HANDLE_DATA;

// A map to associate find handles with our virtual find data
struct FIND_HNDMAP_S {
    HANDLE h;
    VIRTUAL_FIND_HANDLE_DATA *data;
};
typedef struct FIND_HNDMAP_S FIND_HNDMAP;

FIND_HNDMAP mp_findhandles[512] = {};
#endif

/* ============================================================
 * FNV-1a 64 + CHD perfect hash + bloom
 * ============================================================ */

/* FNV-1a 64. Same as Python side. Path must already be lowercased. */
ALWAYS_INLINE uint64_t mp_hash64(const char *s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  while (*s) {
    h ^= (uint8_t)*s++;
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* CHD secondary mix: combines hash with seed to produce slot.
 * splitmix64 finalizer with a seed-derived salt mixed in. */
ALWAYS_INLINE uint64_t mp_chd_mix(uint64_t h, uint32_t seed) {
  uint64_t x = h ^ ((uint64_t)seed * 0x9E3779B97F4A7C15ULL);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x = x ^ (x >> 31);
  return x;
}

/* Lazy-initialised pointers into the map blob. Valid after first
 * mp_embed_init() call. Subsequent calls are noops. The blob is
 * static const, so multiple threads racing here is benign (every
 * thread computes the same pointer value). */
static const mp_embed_header_t *mp_hdr        = NULL;
static const uint64_t          *mp_bloom      = NULL;
static const uint32_t          *mp_seeds      = NULL;
static const uint32_t          *mp_child_slots = NULL;
static const EMAP              *mp_entries    = NULL;
static const char              *mp_paths      = NULL;
static const char              *mp_mtimes     = NULL;  /* NULL if the blob has none */

ALWAYS_INLINE void mp_embed_init(void) {
  if (likely(mp_hdr != NULL)) return;
  if (nuitka_embed_map_len < (long)sizeof(mp_embed_header_t)) {
    /* Empty / malformed blob - leave pointers NULL; lookups return miss. */
    return;
  }
  const char *base = &nuitka_embed_map;
  const mp_embed_header_t *h = (const mp_embed_header_t *)base;
  if (h->num_entries == 0) {
    /* No entries; treat as empty. Still set mp_hdr so we don't reinit. */
    mp_hdr = h;
    return;
  }
  mp_bloom       = (const uint64_t *)(base + h->bloom_offset);
  mp_seeds       = (const uint32_t *)(base + h->seeds_offset);
  mp_child_slots = (const uint32_t *)(base + h->child_slots_offset);
  mp_entries     = (const EMAP     *)(base + h->entries_offset);
  mp_paths       = (const char     *)(base + h->paths_offset);
  if (h->mtimes_offset != 0) mp_mtimes = base + h->mtimes_offset;
  mp_hdr         = h;  /* publish last so other threads see consistent state */
}

/* Bloom filter check. Returns true if path *might* be present. */
ALWAYS_INLINE bool mp_bloom_check(uint64_t h) {
  uint32_t k     = mp_hdr->bloom_num_hashes;
  uint32_t bits  = mp_hdr->bloom_num_bits;
  for (uint32_t i = 0; i < k; i++) {
    uint64_t mixed = h * 0x9E3779B97F4A7C15ULL
                   + (uint64_t)i * 0xbf58476d1ce4e5b9ULL;
    uint32_t bit = (uint32_t)(mixed % bits);
    if (!(mp_bloom[bit >> 6] & (1ULL << (bit & 63)))) return false;
  }
  return true;
}

/* Get path string for an entry. Points into nuitka_embed_map. */
ALWAYS_INLINE const char *mp_path_of(const EMAP *e) {
  return mp_paths + e->path_offset;
}

/* Public-but-undocumented entry point for tests (exposed via the
 * non-inlined wrapper below). */
const EMAP *mp_embed_lookup_for_test(const char *path);

/* Core lookup. Path must be already lowercased & normalized.
 * Returns NULL on miss. */
ALWAYS_INLINE const EMAP *mp_embed_find(const char *path) {
  mp_embed_init();
  if (unlikely(mp_hdr == NULL || mp_hdr->num_entries == 0)) return NULL;

  uint64_t h = mp_hash64(path);

  /* Fast-reject via bloom. ~1% false positive at 12 bits/entry, k=7. */
  if (!mp_bloom_check(h)) return NULL;

  /* CHD lookup: bucket -> seed -> slot. */
  uint32_t bucket = (uint32_t)(h % mp_hdr->num_buckets);
  uint32_t seed   = mp_seeds[bucket];
  uint32_t slot   = (uint32_t)(mp_chd_mix(h, seed) % mp_hdr->num_entries);
  const EMAP *e   = &mp_entries[slot];

  /* 64-bit hash equality is the verification. CHD guarantees the slot
   * is correct iff the hash matches; collisions are vetted absent at
   * gen time. */
  if (likely(e->hash == h)) return e;
  return NULL;
}

/* MP_EMBED_DEBUG is read once: getenv() on every intercepted call is slow and
 * races with setenv() from other threads. */
static int mp_debug_flag = -1;
static bool mp_debug(void) {
  if (unlikely(mp_debug_flag < 0)) {
    const char *dbg = getenv("MP_EMBED_DEBUG");
    mp_debug_flag = (dbg && *dbg) ? 1 : 0;
  }
  return mp_debug_flag == 1;
}

/* ASCII-only, locale-independent; mkembeddata.py folds the same way, so
 * UTF-8 bytes of non-ASCII names pass through untouched. */
ALWAYS_INLINE char mp_ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Backwards-compatible wrapper: existing code does
 *   if (find_embedded_file(path, &map)) ...
 * with `EMAP *map`. Cast away const since the callers don't write. */
static bool find_embedded_file(char *search_path, EMAP **found_map) {
  /* Lowercase the input path. (get_virtual_path already does this for
   * paths derived from absolute paths, but some call sites pass other
   * paths through.) */
  for (char *p = search_path; *p; ++p) *p = mp_ascii_lower(*p);
  if (mp_debug()) {
    fprintf(stderr, "[mp_embed] find: '%s'\n", search_path);
  }
  const EMAP *e = mp_embed_find(search_path);
  if (mp_debug()) {
    fprintf(stderr, "[mp_embed] -> %s\n", e ? "HIT" : "MISS");
  }
  if (!e) return false;
  *found_map = (EMAP *)e;
  return true;
}

/* Iterate children of a directory entry. start/count come from the
 * entry; each child slot is in mp_child_slots[]. */
ALWAYS_INLINE const EMAP *mp_embed_child_at(const EMAP *dir, uint32_t i) {
  uint32_t slot = mp_child_slots[dir->children_start + i];
  return &mp_entries[slot];
}

/* Externally-callable wrapper around mp_embed_find for tests. */
const EMAP *mp_embed_lookup_for_test(const char *path) {
  /* Caller is expected to pass a lowercased, normalised virtual path. */
  return mp_embed_find(path);
}

/* Populate an EFILE for a virtual file. Initializes both the
 * stream-position fields (pos/end/size) and (Windows only) the
 * CRT-buffer-mirror fields (crt_base/crt_ptr/crt_cnt) that MSVC's
 * basic_filebuf reads via _get_stream_buffer_pointers. */
/* Free an EFILE wrapper. Releases the decompressed file buffer for
 * virtual entries before freeing the wrapper itself. Safe to call on
 * uninitialised EFILEs as long as they were calloc'd (owned_buf NULL). */
ALWAYS_INLINE void mp_efile_destroy(EFILE *e) {
  if (!e) return;
  if (e->handle_type == EHANDLE_VIRTUAL && e->owned_buf) {
    free(e->owned_buf);
  }
  free(e);
}

/* Look up the uncompressed size of a virtual file without decompressing
 * it. Returns 0 for empty files and on error. Used by stat() / fstat()
 * to report the user-visible file size while keeping the blob compressed. */
static size_t mp_uncompressed_size(const EMAP *map) {
  if (!map || map->data_size == 0) return 0;
  const char *src = (const char*)&nuitka_embed_data + map->data_pos;
  unsigned long long ucs = ZSTD_getFrameContentSize(src, map->data_size);
  if (ucs == ZSTD_CONTENTSIZE_ERROR || ucs == ZSTD_CONTENTSIZE_UNKNOWN) return 0;
  return (size_t)ucs;
}

/* When the entry's source was last modified, in nanoseconds since the
 * epoch; 0 for a blob without mtimes. */
static int64_t mp_mtime_ns(const EMAP *map) {
  if (mp_mtimes == NULL) return 0;
  int64_t ns;
  memcpy(&ns, mp_mtimes + sizeof ns * (size_t)(map - mp_entries), sizeof ns);
  return ns;
}

/* Decompress the zstd frame for `map` into a freshly malloc'd buffer: the
 * content follows `headroom' zero bytes and is followed by one spare byte,
 * so the buffer is never empty. Returns NULL (errno set) on failure.
 * *out_size receives the uncompressed size. */
static char *mp_decompress_entry(const EMAP *map, size_t headroom, size_t *out_size) {
  const char *src = (const char*)&nuitka_embed_data + map->data_pos;
  size_t ucs = 0;
  /* Empty files have no frame at all. */
  if (map->data_size != 0) {
    unsigned long long n = ZSTD_getFrameContentSize(src, map->data_size);
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN) {
      errno = EIO;
      return NULL;
    }
    ucs = (size_t)n;
  }
  char *buf = (char*)malloc(headroom + ucs + 1);
  if (!buf) {
    errno = ENOMEM;
    return NULL;
  }
  memset(buf, 0, headroom);
  if (ucs != 0) {
    size_t n = ZSTD_decompress(buf + headroom, ucs, src, map->data_size);
    if (ZSTD_isError(n) || n != ucs) {
      free(buf);
      errno = EIO;
      return NULL;
    }
  }
  *out_size = ucs;
  return buf;
}

/* Returns false (errno set) if the entry cannot be decompressed; the EFILE
 * must then not be handed out, or it would read as an empty file. */
static bool mp_efile_init_virtual(EFILE *e, const EMAP *map) {
  size_t ucs = 0;
  char *buf = NULL, *base = NULL;
  if (map->type == ETYPE_FILE) {
    /* One byte of headroom lets ungetc() push back before the first byte. */
    buf = mp_decompress_entry(map, 1, &ucs);
    if (buf == NULL) return false;
    base = buf + 1;
  }
  e->handle_type = EHANDLE_VIRTUAL;
  e->name        = mp_path_of(map);
  e->pos         = base;
  e->end         = base ? base + ucs : NULL;
  e->size        = ucs;
  e->err         = 0;
  e->f           = NULL;
  e->map         = (EMAP*)map;
  e->owned_buf   = buf;     /* free in fclose/close */
#ifdef _WIN32
  e->crt_base    = base;
  e->crt_ptr     = base;
  e->crt_cnt     = (int)ucs;
#endif
  e->unget_saved = 0;
  return true;
}

#ifdef _WIN32
/* For handles that only ever answer metadata queries (CreateFile is how
 * Python's os.stat() looks at a file): no decompression. */
ALWAYS_INLINE void mp_efile_init_metadata(EFILE *e, const EMAP *map) {
  e->handle_type = EHANDLE_VIRTUAL;
  e->name        = mp_path_of(map);
  e->size        = mp_uncompressed_size(map);
  e->map         = (EMAP*)map;
}
#endif

/* Bytes left to read. The position may lie past the end after an lseek()
 * beyond EOF, which is legal and reads as EOF. */
ALWAYS_INLINE size_t mp_avail(const EFILE *e) {
  return e->pos < e->end ? (size_t)(e->end - e->pos) : 0;
}

/* EFILE.err bits of a virtual stream, as feof()/ferror() report them. */
#define MP_EFILE_EOF 1
#define MP_EFILE_ERR 2

/* Puts back the byte an ungetc() overwrote, once the stream is repositioned
 * or the pushed-back byte has been read. */
ALWAYS_INLINE void mp_unget_restore(EFILE *e) {
  if (e->unget_saved != 0) {
    ((char*)e->owned_buf)[e->unget_at] = (char)(e->unget_saved & 0xff);
    e->unget_saved = 0;
  }
}

/* ungetc() on a virtual stream: the next read returns `c'. As stdio
 * guarantees, one byte that differs from the content can be pending. */
static bool mp_unget(EFILE *e, unsigned char c) {
  char *buf = (char*)e->owned_buf;
  if (buf == NULL || e->pos <= buf || e->pos > e->end) return false;
  if (e->unget_saved != 0 && e->pos > buf + e->unget_at) mp_unget_restore(e);
  char *at = (char*)e->pos - 1;
  if ((unsigned char)*at != c) {
    if (e->unget_saved != 0) return false;
    e->unget_saved = 0x100 | (unsigned char)*at;
    e->unget_at = (size_t)(at - buf);
    *at = (char)c;
  }
  e->pos = at;
  e->err &= ~MP_EFILE_EOF;
  return true;
}

#if defined(__linux__) || defined(__APPLE__)
/* ============================================================
 * POSIX: virtual files served as real FILE* over a fake-but-real fd.
 *
 * libstdc++.a (libstdc++/libc++) implements basic_filebuf on top of
 * `__basic_file<char>`, which calls fopen() to get a FILE*, then
 * fileno() to get the fd, then read()/lseek()/fstat()/close() at the
 * fd layer - it does NOT use fread on the FILE*. So a fopencookie/
 * funopen FILE* (which has fileno = -1) doesn't help: basic_filebuf
 * would try read(-1) and fail with EBADF.
 *
 * Instead, mp_fopen for a virtual file:
 *   1. allocates an EFILE wrapper around the embed entry,
 *   2. opens a real fd (open("/dev/null") - cheap, gives a unique
 *      small int),
 *   3. registers fd -> EFILE in mp_fd2file,
 *   4. fdopen()s that fd into a real FILE*, which it returns.
 *
 * The build links with `-Wl,--wrap=fopen --wrap=read --wrap=lseek
 * --wrap=fstat --wrap=close` so libstdc++.a's calls go through our
 * __wrap_* functions below. Each wrap looks the fd up in mp_fd2file:
 *   - hit  -> serve from embed memory zero-copy
 *   - miss -> __real_* (the un-wrapped libc symbol)
 *
 * Result: std::ifstream sees a real FILE* / real fd, but every byte
 * it reads comes straight out of the .rodata embed blob with no
 * intermediate copy and no temp file.
 * ============================================================ */
#endif /* __linux__ || __APPLE__ */

#ifdef _WIN32
/* Synchronisation between mp_X intercepts (which read e->pos) and
 * MSVC basic_filebuf (which gets &e->crt_ptr / &e->crt_cnt from
 * _get_stream_buffer_pointers and advances them itself as it reads
 * via the streambuf cursor).
 *
 * mp_sync_in: pull the streambuf's view back into e->pos before our
 * intercept reads e->pos.
 * mp_sync_out: push e->pos back into the streambuf's view after our
 * intercept updates the position. */
ALWAYS_INLINE void mp_sync_in(EFILE *e) {
  if (e->handle_type != EHANDLE_VIRTUAL) return;
  e->pos = e->crt_ptr;
}
ALWAYS_INLINE void mp_sync_out(EFILE *e) {
  if (e->handle_type != EHANDLE_VIRTUAL) return;
  e->crt_ptr = (char*)e->pos;
  e->crt_cnt = (int)mp_avail(e);
}

/* Implementation of the _get_stream_buffer_pointers intercept.
 * For virtual files, return pointers to our crt_* mirror fields - this
 * lets basic_filebuf treat our embedded blob as the CRT's read buffer
 * and consume bytes via streambuf cursor advancement (zero-copy). */
MP_DECL(int) mp__get_stream_buffer_pointers(void *e, char ***pBase, char ***pPointer, int **pCount) {
  if (MP_FOREIGN_PTR) {
    return _get_stream_buffer_pointers((FILE*)e, pBase, pPointer, pCount);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return _get_stream_buffer_pointers(((EFILE*)e)->f, pBase, pPointer, pCount);
  }
  EFILE *ef = (EFILE*)e;
  if (mp_debug()) {
    fprintf(stderr, "[mp_embed] _get_stream_buffer_pointers: pos=%p crt_ptr=%p crt_cnt=%d\n",
            (void*)ef->pos, (void*)ef->crt_ptr, ef->crt_cnt);
  }
  /* Pull the streambuf's view back into pos first - it may have advanced
   * crt_ptr past pos via direct cursor reads. */
  ef->pos = ef->crt_ptr;
  ef->crt_cnt = (int)mp_avail(ef);
  if (pBase)    *pBase    = &ef->crt_base;
  if (pPointer) *pPointer = &ef->crt_ptr;
  if (pCount)   *pCount   = &ef->crt_cnt;
  return 0;
}
#endif  /* _WIN32 */

/* Expose the get_virtual_path pipeline for tests. */
void mp_resolve_virtual_path_for_test(const char *input, char *out, size_t out_size);

#if defined(__linux__) || defined(__APPLE__)
static pthread_once_t mp_fd2file_init_once = PTHREAD_ONCE_INIT;
static void mp_fd2file_init_impl(void) {
    for (int i = 0; i < 512; ++i) {
      mp_fd2file[i].fd = -1;
      mp_fd2file[i].f = NULL;
    }
    mp_fd2file_initialized = true;
}
ALWAYS_INLINE void init_fd2file() {
  /* pthread_once guarantees the init body runs exactly once across
   * all threads. Without this, two concurrent first-callers would
   * both run the loop and one's writes could clobber slot entries
   * that the other already registered. */
  pthread_once(&mp_fd2file_init_once, mp_fd2file_init_impl);
}
#else
ALWAYS_INLINE void init_fd2file() {
  if (unlikely(!mp_fd2file_initialized)) {
    MP_REGISTRY_LOCK();
    if (!mp_fd2file_initialized) {
      for (int i = 0; i < 512; ++i) {
        mp_fd2file[i].fd = -1;
        // The pointer 'f' is already NULL due to the initial zero-initialization
        // but it's good practice to be explicit if the intent isn't just zeroing.
        mp_fd2file[i].f = NULL;
      }
      mp_fd2file_initialized = true;
    }
    MP_REGISTRY_UNLOCK();
  }
}
#endif

/* fd -> EFILE registry for virtual files opened at the fd level. Slots are
 * claimed and released under the registry lock; lookups need none, as a
 * thread only ever looks up fds it owns. fd -1 marks a free slot, so a
 * negative fd must never be looked up. */
static bool mp_fd_register(int fd, EFILE *e) {
  init_fd2file();
#ifdef _WIN32
  HANDLE h = (HANDLE)_get_osfhandle(fd);
#endif
  bool registered = false;
  MP_REGISTRY_LOCK();
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == -1) {
      mp_fd2file[i].f = e;
      mp_fd2file[i].fd = fd;
      registered = true;
      break;
    }
  }
#ifdef _WIN32
  /* Python's fstat() asks the fd's OS handle, which is the executable's:
   * answer for the virtual file instead. */
  for (int i = 0; registered && h != INVALID_HANDLE_VALUE && i < 512; i++) {
    if (mp_handle2file[i].h == NULL) {
      mp_handle2file[i].f = e;
      mp_handle2file[i].fd_owned = true;
      mp_handle2file[i].h = h;
      break;
    }
  }
#endif
  MP_REGISTRY_UNLOCK();
  if (!registered) errno = EMFILE;
  return registered;
}

static EFILE *mp_fd_lookup(int fd) {
  if (fd < 0 || !mp_fd2file_initialized) return NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) return mp_fd2file[i].f;
  }
  return NULL;
}

/* Must run before the kernel fd is closed: otherwise another thread's open()
 * can be handed the same number while the slot still points here. */
static EFILE *mp_fd_unregister(int fd) {
  if (fd < 0 || !mp_fd2file_initialized) return NULL;
  EFILE *e = NULL;
  MP_REGISTRY_LOCK();
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      mp_fd2file[i].fd = -1;
      mp_fd2file[i].f = NULL;
      break;
    }
  }
#ifdef _WIN32
  for (int i = 0; e != NULL && i < 512; i++) {
    if (mp_handle2file[i].fd_owned && mp_handle2file[i].f == e) {
      mp_handle2file[i].h = NULL;
      mp_handle2file[i].f = NULL;
      mp_handle2file[i].fd_owned = false;
    }
  }
#endif
  MP_REGISTRY_UNLOCK();
  return e;
}

static int mp_fd_of(const EFILE *e) {
  if (!mp_fd2file_initialized) return -1;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].f == e && mp_fd2file[i].fd != -1) return mp_fd2file[i].fd;
  }
  return -1;
}

// Normalize path by resolving ".." and "."
void mp_normalize_path(char *path) {
  char resolved[PATH_MAX];
  char *tokens[PATH_MAX];  // Array to store path segments
  int depth = 0;

  /* Tokenize by "/". Use the reentrant variant (strtok_r on POSIX,
   * strtok_s on Windows) - plain strtok keeps an internal static
   * cursor and corrupts under concurrent calls from multiple threads,
   * which manifests as fopen returning NULL for valid virtual paths. */
#ifdef _WIN32
  char *saveptr = NULL;
  char *token = strtok_s(path, "/", &saveptr);
#else
  char *saveptr = NULL;
  char *token = strtok_r(path, "/", &saveptr);
#endif
  while (token) {
    if (strcmp(token, "..") == 0) {
      if (depth > 0) depth--;  // Go up a directory (if possible)
    } else if (strcmp(token, ".") != 0) {
      tokens[depth++] = token;  // Add to valid path segments
    }
#ifdef _WIN32
    token = strtok_s(NULL, "/", &saveptr);
#else
    token = strtok_r(NULL, "/", &saveptr);
#endif
  }

  // Reconstruct the normalized path
  resolved[0] = '\0';  // Empty the buffer
  for (int i = 0; i < depth; i++) {
    strcat(resolved, "/");
    strcat(resolved, tokens[i]);
  }

  if (depth == 0) strcpy(resolved, "/");  // Root case

  // Copy back the resolved path
  strcpy(path, resolved);
}

#ifdef _WIN32

/**
 * @brief Frees the memory allocated for the path components array.
 * * @param components The array of path components.
 * @param count The number of components in the array.
 */
inline void FreePathComponents(char** components, int count) {
    if (components == NULL) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(components[i]); // Free each individual string.
    }
    free(components); // Free the array of pointers itself.
}

/**
 * @brief Decomposes a file path into its constituent parts (directories/file).
 * * @param path The wide character string path to decompose.
 * @param out_component_count Pointer to an integer that will receive the number of components.
 * @return A dynamically allocated array of wide character strings. The caller is responsible
 * for freeing this array and its contents using FreePathComponents().
 * Returns NULL on failure.
 */
inline char** DecomposePath(const char* path, int* out_component_count) {
    if (path == NULL || out_component_count == NULL) {
        return NULL;
    }

    // Create a mutable copy of the path because wcstok_s modifies the string.
    char* path_copy = _strdup(path);
    if (path_copy == NULL) {
        perror("Failed to duplicate path string");
        return NULL;
    }

    // Initial allocation for component pointers. We'll reallocate if more space is needed.
    int capacity = 8;
    char** components = (char**)malloc(capacity * sizeof(char*));
    if (components == NULL) {
        perror("Failed to allocate memory for components");
        free(path_copy);
        return NULL;
    }

    int count = 0;
    char* context = NULL; // For wcstok_s
    char* token = strtok_s(path_copy, "\\/", &context);

    while (token != NULL) {
        // If we've run out of space, double the capacity.
        if (count >= capacity) {
            capacity *= 2;
            char** new_components = (char**)realloc(components, capacity * sizeof(char*));
            if (new_components == NULL) {
                perror("Failed to reallocate memory for components");
                FreePathComponents(components, count); // Clean up what we have so far
                free(path_copy);
                return NULL;
            }
            components = new_components;
        }

        // Duplicate the token and store it.
        components[count] = _strdup(token);
        if (components[count] == NULL) {
             perror("Failed to duplicate path component");
             FreePathComponents(components, count);
             free(path_copy);
             return NULL;
        }
        count++;
        token = strtok_s(NULL, "\\/", &context);
    }

    free(path_copy); // Free the mutable copy of the path.
    *out_component_count = count;
    return components;
}


/**
 * @brief Expands a short path to its longest possible form, even for non-existent files.
 * * @param short_path The path to expand, which may contain short names (e.g., "PROGRA~1").
 * @param long_path_buffer A buffer to store the expanded long path.
 * @param buffer_size The size of the long_path_buffer in characters.
 * @return TRUE on success, FALSE on failure.
 */
inline BOOL ExpandShortPath(const char* short_path, char* long_path_buffer, DWORD buffer_size) {
    // First, try to get the long path directly. This works if the path exists.
    if (GetLongPathNameA(short_path, long_path_buffer, buffer_size) > 0) {
        return TRUE;
    }

    // If GetLongPathNameW fails, it's likely because the path does not exist.
    // We'll expand it component by component.
    int component_count = 0;
    char** components = DecomposePath(short_path, &component_count);
    if (components == NULL || component_count == 0) {
        // If decomposition fails, copy original path as a fallback.
        strcpy_s(long_path_buffer, buffer_size, short_path);
        return FALSE;
    }

    char current_path[PATH_MAX] = { 0 };
    int start_index = 0;

    // Handle drive letter paths (e.g., "C:").
    if (strlen(components[0]) == 2 && components[0][1] == L':') {
        strcpy_s(current_path, PATH_MAX, components[0]);
        start_index = 1;
    }

    BOOL found_non_existent = FALSE;
    for (int i = start_index; i < component_count; ++i) {
        char temp_path[PATH_MAX];

        // Append the next component to the current path.
        if (strlen(current_path) > 0 && current_path[strlen(current_path) - 1] != L'\\') {
            strcat_s(current_path, PATH_MAX, "\\");
        }
        strcat_s(current_path, PATH_MAX, components[i]);

        // Try to get the long name for the path constructed so far.
        if (GetLongPathNameA(current_path, temp_path, PATH_MAX) > 0) {
            // If successful, update current_path with the expanded version.
            strcpy_s(current_path, PATH_MAX, temp_path);
        } else {
            // This is the first component that doesn't exist.
            // We append the rest of the components without further expansion.
            for (int j = i + 1; j < component_count; ++j) {
                strcat_s(current_path, PATH_MAX, "\\");
                strcat_s(current_path, PATH_MAX, components[j]);
            }
            found_non_existent = TRUE;
            break;
        }
    }

    // Copy the final constructed path to the output buffer.
    strcpy_s(long_path_buffer, buffer_size, current_path);

    // Clean up the memory allocated by DecomposePath.
    FreePathComponents(components, component_count);

    return TRUE;
}
#endif

// Cross-platform function to get absolute path without requiring the path to exist
void mp_get_absolute_path(const char *relative_path, char *absolute_path, size_t size) {
#ifdef _WIN32
  char full_path[PATH_MAX] = {};
  // Windows: _fullpath resolves ".." and "." even if path does not exist
  if (_fullpath(full_path, relative_path, size) == NULL) {
    perror("_fullpath failed");
  }
  if (!ExpandShortPath(full_path, absolute_path, size)) {
    perror("Failed to expand the path.");
  }
#else
  // Linux/macOS: Use getcwd() and manual normalization
  if (relative_path[0] == '/') {
    // Already an absolute path
    strncpy(absolute_path, relative_path, size - 1);
    absolute_path[size - 1] = '\0';
  } else {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      snprintf(absolute_path, size, "%s/%s", cwd, relative_path);
    } else {
      perror("getcwd failed");
      return;
    }
  }
  mp_normalize_path(absolute_path);  // Normalize ".." and "."
#endif
}

/* The executable's location never changes during the process lifetime, so it
 * is resolved once. On miss-heavy workloads (most fopen calls aren't for
 * embedded data) re-deriving it per call dominated the embed lookup. */

#define MP_NOT_IN_FOLDER ((size_t) -1)

#ifdef _WIN32
static wchar_t    mp_executable_w[PATH_MAX];
/* Folder of the executable as launched, with 8.3 names expanded. */
static wchar_t    mp_execfolder_w[PATH_MAX];
/* Folder of the executable's final path (junctions and symlinks resolved),
 * which Python's getpath takes its prefix from; empty if the same folder. */
static wchar_t    mp_execfolder_alias_w[PATH_MAX];
static bool       mp_exec_ok = false;
static INIT_ONCE  mp_exec_once = INIT_ONCE_STATIC_INIT;

/* "\\?\C:\x" -> "C:\x", "\\?\UNC\srv\x" -> "\\srv\x". */
static void mp_win_strip_verbatim(wchar_t *path) {
  if (wcsncmp(path, L"\\\\?\\UNC\\", 8) == 0) {
    memmove(path + 2, path + 8, (wcslen(path + 8) + 1) * sizeof(wchar_t));
  } else if (wcsncmp(path, L"\\\\?\\", 4) == 0 && path[4] != L'\0' && path[5] == L':') {
    memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof(wchar_t));
  }
}

/* Expands 8.3 names in the longest prefix of `path' that exists on disk. */
static void mp_win_long_path(wchar_t *path) {
  size_t len = wcslen(path);
  size_t stop = (len >= 2 && path[1] == L':') ? 2 : 0;
  for (size_t cut = len; cut > stop;) {
    wchar_t saved = path[cut];
    path[cut] = L'\0';
    wchar_t expanded[PATH_MAX];
    DWORD n = GetLongPathNameW(path, expanded, PATH_MAX);
    path[cut] = saved;
    if (n > 0 && n < PATH_MAX) {
      if (n + (len - cut) < PATH_MAX) {
        wcscpy(expanded + n, path + cut);
        wcscpy(path, expanded);
      }
      return;
    }
    while (cut > 0 && path[cut - 1] != L'\\') cut--;
    if (cut > 0) cut--;
  }
}

static BOOL CALLBACK mp_exec_init(PINIT_ONCE once, PVOID param, PVOID *context) {
  (void)once; (void)param; (void)context;
  DWORD n = GetModuleFileNameW(NULL, mp_executable_w, PATH_MAX);
  if (n == 0 || n >= PATH_MAX) return TRUE;
  wcscpy(mp_execfolder_w, mp_executable_w);
  mp_win_strip_verbatim(mp_execfolder_w);
  if (!PathRemoveFileSpecW(mp_execfolder_w)) return TRUE;
  if (wcschr(mp_execfolder_w, L'~')) mp_win_long_path(mp_execfolder_w);
  HANDLE h = CreateFileW(mp_executable_w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (h != INVALID_HANDLE_VALUE) {
    wchar_t final_path[PATH_MAX];
    DWORD len = GetFinalPathNameByHandleW(h, final_path, PATH_MAX, VOLUME_NAME_DOS);
    CloseHandle(h);
    if (len > 0 && len < PATH_MAX) {
      mp_win_strip_verbatim(final_path);
      if (PathRemoveFileSpecW(final_path) &&
          CompareStringOrdinal(final_path, -1, mp_execfolder_w, -1, TRUE) != CSTR_EQUAL) {
        wcscpy(mp_execfolder_alias_w, final_path);
      }
    }
  }
  mp_exec_ok = true;
  return TRUE;
}
#else
static char       mp_executable[PATH_MAX];
static char       mp_execfolder[PATH_MAX];
/* The folder Python's getpath derives from the launch path. It follows
 * symlinks in the final component only (readlink, never realpath), so
 * directory symlinks such as /tmp -> /private/tmp on macOS survive. Empty
 * when it is the same as mp_execfolder. */
static char       mp_execfolder_alias[PATH_MAX];
static dev_t      mp_exec_dev = 0;
static bool       mp_exec_ok = false;
static pthread_once_t mp_exec_once = PTHREAD_ONCE_INIT;

static void mp_parent_dir(char *path) {
  char *slash = strrchr(path, '/');
  if (slash == path) slash[1] = '\0';
  else if (slash != NULL) *slash = '\0';
}

static void mp_set_execfolder_alias(const char *launched) {
  char path[PATH_MAX] = {0};
  mp_get_absolute_path(launched, path, PATH_MAX);
  for (int links = 0; path[0] == '/' && links < 40; links++) {
    char target[PATH_MAX];
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n < 0) break;
    target[n] = '\0';
    if (target[0] == '/') {
      memcpy(path, target, (size_t)n + 1);
    } else {
      mp_parent_dir(path);
      size_t used = strlen(path);
      snprintf(path + used, PATH_MAX - used, "/%s", target);
      mp_normalize_path(path);
    }
  }
  mp_parent_dir(path);
  if (path[0] == '/' && strcmp(path, mp_execfolder) != 0) {
    memcpy(mp_execfolder_alias, path, PATH_MAX);
  }
}

static void mp_exec_init(void) {
#if defined(__APPLE__)
  char launched[PATH_MAX];
  uint32_t bufsize = PATH_MAX;
  if (_NSGetExecutablePath(launched, &bufsize) != 0) return;
  if (realpath(launched, mp_executable) == NULL) return;
#else
  ssize_t len = readlink("/proc/self/exe", mp_executable, sizeof(mp_executable) - 1);
  if (len == -1) return;
  mp_executable[len] = '\0';
  const char *launched = (const char *) getauxval(AT_EXECFN);
#endif
  memcpy(mp_execfolder, mp_executable, PATH_MAX);
  mp_parent_dir(mp_execfolder);
#if defined(__APPLE__)
  mp_set_execfolder_alias(launched);
#else
  if (launched != NULL) mp_set_execfolder_alias(launched);
#endif
  struct stat st;
  if (stat(mp_execfolder, &st) == 0) mp_exec_dev = st.st_dev;
  mp_exec_ok = true;
}
#endif

static bool mp_exec_paths(void) {
#ifdef _WIN32
  InitOnceExecuteOnce(&mp_exec_once, mp_exec_init, NULL, NULL);
#else
  pthread_once(&mp_exec_once, mp_exec_init);
#endif
  return mp_exec_ok;
}

#ifndef _WIN32
/* Length of `folder' if `path' lies inside it, else MP_NOT_IN_FOLDER. Matches
 * on a directory boundary only, so /tmp/x does not claim /tmp/xyz. A trailing
 * separator on `folder' (the root) is not counted, keeping it in the remainder. */
static size_t mp_folder_prefix_len(const char *path, const char *folder) {
  size_t n = strlen(folder);
  if (n == 0 || strncmp(path, folder, n) != 0) return MP_NOT_IN_FOLDER;
  if (folder[n - 1] == '/') return n - 1;
  if (path[n] == '\0' || path[n] == '/') return n;
  return MP_NOT_IN_FOLDER;
}

static void get_virtual_path(char *search_path, const char *absolute_path) {
  size_t prefix = mp_folder_prefix_len(absolute_path, mp_execfolder);
  if (prefix == MP_NOT_IN_FOLDER && mp_execfolder_alias[0] != '\0') {
    prefix = mp_folder_prefix_len(absolute_path, mp_execfolder_alias);
  }
  char *out = search_path;
  const char *in = absolute_path;
  if (prefix != MP_NOT_IN_FOLDER) {
    *out++ = '~';
    in += prefix;
  }
  const char *last = search_path + PATH_MAX - 1;
  for (; *in && out < last; in++) {
    *out++ = (*in == '\\') ? '/' : mp_ascii_lower(*in);
  }
  *out = '\0';
}

static bool mp_find_virtual(const char *path, EMAP **found_map) {
  if (path == NULL || !mp_exec_paths()) return false;
  char absolute_path[PATH_MAX] = {0};
  mp_get_absolute_path(path, absolute_path, PATH_MAX);
  char search_path[PATH_MAX];
  get_virtual_path(search_path, absolute_path);
  return find_embedded_file(search_path, found_map);
}
#else
/* Paths are compared case-insensitively, as the file system does. */
static size_t mp_win_folder_prefix_len(const wchar_t *path, const wchar_t *folder) {
  size_t n = wcslen(folder);
  if (n == 0 || wcslen(path) < n ||
      CompareStringOrdinal(path, (int)n, folder, (int)n, TRUE) != CSTR_EQUAL) {
    return MP_NOT_IN_FOLDER;
  }
  if (folder[n - 1] == L'\\') return n - 1;
  if (path[n] == L'\0' || path[n] == L'\\') return n;
  return MP_NOT_IN_FOLDER;
}

/* The search path for `path': "~/..." below the executable's folder, else
 * "/<drive>/...", as UTF-8 with ASCII lowercased and '/' separators. Built
 * from wide strings, so folders outside the ANSI code page match too. */
static bool mp_win_virtual_path(const wchar_t *path, char *search_path) {
  if (path == NULL || !mp_exec_paths() || wcslen(path) >= PATH_MAX) return false;
  wchar_t raw[PATH_MAX], full[PATH_MAX];
  wcscpy(raw, path);
  mp_win_strip_verbatim(raw);
  if (_wfullpath(full, raw, PATH_MAX) == NULL) return false;
  if (wcschr(full, L'~')) mp_win_long_path(full);
  size_t prefix = mp_win_folder_prefix_len(full, mp_execfolder_w);
  if (prefix == MP_NOT_IN_FOLDER && mp_execfolder_alias_w[0] != L'\0') {
    prefix = mp_win_folder_prefix_len(full, mp_execfolder_alias_w);
  }
  char *out = search_path;
  const wchar_t *rest = full;
  if (prefix != MP_NOT_IN_FOLDER) {
    *out++ = '~';
    rest += prefix;
  } else if (full[0] != L'\0' && full[1] == L':') {
    *out++ = '/';
    *out++ = mp_ascii_lower((char)full[0]);
    rest += 2;
  }
  int room = PATH_MAX - (int)(out - search_path);
  if (WideCharToMultiByte(CP_UTF8, 0, rest, -1, out, room, NULL, NULL) == 0) return false;
  for (char *p = out; *p; ++p) *p = (*p == '\\') ? '/' : mp_ascii_lower(*p);
  return true;
}

static bool mp_find_virtual_w(const wchar_t *path, EMAP **found_map) {
  char search_path[PATH_MAX];
  return mp_win_virtual_path(path, search_path) && find_embedded_file(search_path, found_map);
}

/* Narrow paths are in the ANSI code page, as for the CRT's narrow APIs. */
static bool mp_find_virtual(const char *path, EMAP **found_map) {
  wchar_t wide[PATH_MAX];
  if (path == NULL || MultiByteToWideChar(CP_ACP, 0, path, -1, wide, PATH_MAX) == 0) return false;
  return mp_find_virtual_w(wide, found_map);
}
#endif

/* Serves a virtual entry at the fd level. The fd is a real read-only fd on
 * the executable, so the number is unique and close() works on it, while
 * every read is answered from the EFILE registered for it. */
static int mp_open_virtual_fd(const EMAP *map, int flags) {
  if (flags & (O_WRONLY | O_RDWR)) {
    errno = EROFS;
    return -1;
  }
#ifdef O_DIRECTORY
  if ((flags & O_DIRECTORY) && map->type != ETYPE_DIRECTORY) {
    errno = ENOTDIR;
    return -1;
  }
#endif
  EFILE *e = (EFILE*)calloc(1, sizeof *e);
  if (e == NULL) {
    errno = ENOMEM;
    return -1;
  }
  if (!mp_efile_init_virtual(e, map)) {
    free(e);
    return -1;
  }
#ifdef _WIN32
  int fakefd = _wopen(mp_executable_w, _O_RDONLY | _O_BINARY | (flags & _O_NOINHERIT));
#else
  int fakefd = open(mp_executable, O_RDONLY | (flags & O_CLOEXEC));
#endif
  if (fakefd < 0) {
    mp_efile_destroy(e);
    return -1;
  }
  if (!mp_fd_register(fakefd, e)) {
    close(fakefd);
    mp_efile_destroy(e);
    errno = EMFILE;
    return -1;
  }
  return fakefd;
}

/* fopen() modes that would write: the embedded files are read-only. */
static bool mp_mode_writes(const char *mode) {
  return mode != NULL && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

/* lseek() on a virtual fd. Any position >= 0 is valid, past EOF included
 * (reads there return 0); a failed seek leaves the position unchanged. */
static int64_t mp_virtual_seek(EFILE *e, int64_t offset, int whence) {
  const char *base = e->end - e->size;
  int64_t target;
  switch (whence) {
    case SEEK_SET: target = offset; break;
    case SEEK_CUR: target = (int64_t)(e->pos - base) + offset; break;
    case SEEK_END: target = (int64_t)e->size + offset; break;
    default: errno = EINVAL; return -1;
  }
  if (target < 0) {
    errno = EINVAL;
    return -1;
  }
  mp_unget_restore(e);
  e->pos = base + target;
#ifdef _WIN32
  mp_sync_out(e);
#endif
  return target;
}

#ifndef _WIN32
/* Each entry gets its own inode number on the executable's device, so
 * samefile() and ismount() treat embedded files like files next to it. */
#define MP_VIRTUAL_INO_BASE ((ino_t)0x4d50000000000000ULL)
static void mp_fill_stat(const EMAP *map, struct stat *buf) {
  memset(buf, 0, sizeof *buf);
  buf->st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
  buf->st_mode |= (map->type == ETYPE_DIRECTORY) ? S_IFDIR : S_IFREG;
  buf->st_size = mp_uncompressed_size(map);
  buf->st_nlink = 1;
  buf->st_ino = MP_VIRTUAL_INO_BASE + (ino_t)(map - mp_entries);
  buf->st_dev = mp_exec_dev;
  int64_t ns = mp_mtime_ns(map);
  struct timespec t = { (time_t)(ns / 1000000000), (long)(ns % 1000000000) };
#ifdef __APPLE__
  buf->st_atimespec = buf->st_mtimespec = buf->st_ctimespec = buf->st_birthtimespec = t;
#else
  buf->st_atim = buf->st_mtim = buf->st_ctim = t;
#endif
}
#endif

/* Whether an entry of the directory on disk behind a virtual directory is
 * left out of its listing: the embedded entries win, and "." and ".." are
 * not listed. `name' is UTF-8. */
static bool mp_shadowed(const EMAP *dir_map, const char *name) {
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return true;
  char vpath[PATH_MAX];
  int n = snprintf(vpath, PATH_MAX, "%s/%s", mp_path_of(dir_map), name);
  EMAP *map;
  return n > 0 && n < PATH_MAX && find_embedded_file(vpath, &map);
}

#if defined(__linux__) || defined(__APPLE__)
/* Two link modes for POSIX:
 *
 *   MP_EMBED_USE_WRAP defined: build is linked with
 *     -Wl,--wrap=fopen,--wrap=fclose,--wrap=read,--wrap=lseek,
 *     --wrap=fstat,--wrap=close. The linker rewrites references to
 *     those names to __wrap_* (defined further down) and resolves
 *     __real_* to the original libc symbol. Full transparency for
 *     std::ifstream etc.
 *
 *   MP_EMBED_USE_WRAP NOT defined: only mp_embed.h's macro intercepts
 *     in user TUs are active; precompiled libstdc++.a/libc++.a calls
 *     bypass us. __real_* are macroed to plain libc names so mp_fopen
 *     and the wraps still build and work for direct user calls. */
#  ifdef MP_EMBED_USE_WRAP
extern FILE   *__real_fopen(const char *path, const char *mode);
extern int     __real_fclose(FILE *f);
extern ssize_t __real_read(int fd, void *buf, size_t n);
extern off_t   __real_lseek(int fd, off_t offset, int whence);
extern int     __real_fstat(int fd, struct stat *st);
extern int     __real_close(int fd);
#  else
#    define __real_fopen   fopen
#    define __real_fclose  fclose
#    define __real_read    read
#    define __real_lseek   lseek
#    define __real_fstat   fstat
#    define __real_close   close
#  endif
#endif

/* Test helper: run the mp_get_absolute_path -> get_virtual_path pipeline
 * and return the resulting search path. Useful for verifying path
 * translation without going through fopen. */
void mp_resolve_virtual_path_for_test(const char *input, char *out, size_t out_size) {
  char search_path[PATH_MAX] = {0};
#ifdef _WIN32
  wchar_t wide[PATH_MAX];
  if (MultiByteToWideChar(CP_ACP, 0, input, -1, wide, PATH_MAX) == 0 ||
      !mp_win_virtual_path(wide, search_path)) {
    search_path[0] = '\0';
  }
#else
  if (mp_exec_paths()) {
    char absolute_path[PATH_MAX] = {0};
    mp_get_absolute_path(input, absolute_path, PATH_MAX);
    get_virtual_path(search_path, absolute_path);
  }
#endif
  if (out_size == 0) return;
  strncpy(out, search_path, out_size - 1);
  out[out_size - 1] = '\0';
}

MP_DECL(EFILE*) mp_fopen(const char* file, const char* mode) {
  if (mp_debug()) fprintf(stderr, "[mp_embed] mp_fopen: '%s'\n", file ? file : "(null)");
  EMAP *map;
  if (mp_find_virtual(file, &map) && map->type == ETYPE_FILE) {
    if (mp_mode_writes(mode)) {
      errno = EROFS;
      return NULL;
    }
    /* User-facing virtual fopen returns an EFILE wrapper - mp_fread/
     * mp_fseek/etc check the EHANDLE_VIRTUAL magic and serve from
     * embed memory directly. libstdc++.a, which doesn't go through
     * our macros, takes the __wrap_fopen path further down instead. */
    EFILE *e = (EFILE *) calloc(1, sizeof *e);
    if (e == NULL) {
      errno = ENOMEM;
      return NULL;
    }
    if (!mp_efile_init_virtual(e, map)) {
      free(e);
      return NULL;
    }
    return e;
  }

#if defined(__linux__) || defined(__APPLE__)
  /* Native (non-virtual) files: return a plain FILE* with no EFILE
   * wrap. mp_fread etc detect the foreign pointer (no EFILE magic)
   * and pass it through to libc. Use __real_fopen so that
   * --wrap=fopen doesn't route us back into __wrap_fopen and recurse. */
  FILE *f = __real_fopen(file, mode);
  return (EFILE*)f;
#else
  FILE* f = fopen(file, mode);
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)calloc(1, sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
#endif
}

#if defined(__linux__) || defined(__APPLE__)
/* Linker hooks for the --wrap chain that makes std::ifstream see
 * virtual files transparently on POSIX.
 *
 * Build with `-Wl,--wrap=fopen --wrap=read --wrap=lseek --wrap=fstat
 * --wrap=close`. The linker rewrites every call to those names (in
 * every object file, including the precompiled libstdc++.a /
 * libc++.a) into __wrap_NAME, and exposes the original libc symbol
 * as __real_NAME.
 *
 * Each wrap below looks the fd / path up: if it belongs to a virtual
 * file, we serve from embed memory zero-copy; otherwise we delegate
 * to __real_NAME so real files still work.
 *
 * The weak __real_NAME stubs below provide a fallback when --wrap
 * isn't passed (e.g. an older link command): they just call the libc
 * symbol directly. With --wrap, the linker provides a strong
 * __real_NAME that wins and routes to the un-wrapped libc symbol -
 * no recursion.
 */
/* __real_* are declared further up (see comment block at the top of
 * the POSIX section), guarded by MP_EMBED_USE_WRAP. */

/* Lookup helper. Returns NULL if fd isn't one of ours, else the
 * EFILE bound to it. mp_fd2file is also used by the existing mp_open
 * fakefd registry so this is shared state. */
static EFILE *mp_lookup_virtual_fd(int fd) {
    EFILE *e = mp_fd_lookup(fd);
    return (e && e->handle_type == EHANDLE_VIRTUAL) ? e : NULL;
}

/* __wrap_fopen serves precompiled library callers (libstdc++.a's
 * basic_file::sys_open, third-party static libs, etc.) that don't go
 * through our header macros. They expect a real libc FILE* and will
 * call fileno() on it to get an fd, then read()/lseek()/fstat() at
 * the fd layer. So for virtual paths we create a real FILE* over a
 * fakefd, registered in mp_fd2file - the __wrap_read/lseek/fstat
 * functions look the fakefd up and serve from embed memory. */
FILE *__wrap_fopen(const char *path, const char *mode) {
    EMAP *map = NULL;
    if (!mp_find_virtual(path, &map) || map->type != ETYPE_FILE)
        return __real_fopen(path, mode);
    if (mp_mode_writes(mode)) { errno = EROFS; return NULL; }

    EFILE *e = (EFILE*)calloc(1, sizeof *e);
    if (!e) return NULL;
    if (!mp_efile_init_virtual(e, map)) { free(e); return NULL; }
    int fakefd = open("/dev/null", O_RDONLY);
    if (fakefd < 0) { mp_efile_destroy(e); return NULL; }
    if (!mp_fd_register(fakefd, e)) { __real_close(fakefd); mp_efile_destroy(e); return NULL; }
    FILE *f = fdopen(fakefd, "rb");
    if (!f) {
        mp_fd_unregister(fakefd);
        __real_close(fakefd); mp_efile_destroy(e); return NULL;
    }
    return f;
}

ssize_t __wrap_read(int fd, void *buf, size_t n) {
    EFILE *e = mp_lookup_virtual_fd(fd);
    if (!e) return __real_read(fd, buf, n);
    size_t avail = mp_avail(e);
    size_t to_read = (n < avail) ? n : avail;
    if (to_read == 0) return 0;
    memcpy(buf, e->pos, to_read);
    e->pos = e->pos + to_read;
    return (ssize_t)to_read;
}

off_t __wrap_lseek(int fd, off_t offset, int whence) {
    EFILE *e = mp_lookup_virtual_fd(fd);
    if (!e) return __real_lseek(fd, offset, whence);
    return (off_t)mp_virtual_seek(e, offset, whence);
}

int __wrap_fstat(int fd, struct stat *st) {
    EFILE *e = mp_lookup_virtual_fd(fd);
    if (!e) return __real_fstat(fd, st);
    mp_fill_stat(e->map, st);
    return 0;
}

int __wrap_close(int fd) {
    /* Drop our registry entry BEFORE closing the kernel fd. Otherwise
     * the kernel can recycle the fd into another thread's open() while
     * our slot still points at the (now-freed) EFILE. */
    EFILE *to_free = mp_fd_unregister(fd);
    int r = __real_close(fd);
    if (to_free) mp_efile_destroy(to_free);
    return r;
}

/* basic_file::close calls fclose, which internally uses a libc-private
 * __close that bypasses our --wrap=close. So we wrap fclose itself:
 * snapshot the fd, drop our registry slot, then let real fclose close
 * the underlying fd via libc internals. */
int __wrap_fclose(FILE *f) {
    int fd = -1;
    if (f) fd = fileno(f);
    EFILE *to_free = mp_fd_unregister(fd);
    int r = __real_fclose(f);
    if (to_free) mp_efile_destroy(to_free);
    return r;
}

#if defined(__linux__)
/* gcc 13's libstdc++.a basic_file.o calls fopen64 / lseek64 / fstat64
 * (the LFS variants) directly. On glibc x86-64 these are functionally
 * identical to the unsuffixed versions - off_t is already 64-bit and
 * struct stat == struct stat64 - so aliasing the symbols at link time
 * routes the calls through our wrappers without any data conversion.
 * gcc 15+ switched to plain fopen/lseek/fstat but ubuntu-24.04 runners
 * still ship gcc 13.3, so both code paths must work. The corresponding
 * --wrap=fopen64 / --wrap=lseek64 / --wrap=fstat64 flags are passed
 * via build.sh's LDFLAGS and the test step's WRAPS variable. */
FILE *__wrap_fopen64(const char *path, const char *mode)
    __attribute__((alias("__wrap_fopen")));
off_t __wrap_lseek64(int fd, off_t offset, int whence)
    __attribute__((alias("__wrap_lseek")));
int __wrap_fstat64(int fd, struct stat *st)
    __attribute__((alias("__wrap_fstat")));
#endif

#endif

#ifdef _WIN32
MP_DECL(int) mp_open(const char *file, int flags, int mode) {
#else
MP_DECL(int) mp_open(const char *file, int flags, mode_t mode) {
#endif
  EMAP *map;
  if (mp_find_virtual(file, &map)) return mp_open_virtual_fd(map, flags);
  return open(file, flags, mode);
}

#ifdef _WIN32
MP_DECL(EFILE*) mp_wfopen(const wchar_t *wfile, const wchar_t *mode) {
  EMAP *map;
  if (mp_find_virtual_w(wfile, &map) && map->type == ETYPE_FILE) {
    if (mode != NULL && wcspbrk(mode, L"wa+") != NULL) {
      errno = EROFS;
      return NULL;
    }
    EFILE *e = (EFILE *) calloc(1, sizeof *e);
    if (e == NULL) {
      errno = ENOMEM;
      return NULL;
    }
    if (!mp_efile_init_virtual(e, map)) {
      free(e);
      return NULL;
    }
    return e;
  }

  FILE* f = _wfopen(wfile, mode);
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)calloc(1, sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
}

/* MSVC safer-CRT stdio: errno_t fopen_s(FILE**, ...) */
MP_DECL(int) mp_fopen_s(EFILE **pf, const char *filename, const char *mode) {
  if (pf == NULL || filename == NULL || mode == NULL) return EINVAL;
  *pf = NULL;
  EFILE *e = mp_fopen(filename, mode);
  if (e == NULL) return errno ? errno : ENOENT;
  *pf = e;
  return 0;
}

MP_DECL(int) mp__wfopen_s(EFILE **pf, const wchar_t *filename, const wchar_t *mode) {
  if (pf == NULL || filename == NULL || mode == NULL) return EINVAL;
  *pf = NULL;
  EFILE *e = mp_wfopen(filename, mode);
  if (e == NULL) return errno ? errno : ENOENT;
  *pf = e;
  return 0;
}

/* Safer-CRT _sopen_s / _wsopen_s. We delegate to mp_open / mp_wopen which
 * already does the embed lookup for the file descriptor case. shflag
 * (sharing flag) is only meaningful for real OS files; virtual files
 * are inherently shareable read-only. */
MP_DECL(int) mp__sopen_s(int *pfh, const char *filename, int oflag, int shflag, int pmode) {
  (void)shflag;
  if (pfh == NULL || filename == NULL) return EINVAL;
  int fd = mp_open(filename, oflag, pmode);
  if (fd < 0) {
    *pfh = -1;
    return errno ? errno : ENOENT;
  }
  *pfh = fd;
  return 0;
}

MP_DECL(int) mp__wsopen_s(int *pfh, const wchar_t *filename, int oflag, int shflag, int pmode) {
  (void)shflag;
  if (pfh == NULL || filename == NULL) return EINVAL;
  int fd = mp_wopen(filename, oflag, pmode);
  if (fd < 0) {
    *pfh = -1;
    return errno ? errno : ENOENT;
  }
  *pfh = fd;
  return 0;
}

MP_DECL(int) mp_wopen(const wchar_t *wfile, int flags, int mode) {
  EMAP *map;
  if (mp_find_virtual_w(wfile, &map)) return mp_open_virtual_fd(map, flags);
  return _wopen(wfile, flags, mode);
}
#else
/* `rel' resolved against the virtual directory `dir' (an entry's "~/..." or
 * "/..." path). ".." stops at the root of the embedded tree. */
static void mp_virtual_join(char *out, const char *dir, const char *rel) {
  bool relative_root = dir[0] == '~';
  snprintf(out, PATH_MAX - 1, "%s/%s", relative_root ? dir + 1 : dir, rel);
  mp_normalize_path(out);
  if (relative_root) {
    memmove(out + 1, out, strlen(out) + 1);
    out[0] = '~';
  }
}

/* Lookup for the *at() calls: 1 if `*map' is the entry, 0 if the path is not
 * virtual, -1 (errno set) if it names nothing inside a virtual directory.
 * Relative paths against a real dirfd stay with the OS, which alone knows
 * where that directory is. */
static int mp_find_virtual_at(int dirfd, const char *pathname, EMAP **map) {
  EFILE *dir = pathname[0] == '/' ? NULL : mp_fd_lookup(dirfd);
  if (dir != NULL) {
    if (dir->map->type != ETYPE_DIRECTORY) {
      errno = ENOTDIR;
      return -1;
    }
    char vpath[PATH_MAX];
    mp_virtual_join(vpath, dir->name, pathname);
    if (find_embedded_file(vpath, map)) return 1;
    errno = ENOENT;
    return -1;
  }
  if (pathname[0] != '/' && dirfd != AT_FDCWD) return 0;
  return mp_find_virtual(pathname, map) ? 1 : 0;
}

MP_DECL(int) mp_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
  EMAP *map;
  int found = mp_find_virtual_at(dirfd, pathname, &map);
  if (found > 0) return mp_open_virtual_fd(map, flags);
  if (found < 0) {
    if (errno == ENOENT && (flags & O_CREAT)) errno = EROFS;
    return -1;
  }
  return openat(dirfd, pathname, flags, mode);
}
#endif

MP_DECL(int) mp_fclose(void* e) {
  init_fd2file();
  if (MP_FOREIGN_PTR) {
    // This is not a stream managed by our system, pass to the real fclose.
    return fclose((FILE*)e);
  }

  EFILE* efile = (EFILE*)e;

  // Handle native file streams wrapped in our EFILE struct.
  if (efile->handle_type == EHANDLE_NATIVE) {
    // Call the real fclose on the underlying FILE stream.
    int result = fclose(efile->f);
    mp_efile_destroy(efile);
    return result;
  }

  // Handle virtual file streams.
  if (efile->handle_type == EHANDLE_VIRTUAL) {
    // A virtual stream only owns an fd if mp_fileno() handed one out.
    int fd_to_close = mp_fd_of(efile);
    if (fd_to_close != -1) {
      mp_fd_unregister(fd_to_close);
      mp_efile_destroy(efile);
      return close(fd_to_close);
    }
  }

  mp_efile_destroy(efile);
  return 0;
}

MP_DECL(int) mp_close(int fd) {
  EFILE *e = mp_fd_unregister(fd);
  if (e == NULL) {
    return close(fd);
  }
  if (e->handle_type != EHANDLE_VIRTUAL) {
    fclose(e->f);
    mp_efile_destroy(e);
    return 0;
  }
  mp_efile_destroy(e);

  // Close the underlying file descriptor that was opened on the executable
  return close(fd);
}

MP_DECL(EFILE*) mp_freopen(const char *filename, const char *mode, void *e) {
  if (MP_FOREIGN_PTR) {
    return (EFILE*)freopen(filename, mode, (FILE*)e);
  }
  EFILE *stream = (EFILE*)e;
  if (stream->handle_type == EHANDLE_NATIVE) {
    FILE *f = freopen(filename, mode, stream->f);
    if (f == NULL) {
      free(stream);  /* freopen() closes the stream even when it fails */
      return NULL;
    }
    stream->f = f;
    return stream;
  }

  /* A virtual stream is reopened in place, so callers that keep using the
   * pointer they passed in (the usual idiom) see the new file. */
  if (filename == NULL) {
    if (!mp_mode_writes(mode)) return stream;
    mp_fclose(stream);
    errno = EROFS;
    return NULL;
  }
  int fd = mp_fd_of(stream);
  if (fd != -1) {
    mp_fd_unregister(fd);
    close(fd);
  }
  free(stream->owned_buf);
  memset(stream, 0, sizeof *stream);
  EMAP *map;
  if (mp_find_virtual(filename, &map) && map->type == ETYPE_FILE) {
    if (!mp_mode_writes(mode) && mp_efile_init_virtual(stream, map)) return stream;
    if (mp_mode_writes(mode)) errno = EROFS;
    free(stream);
    return NULL;
  }
#if defined(__linux__) || defined(__APPLE__)
  FILE *f = __real_fopen(filename, mode);
#else
  FILE *f = fopen(filename, mode);
#endif
  if (f == NULL) {
    free(stream);
    return NULL;
  }
  stream->handle_type = EHANDLE_NATIVE;
  stream->f = f;
  return stream;
}

MP_DECL(EFILE*) mp_tmpfile() {
  FILE* f = tmpfile();
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)calloc(1, sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
}

MP_DECL(bool) mp_feof(void* e) {
  if (MP_FOREIGN_PTR) {
    return feof((FILE*)e);
  }
  if(e == NULL){
    errno = EINVAL;
    return true;
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return feof(((EFILE*)e)->f);
  }

  return (((EFILE*)e)->err & MP_EFILE_EOF) != 0;
}

MP_DECL(size_t) mp_fread(void* ptr, size_t size, size_t count, void* e) {
  if (MP_FOREIGN_PTR) {
    return fread(ptr, size, count, (FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fread(ptr, size, count, ((EFILE*)e)->f);
  }

  /* C standard: if size==0 or count==0, fread returns 0 and the stream
   * is left unchanged. */
  if (size == 0 || count == 0) return 0;

  if (mp_debug()) {
    EFILE *ef = (EFILE*)e;
#ifdef _WIN32
    fprintf(stderr, "[mp_embed] mp_fread size=%zu count=%zu pos=%p crt_ptr=%p crt_cnt=%d\n",
            size, count, (void*)ef->pos, (void*)ef->crt_ptr, ef->crt_cnt);
#else
    fprintf(stderr, "[mp_embed] mp_fread size=%zu count=%zu pos=%p\n",
            size, count, (void*)ef->pos);
#endif
  }

#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif

  size_t avail = mp_avail((EFILE*)e);
  size_t want  = (count > (size_t)-1 / size) ? (size_t)-1 : size * count;
  size_t result;
  if (avail < want) {
    size_t scount = avail;
    memcpy(ptr, (void*)((EFILE*)e)->pos, scount);
    ((EFILE*)e)->pos += scount;
    ((EFILE*)e)->err |= MP_EFILE_EOF;
    result = scount / size;
  } else {
    memcpy(ptr, (void*)((EFILE*)e)->pos, want);
    ((EFILE*)e)->pos = (char*)((EFILE*)e)->pos + want;
    result = count;
  }

#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
  return result;
}

#ifdef _WIN32
MP_DECL(int) mp_read(int fd, void *buf, unsigned int count) {
#else
MP_DECL(ssize_t) mp_read(int fd, void *buf, size_t count) {
#endif
  EFILE *e = mp_fd_lookup(fd);
  if (e == NULL) {
    return read(fd, buf, count);
  }
  if (e->map != NULL && e->map->type == ETYPE_DIRECTORY) {
    errno = EISDIR;
    return -1;
  }

#ifdef _WIN32
  mp_sync_in(e);
#endif
  size_t n = mp_avail(e);
  if (count < n) n = count;
  if (n != 0) memcpy(buf, e->pos, n);
  e->pos += n;
#ifdef _WIN32
  mp_sync_out(e);
#endif
  return n;
}

/* Reads at an absolute offset without moving the file position. */
MP_DECL(ssize_t) mp_pread(int fd, void *buf, size_t count, off_t offset) {
  EFILE *e = mp_fd_lookup(fd);
  if (e == NULL) {
#ifdef _WIN32
    __int64 saved = _lseeki64(fd, 0, SEEK_CUR);
    if (saved < 0 || _lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    int n = _read(fd, buf, (unsigned int)count);
    _lseeki64(fd, saved, SEEK_SET);
    return n;
#else
    return pread(fd, buf, count, offset);
#endif
  }
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  if ((uint64_t)offset >= e->size) {
    return 0; // Offset is beyond the end of the file
  }

  size_t n = e->size - (size_t)offset;
  if (count < n) n = count;
  memcpy(buf, (e->end - e->size) + offset, n);
  return (ssize_t)n;
}

MP_DECL(int) mp_fgetpos(void* e, fpos_t* pos) {
  if (MP_FOREIGN_PTR) {
    return fgetpos((FILE*)e, pos);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgetpos(((EFILE*)e)->f, pos);
  }

#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  // fgetpos should store the absolute offset from the start of the file.
  // We use the same logic as mp_ftell_priv to get this value.
  int64_t offset = ((EFILE*)e)->pos - (((EFILE*)e)->end - ((EFILE*)e)->size);
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }

#ifdef __linux__
  // On Linux, fpos_t is a struct.
  fpos_t temp = { 0 };
  temp.__pos = offset;
  memcpy(pos, &temp, sizeof(fpos_t));
#else
  // On other systems, it's typically a numeric type.
  *pos = (fpos_t)offset;
#endif

  return 0;
}

MP_DECL(char*) mp_fgets(char* str, int num, void* e ) {
  if (MP_FOREIGN_PTR) {
    return fgets(str, num, (FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgets(str, num, ((EFILE*)e)->f);
  }

#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  //if num 0 or 1 e->pos will not advance
  if (num <= 1) return NULL;
  if (mp_avail((EFILE*)e) == 0) {
    ((EFILE*)e)->err |= MP_EFILE_EOF;
    return NULL;
  }

  int i = 0;

  while (1)
  {
    //i < (num - 1) so still room for two characters: \n and \0
    if ((str[i++] = *(((EFILE*)e)->pos++)) == '\n') break;
    if (i == (num - 1)) break;
    if (mp_avail((EFILE*)e) == 0) {
      ((EFILE*)e)->err |= MP_EFILE_EOF;
      break;
    }
  }
  str[i] = '\0';
#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
  return str;
}

#ifndef _WIN32
MP_DECL(ssize_t) mp_getline(char **lineptr, size_t *n, void* e) {
  if (MP_FOREIGN_PTR) {
    return getline(lineptr, n, (FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return getline(lineptr, n, ((EFILE*)e)->f);
  }

  if (lineptr == NULL || n == NULL) {
    errno = EINVAL;
    return -1;
  }

  // Initialize buffer if needed
  if (*lineptr == NULL || *n == 0) {
    char *new_ptr = (char*)realloc(*lineptr, 128);
    if (new_ptr == NULL) {
      return -1;
    }
    *lineptr = new_ptr;
    *n = 128; // Initial buffer size
  }

  size_t pos = 0;
  int c;

  while ((c = mp_fgetc(e)) != -1) {
    // Ensure buffer has space for character and null terminator
    if (pos + 1 >= *n) {
      size_t new_size = *n * 2;
      char *new_ptr = (char*)realloc(*lineptr, new_size);
      if (new_ptr == NULL) {
        return -1;
      }
      *lineptr = new_ptr;
      *n = new_size;
    }

    (*lineptr)[pos++] = (char)c;

    // Stop at newline
    if (c == '\n') {
      break;
    }
  }

  // If we read nothing and hit EOF, return -1
  if (pos == 0 && c == -1) {
    return -1;
  }

  // Null-terminate the string
  (*lineptr)[pos] = '\0';

  return (ssize_t)pos;
}
#endif

MP_DECL(int) mp_fgetc(void* e) {
  if (MP_FOREIGN_PTR) {
    return fgetc((FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgetc(((EFILE*)e)->f);
  }

#ifdef _WIN32
  /* MSVC's basic_filebuf<char>::underflow calls fgetc(); the streambuf
   * may have advanced crt_ptr past pos via direct cursor reads, so pull
   * that view back into pos before reading. */
  mp_sync_in((EFILE*)e);
#endif

  if (mp_avail((EFILE*)e) == 0) {
    ((EFILE*)e)->err |= MP_EFILE_EOF;
    return -1;
  }
  int ch = (int)(unsigned char)(*(((EFILE*)e)->pos++));
#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
  return ch;
}

#ifndef _WIN32
MP_DECL(int) mp_getc_unlocked(void* e) {
  if (MP_FOREIGN_PTR) {
    return getc_unlocked((FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return getc_unlocked(((EFILE*)e)->f);
  }

  return mp_fgetc(e);
}
#endif


int64_t mp_ftell_priv(void* e) {
  if (MP_FOREIGN_PTR) {
#ifdef _WIN32
    return _ftelli64((FILE*)e);
#else
    return ftello((FILE*)e);
#endif
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
#ifdef _WIN32
    return _ftelli64(((EFILE*)e)->f);
#else
    return ftello(((EFILE*)e)->f);
#endif
  }

#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  return ((EFILE*)e)->pos - (((EFILE*)e)->end - ((EFILE*)e)->size);
}

int mp_fseek_priv(void* e, int64_t offset, int origin) {
  if (MP_FOREIGN_PTR) {
#ifdef _WIN32
    return _fseeki64((FILE*)e, offset, origin);
#else
    return fseeko((FILE*)e, offset, origin);
#endif
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
#ifdef _WIN32
    return _fseeki64(((EFILE*)e)->f, offset, origin);
#else
    return fseeko(((EFILE*)e)->f, offset, origin);
#endif
  }

#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  if (mp_virtual_seek((EFILE*)e, offset, origin) < 0) return -1;
  ((EFILE*)e)->err &= ~MP_EFILE_EOF;
  return 0;
}

MP_DECL(long int) mp_ftell(void* e) {
  return mp_ftell_priv(e);
}

MP_DECL(int) mp_fseek(void* e, long int offset, int origin) {
  return mp_fseek_priv(e, offset, origin);
}

MP_DECL(int64_t) mp_ftello64(void *e) {
  return mp_ftell_priv(e);
}

MP_DECL(int) mp_fseeko64(void *e, int64_t offset, int origin) {
  return mp_fseek_priv(e, offset, origin);
}

MP_DECL(int) mp_fscanf(void *e, const char *format, ...) {
  if (MP_FOREIGN_PTR) {
    va_list args;
    va_start(args, format);
    int result = vfscanf(((FILE*)e), format, args);
    va_end(args);
    return result;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    va_list args;
    va_start(args, format);
    int result = vfscanf(((EFILE*)e)->f, format, args);
    va_end(args);
    return result;
  }

  /* Scan a private stream over the unread bytes, then advance by what the
   * scan consumed. Windows has no fmemopen(), so it goes through tmpfile(). */
  EFILE *ef = (EFILE*)e;
#ifdef _WIN32
  mp_sync_in(ef);
#endif
  size_t avail = mp_avail(ef);
  if (avail == 0) {
    ef->err |= MP_EFILE_EOF;
    return EOF;
  }
#ifdef _WIN32
  FILE *mem = tmpfile();
  if (mem == NULL) return EOF;
  if (fwrite(ef->pos, 1, avail, mem) != avail) {
    fclose(mem);
    return EOF;
  }
  rewind(mem);
#else
  FILE *mem = fmemopen((void*)ef->pos, avail, "r");
  if (mem == NULL) return EOF;
#endif
  va_list args;
  va_start(args, format);
  int result = vfscanf(mem, format, args);
  va_end(args);
  long used = ftell(mem);
  if (feof(mem)) ef->err |= MP_EFILE_EOF;
  fclose(mem);
  if (used > 0) ef->pos += used;
#ifdef _WIN32
  mp_sync_out(ef);
#endif
  return result;
}

MP_DECL(int) mp_fputc(int character, void *e) {
  if (MP_FOREIGN_PTR) {
    return fputc(character, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputc(character, ((EFILE*)e)->f);
  }
  ((EFILE*)e)->err |= MP_EFILE_ERR;
  errno = EBADF;
  return -1;
}

MP_DECL(int) mp_fputs(const char *str, void *e) {
  if (MP_FOREIGN_PTR) {
    return fputs(str, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputs(str, ((EFILE*)e)->f);
  }
  ((EFILE*)e)->err |= MP_EFILE_ERR;
  errno = EBADF;
  return -1;
}

MP_DECL(int) mp_fprintf(void *e, const char *format, ...) {
  if (MP_FOREIGN_PTR) {
    va_list args;
    va_start(args, format);
    int result = vfprintf((FILE*)e, format, args);
    va_end(args);
    return result;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    va_list args;
    va_start(args, format);
    int result = vfprintf(((EFILE*)e)->f, format, args);
    va_end(args);
    return result;
  }
  ((EFILE*)e)->err |= MP_EFILE_ERR;
  errno = EBADF;
  return -1;
}
MP_DECL(int) mp_vfprintf(void *e, const char *format, va_list args) {
  if (MP_FOREIGN_PTR) {
    return vfprintf((FILE*)e, format, args);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return vfprintf(((EFILE*)e)->f, format, args);
  }
  ((EFILE*)e)->err |= MP_EFILE_ERR;
  errno = EBADF;
  return -1;
}

MP_DECL(size_t) mp_fwrite(const void *ptr, size_t size, size_t count, void *e) {
  if (MP_FOREIGN_PTR) {
    return fwrite(ptr, size, count, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fwrite(ptr, size, count, ((EFILE*)e)->f);
  }
  ((EFILE*)e)->err |= MP_EFILE_ERR;
  errno = EBADF;
  return 0;
}

MP_DECL(void) mp_setbuf(void *e, char *buffer) {
  if (MP_FOREIGN_PTR) {
    setbuf((FILE*)e, buffer);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    setbuf(((EFILE*)e)->f, buffer);
  }
}

MP_DECL(int) mp_setvbuf(void *e, char *buffer, int mode, size_t size) {
  if (MP_FOREIGN_PTR) {
    return setvbuf((FILE*)e, buffer, mode, size);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return setvbuf(((EFILE*)e)->f, buffer, mode, size);
  }
  return 0;
}

MP_DECL(void) mp_rewind(void *e) {
  if (MP_FOREIGN_PTR) {
    rewind((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    rewind(((EFILE*)e)->f);
    return;
  }
  EFILE *ef = (EFILE*)e;
  mp_unget_restore(ef);
  ef->pos = ef->end - ef->size;
  ef->err = 0;
#ifdef _WIN32
  mp_sync_out(ef);
#endif
}

MP_DECL(int) mp_fsetpos(void *e, const fpos_t *pos) {
  if (MP_FOREIGN_PTR) {
    return fsetpos((FILE*)e, pos);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fsetpos(((EFILE*)e)->f, pos);
  }

  // The 'pos' object now contains the absolute offset from the beginning.
  // We can use our fseek implementation with SEEK_SET to position the stream.
#ifdef __linux__
  // On Linux, fpos_t is a struct, extract the position.
  return mp_fseek_priv(e, pos->__pos, SEEK_SET);
#else
  // On other systems, it's typically a numeric type.
  return mp_fseek_priv(e, *pos, SEEK_SET);
#endif
}

MP_DECL(void) mp_clearerr(void *e) {
  if (MP_FOREIGN_PTR) {
    clearerr((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    clearerr(((EFILE*)e)->f);
    return;
  }
  ((EFILE*)e)->err = 0;
}

MP_DECL(int) mp_ferror(void *e) {
  if (MP_FOREIGN_PTR) {
    return ferror((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ferror(((EFILE*)e)->f);
  }
  return (((EFILE*)e)->err & MP_EFILE_ERR) != 0;
}

MP_DECL(int) mp_fileno(void *e) {
  init_fd2file();
  if (MP_FOREIGN_PTR) {
    return fileno((FILE*)e);
  }
  EFILE* efile = (EFILE*)e;

  if (efile->handle_type == EHANDLE_NATIVE) {
    // For a native file stream, call the real fileno on the underlying FILE*.
    return fileno(efile->f);
  }

  // For a virtual file, first attempt to find the existing file descriptor.
  int fd = mp_fd_of(efile);
  if (fd != -1) {
    return fd;
  }

  // The stream has no fd yet: give it one on the executable, registered to
  // this EFILE, which fclose() closes again.
  if (!mp_exec_paths()) {
    errno = EBADF;
    return -1;
  }
#ifdef _WIN32
  int new_fd = _wopen(mp_executable_w, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
  int new_fd = open(mp_executable, O_RDONLY | O_CLOEXEC);
#endif
  if (new_fd == -1) {
    return -1;
  }
  if (!mp_fd_register(new_fd, efile)) {
    close(new_fd);
    return -1;
  }
  return new_fd;
}

MP_DECL(int) mp_fflush(void *e) {
  if (MP_FOREIGN_PTR) {
    return fflush((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fflush(((EFILE*)e)->f);
  }
  return 0;
}

#ifndef _WIN32
MP_DECL(void) mp_flockfile(void *e) {
  if (MP_FOREIGN_PTR) {
    flockfile((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    flockfile(((EFILE*)e)->f);
  }
}

MP_DECL(void) mp_funlockfile(void *e) {
  if (MP_FOREIGN_PTR) {
    funlockfile((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    funlockfile(((EFILE*)e)->f);
  }
}

MP_DECL(int) mp_ftrylockfile(void *e) {
  if (MP_FOREIGN_PTR) {
    return ftrylockfile((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ftrylockfile(((EFILE*)e)->f);
  }
  return 0;
}
#endif

MP_DECL(int) mp_ungetc(int character, void *e) {
  if (MP_FOREIGN_PTR) {
    return ungetc(character, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ungetc(character, ((EFILE*)e)->f);
  }
  if (character == EOF) return EOF;
#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  if (!mp_unget((EFILE*)e, (unsigned char)character)) return EOF;
#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
  return (unsigned char)character;
}

/* Wide-char IO: needed because MSVC's STL filebuf calls fgetwc/fputwc/
 * ungetwc on the FILE* it got back from fopen/_wfopen_s. Without these
 * intercepts the type gets passed through but the underlying call goes
 * to the real CRT which segfaults on our EFILE*. */
#include <wchar.h>

MP_DECL(wint_t) mp_fgetwc(void *e) {
  if (MP_FOREIGN_PTR) return fgetwc((FILE*)e);
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) return fgetwc(((EFILE*)e)->f);
#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  if (mp_avail((EFILE*)e) == 0) {
    ((EFILE*)e)->err |= MP_EFILE_EOF;
    return WEOF;
  }
  /* Virtual files are binary blobs. Returning one byte at a time gives
   * MSVC's filebuf code conversion enough to do its work for ASCII /
   * single-byte UTF-8 input. Multibyte conversion is left to upper
   * layers. */
  wint_t c = (wint_t)(unsigned char)(*(((EFILE*)e)->pos++));
#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
  return c;
}

MP_DECL(wint_t) mp_fputwc(wchar_t c, void *e) {
  if (MP_FOREIGN_PTR) return fputwc(c, (FILE*)e);
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) return fputwc(c, ((EFILE*)e)->f);
  ((EFILE*)e)->err |= MP_EFILE_ERR;
  errno = EBADF;
  return WEOF;
}

MP_DECL(wint_t) mp_ungetwc(wint_t c, void *e) {
  if (MP_FOREIGN_PTR) return ungetwc(c, (FILE*)e);
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) return ungetwc(c, ((EFILE*)e)->f);
  /* Virtual streams read wide characters byte by byte (mp_fgetwc). */
  if (c == WEOF || c > 0xff) return WEOF;
#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  if (!mp_unget((EFILE*)e, (unsigned char)c)) return WEOF;
#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
  return c;
}

#ifdef _WIN32
/* MSVC's STL filebuf does _lock_file/_unlock_file around stream ops. */
MP_DECL(void) mp__lock_file(void *e) {
  if (MP_FOREIGN_PTR) { _lock_file((FILE*)e); return; }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) { _lock_file(((EFILE*)e)->f); return; }
  /* Virtual files don't need locking - they're read-only memory. */
}

MP_DECL(void) mp__unlock_file(void *e) {
  if (MP_FOREIGN_PTR) { _unlock_file((FILE*)e); return; }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) { _unlock_file(((EFILE*)e)->f); return; }
}
#endif

MP_DECL(EFILE*) mp_fdopen(int fd, const char *mode) {
  EFILE *virtual_file = mp_fd_lookup(fd);
  if (virtual_file != NULL) {
    return virtual_file;
  }

  // If this is not a virtual fd, fallback.
  FILE *f = fdopen(fd, mode);
  if (f == NULL) {
    return NULL;
  }
  EFILE* e = (EFILE*)calloc(1, sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;
  return e;
}

/* A copy of a virtual fd's EFILE, at the same position, for a duplicate of
 * the fd or of its handle. Unlike OS duplicates, the two do not share the
 * position afterwards. */
static EFILE *mp_efile_dup(const EFILE *src) {
  EFILE *e = (EFILE*)calloc(1, sizeof *e);
  if (e == NULL) {
    errno = ENOMEM;
    return NULL;
  }
  /* A directory, or a handle that has not read anything yet. */
  if (src->owned_buf == NULL) {
    *e = *src;
    return e;
  }
  if (!mp_efile_init_virtual(e, src->map)) {
    free(e);
    return NULL;
  }
  int64_t at = src->pos - (src->end - src->size);
  if (at > 0) mp_virtual_seek(e, at, SEEK_SET);
  e->err = src->err;
  return e;
}

/* Registers `fd2', a new duplicate of a virtual fd, with a copy of its
 * EFILE. Closes it again on failure. */
static int mp_fd_adopt(int fd2, const EFILE *src) {
  EFILE *e = mp_efile_dup(src);
  if (e != NULL && mp_fd_register(fd2, e)) return fd2;
  int err = errno;
  mp_efile_destroy(e);
  close(fd2);
  errno = err;
  return -1;
}

MP_DECL(int) mp_dup(int fd) {
  EFILE *e = mp_fd_lookup(fd);
#ifdef _WIN32
  int fd2 = _dup(fd);
#else
  int fd2 = dup(fd);
#endif
  return (e != NULL && fd2 >= 0) ? mp_fd_adopt(fd2, e) : fd2;
}

/* dup2() and dup3(); flags < 0 picks dup2(). The OS closes fd2 first, so a
 * virtual fd2 is unregistered beforehand, and registered again if the call
 * fails. */
static int mp_dup_onto(int fd, int fd2, int flags) {
  EFILE *e = mp_fd_lookup(fd);
  EFILE *old = fd == fd2 ? NULL : mp_fd_unregister(fd2);
#if defined(_WIN32)
  (void)flags;
  int r = _dup2(fd, fd2);
#elif defined(__linux__)
  int r = flags < 0 ? dup2(fd, fd2) : dup3(fd, fd2, flags);
#else
  (void)flags;
  int r = dup2(fd, fd2);
#endif
  if (r < 0) {
    int err = errno;
    if (old != NULL) mp_fd_register(fd2, old);
    errno = err;
    return r;
  }
  mp_efile_destroy(old);
  if (e == NULL || fd == fd2) return r;
  /* _dup2() returns 0, dup2() fd2. */
  return mp_fd_adopt(fd2, e) < 0 ? -1 : r;
}

MP_DECL(int) mp_dup2(int fd, int fd2) {
  return mp_dup_onto(fd, fd2, -1);
}

#ifdef __linux__
MP_DECL(int) mp_dup3(int fd, int fd2, int flags) {
  return mp_dup_onto(fd, fd2, flags);
}
#endif

#ifndef _WIN32
/* The argument goes through as the pointer-sized value libc reads it as.
 * F_DUPFD duplicates are registered like dup()'s. */
MP_DECL(int) mp_fcntl(int fd, int cmd, ...) {
  va_list args;
  va_start(args, cmd);
  void *arg = va_arg(args, void *);
  va_end(args);
  int r = fcntl(fd, cmd, arg);
  bool dups = cmd == F_DUPFD;
#ifdef F_DUPFD_CLOEXEC
  dups = dups || cmd == F_DUPFD_CLOEXEC;
#endif
  EFILE *e = (dups && r >= 0) ? mp_fd_lookup(fd) : NULL;
  return e != NULL ? mp_fd_adopt(r, e) : r;
}

/* A virtual fd has no pages of its own: mapping it gives an anonymous
 * mapping holding a copy of the content, which munmap() releases as usual.
 * Like a read-only fd, it cannot back a writable shared mapping. */
MP_DECL(void*) mp_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
  EFILE *e = (flags & MAP_ANON) ? NULL : mp_fd_lookup(fd);
  if (e == NULL) return mmap(addr, len, prot, flags, fd, offset);
  if (e->map->type != ETYPE_FILE) {
    errno = ENODEV;
    return MAP_FAILED;
  }
  if ((prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    errno = EACCES;
    return MAP_FAILED;
  }
  if (len == 0 || offset < 0 || offset % sysconf(_SC_PAGESIZE) != 0) {
    errno = EINVAL;
    return MAP_FAILED;
  }
  char *p = (char*)mmap(addr, len, PROT_READ | PROT_WRITE, (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == (char*)MAP_FAILED) return MAP_FAILED;
  if ((uint64_t)offset < e->size) {
    size_t n = e->size - (size_t)offset;
    memcpy(p, (e->end - e->size) + offset, n < len ? n : len);
  }
  if (prot != (PROT_READ | PROT_WRITE) && mprotect(p, len, prot) != 0) {
    int err = errno;
    munmap(p, len);
    errno = err;
    return MAP_FAILED;
  }
  return p;
}
#endif

#ifdef _WIN32
MP_DECL(__int64) mp_lseeki64(int fd, __int64 offset, int whence) {
#else
MP_DECL(ssize_t) mp_lseek(int fd, off_t offset, int whence) {
#endif
  EFILE *e = mp_fd_lookup(fd);
  if (e == NULL) {
#ifdef _WIN32
    return _lseeki64(fd, offset, whence);
#else
    return lseek(fd, offset, whence);
#endif
  }
#ifdef _WIN32
  mp_sync_in(e);
#endif
  return mp_virtual_seek(e, offset, whence);
}

#ifdef _WIN32

/* Virtual handles live in the same registry as the fds' OS handles. */
static EFILE *mp_handle_lookup(HANDLE h) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) return NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == h) return mp_handle2file[i].f;
    }
    return NULL;
}

/* Registers a handle that owns its EFILE, which CloseHandle() frees. */
static bool mp_handle_register(HANDLE h, EFILE *e) {
    bool registered = false;
    MP_REGISTRY_LOCK();
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == NULL) {
            mp_handle2file[i].f = e;
            mp_handle2file[i].fd_owned = false;
            mp_handle2file[i].h = h;
            registered = true;
            break;
        }
    }
    MP_REGISTRY_UNLOCK();
    return registered;
}

/* Drops a handle from the registry. Returns its EFILE if the handle owned
 * it; an fd-owned handle's EFILE belongs to the fd. */
static EFILE *mp_handle_unregister(HANDLE h) {
    if (mp_handle_lookup(h) == NULL) return NULL;
    EFILE *e = NULL;
    MP_REGISTRY_LOCK();
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == h) {
            if (!mp_handle2file[i].fd_owned) e = mp_handle2file[i].f;
            mp_handle2file[i].h = NULL;
            mp_handle2file[i].f = NULL;
            mp_handle2file[i].fd_owned = false;
            break;
        }
    }
    MP_REGISTRY_UNLOCK();
    return e;
}

/* An entry's mtime as a FILETIME: 100 ns ticks since 1601. */
static FILETIME mp_filetime(const EMAP *map) {
    ULARGE_INTEGER t;
    t.QuadPart = (ULONGLONG)(mp_mtime_ns(map) / 100) + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    return ft;
}

/* The CRT's stat structs differ only in field widths. */
#define MP_FILL_CRT_STAT(buf, map) do {                                        \
    memset((buf), 0, sizeof *(buf));                                           \
    (buf)->st_mode = S_IREAD | S_IEXEC | ((map)->type == ETYPE_DIRECTORY ? S_IFDIR : S_IFREG); \
    (buf)->st_nlink = 1;                                                       \
    (buf)->st_size = mp_uncompressed_size(map);                                \
    (buf)->st_atime = (buf)->st_mtime = (buf)->st_ctime = mp_mtime_ns(map) / 1000000000; \
} while (0)

/* _stat*(), _wstat*() and _fstat*() for one of the CRT's stat structs. */
#define MP_DEFINE_STAT(stat_fn, wstat_fn, fstat_fn, type)                      \
MP_DECL(int) mp_##stat_fn(const char *file, struct type *buf) {               \
    EMAP *map;                                                                 \
    if (!mp_find_virtual(file, &map)) return stat_fn(file, buf);               \
    MP_FILL_CRT_STAT(buf, map);                                                \
    return 0;                                                                  \
}                                                                              \
MP_DECL(int) mp_##wstat_fn(const wchar_t *file, struct type *buf) {           \
    EMAP *map;                                                                 \
    if (!mp_find_virtual_w(file, &map)) return wstat_fn(file, buf);            \
    MP_FILL_CRT_STAT(buf, map);                                                \
    return 0;                                                                  \
}                                                                              \
MP_DECL(int) mp_##fstat_fn(int fd, struct type *buf) {                        \
    EFILE *e = mp_fd_lookup(fd);                                               \
    if (e == NULL) return fstat_fn(fd, buf);                                   \
    MP_FILL_CRT_STAT(buf, e->map);                                             \
    return 0;                                                                  \
}

MP_DEFINE_STAT(_stat32, _wstat32, _fstat32, _stat32)
MP_DEFINE_STAT(_stat64, _wstat64, _fstat64, _stat64)
MP_DEFINE_STAT(_stat32i64, _wstat32i64, _fstat32i64, _stat32i64)
MP_DEFINE_STAT(_stat64i32, _wstat64i32, _fstat64i32, _stat64i32)

MP_STD(DWORD) mp_GetFileAttributesA(LPCSTR lpFileName) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return GetFileAttributesA(lpFileName);
    }
    return mp_GetFileAttributesW(wide_path);
}

MP_STD(DWORD) mp_GetFileAttributesW(LPCWSTR lpFileName) {
    EMAP *map;
    if (mp_find_virtual_w(lpFileName, &map)) {
        DWORD attributes = FILE_ATTRIBUTE_READONLY;
        if (map->type == ETYPE_DIRECTORY) {
            attributes |= FILE_ATTRIBUTE_DIRECTORY;
        }
        return attributes;
    }
    return GetFileAttributesW(lpFileName);
}

MP_STD(BOOL) mp_GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
    }
    return mp_GetFileAttributesExW(wide_path, fInfoLevelId, lpFileInformation);
}

MP_STD(BOOL) mp_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
    // This logic only supports the standard information level.
    if (fInfoLevelId != GetFileExInfoStandard || !lpFileInformation) {
        return GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
    }

    EMAP *map;
    if (mp_find_virtual_w(lpFileName, &map)) {
        WIN32_FILE_ATTRIBUTE_DATA *pData = (WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation;
        memset(pData, 0, sizeof(WIN32_FILE_ATTRIBUTE_DATA));

        pData->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
        if (map->type == ETYPE_DIRECTORY) {
            pData->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        }

        // Populate file size
        ULARGE_INTEGER fileSize;
        fileSize.QuadPart = mp_uncompressed_size(map);
        pData->nFileSizeHigh = fileSize.HighPart;
        pData->nFileSizeLow = fileSize.LowPart;

        pData->ftCreationTime = pData->ftLastAccessTime = pData->ftLastWriteTime = mp_filetime(map);
        return TRUE;
    }
    return GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

/**
 * @brief Closes an open object handle. This can be a handle to a file,
 * mapping, process, thread, etc.
 * @param hObject A valid handle to an open object.
 * @return If the function succeeds, the return value is nonzero.
 *
 * This implementation intercepts handles to virtual files and file mappings,
 * cleans up the associated resources, and then calls the appropriate
 * underlying close function if necessary.
 */
MP_STD(BOOL) mp_CloseHandle(HANDLE hObject) {
    // Check if it's a handle to a virtual file mapping.
    for (int i = 0; i < 512; i++) {
        if (mp_mapping2file[i].h == hObject) {
            // This is a handle to one of our virtual file mappings, not a
            // system object. Its data lives on until the last view is unmapped.
            MP_REGISTRY_LOCK();
            VMAPPING *m = mp_mapping2file[i].h == hObject ? mp_mapping2file[i].m : NULL;
            bool unused = false;
            if (m != NULL) {
                mp_mapping2file[i].h = NULL;
                mp_mapping2file[i].m = NULL;
                m->closed = true;
                unused = m->views == 0;
            }
            MP_REGISTRY_UNLOCK();
            if (m == NULL) {
                SetLastError(ERROR_INVALID_HANDLE);
                return FALSE;
            }
            if (unused) {
                free(m->data);
                free(m);
            }
            return TRUE; // Indicate success.
        }
    }

    // Check if it's a handle to a virtual file.
    mp_efile_destroy(mp_handle_unregister(hObject));

    // Close the native handle: the executable handle behind a virtual file,
    // or one we don't manage.
    return CloseHandle(hObject);
}

MP_STD(HANDLE) mp_CreateFileA(
        LPCSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile
) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    }
    return mp_CreateFileW(wide_path, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

/**
 * @brief Creates or opens a file or I/O device.
 * @return If the function succeeds, the return value is an open handle to the
 * specified file. If the function fails, the return value is INVALID_HANDLE_VALUE.
 *
 * This wrapper checks if the requested file exists in the embedded data.
 * If so, it creates a virtual file handle. Otherwise, it calls the original
 * CreateFileW API.
 */
MP_STD(HANDLE) mp_CreateFileW(
        LPCWSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile
) {
    EMAP *map;
    if (mp_find_virtual_w(lpFileName, &map)) {
        // Virtual files are read-only. If write access is requested, return access denied.
        if (dwDesiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }

        // Virtual files can only be opened if they exist, not created.
        if (dwCreationDisposition == CREATE_NEW || dwCreationDisposition == CREATE_ALWAYS || dwCreationDisposition == TRUNCATE_EXISTING) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }

        if (!(dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) && map->type != ETYPE_FILE) {
            // We are supposed to allow opening a handle to a directory only if
            // FILE_FLAG_BACKUP_SEMANTICS is specified because windows. ¯\_(ツ)_/¯
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        // We do intentionally allow to "open" a directory.

        // The file exists in our virtual file system.
        EFILE* e = (EFILE*)calloc(1, sizeof *e);
        if (!e) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE_VALUE;
        }

        // Metadata only, until the first read or seek (mp_efile_load).
        mp_efile_init_metadata(e, map);

        // Create a real, underlying handle by opening the executable itself.
        // This gives us a valid system handle to return to the caller.
        HANDLE hFake = CreateFileW(mp_executable_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFake == INVALID_HANDLE_VALUE) {
            mp_efile_destroy(e);
            return INVALID_HANDLE_VALUE; // Let caller get the error from CreateFileW.
        }

        if (mp_handle_register(hFake, e)) return hFake;

        // No free slots in our map.
        mp_efile_destroy(e);
        CloseHandle(hFake);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }

    // File not found in virtual FS, call the real API.
    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

/* CreateFile2 - newer Windows SDK entry, used by MSVC <filesystem>. We
 * delegate to mp_CreateFileW after unpacking the extended parameters. */
MP_STD(HANDLE) mp_CreateFile2(
        LPCWSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        DWORD dwCreationDisposition,
        LPCREATEFILE2_EXTENDED_PARAMETERS pCreateExParams
) {
    DWORD dwFlagsAndAttributes = 0;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes = NULL;
    HANDLE hTemplateFile = NULL;
    if (pCreateExParams) {
        dwFlagsAndAttributes = pCreateExParams->dwFileAttributes
                             | pCreateExParams->dwFileFlags
                             | pCreateExParams->dwSecurityQosFlags;
        lpSecurityAttributes = pCreateExParams->lpSecurityAttributes;
        hTemplateFile = pCreateExParams->hTemplateFile;
    }
    return mp_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                          lpSecurityAttributes, dwCreationDisposition,
                          dwFlagsAndAttributes, hTemplateFile);
}

MP_STD(BOOL) mp_DeleteFileA(LPCSTR lpFileName) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return DeleteFileA(lpFileName);
    }
    return mp_DeleteFileW(wide_path);
}

MP_STD(BOOL) mp_DeleteFileW(LPCWSTR lpFileName) {
    EMAP *map;
    if (mp_find_virtual_w(lpFileName, &map)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return DeleteFileW(lpFileName);
}

/**
 * @brief Retrieves information about the specified file.
 * @param hFile A handle to the file.
 * @param lpFileInformation A pointer to a BY_HANDLE_FILE_INFORMATION structure.
 * @return Nonzero on success, zero on failure.
 *
 * Intercepts handles to virtual files and provides the file information from
 * the embedded data map.
 */
MP_STD(BOOL) mp_GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            e = mp_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // Handle belongs to a virtual file.
        if (lpFileInformation == NULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        memset(lpFileInformation, 0, sizeof(BY_HANDLE_FILE_INFORMATION));

        // Set attributes.
        lpFileInformation->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
        if (e->map->type == ETYPE_DIRECTORY) {
            lpFileInformation->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
        }

        // Set file size.
        ULARGE_INTEGER fileSize;
        fileSize.QuadPart = e->size;
        lpFileInformation->nFileSizeHigh = fileSize.HighPart;
        lpFileInformation->nFileSizeLow = fileSize.LowPart;

        // Set link count.
        lpFileInformation->nNumberOfLinks = 1;

        // Use the address of the map entry as a unique file index.
        uintptr_t fileIndex = (uintptr_t)(e->map);
        lpFileInformation->nFileIndexHigh = (DWORD)((uint64_t)fileIndex >> 32);
        lpFileInformation->nFileIndexLow = (DWORD)(fileIndex & 0xFFFFFFFF);

        lpFileInformation->ftCreationTime = lpFileInformation->ftLastAccessTime =
            lpFileInformation->ftLastWriteTime = mp_filetime(e->map);
        // The volume serial is zeroed by memset.
        return TRUE;
    }

    // Not a virtual handle, fall back to the real API.
    return GetFileInformationByHandle(hFile, lpFileInformation);
}

/**
 * @brief Retrieves information for the specified file.
 * @param hFile A handle to the file.
 * @param FileInformationClass The type of information to retrieve.
 * @param lpFileInformation Pointer to the buffer to receive the information.
 * @param dwBufferSize Size of the lpFileInformation buffer.
 * @return Nonzero on success, zero on failure.
 *
 * Supports retrieving basic and standard information for virtual files.
 */
MP_STD(BOOL) mp_GetFileInformationByHandleEx(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            e = mp_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual file.
        if (lpFileInformation == NULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        switch (FileInformationClass) {
            case FileBasicInfo: {
                if (dwBufferSize < sizeof(FILE_BASIC_INFO)) {
                    SetLastError(ERROR_INSUFFICIENT_BUFFER);
                    return FALSE;
                }
                FILE_BASIC_INFO* info = (FILE_BASIC_INFO*)lpFileInformation;
                memset(info, 0, sizeof(FILE_BASIC_INFO));
                info->FileAttributes = FILE_ATTRIBUTE_READONLY;
                if (e->map->type == ETYPE_DIRECTORY) {
                    info->FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
                }
                FILETIME ft = mp_filetime(e->map);
                info->LastWriteTime.LowPart = ft.dwLowDateTime;
                info->LastWriteTime.HighPart = (LONG)ft.dwHighDateTime;
                info->CreationTime = info->LastAccessTime = info->ChangeTime = info->LastWriteTime;
                return TRUE;
            }

            case FileStandardInfo: {
                if (dwBufferSize < sizeof(FILE_STANDARD_INFO)) {
                    SetLastError(ERROR_INSUFFICIENT_BUFFER);
                    return FALSE;
                }
                FILE_STANDARD_INFO* info = (FILE_STANDARD_INFO*)lpFileInformation;
                memset(info, 0, sizeof(FILE_STANDARD_INFO));
                info->AllocationSize.QuadPart = e->size;
                info->EndOfFile.QuadPart = e->size;
                info->NumberOfLinks = 1;
                info->DeletePending = FALSE;
                info->Directory = (e->map->type == ETYPE_DIRECTORY);
                return TRUE;
            }

                // Other information levels are not supported for our virtual files.
            default:
                SetLastError(ERROR_NOT_SUPPORTED);
                return FALSE;
        }
    }

    // Not a virtual file, fall back to the real API.
    return GetFileInformationByHandleEx(hFile, FileInformationClass, lpFileInformation, dwBufferSize);
}


/**
 * @brief Retrieves the type of the specified file.
 * @param hFile A handle to the file.
 * @return One of the following values: FILE_TYPE_CHAR, FILE_TYPE_DISK,
 * FILE_TYPE_PIPE, FILE_TYPE_REMOTE, or FILE_TYPE_UNKNOWN.
 *
 * Reports virtual files as being of type FILE_TYPE_DISK.
 */
MP_STD(DWORD) mp_GetFileType(HANDLE hFile) {
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            // This handle belongs to one of our virtual files.
            // They behave like disk files.
            return FILE_TYPE_DISK;
        }
    }

    // Not a virtual file, fall back to the real API.
    return GetFileType(hFile);
}

/**
 * @brief Retrieves the final path for the specified file.
 * @param hFile A handle to a file or directory.
 * @param lpszFilePath A pointer to a buffer that receives the path.
 * @param cchFilePath The size of the lpszFilePath buffer, in TCHARs.
 * @param dwFlags The options for how the path should be formatted.
 * @return The length of the path copied to lpszFilePath, or the required buffer size.
 *
 * For virtual files, this returns the path stored within the embedded data.
 */
MP_STD(DWORD) mp_GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            e = mp_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // Embedded paths below the executable resolve against the folder
        // Python's getpath uses; "/c/..." entries map back to "c:\...".
        wchar_t resulting_path[PATH_MAX];
        const char *name = e->name;
        size_t used = 0;
        if (name[0] == '~') {
            const wchar_t *folder = mp_execfolder_alias_w[0] != L'\0' ? mp_execfolder_alias_w : mp_execfolder_w;
            wcscpy(resulting_path, folder);
            used = wcslen(resulting_path);
            if (used > 0 && resulting_path[used - 1] == L'\\') used--;
            name++;
        } else if (name[0] == '/' && name[1] != '\0') {
            resulting_path[0] = (wchar_t)name[1];
            resulting_path[1] = L':';
            used = 2;
            name += 2;
        }
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, resulting_path + used, PATH_MAX - (int)used) == 0) {
            SetLastError(ERROR_INVALID_DATA);
            return 0;
        }
        for (wchar_t *p = resulting_path + used; *p; ++p) {
            if (*p == L'/') *p = L'\\';
        }

        size_t required_chars = wcslen(resulting_path);
        if (cchFilePath < required_chars + 1) {
            // Buffer is too small. Return the required size.
            return (DWORD)(required_chars + 1);
        }
        wcscpy(lpszFilePath, resulting_path);
        return (DWORD)required_chars;
    }

    // Not a virtual file, fall back to the real API.
    return GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

/**
 * @brief Retrieves the full path and file name of the specified file.
 * @return The length of the string copied to lpBuffer, or the required buffer size.
 *
 * The real API is purely lexical and never touches the disk, so it already
 * gives the right answer for virtual paths.
 */
MP_STD(DWORD) mp_GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart) {
    return GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

/**
 * @brief Retrieves the volume mount point where the specified path is located.
 * @return Nonzero on success, zero on failure.
 *
 * For virtual files, this retrieves the volume of the executable itself.
 */
MP_STD(BOOL) mp_GetVolumePathNameW(LPCWSTR lpszFileName, LPWSTR lpszVolumePathName, DWORD cchBufferLength) {
    EMAP *map;
    if (mp_find_virtual_w(lpszFileName, &map)) {
        // It's a virtual file. The volume is the volume of the executable.
        return GetVolumePathNameW(mp_executable_w, lpszVolumePathName, cchBufferLength);
    }

    // Not a virtual file, fall back to the real API.
    return GetVolumePathNameW(lpszFileName, lpszVolumePathName, cchBufferLength);
}

/**
 * @brief Retrieves information about the amount of space on a disk volume.
 * @return Nonzero on success, zero on failure.
 *
 * For virtual paths, this returns the disk space for the volume hosting the executable.
 */
MP_STD(BOOL) mp_GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    EMAP *map;
    if (lpDirectoryName != NULL && mp_find_virtual_w(lpDirectoryName, &map) && map->type == ETYPE_DIRECTORY) {
        // It's a virtual directory. The disk space is the one for the executable's drive.
        return GetDiskFreeSpaceExW(mp_execfolder_w, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
    }

    // Fall back for real paths.
    return GetDiskFreeSpaceExW(lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

// Helper to convert WIN32_FIND_DATAW to WIN32_FIND_DATAA
static void ConvertFindDataWtoA(LPWIN32_FIND_DATAA lpFindFileDataA, const WIN32_FIND_DATAW* lpFindFileDataW) {
    lpFindFileDataA->dwFileAttributes = lpFindFileDataW->dwFileAttributes;
    lpFindFileDataA->ftCreationTime = lpFindFileDataW->ftCreationTime;
    lpFindFileDataA->ftLastAccessTime = lpFindFileDataW->ftLastAccessTime;
    lpFindFileDataA->ftLastWriteTime = lpFindFileDataW->ftLastWriteTime;
    lpFindFileDataA->nFileSizeHigh = lpFindFileDataW->nFileSizeHigh;
    lpFindFileDataA->nFileSizeLow = lpFindFileDataW->nFileSizeLow;
    lpFindFileDataA->dwReserved0 = lpFindFileDataW->dwReserved0;
    lpFindFileDataA->dwReserved1 = lpFindFileDataW->dwReserved1;
    WideCharToMultiByte(CP_ACP, 0, lpFindFileDataW->cFileName, -1, lpFindFileDataA->cFileName, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, lpFindFileDataW->cAlternateFileName, -1, lpFindFileDataA->cAlternateFileName, 14, NULL, NULL);
}

// Last component of an entry's path; entry paths are stored as UTF-8.
static void mp_entry_name_w(const EMAP* map, wchar_t* out, int cch) {
    const char* path = mp_path_of(map);
    const char* slash = strrchr(path, '/');
    if (MultiByteToWideChar(CP_UTF8, 0, slash ? slash + 1 : path, -1, out, cch) == 0) {
        out[0] = L'\0';
    }
}

// Helper to populate WIN32_FIND_DATAW from an EMAP entry for FindFirst/Next
static void populate_find_data(EMAP* map, WIN32_FIND_DATAW* find_data) {
    memset(find_data, 0, sizeof(WIN32_FIND_DATAW));

    mp_entry_name_w(map, find_data->cFileName, _countof(find_data->cFileName));

    // Set attributes
    find_data->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    if (map->type == ETYPE_DIRECTORY) {
        find_data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }

    // Set file size
    ULARGE_INTEGER fileSize;
    fileSize.QuadPart = mp_uncompressed_size(map);
    find_data->nFileSizeHigh = fileSize.HighPart;
    find_data->nFileSizeLow = fileSize.LowPart;

    find_data->ftCreationTime = find_data->ftLastAccessTime = find_data->ftLastWriteTime = mp_filetime(map);
}

/* Next match of a virtual directory search: its embedded entries, then the
 * entries of the same directory on disk that they do not shadow. */
static bool mp_find_next(VIRTUAL_FIND_HANDLE_DATA* data, WIN32_FIND_DATAW* out) {
    const EMAP *dir_map = data->dir_map;
    while (data->next_child < dir_map->children_count) {
        const EMAP* child = mp_embed_child_at(dir_map, data->next_child++);
        wchar_t filename_part[MAX_PATH];
        mp_entry_name_w(child, filename_part, MAX_PATH);
        if (PathMatchSpecW(filename_part, data->search_pattern)) {
            populate_find_data((EMAP*)child, out);
            return true;
        }
    }
    while (data->real_ready) {
        WIN32_FIND_DATAW found = data->real_next;
        data->real_ready = FindNextFileW(data->real, &data->real_next) != FALSE;
        char name[MAX_PATH * 4];
        if (WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof name, NULL, NULL) == 0 ||
            !mp_shadowed(dir_map, name)) {
            *out = found;
            return true;
        }
    }
    return false;
}

static void mp_find_free(VIRTUAL_FIND_HANDLE_DATA* data) {
    if (data->real != INVALID_HANDLE_VALUE) FindClose(data->real);
    free(data);
}

MP_STD(HANDLE) mp_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return FindFirstFileA(lpFileName, lpFindFileData);
    }

    WIN32_FIND_DATAW find_data_w;
    HANDLE hFindFile = mp_FindFirstFileW(wide_path, &find_data_w);

    if (hFindFile != INVALID_HANDLE_VALUE) {
        ConvertFindDataWtoA(lpFindFileData, &find_data_w);
    }

    return hFindFile;
}

MP_STD(BOOL) mp_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
    VIRTUAL_FIND_HANDLE_DATA* find_handle_data = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_findhandles[i].h == hFindFile) {
            find_handle_data = mp_findhandles[i].data;
            break;
        }
    }

    if (find_handle_data == NULL) {
        return FindNextFileA(hFindFile, lpFindFileData);
    }

    WIN32_FIND_DATAW find_data_w;
    if (mp_FindNextFileW(hFindFile, &find_data_w)) {
        ConvertFindDataWtoA(lpFindFileData, &find_data_w);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief Searches a directory for a file or subdirectory.
 * @return A search handle on success, or INVALID_HANDLE_VALUE on failure.
 *
 * Intercepts searches in virtual directories.
 */
MP_STD(HANDLE) mp_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    if (lpFileName == NULL || wcslen(lpFileName) >= PATH_MAX) {
        return FindFirstFileW(lpFileName, lpFindFileData);
    }
    wchar_t search_dir[PATH_MAX];
    wcscpy(search_dir, lpFileName);
    PathRemoveFileSpecW(search_dir);
    if (search_dir[0] == L'\0') {
        wcscpy(search_dir, L".");
    }
    const wchar_t* search_pattern = PathFindFileNameW(lpFileName);

    EMAP *dir_map;
    if (mp_find_virtual_w(search_dir, &dir_map) && dir_map->type == ETYPE_DIRECTORY) {
        VIRTUAL_FIND_HANDLE_DATA* find_handle_data = (VIRTUAL_FIND_HANDLE_DATA*)calloc(1, sizeof(VIRTUAL_FIND_HANDLE_DATA));
        if (!find_handle_data) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE_VALUE;
        }
        find_handle_data->dir_map = dir_map;
        wcsncpy(find_handle_data->search_pattern, search_pattern, MAX_PATH - 1);
        find_handle_data->search_pattern[MAX_PATH - 1] = L'\0';
        find_handle_data->real = FindFirstFileW(lpFileName, &find_handle_data->real_next);
        find_handle_data->real_ready = find_handle_data->real != INVALID_HANDLE_VALUE;

        WIN32_FIND_DATAW first;
        if (!mp_find_next(find_handle_data, &first)) {
            mp_find_free(find_handle_data);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        MP_REGISTRY_LOCK();
        for (int i = 0; i < 512; i++) {
            if (mp_findhandles[i].h == NULL) {
                mp_findhandles[i].data = find_handle_data;
                mp_findhandles[i].h = (HANDLE)find_handle_data;
                MP_REGISTRY_UNLOCK();
                *lpFindFileData = first;
                return (HANDLE)find_handle_data;
            }
        }
        MP_REGISTRY_UNLOCK();
        mp_find_free(find_handle_data);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }

    return FindFirstFileW(lpFileName, lpFindFileData);
}

/**
 * @brief Continues a file search from a previous call to FindFirstFileW.
 * @return Nonzero on success, zero on failure.
 */
MP_STD(BOOL) mp_FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
    VIRTUAL_FIND_HANDLE_DATA* find_handle_data = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_findhandles[i].h == hFindFile) {
            find_handle_data = mp_findhandles[i].data;
            break;
        }
    }

    if (find_handle_data == NULL) {
        return FindNextFileW(hFindFile, lpFindFileData);
    }

    if (mp_find_next(find_handle_data, lpFindFileData)) return TRUE;
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

/**
 * @brief Closes a file search handle.
 * @return Nonzero on success, zero on failure.
 */
MP_STD(BOOL) mp_FindClose(HANDLE hFindFile) {
    for (int i = 0; i < 512; i++) {
        if (mp_findhandles[i].h == hFindFile) {
            VIRTUAL_FIND_HANDLE_DATA* find_handle_data = mp_findhandles[i].data;
            MP_REGISTRY_LOCK();
            mp_findhandles[i].h = NULL;
            mp_findhandles[i].data = NULL;
            MP_REGISTRY_UNLOCK();
            mp_find_free(find_handle_data);
            return TRUE;
        }
    }

    return FindClose(hFindFile);
}

/**
 * @brief Retrieves the size of the specified file, in bytes.
 * @param hFile A handle to the file.
 * @param lpFileSize A pointer to a variable where the file size is returned.
 * @return If the function succeeds, the return value is nonzero.
 *
 * Intercepts handles to virtual files and provides the file size from the
 * embedded data map.
 */
MP_STD(BOOL) mp_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            e = mp_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual file.
        if (lpFileSize == NULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        lpFileSize->QuadPart = e->size;
        return TRUE;
    }

    // Not a virtual file, fall back to the real API.
    return GetFileSizeEx(hFile, lpFileSize);
}

/**
 * @brief Retrieves the size of the specified file. (Legacy)
 * @param hFile A handle to the file.
 * @param lpFileSizeHigh A pointer to a variable that receives the high-order DWORD of the file size. Can be NULL.
 * @return If the function succeeds, the return value is the low-order DWORD of the file size.
 * If the function fails and lpFileSizeHigh is NULL, the return value is INVALID_FILE_SIZE.
 *
 * Intercepts handles to virtual files and provides the file size from the
 * embedded data map.
 */
MP_STD(DWORD) mp_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            e = mp_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual file.
        ULARGE_INTEGER fileSize;
        fileSize.QuadPart = e->size;

        if (lpFileSizeHigh != NULL) {
            *lpFileSizeHigh = fileSize.HighPart;
        }

        // The original API returns INVALID_FILE_SIZE on failure. If the low part
        // is exactly that value, we must clear the last error to indicate success.
        if (fileSize.LowPart == INVALID_FILE_SIZE) {
            SetLastError(NO_ERROR);
        }

        return fileSize.LowPart;
    }

    // Not a virtual file, fall back to the real API.
    return GetFileSize(hFile, lpFileSizeHigh);
}

MP_STD(HANDLE) mp_CreateFileMappingA(
    HANDLE                hFile,
    LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
    DWORD                 flProtect,
    DWORD                 dwMaximumSizeHigh,
    DWORD                 dwMaximumSizeLow,
    LPCSTR                lpName
) {
    // If a name is provided for the mapping object, we need to convert it
    // to a wide character string to pass to the mp_CreateFileMappingW function.
    if (lpName) {
        // Determine the required buffer size for the wide string.
        int required_chars = MultiByteToWideChar(CP_ACP, 0, lpName, -1, NULL, 0);
        if (required_chars == 0) {
             // Let the real API handle the error.
            return CreateFileMappingA(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
        }

        // Allocate memory for the wide string.
        wchar_t* wide_name = (wchar_t*)malloc(required_chars * sizeof(wchar_t));
        if (wide_name == NULL) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }

        // Perform the conversion.
        MultiByteToWideChar(CP_ACP, 0, lpName, -1, wide_name, required_chars);

        // Call our wide-character implementation with the converted name.
        HANDLE result = mp_CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, wide_name);

        // Free the allocated memory for the wide string.
        free(wide_name);

        return result;
    } else {
        // If no name is provided, we can call the wide-character version directly with NULL.
        return mp_CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, NULL);
    }
}


MP_STD(HANDLE) mp_CreateFileMappingW(
    HANDLE                hFile,
    LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
    DWORD                 flProtect,
    DWORD                 dwMaximumSizeHigh,
    DWORD                 dwMaximumSizeLow,
    LPCWSTR               lpName
) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hFile) {
            e = mp_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // This is a virtual file.
        // Virtual files are read-only, so deny writeable mappings.
        if (flProtect & (PAGE_EXECUTE_READWRITE | PAGE_READWRITE)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return NULL;
        }

        // Like a real empty file, an empty virtual file cannot be mapped.
        if (e->size == 0) {
            SetLastError(ERROR_FILE_INVALID);
            return NULL;
        }

        // The size of the mapping must be within the file's bounds.
        ULONGLONG maxSize = ((ULONGLONG)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
        if (maxSize > e->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }

        // We can't create a real mapping object for in-memory data. The
        // mapping gets its own decompressed copy, and its address serves as
        // the "handle".
        VMAPPING* m = (VMAPPING*)calloc(1, sizeof *m);
        if (m == NULL) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        m->data = mp_decompress_entry(e->map, 0, &m->size);
        if (m->data == NULL) {
            free(m);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        MP_REGISTRY_LOCK();
        for (int i = 0; i < 512; i++) {
            if (mp_mapping2file[i].h == NULL) {
                mp_mapping2file[i].m = m;
                mp_mapping2file[i].h = (HANDLE)m;
                MP_REGISTRY_UNLOCK();
                return (HANDLE)m;
            }
        }
        MP_REGISTRY_UNLOCK();
        free(m->data);
        free(m);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return NULL;
    }

    // Not a virtual file, fall back to the real API.
    return CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
}

MP_STD(LPVOID) mp_MapViewOfFile(
    HANDLE hFileMappingObject,
    DWORD  dwDesiredAccess,
    DWORD  dwFileOffsetHigh,
    DWORD  dwFileOffsetLow,
    SIZE_T dwNumberOfBytesToMap
) {
    VMAPPING* m = NULL;
    for (int i = 0; i < 512; i++) {
        // Check if the mapping object is one of our virtual ones.
        if (mp_mapping2file[i].h == hFileMappingObject) {
            m = mp_mapping2file[i].m;
            break;
        }
    }

    if (m != NULL) {
        // It's a virtual mapping. The "view" is just a pointer into our data block.
        // Check for write access, which is not allowed.
        if (dwDesiredAccess & FILE_MAP_WRITE) {
            SetLastError(ERROR_ACCESS_DENIED);
            return NULL;
        }

        ULONGLONG offset = ((ULONGLONG)dwFileOffsetHigh << 32) | dwFileOffsetLow;
        if (offset >= m->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }

        LPVOID view = (LPVOID)(m->data + offset);

        // Store the view so we can recognize it in UnmapViewOfFile
        MP_REGISTRY_LOCK();
        for (int i = 0; i < 512; i++) {
            if (mp_view2file[i].view == NULL) {
                mp_view2file[i].m = m;
                mp_view2file[i].view = view;
                m->views++;
                MP_REGISTRY_UNLOCK();
                return view;
            }
        }
        MP_REGISTRY_UNLOCK();
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return NULL;
    }

    // Not a virtual mapping, fall back to the real API.
    return MapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
}

MP_STD(BOOL) mp_UnmapViewOfFile(
    LPCVOID lpBaseAddress
) {
    for (int i = 0; i < 512; i++) {
        if (mp_view2file[i].view == lpBaseAddress) {
            // This is a view of one of our virtual files. The data belongs to
            // the mapping, freed here only if the mapping is already closed.
            MP_REGISTRY_LOCK();
            VMAPPING* m = mp_view2file[i].m;
            mp_view2file[i].view = NULL;
            mp_view2file[i].m = NULL;
            bool unused = --m->views == 0 && m->closed;
            MP_REGISTRY_UNLOCK();
            if (unused) {
                free(m->data);
                free(m);
            }
            return TRUE;
        }
    }

    // Not a virtual view, fall back to the real API.
    return UnmapViewOfFile(lpBaseAddress);
}

/* A CreateFile handle starts out metadata-only, as os.stat() opens one per
 * call; the first read or seek through it decompresses the content. */
static bool mp_efile_load(EFILE *e) {
    if (e->owned_buf != NULL || e->map->type != ETYPE_FILE) return true;
    EFILE loaded;
    memset(&loaded, 0, sizeof loaded);
    if (!mp_efile_init_virtual(&loaded, e->map)) return false;
    MP_REGISTRY_LOCK();
    bool first = e->owned_buf == NULL;
    if (first) {
        e->pos = loaded.pos;
        e->end = loaded.end;
        e->owned_buf = loaded.owned_buf;
        e->crt_base = loaded.crt_base;
        e->crt_ptr = loaded.crt_ptr;
        e->crt_cnt = loaded.crt_cnt;
    }
    MP_REGISTRY_UNLOCK();
    if (!first) free(loaded.owned_buf);
    return true;
}

MP_STD(BOOL) mp_ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
    EFILE *e = mp_handle_lookup(hFile);
    if (e == NULL) {
        return ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }
    if (lpNumberOfBytesRead != NULL) *lpNumberOfBytesRead = 0;
    if (e->map->type != ETYPE_FILE) {
        SetLastError(ERROR_INVALID_FUNCTION);
        return FALSE;
    }
    if (!mp_efile_load(e)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    mp_sync_in(e);
    // An OVERLAPPED gives the offset, and the read completes synchronously.
    if (lpOverlapped != NULL) {
        mp_virtual_seek(e, (int64_t)(((uint64_t)lpOverlapped->OffsetHigh << 32) | lpOverlapped->Offset), SEEK_SET);
    }
    size_t n = mp_avail(e);
    if (n > nNumberOfBytesToRead) n = nNumberOfBytesToRead;
    if (n != 0) memcpy(lpBuffer, e->pos, n);
    e->pos += n;
    mp_sync_out(e);
    if (lpNumberOfBytesRead != NULL) *lpNumberOfBytesRead = (DWORD)n;
    if (lpOverlapped != NULL) {
        bool eof = n == 0 && nNumberOfBytesToRead != 0;
        lpOverlapped->Internal = eof ? (ULONG_PTR)0xC0000011L /* STATUS_END_OF_FILE */ : 0;
        lpOverlapped->InternalHigh = n;
        if (lpOverlapped->hEvent != NULL) SetEvent(lpOverlapped->hEvent);
        if (eof) {
            SetLastError(ERROR_HANDLE_EOF);
            return FALSE;
        }
    }
    return TRUE;
}

/* SetFilePointer(Ex) on a virtual handle: the new position, or -1 with the
 * last error set. FILE_BEGIN/CURRENT/END are SEEK_SET/CUR/END. */
static int64_t mp_handle_seek(EFILE *e, int64_t distance, DWORD method) {
    if (method > FILE_END) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (!mp_efile_load(e)) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return -1;
    }
    mp_sync_in(e);
    int64_t at = mp_virtual_seek(e, distance, (int)method);
    if (at < 0) SetLastError(ERROR_NEGATIVE_SEEK);
    return at;
}

MP_STD(DWORD) mp_SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod) {
    EFILE *e = mp_handle_lookup(hFile);
    if (e == NULL) {
        return SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
    }
    int64_t distance = lpDistanceToMoveHigh == NULL ? (int64_t)lDistanceToMove :
        (int64_t)(((uint64_t)(uint32_t)*lpDistanceToMoveHigh << 32) | (uint32_t)lDistanceToMove);
    int64_t at = mp_handle_seek(e, distance, dwMoveMethod);
    if (at < 0) return INVALID_SET_FILE_POINTER;
    if (lpDistanceToMoveHigh != NULL) *lpDistanceToMoveHigh = (LONG)(at >> 32);
    // A low part equal to INVALID_SET_FILE_POINTER is told apart by the last error.
    SetLastError(NO_ERROR);
    return (DWORD)at;
}

MP_STD(BOOL) mp_SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer, DWORD dwMoveMethod) {
    EFILE *e = mp_handle_lookup(hFile);
    if (e == NULL) {
        return SetFilePointerEx(hFile, liDistanceToMove, lpNewFilePointer, dwMoveMethod);
    }
    int64_t at = mp_handle_seek(e, liDistanceToMove.QuadPart, dwMoveMethod);
    if (at < 0) return FALSE;
    if (lpNewFilePointer != NULL) lpNewFilePointer->QuadPart = at;
    return TRUE;
}

static bool mp_is_this_process(HANDLE process) {
    return process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId();
}

/* A duplicate of a virtual handle in this process is registered with a copy
 * of its EFILE (Python's mmap maps the duplicate of the fd's handle). */
MP_STD(BOOL) mp_DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle, LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions) {
    EFILE *e = mp_handle_lookup(hSourceHandle);
    if (e == NULL || !mp_is_this_process(hSourceProcessHandle)) {
        return DuplicateHandle(hSourceProcessHandle, hSourceHandle, hTargetProcessHandle, lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions);
    }
    EFILE *copy = NULL;
    if (lpTargetHandle != NULL && mp_is_this_process(hTargetProcessHandle)) {
        copy = mp_efile_dup(e);
        if (copy == NULL) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
    }
    // DUPLICATE_CLOSE_SOURCE closes the source handle even if the call fails.
    EFILE *closed = (dwOptions & DUPLICATE_CLOSE_SOURCE) ? mp_handle_unregister(hSourceHandle) : NULL;
    BOOL ok = DuplicateHandle(hSourceProcessHandle, hSourceHandle, hTargetProcessHandle, lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions);
    mp_efile_destroy(closed);
    if (!ok || copy == NULL) {
        mp_efile_destroy(copy);
        return ok;
    }
    if (!mp_handle_register(*lpTargetHandle, copy)) {
        CloseHandle(*lpTargetHandle);
        mp_efile_destroy(copy);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return FALSE;
    }
    return TRUE;
}
#else
MP_DECL(int) mp_stat(const char *file, struct stat *buf) {
  EMAP *map;
  if (mp_find_virtual(file, &map)) {
    mp_fill_stat(map, buf);
    return 0;
  }
  return stat(file, buf);
}

MP_DECL(int) mp_lstat(const char *file, struct stat *buf) {
  EMAP *map;
  if (mp_find_virtual(file, &map)) {
    mp_fill_stat(map, buf);
    return 0;
  }
  return lstat(file, buf);
}

MP_DECL(int) mp_fstat(int fd, struct stat *buf) {
  // Check if the file descriptor is for a virtual file.
  EFILE *e = mp_fd_lookup(fd);
  if (e != NULL) {
    mp_fill_stat(e->map, buf);
    return 0;
  }

  // If the fd is not in our virtual file table, fall back to the native call.
  return fstat(fd, buf);
}

MP_DECL(int) mp_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
#ifdef AT_EMPTY_PATH
  if (pathname[0] == '\0' && (flags & AT_EMPTY_PATH)) return mp_fstat(dirfd, buf);
#endif
  // The virtual file system does not support symlinks, so AT_SYMLINK_NOFOLLOW
  // is effectively ignored as the behavior is the same as lstat/stat.
  EMAP *map;
  int found = mp_find_virtual_at(dirfd, pathname, &map);
  if (found > 0) {
    mp_fill_stat(map, buf);
    return 0;
  }
  if (found < 0) return -1;

  // Fallback to the real fstatat for non-virtual files.
  return fstatat(dirfd, pathname, buf, flags);
}

/* Virtual entries exist and can be read and searched, never written. */
static int mp_virtual_access(int mode) {
  if (mode & W_OK) {
    errno = EROFS;
    return -1;
  }
  return 0;
}

MP_DECL(int) mp_access(const char *pathname, int mode) {
  EMAP *map;
  if (mp_find_virtual(pathname, &map)) return mp_virtual_access(mode);
  return access(pathname, mode);
}

MP_DECL(int) mp_faccessat(int dirfd, const char *pathname, int mode, int flags) {
  EMAP *map;
  int found = mp_find_virtual_at(dirfd, pathname, &map);
  if (found > 0) return mp_virtual_access(mode);
  if (found < 0) return -1;
  return faccessat(dirfd, pathname, mode, flags);
}

MP_DECL(int) mp_statvfs(const char *path, struct statvfs *buf) {
  EMAP *map;
  if (mp_find_virtual(path, &map)) {
    // Virtual file system lives on the same FS as the executable
    return statvfs(mp_execfolder, buf);
  }
  return statvfs(path, buf);
}

MP_DECL(int) mp_fstatvfs(int fd, struct statvfs *buf) {
  if (mp_fd_lookup(fd) != NULL) {
    return statvfs(mp_execfolder, buf);
  }
  return fstatvfs(fd, buf);
}

MP_DECL(ssize_t) mp_pwrite(int fd, const void *buf, size_t count, off_t offset) {
  if (mp_fd_lookup(fd) != NULL) {
    errno = EROFS; // Read-only file system
    return -1;
  }
  return pwrite(fd, buf, count, offset);
}

MP_DECL(ssize_t) mp_readv(int fd, const struct iovec *iov, int iovcnt) {
  EFILE *e = mp_fd_lookup(fd);
  if (e == NULL) {
    return readv(fd, iov, iovcnt);
  }
  if (e->map->type == ETYPE_DIRECTORY) {
    errno = EISDIR;
    return -1;
  }

  ssize_t total_read = 0;
  for (int i = 0; i < iovcnt; i++) {
    size_t bytes_to_read = iov[i].iov_len;
    size_t remaining_in_file = mp_avail(e);
    if (bytes_to_read > remaining_in_file) {
      bytes_to_read = remaining_in_file;
    }
    memcpy(iov[i].iov_base, e->pos, bytes_to_read);
    e->pos += bytes_to_read;
    total_read += bytes_to_read;
    if (bytes_to_read < iov[i].iov_len) {
      // Reached EOF
      break;
    }
  }
  return total_read;
}

MP_DECL(ssize_t) mp_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  EFILE *e = mp_fd_lookup(fd);
  if (e == NULL) {
    return preadv(fd, iov, iovcnt, offset);
  }

  if (e->map->type == ETYPE_DIRECTORY) {
    errno = EISDIR;
    return -1;
  }
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  if ((uint64_t)offset >= e->size) return 0;

  const char* start_pos = (e->end - e->size) + offset;
  ssize_t total_read = 0;
  for (int i = 0; i < iovcnt; i++) {
    size_t bytes_to_read = iov[i].iov_len;
    size_t remaining_in_file = (size_t)(e->end - start_pos);
    if (bytes_to_read > remaining_in_file) {
      bytes_to_read = remaining_in_file;
    }
    memcpy(iov[i].iov_base, start_pos, bytes_to_read);
    start_pos += bytes_to_read;
    total_read += bytes_to_read;
    if (bytes_to_read < iov[i].iov_len) {
      break;
    }
  }
  return total_read;
}

#ifdef __linux
MP_DECL(ssize_t) mp_preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  if (mp_fd_lookup(fd) == NULL) return preadv2(fd, iov, iovcnt, offset, flags);
  // Ignoring flags for virtual files; offset -1 reads at the file position.
  return offset == -1 ? mp_readv(fd, iov, iovcnt) : mp_preadv(fd, iov, iovcnt, offset);
}
#endif

MP_DECL(ssize_t) mp_writev(int fd, const struct iovec *iov, int iovcnt)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  if (mp_fd_lookup(fd) != NULL) {
    errno = EROFS; // Read-only
    return -1;
  }
  return writev(fd, iov, iovcnt);
}

MP_DECL(ssize_t) mp_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  if (mp_fd_lookup(fd) != NULL) {
    errno = EROFS; // Read-only
    return -1;
  }
  return pwritev(fd, iov, iovcnt, offset);
}

#ifdef __linux
MP_DECL(ssize_t) mp_pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  if (mp_fd_lookup(fd) != NULL) {
    errno = EROFS; // Read-only
    return -1;
  }
  return pwritev2(fd, iov, iovcnt, offset, flags);
}
#endif

typedef struct VDIR_S {
  const EMAP* dir_map;       // The directory entry being iterated
  uint32_t next_child;       // Index into dir_map->children of next entry to return
  int fd;                    // fd taken over by fdopendir(), closed by closedir(); else -1
  DIR *real;                 // The same directory on disk, listed after dir_map; or NULL
  struct dirent entry;       // The current entry to be returned
} VDIR;

// A map to associate DIR* pointers with our VDIR structs
VDIR* mp_open_vdirs[512] = {};

/* Opens the directory on disk that a virtual directory lies over, if any:
 * files there stay visible in listings. "~/..." is below the executable. */
static DIR *mp_vdir_open_real(const EMAP *dir_map) {
  const char *vpath = mp_path_of(dir_map);
  char path[PATH_MAX];
  if (vpath[0] == '~') snprintf(path, PATH_MAX, "%s%s", mp_execfolder, vpath + 1);
  else snprintf(path, PATH_MAX, "%s", vpath);
  int err = errno;
  DIR *real = opendir(path);
  errno = err;
  return real;
}

static DIR *mp_vdir_open(const EMAP *dir_map, int fd) {
  VDIR* vdir = (VDIR*)calloc(1, sizeof(VDIR));
  if (!vdir) {
    errno = ENOMEM;
    return NULL;
  }
  vdir->dir_map = dir_map;
  vdir->fd = fd;
  vdir->real = mp_vdir_open_real(dir_map);

  MP_REGISTRY_LOCK();
  for (int i = 0; i < 512; i++) {
    if (mp_open_vdirs[i] == NULL) {
      mp_open_vdirs[i] = vdir;
      MP_REGISTRY_UNLOCK();
      return (DIR*)vdir;
    }
  }
  MP_REGISTRY_UNLOCK();
  if (vdir->real) closedir(vdir->real);
  free(vdir);
  errno = EMFILE; // Too many open files
  return NULL;
}

static VDIR *mp_vdir_lookup(DIR *dirp) {
  for (int i = 0; i < 512; i++) {
    if (mp_open_vdirs[i] == (VDIR*)dirp) return (VDIR*)dirp;
  }
  return NULL;
}

MP_DECL(DIR*) mp_opendir(const char* name) {
  EMAP *dir_map;
  if (mp_find_virtual(name, &dir_map)) {
    if (dir_map->type != ETYPE_DIRECTORY) {
      errno = ENOTDIR;
      return NULL;
    }
    return mp_vdir_open(dir_map, -1);
  }
  return opendir(name);
}

MP_DECL(DIR*) mp_fdopendir(int fd) {
  EFILE *e = mp_fd_lookup(fd);
  if (e != NULL) {
    // We have a virtual file descriptor. We can open it if it's a directory.
    if (e->map->type != ETYPE_DIRECTORY) {
      errno = ENOTDIR;
      return NULL;
    }
    return mp_vdir_open(e->map, fd);
  }
  return fdopendir(fd);
}

MP_DECL(struct dirent*) mp_readdir(DIR *dirp) {
  VDIR* vdir = mp_vdir_lookup(dirp);
  if (vdir == NULL) {
    return readdir(dirp);
  }

  if (vdir->next_child >= vdir->dir_map->children_count) {
    struct dirent *real;
    while (vdir->real && (real = readdir(vdir->real)) != NULL) {
      if (!mp_shadowed(vdir->dir_map, real->d_name)) return real;
    }
    return NULL; // No more entries
  }
  const EMAP* current_map = mp_embed_child_at(vdir->dir_map, vdir->next_child++);
  const char* full_path = mp_path_of(current_map);

  // Extract just the filename component (last segment after final '/').
  const char* filename_part = strrchr(full_path, '/');
  filename_part = filename_part ? filename_part + 1 : full_path;

  strncpy(vdir->entry.d_name, filename_part, sizeof(vdir->entry.d_name));
  vdir->entry.d_name[sizeof(vdir->entry.d_name) - 1] = '\0';
#ifdef __APPLE__
  vdir->entry.d_namlen = (uint16_t)strlen(vdir->entry.d_name);
#endif
  vdir->entry.d_ino = MP_VIRTUAL_INO_BASE + (ino_t)(current_map - mp_entries);
  vdir->entry.d_type = (current_map->type == ETYPE_DIRECTORY) ? DT_DIR : DT_REG;
  return &vdir->entry;
}

MP_DECL(int) mp_closedir(DIR *dirp) {
  VDIR* vdir = mp_vdir_lookup(dirp);
  if (vdir == NULL) {
    return closedir(dirp);
  }
  MP_REGISTRY_LOCK();
  for (int i = 0; i < 512; i++) {
    if (mp_open_vdirs[i] == vdir) {
      mp_open_vdirs[i] = NULL;
      break;
    }
  }
  MP_REGISTRY_UNLOCK();
  int fd = vdir->fd;
  if (vdir->real) closedir(vdir->real);
  free(vdir);
  return fd >= 0 ? mp_close(fd) : 0;
}

MP_DECL(void) mp_rewinddir(DIR *dirp) {
  VDIR* vdir = mp_vdir_lookup(dirp);
  if (vdir != NULL) {
    vdir->next_child = 0;
    if (vdir->real) rewinddir(vdir->real);
  } else {
    rewinddir(dirp);
  }
}

#endif
