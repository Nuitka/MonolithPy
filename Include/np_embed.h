/*
# np_embed
# Inspired by c-embed by nicholas mcdonald 2022
*/

#ifndef NUITKAEMBED
#define NUITKAEMBED

#if !defined(__ASSEMBLER__) && !defined(BYPASS_NP_EMBED)
#ifdef __cplusplus
extern "C" {
#endif

#ifdef __linux
# define _GNU_SOURCE
#endif

#ifdef FOPEN_MAX
// This means that we were loaded too late so we can't intercept the necessary calls.
// Don't even try in that case.
#define NP_STDIO_ALREADY_LOADED
#include <fcntl.h>
#ifdef _WIN32
    #include <wchar.h>
    #include <BaseTsd.h>
    #define PATH_MAX MAX_PATH
    typedef SSIZE_T ssize_t;
#else
    #include <limits.h>
    #include <unistd.h>
    #include <libgen.h>
#endif
#else

#ifdef _WIN32
#define _CRTIMP
#endif
#ifndef NUITKAPYTHON_EMBED_BUILD
#define open orig_open
#define _open orig__open
#define fopen orig_fopen
#define _fopen orig__fopen
#define fdopen orig_fdopen
#define _fdopen orig__fdopen
#define freopen orig_freopen
#define _freopen orig__freopen
#define fclose orig_fclose
#define _fclose orig__fclose
#define close orig_close
#define _close orig__close
#define wopen orig_wopen
#define _wopen orig__wopen
#define wfopen orig_wfopen
#define _wfopen orig__wfopen
#define tmpfile orig_tmpfile
#define fgets orig_fgets
#define _fgets orig__fgets
#define getc orig_getc
#define fgetc orig_fgetc
#define _fgetc orig__fgetc
#define fscanf orig_fscanf
#define _fscanf orig__fscanf
#define fread orig_fread
#define _fread orig__fread
#define read orig_read
#define _read orig__read
#define pread orig_pread
#define getc_unlocked orig_getc_unlocked
#define ungetc orig_ungetc
#define _ungetc orig__ungetc
#define putc orig_putc
#define _putc orig__putc
#define fputc orig_fputc
#define _fputc orig__fputc
#define fputs orig_fputs
#define _fputs orig__fputs
#define fwrite orig_fwrite
#define _fwrite orig__fwrite
#define setbuf orig_setbuf
#define _setbuf orig__setbuf
#define setvbuf orig_setvbuf
#define _setvbuf orig__setvbuf
#define fseek orig_fseek
#define _fseek orig__fseek
#define ftell orig_ftell
#define _ftell orig__ftell
#define _fseeki64 orig__fseeki64
#define _ftelli64 orig__ftelli64
#define fseeko orig_fseeko
#define ftello orig_ftello
#define rewind orig_rewind
#define _rewind orig__rewind
#define fgetpos orig_fgetpos
#define _fgetpos orig__fgetpos
#define fsetpos orig_fsetpos
#define _fsetpos orig__fsetpos
#define clearerr orig_clearerr
#define _clearerr orig__clearerr
#define feof orig_feof
#define _feof orig__feof
#define ferror orig_ferror
#define _ferror orig__ferror
#define fileno orig_fileno
#define _fileno orig__fileno
#define fflush orig_fflush
#define _fflush orig__fflush
#define flockfile orig_flockfile
#define funlockfile orig_funlockfile
#define ftrylockfile orig_ftrylockfile

#ifdef __linux
#define stdin orig_stdin
#define stdout orig_stdout
#define stderr orig_stderr
#endif
#endif
#ifdef __linux
#include <features.h>
#undef __USE_EXTERN_INLINES
#define _BITS_STDIO_H
#endif
#include <stdio.h>
#include <fcntl.h>
#ifdef _WIN32
    #include <wchar.h>
    #include <BaseTsd.h>
    #define PATH_MAX MAX_PATH
    typedef SSIZE_T ssize_t;
#else
    #include <limits.h>
    #include <unistd.h>
    #include <libgen.h>
#endif
#undef open
#undef _open
#undef fopen
#undef _fopen
#undef fdopen
#undef _fdopen
#undef freopen
#undef _freopen
#undef fclose
#undef _fclose
#undef close
#undef _close
#undef wopen
#undef _wopen
#undef wfopen
#undef _wfopen
#undef tmpfile
#undef fgets
#undef _fgets
#undef getc
#undef fgetc
#undef _fgetc
#undef fscanf
#undef _fscanf
#undef fread
#undef _fread
#undef read
#undef _read
#undef pread
#undef getc_unlocked
#undef ungetc
#undef _ungetc
#undef putc
#undef fputc
#undef _fputc
#undef fputs
#undef _fputs
#undef fwrite
#undef _fwrite
#undef setbuf
#undef _setbuf
#undef setvbuf
#undef _setvbuf
#undef fseek
#undef _fseek
#undef ftell
#undef _ftell
#undef _fseeki64
#undef _ftelli64
#undef fseeko
#undef ftello
#undef rewind
#undef _rewind
#undef fgetpos
#undef _fgetpos
#undef fsetpos
#undef _fsetpos
#undef clearerr
#undef _clearerr
#undef feof
#undef _feof
#undef ferror
#undef _ferror
#undef fileno
#undef _fileno
#undef fflush
#undef _fflush
#undef flockfile
#undef funlockfile
#undef ftrylockfile
#ifdef __linux
#undef stdin
#undef stdout
#undef stderr
#endif

#endif

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdbool.h>
#include <string.h>


struct EMAP_S {     // Map Indexing Struct
  uint32_t hash;
  uint32_t parentidx;
  uint32_t pathpos;
  uint32_t pathsize;
  uint8_t type;
  uint32_t pos;
  uint32_t size;
};
typedef struct EMAP_S EMAP;

struct EFILE_S {    // Virtual File Stream
  uint32_t handle_type;
  const char* name;
  const char* pos;
  const char* end;
  size_t size;
  int err;
  FILE* f;
};
typedef struct EFILE_S EFILE;

#define ETYPE_DIRECTORY 0
#define ETYPE_FILE 1

#define EHANDLE_VIRTUAL 0x11111111
#define EHANDLE_NATIVE 0xFFFFFFFF

#ifdef _WIN32
#define NP_DECL(x) _ACRTIMP x __cdecl
#else
#define NP_DECL(x) x
#endif

#if defined(_MSC_VER)
#ifdef __cplusplus
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE static __forceinline
#endif
#elif defined(__GNUC__) || defined(__clang__)
#ifdef __cplusplus
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE __attribute__((always_inline)) static inline
#endif
#else
#ifdef __cplusplus
#define ALWAYS_INLINE inline
#else
#define ALWAYS_INLINE static inline
#endif
#endif

#ifdef _WIN32
#define NP_FOREIGN_PTR *(void**)e == NULL || (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE)
#else
#define NP_FOREIGN_PTR ((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE
#endif

NP_DECL(EFILE*) np_fopen(const char* file, const char* mode);
#ifdef _WIN32
NP_DECL(int) np_open(const char *pathname, int flags, int mode);
NP_DECL(EFILE*) np_wfopen(const wchar_t *wfile, const wchar_t *mode);
NP_DECL(int) np_wopen(const wchar_t *pathname, int flags, int mode);
#else
NP_DECL(int) np_open(const char *pathname, int flags, mode_t mode);
#endif
NP_DECL(int) np_fclose(void* e);
NP_DECL(int) np_close(int fd);
NP_DECL(EFILE*) np_tmpfile();
NP_DECL(bool) np_feof(void* e);
NP_DECL(size_t) np_fread(void* ptr, size_t size, size_t count, void* stream);
#ifdef _WIN32
NP_DECL(int) np_read(int fd, void *buf, unsigned int count);
#else
NP_DECL(ssize_t) np_read(int fd, void *buf, size_t count);
#endif
NP_DECL(ssize_t) np_pread(int fd, void *buf, size_t count, off_t offset);
NP_DECL(int) np_fgetpos(void* e, fpos_t* pos);
/* File Opening and Closing */
NP_DECL(EFILE*) np_freopen(const char *filename, const char *mode, void *stream);
NP_DECL(EFILE*) np_fdopen(int fd, const char *mode);

/* File Input Functions */
NP_DECL(int) np_fgetc(void *stream);
NP_DECL(char*) np_fgets(char *str, int n, void *stream);
NP_DECL(int) np_fscanf(void *stream, const char *format, ...);
NP_DECL(int) np_getc_unlocked(void *stream);  /* Unlocked version of egetc */

/* File Output Functions */
NP_DECL(int) np_fputc(int character, void *stream);
NP_DECL(int) np_fputs(const char *str, void *stream);
NP_DECL(int) np_fprintf(void *stream, const char *format, ... );
NP_DECL(int) np_vfprintf(void *e, const char *format, va_list args);
NP_DECL(size_t) np_fwrite(const void *ptr, size_t size, size_t count, void *stream);

/* File Buffering */
NP_DECL(void) np_setbuf(void *stream, char *buffer);
NP_DECL(int) np_setvbuf(void *stream, char *buffer, int mode, size_t size);

/* File Positioning */
NP_DECL(int) np_fseek(void *stream, long int offset, int origin);
NP_DECL(long int) np_ftell(void *stream);
NP_DECL(int) np_fseeko64(void *stream, int64_t offset, int origin);
NP_DECL(int64_t) np_ftello64(void *stream);
NP_DECL(void) np_rewind(void *stream);
NP_DECL(int) np_fsetpos(void *stream, const fpos_t *pos);
NP_DECL(int) np_ungetc(int character, void *stream);

/* Error Handling & Other Utilities */
NP_DECL(void) np_clearerr(void *stream);
NP_DECL(int) np_ferror(void *stream);
NP_DECL(int) np_fileno(void *stream);
NP_DECL(int) np_fflush(void *stream);

/* Locking Functions */
NP_DECL(void) np_flockfile(void *e);
NP_DECL(void) np_funlockfile(void *e);
NP_DECL(int) np_ftrylockfile(void *e);

#if !defined(NUITKAPYTHON_EMBED_BUILD) && !defined(NP_STDIO_ALREADY_LOADED)

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
#define CAT(a, ...) PRIMITIVE_CAT(a, __VA_ARGS__)
#define PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

#define IIF(c) PRIMITIVE_CAT(IIF_, c)
#define IIF_0(t, ...) __VA_ARGS__
#define IIF_1(t, ...) t

#define COMPL(b) PRIMITIVE_CAT(COMPL_, b)
#define COMPL_0 1
#define COMPL_1 0

#define INC(x) PRIMITIVE_CAT(INC_, x)
#define INC_0 1
#define INC_1 2
#define INC_2 3
#define INC_3 4
#define INC_4 5
#define INC_5 6
#define INC_6 7
#define INC_7 8
#define INC_8 9
#define INC_9 10
#define INC_10 11
#define INC_11 12
#define INC_12 13
#define INC_13 14
#define INC_14 15
#define INC_15 16
#define INC_16 17
#define INC_17 18
#define INC_18 19
#define INC_19 20

#define DEC(x) PRIMITIVE_CAT(DEC_, x)
#define DEC_0 0
#define DEC_1 0
#define DEC_2 1
#define DEC_3 2
#define DEC_4 3
#define DEC_5 4
#define DEC_6 5
#define DEC_7 6
#define DEC_8 7
#define DEC_9 8
#define DEC_10 9
#define DEC_11 10
#define DEC_12 11
#define DEC_13 12
#define DEC_14 13
#define DEC_15 14
#define DEC_16 15
#define DEC_17 16
#define DEC_18 17
#define DEC_19 18
#define DEC_20 19

#define CHECK_N(x, n, ...) n
#define CHECK(...) CHECK_N(__VA_ARGS__, 0,)
#define PROBE(x) x, 1,

#define IS_PAREN(x) CHECK(IS_PAREN_PROBE x)
#define IS_PAREN_PROBE(...) PROBE(~)

#define NOT(x) CHECK(PRIMITIVE_CAT(NOT_, x))
#define NOT_0 PROBE(~)

#define BOOL(x) COMPL(NOT(x))
#define IF(c) IIF(BOOL(c))

#define EAT(...)
#define EXPAND(...) __VA_ARGS__
#define WHEN(c) IF(c)(EXPAND, EAT)

#define EMPTY()
#define DEFER(id) id EMPTY()
#define OBSTRUCT(...) __VA_ARGS__ DEFER(EMPTY)()
#define EXPAND(...) __VA_ARGS__

#define NUM_ARGS1(_20,_19,_18,_17,_16,_15,_14,_13,_12,_11,_10,_9,_8,_7,_6,_5,_4,_3,_2,_1, n, ...) n
#define NUM_ARGS0(...) NUM_ARGS1(__VA_ARGS__,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define NUM_ARGS(...) IF(DEC(NUM_ARGS0(__VA_ARGS__)))(NUM_ARGS0(__VA_ARGS__),IF(IS_PAREN(__VA_ARGS__ ()))(0,1))
#endif  // GCC only

// Preprocessor Translation
#define FILE EFILE

#ifdef __linux
/* Standard streams.  */
extern EFILE *stdin;		/* Standard input stream.  */
extern EFILE *stdout;		/* Standard output stream.  */
extern EFILE *stderr;		/* Standard error output stream.  */
#endif  // __linux

/* File Opening and Closing */
ALWAYS_INLINE NP_DECL(EFILE*) fopen(const char* file, const char* mode) {
    return np_fopen(file, mode);
}
ALWAYS_INLINE NP_DECL(EFILE*) _fopen(const char* file, const char* mode) {
    return np_fopen(file, mode);
}

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
#ifdef __cplusplus
ALWAYS_INLINE NP_DECL(int) open(const char *pathname, int flags, mode_t mode = 0) {
#else
ALWAYS_INLINE NP_DECL(int) open(const char *pathname, int flags, mode_t mode) {
#endif
    return np_open(pathname, flags, mode);
}
#ifdef __cplusplus
ALWAYS_INLINE NP_DECL(int) _open(const char *pathname, int flags, mode_t mode = 0) {
#else
ALWAYS_INLINE NP_DECL(int) _open(const char *pathname, int flags, mode_t mode) {
#endif
  return np_open(pathname, flags, mode);
}
#ifndef __cplusplus
// C does not support optional parameters so we are forced to rely on the mess below
// because GCC does not support inlining vararg function. :(
#define open0() open()
#define open1(a) open(a)
#define open2(a, b) open(a, b, 0)
#define open3(a, b, c) open(a, b, c)
#define open4(a, b, c, d) open(a, b, c, d)
#define open5(a, b, c, d, e) open(a, b, c, d, e)
#define open6(a, b, c, d, e, f) open(a, b, c, d, e, f)
#define open7(a, b, c, d, e, f, g) open(a, b, c, d, e, f, g)
#define open(...) CAT( open, NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )
#define _open(...) CAT( open, NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )
#endif  // !__cplusplus
#else  // GCC only
ALWAYS_INLINE NP_DECL(int) open(const char *pathname, int flags, ... /* mode_t mode */ ) {
    va_list args;
    va_start(args, flags);
#ifdef _WIN32
    int mode = 0;
#else
    mode_t mode = 0;
#endif
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return np_open(pathname, flags, mode);
}
ALWAYS_INLINE NP_DECL(int) _open(const char *pathname, int flags, ... /* mode_t mode */ ) {
    va_list args;
    va_start(args, flags);
#ifdef _WIN32
    int mode = 0;
#else
    mode_t mode = 0;
#endif
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return np_open(pathname, flags, mode);
}
#endif  // !GCC

ALWAYS_INLINE NP_DECL(EFILE*) fdopen(int fd, const char *mode) {
  return np_fdopen(fd, mode);
}
ALWAYS_INLINE NP_DECL(EFILE*) _fdopen(int fd, const char *mode) {
  return np_fdopen(fd, mode);
}

ALWAYS_INLINE NP_DECL(EFILE*) freopen(const char *filename, const char *mode, void *stream) {
  return np_freopen(filename, mode, stream);
}
ALWAYS_INLINE NP_DECL(EFILE*) _freopen(const char *filename, const char *mode, void *stream) {
  return np_freopen(filename, mode, stream);
}

ALWAYS_INLINE NP_DECL(int) fclose(FILE* e) {
  return np_fclose(e);
}
ALWAYS_INLINE NP_DECL(int) _fclose(FILE* e) {
  return np_fclose(e);
}

ALWAYS_INLINE NP_DECL(int) close(int fd) {
  return np_close(fd);
}
ALWAYS_INLINE NP_DECL(int) _close(int fd) {
  return np_close(fd);
}

#ifdef _WIN32
ALWAYS_INLINE NP_DECL(int) wopen(const wchar_t *pathname, int flags, ... /* int mode */ ) {
    va_list args;
    va_start(args, flags);
    int mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return np_wopen(pathname, flags, mode);
}
ALWAYS_INLINE NP_DECL(int) _wopen(const wchar_t *pathname, int flags, ... /* int mode */ ) {
    va_list args;
    va_start(args, flags);
    int mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return np_wopen(pathname, flags, mode);
}

ALWAYS_INLINE NP_DECL(EFILE*) wfopen(const wchar_t* file, const wchar_t* mode) {
    return np_wfopen(file, mode);
}
ALWAYS_INLINE NP_DECL(EFILE*) _wfopen(const wchar_t* file, const wchar_t* mode) {
    return np_wfopen(file, mode);
}
#endif  // _WIN32

ALWAYS_INLINE NP_DECL(EFILE*) tmpfile() {
  return np_tmpfile();
}

/* File Input Functions */
ALWAYS_INLINE NP_DECL(char*) fgets(char *str, int n, void *stream) {
    return np_fgets(str, n, stream);
}
ALWAYS_INLINE NP_DECL(char*) _fgets(char *str, int n, void *stream) {
    return np_fgets(str, n, stream);
}

ALWAYS_INLINE NP_DECL(int) getc(void *stream) {
    return np_fgetc(stream);
}
ALWAYS_INLINE NP_DECL(int) fgetc(void *stream) {
    return np_fgetc(stream);
}
ALWAYS_INLINE NP_DECL(int) _fgetc(void *stream) {
    return np_fgetc(stream);
}

// Need to use a macro for this one due to varargs.
#define fscanf np_fscanf
#define _fscanf np_fscanf

ALWAYS_INLINE NP_DECL(size_t) fread(void* ptr, size_t size, size_t count, FILE* stream) {
    return np_fread(ptr, size, count, stream);
}
ALWAYS_INLINE NP_DECL(size_t) _fread(void* ptr, size_t size, size_t count, FILE* stream) {
    return np_fread(ptr, size, count, stream);
}

#ifdef _WIN32
ALWAYS_INLINE NP_DECL(int) read(int fd, void *buf, unsigned int count) {
#else
ALWAYS_INLINE NP_DECL(ssize_t) read(int fd, void *buf, size_t count) {
#endif
    return np_read(fd, buf, count);
}
#ifdef _WIN32
ALWAYS_INLINE NP_DECL(int) _read(int fd, void *buf, unsigned int count) {
#else
ALWAYS_INLINE NP_DECL(ssize_t) _read(int fd, void *buf, size_t count) {
#endif
    return np_read(fd, buf, count);
}

#ifndef _WIN32
ALWAYS_INLINE NP_DECL(ssize_t) pread(int fd, void *buf, size_t count, off_t offset) {
    return np_pread(fd, buf, count, offset);
}
ALWAYS_INLINE NP_DECL(int) getc_unlocked(void *stream) {
    return np_getc_unlocked(stream);
}
#endif

ALWAYS_INLINE NP_DECL(int) ungetc(int character, void *stream) {
    return np_ungetc(character, stream);
}
ALWAYS_INLINE NP_DECL(int) _ungetc(int character, void *stream) {
    return np_ungetc(character, stream);
}

/* File Output Functions */
ALWAYS_INLINE NP_DECL(int) putc(int character, void *stream) {
    return np_fputc(character, stream);
}
ALWAYS_INLINE NP_DECL(int) _putc(int character, void *stream) {
    return np_fputc(character, stream);
}
ALWAYS_INLINE NP_DECL(int) fputc(int character, void *stream) {
    return np_fputc(character, stream);
}
ALWAYS_INLINE NP_DECL(int) _fputc(int character, void *stream) {
    return np_fputc(character, stream);
}

ALWAYS_INLINE NP_DECL(int) fputs(const char *str, void *stream) {
    return np_fputs(str, stream);
}
ALWAYS_INLINE NP_DECL(int) _fputs(const char *str, void *stream) {
    return np_fputs(str, stream);
}

// Need to use a macro for this one due to varargs.
#define fprintf np_fprintf
#define _fprintf np_fprintf
#define vfprintf np_vfprintf
#define _vfprintf np_vfprintf


ALWAYS_INLINE NP_DECL(size_t) fwrite(const void *ptr, size_t size, size_t count, void *stream) {
    return np_fwrite(ptr, size, count, stream);
}
ALWAYS_INLINE NP_DECL(size_t) _fwrite(const void *ptr, size_t size, size_t count, void *stream) {
    return np_fwrite(ptr, size, count, stream);
}

/* File Buffering */
ALWAYS_INLINE NP_DECL(void) setbuf(void *stream, char *buffer) {
    np_setbuf(stream, buffer);
}
ALWAYS_INLINE NP_DECL(void) _setbuf(void *stream, char *buffer) {
    np_setbuf(stream, buffer);
}
ALWAYS_INLINE NP_DECL(int) setvbuf(void *stream, char *buffer, int mode, size_t size) {
    return np_setvbuf(stream, buffer, mode, size);
}
ALWAYS_INLINE NP_DECL(int) _setvbuf(void *stream, char *buffer, int mode, size_t size) {
    return np_setvbuf(stream, buffer, mode, size);
}

/* File Positioning */
ALWAYS_INLINE NP_DECL(int) fseek(void *stream, long int offset, int origin) {
    return np_fseek(stream, offset, origin);
}
ALWAYS_INLINE NP_DECL(int) _fseek(void *stream, long int offset, int origin) {
    return np_fseek(stream, offset, origin);
}

ALWAYS_INLINE NP_DECL(long) ftell(void *stream) {
    return np_ftell(stream);
}
ALWAYS_INLINE NP_DECL(long) _ftell(void *stream) {
    return np_ftell(stream);
}
#ifdef _WIN32
ALWAYS_INLINE NP_DECL(int) _fseeki64(void *stream, int64_t offset, int origin) {
    return np_fseeko64(stream, offset, origin);
}
ALWAYS_INLINE NP_DECL(int64_t) _ftelli64(void *stream) {
    return np_ftello64(stream);
}
#else
#if defined _FILE_OFFSET_BITS && _FILE_OFFSET_BITS == 64
ALWAYS_INLINE NP_DECL(int) fseeko(void *stream, int64_t offset, int origin) {
    return np_fseeko64(stream, offset, origin);
}
ALWAYS_INLINE NP_DECL(int64_t) ftello(void *stream) {
    return np_ftello64(stream);
}
#else
ALWAYS_INLINE NP_DECL(int) fseeko(void *stream, long int offset, int origin) {
    return np_fseek(stream, offset, origin);
}
ALWAYS_INLINE NP_DECL(long int) ftello(void *stream) {
    return np_ftell(stream);
}
#endif
#endif

ALWAYS_INLINE NP_DECL(void) rewind(void *stream) {
    np_rewind(stream);
}
ALWAYS_INLINE NP_DECL(void) _rewind(void *stream) {
    np_rewind(stream);
}

ALWAYS_INLINE NP_DECL(int) fgetpos(void *e, fpos_t* pos) {
    return np_fgetpos(e, pos);
}
ALWAYS_INLINE NP_DECL(int) _fgetpos(void *e, fpos_t* pos) {
    return np_fgetpos(e, pos);
}

ALWAYS_INLINE NP_DECL(int) fsetpos(void *e, fpos_t* pos) {
    return np_fsetpos(e, pos);
}
ALWAYS_INLINE NP_DECL(int) _fsetpos(void *e, fpos_t* pos) {
    return np_fsetpos(e, pos);
}

/* Error Handling & Other Utilities */
ALWAYS_INLINE NP_DECL(void) clearerr(void *stream) {
    np_clearerr(stream);
}
ALWAYS_INLINE NP_DECL(void) _clearerr(void *stream) {
    np_clearerr(stream);
}

ALWAYS_INLINE NP_DECL(bool) feof(void *e) {
    return np_feof(e);
}
ALWAYS_INLINE NP_DECL(bool) _feof(void *e) {
    return np_feof(e);
}

ALWAYS_INLINE NP_DECL(int) ferror(void *stream) {
    return np_ferror(stream);
}
ALWAYS_INLINE NP_DECL(int) _ferror(void *stream) {
    return np_ferror(stream);
}

ALWAYS_INLINE NP_DECL(int) fileno(void *stream) {
    return np_fileno(stream);
}
ALWAYS_INLINE NP_DECL(int) _fileno(void *stream) {
    return np_fileno(stream);
}

ALWAYS_INLINE NP_DECL(int) fflush(void *stream) {
    return np_fflush(stream);
}
ALWAYS_INLINE NP_DECL(int) _fflush(void *stream) {
    return np_fflush(stream);
}

#ifndef _WIN32
/* Locking Functions */
ALWAYS_INLINE NP_DECL(void) flockfile(void *stream) {
    np_flockfile(stream);
}
ALWAYS_INLINE NP_DECL(void) funlockfile(void *stream) {
    np_funlockfile(stream);
}
ALWAYS_INLINE NP_DECL(int) ftrylockfile(void *stream) {
    return np_ftrylockfile(stream);
}
#endif

#endif

#ifdef __cplusplus
}
#endif

#endif
#endif
