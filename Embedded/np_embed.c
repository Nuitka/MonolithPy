#define NUITKAPYTHON_EMBED_BUILD

#include <stdarg.h>
#include "np_embed.h"

extern const char nuitka_embed_map;
extern const long nuitka_embed_map_len;

extern const char nuitka_embed_data;
extern const long nuitka_embed_data_len;

struct FDMAP_S {    // Virtual File Stream
    int fd;
    EFILE *f;
  };
typedef struct FDMAP_S FDMAP;

FDMAP np_fd2file[512] = {};

u_int32_t hash(char * key){   // Hash Function: MurmurOAAT64
  u_int32_t h = 3323198485ul;
  for (;*key;++key) {
    h ^= *key;
    h *= 0x5bd1e995;
    h ^= h >> 15;
  }
  return h;
}

// Normalize path by resolving ".." and "."
void np_normalize_path(char *path) {
  char resolved[PATH_MAX];
  char *tokens[PATH_MAX];  // Array to store path segments
  int depth = 0;

  // Tokenize by "/"
  char *token = strtok(path, "/");
  while (token) {
    if (strcmp(token, "..") == 0) {
      if (depth > 0) depth--;  // Go up a directory (if possible)
    } else if (strcmp(token, ".") != 0) {
      tokens[depth++] = token;  // Add to valid path segments
    }
    token = strtok(NULL, "/");
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

// Cross-platform function to get absolute path without requiring the path to exist
void np_get_absolute_path(const char *relative_path, char *absolute_path, size_t size) {
#ifdef _WIN32
  // Windows: _fullpath resolves ".." and "." even if path does not exist
  if (_fullpath(absolute_path, relative_path, size) == NULL) {
      perror("_fullpath failed");
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
  np_normalize_path(absolute_path);  // Normalize ".." and "."
#endif
}

EFILE* efopen_native_fallback(const char* file, const char* mode) {
  FILE* f = fopen(file, mode);
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)malloc(sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
}

EFILE* efopen(const char* file, const char* mode) {
  // We need to start by translating the given path to a virtual path.
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, (char*)&absolute_path, PATH_MAX);
  size_t absolute_path_len = strlen((char*)&absolute_path);

  // Next, we need to special-case paths relative to the executable.
  //So, lets get the path of our executable.+
  char execfolder[PATH_MAX] = {};
#ifdef _WIN32
  if (GetModuleFileName(NULL, execfolder, (DWORD)PATH_MAX) == 0) {
    return efopen_native_fallback(file, mode);
  }
  // Remove the executable name, leaving only the directory
  if (!PathRemoveFileSpec(execfolder)) {
    return efopen_native_fallback(file, mode);
  }
#elif defined(__APPLE__)
  char path[PATH_MAX];
  uint32_t bufsize = PATH_MAX;
  // _NSGetExecutablePath returns a path that may contain symlinks
  if (_NSGetExecutablePath(path, &bufsize) != 0) {
    return efopen_native_fallback(file, mode);
  }
  // Resolve any symlinks to get the canonical path
  char resolved_path[PATH_MAX];
  if (realpath(path, resolved_path) == NULL) {
    return efopen_native_fallback(file, mode);
  }
  // dirname() may modify its argument; use it to extract the directory
  strncpy(execfolder, dirname(resolved_path), PATH_MAX);
#else
  char path[PATH_MAX];
  // On Linux, read the symlink /proc/self/exe to get the executable path
  size_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len == -1) {
    return efopen_native_fallback(file, mode);
  }
  path[len] = '\0'; // Null-terminate the path
  strncpy(execfolder, dirname(path), PATH_MAX);
#endif
  size_t execfolder_len = strlen(execfolder);

  char search_path[PATH_MAX] = {};

  if (strncmp(absolute_path, execfolder, execfolder_len) == 0) {
    // We are trying to find a file next to the exe.
    search_path[0] = '~';
    int pos = 0;
    for (size_t i = execfolder_len; i < absolute_path_len; i++) {
      pos = i - execfolder_len + 1;
      if (absolute_path[i] == '\\') {
        search_path[pos] = '/';
      } else {
        search_path[pos] = absolute_path[i];
      }
    }
    search_path[pos + 1] = '\0';
  } else {
#ifdef _WIN32
    int pos = 2;
    // We convert into POSIX style paths on windows.
    search_path[0] = '/';
    search_path[1] = absolute_path[0];
    for (size_t i = 2; i < absolute_path_len; i++) {
#else
    int pos = 0;
    for (size_t i = 0; i < absolute_path_len; i++) {
#endif
      if (absolute_path[i] == '\\') {
        search_path[pos] = '/';
      } else {
        search_path[pos] = absolute_path[i];
      }
      pos++;
    }
    search_path[pos] = '\0';
  }

  EMAP* map = (EMAP*)(&nuitka_embed_map);
  const char* end = &nuitka_embed_map + nuitka_embed_map_len;

  if ( map == NULL || end == NULL ) {
    return efopen_native_fallback(file, mode);
  }

  const u_int32_t key = hash((char*)search_path);
  bool found = false;
  while ( (char*)map < end ) {
    if (map->type == ETYPE_FILE && map->hash == key && strcmp(&nuitka_embed_data + map->pathpos, search_path) == 0) {
      found = true;
      break;
    }
    map++;
  }

  if (found) {
    EFILE* e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = (&nuitka_embed_data + map->pathpos);
    e->pos = (&nuitka_embed_data + map->pos);
    e->end = (&nuitka_embed_data + map->pos + map->size);
    e->size = map->size;
    return e;
  }

  return efopen_native_fallback(file, mode);
}

void efclose(EFILE* e) {
  if (e->handle_type != EHANDLE_VIRTUAL) {
    fclose(e->f);
  }

  free(e);
  e = NULL;
}

int eopen(const char *file, int flags, ...) {
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, mode_t);
    va_end(args);
  }

  // We need to start by translating the given path to a virtual path.
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, (char*)&absolute_path, PATH_MAX);
  size_t absolute_path_len = strlen((char*)&absolute_path);

  // Next, we need to special-case paths relative to the executable.
  //So, lets get the path of our executable.

  char executable[PATH_MAX];
  char execfolder[PATH_MAX] = {};
#ifdef _WIN32
  if (GetModuleFileName(NULL, executable, (DWORD)PATH_MAX) == 0) {
    return open(file, flags, mode);
  }
  if (GetModuleFileName(NULL, execfolder, (DWORD)PATH_MAX) == 0) {
    return open(file, flags, mode);
  }
  // Remove the executable name, leaving only the directory
  if (!PathRemoveFileSpec(execfolder)) {
    return open(file, flags, mode);
  }
#elif defined(__APPLE__)
  char path[PATH_MAX];
  uint32_t bufsize = PATH_MAX;
  // _NSGetExecutablePath returns a path that may contain symlinks
  if (_NSGetExecutablePath(path, &bufsize) != 0) {
    return open(file, flags, mode);
  }
  // Resolve any symlinks to get the canonical path
  char resolved_path[PATH_MAX];
  if (realpath(path, resolved_path) == NULL) {
    return open(file, flags, mode);
  }
  strncpy(executable, resolved_path, PATH_MAX);
  // dirname() may modify its argument; use it to extract the directory
  strncpy(execfolder, dirname(resolved_path), PATH_MAX);
#else
  char path[PATH_MAX];
  // On Linux, read the symlink /proc/self/exe to get the executable path
  size_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len == -1) {
    return open(file, flags, mode);
  }
  path[len] = '\0'; // Null-terminate the path
  strncpy(executable, path, PATH_MAX);
  strncpy(execfolder, dirname(path), PATH_MAX);
#endif
  size_t execfolder_len = strlen(execfolder);

  char search_path[PATH_MAX] = {};

  if (strncmp(absolute_path, execfolder, execfolder_len) == 0) {
    // We are trying to find a file next to the exe.
    search_path[0] = '~';
    int pos = 0;
    for (size_t i = execfolder_len; i < absolute_path_len; i++) {
      pos = i - execfolder_len + 1;
      if (absolute_path[i] == '\\') {
        search_path[pos] = '/';
      } else {
        search_path[pos] = absolute_path[i];
      }
    }
    search_path[pos + 1] = '\0';
  } else {
#ifdef _WIN32
    int pos = 2;
    // We convert into POSIX style paths on windows.
    search_path[0] = '/';
    search_path[1] = absolute_path[0];
    for (size_t i = 2; i < absolute_path_len; i++) {
#else
    int pos = 0;
    for (size_t i = 0; i < absolute_path_len; i++) {
#endif
      if (absolute_path[i] == '\\') {
        search_path[pos] = '/';
      } else {
        search_path[pos] = absolute_path[i];
      }
      pos++;
    }
    search_path[pos] = '\0';
  }

  EMAP* map = (EMAP*)(&nuitka_embed_map);
  const char* end = &nuitka_embed_map + nuitka_embed_map_len;

  if ( map == NULL || end == NULL ) {
    return open(file, flags, mode);
  }

  const u_int32_t key = hash((char*)search_path);
  bool found = false;
  while ( (char*)map < end ) {
    if (map->type == ETYPE_FILE && map->hash == key && strcmp(&nuitka_embed_data + map->pathpos, search_path) == 0) {
      found = true;
      break;
    }
    map++;
  }

  if (found) {
    EFILE* e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = (&nuitka_embed_data + map->pathpos);
    e->pos = (&nuitka_embed_data + map->pos);
    e->end = (&nuitka_embed_data + map->pos + map->size);
    e->size = map->size;
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
        if (np_fd2file[i].fd == 0) {
            // This one is free.
            np_fd2file[i].fd = fakefd;
            np_fd2file[i].f = e;
            break;
        }
    }
    return fakefd;
  }

  return open(file, flags, mode);
}

int eclose(int fd) {
  EFILE *e = NULL;
  int fd_idx = -1;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      fd_idx = -1;
      break;
    }
  }
  if (e == NULL) {
    return close(fd);
  }
  if (e->handle_type != EHANDLE_VIRTUAL) {
    fclose(e->f);
    return 0;
  }
  
  free(e);
  e = NULL;
  return 0;
}

bool eeof(EFILE* e) {
  if(e == NULL){
    errno = EINVAL;
    return true;
  }

  if (e->handle_type != EHANDLE_VIRTUAL) {
    return feof(e->f);
  }

  if(e->end < e->pos){
    errno = EINVAL;
    return true;
  }
  if((e->end - e->pos) - e->size < 0){
    errno = EINVAL;
    return true;
  }
  return (e->end == e->pos);
}

size_t efread(void* ptr, size_t size, size_t count, EFILE* stream) {

  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fread(ptr, size, count, stream->f);
  }

  if(stream->end - stream->pos < size*count){
    size_t scount = stream->end - stream->pos;
    memcpy(ptr, (void*)stream->pos, scount);
    stream->pos = stream->end;
    return (scount/size);
  }

  memcpy(ptr, (void*)stream->pos, size*count);
  stream->pos = (void*)stream->pos + size*count;
  return count;
}

ssize_t eread(int fd, void *buf, size_t count) {
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
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
  e->pos = (void*)e->pos + count;
  return count;
}

ssize_t epread(int fd, void *buf, size_t count, off_t offset) {
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
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

int egetpos(EFILE* e, epos_t* pos) {

  if (e->handle_type != EHANDLE_VIRTUAL) {
    return fgetpos(e->f, pos);
  }

  if(e->end <= e->pos){
    pos = NULL;
    return 1;
  }

  *pos = (epos_t)(e->end - e->pos);
  return 0;

}

char* egets(char* str, int num, EFILE* stream ) {

  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fgets(str, num, stream->f);
  }

  //if num 0 or 1 stream->pos will not advance
  if (num <= 1) return NULL;
  if (eeof(stream)) return NULL;

  int i = 0;

  while (1)
  {
    //i < (num - 1) so still room for two characters: \n and \0
    if ((str[i++] = *(stream->pos++)) == '\n') break;
    if (eeof(stream) || (i == (num - 1))) break;
  }
  str[i] = '\0';
  return str;
}

int egetc ( EFILE* stream ) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fgetc(stream->f);
  }

  if(eeof(stream))
    return -1;
  return (int)(*(stream->pos++));
}

int egetc_unlocked ( EFILE* e ) {
  if (e->handle_type != EHANDLE_VIRTUAL) {
    return getc_unlocked(e->f);
  }

  return egetc(e);
}

long int etell(EFILE* e) {
  if (e->handle_type != EHANDLE_VIRTUAL) {
    return ftell(e->f);
  }

  return (e->end - e->pos) - e->size;
}

int eseek(EFILE* stream, long int offset, int origin) {

  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fseek(stream->f, offset, origin);
  }

  if(origin == SEEK_SET)
    stream->pos = stream->end - stream->size + offset;
  if(origin == SEEK_CUR)
    stream->pos += offset;
  if(origin == SEEK_END)
    stream->pos = stream->end + offset;

  if(stream->end < stream->pos || etell(stream) < 0) {
    errno = EINVAL;
    return true;
  }

  return 0;
}

int escanf(EFILE *stream, const char *format, ...) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    va_list args;
    va_start(args, format);
    int result = vfscanf(stream->f, format, args);
    va_end(args);
    return result;
  }
  return 0;
}

/* File Output Functions */
int eputc(int character, EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fputc(character, stream->f);
  }
  return 0;
}

int eputs(const char *str, EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fputs(str, stream->f);
  }
  return 0;
}

int eprintf(EFILE *stream, const char *format, ...) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    va_list args;
    va_start(args, format);
    int result = vfprintf(stream->f, format, args);
    va_end(args);
    return result;
  }
  return 0;
}

size_t ewrite(const void *ptr, size_t size, size_t count, EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fwrite(ptr, size, count, stream->f);
  }
  return 0;
}

int eputchar(int character) {
  return putchar(character);
}

/* File Buffering */
void esetbuf(EFILE *stream, char *buffer) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    setbuf(stream->f, buffer);
  }
}

int esetvbuf(EFILE *stream, char *buffer, int mode, size_t size) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return setvbuf(stream->f, buffer, mode, size);
  }
  return 0;
}

/* File Positioning */
void erewind(EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    rewind(stream->f);
  }
}

int esetpos(EFILE *stream, const fpos_t *pos) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fsetpos(stream->f, pos);
  }
  return 0;
}

/* Error Handling & Other Utilities */
void eclearerr(EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    clearerr(stream->f);
  }
}

int eerror(EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return ferror(stream->f);
  }
  return 0;
}

int efileno(EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fileno(stream->f);
  }
  return 0;
}

int eflush(EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return fflush(stream->f);
  }
  return 0;
}

/* Locking Functions */
void elockfile(EFILE *e) {
  if (e->handle_type != EHANDLE_VIRTUAL) {
    flockfile(e->f);
  }
}

void eunlockfile(EFILE *e) {
  if (e->handle_type != EHANDLE_VIRTUAL) {
    funlockfile(e->f);
  }
}

int etrylockfile(EFILE *e) {
  if (e->handle_type != EHANDLE_VIRTUAL) {
    return ftrylockfile(e->f);
  }
  return 0;
}

int eungetc(int character, EFILE *stream) {
  if (stream->handle_type != EHANDLE_VIRTUAL) {
    return ungetc(character, stream->f);
  }
  return character;
}

EFILE *efdopen(int fd, const char *mode) {
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      return np_fd2file[i].f;
    }
  }

  // If this is not a virtual fd, fallback.
  EFILE* e = (EFILE*)malloc(sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = fdopen(fd, mode);
  return e;
}