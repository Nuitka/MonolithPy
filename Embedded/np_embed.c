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

#if _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#else
#define unlikely(x)     (x)
#endif

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
bool np_fd2file_initialized = false;

#ifdef _WIN32
// A map to associate mapping HANDLEs with our virtual EFILE structs.
struct VMAP_S {   // Virtual File Mapping
    HANDLE h;
    EFILE *f;
};
typedef struct VMAP_S VMAP;

// An array to store the mappings for open virtual files.
VMAP np_mapping2file[512] = {};

// A map to associate view pointers with our virtual EFILE structs
struct VIEWMAP_S { // Virtual File View
    LPVOID view;
    EFILE *f;
};
typedef struct VIEWMAP_S VIEWMAP;

VIEWMAP np_view2file[512] = {};

// For FindFirst/NextFile functionality
typedef struct VIRTUAL_FIND_HANDLE_DATA_S {
    EMAP* map_ptr; // Current position in the embedded map iterator
    char virtual_dir_path[PATH_MAX]; // The virtual path of the directory being searched
    char search_pattern[PATH_MAX]; // The file pattern (e.g., "*.txt")
} VIRTUAL_FIND_HANDLE_DATA;

// A map to associate find handles with our virtual find data
struct FIND_HNDMAP_S {
    HANDLE h;
    VIRTUAL_FIND_HANDLE_DATA *data;
};
typedef struct FIND_HNDMAP_S FIND_HNDMAP;

FIND_HNDMAP np_findhandles[512] = {};
#endif

uint32_t hash(char * key) {   // Hash Function: MurmurOAAT64
  uint32_t h = 3323198485ul;
  for (;*key;++key) {
    h ^= *key;
    h *= 0x5bd1e995;
    h ^= h >> 15;
  }
  return h;
}

ALWAYS_INLINE void init_fd2file() {
  if (unlikely(!np_fd2file_initialized)) {
    for (int i = 0; i < 512; ++i) {
      np_fd2file[i].fd = -1;
      // The pointer 'f' is already NULL due to the initial zero-initialization
      // but it's good practice to be explicit if the intent isn't just zeroing.
      np_fd2file[i].f = NULL;
    }
    np_fd2file_initialized = true;
  }
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
    strcpy_s(long_path_buffer, buffer_size, current_path);

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

static bool find_embedded_file(char *search_path, EMAP **found_map) {
  EMAP *map = (EMAP*)(&nuitka_embed_map);
  const char *end = &nuitka_embed_map + nuitka_embed_map_len;
  for (char *p = search_path ; *p; ++p) *p = tolower(*p);
  uint32_t key = hash(search_path);

  while ((char*)map < end) {
    if (map->hash == key && strcasecmp(&nuitka_embed_data + map->pathpos, search_path) == 0) {
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
      e->map = map;
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
  init_fd2file();
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
    e->map = map;
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == -1) {
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
      e->map = map;
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
  init_fd2file();
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
    e->map = map;
    int fakefd = open(executable, O_RDONLY);
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == -1) {
        np_fd2file[i].fd = fakefd;
        np_fd2file[i].f = e;
        break;
      }
    }
    return fakefd;
  }

  return _wopen(wfile, flags, mode);
}
#else
NP_DECL(int) np_openat(int dirfd, const char *pathname, int flags, mode_t mode) {
  init_fd2file();
  char full_virtual_path[PATH_MAX] = {};
  bool is_virtual_context = false;

  // Handle cases where pathname is relative to a virtual directory fd
  if (pathname[0] != '/') {
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == dirfd) {
        is_virtual_context = true;
        EFILE *e = np_fd2file[i].f;
        // We found the fd, it corresponds to a virtual file.
        if (e->map->type != ETYPE_DIRECTORY) {
          errno = ENOTDIR;
          return -1;
        }

        // It's a directory, construct the full virtual path.
        snprintf(full_virtual_path, PATH_MAX, "%s/%s", e->name, pathname);

        // The base virtual path is already normalized, but the new one might not be.
        np_normalize_path(full_virtual_path);
        break; // Found our context, exit the loop
      }
    }
  }

  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (!get_executable_path(execfolder, executable)) {
    // If we can't get executable path, we cannot proceed with virtual file logic.
    return openat(dirfd, pathname, flags, mode);
  }

  // If we are not in a virtual context, resolve the path from CWD or absolute.
  if (!is_virtual_context && pathname[0] == '/') {
    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(pathname, absolute_path, PATH_MAX);
    get_virtual_path(full_virtual_path, execfolder, absolute_path);
  }

  EMAP *map;
  if (find_embedded_file(full_virtual_path, &map)) {
    // Virtual files are read-only.
    if ((flags & O_WRONLY) || (flags & O_RDWR)) {
      errno = EROFS;
      return -1;
    }

    // Do not allow opening a directory with open/openat unless O_DIRECTORY is specified.
    if (map->type == ETYPE_DIRECTORY && !(flags & O_DIRECTORY)) {
      errno = EISDIR;
      return -1;
    }

    if (map->type == ETYPE_FILE && (flags & O_DIRECTORY)) {
      errno = ENOTDIR;
      return -1;
    }

    EFILE *e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_VIRTUAL;
    e->name = &nuitka_embed_data + map->pathpos;
    e->pos = &nuitka_embed_data + map->pos;
    e->end = &nuitka_embed_data + map->pos + map->size;
    e->size = map->size;
    e->map = map;

    // Use the executable itself for the underlying native fd
    int fakefd = open(executable, O_RDONLY);
    if (fakefd == -1) {
      free(e);
      return -1; // errno will be set by open()
    }

    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == -1) {
        np_fd2file[i].fd = fakefd;
        np_fd2file[i].f = e;
        return fakefd;
      }
    }

    // No more file descriptors available in our virtual table
    free(e);
    close(fakefd);
    errno = EMFILE;
    return -1;
  }

  // If we determined the context was a virtual directory but didn't find the file
  if (is_virtual_context) {
    errno = ENOENT;
    return -1;
  }

  // Fallback to the real openat for all other cases (non-virtual paths)
  return openat(dirfd, pathname, flags, mode);
}
#endif

NP_DECL(int) np_fclose(void* e) {
  init_fd2file();
  if (NP_FOREIGN_PTR) {
    // This is not a stream managed by our system, pass to the real fclose.
    return fclose((FILE*)e);
  }

  EFILE* efile = (EFILE*)e;

  // Handle native file streams wrapped in our EFILE struct.
  if (efile->handle_type == EHANDLE_NATIVE) {
    // Call the real fclose on the underlying FILE stream.
    int result = fclose(efile->f);
    // Free the EFILE wrapper struct.
    free(efile);
    return result;
  }

  // Handle virtual file streams.
  if (efile->handle_type == EHANDLE_VIRTUAL) {
    // To close a virtual file, we must find its associated native file
    // descriptor in our map and close it.
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].f == efile) {
        int fd_to_close = np_fd2file[i].fd;

        // Important: Clear the map entry to prevent dangling pointers
        // and incorrect reuse of the slot.
        np_fd2file[i].fd = -1;
        np_fd2file[i].f = NULL;

        // Free the EFILE struct itself.
        free(efile);

        // Finally, close the underlying native file descriptor.
        // The return value of the underlying close() is the result of this operation.
        return close(fd_to_close);
      }
    }
  }

  // Fallback for unrecognized handle types or for virtual files that
  // were somehow created without a map entry (which indicates a bug).
  // The best we can do is free the wrapper to prevent a memory leak.
  free(efile);
  return 0;
}

NP_DECL(int) np_close(int fd) {
  init_fd2file();
  EFILE *e = NULL;
  int fd_idx = -1;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      fd_idx = i;
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

  if (fd_idx != -1) {
    np_fd2file[fd_idx].fd = -1;
    np_fd2file[fd_idx].f = NULL;
  }

  // Close the underlying file descriptor that was opened on the executable
  return close(fd);
}

NP_DECL(EFILE*) np_freopen(const char *filename, const char *mode, void *e) {
  if (NP_FOREIGN_PTR) {
    EFILE* e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_NATIVE;
    e->f = freopen(filename, mode, (FILE*)e);
    return e;
  }
  if (((EFILE*)e)->handle_type == EHANDLE_NATIVE) {
    EFILE* e = (EFILE*)malloc(sizeof *e);
    e->handle_type = EHANDLE_NATIVE;
    e->f = freopen(filename, mode, ((EFILE*)e)->f);
    return e;
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

  if (((EFILE*)e)->end < ((EFILE*)e)->pos){
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
  init_fd2file();
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
  init_fd2file();
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

  // fgetpos should store the absolute offset from the start of the file.
  // We use the same logic as np_ftell_priv to get this value.
  int64_t offset = ((EFILE*)e)->pos - (((EFILE*)e)->end - ((EFILE*)e)->size);
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }

#ifdef __linux__
  // On Linux, fpos_t is a struct.
  fpos_t temp = { 0 };
  temp.__pos = offset;
  memcpy(pos, &temp, sizeof(fpos_t));
#else
  // On other systems, it's typically a numeric type.
  *pos = (fpos_t)offset;
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

#ifndef _WIN32
NP_DECL(ssize_t) np_getline(char **lineptr, size_t *n, void* e) {
  if (NP_FOREIGN_PTR) {
    return getline(lineptr, n, (FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return getline(lineptr, n, ((EFILE*)e)->f);
  }

  // Handle virtual files
  if (np_feof(e)) {
    return -1;
  }

  // Initialize buffer if needed
  if (*lineptr == NULL || *n == 0) {
    *n = 128; // Initial buffer size
    *lineptr = (char*)malloc(*n);
    if (*lineptr == NULL) {
      return -1;
    }
  }

  size_t pos = 0;
  int c;

  while ((c = np_fgetc(e)) != -1) {
    // Ensure buffer has space for character and null terminator
    if (pos + 1 >= *n) {
      size_t new_size = *n * 2;
      char *new_ptr = (char*)realloc(*lineptr, new_size);
      if (new_ptr == NULL) {
        return -1;
      }
      *lineptr = new_ptr;
      *n = new_size;
    }

    (*lineptr)[pos++] = (char)c;

    // Stop at newline
    if (c == '\n') {
      break;
    }
  }

  // If we read nothing and hit EOF, return -1
  if (pos == 0 && c == -1) {
    return -1;
  }

  // Null-terminate the string
  (*lineptr)[pos] = '\0';

  return (ssize_t)pos;
}
#endif

NP_DECL(int) np_fgetc(void* e) {
  if (NP_FOREIGN_PTR) {
    return fgetc((FILE*)e);
  }

  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fgetc(((EFILE*)e)->f);
  }

  if(np_feof(e))
    return -1;
  return (int)(unsigned char)(*(((EFILE*)e)->pos++));
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

  return ((EFILE*)e)->pos - (((EFILE*)e)->end - ((EFILE*)e)->size);
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
  errno = EACCES;
  return -1;
}

NP_DECL(int) np_fputs(const char *str, void *e) {
  if (NP_FOREIGN_PTR) {
    return fputs(str, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fputs(str, ((EFILE*)e)->f);
  }
  errno = EACCES;
  return -1;
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
  errno = EACCES;
  return -1;
}
NP_DECL(int) np_vfprintf(void *e, const char *format, va_list args) {
  if (NP_FOREIGN_PTR) {
    return vfprintf((FILE*)e, format, args);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return vfprintf(((EFILE*)e)->f, format, args);
  }
  errno = EACCES;
  return -1;
}

NP_DECL(size_t) np_fwrite(const void *ptr, size_t size, size_t count, void *e) {
  if (NP_FOREIGN_PTR) {
    return fwrite(ptr, size, count, (FILE*)e);
  }
  if (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL) {
    return fwrite(ptr, size, count, ((EFILE*)e)->f);
  }
  errno = EACCES;
  return -1;
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

  // The 'pos' object now contains the absolute offset from the beginning.
  // We can use our fseek implementation with SEEK_SET to position the stream.
#ifdef __linux__
  // On Linux, fpos_t is a struct, extract the position.
  return np_fseek_priv(e, pos->__pos, SEEK_SET);
#else
  // On other systems, it's typically a numeric type.
  return np_fseek_priv(e, *pos, SEEK_SET);
#endif
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
  init_fd2file();
  if (NP_FOREIGN_PTR) {
    return fileno((FILE*)e);
  }
  EFILE* efile = (EFILE*)e;

  if (efile->handle_type == EHANDLE_NATIVE) {
    // For a native file stream, call the real fileno on the underlying FILE*.
    return fileno(efile->f);
  }

  // For a virtual file, first attempt to find the existing file descriptor.
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].f == efile) {
      // Found the existing mapping, which is the correct behavior.
      return np_fd2file[i].fd;
    }
  }

  // The stream was not found in the map. We will now
  // create and add a new file descriptor.

  char execfolder[PATH_MAX] = {}, executable_path[PATH_MAX];
  // We need the executable path to open a new native handle.
  if (!get_executable_path(execfolder, executable_path)) {
    // Cannot get executable path, cannot create a new fd.
    errno = EBADF;
    return -1;
  }

  // DANGER: Opening a new file descriptor here because the original was lost.
  int new_fd = open(executable_path, O_RDONLY);
  if (new_fd == -1) {
    // The open call failed, return its error.
    return -1;
  }

  // Now, find an empty slot in the map to store this new, leaked descriptor.
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == -1) {
      np_fd2file[i].fd = new_fd;
      np_fd2file[i].f = efile;
      // The function now returns a NEW descriptor that the calling code wasn't expecting.
      return new_fd;
    }
  }

  // If we are here, our virtual descriptor map is full.
  // We must close the descriptor we just opened and return an error.
  close(new_fd);
  errno = EMFILE; // Too many open files
  return -1;
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
  init_fd2file();
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
NP_DECL(__int64) np_lseeki64(int fd, __int64 offset, int whence) {
#else
NP_DECL(ssize_t) np_lseek(int fd, off_t offset, int whence) {
#endif
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
#ifdef _WIN32
    return _lseeki64(fd, offset, whence);
#else
    return lseek(fd, offset, whence);
#endif
  }
  if(whence == SEEK_SET)
    e->pos = (e->end - e->size) + offset;
  else if(whence == SEEK_CUR)
    e->pos += offset;
  else if(whence == SEEK_END)
    e->pos = e->end + offset;

  if(e->end < e->pos || (e->pos - (e->end - e->size)) < 0) {
    errno = EINVAL;
    return -1;
  }
  return (e->pos - (e->end - e->size));
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
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return GetFileAttributesA(lpFileName);
    }
    return np_GetFileAttributesW(wide_path);
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
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
    }
    return np_GetFileAttributesExW(wide_path, fInfoLevelId, lpFileInformation);
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
 * This implementation intercepts handles to virtual files and file mappings,
 * cleans up the associated resources, and then calls the appropriate
 * underlying close function if necessary.
 */
NP_STD(BOOL) np_CloseHandle(HANDLE hObject) {
    // Check if it's a handle to a virtual file mapping.
    for (int i = 0; i < 512; i++) {
        if (np_mapping2file[i].h == hObject) {
            // This is a handle to one of our virtual file mappings.
            // Since the "handle" is just a pointer to an EFILE struct
            // and doesn't correspond to a real system object that needs closing,
            // we just clear our tracking entry. The actual EFILE struct
            // will be freed when the original file handle is closed.
            np_mapping2file[i].h = NULL;
            np_mapping2file[i].f = NULL;
            return TRUE; // Indicate success.
        }
    }

    // Check if it's a handle to a virtual file created by CreateFile.
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

NP_STD(HANDLE) np_CreateFileA(
        LPCSTR lpFileName,
        DWORD dwDesiredAccess,
        DWORD dwShareMode,
        LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        DWORD dwCreationDisposition,
        DWORD dwFlagsAndAttributes,
        HANDLE hTemplateFile
) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    }
    return np_CreateFileW(wide_path, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
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

NP_STD(BOOL) np_DeleteFileA(LPCSTR lpFileName) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return DeleteFileA(lpFileName);
    }
    return np_DeleteFileW(wide_path);
}

NP_STD(BOOL) np_DeleteFileW(LPCWSTR lpFileName) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, sizeof(file_mb));

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
    }
    return DeleteFileW(lpFileName);
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

/**
 * @brief Retrieves the final path for the specified file.
 * @param hFile A handle to a file or directory.
 * @param lpszFilePath A pointer to a buffer that receives the path.
 * @param cchFilePath The size of the lpszFilePath buffer, in TCHARs.
 * @param dwFlags The options for how the path should be formatted.
 * @return The length of the path copied to lpszFilePath, or the required buffer size.
 *
 * For virtual files, this returns the path stored within the embedded data.
 */
NP_STD(DWORD) np_GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            e = np_handle2file[i].f;
            break;
        }
    }

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (e != NULL && get_executable_path(execfolder, executable)) {
        char resulting_path[PATH_MAX] = {};
        if (e->name[0] == '~') {
            // This is a relative path.
            size_t execfolder_len = strlen(execfolder);
            strcpy(&resulting_path[0], execfolder);
            size_t curr_res_pos = execfolder_len;
            for (size_t i = 1; i < PATH_MAX && curr_res_pos < PATH_MAX - 1; i++) {
                char curr = e->name[i];
                if (curr == '\0')
                    break;
                if (curr == '/')
                    curr = '\\';

                resulting_path[curr_res_pos] = curr;
                curr_res_pos++;
            }
            resulting_path[PATH_MAX - 1] = '\0';
        } else {
            strcpy(&resulting_path[0], e->name);
        }

        // It's a virtual file. The name is the final path.
        size_t required_chars = mbstowcs(NULL, resulting_path, 0);
        if (required_chars == (size_t)-1) {
            SetLastError(ERROR_INVALID_DATA);
            return 0;
        }

        if (cchFilePath < required_chars + 1) {
            // Buffer is too small. Return the required size.
            return required_chars + 1;
        }

        size_t converted_chars = mbstowcs(lpszFilePath, resulting_path, cchFilePath);
        if (converted_chars == (size_t)-1) {
            SetLastError(ERROR_INVALID_DATA);
            return 0;
        }

        return converted_chars;
    }

    // Not a virtual file, fall back to the real API.
    return GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

/**
 * @brief Retrieves the full path and file name of the specified file.
 * @return The length of the string copied to lpBuffer, or the required buffer size.
 *
 * For virtual files, this returns the calculated absolute path.
 */
NP_STD(DWORD) np_GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart) {
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
            // It's a virtual file. Return the absolute path we constructed.
            size_t required_chars = mbstowcs(NULL, absolute_path, 0) + 1;
            if (required_chars == 0) {
                SetLastError(ERROR_INVALID_DATA);
                return 0;
            }

            if (nBufferLength < required_chars) {
                return required_chars; // Return required buffer size
            }

            mbstowcs(lpBuffer, absolute_path, nBufferLength);

            if (lpFilePart) {
                *lpFilePart = PathFindFileNameW(lpBuffer);
            }
            return wcslen(lpBuffer);
        }
    }

    // Not a virtual file, fall back to the real API.
    return GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

/**
 * @brief Retrieves the volume mount point where the specified path is located.
 * @return Nonzero on success, zero on failure.
 *
 * For virtual files, this retrieves the volume of the executable itself.
 */
NP_STD(BOOL) np_GetVolumePathNameW(LPCWSTR lpszFileName, LPWSTR lpszVolumePathName, DWORD cchBufferLength) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpszFileName, PATH_MAX);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(file_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map)) {
            // It's a virtual file. The volume is the volume of the executable.
            wchar_t exec_path_w[PATH_MAX];
            mbstowcs(exec_path_w, executable, PATH_MAX);

            return GetVolumePathNameW(exec_path_w, lpszVolumePathName, cchBufferLength);
        }
    }

    // Not a virtual file, fall back to the real API.
    return GetVolumePathNameW(lpszFileName, lpszVolumePathName, cchBufferLength);
}

/**
 * @brief Retrieves information about the amount of space on a disk volume.
 * @return Nonzero on success, zero on failure.
 *
 * For virtual paths, this returns the disk space for the volume hosting the executable.
 */
NP_STD(BOOL) np_GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
    char dir_mb[PATH_MAX];
    if (lpDirectoryName != NULL) {
        wcstombs(dir_mb, lpDirectoryName, PATH_MAX);
    } else {
        GetCurrentDirectoryA(PATH_MAX, dir_mb);
    }

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(dir_mb, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
        char search_path[PATH_MAX] = {};
        get_virtual_path(search_path, execfolder, absolute_path);

        EMAP *map;
        if (find_embedded_file(search_path, &map) && map->type == ETYPE_DIRECTORY) {
            // It's a virtual directory. The disk space is the one for the executable's drive.
            return GetDiskFreeSpaceExA(execfolder, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
        }
    }

    // Fall back for real paths.
    return GetDiskFreeSpaceExW(lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

// Helper to convert WIN32_FIND_DATAW to WIN32_FIND_DATAA
static void ConvertFindDataWtoA(LPWIN32_FIND_DATAA lpFindFileDataA, const WIN32_FIND_DATAW* lpFindFileDataW) {
    lpFindFileDataA->dwFileAttributes = lpFindFileDataW->dwFileAttributes;
    lpFindFileDataA->ftCreationTime = lpFindFileDataW->ftCreationTime;
    lpFindFileDataA->ftLastAccessTime = lpFindFileDataW->ftLastAccessTime;
    lpFindFileDataA->ftLastWriteTime = lpFindFileDataW->ftLastWriteTime;
    lpFindFileDataA->nFileSizeHigh = lpFindFileDataW->nFileSizeHigh;
    lpFindFileDataA->nFileSizeLow = lpFindFileDataW->nFileSizeLow;
    lpFindFileDataA->dwReserved0 = lpFindFileDataW->dwReserved0;
    lpFindFileDataA->dwReserved1 = lpFindFileDataW->dwReserved1;
    WideCharToMultiByte(CP_ACP, 0, lpFindFileDataW->cFileName, -1, lpFindFileDataA->cFileName, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, lpFindFileDataW->cAlternateFileName, -1, lpFindFileDataA->cAlternateFileName, 14, NULL, NULL);
}

// Helper to populate WIN32_FIND_DATAW from an EMAP entry for FindFirst/Next
static void populate_find_data(EMAP* map, WIN32_FIND_DATAW* find_data) {
    memset(find_data, 0, sizeof(WIN32_FIND_DATAW));

    // Convert filename to wide char
    const char* filename = PathFindFileNameA(&nuitka_embed_data + map->pathpos);
    mbstowcs(find_data->cFileName, filename, _countof(find_data->cFileName));
    // Ensure null-termination for safety
    find_data->cFileName[_countof(find_data->cFileName) - 1] = L'\0';

    // Set attributes
    find_data->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    if (map->type == ETYPE_DIRECTORY) {
        find_data->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }

    // Set file size
    ULARGE_INTEGER fileSize;
    fileSize.QuadPart = map->size;
    find_data->nFileSizeHigh = fileSize.HighPart;
    find_data->nFileSizeLow = fileSize.LowPart;
}

NP_STD(HANDLE) np_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    wchar_t wide_path[PATH_MAX];
    if (MultiByteToWideChar(CP_ACP, 0, lpFileName, -1, wide_path, PATH_MAX) == 0) {
        return FindFirstFileA(lpFileName, lpFindFileData);
    }

    WIN32_FIND_DATAW find_data_w;
    HANDLE hFindFile = np_FindFirstFileW(wide_path, &find_data_w);

    if (hFindFile != INVALID_HANDLE_VALUE) {
        ConvertFindDataWtoA(lpFindFileData, &find_data_w);
    }

    return hFindFile;
}

NP_STD(BOOL) np_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
    VIRTUAL_FIND_HANDLE_DATA* find_handle_data = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_findhandles[i].h == hFindFile) {
            find_handle_data = np_findhandles[i].data;
            break;
        }
    }

    if (find_handle_data == NULL) {
        return FindNextFileA(hFindFile, lpFindFileData);
    }

    WIN32_FIND_DATAW find_data_w;
    if (np_FindNextFileW(hFindFile, &find_data_w)) {
        ConvertFindDataWtoA(lpFindFileData, &find_data_w);
        return TRUE;
    }

    return FALSE;
}

/**
 * @brief Searches a directory for a file or subdirectory.
 * @return A search handle on success, or INVALID_HANDLE_VALUE on failure.
 *
 * Intercepts searches in virtual directories.
 */
NP_STD(HANDLE) np_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
    char file_mb[PATH_MAX];
    wcstombs(file_mb, lpFileName, sizeof(file_mb));

    char search_dir[PATH_MAX];
    strcpy(search_dir, file_mb);
    PathRemoveFileSpecA(search_dir);
    char* search_pattern = PathFindFileNameA(file_mb);

    char absolute_path[PATH_MAX] = {};
    np_get_absolute_path(search_dir, absolute_path, PATH_MAX);

    char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
    if (!get_executable_path(execfolder, executable)) {
        return FindFirstFileW(lpFileName, lpFindFileData);
    }

    char virtual_dir_path[PATH_MAX] = {};
    get_virtual_path(virtual_dir_path, execfolder, absolute_path);

    EMAP *dir_map;
    if (find_embedded_file(virtual_dir_path, &dir_map) && dir_map->type == ETYPE_DIRECTORY) {
        // Ensure virtual directory path ends with a slash for correct matching
        if (strlen(virtual_dir_path) > 1 && virtual_dir_path[strlen(virtual_dir_path)-1] != '/') {
            strcat(virtual_dir_path, "/");
        }

        EMAP *map = (EMAP*)(&nuitka_embed_map);
        const char *end = &nuitka_embed_map + nuitka_embed_map_len;

        while ((char*)map < end) {
            if (strncmp(&nuitka_embed_data + map->pathpos, virtual_dir_path, strlen(virtual_dir_path)) == 0) {
                const char* filename_part = (&nuitka_embed_data + map->pathpos) + strlen(virtual_dir_path);
                if (strchr(filename_part, '/') == NULL) {
                    if (PathMatchSpecA(filename_part, search_pattern)) {
                        VIRTUAL_FIND_HANDLE_DATA* find_handle_data = (VIRTUAL_FIND_HANDLE_DATA*)malloc(sizeof(VIRTUAL_FIND_HANDLE_DATA));
                        if (!find_handle_data) {
                            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                            return INVALID_HANDLE_VALUE;
                        }
                        find_handle_data->map_ptr = map;
                        strcpy(find_handle_data->search_pattern, search_pattern);
                        strcpy(find_handle_data->virtual_dir_path, virtual_dir_path);

                        for (int i = 0; i < 512; i++) {
                            if (np_findhandles[i].h == NULL) {
                                np_findhandles[i].h = (HANDLE)find_handle_data;
                                np_findhandles[i].data = find_handle_data;
                                populate_find_data(map, lpFindFileData);
                                return np_findhandles[i].h;
                            }
                        }
                        free(find_handle_data);
                        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
                        return INVALID_HANDLE_VALUE;
                    }
                }
            }
            map++;
        }

        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    return FindFirstFileW(lpFileName, lpFindFileData);
}

/**
 * @brief Continues a file search from a previous call to FindFirstFileW.
 * @return Nonzero on success, zero on failure.
 */
NP_STD(BOOL) np_FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
    VIRTUAL_FIND_HANDLE_DATA* find_handle_data = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_findhandles[i].h == hFindFile) {
            find_handle_data = np_findhandles[i].data;
            break;
        }
    }

    if (find_handle_data == NULL) {
        return FindNextFileW(hFindFile, lpFindFileData);
    }

    EMAP *map = find_handle_data->map_ptr + 1;
    const char *end = &nuitka_embed_map + nuitka_embed_map_len;

    while ((char*)map < end) {
        if (strncmp(&nuitka_embed_data + map->pathpos, find_handle_data->virtual_dir_path, strlen(find_handle_data->virtual_dir_path)) == 0) {
            const char* filename_part = (&nuitka_embed_data + map->pathpos) + strlen(find_handle_data->virtual_dir_path);
            if (strchr(filename_part, '/') == NULL) {
                if (PathMatchSpecA(filename_part, find_handle_data->search_pattern)) {
                    find_handle_data->map_ptr = map;
                    populate_find_data(map, lpFindFileData);
                    return TRUE;
                }
            }
        }
        map++;
    }

    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

/**
 * @brief Closes a file search handle.
 * @return Nonzero on success, zero on failure.
 */
NP_STD(BOOL) np_FindClose(HANDLE hFindFile) {
    for (int i = 0; i < 512; i++) {
        if (np_findhandles[i].h == hFindFile) {
            free(np_findhandles[i].data);
            np_findhandles[i].h = NULL;
            np_findhandles[i].data = NULL;
            return TRUE;
        }
    }

    return FindClose(hFindFile);
}

/**
 * @brief Retrieves the size of the specified file, in bytes.
 * @param hFile A handle to the file.
 * @param lpFileSize A pointer to a variable where the file size is returned.
 * @return If the function succeeds, the return value is nonzero.
 *
 * Intercepts handles to virtual files and provides the file size from the
 * embedded data map.
 */
NP_STD(BOOL) np_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            e = np_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual file.
        if (lpFileSize == NULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        lpFileSize->QuadPart = e->size;
        return TRUE;
    }

    // Not a virtual file, fall back to the real API.
    return GetFileSizeEx(hFile, lpFileSize);
}

/**
 * @brief Retrieves the size of the specified file. (Legacy)
 * @param hFile A handle to the file.
 * @param lpFileSizeHigh A pointer to a variable that receives the high-order DWORD of the file size. Can be NULL.
 * @return If the function succeeds, the return value is the low-order DWORD of the file size.
 * If the function fails and lpFileSizeHigh is NULL, the return value is INVALID_FILE_SIZE.
 *
 * Intercepts handles to virtual files and provides the file size from the
 * embedded data map.
 */
NP_STD(DWORD) np_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            e = np_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual file.
        ULARGE_INTEGER fileSize;
        fileSize.QuadPart = e->size;

        if (lpFileSizeHigh != NULL) {
            *lpFileSizeHigh = fileSize.HighPart;
        }

        // The original API returns INVALID_FILE_SIZE on failure. If the low part
        // is exactly that value, we must clear the last error to indicate success.
        if (fileSize.LowPart == INVALID_FILE_SIZE) {
            SetLastError(NO_ERROR);
        }

        return fileSize.LowPart;
    }

    // Not a virtual file, fall back to the real API.
    return GetFileSize(hFile, lpFileSizeHigh);
}

NP_STD(HANDLE) np_CreateFileMappingA(
    HANDLE                hFile,
    LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
    DWORD                 flProtect,
    DWORD                 dwMaximumSizeHigh,
    DWORD                 dwMaximumSizeLow,
    LPCSTR                lpName
) {
    // If a name is provided for the mapping object, we need to convert it
    // to a wide character string to pass to the np_CreateFileMappingW function.
    if (lpName) {
        // Determine the required buffer size for the wide string.
        int required_chars = MultiByteToWideChar(CP_ACP, 0, lpName, -1, NULL, 0);
        if (required_chars == 0) {
             // Let the real API handle the error.
            return CreateFileMappingA(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
        }

        // Allocate memory for the wide string.
        wchar_t* wide_name = (wchar_t*)malloc(required_chars * sizeof(wchar_t));
        if (wide_name == NULL) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }

        // Perform the conversion.
        MultiByteToWideChar(CP_ACP, 0, lpName, -1, wide_name, required_chars);

        // Call our wide-character implementation with the converted name.
        HANDLE result = np_CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, wide_name);

        // Free the allocated memory for the wide string.
        free(wide_name);

        return result;
    } else {
        // If no name is provided, we can call the wide-character version directly with NULL.
        return np_CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, NULL);
    }
}


NP_STD(HANDLE) np_CreateFileMappingW(
    HANDLE                hFile,
    LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
    DWORD                 flProtect,
    DWORD                 dwMaximumSizeHigh,
    DWORD                 dwMaximumSizeLow,
    LPCWSTR               lpName
) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        if (np_handle2file[i].h == hFile) {
            e = np_handle2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // This is a virtual file.
        // Virtual files are read-only, so deny writeable mappings.
        if (flProtect & (PAGE_EXECUTE_READWRITE | PAGE_READWRITE)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return NULL;
        }

        // The size of the mapping must be within the file's bounds.
        ULONGLONG maxSize = ((ULONGLONG)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
        if (maxSize > e->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }

        // We can't create a real mapping object for in-memory data.
        // Instead, we'll use a pointer to our EFILE struct as a "handle".
        // This is a bit of a hack, but it allows us to identify the mapping later.
        // Find an empty slot in our mapping table.
        for (int i = 0; i < 512; i++) {
            if (np_mapping2file[i].h == NULL) {
                // We use the EFILE pointer itself as the handle.
                np_mapping2file[i].h = (HANDLE)e;
                np_mapping2file[i].f = e;
                return (HANDLE)e;
            }
        }
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return NULL;
    }

    // Not a virtual file, fall back to the real API.
    return CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
}

NP_STD(LPVOID) np_MapViewOfFile(
    HANDLE hFileMappingObject,
    DWORD  dwDesiredAccess,
    DWORD  dwFileOffsetHigh,
    DWORD  dwFileOffsetLow,
    SIZE_T dwNumberOfBytesToMap
) {
    EFILE* e = NULL;
    for (int i = 0; i < 512; i++) {
        // Check if the mapping object is one of our virtual ones.
        if (np_mapping2file[i].h == hFileMappingObject) {
            e = np_mapping2file[i].f;
            break;
        }
    }

    if (e != NULL) {
        // It's a virtual mapping. The "view" is just a pointer into our data block.
        // Check for write access, which is not allowed.
        if (dwDesiredAccess & (FILE_MAP_WRITE | FILE_MAP_ALL_ACCESS)) {
            SetLastError(ERROR_ACCESS_DENIED);
            return NULL;
        }

        ULONGLONG offset = ((ULONGLONG)dwFileOffsetHigh << 32) | dwFileOffsetLow;
        if (offset >= e->size) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }

        LPVOID view = (LPVOID)(e->pos + offset);

        // Store the view so we can recognize it in UnmapViewOfFile
        for (int i = 0; i < 512; i++) {
            if (np_view2file[i].view == NULL) {
                np_view2file[i].view = view;
                np_view2file[i].f = e;
                return view;
            }
        }
        SetLastError(ERROR_TOO_MANY_OPEN_FILES);
        return NULL;
    }

    // Not a virtual mapping, fall back to the real API.
    return MapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
}

NP_STD(BOOL) np_UnmapViewOfFile(
    LPCVOID lpBaseAddress
) {
    for (int i = 0; i < 512; i++) {
        if (np_view2file[i].view == lpBaseAddress) {
            // This is a view of one of our virtual files.
            // Since we didn't allocate memory with MapViewOfFile,
            // we don't need to free it here. We just clear our tracking entry.
            np_view2file[i].view = NULL;
            np_view2file[i].f = NULL;
            return TRUE;
        }
    }

    // Not a virtual view, fall back to the real API.
    return UnmapViewOfFile(lpBaseAddress);
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
  init_fd2file();
  // Check if the file descriptor is for a virtual file.
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      EFILE *e = np_fd2file[i].f;
      EMAP *map = e->map;

      // This is a virtual file, populate the stat buffer from the map.
      struct stat tmp;
      memset(&tmp, 0, sizeof(tmp));
      // Set standard read/execute permissions for user, group, and other.
      tmp.st_mode = S_IRUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      switch (map->type) {
        case ETYPE_FILE:
          tmp.st_mode |= S_IFREG; // Regular file
          break;
        case ETYPE_DIRECTORY:
          tmp.st_mode |= S_IFDIR; // Directory
          break;
      }
      tmp.st_size = map->size; // Set the file size.
      // Other fields like timestamps, user/group ID etc., remain 0.

      memcpy(buf, &tmp, sizeof(tmp));
      return 0; // Success
    }
  }

  // If the fd is not in our virtual file table, fall back to the native call.
  return fstat(fd, buf);
}

NP_DECL(int) np_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
  init_fd2file();
  // Handle cases where pathname is relative to a virtual directory fd
  if (pathname[0] != '/') {
    for (int i = 0; i < 512; i++) {
      if (np_fd2file[i].fd == dirfd) {
        EFILE *e = np_fd2file[i].f;
        // We found the fd, it corresponds to a virtual file.
        if (e->map->type != ETYPE_DIRECTORY) {
          errno = ENOTDIR;
          return -1;
        }

        // It's a directory, construct the full virtual path.
        char full_virtual_path[PATH_MAX];
        snprintf(full_virtual_path, PATH_MAX, "%s/%s", e->name, pathname);

        // The virtual path is already normalized, but the new one might not be (e.g., ../)
        char temp_path_for_norm[PATH_MAX];
        strcpy(temp_path_for_norm, full_virtual_path);
        np_normalize_path(temp_path_for_norm);

        EMAP *map;
        if (find_embedded_file(temp_path_for_norm, &map)) {
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
        } else {
          errno = ENOENT;
          return -1;
        }
      }
    }
  }

  // Fallback for absolute paths, AT_FDCWD, or native file descriptors
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(pathname, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      // The virtual file system does not support symlinks, so AT_SYMLINK_NOFOLLOW
      // is effectively ignored as the behavior is the same as lstat/stat.
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

  // Fallback to the real fstatat for non-virtual files.
  return fstatat(dirfd, pathname, buf, flags);
}

NP_DECL(int) np_access(const char *pathname, int mode) {
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(pathname, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      if (mode == F_OK) return 0; // Existence check passes
      if ((mode & W_OK)) { // Write check fails
        errno = EROFS;
        return -1;
      }
      if ((mode & R_OK) || (mode & X_OK)) { // Read/Exec check passes
        return 0;
      }
    }
  }
  return access(pathname, mode);
}

NP_DECL(int) np_faccessat(int dirfd, const char *pathname, int mode, int flags) {
  // This implementation ignores dirfd and flags for virtual files.
  return np_access(pathname, mode);
}

NP_DECL(int) np_statvfs(const char *path, struct statvfs *buf) {
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(path, absolute_path, PATH_MAX);
  char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
  if (get_executable_path(execfolder, executable)) {
    char search_path[PATH_MAX] = {};
    get_virtual_path(search_path, execfolder, absolute_path);

    EMAP *map;
    if (find_embedded_file(search_path, &map)) {
      // Virtual file system lives on the same FS as the executable
      return statvfs(execfolder, buf);
    }
  }
  return statvfs(path, buf);
}

NP_DECL(int) np_fstatvfs(int fd, struct statvfs *buf) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    char execfolder[PATH_MAX] = {}, executable[PATH_MAX];
    if (get_executable_path(execfolder, executable)) {
      return statvfs(execfolder, buf);
    }
  }
  return fstatvfs(fd, buf);
}

NP_DECL(ssize_t) np_pwrite(int fd, const void *buf, size_t count, off_t offset) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    errno = EROFS; // Read-only file system
    return -1;
  }
  return pwrite(fd, buf, count, offset);
}

NP_DECL(ssize_t) np_readv(int fd, const struct iovec *iov, int iovcnt) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
    return readv(fd, iov, iovcnt);
  }

  ssize_t total_read = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t bytes_to_read = iov[i].iov_len;
    ssize_t remaining_in_file = e->end - e->pos;
    if (bytes_to_read > remaining_in_file) {
      bytes_to_read = remaining_in_file;
    }
    memcpy(iov[i].iov_base, e->pos, bytes_to_read);
    e->pos += bytes_to_read;
    total_read += bytes_to_read;
    if (bytes_to_read < iov[i].iov_len) {
      // Reached EOF
      break;
    }
  }
  return total_read;
}

NP_DECL(ssize_t) np_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e == NULL) {
    return preadv(fd, iov, iovcnt, offset);
  }

  if (offset >= e->size) return 0;

  const char* start_pos = (e->end - e->size) + offset;
  ssize_t total_read = 0;
  for (int i = 0; i < iovcnt; i++) {
    ssize_t bytes_to_read = iov[i].iov_len;
    ssize_t remaining_in_file = e->end - start_pos;
    if (bytes_to_read > remaining_in_file) {
      bytes_to_read = remaining_in_file;
    }
    memcpy(iov[i].iov_base, start_pos, bytes_to_read);
    start_pos += bytes_to_read;
    total_read += bytes_to_read;
    if (bytes_to_read < iov[i].iov_len) {
      break;
    }
  }
  return total_read;
}

#ifdef __linux
NP_DECL(ssize_t) np_preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  // Ignoring flags for virtual files
  return np_preadv(fd, iov, iovcnt, offset);
}
#endif

NP_DECL(ssize_t) np_writev(int fd, const struct iovec *iov, int iovcnt)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    errno = EROFS; // Read-only
    return -1;
  }
  return writev(fd, iov, iovcnt);
}

NP_DECL(ssize_t) np_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset)
#ifdef __APPLE__
__API_AVAILABLE(macos(11.0), ios(14.0), watchos(7.0), tvos(14.0))
#endif
{
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    errno = EROFS; // Read-only
    return -1;
  }
  return pwritev(fd, iov, iovcnt, offset);
}

#ifdef __linux
NP_DECL(ssize_t) np_pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
  // Ignoring flags for virtual files
  return np_pwritev(fd, iov, iovcnt, offset);
}
#endif

typedef struct VDIR_S {
  EMAP* map_ptr; // Current position in the embedded map iterator
  char virtual_dir_path[PATH_MAX]; // The virtual path of the directory being searched
  struct dirent entry; // The current entry to be returned
} VDIR;

// A map to associate DIR* pointers with our VDIR structs
VDIR* np_open_vdirs[512] = {};

NP_DECL(DIR*) np_opendir(const char* name) {
  char absolute_path[PATH_MAX] = {};
  np_get_absolute_path(name, absolute_path, PATH_MAX);

  char execfolder[PATH_MAX] = {}, executable[PATH_MAX] = {};
  if (!get_executable_path(execfolder, executable)) {
    return opendir(name);
  }

  char virtual_dir_path[PATH_MAX] = {};
  get_virtual_path(virtual_dir_path, execfolder, absolute_path);

  EMAP *dir_map;
  if (find_embedded_file(virtual_dir_path, &dir_map) && dir_map->type == ETYPE_DIRECTORY) {
    VDIR* vdir = (VDIR*)malloc(sizeof(VDIR));
    if (!vdir) {
      errno = ENOMEM;
      return NULL;
    }
    vdir->map_ptr = (EMAP*)(&nuitka_embed_map);
    strcpy(vdir->virtual_dir_path, virtual_dir_path);
    // Ensure path ends with a slash for correct matching
    if (strlen(vdir->virtual_dir_path) > 1 && vdir->virtual_dir_path[strlen(vdir->virtual_dir_path)-1] != '/') {
      strcat(vdir->virtual_dir_path, "/");
    }

    for (int i = 0; i < 512; i++) {
      if (np_open_vdirs[i] == NULL) {
        np_open_vdirs[i] = vdir;
        return (DIR*)vdir;
      }
    }
    free(vdir);
    errno = EMFILE; // Too many open files
    return NULL;
  }
  return opendir(name);
}

NP_DECL(DIR*) np_fdopendir(int fd) {
  init_fd2file();
  EFILE *e = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_fd2file[i].fd == fd) {
      e = np_fd2file[i].f;
      break;
    }
  }
  if (e != NULL) {
    // We have a virtual file descriptor. We can open it if it's a directory.
    return np_opendir(e->name);
  }
  return fdopendir(fd);
}

NP_DECL(struct dirent*) np_readdir(DIR *dirp) {
  VDIR* vdir = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_open_vdirs[i] == (VDIR*)dirp) {
      vdir = (VDIR*)dirp;
      break;
    }
  }
  if (vdir == NULL) {
    return readdir(dirp);
  }

  const char *end = &nuitka_embed_map + nuitka_embed_map_len;
  while ((char*)vdir->map_ptr < end) {
    EMAP* current_map = vdir->map_ptr;
    vdir->map_ptr++; // Advance for next call

    const char* current_path_str = &nuitka_embed_data + current_map->pathpos;

    if (strncmp(current_path_str, vdir->virtual_dir_path, strlen(vdir->virtual_dir_path)) == 0) {
      const char* filename_part = current_path_str + strlen(vdir->virtual_dir_path);
      if (*filename_part != '\0' && strchr(filename_part, '/') == NULL) {
        // It's a direct child.
        strncpy(vdir->entry.d_name, filename_part, sizeof(vdir->entry.d_name));
        vdir->entry.d_name[sizeof(vdir->entry.d_name) - 1] = '\0';
        // Fake inode number from map position
        vdir->entry.d_ino = (ino_t)((char*)current_map - (const char*) &nuitka_embed_map);
        vdir->entry.d_type = (current_map->type == ETYPE_DIRECTORY) ? DT_DIR : DT_REG;
        return &vdir->entry;
      }
    }
  }
  return NULL; // No more entries
}

NP_DECL(int) np_closedir(DIR *dirp) {
  for (int i = 0; i < 512; i++) {
    if (np_open_vdirs[i] == (VDIR*)dirp) {
      free(np_open_vdirs[i]);
      np_open_vdirs[i] = NULL;
      return 0;
    }
  }
  return closedir(dirp);
}

NP_DECL(void) np_rewinddir(DIR *dirp) {
  VDIR* vdir = NULL;
  for (int i = 0; i < 512; i++) {
    if (np_open_vdirs[i] == (VDIR*)dirp) {
      vdir = (VDIR*)dirp;
      break;
    }
  }
  if (vdir != NULL) {
    vdir->map_ptr = (EMAP*)(&nuitka_embed_map);
  } else {
    rewinddir(dirp);
  }
}

#endif
