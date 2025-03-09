/*
# np_embed
# Inspired by c-embed by nicholas mcdonald 2022
*/

#ifndef NUITKAEMBED
#define NUITKAEMBED

#ifndef __ASSEMBLER__
#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdbool.h>
#include <string.h>
#include <libgen.h>
#include <fcntl.h>

#ifdef _WIN32
    #include <windows.h>
    #define PATH_MAX MAX_PATH
#else
    #include <limits.h>
    #include <unistd.h>
#endif

typedef fpos_t epos_t;

struct EMAP_S {     // Map Indexing Struct
  u_int32_t hash;
  u_int32_t parentidx;
  u_int32_t pathpos;
  u_int32_t pathsize;
  u_int8_t type;
  u_int32_t pos;
  u_int32_t size;
};
typedef struct EMAP_S EMAP;

struct EFILE_S {    // Virtual File Stream
  u_int32_t handle_type;
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

void np_normalize_path(char *path);
void np_get_absolute_path(const char *relative_path, char *absolute_path, size_t size);
EFILE* efopen_native_fallback(const char* file, const char* mode);
EFILE* efopen(const char* file, const char* mode);
int eopen(const char *pathname, int flags, ...
    /* mode_t mode */ );
void efclose(EFILE* e);
int eclose(int fd);
bool eeof(EFILE* e);
size_t efread(void* ptr, size_t size, size_t count, EFILE* stream);
ssize_t eread(int fd, void *buf, size_t count);
ssize_t epread(int fd, void *buf, size_t count, off_t offset);
int egetpos(EFILE* e, epos_t* pos);
/* File Opening and Closing */
EFILE *ereopen(const char *filename, const char *mode, EFILE *stream);
EFILE *efdopen(int fd, const char *mode);

/* File Input Functions */
int egetc(EFILE *stream);
char *egets(char *str, int n, EFILE *stream);
int escanf(EFILE *stream, const char *format, ...);
size_t efread(void *ptr, size_t size, size_t count, EFILE *stream);
int egetchar(void);
int egetc_unlocked(EFILE *stream);  /* Unlocked version of egetc */

/* File Output Functions */
int eputc(int character, EFILE *stream);
int eputs(const char *str, EFILE *stream);
int eprintf(EFILE *stream, const char *format, ...);
size_t ewrite(const void *ptr, size_t size, size_t count, EFILE *stream);

/* File Buffering */
void esetbuf(EFILE *stream, char *buffer);
int esetvbuf(EFILE *stream, char *buffer, int mode, size_t size);

/* File Positioning */
int eseek(EFILE *stream, long int offset, int origin);
long int etell(EFILE *stream);
void erewind(EFILE *stream);
int egetpos(EFILE *stream, fpos_t *pos);
int esetpos(EFILE *stream, const fpos_t *pos);
int eungetc(int character, EFILE *stream);

/* Error Handling & Other Utilities */
void eclearerr(EFILE *stream);
int eerror(EFILE *stream);
int efileno(EFILE *stream);
int eflush(EFILE *stream);

/* Locking Functions */
void elockfile(EFILE *e);
void eunlockfile(EFILE *e);
int etrylockfile(EFILE *e);

#ifndef NUITKAPYTHON_EMBED_BUILD
// Preprocessor Translation
#define FILE EFILE
/* File Opening and Closing */
#define fopen efopen
#define open eopen
#define fdopen efdopen
#define freopen ereopen
#define fclose efclose
#define close eclose


/* File Input Functions */
#define fgets egets
#define fgetc egetc
#define fscanf escanf
#define fread efread
#define read eread
#define pread epread
#define getc egetc
#define getc_unlocked egetc_unlocked
#define ungetc eungetc

/* File Output Functions */
#define fputc eputc
#define fputs eputs
#define fprintf eprintf
#define fwrite ewrite
#define putc eputc

/* File Buffering */
#define setbuf esetbuf
#define setvbuf esetvbuf

/* File Positioning */
#define fseek eseek
#define ftell etell
#define rewind erewind
#define fgetpos egetpos
#define fsetpos esetpos

/* Error Handling & Other Utilities */
#define clearerr eclearerr
#define feof eeof
#define ferror eerror
#define fileno efileno
#define fflush eflush

/* Locking Functions */
#define flockfile elockfile
#define funlockfile eunlockfile
#define ftrylockfile etrylockfile

#endif

#ifdef __cplusplus
}
#endif

#endif
#endif
