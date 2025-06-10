#define NUITKAPYTHON_EMBED_BUILD
#define _FILE_OFFSET_BITS 64
#include "np_embed.h"
#define PATH_MAX 4096
#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#endif
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
    strcat_s(long_path_buffer, buffer_size, current_path);

    // Clean up the memory allocated by DecomposePath.
    FreePathComponents(components, component_count);

    return TRUE;
}
#endif

// Cross-platform function to get absolute path without requiring the path to exist
void np_get_absolute_path(const char *relative_path, char *absolute_path, size_t size) {
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
    if (map->hash == key && strcmp(&nuitka_embed_data + map->pathpos, search_path) == 0) {
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
    if (find_embedded_file(search_path, &map) && map->type == ETYPE_FILE) {
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

#ifdef _WIN32
NP_DECL(int) np_open(const char *file, int flags, int mode) {
#else
NP_DECL(int) np_open(const char *file, int flags, mode_t mode) {
#endif
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(file, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    return open(file, flags, mode);
  }
  char search_path[PATH_MAX] = {};
  get_virtual_path(search_path, execfolder, absolute_path);

  EMAP *map;
  if (find_embedded_file(search_path, &map) && map->type == ETYPE_FILE) {
    EFILE *e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = &nuitka_embed_data + map->pathpos;
    e->pos = &nuitka_embed_data + map->pos;
    e->end = &nuitka_embed_data + map->pos + map->size;
    e->size = map->size;
    e->map = map;
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
    if (find_embedded_file(search_path, &map) && map->type == ETYPE_FILE) {
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

NP_DECL(int) np_wopen(const wchar_t *wfile, int flags, int mode) {
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
  if (find_embedded_file(search_path, &map) && map->type == ETYPE_FILE) {
    EFILE* e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = (&nuitka_embed_data + map->pathpos);
    e->pos = (&nuitka_embed_data + map->pos);
    e->end = (&nuitka_embed_data + map->pos + map->size);
    e->size = map->size;
    e->map = map;
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

NP_DECL(EFILE*) np_freopen(const char *filename, const char *mode, void *e) {
  if (NP_FOREIGN_PTR) {
    return freopen(filename, mode, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type == EHANDLE_NATIVE) {
    return freopen(filename, mode, ((EFILE*)e)->f);
  }

  return NULL;
}

NP_DECL(EFILE*) np_tmpfile() {
  FILE* f = tmpfile();
  if (f == NULL)
    return NULL;

  EFILE* e = (EFILE*)malloc(sizeof *e);
  e->handle_type = EHANDLE_NATIVE;
  e->f = f;

  return e;
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

NP_DECL(int) np_fgetpos(void* e, fpos_t* pos) {
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

#ifdef __linux__
  fpos_t temp = {};
  temp.__pos = ((EFILE*)e)->end - ((EFILE*)e)->pos;
  memcpy(pos, &temp, sizeof(fpos_t));
#else
  *pos = (fpos_t)(((EFILE*)e)->end - ((EFILE*)e)->pos);
#endif

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
NP_DECL(int) np_vfprintf(void *e, const char *format, va_list args) {
  if (NP_FOREIGN_PTR) {
    return vfprintf((FILE*)e, format, args);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return vfprintf(((EFILE*)e)->f, format, args);
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



#ifdef _WIN32

NP_DECL(int) np__stat32(const char *file, struct _stat32 *buf) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat32(file, buf);
}

NP_DECL(int) np__wstat32(const wchar_t *file, struct _stat32 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat32(file, buf);
}

NP_DECL(int) np__stat64(const char *file, struct _stat64 *buf) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat64(file, buf);
}

NP_DECL(int) np__wstat64(const wchar_t *file, struct _stat64 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat64(file, buf);
}

NP_DECL(int) np__stat32i64(const char *file, struct _stat32i64 *buf) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat32i64(file, buf);
}

NP_DECL(int) np__wstat32i64(const wchar_t *file, struct _stat32i64 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat32i64(file, buf);
}

NP_DECL(int) np__stat64i32(const char *file, struct _stat64i32 *buf) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _stat64i32(file, buf);
}

NP_DECL(int) np__wstat64i32(const wchar_t *file, struct _stat64i32 *buf) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, file, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return _wstat64i32(file, buf);
}

NP_STD(DWORD) np_GetFileAttributesA(LPCSTR lpFileName) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(lpFileName, absolute_path, PATH_MAX);
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
    return GetFileAttributesA(lpFileName);
}

NP_STD(DWORD) np_GetFileAttributesW(LPCWSTR lpFileName) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);
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

NP_STD(BOOL) np_GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
    // This logic only supports the standard information level.
    if (fInfoLevelId != GetFileExInfoStandard || !lpFileInformation) {
        return GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
    }

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(lpFileName, absolute_path, PATH_MAX);
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
            fileSize.QuadPart = map->size;
            pData->nFileSizeHigh = fileSize.HighPart;
            pData->nFileSizeLow = fileSize.LowPart;

            // Timestamps are zeroed by memset, which is acceptable for a virtual file.
            return TRUE;
        }
    }
    return GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

NP_STD(BOOL) np_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
    // This logic only supports the standard information level.
    if (fInfoLevelId != GetFileExInfoStandard || !lpFileInformation) {
        return GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
    }

    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);
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
            fileSize.QuadPart = map->size;
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
HNDMAP np_handle2file[512] = {};


/**
 * @brief Closes an open object handle. This can be a handle to a file,
 * mapping, process, thread, etc.
 * @param hObject A valid handle to an open object.
 * @return If the function succeeds, the return value is nonzero.
 *
 * This implementation intercepts handles to virtual files and cleans up
 * the associated resources before closing the underlying native handle.
 */
NP_STD(BOOL) np_CloseHandle(HANDLE hObject) {
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hObject) {
            // This is a handle to one of our virtual files.
            // Free the memory for the EFILE struct.
            free(np_handle2file[i].f);

            // Clear the entry in our map.
            np_handle2file[i].h = NULL;
            np_handle2file[i].f = NULL;

            // Now, close the underlying native handle we created.
            return CloseHandle(hObject);
        }
    }

    // If it's not a handle we're managing, pass it to the real API.
    return CloseHandle(hObject);
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
NP_STD(HANDLE) np_CreateFileW(
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
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);

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
        EFILE* e = (EFILE*)malloc(sizeof *e);
        if (!e) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE_VALUE;
        }

        e->handle_type = EHANDLE_VIRTUAL;
        e->name = (&nuitka_embed_data + map->pathpos);
        e->pos = (&nuitka_embed_data + map->pos);
        e->end = (&nuitka_embed_data + map->pos + map->size);
        e->size = map->size;
        e->map = map; // Store map pointer for retrieving info later.

        // Create a real, underlying handle by opening the executable itself.
        // This gives us a valid system handle to return to the caller.
        HANDLE hFake = CreateFileA(executable, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFake == INVALID_HANDLE_VALUE) {
            free(e);
            return INVALID_HANDLE_VALUE; // Let caller get the error from CreateFileA.
        }

        // Find an empty slot in our handle map to store the association.
        for (int i = 0; i < 512; i++) {
            if (np_handle2file[i].h == NULL) {
                np_handle2file[i].h = hFake;
                np_handle2file[i].f = e;
                return hFake; // Return the fake handle to the caller.
            }
        }

        // No free slots in our map.
        free(e);
        CloseHandle(hFake);
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return INVALID_HANDLE_VALUE;
    }

    // File not found in virtual FS, call the real API.
    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
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
NP_STD(BOOL) np_GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            e = np_handle2file[i].f;
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
NP_STD(BOOL) np_GetFileInformationByHandleEx(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            e = np_handle2file[i].f;
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
NP_STD(DWORD) np_GetFileType(HANDLE hFile) {
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            // This handle belongs to one of our virtual files.
            // They behave like disk files.
            return FILE_TYPE_DISK;
        }
    }

    // Not a virtual file, fall back to the real API.
    return GetFileType(hFile);
}

#else
NP_DECL(int) np_stat(const char *file, struct stat *buf) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return stat(file, buf);
}

NP_DECL(int) np_lstat(const char *file, struct stat *buf) {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file, absolute_path, PATH_MAX);
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
            tmp.st_size = map->size;
            memcpy(buf, &tmp, sizeof(tmp));
            return 0;
        }
    }
    return lstat(file, buf);
}

NP_DECL(int) np_fstat(int fd, struct stat *buf) {
    return fstat(fd, buf);
}
#endif
