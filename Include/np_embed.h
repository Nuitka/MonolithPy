/*
# np_embed
# Inspired by c-embed by nicholas mcdonald 2022
*/

#ifndef NUITKAEMBED
#define NUITKAEMBED

#ifndef __ASSEMBLER__
#ifdef __cplusplus
#if __has_include (<QtCore>)
#  include <QtCore>
#endif

extern "C" {
#endif

#ifdef _WIN32
#define _CRTIMP
#include <WinSock2.h>
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #define PATH_MAX MAX_PATH
    typedef SSIZE_T ssize_t;
#else
    #include <limits.h>
    #include <unistd.h>
    #include <libgen.h>
#endif

typedef fpos_t epos_t;

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

NP_DECL(EFILE*) np_fopen(const char* file, const char* mode);
NP_DECL(int) np_open(const char *pathname, int flags, ...
    /* mode_t mode */ );
#ifdef _WIN32
NP_DECL(EFILE*) np_wfopen(const wchar_t *wfile, const wchar_t *mode);
NP_DECL(int) np_wopen(const wchar_t *pathname, int flags, ...
        /* mode_t mode */ );
#endif
NP_DECL(int) np_fclose(void* e);
NP_DECL(int) np_close(int fd);
NP_DECL(bool) np_feof(void* e);
NP_DECL(size_t) np_fread(void* ptr, size_t size, size_t count, void* stream);
#ifdef _WIN32
NP_DECL(int) np_read(int fd, void *buf, unsigned int count);
#else
NP_DECL(ssize_t) np_read(int fd, void *buf, size_t count);
#endif
NP_DECL(ssize_t) np_pread(int fd, void *buf, size_t count, off_t offset);
NP_DECL(int) np_fgetpos(void* e, epos_t* pos);
/* File Opening and Closing */
NP_DECL(EFILE*) np_freopen(const char *filename, const char *mode, EFILE *stream);
NP_DECL(EFILE*) np_fdopen(int fd, const char *mode);

/* File Input Functions */
NP_DECL(int) np_fgetc(void *stream);
NP_DECL(char*) np_fgets(char *str, int n, void *stream);
NP_DECL(int) np_fscanf(void *stream, const char *format, ...);
NP_DECL(int) np_getc_unlocked(void *stream);  /* Unlocked version of egetc */

/* File Output Functions */
NP_DECL(int) np_fputc(int character, void *stream);
NP_DECL(int) np_fputs(const char *str, void *stream);
NP_DECL(int) np_fprintf(void *stream, const char *format, ...);
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

#ifndef NUITKAPYTHON_EMBED_BUILD

// Preprocessor Translation
#define FILE EFILE
/* File Opening and Closing */
#define fopen np_fopen
#define _fopen np_fopen
#define _open np_open
#define fdopen np_fdopen
#define _fdopen np_fdopen
#define freopen np_freopen
#define _freopen np_freopen
#define fclose np_fclose
#define _fclose np_fclose
#define _close np_close
#ifdef _WIN32
#define wopen np_wopen
#define _wopen np_wopen
#define wfopen np_wfopen
#define _wfopen np_wfopen
#endif

/* File Input Functions */
#define fgets np_fgets
#define _fgets np_fgets
#define fgetc np_fgetc
#define _fgetc np_fgetc
#define fscanf np_fscanf
#define _fscanf np_fscanf
#define fread np_fread
#define _fread np_fread
#define read np_read
#define _read np_read
#ifndef _WIN32
#define pread np_pread
#define getc_unlocked np_getc_unlocked
#endif
#define getc np_fgetc
#define ungetc np_ungetc
#define _ungetc np_ungetc

/* File Output Functions */
#define fputc np_fputc
#define _fputc np_fputc
#define fputs np_fputs
#define _fputs np_fputs
#define fprintf np_fprintf
#define _fprintf np_fprintf
#define fwrite np_fwrite
#define _fwrite np_fwrite
#define putc np_fputc
#define _putc np_fputc

/* File Buffering */
#define setbuf np_setbuf
#define _setbuf np_setbuf
#define setvbuf np_setvbuf
#define _setvbuf np_setvbuf

/* File Positioning */
#define fseek np_fseek
#define _fseek np_fseek
#define ftell np_ftell
#define _ftell np_ftell
#ifdef _WIN32
#define _fseeki64 np_fseeko64
#define _ftelli64 np_ftello64
#else
#if defined _FILE_OFFSET_BITS && _FILE_OFFSET_BITS == 64
#define fseeko np_fseeko64
#define ftello np_ftello64
#else
#define fseeko np_fseek
#define ftello np_ftell
#endif
#endif
#define rewind np_rewind
#define _rewind np_rewind
#define fgetpos np_fgetpos
#define _fgetpos np_fgetpos
#define fsetpos np_fsetpos
#define _fsetpos np_fsetpos

/* Error Handling & Other Utilities */
#define clearerr np_clearerr
#define _clearerr np_clearerr
#define feof np_feof
#define _feof np_feof
#define ferror np_ferror
#define _ferror np_ferror
#define fileno np_fileno
#define _fileno np_fileno
#define fflush np_fflush
#define _fflush np_fflush

#ifndef _WIN32
/* Locking Functions */
#define flockfile np_flockfile
#define funlockfile np_funlockfile
#define ftrylockfile np_ftrylockfile
#endif

#endif

#ifdef __cplusplus
}
#endif

#endif
#endif
