#define MONOLITHPY_EMBED_BUILD
#define _FILE_OFFSET_BITS 64
#include "mp_embed.h"
#define PATH_MAX 4096
#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
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

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
/* Protects mp_fd2file slot allocation/free. The std::ifstream
 * threading torture (16 threads x 200 iters of open/read/close)
 * otherwise races on slot reuse. */
static pthread_mutex_t mp_fd2file_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifdef _WIN32
// A map to associate mapping HANDLEs with our virtual EFILE structs.
struct VMAP_S {   // Virtual File Mapping
    HANDLE h;
    EFILE *f;
};
typedef struct VMAP_S VMAP;

// An array to store the mappings for open virtual files.
VMAP mp_mapping2file[512] = {};

// A map to associate view pointers with our virtual EFILE structs
struct VIEWMAP_S { // Virtual File View
    LPVOID view;
    EFILE *f;
};
typedef struct VIEWMAP_S VIEWMAP;

VIEWMAP mp_view2file[512] = {};

// For FindFirst/NextFile functionality
typedef struct VIRTUAL_FIND_HANDLE_DATA_S {
    const EMAP* dir_map;        // The directory entry being iterated
    uint32_t next_child;        // Index into dir_map->children of next entry to return
    char search_pattern[PATH_MAX]; // The file pattern (e.g., "*.txt")
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

/* Backwards-compatible wrapper: existing code does
 *   if (find_embedded_file(path, &map)) ...
 * with `EMAP *map`. Cast away const since the callers don't write. */
static bool find_embedded_file(char *search_path, EMAP **found_map) {
  /* Lowercase the input path. (get_virtual_path already does this for
   * paths derived from absolute paths, but some call sites pass other
   * paths through.) */
  for (char *p = search_path; *p; ++p) *p = (char)tolower((unsigned char)*p);
  const char *dbg = getenv("MP_EMBED_DEBUG");
  if (dbg && *dbg) {
    fprintf(stderr, "[mp_embed] find: '%s'\n", search_path);
  }
  const EMAP *e = mp_embed_find(search_path);
  if (dbg && *dbg) {
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

/* Decompress the zstd frame for `map` into a freshly malloc'd buffer.
 * Returns NULL on failure. *out_size receives the uncompressed size. */
static char *mp_decompress_entry(const EMAP *map, size_t *out_size) {
  if (map->data_size == 0) {
    /* Empty file shortcut - mkembeddata stores no frame at all. */
    *out_size = 0;
    return NULL;
  }
  const char *src = (const char*)&nuitka_embed_data + map->data_pos;
  unsigned long long ucs = ZSTD_getFrameContentSize(src, map->data_size);
  if (ucs == ZSTD_CONTENTSIZE_ERROR || ucs == ZSTD_CONTENTSIZE_UNKNOWN) {
    return NULL;
  }
  if (ucs == 0) {
    *out_size = 0;
    /* Allocate 1 byte so the EFILE has a valid (non-NULL) base pointer
     * we can later free; pos==end means "EOF immediately". */
    return (char*)malloc(1);
  }
  char *buf = (char*)malloc((size_t)ucs);
  if (!buf) return NULL;
  size_t n = ZSTD_decompress(buf, (size_t)ucs, src, map->data_size);
  if (ZSTD_isError(n) || n != (size_t)ucs) {
    free(buf);
    return NULL;
  }
  *out_size = (size_t)ucs;
  return buf;
}

ALWAYS_INLINE void mp_efile_init_virtual(EFILE *e, const EMAP *map) {
  size_t ucs = 0;
  char *base = mp_decompress_entry(map, &ucs);
  e->handle_type = EHANDLE_VIRTUAL;
  e->name        = mp_path_of(map);
  e->pos         = base;
  e->end         = base ? base + ucs : NULL;
  e->size        = ucs;
  e->err         = 0;
  e->f           = NULL;
  e->map         = (EMAP*)map;
  e->owned_buf   = base;     /* free in fclose/close */
#ifdef _WIN32
  e->crt_base    = base;
  e->crt_ptr     = base;
  e->crt_cnt     = (int)ucs;
#endif
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
  e->crt_cnt = (int)((const char*)e->end - e->pos);
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
  const char *dbg = getenv("MP_EMBED_DEBUG");
  if (dbg && *dbg) {
    fprintf(stderr, "[mp_embed] _get_stream_buffer_pointers: pos=%p crt_ptr=%p crt_cnt=%d\n",
            (void*)ef->pos, (void*)ef->crt_ptr, ef->crt_cnt);
  }
  /* Pull the streambuf's view back into pos first - it may have advanced
   * crt_ptr past pos via direct cursor reads. */
  ef->pos = ef->crt_ptr;
  ef->crt_cnt = (int)((const char*)ef->end - ef->pos);
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
    for (int i = 0; i < 512; ++i) {
      mp_fd2file[i].fd = -1;
      // The pointer 'f' is already NULL due to the initial zero-initialization
      // but it's good practice to be explicit if the intent isn't just zeroing.
      mp_fd2file[i].f = NULL;
    }
    mp_fd2file_initialized = true;
  }
}
#endif

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

/* Cache executable path - it doesn't change during process lifetime, but
 * the legacy code re-derived it on every fopen/stat/etc. via GetModuleFileName
 * or readlink("/proc/self/exe"). On miss-heavy workloads (most fopen calls
 * aren't for embedded data) that syscall dominates the embed lookup.
 *
 * We populate the cache on first call. Race conditions are benign: every
 * thread that loses the race writes the same content. */
static char       mp_cached_execfolder[PATH_MAX] = {0};
static char       mp_cached_executable[PATH_MAX] = {0};
static int        mp_cached_exec_state           = 0;  /* 0=unset, 1=ok, -1=fail */

static bool mp_resolve_exec_path_uncached(char *execfolder, char *executable) {
#ifdef _WIN32
  if (GetModuleFileName(NULL, executable, PATH_MAX) == 0) return false;
  if (GetModuleFileName(NULL, execfolder, PATH_MAX) == 0) return false;
  if (!PathRemoveFileSpecA(execfolder)) return false;
#elif defined(__APPLE__)
  char path[PATH_MAX];
  uint32_t bufsize = PATH_MAX;
  if (_NSGetExecutablePath(path, &bufsize) != 0) return false;
  char resolved_path[PATH_MAX];
  if (realpath(path, resolved_path) == NULL) return false;
  strncpy(executable, resolved_path, PATH_MAX);
  strncpy(execfolder, dirname(resolved_path), PATH_MAX);
#else
  char path[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len == -1) return false;
  path[len] = '\0';
  strncpy(executable, path, PATH_MAX);
  strncpy(execfolder, dirname(path), PATH_MAX);
#endif
  return true;
}

static bool get_executable_path(char *execfolder, char *executable) {
  if (likely(mp_cached_exec_state == 1)) {
    memcpy(execfolder, mp_cached_execfolder, PATH_MAX);
    memcpy(executable, mp_cached_executable, PATH_MAX);
    return true;
  }
  if (mp_cached_exec_state == -1) return false;

  /* First call - resolve and cache. */
  if (!mp_resolve_exec_path_uncached(execfolder, executable)) {
    mp_cached_exec_state = -1;
    return false;
  }
  memcpy(mp_cached_execfolder, execfolder, PATH_MAX);
  memcpy(mp_cached_executable, executable, PATH_MAX);
  mp_cached_exec_state = 1;
  return true;
}

/* find_embedded_file moved to top of file with v2 implementation. */

static void get_virtual_path(char *search_path, const char *execfolder, const char *absolute_path) {
  size_t execfolder_len = strlen(execfolder);
  size_t absolute_path_len = strlen(absolute_path);

  if (strncmp(absolute_path, execfolder, execfolder_len) == 0) {
    search_path[0] = '~';
    int pos = 0;
    for (size_t i = execfolder_len; i < absolute_path_len; i++) {
      pos = i - execfolder_len + 1;
      search_path[pos] = (absolute_path[i] == '\\') ? '/' : tolower(absolute_path[i]);
    }
    search_path[pos + 1] = '\0';
  } else {
#ifdef _WIN32
    int pos = 2;
    search_path[0] = '/';
    search_path[1] = tolower(absolute_path[0]);
    for (size_t i = 2; i < absolute_path_len; i++) {
#else
    int pos = 0;
    for (size_t i = 0; i < absolute_path_len; i++) {
#endif
      search_path[pos] = (absolute_path[i] == '\\') ? '/' : tolower(absolute_path[i]);
      pos++;
    }
    search_path[pos] = '\0';
  }
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
  char absolute_path[PATH_MAX] = {0};
  mp_get_absolute_path(input, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {0}, executable[PATH_MAX] = {0};
  if (!get_executable_path(execfolder, executable)) {
    if (out_size > 0) out[0] = '\0';
    return;
  }
  char search_path[PATH_MAX] = {0};
  get_virtual_path(search_path, execfolder, absolute_path);
  strncpy(out, search_path, out_size - 1);
  out[out_size - 1] = '\0';
}

MP_DECL(EFILE*) mp_fopen(const char* file, const char* mode) {
  const char *dbg = getenv("MP_EMBED_DEBUG");
  if (dbg && *dbg) fprintf(stderr, "[mp_embed] mp_fopen: '%s'\n", file ? file : "(null)");
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);
    if (dbg && *dbg) fprintf(stderr, "[mp_embed]   abs='%s' virt='%s'\n", absolute_path, search_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map) && map->type == ETYPE_FILE) {
      /* User-facing virtual fopen returns an EFILE wrapper - mp_fread/
       * mp_fseek/etc check the EHANDLE_VIRTUAL magic and serve from
       * embed memory directly. libstdc++.a, which doesn't go through
       * our macros, takes the __wrap_fopen path further down instead. */
      EFILE *e = (EFILE *) calloc(1, sizeof *e);
      mp_efile_init_virtual(e, map);
      return e;
    }
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
    if (!mp_fd2file_initialized) return NULL;
    for (int i = 0; i < 512; i++) {
        if (mp_fd2file[i].fd == fd) {
            EFILE *e = mp_fd2file[i].f;
            return (e && e->handle_type == EHANDLE_VIRTUAL) ? e : NULL;
        }
    }
    return NULL;
}

/* __wrap_fopen serves precompiled library callers (libstdc++.a's
 * basic_file::sys_open, third-party static libs, etc.) that don't go
 * through our header macros. They expect a real libc FILE* and will
 * call fileno() on it to get an fd, then read()/lseek()/fstat() at
 * the fd layer. So for virtual paths we create a real FILE* over a
 * fakefd, registered in mp_fd2file - the __wrap_read/lseek/fstat
 * functions look the fakefd up and serve from embed memory. */
FILE *__wrap_fopen(const char *path, const char *mode) {
    /* Path translation: same as mp_fopen, but we want our own EFILE
     * + real FILE* rather than the user-facing EFILE wrapper. */
    char absolute_path[PATH_MAX] = {0};
    mp_get_absolute_path(path, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {0}, executable[PATH_MAX] = {0};
    if (!get_executable_path(execfolder, executable))
        return __real_fopen(path, mode);
    char search_path[PATH_MAX] = {0};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map = NULL;
    if (!find_embedded_file(search_path, &map) || map->type != ETYPE_FILE)
        return __real_fopen(path, mode);

    EFILE *e = (EFILE*)calloc(1, sizeof *e);
    if (!e) return NULL;
    mp_efile_init_virtual(e, map);
    init_fd2file();
    int fakefd = open("/dev/null", O_RDONLY);
    if (fakefd < 0) { mp_efile_destroy(e); return NULL; }
    bool registered = false;
    pthread_mutex_lock(&mp_fd2file_mutex);
    for (int i = 0; i < 512; i++) {
        if (mp_fd2file[i].fd == -1) {
            mp_fd2file[i].fd = fakefd;
            mp_fd2file[i].f  = e;
            registered = true;
            break;
        }
    }
    pthread_mutex_unlock(&mp_fd2file_mutex);
    if (!registered) { __real_close(fakefd); mp_efile_destroy(e); return NULL; }
    FILE *f = fdopen(fakefd, "rb");
    if (!f) {
        pthread_mutex_lock(&mp_fd2file_mutex);
        for (int i = 0; i < 512; i++) {
            if (mp_fd2file[i].fd == fakefd) {
                mp_fd2file[i].fd = -1; mp_fd2file[i].f = NULL; break;
            }
        }
        pthread_mutex_unlock(&mp_fd2file_mutex);
        __real_close(fakefd); mp_efile_destroy(e); return NULL;
    }
    return f;
}

ssize_t __wrap_read(int fd, void *buf, size_t n) {
    EFILE *e = mp_lookup_virtual_fd(fd);
    if (!e) return __real_read(fd, buf, n);
    size_t avail = (size_t)(e->end - e->pos);
    size_t to_read = (n < avail) ? n : avail;
    if (to_read == 0) return 0;
    memcpy(buf, e->pos, to_read);
    e->pos = e->pos + to_read;
    return (ssize_t)to_read;
}

off_t __wrap_lseek(int fd, off_t offset, int whence) {
    EFILE *e = mp_lookup_virtual_fd(fd);
    if (!e) return __real_lseek(fd, offset, whence);
    const char *base = e->end - e->size;
    const char *newp;
    switch (whence) {
        case SEEK_SET: newp = base + offset; break;
        case SEEK_CUR: newp = e->pos + offset; break;
        case SEEK_END: newp = e->end + offset; break;
        default: errno = EINVAL; return (off_t)-1;
    }
    if (newp < base || newp > e->end) { errno = EINVAL; return (off_t)-1; }
    e->pos = newp;
    return (off_t)(newp - base);
}

int __wrap_fstat(int fd, struct stat *st) {
    EFILE *e = mp_lookup_virtual_fd(fd);
    if (!e) return __real_fstat(fd, st);
    memset(st, 0, sizeof *st);
    st->st_size = (off_t)e->size;
    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    return 0;
}

/* Helper: drop the registry entry for a given fakefd if it's one of
 * ours. Returns the EFILE that should be freed (or NULL). The caller
 * is responsible for the actual free outside the lock. */
static EFILE *mp_unregister_fakefd(int fd) {
    EFILE *to_free = NULL;
    pthread_mutex_lock(&mp_fd2file_mutex);
    if (mp_fd2file_initialized) {
        for (int i = 0; i < 512; i++) {
            if (mp_fd2file[i].fd == fd) {
                EFILE *e = mp_fd2file[i].f;
                if (e && e->handle_type == EHANDLE_VIRTUAL) {
                    to_free = e;
                    mp_fd2file[i].fd = -1;
                    mp_fd2file[i].f  = NULL;
                }
                break;
            }
        }
    }
    pthread_mutex_unlock(&mp_fd2file_mutex);
    return to_free;
}

int __wrap_close(int fd) {
    /* Drop our registry entry BEFORE closing the kernel fd. Otherwise
     * the kernel can recycle the fd into another thread's open() while
     * our slot still points at the (now-freed) EFILE. */
    EFILE *to_free = mp_unregister_fakefd(fd);
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
    EFILE *to_free = (fd >= 0) ? mp_unregister_fakefd(fd) : NULL;
    int r = __real_fclose(f);
    if (to_free) mp_efile_destroy(to_free);
    return r;
}
#endif

#ifdef _WIN32
MP_DECL(int) mp_open(const char *file, int flags, int mode) {
#else
MP_DECL(int) mp_open(const char *file, int flags, mode_t mode) {
#endif
  init_fd2file();
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    return open(file, flags, mode);
  }
  char search_path[PATH_MAX] = {};
  get_virtual_path(search_path, execfolder, absolute_path);

  EMAP *map;
  if (find_embedded_file(search_path, &map)) {
    EFILE *e = (EFILE*)calloc(1, sizeof *e);
    mp_efile_init_virtual(e, map);
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
      if (mp_fd2file[i].fd == -1) {
        mp_fd2file[i].fd = fakefd;
        mp_fd2file[i].f = e;
        break;
      }
    }
    return fakefd;
  }
  return open(file, flags, mode);
}

#ifdef _WIN32
MP_DECL(EFILE*) mp_wfopen(const wchar_t *wfile, const wchar_t *mode) {
  char file[PATH_MAX] = {};
  wcstombs(file, wfile, PATH_MAX);

  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map) && map->type == ETYPE_FILE) {
      EFILE *e = (EFILE *) calloc(1, sizeof *e);
      mp_efile_init_virtual(e, map);
      return e;
    }
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
  init_fd2file();
  char file[PATH_MAX] = {};
  wcstombs((char*)&file, wfile, PATH_MAX);

  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    return _wopen(wfile, flags, mode);
  }
  char search_path[PATH_MAX] = {};
  get_virtual_path(search_path, execfolder, absolute_path);

  EMAP *map;
  if (find_embedded_file(search_path, &map)) {
    EFILE* e = (EFILE*)calloc(1, sizeof *e);
    mp_efile_init_virtual(e, map);
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
      if (mp_fd2file[i].fd == -1) {
        mp_fd2file[i].fd = fakefd;
        mp_fd2file[i].f = e;
        break;
      }
    }
    return fakefd;
  }

  return _wopen(wfile, flags, mode);
}
#else
MP_DECL(int) mp_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
  init_fd2file();
  char full_virtual_path[PATH_MAX] = {};
  bool is_virtual_context = false;

  // Handle cases where pathname is relative to a virtual directory fd
  if (pathname[0] != '/') {
    for (int i = 0; i < 512; i++) {
      if (mp_fd2file[i].fd == dirfd) {
        is_virtual_context = true;
        EFILE *e = mp_fd2file[i].f;
        // We found the fd, it corresponds to a virtual file.
        if (e->map->type != ETYPE_DIRECTORY) {
          errno = ENOTDIR;
          return -1;
        }

        // It's a directory, construct the full virtual path.
        snprintf(full_virtual_path, PATH_MAX, "%s/%s", e->name, pathname);

        // The base virtual path is already normalized, but the new one might not be.
        mp_normalize_path(full_virtual_path);
        break; // Found our context, exit the loop
      }
    }
  }

  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    // If we can't get executable path, we cannot proceed with virtual file logic.
    return openat(dirfd, pathname, flags, mode);
  }

  // If we are not in a virtual context, resolve the path from CWD or absolute.
  if (!is_virtual_context && pathname[0] == '/') {
    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(pathname, absolute_path, PATH_MAX);
    get_virtual_path(full_virtual_path, execfolder, absolute_path);
  }

  EMAP *map;
  if (find_embedded_file(full_virtual_path, &map)) {
    // Virtual files are read-only.
    if ((flags & O_WRONLY) || (flags & O_RDWR)) {
      errno = EROFS;
      return -1;
    }

    // Do not allow opening a directory with open/openat unless O_DIRECTORY is specified.
    if (map->type == ETYPE_DIRECTORY && !(flags & O_DIRECTORY)) {
      errno = EISDIR;
      return -1;
    }

    if (map->type == ETYPE_FILE && (flags & O_DIRECTORY)) {
      errno = ENOTDIR;
      return -1;
    }

    EFILE *e = (EFILE*)calloc(1, sizeof *e);
    mp_efile_init_virtual(e, map);

    // Use the executable itself for the underlying native fd
    int fakefd = open(executable, O_RDONLY);
    if (fakefd == -1) {
      mp_efile_destroy(e);
      return -1; // errno will be set by open()
    }

    for (int i = 0; i < 512; i++) {
      if (mp_fd2file[i].fd == -1) {
        mp_fd2file[i].fd = fakefd;
        mp_fd2file[i].f = e;
        return fakefd;
      }
    }

    // No more file descriptors available in our virtual table
    mp_efile_destroy(e);
    close(fakefd);
    errno = EMFILE;
    return -1;
  }

  // If we determined the context was a virtual directory but didn't find the file
  if (is_virtual_context) {
    errno = ENOENT;
    return -1;
  }

  // Fallback to the real openat for all other cases (non-virtual paths)
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
    // To close a virtual file, we must find its associated native file
    // descriptor in our map and close it.
    for (int i = 0; i < 512; i++) {
      if (mp_fd2file[i].f == efile) {
        int fd_to_close = mp_fd2file[i].fd;

        // Important: Clear the map entry to prevent dangling pointers
        // and incorrect reuse of the slot.
        mp_fd2file[i].fd = -1;
        mp_fd2file[i].f = NULL;

        mp_efile_destroy(efile);

        // Finally, close the underlying native file descriptor.
        // The return value of the underlying close() is the result of this operation.
        return close(fd_to_close);
      }
    }
  }

  // Fallback for unrecognized handle types or for virtual files that
  // were somehow created without a map entry (which indicates a bug).
  // The best we can do is free the wrapper to prevent a memory leak.
  mp_efile_destroy(efile);
  return 0;
}

MP_DECL(int) mp_close(int fd) {
  init_fd2file();
  EFILE *e = NULL;
  int fd_idx = -1;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      fd_idx = i;
      break;
    }
  }
  if (e == NULL) {
    return close(fd);
  }
  if (e->handle_type != EHANDLE_VIRTUAL) {
    fclose(e->f);
    if (fd_idx != -1) {
      mp_fd2file[fd_idx].fd = -1;
      mp_fd2file[fd_idx].f = NULL;
    }
    mp_efile_destroy(e);
    return 0;
  }

  if (fd_idx != -1) {
    mp_fd2file[fd_idx].fd = -1;
    mp_fd2file[fd_idx].f = NULL;
  }
  mp_efile_destroy(e);

  // Close the underlying file descriptor that was opened on the executable
  return close(fd);
}

MP_DECL(EFILE*) mp_freopen(const char *filename, const char *mode, void *e) {
  if (MP_FOREIGN_PTR) {
    EFILE* e = (EFILE*)calloc(1, sizeof *e);
    e->handle_type = EHANDLE_NATIVE;
    e->f = freopen(filename, mode, (FILE*)e);
    return e;
  }
  if (((EFILE*)e)->handle_type == EHANDLE_NATIVE) {
    EFILE* e = (EFILE*)calloc(1, sizeof *e);
    e->handle_type = EHANDLE_NATIVE;
    e->f = freopen(filename, mode, ((EFILE*)e)->f);
    return e;
  }

  return NULL;
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

  if (((EFILE*)e)->end < ((EFILE*)e)->pos){
    errno = EINVAL;
    return true;
  }
  return (((EFILE*)e)->end == ((EFILE*)e)->pos);
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

  const char *dbg = getenv("MP_EMBED_DEBUG");
  if (dbg && *dbg) {
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

  size_t avail = (size_t)(((EFILE*)e)->end - ((EFILE*)e)->pos);
  size_t want  = size * count;
  size_t result;
  if (avail < want) {
    size_t scount = avail;
    memcpy(ptr, (void*)((EFILE*)e)->pos, scount);
    ((EFILE*)e)->pos = ((EFILE*)e)->end;
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
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
    return read(fd, buf, count);
  }

  if (e->end - e->pos < count){
    size_t scount = e->end - e->pos;
    memcpy(buf, (void*)e->pos, scount);
    e->pos = e->end;
    return (scount);
  }

  memcpy(buf, (void*)e->pos, count);
  e->pos = (char*)e->pos + count;
  return count;
}

MP_DECL(ssize_t) mp_pread(int fd, void *buf, size_t count, off_t offset) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
    return read(fd, buf, count);
  }

  if (offset >= e->size) {
    return 0; // Offset is beyond the end of the file
  }

  size_t available = e->end - e->pos;
  if (offset + count > e->size) {
    count = e->size - offset; // Adjust count to not exceed the file size
  }

  memcpy(buf, e->pos + offset, count);
  return count;
}

MP_DECL(int) mp_fgetpos(void* e, fpos_t* pos) {
  if (MP_FOREIGN_PTR) {
    return fgetpos((FILE*)e, pos);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgetpos(((EFILE*)e)->f, pos);
  }

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

  //if num 0 or 1 e->pos will not advance
  if (num <= 1) return NULL;
  if (mp_feof(e)) return NULL;

  int i = 0;

  while (1)
  {
    //i < (num - 1) so still room for two characters: \n and \0
    if ((str[i++] = *(((EFILE*)e)->pos++)) == '\n') break;
    if (mp_feof(e) || (i == (num - 1))) break;
  }
  str[i] = '\0';
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

  // Handle virtual files
  if (mp_feof(e)) {
    return -1;
  }

  // Initialize buffer if needed
  if (*lineptr == NULL || *n == 0) {
    *n = 128; // Initial buffer size
    *lineptr = (char*)malloc(*n);
    if (*lineptr == NULL) {
      return -1;
    }
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

  if(mp_feof(e))
    return -1;
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

  /* Compute the new position into a local first so we can validate
   * before mutating - leaving pos in a half-updated state on failure
   * (and then having a subsequent fread return garbage past the end)
   * was a real bug. */
  const char *base = ((EFILE*)e)->end - ((EFILE*)e)->size;
  const char *new_pos = ((EFILE*)e)->pos;
  if (origin == SEEK_SET)      new_pos = base + offset;
  else if (origin == SEEK_CUR) new_pos = ((EFILE*)e)->pos + offset;
  else if (origin == SEEK_END) new_pos = ((EFILE*)e)->end + offset;
  else { errno = EINVAL; return -1; }

  if (new_pos < base || new_pos > ((EFILE*)e)->end) {
    errno = EINVAL;
    return -1;
  }

  ((EFILE*)e)->pos = new_pos;
#ifdef _WIN32
  mp_sync_out((EFILE*)e);
#endif
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
  return 0;
}

MP_DECL(int) mp_fputc(int character, void *e) {
  if (MP_FOREIGN_PTR) {
    return fputc(character, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputc(character, ((EFILE*)e)->f);
  }
  errno = EACCES;
  return -1;
}

MP_DECL(int) mp_fputs(const char *str, void *e) {
  if (MP_FOREIGN_PTR) {
    return fputs(str, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputs(str, ((EFILE*)e)->f);
  }
  errno = EACCES;
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
  errno = EACCES;
  return -1;
}
MP_DECL(int) mp_vfprintf(void *e, const char *format, va_list args) {
  if (MP_FOREIGN_PTR) {
    return vfprintf((FILE*)e, format, args);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return vfprintf(((EFILE*)e)->f, format, args);
  }
  errno = EACCES;
  return -1;
}

MP_DECL(size_t) mp_fwrite(const void *ptr, size_t size, size_t count, void *e) {
  if (MP_FOREIGN_PTR) {
    return fwrite(ptr, size, count, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fwrite(ptr, size, count, ((EFILE*)e)->f);
  }
  errno = EACCES;
  return -1;
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
  }
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
  }
}

MP_DECL(int) mp_ferror(void *e) {
  if (MP_FOREIGN_PTR) {
    return ferror((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ferror(((EFILE*)e)->f);
  }
  return 0;
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
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].f == efile) {
      // Found the existing mapping, which is the correct behavior.
      return mp_fd2file[i].fd;
    }
  }

  // The stream was not found in the map. We will now
  // create and add a new file descriptor.

  char execfolder[PATH_MAX] = {}, executable_path[PATH_MAX];
  // We need the executable path to open a new native handle.
  if (!get_executable_path(execfolder, executable_path)) {
    // Cannot get executable path, cannot create a new fd.
    errno = EBADF;
    return -1;
  }

  // DANGER: Opening a new file descriptor here because the original was lost.
  int new_fd = open(executable_path, O_RDONLY);
  if (new_fd == -1) {
    // The open call failed, return its error.
    return -1;
  }

  // Now, find an empty slot in the map to store this new, leaked descriptor.
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == -1) {
      mp_fd2file[i].fd = new_fd;
      mp_fd2file[i].f = efile;
      // The function now returns a NEW descriptor that the calling code wasn't expecting.
      return new_fd;
    }
  }

  // If we are here, our virtual descriptor map is full.
  // We must close the descriptor we just opened and return an error.
  close(new_fd);
  errno = EMFILE; // Too many open files
  return -1;
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
#ifdef _WIN32
  mp_sync_in((EFILE*)e);
#endif
  EFILE *ef = (EFILE*)e;
  /* Move pos back one byte if possible. The C standard allows ungetting
   * a different character than what was last read; we just store the
   * caller's character at pos-1 if we can, but for our read-only blob
   * we just rewind without storing - the next read will get the original
   * bytes again, which is fine because callers normally unget the same
   * character they just read. */
  const char *base = ef->end - ef->size;
  if (ef->pos > base) {
    ef->pos--;
  } else {
    return -1;  // can't unget at start of file
  }
#ifdef _WIN32
  mp_sync_out(ef);
#endif
  return character;
}

/* Wide-char IO: needed because MSVC's STL filebuf calls fgetwc/fputwc/
 * ungetwc on the FILE* it got back from fopen/_wfopen_s. Without these
 * intercepts the type gets passed through but the underlying call goes
 * to the real CRT which segfaults on our EFILE*. */
#include <wchar.h>

MP_DECL(wint_t) mp_fgetwc(void *e) {
  if (MP_FOREIGN_PTR) return fgetwc((FILE*)e);
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) return fgetwc(((EFILE*)e)->f);
  if (mp_feof(e)) return WEOF;
  /* Virtual files are binary blobs. Returning one byte at a time gives
   * MSVC's filebuf code conversion enough to do its work for ASCII /
   * single-byte UTF-8 input. Multibyte conversion is left to upper
   * layers. */
  return (wint_t)(unsigned char)(*(((EFILE*)e)->pos++));
}

MP_DECL(wint_t) mp_fputwc(wchar_t c, void *e) {
  if (MP_FOREIGN_PTR) return fputwc(c, (FILE*)e);
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) return fputwc(c, ((EFILE*)e)->f);
  errno = EACCES;
  return WEOF;
}

MP_DECL(wint_t) mp_ungetwc(wint_t c, void *e) {
  if (MP_FOREIGN_PTR) return ungetwc(c, (FILE*)e);
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) return ungetwc(c, ((EFILE*)e)->f);
  if (((EFILE*)e)->pos > ((EFILE*)e)->end - ((EFILE*)e)->size) {
    --((EFILE*)e)->pos;
    return c;
  }
  return WEOF;
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
  init_fd2file();
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      return mp_fd2file[i].f;
    }
  }

  // If this is not a virtual fd, fallback.
  EFILE* e = (EFILE*)calloc(1, sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = fdopen(fd, mode);
  return e;
}

#ifdef _WIN32
MP_DECL(__int64) mp_lseeki64(int fd, __int64 offset, int whence) {
#else
MP_DECL(ssize_t) mp_lseek(int fd, off_t offset, int whence) {
#endif
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
#ifdef _WIN32
    return _lseeki64(fd, offset, whence);
#else
    return lseek(fd, offset, whence);
#endif
  }
  if(whence == SEEK_SET)
    e->pos = (e->end - e->size) + offset;
  else if(whence == SEEK_CUR)
    e->pos += offset;
  else if(whence == SEEK_END)
    e->pos = e->end + offset;

  if(e->end < e->pos || (e->pos - (e->end - e->size)) < 0) {
    errno = EINVAL;
    return -1;
  }
  return (e->pos - (e->end - e->size));
}

#ifdef _WIN32

MP_DECL(int) mp__stat32(const char *file, struct _stat32 *buf) {
    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat32 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat32(file, buf);
}

MP_DECL(int) mp__wstat32(const wchar_t *file, struct _stat32 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat32 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat32(file, buf);
}

MP_DECL(int) mp__stat64(const char *file, struct _stat64 *buf) {
    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat64 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat64(file, buf);
}

MP_DECL(int) mp__wstat64(const wchar_t *file, struct _stat64 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat64 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat64(file, buf);
}

MP_DECL(int) mp__stat32i64(const char *file, struct _stat32i64 *buf) {
    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat32i64 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat32i64(file, buf);
}

MP_DECL(int) mp__wstat32i64(const wchar_t *file, struct _stat32i64 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat32i64 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat32i64(file, buf);
}

MP_DECL(int) mp__stat64i32(const char *file, struct _stat64i32 *buf) {
    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat64i32 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat64i32(file, buf);
}

MP_DECL(int) mp__wstat64i32(const wchar_t *file, struct _stat64i32 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            struct _stat64i32 tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.st_mode = S_IREAD | S_IEXEC;
            switch (map->type) {
                case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
                case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
            }
            tmp.st_size = mp_uncompressed_size(map);
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat64i32(file, buf);
}

MP_STD(DWORD) mp_GetFileAttributesA(LPCSTR lpFileName) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return GetFileAttributesA(lpFileName);
    }
    return mp_GetFileAttributesW(wide_path);
}

MP_STD(DWORD) mp_GetFileAttributesW(LPCWSTR lpFileName) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            DWORD attributes = FILE_ATTRIBUTE_READONLY;
            if (map->type == ETYPE_DIRECTORY) {
                attributes |= FILE_ATTRIBUTE_DIRECTORY;
            }
            return attributes;
        }
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

    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
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

            // Timestamps are zeroed by memset.
            return TRUE;
        }
    }
    return GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

// A map to associate native HANDLEs with our virtual EFILE structs.
struct HNDMAP_S {   // Virtual File Handle
    HANDLE h;
    EFILE *f;
};
typedef struct HNDMAP_S HNDMAP;

// An array to store the mappings for open virtual files.
HNDMAP mp_handle2file[512] = {};


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
            // This is a handle to one of our virtual file mappings.
            // Since the "handle" is just a pointer to an EFILE struct
            // and doesn't correspond to a real system object that needs closing,
            // we just clear our tracking entry. The actual EFILE struct
            // will be freed when the original file handle is closed.
            mp_mapping2file[i].h = NULL;
            mp_mapping2file[i].f = NULL;
            return TRUE; // Indicate success.
        }
    }

    // Check if it's a handle to a virtual file created by CreateFile.
    for (int i = 0; i < 512; i++) {
        if (mp_handle2file[i].h == hObject) {
            // This is a handle to one of our virtual files.
            // Free the memory for the EFILE struct.
            free(mp_handle2file[i].f);

            // Clear the entry in our map.
            mp_handle2file[i].h = NULL;
            mp_handle2file[i].f = NULL;

            // Now, close the underlying native handle we created.
            return CloseHandle(hObject);
        }
    }

    // If it's not a handle we're managing, pass it to the real API.
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
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, sizeof(file_mb));

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
    if (!get_executable_path(execfolder, executable)) {
        return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    }

    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
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

        mp_efile_init_virtual(e, map);

        // Create a real, underlying handle by opening the executable itself.
        // This gives us a valid system handle to return to the caller.
        HANDLE hFake = CreateFileA(executable, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFake == INVALID_HANDLE_VALUE) {
            mp_efile_destroy(e);
            return INVALID_HANDLE_VALUE; // Let caller get the error from CreateFileA.
        }

        // Find an empty slot in our handle map to store the association.
        for (int i = 0; i < 512; i++) {
            if (mp_handle2file[i].h == NULL) {
                mp_handle2file[i].h = hFake;
                mp_handle2file[i].f = e;
                return hFake; // Return the fake handle to the caller.
            }
        }

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
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, sizeof(file_mb));

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
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
        lpFileInformation->nFileIndexHigh = (DWORD)(fileIndex >> 32);
        lpFileInformation->nFileIndexLow = (DWORD)(fileIndex & 0xFFFFFFFF);

        // Timestamps and volume serial are zeroed by memset.
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
                // Timestamps are zeroed by memset.
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

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (e != NULL && get_executable_path(execfolder, executable)) {
        char resulting_path[PATH_MAX] = {};
        if (e->name[0] == '~') {
            // This is a relative path.
            size_t execfolder_len = strlen(execfolder);
            strcpy(&resulting_path[0], execfolder);
            size_t curr_res_pos = execfolder_len;
            for (size_t i = 1; i < PATH_MAX && curr_res_pos < PATH_MAX - 1; i++) {
                char curr = e->name[i];
                if (curr == '\0')
                    break;
                if (curr == '/')
                    curr = '\\';

                resulting_path[curr_res_pos] = curr;
                curr_res_pos++;
            }
            resulting_path[PATH_MAX - 1] = '\0';
        } else {
            strcpy(&resulting_path[0], e->name);
        }

        // It's a virtual file. The name is the final path.
        size_t required_chars = mbstowcs(NULL, resulting_path, 0);
        if (required_chars == (size_t)-1) {
            SetLastError(ERROR_INVALID_DATA);
            return 0;
        }

        if (cchFilePath < required_chars + 1) {
            // Buffer is too small. Return the required size.
            return required_chars + 1;
        }

        size_t converted_chars = mbstowcs(lpszFilePath, resulting_path, cchFilePath);
        if (converted_chars == (size_t)-1) {
            SetLastError(ERROR_INVALID_DATA);
            return 0;
        }

        return converted_chars;
    }

    // Not a virtual file, fall back to the real API.
    return GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

/**
 * @brief Retrieves the full path and file name of the specified file.
 * @return The length of the string copied to lpBuffer, or the required buffer size.
 *
 * For virtual files, this returns the calculated absolute path.
 */
MP_STD(DWORD) mp_GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            // It's a virtual file. Return the absolute path we constructed.
            size_t required_chars = mbstowcs(NULL, absolute_path, 0) + 1;
            if (required_chars == 0) {
                SetLastError(ERROR_INVALID_DATA);
                return 0;
            }

            if (nBufferLength < required_chars) {
                return required_chars; // Return required buffer size
            }

            mbstowcs(lpBuffer, absolute_path, nBufferLength);

            if (lpFilePart) {
                *lpFilePart = PathFindFileNameW(lpBuffer);
            }
            return wcslen(lpBuffer);
        }
    }

    // Not a virtual file, fall back to the real API.
    return GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

/**
 * @brief Retrieves the volume mount point where the specified path is located.
 * @return Nonzero on success, zero on failure.
 *
 * For virtual files, this retrieves the volume of the executable itself.
 */
MP_STD(BOOL) mp_GetVolumePathNameW(LPCWSTR lpszFileName, LPWSTR lpszVolumePathName, DWORD cchBufferLength) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpszFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(file_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            // It's a virtual file. The volume is the volume of the executable.
            wchar_t exec_path_w[PATH_MAX];
            mbstowcs(exec_path_w, executable, PATH_MAX);

            return GetVolumePathNameW(exec_path_w, lpszVolumePathName, cchBufferLength);
        }
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
    char dir_mb[PATH_MAX];
    if (lpDirectoryName != NULL) {
        wcstombs(dir_mb, lpDirectoryName, PATH_MAX);
    } else {
        GetCurrentDirectoryA(PATH_MAX, dir_mb);
    }

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(dir_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map) && map->type == ETYPE_DIRECTORY) {
            // It's a virtual directory. The disk space is the one for the executable's drive.
            return GetDiskFreeSpaceExA(execfolder, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
        }
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

// Helper to populate WIN32_FIND_DATAW from an EMAP entry for FindFirst/Next
static void populate_find_data(EMAP* map, WIN32_FIND_DATAW* find_data) {
    memset(find_data, 0, sizeof(WIN32_FIND_DATAW));

    // Convert filename to wide char
    const char* filename = PathFindFileNameA(mp_path_of(map));
    mbstowcs(find_data->cFileName, filename, _countof(find_data->cFileName));
    // Ensure null-termination for safety
    find_data->cFileName[_countof(find_data->cFileName) - 1] = L'\0';

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
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, sizeof(file_mb));

    char search_dir[PATH_MAX];
    strcpy(search_dir, file_mb);
    PathRemoveFileSpecA(search_dir);
    char* search_pattern = PathFindFileNameA(file_mb);

    char absolute_path[PATH_MAX] = {};
    mp_get_absolute_path(search_dir, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
    if (!get_executable_path(execfolder, executable)) {
        return FindFirstFileW(lpFileName, lpFindFileData);
    }

    char virtual_dir_path[PATH_MAX] = {};
    get_virtual_path(virtual_dir_path, execfolder, absolute_path);

    EMAP *dir_map;
    if (find_embedded_file(virtual_dir_path, &dir_map) && dir_map->type == ETYPE_DIRECTORY) {
        /* Walk children directly via the precomputed children index. */
        for (uint32_t ci = 0; ci < dir_map->children_count; ++ci) {
            const EMAP* child = mp_embed_child_at(dir_map, ci);
            const char* filename_part = PathFindFileNameA(mp_path_of(child));
            if (PathMatchSpecA(filename_part, search_pattern)) {
                VIRTUAL_FIND_HANDLE_DATA* find_handle_data = (VIRTUAL_FIND_HANDLE_DATA*)malloc(sizeof(VIRTUAL_FIND_HANDLE_DATA));
                if (!find_handle_data) {
                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                    return INVALID_HANDLE_VALUE;
                }
                find_handle_data->dir_map = dir_map;
                find_handle_data->next_child = ci + 1;
                strcpy(find_handle_data->search_pattern, search_pattern);

                for (int i = 0; i < 512; i++) {
                    if (mp_findhandles[i].h == NULL) {
                        mp_findhandles[i].h = (HANDLE)find_handle_data;
                        mp_findhandles[i].data = find_handle_data;
                        populate_find_data((EMAP*)child, lpFindFileData);
                        return mp_findhandles[i].h;
                    }
                }
                free(find_handle_data);
                SetLastError(ERROR_TOO_MANY_OPEN_FILES);
                return INVALID_HANDLE_VALUE;
            }
        }

        SetLastError(ERROR_FILE_NOT_FOUND);
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

    const EMAP *dir_map = find_handle_data->dir_map;
    while (find_handle_data->next_child < dir_map->children_count) {
        const EMAP* child = mp_embed_child_at(dir_map, find_handle_data->next_child++);
        const char* filename_part = PathFindFileNameA(mp_path_of(child));
        if (PathMatchSpecA(filename_part, find_handle_data->search_pattern)) {
            populate_find_data((EMAP*)child, lpFindFileData);
            return TRUE;
        }
    }

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
            free(mp_findhandles[i].data);
            mp_findhandles[i].h = NULL;
            mp_findhandles[i].data = NULL;
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

        // The size of the mapping must be within the file's bounds.
        ULONGLONG maxSize = ((ULONGLONG)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
        if (maxSize > e->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }

        // We can't create a real mapping object for in-memory data.
        // Instead, we'll use a pointer to our EFILE struct as a "handle".
        // This is a bit of a hack, but it allows us to identify the mapping later.
        // Find an empty slot in our mapping table.
        for (int i = 0; i < 512; i++) {
            if (mp_mapping2file[i].h == NULL) {
                // We use the EFILE pointer itself as the handle.
                mp_mapping2file[i].h = (HANDLE)e;
                mp_mapping2file[i].f = e;
                return (HANDLE)e;
            }
        }
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
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        // Check if the mapping object is one of our virtual ones.
        if (mp_mapping2file[i].h == hFileMappingObject) {
            e = mp_mapping2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual mapping. The "view" is just a pointer into our data block.
        // Check for write access, which is not allowed.
        if (dwDesiredAccess & (FILE_MAP_WRITE | FILE_MAP_ALL_ACCESS)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return NULL;
        }

        ULONGLONG offset = ((ULONGLONG)dwFileOffsetHigh << 32) | dwFileOffsetLow;
        if (offset >= e->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }

        LPVOID view = (LPVOID)(e->pos + offset);

        // Store the view so we can recognize it in UnmapViewOfFile
        for (int i = 0; i < 512; i++) {
            if (mp_view2file[i].view == NULL) {
                mp_view2file[i].view = view;
                mp_view2file[i].f = e;
                return view;
            }
        }
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
            // This is a view of one of our virtual files.
            // Since we didn't allocate memory with MapViewOfFile,
            // we don't need to free it here. We just clear our tracking entry.
            mp_view2file[i].view = NULL;
            mp_view2file[i].f = NULL;
            return TRUE;
        }
    }

    // Not a virtual view, fall back to the real API.
    return UnmapViewOfFile(lpBaseAddress);
}
#else
MP_DECL(int) mp_stat(const char *file, struct stat *buf) {
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      struct stat tmp;
      memset(&tmp, 0, sizeof(tmp));
      tmp.st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      switch (map->type) {
        case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
        case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
      }
      tmp.st_size = mp_uncompressed_size(map);
      memcpy(buf, &tmp, sizeof(tmp));
      return 0;
    }
  }
  return stat(file, buf);
}

MP_DECL(int) mp_lstat(const char *file, struct stat *buf) {
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      struct stat tmp;
      memset(&tmp, 0, sizeof(tmp));
      tmp.st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      switch (map->type) {
        case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
        case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
      }
      tmp.st_size = mp_uncompressed_size(map);
      memcpy(buf, &tmp, sizeof(tmp));
      return 0;
    }
  }
  return lstat(file, buf);
}

MP_DECL(int) mp_fstat(int fd, struct stat *buf) {
  init_fd2file();
  // Check if the file descriptor is for a virtual file.
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      EFILE *e = mp_fd2file[i].f;
      EMAP *map = e->map;

      // This is a virtual file, populate the stat buffer from the map.
      struct stat tmp;
      memset(&tmp, 0, sizeof(tmp));
      // Set standard read/execute permissions for user, group, and other.
      tmp.st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      switch (map->type) {
        case ETYPE_FILE:
          tmp.st_mode |= S_IFREG; // Regular file
          break;
        case ETYPE_DIRECTORY:
          tmp.st_mode |= S_IFDIR; // Directory
          break;
      }
      tmp.st_size = mp_uncompressed_size(map); // Set the file size.
      // Other fields like timestamps, user/group ID etc., remain 0.

      memcpy(buf, &tmp, sizeof(tmp));
      return 0; // Success
    }
  }

  // If the fd is not in our virtual file table, fall back to the native call.
  return fstat(fd, buf);
}

MP_DECL(int) mp_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
  init_fd2file();
  // Handle cases where pathname is relative to a virtual directory fd
  if (pathname[0] != '/') {
    for (int i = 0; i < 512; i++) {
      if (mp_fd2file[i].fd == dirfd) {
        EFILE *e = mp_fd2file[i].f;
        // We found the fd, it corresponds to a virtual file.
        if (e->map->type != ETYPE_DIRECTORY) {
          errno = ENOTDIR;
          return -1;
        }

        // It's a directory, construct the full virtual path.
        char full_virtual_path[PATH_MAX];
        snprintf(full_virtual_path, PATH_MAX, "%s/%s", e->name, pathname);

        // The virtual path is already normalized, but the new one might not be (e.g., ../)
        char temp_path_for_norm[PATH_MAX];
        strcpy(temp_path_for_norm, full_virtual_path);
        mp_normalize_path(temp_path_for_norm);

        EMAP *map;
        if (find_embedded_file(temp_path_for_norm, &map)) {
          struct stat tmp;
          memset(&tmp, 0, sizeof(tmp));
          tmp.st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
          switch (map->type) {
            case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
            case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
          }
          tmp.st_size = mp_uncompressed_size(map);
          memcpy(buf, &tmp, sizeof(tmp));
          return 0;
        } else {
          errno = ENOENT;
          return -1;
        }
      }
    }
  }

  // Fallback for absolute paths, AT_FDCWD, or native file descriptors
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(pathname, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      // The virtual file system does not support symlinks, so AT_SYMLINK_NOFOLLOW
      // is effectively ignored as the behavior is the same as lstat/stat.
      struct stat tmp;
      memset(&tmp, 0, sizeof(tmp));
      tmp.st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      switch (map->type) {
        case ETYPE_FILE: tmp.st_mode |= S_IFREG; break;
        case ETYPE_DIRECTORY: tmp.st_mode |= S_IFDIR; break;
      }
      tmp.st_size = mp_uncompressed_size(map);
      memcpy(buf, &tmp, sizeof(tmp));
      return 0;
    }
  }

  // Fallback to the real fstatat for non-virtual files.
  return fstatat(dirfd, pathname, buf, flags);
}

MP_DECL(int) mp_access(const char *pathname, int mode) {
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(pathname, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      if (mode == F_OK) return 0; // Existence check passes
      if ((mode & W_OK)) { // Write check fails
        errno = EROFS;
        return -1;
      }
      if ((mode & R_OK) || (mode & X_OK)) { // Read/Exec check passes
        return 0;
      }
    }
  }
  return access(pathname, mode);
}

MP_DECL(int) mp_faccessat(int dirfd, const char *pathname, int mode, int flags) {
  // This implementation ignores dirfd and flags for virtual files.
  return mp_access(pathname, mode);
}

MP_DECL(int) mp_statvfs(const char *path, struct statvfs *buf) {
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(path, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      // Virtual file system lives on the same FS as the executable
      return statvfs(execfolder, buf);
    }
  }
  return statvfs(path, buf);
}

MP_DECL(int) mp_fstatvfs(int fd, struct statvfs *buf) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
      return statvfs(execfolder, buf);
    }
  }
  return fstatvfs(fd, buf);
}

MP_DECL(ssize_t) mp_pwrite(int fd, const void *buf, size_t count, off_t offset) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    errno = EROFS; // Read-only file system
    return -1;
  }
  return pwrite(fd, buf, count, offset);
}

MP_DECL(ssize_t) mp_readv(int fd, const struct iovec *iov, int iovcnt) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
    return readv(fd, iov, iovcnt);
  }

  ssize_t total_read = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t bytes_to_read = iov[i].iov_len;
    ssize_t remaining_in_file = e->end - e->pos;
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
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
    return preadv(fd, iov, iovcnt, offset);
  }

  if (offset >= e->size) return 0;

  const char* start_pos = (e->end - e->size) + offset;
  ssize_t total_read = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t bytes_to_read = iov[i].iov_len;
    ssize_t remaining_in_file = e->end - start_pos;
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
  // Ignoring flags for virtual files
  return mp_preadv(fd, iov, iovcnt, offset);
}
#endif

MP_DECL(ssize_t) mp_writev(int fd, const struct iovec *iov, int iovcnt)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
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
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    errno = EROFS; // Read-only
    return -1;
  }
  return pwritev(fd, iov, iovcnt, offset);
}

#ifdef __linux
MP_DECL(ssize_t) mp_pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  // Ignoring flags for virtual files
  return mp_pwritev(fd, iov, iovcnt, offset);
}
#endif

typedef struct VDIR_S {
  const EMAP* dir_map;       // The directory entry being iterated
  uint32_t next_child;       // Index into dir_map->children of next entry to return
  struct dirent entry;       // The current entry to be returned
} VDIR;

// A map to associate DIR* pointers with our VDIR structs
VDIR* mp_open_vdirs[512] = {};

MP_DECL(DIR*) mp_opendir(const char* name) {
  char absolute_path[PATH_MAX] = {};
  mp_get_absolute_path(name, absolute_path, PATH_MAX);

  char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
  if (!get_executable_path(execfolder, executable)) {
    return opendir(name);
  }

  char virtual_dir_path[PATH_MAX] = {};
  get_virtual_path(virtual_dir_path, execfolder, absolute_path);

  EMAP *dir_map;
  if (find_embedded_file(virtual_dir_path, &dir_map) && dir_map->type == ETYPE_DIRECTORY) {
    VDIR* vdir = (VDIR*)malloc(sizeof(VDIR));
    if (!vdir) {
      errno = ENOMEM;
      return NULL;
    }
    vdir->dir_map = dir_map;
    vdir->next_child = 0;

    for (int i = 0; i < 512; i++) {
      if (mp_open_vdirs[i] == NULL) {
        mp_open_vdirs[i] = vdir;
        return (DIR*)vdir;
      }
    }
    free(vdir);
    errno = EMFILE; // Too many open files
    return NULL;
  }
  return opendir(name);
}

MP_DECL(DIR*) mp_fdopendir(int fd) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_fd2file[i].fd == fd) {
      e = mp_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    // We have a virtual file descriptor. We can open it if it's a directory.
    return mp_opendir(e->name);
  }
  return fdopendir(fd);
}

MP_DECL(struct dirent*) mp_readdir(DIR *dirp) {
  VDIR* vdir = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_open_vdirs[i] == (VDIR*)dirp) {
      vdir = (VDIR*)dirp;
      break;
    }
  }
  if (vdir == NULL) {
    return readdir(dirp);
  }

  if (vdir->next_child >= vdir->dir_map->children_count) {
    return NULL; // No more entries
  }
  const EMAP* current_map = mp_embed_child_at(vdir->dir_map, vdir->next_child++);
  const char* full_path = mp_path_of(current_map);

  // Extract just the filename component (last segment after final '/').
  const char* filename_part = strrchr(full_path, '/');
  filename_part = filename_part ? filename_part + 1 : full_path;

  strncpy(vdir->entry.d_name, filename_part, sizeof(vdir->entry.d_name));
  vdir->entry.d_name[sizeof(vdir->entry.d_name) - 1] = '\0';
  vdir->entry.d_ino = (ino_t)(uintptr_t)current_map; // unique-enough fake inode
  vdir->entry.d_type = (current_map->type == ETYPE_DIRECTORY) ? DT_DIR : DT_REG;
  return &vdir->entry;
}

MP_DECL(int) mp_closedir(DIR *dirp) {
  for (int i = 0; i < 512; i++) {
    if (mp_open_vdirs[i] == (VDIR*)dirp) {
      free(mp_open_vdirs[i]);
      mp_open_vdirs[i] = NULL;
      return 0;
    }
  }
  return closedir(dirp);
}

MP_DECL(void) mp_rewinddir(DIR *dirp) {
  VDIR* vdir = NULL;
  for (int i = 0; i < 512; i++) {
    if (mp_open_vdirs[i] == (VDIR*)dirp) {
      vdir = (VDIR*)dirp;
      break;
    }
  }
  if (vdir != NULL) {
    vdir->next_child = 0;
  } else {
    rewinddir(dirp);
  }
}

#endif
