/* mp_embed C++ test suite.
 *
 * What this verifies:
 *  - mp_embed.h can be included as the first header in a C++ TU without
 *    breaking <fstream>, <filesystem>, <string>, etc.
 *  - C-style APIs work from C++: std::fopen, std::fread, std::fclose.
 *  - std::ifstream transparently sees virtual files on every platform:
 *      * Windows: via std::_Fiopen and _get_stream_buffer_pointers
 *        intercepts that route MSVC's basic_filebuf through us.
 *      * Linux: via fopencookie + `-Wl,--wrap=fopen` so libstdc++.a's
 *        precompiled basic_filebuf::open ends up calling our fopen.
 *      * macOS: via funopen + `-Wl,-wrap,fopen` so libc++.a's
 *        basic_filebuf does the same.
 *  - Concurrent multi-threaded reads of the same and different virtual
 *    files don't corrupt state.
 */

#include <mp_embed.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include <fstream>          // included to verify it compiles cleanly
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>       // included to verify it compiles cleanly
#include <stdexcept>

#ifdef _WIN32
#  include <windows.h>
#  define VPATH_PREFIX  "C:\\embedtest"
#  define VPATH_SEP "\\"
#else
#  define VPATH_PREFIX "/c/embedtest"
#  define VPATH_SEP "/"
#endif

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                       \
    if (cond) { ++g_pass; }                                         \
    else {                                                          \
        ++g_fail;                                                   \
        std::fprintf(stderr, "FAIL %s:%d: %s\n",                    \
                __FILE__, __LINE__, msg);                           \
    }                                                               \
} while (0)

/* ============================================================
 * What works: C APIs called from C++
 * ============================================================ */

static void test_cstdio_basic() {
    std::fprintf(stderr, "[std::fopen + fread + fclose]\n");
    FILE *f = std::fopen(VPATH_PREFIX VPATH_SEP "small.txt", "rb");
    CHECK(f != nullptr, "std::fopen small.txt");
    if (!f) return;
    char buf[16] = {};
    size_t n = std::fread(buf, 1, 14, f);
    CHECK(n == 14, "std::fread 14 bytes");
    CHECK(std::string(buf) == "Hello, World!\n", "std::fopen content");
    std::fclose(f);
}

static void test_cstdio_seek() {
    std::fprintf(stderr, "[std::fseek + std::ftell from C++]\n");
    FILE *f = std::fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
    if (!f) { CHECK(false, "open binary.bin"); return; }
    CHECK(std::fseek(f, 0, SEEK_END) == 0, "fseek end");
    CHECK(std::ftell(f) == 256, "ftell 256");
    CHECK(std::fseek(f, 100, SEEK_SET) == 0, "fseek 100");
    int c = std::fgetc(f);
    CHECK(c == 100, "byte at 100 == 100");
    std::fclose(f);
}

static void test_cstdio_huge() {
    std::fprintf(stderr, "[std::fopen huge.dat + read 1MB]\n");
    FILE *f = std::fopen(VPATH_PREFIX VPATH_SEP "huge.dat", "rb");
    if (!f) { CHECK(false, "open huge.dat"); return; }
    std::vector<char> buf(1024 * 1024);
    size_t total = 0;
    size_t n;
    while ((n = std::fread(buf.data() + total, 1,
                           std::min<size_t>(4096, buf.size() - total), f)) > 0) {
        total += n;
        if (total >= buf.size()) break;
    }
    CHECK(total == 1024 * 1024, "huge total = 1MB");
    std::fclose(f);
}

static void test_cstdio_concurrent() {
    std::fprintf(stderr, "[multiple concurrent FILE* in C++]\n");
    /* Open 30 file handles concurrently, exercise the EFILE allocator
     * from C++ context. */
    std::vector<FILE*> handles;
    handles.reserve(30);
    for (int i = 0; i < 30; ++i) {
        FILE *fp = std::fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
        if (!fp) { CHECK(false, "concurrent open"); return; }
        std::fseek(fp, i, SEEK_SET);
        handles.push_back(fp);
    }
    bool ok = true;
    for (int i = 0; i < 30; ++i) {
        unsigned char c;
        std::fread(&c, 1, 1, handles[i]);
        if (c != static_cast<unsigned char>(i)) { ok = false; break; }
    }
    for (FILE *h : handles) std::fclose(h);
    CHECK(ok, "concurrent positions independent");
}

static void test_cstdio_uppercase() {
    std::fprintf(stderr, "[std::fopen UPPERCASE.TXT (case folding)]\n");
    FILE *f = std::fopen(VPATH_PREFIX VPATH_SEP "UPPERCASE.TXT", "rb");
    CHECK(f != nullptr, "fopen UPPERCASE.TXT");
    if (f) std::fclose(f);
}

static void test_cstdio_with_string() {
    std::fprintf(stderr, "[std::string path -> std::fopen]\n");
    /* Build path via std::string and pass to std::fopen. */
    std::string path = std::string(VPATH_PREFIX)
        + VPATH_SEP "deep" VPATH_SEP "nested" VPATH_SEP "tree" VPATH_SEP "leaf.txt";
    FILE *f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr, "fopen via std::string");
    if (f) {
        char buf[32] = {};
        size_t n = std::fread(buf, 1, 31, f);
        CHECK(n > 0 && std::string(buf, n) == "found it\n",
              "leaf.txt content via std::string path");
        std::fclose(f);
    }
}

static void test_path_with_spaces_cpp() {
    std::fprintf(stderr, "[std::fopen path with spaces]\n");
    FILE *f = std::fopen(VPATH_PREFIX VPATH_SEP "name with spaces.txt", "rb");
    CHECK(f != nullptr, "fopen 'name with spaces'");
    if (f) std::fclose(f);
}

/* ============================================================
 * std::ifstream on virtual paths - transparent on all platforms.
 * ============================================================ */

static void test_ifstream_virtual_read() {
    std::fprintf(stderr, "[std::ifstream::read on virtual path]\n");
    std::ifstream f(VPATH_PREFIX VPATH_SEP "small.txt", std::ios::binary);
    CHECK(f.is_open(), "open small.txt for read");
    if (!f.is_open()) return;
    char buf[32] = {};
    f.read(buf, 14);
    CHECK(f.gcount() == 14, "read 14 bytes");
    CHECK(std::string(buf, 14) == "Hello, World!\n", "read content correct");
    /* try to read more - should hit EOF */
    char extra[8] = {};
    f.read(extra, 8);
    CHECK(f.gcount() == 0, "read past EOF returns 0");
}

static void test_ifstream_virtual_basic() {
    std::fprintf(stderr, "[std::ifstream basic on virtual path]\n");
    std::ifstream f(VPATH_PREFIX VPATH_SEP "small.txt", std::ios::binary);
    CHECK(f.is_open(), "std::ifstream is_open small.txt (virtual)");
    if (!f.is_open()) return;
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str() == "Hello, World!\n", "ifstream content");
}

static void test_ifstream_virtual_getline() {
    std::fprintf(stderr, "[std::ifstream getline on virtual path]\n");
    std::ifstream f(VPATH_PREFIX VPATH_SEP "lorem.txt");
    CHECK(f.is_open(), "open lorem.txt (virtual)");
    if (!f.is_open()) return;
    std::string line;
    bool got = static_cast<bool>(std::getline(f, line));
    CHECK(got, "getline returned content");
    CHECK(line.find("Lorem ipsum") == 0, "first line starts with 'Lorem ipsum'");
}

static void test_ifstream_virtual_seek() {
    std::fprintf(stderr, "[std::ifstream seek/tell on virtual path]\n");
    std::ifstream f(VPATH_PREFIX VPATH_SEP "binary.bin", std::ios::binary);
    CHECK(f.is_open(), "open binary.bin (virtual)");
    if (!f.is_open()) return;
    f.seekg(0, std::ios::end);
    CHECK(f.tellg() == std::streampos(256), "tellg at end == 256");
    f.seekg(100);
    char ch = 0;
    f.read(&ch, 1);
    CHECK(static_cast<unsigned char>(ch) == 100, "byte at 100 == 100");
}

static void test_ifstream_virtual_huge() {
    std::fprintf(stderr, "[std::ifstream huge.dat on virtual path]\n");
    std::ifstream f(VPATH_PREFIX VPATH_SEP "huge.dat", std::ios::binary);
    CHECK(f.is_open(), "open huge.dat (virtual)");
    if (!f.is_open()) return;
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    CHECK(sz == std::streampos(1024 * 1024), "huge.dat size 1MB");
    f.seekg(0);
    std::vector<char> buf(1024 * 1024);
    f.read(buf.data(), buf.size());
    CHECK(static_cast<size_t>(f.gcount()) == buf.size(), "read full 1MB");
}

/* But std::ifstream DOES work on real files (transparently passes through). */
static void test_ifstream_real_file_compiles_and_runs() {
    std::fprintf(stderr, "[std::ifstream on a real file (passthrough)]\n");
#ifdef _WIN32
    std::ifstream f("C:\\Windows\\System32\\drivers\\etc\\hosts");
#else
    std::ifstream f("/etc/hosts");
#endif
    /* Whether the file exists is not what we're testing - just that the
     * code compiles and runs without crashing. */
    if (f.is_open()) {
        std::string line;
        std::getline(f, line);
        std::fprintf(stderr, "  read line of %zu bytes from real file\n", line.size());
    }
    ++g_pass; /* compile/run is the test */
}

/* ============================================================
 * Compile-only sanity: <fstream> and <filesystem> can be used in
 * a TU that has mp_embed.h prepended.
 * ============================================================ */

static void test_cpp_headers_compile() {
    std::fprintf(stderr, "[<fstream> + <filesystem> compile cleanly]\n");
    /* Just instantiate the types - if mp_embed.h's prepended macros broke
     * STL templates, this wouldn't even compile. */
    std::ifstream ifs;
    std::ofstream ofs;
    fs::path p("dummy");
    (void)ifs; (void)ofs; (void)p;
    ++g_pass;
}

/* ============================================================
 * Threading: concurrent std::ifstream reads from multiple threads.
 * ============================================================ */

#include <thread>
#include <atomic>
#include <vector>

static void test_ifstream_threading() {
    std::fprintf(stderr, "[std::ifstream concurrent: 16 threads x 200 iters]\n");
    constexpr int kThreads = 16;
    constexpr int kIters   = 200;
    std::atomic<int> failures{0};
    std::atomic<int> mismatches{0};

    auto worker = [&](int id) {
        for (int i = 0; i < kIters; ++i) {
            /* Concurrent reads of binary.bin via std::ifstream - each
             * thread seeks to a different offset then reads 4 bytes. */
            {
                std::ifstream f(VPATH_PREFIX VPATH_SEP "binary.bin", std::ios::binary);
                if (!f.is_open()) { ++failures; continue; }
                int off = (id * 13 + i * 5) & 0xff;
                f.seekg(off);
                unsigned char b[4]{};
                f.read(reinterpret_cast<char*>(b), 4);
                std::streamsize got = f.gcount();
                std::streamsize want = (off + 4 <= 256) ? 4 : 256 - off;
                if (got != want) { ++failures; continue; }
                for (int k = 0; k < got; ++k) {
                    if (b[k] != static_cast<unsigned char>(off + k)) { ++mismatches; break; }
                }
            }
            /* Concurrent reads of small.txt - fast lookup contention. */
            {
                std::ifstream f(VPATH_PREFIX VPATH_SEP "small.txt", std::ios::binary);
                if (!f.is_open()) { ++failures; continue; }
                std::string content((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
                if (content != "Hello, World!\n") ++mismatches;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    CHECK(failures.load() == 0, "ifstream threading: open/read failures");
    CHECK(mismatches.load() == 0, "ifstream threading: content mismatches");
}

/* Threading using the C-style fopen path - exercises the EFILE allocator
 * concurrently across many threads. Independent of std::ifstream. */
static void test_cstdio_threading() {
    std::fprintf(stderr, "[std::fopen concurrent: 16 threads x 500 iters]\n");
    constexpr int kThreads = 16;
    constexpr int kIters   = 500;
    std::atomic<int> failures{0};
    std::atomic<int> mismatches{0};

    auto worker = [&](int id) {
        for (int i = 0; i < kIters; ++i) {
            FILE *fp = std::fopen(VPATH_PREFIX VPATH_SEP "binary.bin", "rb");
            if (!fp) { ++failures; continue; }
            int off = (id * 31 + i * 7) & 0xff;
            std::fseek(fp, off, SEEK_SET);
            unsigned char buf[8]{};
            size_t got = std::fread(buf, 1, 8, fp);
            size_t want = (off + 8 <= 256) ? 8 : 256 - off;
            if (got != want) ++failures;
            else {
                for (size_t k = 0; k < got; ++k) {
                    if (buf[k] != static_cast<unsigned char>(off + k)) { ++mismatches; break; }
                }
            }
            std::fclose(fp);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();

    CHECK(failures.load() == 0, "cstdio threading: open/read failures");
    CHECK(mismatches.load() == 0, "cstdio threading: content mismatches");
}

int main() {
    test_cstdio_basic();
    test_cstdio_seek();
    test_cstdio_huge();
    test_cstdio_concurrent();
    test_cstdio_uppercase();
    test_cstdio_with_string();
    test_path_with_spaces_cpp();
    test_ifstream_virtual_read();
    test_ifstream_virtual_basic();
    test_ifstream_virtual_getline();
    test_ifstream_virtual_seek();
    test_ifstream_virtual_huge();
    test_ifstream_real_file_compiles_and_runs();
    test_cpp_headers_compile();
    test_cstdio_threading();
    test_ifstream_threading();

    std::fprintf(stderr, "\n=== C++ tests: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
