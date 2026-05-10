/* mp_embed C test suite.
 *
 * Covers virtual-file open/read/seek/stat/dirent on every supported
 * platform plus Win32-specific paths (FindFirstFileW, fopen_s, _wfopen_s).
 * Final section runs concurrent multi-threaded reads to verify the
 * runtime is reentrant for read-only virtual handles.
 *
 * IMPORTANT: <mp_embed.h> must be included before any standard headers
 * for the macro-based stdio interception to take effect. The build hook
 * normally prepends it to every TU.
 */

#include <mp_embed.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#  define VPATH_PREFIX "C:\\embedtest"
#  define VPATH_SEP "\\"
#else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <pthread.h>
#  define VPATH_PREFIX "/c/embedtest"
#  define VPATH_SEP "/"
#endif

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, fmt, ...) do {                                  \
    if (cond) { ++g_pass; }                                         \
    else {                                                          \
        ++g_fail;                                                   \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n",                    \
                __FILE__, __LINE__, ##__VA_ARGS__);                 \
    }                                                               \
} while (0)

/* Helper: run an fopen + fread + fclose round-trip. */
static int read_full(const char *path, char *buf, size_t cap, size_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "  fopen(%s) -> NULL (errno=%d)\n", path, errno);
        return 0;
    }
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    if (n_out) *n_out = n;
    return 1;
}

/* ============================================================
 * Test groups
 * ============================================================ */

static void test_basic_fopen(void) {
    fprintf(stderr, "[basic_fopen]\n");
    char buf[64] = {0};
    size_t n = 0;
    CHECK(read_full(VPATH_PREFIX VPATH_SEP "small.txt", buf, sizeof buf - 1, &n),
          "read small.txt failed");
    buf[n] = 0;
    CHECK(strcmp(buf, "Hello, World!\n") == 0,
          "small.txt content wrong: '%s' (n=%zu)", buf, n);
}

static void test_empty_file(void) {
    fprintf(stderr, "[empty_file]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "empty.txt", "rb");
    CHECK(f != NULL, "fopen empty.txt: NULL (errno=%d)", errno);
    if (!f) return;
    char buf[16];
    size_t n = fread(buf, 1, 16, f);
    CHECK(n == 0, "empty.txt fread n=%zu (want 0)", n);
    CHECK(feof(f), "empty.txt feof should be true");
    fclose(f);
}

static void test_one_byte_file(void) {
    fprintf(stderr, "[one_byte_file]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "one.bin", "rb");
    CHECK(f != NULL, "fopen one.bin: NULL");
    if (!f) return;
    int c = fgetc(f);
    CHECK(c == 0x42, "one.bin first byte = 0x%02x (want 0x42)", c);
    int c2 = fgetc(f);
    CHECK(c2 == EOF, "one.bin second read should be EOF, got 0x%02x", c2);
    fclose(f);
}

static void test_binary_all_bytes(void) {
    fprintf(stderr, "[binary_all_bytes]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
    CHECK(f != NULL, "fopen binary.bin: NULL");
    if (!f) return;
    unsigned char buf[256];
    size_t n = fread(buf, 1, 256, f);
    CHECK(n == 256, "binary.bin: read %zu (want 256)", n);
    int ok = 1;
    for (int i = 0; i < 256; ++i) if (buf[i] != (unsigned char)i) { ok = 0; break; }
    CHECK(ok, "binary.bin: byte mismatch");
    fclose(f);
}

static void test_seek_tell(void) {
    fprintf(stderr, "[seek_tell]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
    if (!f) { CHECK(0, "open binary.bin"); return; }

    CHECK(fseek(f, 0, SEEK_END) == 0, "seek end failed");
    long sz = ftell(f);
    CHECK(sz == 256, "ftell at end = %ld (want 256)", sz);

    CHECK(fseek(f, 100, SEEK_SET) == 0, "seek 100 failed");
    int c = fgetc(f);
    CHECK(c == 100, "byte at offset 100 = %d (want 100)", c);

    CHECK(fseek(f, -50, SEEK_CUR) == 0, "seek -50 from cur failed");
    long cur = ftell(f);
    CHECK(cur == 51, "ftell after relative seek = %ld (want 51)", cur);

    CHECK(fseek(f, -1, SEEK_END) == 0, "seek -1 from end failed");
    c = fgetc(f);
    CHECK(c == 255, "last byte = %d (want 255)", c);

    fclose(f);
}

static void test_repeated_open(void) {
    fprintf(stderr, "[repeated_open]\n");
    /* Many rapid open/close cycles - exercises malloc/free of EFILE. */
    for (int i = 0; i < 1000; ++i) {
        FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
        if (!f) { CHECK(0, "iter %d: open failed", i); return; }
        char ch;
        size_t n = fread(&ch, 1, 1, f);
        if (n != 1 || ch != 'H') { CHECK(0, "iter %d: bad first byte", i); fclose(f); return; }
        fclose(f);
    }
    CHECK(1, "1000 open/close cycles");
}

static void test_concurrent_handles(void) {
    fprintf(stderr, "[concurrent_handles]\n");
    /* 50 file handles to the same virtual file at once. */
    FILE *fps[50] = {0};
    for (int i = 0; i < 50; ++i) {
        fps[i] = fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
        if (!fps[i]) { CHECK(0, "open %d failed", i); return; }
        if (fseek(fps[i], i, SEEK_SET) != 0) { CHECK(0, "seek %d failed", i); return; }
    }
    /* Each fp should be at its own position. */
    int ok = 1;
    for (int i = 0; i < 50; ++i) {
        int c = fgetc(fps[i]);
        if (c != i) { ok = 0; fprintf(stderr, "  fp[%d] byte=%d (want %d)\n", i, c, i); }
    }
    for (int i = 0; i < 50; ++i) fclose(fps[i]);
    CHECK(ok, "concurrent handles independent positions");
}

static void test_out_of_bounds(void) {
    fprintf(stderr, "[out_of_bounds]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    if (!f) { CHECK(0, "open"); return; }

    /* Read way more than file size. */
    char buf[1024] = {0};
    size_t n = fread(buf, 1, 1024, f);
    CHECK(n < 1024, "fread should return < 1024 (got %zu)", n);
    CHECK(n > 0,    "fread should return > 0 (got %zu)", n);

    /* Subsequent read returns 0. */
    n = fread(buf, 1, 1024, f);
    CHECK(n == 0, "second fread past EOF n=%zu (want 0)", n);

    /* Seek past end: mp_embed rejects this for read-only virtual files
     * (errno=EINVAL) and leaves the position unchanged. After a failed
     * seek a subsequent fread should still see EOF since we were already
     * at end. */
    long pre_pos = ftell(f);
    int seek_rc = fseek(f, 9999, SEEK_SET);
    CHECK(seek_rc != 0, "seek past end on virtual file should fail");
    CHECK(ftell(f) == pre_pos, "failed seek must not change position");
    n = fread(buf, 1, 8, f);
    CHECK(n == 0, "fread after failed seek n=%zu (want 0)", n);

    fclose(f);
}

static void test_zero_count(void) {
    fprintf(stderr, "[zero_count]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    if (!f) { CHECK(0, "open"); return; }
    char buf[16];
    /* size=0 is well-defined as "read nothing", returns 0. */
    size_t n = fread(buf, 0, 10, f);
    CHECK(n == 0, "fread size=0 n=%zu", n);
    n = fread(buf, 10, 0, f);
    CHECK(n == 0, "fread count=0 n=%zu", n);
    fclose(f);
}

static void test_uppercase_path(void) {
    fprintf(stderr, "[uppercase_path]\n");
    /* Path translation should fold case, so this path with a different
     * case still hits the same entry. */
    char buf[64] = {0};
    size_t n = 0;
    CHECK(read_full(VPATH_PREFIX VPATH_SEP "uppercase.txt", buf, sizeof buf - 1, &n),
          "read uppercase.txt failed");
    buf[n] = 0;
    CHECK(strcmp(buf, "case folding test\n") == 0,
          "uppercase content: '%s'", buf);
}

static void test_path_with_spaces(void) {
    fprintf(stderr, "[path_with_spaces]\n");
    char buf[64] = {0};
    size_t n = 0;
    CHECK(read_full(VPATH_PREFIX VPATH_SEP "name with spaces.txt", buf, sizeof buf - 1, &n),
          "read 'name with spaces.txt' failed");
    buf[n] = 0;
    CHECK(strcmp(buf, "spaces work\n") == 0, "spaces content: '%s'", buf);
}

static void test_dotted_name(void) {
    fprintf(stderr, "[dotted_name]\n");
    char buf[16] = {0};
    size_t n = 0;
    CHECK(read_full(VPATH_PREFIX VPATH_SEP "multiple.dots.in.name", buf, sizeof buf - 1, &n),
          "read multiple.dots.in.name failed");
    buf[n] = 0;
    CHECK(strcmp(buf, "dots\n") == 0, "dots content: '%s'", buf);
}

static void test_deep_nesting(void) {
    fprintf(stderr, "[deep_nesting]\n");
    char buf[32] = {0};
    size_t n = 0;
    CHECK(read_full(VPATH_PREFIX VPATH_SEP "deep" VPATH_SEP "nested" VPATH_SEP "tree" VPATH_SEP "leaf.txt",
                    buf, sizeof buf - 1, &n),
          "read deep leaf failed");
    buf[n] = 0;
    CHECK(strcmp(buf, "found it\n") == 0, "deep leaf content: '%s'", buf);
}

static void test_missing_file(void) {
    fprintf(stderr, "[missing_file]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "definitely-not-there.bin", "rb");
    CHECK(f == NULL, "open of non-existent file should return NULL");
    if (f) fclose(f);
}

static void test_huge_file(void) {
    fprintf(stderr, "[huge_file]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "huge.dat", "rb");
    if (!f) { CHECK(0, "open huge.dat"); return; }

    /* Read in chunks of various sizes. */
    size_t total = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        total += n;
    }
    CHECK(total == 1024 * 1024, "huge.dat total bytes = %zu (want 1MB)", total);

    /* fseek to start then partial read at random offset. */
    CHECK(fseek(f, 524288, SEEK_SET) == 0, "seek to mid");
    n = fread(chunk, 1, 100, f);
    CHECK(n == 100, "mid-file read n=%zu", n);

    fclose(f);
}

/* Verifies the zstd round-trip on a file with a high compression ratio.
 * compressible.dat is ~256 KB of repeating text - the embed blob stores
 * it in well under 1 KB. This exercises decompression of a file whose
 * uncompressed size is meaningfully larger than the compressed frame. */
static void test_compressible_file(void) {
    fprintf(stderr, "[compressible_file]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "compressible.dat", "rb");
    if (!f) { CHECK(0, "open compressible.dat"); return; }
    const char *pattern = "the quick brown fox jumps over the lazy dog. ";
    size_t plen = strlen(pattern);
    size_t expected = plen * 6000;
    char *buf = (char*)malloc(expected + 1);
    size_t n = fread(buf, 1, expected, f);
    CHECK(n == expected, "compressible.dat total bytes = %zu (want %zu)", n, expected);
    int ok = 1;
    for (size_t i = 0; i + plen <= n; i += plen) {
        if (memcmp(buf + i, pattern, plen) != 0) { ok = 0; break; }
    }
    CHECK(ok, "compressible.dat content matches repeated pattern");
    free(buf);
    fclose(f);
}

static void test_fgets(void) {
    fprintf(stderr, "[fgets]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    if (!f) { CHECK(0, "open"); return; }
    char buf[64] = {0};
    char *r = fgets(buf, sizeof buf, f);
    CHECK(r == buf, "fgets returned %p", (void*)r);
    CHECK(strcmp(buf, "Hello, World!\n") == 0, "fgets content '%s'", buf);
    /* Past-EOF fgets returns NULL. */
    r = fgets(buf, sizeof buf, f);
    CHECK(r == NULL, "fgets past EOF should be NULL");
    fclose(f);
}

static void test_mixed_real_and_virtual(void) {
    fprintf(stderr, "[mixed_real_and_virtual]\n");
    /* Open a real file (the running exe) and a virtual file simultaneously.
     * Make sure we don't confuse the two. */
    FILE *real = fopen(
#ifdef _WIN32
        "C:\\Windows\\System32\\drivers\\etc\\hosts",
#else
        "/etc/hosts",
#endif
        "rb");

    FILE *virt = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");

    /* The real file may not exist on this machine; we just want
     * mp_embed not to crash with a null pointer. */
    if (real) {
        char b[8];
        fread(b, 1, sizeof b, real);
        fclose(real);
    }
    CHECK(virt != NULL, "virtual open in mixed test");
    if (virt) {
        char b[16] = {0};
        size_t n = fread(b, 1, 14, virt);
        b[n] = 0;
        CHECK(strcmp(b, "Hello, World!\n") == 0, "virt content under mix '%s'", b);
        fclose(virt);
    }
}

#ifdef _WIN32
static void test_findfirst(void) {
    fprintf(stderr, "[FindFirstFileW]\n");
    /* List manyfiles/ contents. */
    wchar_t pattern[] = L"C:\\embedtest\\manyfiles\\*";
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) {
        CHECK(0, "FindFirstFileW returned INVALID_HANDLE_VALUE");
        return;
    }
    int count = 0;
    do {
        ++count;
    } while (FindNextFileW(h, &data));
    FindClose(h);
    CHECK(count == 50, "FindFirstFileW found %d entries (want 50)", count);
}
#else
static void test_findfirst(void) { (void)0; }

static void test_opendir(void) {
    fprintf(stderr, "[opendir]\n");
    DIR *d = opendir(VPATH_PREFIX VPATH_SEP "manyfiles");
    if (!d) { CHECK(0, "opendir manyfiles"); return; }
    int count = 0;
    while (readdir(d)) ++count;
    closedir(d);
    CHECK(count == 50, "opendir/readdir manyfiles got %d (want 50)", count);
}
#endif

static void test_stat(void) {
    fprintf(stderr, "[stat]\n");
    struct stat st;
    int rc = stat(VPATH_PREFIX VPATH_SEP "binary.bin", &st);
    CHECK(rc == 0, "stat binary.bin rc=%d errno=%d", rc, errno);
    CHECK(st.st_size == 256, "stat binary.bin size=%lld", (long long)st.st_size);

    rc = stat(VPATH_PREFIX VPATH_SEP "deep", &st);
    CHECK(rc == 0, "stat deep dir rc=%d", rc);

    rc = stat(VPATH_PREFIX VPATH_SEP "no-such-thing.bin", &st);
    CHECK(rc != 0, "stat non-existent should fail");
}

#ifdef _WIN32
static void test_fopen_s(void) {
    fprintf(stderr, "[fopen_s]\n");
    FILE *f = NULL;
    int rc = fopen_s(&f, VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    CHECK(rc == 0, "fopen_s rc=%d", rc);
    CHECK(f != NULL, "fopen_s f=NULL");
    if (f) {
        char b[16] = {0};
        size_t n = fread(b, 1, 14, f);
        b[n] = 0;
        CHECK(strcmp(b, "Hello, World!\n") == 0, "fopen_s content '%s'", b);
        fclose(f);
    }
    /* Invalid args. */
    rc = fopen_s(NULL, "x", "rb");
    CHECK(rc == EINVAL, "fopen_s NULL pf rc=%d", rc);
}

static void test_wfopen_s(void) {
    fprintf(stderr, "[_wfopen_s]\n");
    FILE *f = NULL;
    int rc = _wfopen_s(&f, L"C:\\embedtest\\small.txt", L"rb");
    CHECK(rc == 0, "_wfopen_s rc=%d", rc);
    CHECK(f != NULL, "_wfopen_s f=NULL");
    if (f) fclose(f);
}
#endif

/* ============================================================
 * Threading - concurrent read access to virtual files.
 * ============================================================ */

#define MP_THREADS 16
#define MP_ITERS_PER_THREAD 200

typedef struct {
    int thread_id;
    int read_failures;   /* number of opens that returned NULL or read wrong content */
    int byte_mismatches; /* number of byte miscompares against the expected pattern */
} thread_stats_t;

/* Each worker opens binary.bin many times, seeks to (id % 256), reads 4
 * bytes, and verifies the content matches what we expect (binary.bin is
 * 0..255 in order, so byte at offset N is N). Then opens small.txt and
 * verifies the full content. This exercises:
 *   - the EFILE allocator under contention
 *   - independent stream positions across threads
 *   - mp_embed_find / CHD lookup path concurrency (read-only, lock-free) */
static int worker_body(int thread_id) {
    int failures = 0;
    int mismatches = 0;
    for (int i = 0; i < MP_ITERS_PER_THREAD; i++) {
        FILE *fp = fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
        if (!fp) { failures++; continue; }
        int off = (thread_id * 7 + i * 3) & 0xff;
        if (fseek(fp, off, SEEK_SET) != 0) { failures++; fclose(fp); continue; }
        unsigned char b[4];
        size_t n = fread(b, 1, 4, fp);
        if (n != (size_t)((off + 4 <= 256) ? 4 : 256 - off)) { failures++; fclose(fp); continue; }
        for (size_t k = 0; k < n; k++) {
            if (b[k] != (unsigned char)(off + k)) { mismatches++; break; }
        }
        fclose(fp);

        /* Concurrent reads of small.txt from each thread. */
        FILE *fp2 = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
        if (!fp2) { failures++; continue; }
        char text[16] = {0};
        n = fread(text, 1, 14, fp2);
        if (n != 14 || memcmp(text, "Hello, World!\n", 14) != 0) mismatches++;
        fclose(fp2);
    }
    return (failures << 16) | mismatches;
}

#ifdef _WIN32
static unsigned __stdcall mp_thread_main(void *arg) {
    thread_stats_t *s = (thread_stats_t*)arg;
    int packed = worker_body(s->thread_id);
    s->read_failures = packed >> 16;
    s->byte_mismatches = packed & 0xffff;
    return 0;
}
#else
static void *mp_thread_main(void *arg) {
    thread_stats_t *s = (thread_stats_t*)arg;
    int packed = worker_body(s->thread_id);
    s->read_failures = packed >> 16;
    s->byte_mismatches = packed & 0xffff;
    return NULL;
}
#endif

static void test_threading(void) {
    fprintf(stderr, "[threading: %d threads x %d iters]\n", MP_THREADS, MP_ITERS_PER_THREAD);
    thread_stats_t stats[MP_THREADS] = {0};

#ifdef _WIN32
    HANDLE handles[MP_THREADS];
    for (int i = 0; i < MP_THREADS; i++) {
        stats[i].thread_id = i;
        handles[i] = (HANDLE)_beginthreadex(NULL, 0, mp_thread_main, &stats[i], 0, NULL);
        if (handles[i] == 0) { CHECK(0, "thread create %d", i); return; }
    }
    WaitForMultipleObjects(MP_THREADS, handles, TRUE, INFINITE);
    for (int i = 0; i < MP_THREADS; i++) CloseHandle(handles[i]);
#else
    pthread_t threads[MP_THREADS];
    for (int i = 0; i < MP_THREADS; i++) {
        stats[i].thread_id = i;
        if (pthread_create(&threads[i], NULL, mp_thread_main, &stats[i]) != 0) {
            CHECK(0, "thread create %d", i); return;
        }
    }
    for (int i = 0; i < MP_THREADS; i++) pthread_join(threads[i], NULL);
#endif

    int total_failures = 0, total_mismatches = 0;
    for (int i = 0; i < MP_THREADS; i++) {
        total_failures += stats[i].read_failures;
        total_mismatches += stats[i].byte_mismatches;
    }
    CHECK(total_failures == 0,
          "%d threads x %d iters: %d open/seek/read failures",
          MP_THREADS, MP_ITERS_PER_THREAD, total_failures);
    CHECK(total_mismatches == 0,
          "%d threads x %d iters: %d byte mismatches",
          MP_THREADS, MP_ITERS_PER_THREAD, total_mismatches);
}

int main(void) {
    test_basic_fopen();
    test_empty_file();
    test_one_byte_file();
    test_binary_all_bytes();
    test_seek_tell();
    test_repeated_open();
    test_concurrent_handles();
    test_out_of_bounds();
    test_zero_count();
    test_uppercase_path();
    test_path_with_spaces();
    test_dotted_name();
    test_deep_nesting();
    test_missing_file();
    test_huge_file();
    test_compressible_file();
    test_fgets();
    test_mixed_real_and_virtual();
#ifdef _WIN32
    test_findfirst();
    test_fopen_s();
    test_wfopen_s();
#else
    test_opendir();
#endif
    test_stat();
    test_threading();

    fprintf(stderr, "\n=== C tests: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
