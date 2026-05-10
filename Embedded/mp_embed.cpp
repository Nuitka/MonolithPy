/* mp_embed.cpp - C++ entry points that need C++ name mangling.
 *
 * Specifically std::_Fiopen, the function MSVC's basic_filebuf::open
 * uses to actually open the FILE*. After mp_embed.h does
 *   #define _Fiopen mp__Fiopen
 * basic_filebuf's instantiation in user TUs routes through here
 * instead of going to libcpmt.lib's precompiled _Fiopen.
 *
 * Linux/macOS C++ stdlibs don't use _Fiopen; this file is Windows-only.
 *
 * NOTE: this file does NOT include <mp_embed.h>. The renaming and
 * inline machinery there are intended for user TUs that want to call
 * fopen/fread/etc. as if they were the standard CRT - we, the runtime,
 * just need the EFILE struct definition and a few entry-point
 * declarations. We declare those manually below to avoid pulling the
 * whole header in (and avoid recursive macro substitution, which would
 * rename the very function we're defining).
 */

#ifdef _WIN32

#include <ios>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <cstring>

/* Local copy of the EFILE struct. Must match Include/mp_embed.h. */
struct EMAP_STUB; /* opaque to this TU */
struct EFILE_S {
    uint32_t handle_type;
    const char* name;
    const char* pos;
    const char* end;
    size_t size;
    int err;
    FILE* f;
    struct EMAP_STUB* map;
    char* crt_base;
    char* crt_ptr;
    int   crt_cnt;
};
typedef struct EFILE_S EFILE;

#define EHANDLE_VIRTUAL 0x11111111

extern "C" {
    /* From mp_embed.c. */
    EFILE* mp_wfopen(const wchar_t *filename, const wchar_t *mode);
}

/* Mirror of MSVC's _Fiopen mode-table. Returns nullptr for invalid
 * combinations - matches what the original _Fiopen does. */
static const wchar_t* mode_string_for(std::ios_base::openmode mode) {
    using std::ios_base;
    /* Indexed by: in(1) | out(2) | trunc(4) | app(8) | binary(16). ate
     * is handled separately by the caller. */
    static const wchar_t* const mods[] = {
        nullptr,        // 0:  --
        L"r",           // 1:  in
        L"w",           // 2:  out
        L"r+",          // 3:  in|out
        nullptr,        // 4:  trunc
        nullptr,        // 5:  in|trunc
        L"w",           // 6:  out|trunc
        L"w+",          // 7:  in|out|trunc
        nullptr,        // 8:  app
        nullptr,        // 9:  in|app
        L"a",           // 10: out|app
        L"a+",          // 11: in|out|app
        nullptr, nullptr, nullptr, nullptr,
        nullptr,        // 16: --|binary
        L"rb",          // 17: in|binary
        L"wb",          // 18: out|binary
        L"r+b",         // 19: in|out|binary
        nullptr, nullptr,
        L"wb",          // 22: out|trunc|binary
        L"w+b",         // 23: in|out|trunc|binary
        nullptr, nullptr,
        L"ab",          // 26: out|app|binary
        L"a+b",         // 27: in|out|app|binary
    };
    unsigned idx = 0;
    if (mode & ios_base::in)     idx |= 1;
    if (mode & ios_base::out)    idx |= 2;
    if (mode & ios_base::trunc)  idx |= 4;
    if (mode & ios_base::app)    idx |= 8;
    if (mode & ios_base::binary) idx |= 16;
    if (idx >= sizeof(mods) / sizeof(mods[0])) return nullptr;
    return mods[idx];
}

/* The C++ overloads MSVC's basic_filebuf calls (after our macro rename).
 * Must be defined inside namespace std because the original _Fiopen
 * declaration in <__msvc_filebuf.hpp> is in std namespace - our macro
 * rename `_Fiopen -> mp__Fiopen` turns it into `std::mp__Fiopen`. */
namespace std {

extern "C++" FILE* mp__Fiopen(const wchar_t* filename, ios_base::openmode mode, int /*prot*/) {
    const wchar_t* mode_str = mode_string_for(mode);
    if (!mode_str) return nullptr;

    EFILE* e = mp_wfopen(filename, mode_str);
    if (!e) return nullptr;

    /* Honour ios_base::ate (seek to end after open). For virtual files
     * we update pos directly; for native we punt to fseek. */
    if ((mode & std::ios_base::ate) != 0) {
        if (e->handle_type == EHANDLE_VIRTUAL) {
            e->pos = e->end;
            e->crt_ptr = (char*)e->end;
            e->crt_cnt = 0;
        } else if (e->f) {
            if (fseek(e->f, 0, SEEK_END) != 0) {
                fclose(e->f);
                free(e);
                return nullptr;
            }
        }
    }
    return reinterpret_cast<FILE*>(e);
}

extern "C++" FILE* mp__Fiopen(const char* filename, ios_base::openmode mode, int prot) {
    /* Convert narrow path to wide and dispatch. */
    size_t need = mbstowcs(nullptr, filename, 0);
    if (need == (size_t)-1) return nullptr;
    wchar_t stack_buf[260];
    wchar_t* wfn = (need + 1 <= sizeof(stack_buf) / sizeof(stack_buf[0]))
                       ? stack_buf
                       : (wchar_t*)malloc((need + 1) * sizeof(wchar_t));
    if (!wfn) return nullptr;
    mbstowcs(wfn, filename, need + 1);
    FILE* result = mp__Fiopen(wfn, mode, prot);
    if (wfn != stack_buf) free(wfn);
    return result;
}

}  /* namespace std */

#endif  /* _WIN32 */
