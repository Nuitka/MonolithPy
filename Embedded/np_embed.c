#define NUITKAPYTHON_EMBED_BUILD
#define _FILE_OFFSET_BITS 64
#include "np_embed.h"
#include <ctype.h>
#include <stdarg.h>

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

#ifdef _WIN32
#define NP_FOREIGN_PTR *(void**)e == NULL || (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE)
#else
#define NP_FOREIGN_PTR ((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE
#endif

uint32_t hash(char * key){   // Hash Function: MurmurOAAT64
  uint32_t h = 3323198485ul;
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

static bool get_executable_path(char *execfolder, char *executable) {
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
  size_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len == -1) return false;
  path[len] = '\0';
  strncpy(executable, path, PATH_MAX);
  strncpy(execfolder, dirname(path), PATH_MAX);
#endif

  return true;
}

static bool find_embedded_file(const char *search_path, EMAP **found_map) {
  EMAP *map = (EMAP*)(&nuitka_embed_map);
  const char *end = &nuitka_embed_map + nuitka_embed_map_len;
  uint32_t key = hash((char*)search_path);

  while ((char*)map < end) {
    if (map->type == ETYPE_FILE && map->hash == key && strcmp(&nuitka_embed_data + map->pathpos, search_path) == 0) {
      *found_map = map;
      return true;
    }
    map++;
  }
  return false;
}

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

NP_DECL(EFILE*) np_fopen(const char* file, const char* mode) {
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      EFILE *e = (EFILE *) malloc(sizeof *e);
      e->handle_type = EHANDLE_VIRTUAL;
      e->name = &nuitka_embed_data + map->pathpos;
      e->pos = &nuitka_embed_data + map->pos;
      e->end = &nuitka_embed_data + map->pos + map->size;
      e->size = map->size;
      return e;
    }
  }

  FILE* f = fopen(file, mode);
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)malloc(sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
}

NP_DECL(int) np_open(const char *file, int flags, ...) {
#ifdef _WIN32
  int mode = 0;
#else
  mode_t mode = 0;
#endif
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
#ifdef _WIN32
    mode = va_arg(args, int);
#else
    mode = va_arg(args, mode_t);
#endif
    va_end(args);
  }

  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    return open(file, flags, mode);
  }
  char search_path[PATH_MAX] = {};
  get_virtual_path(search_path, execfolder, absolute_path);

  EMAP *map;
  if (find_embedded_file(search_path, &map)) {
    EFILE *e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = &nuitka_embed_data + map->pathpos;
    e->pos = &nuitka_embed_data + map->pos;
    e->end = &nuitka_embed_data + map->pos + map->size;
    e->size = map->size;
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == 0) {
        np_fd2file[i].fd = fakefd;
        np_fd2file[i].f = e;
        break;
      }
    }
    return fakefd;
  }
  return open(file, flags, mode);
}

#ifdef _WIN32
NP_DECL(EFILE*) np_wfopen(const wchar_t *wfile, const wchar_t *mode) {
  char file[PATH_MAX] = {};
  wcstombs(file, wfile, PATH_MAX);

  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      EFILE *e = (EFILE *) malloc(sizeof *e);
      e->handle_type = EHANDLE_VIRTUAL;
      e->name = &nuitka_embed_data + map->pathpos;
      e->pos = &nuitka_embed_data + map->pos;
      e->end = &nuitka_embed_data + map->pos + map->size;
      e->size = map->size;
      return e;
    }
  }

  FILE* f = _wfopen(wfile, mode);
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)malloc(sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
}

NP_DECL(int) np_wopen(const wchar_t *wfile, int flags, ...) {
  int mode = 0;
  if (flags & O_CREAT) {
    va_list args;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
  }

  char file[PATH_MAX] = {};
  wcstombs((char*)&file, wfile, PATH_MAX);

  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    return _wopen(wfile, flags, mode);
  }
  char search_path[PATH_MAX] = {};
  get_virtual_path(search_path, execfolder, absolute_path);

  EMAP *map;
  if (find_embedded_file(search_path, &map)) {
    EFILE* e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = (&nuitka_embed_data + map->pathpos);
    e->pos = (&nuitka_embed_data + map->pos);
    e->end = (&nuitka_embed_data + map->pos + map->size);
    e->size = map->size;
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == 0) {
        np_fd2file[i].fd = fakefd;
        np_fd2file[i].f = e;
        break;
      }
    }
    return fakefd;
  }

  return _wopen(wfile, flags, mode);
}
#endif

NP_DECL(int) np_fclose(void* e) {
  if (NP_FOREIGN_PTR) {
    return fclose((FILE*)e);
  }
  if (((EFILE*)e)->handle_type == EHANDLE_NATIVE) {
    fclose(((EFILE*)e)->f);
  }

  free(e);
  e = NULL;
  return 0;
}

NP_DECL(int) np_close(int fd) {
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

NP_DECL(bool) np_feof(void* e) {
  if (NP_FOREIGN_PTR) {
    return feof((FILE*)e);
  }
  if(e == NULL){
    errno = EINVAL;
    return true;
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return feof(((EFILE*)e)->f);
  }

  if(((EFILE*)e)->end < ((EFILE*)e)->pos){
    errno = EINVAL;
    return true;
  }
  if((((EFILE*)e)->end - ((EFILE*)e)->pos) - ((EFILE*)e)->size < 0){
    errno = EINVAL;
    return true;
  }
  return (((EFILE*)e)->end == ((EFILE*)e)->pos);
}

NP_DECL(size_t) np_fread(void* ptr, size_t size, size_t count, void* e) {
  if (NP_FOREIGN_PTR) {
    return fread(ptr, size, count, (FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fread(ptr, size, count, ((EFILE*)e)->f);
  }

  if(((EFILE*)e)->end - ((EFILE*)e)->pos < size*count){
    size_t scount = ((EFILE*)e)->end - ((EFILE*)e)->pos;
    memcpy(ptr, (void*)((EFILE*)e)->pos, scount);
    ((EFILE*)e)->pos = ((EFILE*)e)->end;
    return (scount/size);
  }

  memcpy(ptr, (void*)((EFILE*)e)->pos, size*count);
  ((EFILE*)e)->pos = (char*)((EFILE*)e)->pos + size*count;
  return count;
}

#ifdef _WIN32
NP_DECL(int) np_read(int fd, void *buf, unsigned int count) {
#else
NP_DECL(ssize_t) np_read(int fd, void *buf, size_t count) {
#endif
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
  e->pos = (char*)e->pos + count;
  return count;
}

NP_DECL(ssize_t) np_pread(int fd, void *buf, size_t count, off_t offset) {
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

NP_DECL(int) np_fgetpos(void* e, epos_t* pos) {
  if (NP_FOREIGN_PTR) {
    return fgetpos((FILE*)e, pos);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgetpos(((EFILE*)e)->f, pos);
  }

  if(((EFILE*)e)->end <= ((EFILE*)e)->pos){
    pos = NULL;
    return 1;
  }

  *pos = (epos_t)(((EFILE*)e)->end - ((EFILE*)e)->pos);
  return 0;

}

NP_DECL(char*) np_fgets(char* str, int num, void* e ) {
  if (NP_FOREIGN_PTR) {
    return fgets(str, num, (FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgets(str, num, ((EFILE*)e)->f);
  }

  //if num 0 or 1 e->pos will not advance
  if (num <= 1) return NULL;
  if (np_feof(e)) return NULL;

  int i = 0;

  while (1)
  {
    //i < (num - 1) so still room for two characters: \n and \0
    if ((str[i++] = *(((EFILE*)e)->pos++)) == '\n') break;
    if (np_feof(e) || (i == (num - 1))) break;
  }
  str[i] = '\0';
  return str;
}

NP_DECL(int) np_fgetc(void* e) {
  if (NP_FOREIGN_PTR) {
    return fgetc((FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgetc(((EFILE*)e)->f);
  }

  if(np_feof(e))
    return -1;
  return (int)(*(((EFILE*)e)->pos++));
}

#ifndef _WIN32
NP_DECL(int) np_getc_unlocked(void* e) {
  if (NP_FOREIGN_PTR) {
    return getc_unlocked((FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return getc_unlocked(((EFILE*)e)->f);
  }

  return np_fgetc(e);
}
#endif


int64_t np_ftell_priv(void* e) {
  if (NP_FOREIGN_PTR) {
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

  return (((EFILE*)e)->end - ((EFILE*)e)->pos) - ((EFILE*)e)->size;
}

int np_fseek_priv(void* e, int64_t offset, int origin) {
  if (NP_FOREIGN_PTR) {
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

  if(origin == SEEK_SET)
    ((EFILE*)e)->pos = ((EFILE*)e)->end - ((EFILE*)e)->size + offset;
  if(origin == SEEK_CUR)
    ((EFILE*)e)->pos += offset;
  if(origin == SEEK_END)
    ((EFILE*)e)->pos = ((EFILE*)e)->end + offset;

  if(((EFILE*)e)->end < ((EFILE*)e)->pos || np_ftell(e) < 0) {
    errno = EINVAL;
    return -1;
  }

  return 0;
}

NP_DECL(long int) np_ftell(void* e) {
  return np_ftell_priv(e);
}

NP_DECL(int) np_fseek(void* e, long int offset, int origin) {
  return np_fseek_priv(e, offset, origin);
}

NP_DECL(int64_t) np_ftello64(void *e) {
  return np_ftell_priv(e);
}

NP_DECL(int) np_fseeko64(void *e, int64_t offset, int origin) {
  return np_fseek_priv(e, offset, origin);
}

NP_DECL(int) np_fscanf(void *e, const char *format, ...) {
  if (NP_FOREIGN_PTR) {
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

NP_DECL(int) np_fputc(int character, void *e) {
  if (NP_FOREIGN_PTR) {
    return fputc(character, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputc(character, ((EFILE*)e)->f);
  }
  return 0;
}

NP_DECL(int) np_fputs(const char *str, void *e) {
  if (NP_FOREIGN_PTR) {
    return fputs(str, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputs(str, ((EFILE*)e)->f);
  }
  return 0;
}

NP_DECL(int) np_fprintf(void *e, const char *format, ...) {
  if (NP_FOREIGN_PTR) {
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
  return 0;
}

NP_DECL(size_t) np_fwrite(const void *ptr, size_t size, size_t count, void *e) {
  if (NP_FOREIGN_PTR) {
    return fwrite(ptr, size, count, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fwrite(ptr, size, count, ((EFILE*)e)->f);
  }
  return 0;
}

NP_DECL(void) np_setbuf(void *e, char *buffer) {
  if (NP_FOREIGN_PTR) {
    setbuf((FILE*)e, buffer);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    setbuf(((EFILE*)e)->f, buffer);
  }
}

NP_DECL(int) np_setvbuf(void *e, char *buffer, int mode, size_t size) {
  if (NP_FOREIGN_PTR) {
    return setvbuf((FILE*)e, buffer, mode, size);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return setvbuf(((EFILE*)e)->f, buffer, mode, size);
  }
  return 0;
}

NP_DECL(void) np_rewind(void *e) {
  if (NP_FOREIGN_PTR) {
    rewind((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    rewind(((EFILE*)e)->f);
  }
}

NP_DECL(int) np_fsetpos(void *e, const fpos_t *pos) {
  if (NP_FOREIGN_PTR) {
    return fsetpos((FILE*)e, pos);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fsetpos(((EFILE*)e)->f, pos);
  }
  return 0;
}

NP_DECL(void) np_clearerr(void *e) {
  if (NP_FOREIGN_PTR) {
    clearerr((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    clearerr(((EFILE*)e)->f);
  }
}

NP_DECL(int) np_ferror(void *e) {
  if (NP_FOREIGN_PTR) {
    return ferror((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ferror(((EFILE*)e)->f);
  }
  return 0;
}

NP_DECL(int) np_fileno(void *e) {
  if (NP_FOREIGN_PTR) {
    return fileno((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fileno(((EFILE*)e)->f);
  }
  return 0;
}

NP_DECL(int) np_fflush(void *e) {
  if (NP_FOREIGN_PTR) {
    return fflush((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fflush(((EFILE*)e)->f);
  }
  return 0;
}

#ifndef _WIN32
NP_DECL(void) np_flockfile(void *e) {
  if (NP_FOREIGN_PTR) {
    flockfile((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    flockfile(((EFILE*)e)->f);
  }
}

NP_DECL(void) np_funlockfile(void *e) {
  if (NP_FOREIGN_PTR) {
    funlockfile((FILE*)e);
    return;
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    funlockfile(((EFILE*)e)->f);
  }
}

NP_DECL(int) np_ftrylockfile(void *e) {
  if (NP_FOREIGN_PTR) {
    return ftrylockfile((FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ftrylockfile(((EFILE*)e)->f);
  }
  return 0;
}
#endif

NP_DECL(int) np_ungetc(int character, void *e) {
  if (NP_FOREIGN_PTR) {
    return ungetc(character, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return ungetc(character, ((EFILE*)e)->f);
  }
  return character;
}

NP_DECL(EFILE*) np_fdopen(int fd, const char *mode) {
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
