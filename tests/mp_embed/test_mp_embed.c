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
#  include <io.h>
#  include <fcntl.h>
#  define VPATH_PREFIX "C:\\embedtest"
#  define VPATH_SEP "\\"
#else
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  include <dirent.h>
#  include <fcntl.h>
#  include <limits.h>
#  include <pthread.h>
#  include <sys/mman.h>
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

    /* Seek past end: allowed, as on a real file, and reads there hit EOF.
     * A seek before the start fails and leaves the position alone. */
    int seek_rc = fseek(f, 9999, SEEK_SET);
    CHECK(seek_rc == 0, "seek past end on virtual file failed");
    CHECK(ftell(f) == 9999, "ftell after seek past end = %ld (want 9999)", ftell(f));
    n = fread(buf, 1, 8, f);
    CHECK(n == 0, "fread past end n=%zu (want 0)", n);
    CHECK(feof(f), "fread past end should set EOF");
    CHECK(fseek(f, -1, SEEK_SET) != 0, "seek before start should fail");
    CHECK(ftell(f) == 9999, "failed seek must not change position");

    fclose(f);
}

static void test_eof_indicator(void) {
    fprintf(stderr, "[eof_indicator]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    if (!f) { CHECK(0, "open"); return; }
    char buf[32];
    /* As on a real file, EOF is set by a read that hits the end, not by
     * reaching it. */
    CHECK(fread(buf, 1, 14, f) == 14, "read the whole file");
    CHECK(!feof(f), "EOF set before a read hit the end");
    CHECK(fgetc(f) == EOF && feof(f), "fgetc at end should set EOF");
    clearerr(f);
    CHECK(!feof(f), "clearerr should reset EOF");
    long pos = ftell(f);
    CHECK(ungetc(EOF, f) == EOF && ftell(f) == pos, "ungetc(EOF) must not move the stream");
    CHECK(fseek(f, -2, SEEK_END) == 0 && fgetc(f) == '!' && ungetc('!', f) == '!' && fgetc(f) == '!',
          "ungetc + fgetc");
    CHECK(fwrite("x", 1, 1, f) == 0 && ferror(f), "fwrite to a virtual stream should write nothing and set the error");
    clearerr(f);
    CHECK(!ferror(f), "clearerr should reset the error");
    fclose(f);
}

static void test_read_only(void) {
    fprintf(stderr, "[read_only]\n");
    static const char *modes[] = {"r+b", "wb", "ab"};
    for (int i = 0; i < 3; ++i) {
        errno = 0;
        FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", modes[i]);
        CHECK(f == NULL && errno == EROFS, "fopen(virtual, \"%s\") should fail with EROFS (errno=%d)", modes[i], errno);
        if (f) fclose(f);
    }
}

static void test_ungetc(void) {
    fprintf(stderr, "[ungetc]\n");
    FILE *f = fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    if (!f) { CHECK(0, "open"); return; }
    CHECK(fgetc(f) == 'H' && ungetc('X', f) == 'X' && ftell(f) == 0, "ungetc of a different byte");
    CHECK(fgetc(f) == 'X' && fgetc(f) == 'e', "the pushed-back byte is read once, then the content");
    CHECK(fgetc(f) == 'l' && ungetc('Y', f) == 'Y' && ungetc('Z', f) == EOF,
          "only one differing byte may be pending");
    CHECK(fseek(f, 0, SEEK_SET) == 0 && fgetc(f) == 'H' && fgetc(f) == 'e' && fgetc(f) == 'l',
          "repositioning drops the pushed-back byte");
    rewind(f);
    CHECK(ungetc('Q', f) == 'Q' && fgetc(f) == 'Q' && fgetc(f) == 'H', "ungetc before the first byte");
    fclose(f);
    f = fopen(VPATH_PREFIX VPATH_SEP "empty.txt", "rb");
    CHECK(f != NULL && ungetc('E', f) == 'E' && fgetc(f) == 'E' && fgetc(f) == EOF, "ungetc on an empty file");
    if (f) fclose(f);
}

/* build_test_tree.py sets small.txt's mtime. */
#define SMALL_MTIME 1700000000
static void test_mtime(void) {
    fprintf(stderr, "[mtime]\n");
#ifdef _WIN32
    struct _stat64 st = {0};
    CHECK(_stat64(VPATH_PREFIX VPATH_SEP "small.txt", &st) == 0 && st.st_mtime == SMALL_MTIME,
          "_stat64 mtime %lld", (long long)st.st_mtime);
    WIN32_FILE_ATTRIBUTE_DATA a;
    ULARGE_INTEGER t = {0};
    if (GetFileAttributesExW(L"C:\\embedtest\\small.txt", GetFileExInfoStandard, &a)) {
        t.LowPart = a.ftLastWriteTime.dwLowDateTime;
        t.HighPart = a.ftLastWriteTime.dwHighDateTime;
    }
    CHECK(t.QuadPart == SMALL_MTIME * 10000000ULL + 5000000ULL + 116444736000000000ULL,
          "GetFileAttributesExW last write time %llu", t.QuadPart);
#else
    struct stat st = {0};
    int rc = stat(VPATH_PREFIX VPATH_SEP "small.txt", &st);
#ifdef __APPLE__
    long nsec = st.st_mtimespec.tv_nsec;
#else
    long nsec = st.st_mtim.tv_nsec;
#endif
    CHECK(rc == 0 && st.st_mtime == SMALL_MTIME && nsec == 500000000,
          "stat mtime %lld.%09ld", (long long)st.st_mtime, nsec);
#endif
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

static void test_win_handles(void) {
    fprintf(stderr, "[win_handles]\n");
    /* Python's fstat(): GetFileType + GetFileInformationByHandle on the
     * fd's OS handle. */
    int fd = _open(VPATH_PREFIX VPATH_SEP "binary.bin", _O_RDONLY | _O_BINARY);
    CHECK(fd >= 0, "_open(virtual) errno=%d", errno);
    if (fd >= 0) {
        HANDLE oh = (HANDLE)_get_osfhandle(fd);
        BY_HANDLE_FILE_INFORMATION info = {0};
        CHECK(GetFileType(oh) == FILE_TYPE_DISK && GetFileInformationByHandle(oh, &info)
              && info.nFileSizeHigh == 0 && info.nFileSizeLow == 256,
              "handle of a virtual fd reports size %lu (want 256)", info.nFileSizeLow);
        _close(fd);
    }
    errno = 0;
    fd = _open(VPATH_PREFIX VPATH_SEP "small.txt", _O_RDWR | _O_BINARY);
    CHECK(fd == -1 && errno == EROFS, "_open(virtual, O_RDWR) fd=%d errno=%d", fd, errno);

    HANDLE h = CreateFileW(L"C:\\embedtest\\small.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW(virtual) err=%lu", GetLastError());
    if (h == INVALID_HANDLE_VALUE) return;
    HANDLE m = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    CHECK(m != NULL, "CreateFileMappingW(virtual) err=%lu", GetLastError());
    const char *view = m ? (const char *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    CHECK(view != NULL, "MapViewOfFile(virtual, FILE_MAP_READ) err=%lu", GetLastError());
    if (m) CloseHandle(m);
    CloseHandle(h);
    /* Like a real section, the view outlives both handles. */
    CHECK(view != NULL && memcmp(view, "Hello, World!\n", 14) == 0, "mapped view content");
    if (view) UnmapViewOfFile(view);

    /* sqlite reads through ReadFile, positioned by SetFilePointer or an OVERLAPPED. */
    h = CreateFileW(L"C:\\embedtest\\binary.bin", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileW(binary.bin) err=%lu", GetLastError());
    if (h == INVALID_HANDLE_VALUE) return;
    unsigned char b[8];
    DWORD got = 0;
    CHECK(ReadFile(h, b, 4, &got, NULL) && got == 4 && b[0] == 0 && b[3] == 3, "ReadFile(virtual handle)");
    CHECK(SetFilePointer(h, 200, NULL, FILE_BEGIN) == 200 && ReadFile(h, b, 1, &got, NULL) && got == 1 && b[0] == 200,
          "SetFilePointer, then ReadFile");
    LARGE_INTEGER zero = {0}, at = {0};
    CHECK(SetFilePointerEx(h, zero, &at, FILE_CURRENT) && at.QuadPart == 201,
          "SetFilePointerEx(FILE_CURRENT) = %lld", at.QuadPart);
    OVERLAPPED ov = {0};
    ov.Offset = 100;
    CHECK(ReadFile(h, b, 2, &got, &ov) && got == 2 && b[0] == 100 && b[1] == 101, "ReadFile at an OVERLAPPED offset");
    ov.Offset = 300;
    CHECK(!ReadFile(h, b, 2, &got, &ov) && GetLastError() == ERROR_HANDLE_EOF && got == 0,
          "ReadFile at an OVERLAPPED offset past the end");
    CHECK(SetFilePointer(h, 0, NULL, FILE_END) == 256 && ReadFile(h, b, 2, &got, NULL) && got == 0,
          "ReadFile at the end reads nothing");
    CHECK(SetFilePointer(h, -1, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() == ERROR_NEGATIVE_SEEK,
          "SetFilePointer before the start");

    /* A duplicate outlives the handle it was made from. */
    HANDLE dup = NULL;
    CHECK(DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS),
          "DuplicateHandle(virtual) err=%lu", GetLastError());
    CloseHandle(h);
    LARGE_INTEGER size = {0};
    CHECK(dup != NULL && GetFileSizeEx(dup, &size) && size.QuadPart == 256, "size through the duplicate");
    CHECK(dup != NULL && SetFilePointer(dup, 0, NULL, FILE_CURRENT) == 256, "the duplicate starts at the original's position");
    if (dup) CloseHandle(dup);

    /* Python's mmap maps a duplicate of the fd's handle. */
    int fd2 = _open(VPATH_PREFIX VPATH_SEP "small.txt", _O_RDONLY | _O_BINARY);
    HANDLE fdup = NULL;
    CHECK(fd2 >= 0 && DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(fd2), GetCurrentProcess(), &fdup,
                                      0, FALSE, DUPLICATE_SAME_ACCESS), "DuplicateHandle(fd's handle)");
    if (fd2 >= 0) _close(fd2);
    m = fdup ? CreateFileMappingW(fdup, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    view = m ? (const char *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    CHECK(view != NULL && memcmp(view, "Hello, World!\n", 14) == 0, "mapping of a duplicated fd handle");
    if (view) UnmapViewOfFile(view);
    if (m) CloseHandle(m);
    if (fdup) CloseHandle(fdup);
}

static void test_dup(void) {
    fprintf(stderr, "[dup]\n");
    int fd = _open(VPATH_PREFIX VPATH_SEP "small.txt", _O_RDONLY | _O_BINARY);
    if (fd < 0) { CHECK(0, "_open small.txt errno=%d", errno); return; }
    char b[16] = {0};
    CHECK(_read(fd, b, 7) == 7, "read before _dup");
    int d1 = _dup(fd);
    _close(fd);
    memset(b, 0, sizeof b);
    CHECK(d1 >= 0 && _read(d1, b, sizeof b - 1) == 7 && strcmp(b, "World!\n") == 0,
          "_dup() of a virtual fd reads on from its position: '%s'", b);
    struct _stat64 st = {0};
    CHECK(d1 >= 0 && _fstat64(d1, &st) == 0 && st.st_size == 14 && (st.st_mode & _S_IFREG) && st.st_mtime == SMALL_MTIME,
          "_fstat64(virtual fd) size=%lld", (long long)st.st_size);
    int target = _open("NUL", _O_RDONLY);
    CHECK(target >= 0 && _dup2(d1, target) == 0 && _lseek(target, 0, SEEK_SET) == 0 &&
          _read(target, b, 5) == 5 && memcmp(b, "Hello", 5) == 0, "_dup2() of a virtual fd onto another");
    int nul = _open("NUL", _O_RDONLY);
    CHECK(nul >= 0 && _dup2(nul, d1) == 0 && _fstat64(d1, &st) == 0 && !(st.st_mode & _S_IFREG),
          "_dup2() of a real fd over a virtual one");
    if (nul >= 0) _close(nul);
    if (target >= 0) _close(target);
    if (d1 >= 0) _close(d1);
}

#else
static void test_fd_level(void) {
    fprintf(stderr, "[fd_level]\n");
    struct stat st;
    errno = 0;
    CHECK(fstat(-1, &st) == -1 && errno == EBADF, "fstat(-1) errno=%d", errno);
    int fd = open(VPATH_PREFIX VPATH_SEP "binary.bin", O_RDONLY);
    if (fd < 0) { CHECK(0, "open binary.bin errno=%d", errno); return; }
    CHECK(fstat(fd, &st) == 0 && st.st_size == 256 && S_ISREG(st.st_mode), "fstat(virtual fd)");
    unsigned char b[8];
    CHECK(pread(fd, b, 4, 200) == 4 && b[0] == 200 && lseek(fd, 0, SEEK_CUR) == 0,
          "pread must read at the offset without moving the position");
    CHECK(lseek(fd, 1000, SEEK_SET) == 1000 && read(fd, b, 8) == 0, "lseek past end, then read");
    errno = 0;
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL && lseek(fd, 0, SEEK_CUR) == 1000,
          "failed lseek must fail with EINVAL and keep the position");
    /* Python's mmap module maps the fd. */
    const unsigned char *map = mmap(NULL, 256, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED && map[0] == 0 && map[200] == 200 && map[255] == 255, "mmap(virtual fd)");
    if (map != MAP_FAILED) munmap((void *)map, 256);
    errno = 0;
    CHECK(mmap(NULL, 256, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES,
          "writable shared mmap of a virtual fd errno=%d", errno);
    close(fd);
    errno = 0;
    fd = open(VPATH_PREFIX VPATH_SEP "small.txt", O_RDWR);
    CHECK(fd == -1 && errno == EROFS, "open(virtual, O_RDWR) fd=%d errno=%d", fd, errno);

    int dfd = open(VPATH_PREFIX VPATH_SEP "deep", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0, "open(virtual dir) errno=%d", errno);
    if (dfd >= 0) {
        CHECK(fstatat(dfd, "nested/tree/leaf.txt", &st, 0) == 0 && st.st_size == 9, "fstatat(virtual dirfd)");
        fd = openat(dfd, "nested/tree/leaf.txt", O_RDONLY);
        char t[16] = {0};
        CHECK(fd >= 0 && read(fd, t, sizeof t - 1) == 9 && strcmp(t, "found it\n") == 0, "openat(virtual dirfd)");
        if (fd >= 0) close(fd);
        close(dfd);
    }

    dfd = open(VPATH_PREFIX VPATH_SEP "manyfiles", O_RDONLY | O_DIRECTORY);
    DIR *d = dfd < 0 ? NULL : fdopendir(dfd);
    CHECK(d != NULL, "fdopendir(virtual dir fd) errno=%d", errno);
    if (d) {
        int count = 0;
        ino_t ino = 0;
        char name[256] = "";
        struct dirent *de;
        while ((de = readdir(d))) {
            if (count++ == 0) { ino = de->d_ino; snprintf(name, sizeof name, "%s", de->d_name); }
        }
        closedir(d);
        CHECK(count == 50, "fdopendir/readdir manyfiles got %d (want 50)", count);
        char p[512];
        snprintf(p, sizeof p, VPATH_PREFIX VPATH_SEP "manyfiles" VPATH_SEP "%s", name);
        CHECK(stat(p, &st) == 0 && st.st_ino == ino, "readdir d_ino must match stat st_ino");
    }
}

static void test_dup(void) {
    fprintf(stderr, "[dup]\n");
    int fd = open(VPATH_PREFIX VPATH_SEP "small.txt", O_RDONLY);
    if (fd < 0) { CHECK(0, "open small.txt errno=%d", errno); return; }
    char b[16] = {0};
    CHECK(read(fd, b, 7) == 7, "read before dup");
    int d1 = dup(fd);
    int d2 = fcntl(fd, F_DUPFD_CLOEXEC, 100);
    close(fd);
    memset(b, 0, sizeof b);
    CHECK(d1 >= 0 && read(d1, b, sizeof b - 1) == 7 && strcmp(b, "World!\n") == 0,
          "dup() of a virtual fd reads on from its position: '%s'", b);
    struct stat st;
    CHECK(d2 >= 100 && fstat(d2, &st) == 0 && st.st_size == 14 && (fcntl(d2, F_GETFD) & FD_CLOEXEC),
          "fcntl(F_DUPFD_CLOEXEC) of a virtual fd");
    int target = open("/dev/null", O_RDONLY);
    CHECK(target >= 0 && dup2(d2, target) == target && lseek(target, 0, SEEK_SET) == 0 &&
          read(target, b, 5) == 5 && memcmp(b, "Hello", 5) == 0, "dup2() of a virtual fd onto another");
    int nul = open("/dev/null", O_RDONLY);
    CHECK(nul >= 0 && dup2(nul, d1) == d1 && fstat(d1, &st) == 0 && !S_ISREG(st.st_mode),
          "dup2() of a real fd over a virtual one");
    if (nul >= 0) close(nul);
    if (target >= 0) close(target);
    if (d1 >= 0) close(d1);
    if (d2 >= 0) close(d2);

    /* os.listdir(fd) and os.scandir(fd) list an fdopendir() of a duplicate. */
    int dfd = open(VPATH_PREFIX VPATH_SEP "manyfiles", O_RDONLY | O_DIRECTORY);
    int copy = dfd < 0 ? -1 : fcntl(dfd, F_DUPFD_CLOEXEC, 0);
    DIR *d = copy < 0 ? NULL : fdopendir(copy);
    int count = 0;
    if (d) {
        rewinddir(d);
        while (readdir(d)) ++count;
        closedir(d);
    }
    CHECK(count == 50, "listing a duplicate of a virtual directory fd got %d (want 50)", count);
    CHECK(dfd >= 0 && fstatat(dfd, "file_00.txt", &st, 0) == 0, "the directory fd survives its duplicate");
    if (dfd >= 0) close(dfd);
}
#endif

/* Entries below the executable resolve against the folder Python's getpath
 * derives, which keeps symlinks (POSIX: argv[0] made absolute, then the
 * final component readlink()ed until it is no link) or is the final path
 * of the executable (Windows realpath). */
#ifdef _WIN32
static void check_exe_relative(const wchar_t *dir) {
    wchar_t w[MAX_PATH * 4];
    swprintf(w, MAX_PATH * 4, L"%ls\\embedrel\\rel.txt", dir);
    FILE *f = _wfopen(w, L"rb");
    char buf[64] = {0};
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    CHECK(strcmp(buf, "relative to the executable\n") == 0, "'%ls' not served (n=%zu)", w, n);
}

/* A virtual directory over a real one lists the files of both, and the
 * embedded ones win. */
static void check_union(const wchar_t *dir) {
    wchar_t real[MAX_PATH * 4], disk[MAX_PATH * 4], tmp[MAX_PATH * 4], shadow[MAX_PATH * 4], pattern[MAX_PATH * 4];
    swprintf(real, MAX_PATH * 4, L"%ls\\embedrel", dir);
    swprintf(disk, MAX_PATH * 4, L"%ls\\disk_only.txt", real);
    swprintf(tmp, MAX_PATH * 4, L"%ls\\shadow.tmp", real);
    swprintf(shadow, MAX_PATH * 4, L"%ls\\rel.txt", real);
    swprintf(pattern, MAX_PATH * 4, L"%ls\\*", real);
    CreateDirectoryW(real, NULL);
    FILE *f = _wfopen(disk, L"wb");
    if (f) { fputs("on disk\n", f); fclose(f); }
    /* rel.txt is virtual, so it can only be created on disk by a rename. */
    f = _wfopen(tmp, L"wb");
    if (f) { fputs("shadowed\n", f); fclose(f); }
    _wrename(tmp, shadow);

    int rel = 0, on_disk = 0, other = 0;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(pattern, &data);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L"rel.txt")) rel++;
            else if (!wcscmp(data.cFileName, L"disk_only.txt")) on_disk++;
            else other++;
        } while (FindNextFileW(h, &data));
        FindClose(h);
    }
    CHECK(rel == 1 && on_disk == 1 && other == 0, "union listing: rel.txt x%d, disk_only.txt x%d, %d others",
          rel, on_disk, other);
    char buf[64] = {0};
    f = _wfopen(disk, L"rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    CHECK(n == 8 && strcmp(buf, "on disk\n") == 0, "a real file in a virtual directory");
    memset(buf, 0, sizeof buf);
    f = _wfopen(shadow, L"rb");
    n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    CHECK(strcmp(buf, "relative to the executable\n") == 0, "the embedded file wins over the real one");
    _wremove(disk);
    _wremove(shadow);
    RemoveDirectoryW(real);
}

static void test_exe_relative(const char *argv0) {
    fprintf(stderr, "[exe_relative]\n");
    (void)argv0;
    wchar_t dir[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH * 4);
    while (n > 0 && dir[n - 1] != L'\\') n--;
    dir[n ? n - 1 : 0] = 0;
    check_exe_relative(dir);
    check_union(dir);
    _wcslwr(dir);  /* ntpath.normcase() */
    check_exe_relative(dir);

    HANDLE h = CreateFileW(dir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    wchar_t fin[MAX_PATH * 4];
    DWORD k = h == INVALID_HANDLE_VALUE ? 0 : GetFinalPathNameByHandleW(h, fin, MAX_PATH * 4, 0);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CHECK(k > 0 && k < MAX_PATH * 4, "final path of the executable folder err=%lu", GetLastError());
    if (k > 0 && k < MAX_PATH * 4) check_exe_relative(wcsncmp(fin, L"\\\\?\\", 4) ? fin : fin + 4);
}
#else
static void normalize(char *p) {
    char in[PATH_MAX], out[PATH_MAX] = "";
    snprintf(in, sizeof in, "%s", p);
    size_t o = 0;
    char *save = NULL;
    for (char *t = strtok_r(in, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { while (o > 0 && out[--o] != '/') {} out[o] = 0; continue; }
        o += snprintf(out + o, sizeof out - o, "/%s", t);
    }
    snprintf(p, PATH_MAX, "%s", o ? out : "/");
}

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, text, strlen(text)) < 0) {}
    close(fd);
}

/* A virtual directory over a real one lists the files of both, and the
 * embedded ones win. */
static void check_union(const char *dir) {
    char real[PATH_MAX + 16], disk[PATH_MAX + 32], tmp[PATH_MAX + 32], shadow[PATH_MAX + 32];
    snprintf(real, sizeof real, "%s/embedrel", dir);
    snprintf(disk, sizeof disk, "%s/disk_only.txt", real);
    snprintf(tmp, sizeof tmp, "%s/shadow.tmp", real);
    snprintf(shadow, sizeof shadow, "%s/rel.txt", real);
    mkdir(real, 0755);
    write_file(disk, "on disk\n");
    /* rel.txt is virtual, so it can only be created on disk by a rename. */
    write_file(tmp, "shadowed\n");
    rename(tmp, shadow);

    for (int by_fd = 0; by_fd < 2; by_fd++) {
        int dfd = by_fd ? open(real, O_RDONLY | O_DIRECTORY) : -1;
        DIR *d = by_fd ? (dfd < 0 ? NULL : fdopendir(dfd)) : opendir(real);
        int rel = 0, on_disk = 0, other = 0;
        struct dirent *de;
        while (d && (de = readdir(d))) {
            if (!strcmp(de->d_name, "rel.txt")) rel++;
            else if (!strcmp(de->d_name, "disk_only.txt")) on_disk++;
            else other++;
        }
        if (d) closedir(d);
        CHECK(rel == 1 && on_disk == 1 && other == 0, "union listing (%s): rel.txt x%d, disk_only.txt x%d, %d others",
              by_fd ? "fdopendir" : "opendir", rel, on_disk, other);
    }
    char buf[64] = {0};
    size_t n = 0;
    CHECK(read_full(disk, buf, sizeof buf - 1, &n) && strcmp(buf, "on disk\n") == 0, "a real file in a virtual directory");
    memset(buf, 0, sizeof buf);
    CHECK(read_full(shadow, buf, sizeof buf - 1, &n) && strcmp(buf, "relative to the executable\n") == 0,
          "the embedded file wins over the real one");
    unlink(disk);
    unlink(shadow);
    rmdir(real);
}

static void test_exe_relative(const char *argv0) {
    fprintf(stderr, "[exe_relative]\n");
    char p[PATH_MAX], t[PATH_MAX], q[PATH_MAX + 32];
    if (argv0[0] == '/' || !getcwd(p, sizeof p)) p[0] = 0;
    size_t lp = strlen(p), la = strlen(argv0);
    if (lp + 1 + la >= sizeof p) { CHECK(0, "argv[0] too long"); return; }
    if (lp) p[lp++] = '/';
    memcpy(p + lp, argv0, la + 1);
    normalize(p);
    for (int i = 0; i < 40; i++) {
        ssize_t k = readlink(p, t, sizeof t - 1);
        if (k < 0) break;
        t[k] = 0;
        /* A relative target replaces the final component. */
        size_t keep = t[0] == '/' ? 0 : (size_t)(strrchr(p, '/') - p) + 1;
        if (keep + (size_t)k >= sizeof p) break;
        memcpy(p + keep, t, (size_t)k + 1);
        normalize(p);
    }
    *strrchr(p, '/') = 0;
    snprintf(q, sizeof q, "%s/embedrel/rel.txt", p);
    char buf[64] = {0};
    size_t n = 0;
    CHECK(read_full(q, buf, sizeof buf - 1, &n) && strcmp(buf, "relative to the executable\n") == 0,
          "'%s' not served (n=%zu)", q, n);
    check_union(p);
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

        /* Concurrent directory listings. */
        int count = 0;
#ifdef _WIN32
        WIN32_FIND_DATAW data;
        HANDLE h = FindFirstFileW(L"C:\\embedtest\\manyfiles\\*", &data);
        if (h != INVALID_HANDLE_VALUE) {
            do ++count; while (FindNextFileW(h, &data));
            FindClose(h);
        }
#else
        DIR *d = opendir(VPATH_PREFIX VPATH_SEP "manyfiles");
        if (d) {
            while (readdir(d)) ++count;
            closedir(d);
        }
#endif
        if (count != 50) failures++;
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

int main(int argc, char **argv) {
    (void)argc;
    test_basic_fopen();
    test_empty_file();
    test_one_byte_file();
    test_binary_all_bytes();
    test_seek_tell();
    test_repeated_open();
    test_concurrent_handles();
    test_out_of_bounds();
    test_eof_indicator();
    test_ungetc();
    test_mtime();
    test_read_only();
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
    test_win_handles();
    test_dup();
#else
    test_opendir();
    test_fd_level();
    test_dup();
#endif
    test_stat();
    test_exe_relative(argv[0]);
    test_threading();

    fprintf(stderr, "\n=== C tests: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
