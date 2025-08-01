/*
# np_embed
# Inspired by c-embed by nicholas mcdonald 2022
*/

#ifndef NUITKAEMBED
#define NUITKAEMBED

#if !defined(__ASSEMBLER__) && !defined(BYPASS_NP_EMBED)

#ifdef __linux
# define _GNU_SOURCE
#endif

#ifdef FOPEN_MAX
// This means that we were loaded too late so we can't intercept the necessary calls.
// Don't even try in that case.
#define NP_STDIO_ALREADY_LOADED
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#define NOMINMAX
#include <wchar.h>
#include <basetsd.h>
#include <intsafe.h>
#if defined(_M_IX86)
#define _X86_
#elif defined(_M_X64)
#define _AMD64_
#elif defined(_M_ARM)
#define _ARM_
    #elif defined(_M_ARM64)
    #define _ARM64_
#endif

#include <fileapi.h>
#include <handleapi.h>
#include <windef.h>
#include <WinBase.h>
typedef int BOOL;
typedef const char *LPCSTR;
typedef const wchar_t *LPCWSTR;
typedef void *LPVOID;
typedef SSIZE_T ssize_t;
#else
    #include <dirent.h>
    #include <limits.h>
    #include <unistd.h>
    #include <libgen.h>
    #include <sys/statvfs.h>
#endif
#else

#ifdef _WIN32
#define _CRTIMP
#endif
#ifndef NUITKAPYTHON_EMBED_BUILD
#define open orig_open
#define _open orig__open
#define openat orig_openat
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
#define getline orig_getline
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

#define _stat32 orig__stat32
#define _wstat32 orig__wstat32
#define _stat64 orig__stat64
#define _wstat64 orig__wstat64
#define _stat32i64 orig__stat32i64
#define _wstat32i64 orig__wstat32i64
#define _stat64i32 orig__stat64i32
#define _wstat64i32 orig__wstat64i32
#define GetFileAttributesA orig_GetFileAttributesA
#define GetFileAttributesExA orig_GetFileAttributesExA
#define GetFileAttributesExW orig_GetFileAttributesExW
#define GetFileAttributesW orig_GetFileAttributesW
#define CloseHandle orig_CloseHandle
#define CreateFileA orig_CreateFileA
#define CreateFileW orig_CreateFileW
#define DeleteFileA orig_DeleteFileA
#define DeleteFileW orig_DeleteFileW
#define GetFileInformationByHandle orig_GetFileInformationByHandle
#define GetFileInformationByHandleEx orig_GetFileInformationByHandleEx
#define GetFileType orig_GetFileType
#define GetFinalPathNameByHandleW orig_GetFinalPathNameByHandleW
#define GetFullPathNameW orig_GetFullPathNameW
#define GetVolumePathNameW orig_GetVolumePathNameW
#define GetDiskFreeSpaceExW orig_GetDiskFreeSpaceExW
#define FindFirstFileA orig_FindFirstFileA
#define FindFirstFileW orig_FindFirstFileW
#define FindNextFileA orig_FindNextFileA
#define FindNextFileW orig_FindNextFileW
#define FindClose orig_FindClose
#define GetFileSizeEx orig_GetFileSizeEx
#define GetFileSize orig_GetFileSize
#define CreateFileMappingA orig_CreateFileMappingA
#define CreateFileMappingW orig_CreateFileMappingW
#define MapViewOfFile orig_MapViewOfFile
#define UnmapViewOfFile orig_UnmapViewOfFile
#define stat orig_stat
#define fstat orig_fstat
#define fstatat orig_fstatat
#define lstat orig_lstat
#define access orig_access
#define faccessat orig_faccessat
#define statvfs orig_statvfs
#define fstatvfs orig_fstatvfs
#define readv orig_readv
#define preadv orig_preadv
#define preadv2 orig_preadv2
#define writev orig_writev
#define pwritev orig_pwritev
#define pwritev2 orig_pwritev2
#define pwrite orig_pwrite
#define lseek orig_lseek
#define _lseek orig__lseek
#define _lseeki64 orig__lseeki64
#define opendir orig_opendir
#define fdopendir orig_fdopendir
#define readdir orig_readdir
#define closedir orig_closedir
#define rewinddir orig_rewinddir

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
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#define NOMINMAX
#include <wchar.h>
#include <basetsd.h>
#include <intsafe.h>
#if defined(_M_IX86)
#define _X86_
#elif defined(_M_X64)
#define _AMD64_
#elif defined(_M_ARM)
#define _ARM_
    #elif defined(_M_ARM64)
    #define _ARM64_
#endif

#include <fileapi.h>
#include <handleapi.h>
#include <windef.h>
#include <WinBase.h>
typedef int BOOL;
typedef const char *LPCSTR;
typedef const wchar_t *LPCWSTR;
typedef void *LPVOID;
typedef SSIZE_T ssize_t;
#else
#include <limits.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/uio.h>
#include <sys/statvfs.h>
#endif
#ifndef NUITKAPYTHON_EMBED_BUILD
#undef open
#undef _open
#undef openat
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
#undef getline
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
#undef _stat32
#undef _wstat32
#undef _stat64
#undef _wstat64
#undef _stat32i64
#undef _wstat32i64
#undef _stat64i32
#undef _wstat64i32
#undef GetFileAttributesA
#undef GetFileAttributesExA
#undef GetFileAttributesExW
#undef GetFileAttributesW
#undef CloseHandle
#undef CreateFileA
#undef CreateFileW
#undef DeleteFileA
#undef DeleteFileW
#undef GetFileInformationByHandle
#undef GetFileInformationByHandleEx
#undef GetFileType
#undef GetFinalPathNameByHandleW
#undef GetFullPathNameW
#undef GetVolumePathNameW
#undef GetDiskFreeSpaceExW
#undef FindFirstFileA
#undef FindFirstFileW
#undef FindNextFileA
#undef FindNextFileW
#undef FindClose
#undef GetFileSizeEx
#undef GetFileSize
#undef CreateFileMappingA
#undef CreateFileMappingW
#undef MapViewOfFile
#undef UnmapViewOfFile
#undef stat
#undef fstat
#undef fstatat
#undef lstat
#undef access
#undef faccessat
#undef statvfs
#undef fstatvfs
#undef readv
#undef preadv
#undef preadv2
#undef writev
#undef pwritev
#undef pwritev2
#undef pwrite
#undef lseek
#undef _lseek
#undef _lseeki64
#undef opendir
#undef fdopendir
#undef readdir
#undef closedir
#undef rewinddir

#ifdef __linux
#undef stdin
#undef stdout
#undef stderr
#endif
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
    EMAP* map;
};
typedef struct EFILE_S EFILE;

#define ETYPE_DIRECTORY 0
#define ETYPE_FILE 1

#define EHANDLE_VIRTUAL 0x11111111
#define EHANDLE_NATIVE 0xFFFFFFFF

#ifdef _WIN32
#define NP_DECL(x) _ACRTIMP x __cdecl
#define NP_STD(x) _ACRTIMP x __stdcall
#else
#define NP_DECL(x) x
#endif

#if defined(_MSC_VER)
#define ALWAYS_INLINE static __forceinline
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
#define NP_FOREIGN_PTR e == NULL || *(void**)e == NULL || (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE)
#else
#define NP_FOREIGN_PTR ((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE
#endif

#if !defined(NUITKAPYTHON_EMBED_BUILD) && !defined(NP_STDIO_ALREADY_LOADED)
#if defined(_WIN32)
struct _stat32
{
    _dev_t         st_dev;
    _ino_t         st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    _dev_t         st_rdev;
    _off_t         st_size;
    __time32_t     st_atime;
    __time32_t     st_mtime;
    __time32_t     st_ctime;
};

struct _stat32i64
{
    _dev_t         st_dev;
    _ino_t         st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    _dev_t         st_rdev;
    __int64        st_size;
    __time32_t     st_atime;
    __time32_t     st_mtime;
    __time32_t     st_ctime;
};

struct _stat64i32
{
    _dev_t         st_dev;
    _ino_t         st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    _dev_t         st_rdev;
    _off_t         st_size;
    __time64_t     st_atime;
    __time64_t     st_mtime;
    __time64_t     st_ctime;
};

struct _stat64
{
    _dev_t         st_dev;
    _ino_t         st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    _dev_t         st_rdev;
    __int64        st_size;
    __time64_t     st_atime;
    __time64_t     st_mtime;
    __time64_t     st_ctime;
};

#define __stat64 _stat64 // For legacy compatibility

struct stat
{
    _dev_t         st_dev;
    _ino_t         st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    _dev_t         st_rdev;
    _off_t         st_size;
    time_t         st_atime;
    time_t         st_mtime;
    time_t         st_ctime;
};
#ifdef _USE_32BIT_TIME_T
    #define _stat       _stat32
    #define _stati64    _stat32i64
    #define _wstat      _wstat32
    #define _wstati64   _wstat32i64
#else
    #define _stat       _stat64i32
    #define _stati64    _stat64
    #define _wstat      _wstat64i32
    #define _wstati64   _wstat64
#endif
#endif  // _WIN32
#ifdef __APPLE__
#if __DARWIN_64_BIT_INO_T

struct stat {
    dev_t st_dev;
    mode_t st_mode;
    nlink_t st_nlink;
    __darwin_ino64_t st_ino;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
    struct  timespec st_atimespec;  /* time of last access */
    struct  timespec st_mtimespec;  /* time of last data modification */
    struct  timespec st_ctimespec;  /* time of last status change */
    struct  timespec st_birthtimespec;  /* time of file creation(birth) */
#else
    time_t          st_atime;       /* [XSI] Time of last access */
	long            st_atimensec;   /* nsec of last access */
	time_t          st_mtime;       /* [XSI] Last data modification time */
	long            st_mtimensec;   /* last data modification nsec */
	time_t          st_ctime;       /* [XSI] Time of last status change */
	long            st_ctimensec;   /* nsec of last status change */
	time_t		    st_birthtime;       /*  File creation time(birth)  */
	long		    st_birthtimensec;   /* nsec of File creation time */
#endif
    off_t st_size;
    blkcnt_t st_blocks;
    blksize_t st_blksize;
    __uint32_t st_flags;
    __uint32_t st_gen;
    __int32_t st_lspare;
    __int64_t st_qspare[2];
};

#else /* !__DARWIN_64_BIT_INO_T */

struct stat {
	dev_t           st_dev;         /* [XSI] ID of device containing file */
	ino_t           st_ino;         /* [XSI] File serial number */
	mode_t          st_mode;        /* [XSI] Mode of file (see below) */
	nlink_t         st_nlink;       /* [XSI] Number of hard links */
	uid_t           st_uid;         /* [XSI] User ID of the file */
	gid_t           st_gid;         /* [XSI] Group ID of the file */
	dev_t           st_rdev;        /* [XSI] Device ID */
#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)
	struct  timespec st_atimespec;  /* time of last access */
	struct  timespec st_mtimespec;  /* time of last data modification */
	struct  timespec st_ctimespec;  /* time of last status change */
#else
	time_t          st_atime;       /* [XSI] Time of last access */
	long            st_atimensec;   /* nsec of last access */
	time_t          st_mtime;       /* [XSI] Last data modification time */
	long            st_mtimensec;   /* last data modification nsec */
	time_t          st_ctime;       /* [XSI] Time of last status change */
	long            st_ctimensec;   /* nsec of last status change */
#endif
	off_t           st_size;        /* [XSI] file size, in bytes */
	blkcnt_t        st_blocks;      /* [XSI] blocks allocated for file */
	blksize_t       st_blksize;     /* [XSI] optimal blocksize for I/O */
	__uint32_t      st_flags;       /* user defined flags for file */
	__uint32_t      st_gen;         /* file generation number */
	__int32_t       st_lspare;      /* RESERVED: DO NOT USE! */
	__int64_t       st_qspare[2];   /* RESERVED: DO NOT USE! */
};

#endif /* __DARWIN_64_BIT_INO_T */

struct statvfs {
	unsigned long	f_bsize;	/* File system block size */
	unsigned long	f_frsize;	/* Fundamental file system block size */
	fsblkcnt_t	f_blocks;	/* Blocks on FS in units of f_frsize */
	fsblkcnt_t	f_bfree;	/* Free blocks */
	fsblkcnt_t	f_bavail;	/* Blocks available to non-root */
	fsfilcnt_t	f_files;	/* Total inodes */
	fsfilcnt_t	f_ffree;	/* Free inodes */
	fsfilcnt_t	f_favail;	/* Free inodes for non-root */
	unsigned long	f_fsid;		/* Filesystem ID */
	unsigned long	f_flag;		/* Bit mask of values */
	unsigned long	f_namemax;	/* Max file name length */
};
#endif
#ifdef __linux
struct stat
{
  __dev_t st_dev;		/* Device.  */
#ifndef __x86_64__
  unsigned short int __pad1;
#endif
#if defined __x86_64__ || !defined __USE_FILE_OFFSET64
  __ino_t st_ino;		/* File serial number.	*/
#else
  __ino_t __st_ino;			/* 32bit file serial number.	*/
#endif
#ifndef __x86_64__
  __mode_t st_mode;			/* File mode.  */
    __nlink_t st_nlink;			/* Link count.  */
#else
  __nlink_t st_nlink;		/* Link count.  */
  __mode_t st_mode;		/* File mode.  */
#endif
  __uid_t st_uid;		/* User ID of the file's owner.	*/
  __gid_t st_gid;		/* Group ID of the file's group.*/
#ifdef __x86_64__
  int __pad0;
#endif
  __dev_t st_rdev;		/* Device number, if device.  */
#ifndef __x86_64__
  unsigned short int __pad2;
#endif
#if defined __x86_64__ || !defined __USE_FILE_OFFSET64
  __off_t st_size;			/* Size of file, in bytes.  */
#else
  __off64_t st_size;			/* Size of file, in bytes.  */
#endif
  __blksize_t st_blksize;	/* Optimal block size for I/O.  */
#if defined __x86_64__  || !defined __USE_FILE_OFFSET64
  __blkcnt_t st_blocks;		/* Number 512-byte blocks allocated. */
#else
  __blkcnt64_t st_blocks;		/* Number 512-byte blocks allocated. */
#endif
#ifdef __USE_XOPEN2K8
  /* Nanosecond resolution timestamps are stored in a format
     equivalent to 'struct timespec'.  This is the type used
     whenever possible but the Unix namespace rules do not allow the
     identifier 'timespec' to appear in the <sys/stat.h> header.
     Therefore we have to handle the use of this header in strictly
     standard-compliant sources special.  */
  struct timespec st_atim;		/* Time of last access.  */
  struct timespec st_mtim;		/* Time of last modification.  */
  struct timespec st_ctim;		/* Time of last status change.  */
# define st_atime st_atim.tv_sec	/* Backward compatibility.  */
# define st_mtime st_mtim.tv_sec
# define st_ctime st_ctim.tv_sec
#else
  __time_t st_atime;			/* Time of last access.  */
  __syscall_ulong_t st_atimensec;	/* Nscecs of last access.  */
  __time_t st_mtime;			/* Time of last modification.  */
  __syscall_ulong_t st_mtimensec;	/* Nsecs of last modification.  */
  __time_t st_ctime;			/* Time of last status change.  */
  __syscall_ulong_t st_ctimensec;	/* Nsecs of last status change.  */
#endif
#ifdef __x86_64__
  __syscall_slong_t __glibc_reserved[3];
#else
# ifndef __USE_FILE_OFFSET64
  unsigned long int __glibc_reserved4;
  unsigned long int __glibc_reserved5;
# else
  __ino64_t st_ino;			/* File serial number.	*/
# endif
#endif
};
struct statvfs
{
  unsigned long int f_bsize;
  unsigned long int f_frsize;
#ifndef __USE_FILE_OFFSET64
  __fsblkcnt_t f_blocks;
  __fsblkcnt_t f_bfree;
  __fsblkcnt_t f_bavail;
  __fsfilcnt_t f_files;
  __fsfilcnt_t f_ffree;
  __fsfilcnt_t f_favail;
#else
  __fsblkcnt64_t f_blocks;
  __fsblkcnt64_t f_bfree;
  __fsblkcnt64_t f_bavail;
  __fsfilcnt64_t f_files;
  __fsfilcnt64_t f_ffree;
  __fsfilcnt64_t f_favail;
#endif
  unsigned long int f_fsid;
#ifdef _STATVFSBUF_F_UNUSED
  int __f_unused;
#endif
  unsigned long int f_flag;
  unsigned long int f_namemax;
  unsigned int f_type;
  int __f_spare[5];
};
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

NP_DECL(EFILE*) np_fopen(const char* file, const char* mode);
#ifdef _WIN32
NP_DECL(int) np_open(const char *pathname, int flags, int mode);
NP_DECL(EFILE*) np_wfopen(const wchar_t *wfile, const wchar_t *mode);
NP_DECL(int) np_wopen(const wchar_t *pathname, int flags, int mode);
#else
NP_DECL(int) np_open(const char *pathname, int flags, mode_t mode);
NP_DECL(int) np_openat(int dirfd, const char* pathname, int flags, mode_t mode);
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
#ifndef _WIN32
NP_DECL(ssize_t) np_getline(char **lineptr, size_t *n, void *stream);
#endif
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
#ifdef _WIN32
NP_DECL(__int64) np_lseeki64(int fd, __int64 offset, int whence);
#else
NP_DECL(ssize_t) np_lseek(int fd, off_t offset, int whence);
#endif

/* Error Handling & Other Utilities */
NP_DECL(void) np_clearerr(void *stream);
NP_DECL(int) np_ferror(void *stream);
NP_DECL(int) np_fileno(void *stream);
NP_DECL(int) np_fflush(void *stream);

/* Locking Functions */
NP_DECL(void) np_flockfile(void *e);
NP_DECL(void) np_funlockfile(void *e);
NP_DECL(int) np_ftrylockfile(void *e);

#ifdef _WIN32
NP_DECL(int) np__stat32(const char *path, struct __stat32 *buffer);
NP_DECL(int) np__stat64(const char *path, struct __stat64 *buffer);
NP_DECL(int) np__stat32i64(const char *path, struct _stat32i64 *buffer);
NP_DECL(int) np__stat64i32(const char *path, struct _stat64i32 *buffer);
NP_DECL(int) np__wstat32(const wchar_t *path, struct __stat32 *buffer);
NP_DECL(int) np__wstat64(const wchar_t *path, struct __stat64 *buffer);
NP_DECL(int) np__wstat32i64(const wchar_t *path, struct _stat32i64 *buffer);
NP_DECL(int) np__wstat64i32(const wchar_t *path, struct _stat64i32 *buffer);
NP_DECL(DWORD) np_GetFileAttributesA(LPCSTR lpFileName);
NP_DECL(BOOL) np_GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
NP_DECL(BOOL) np_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
NP_DECL(DWORD) np_GetFileAttributesW(LPCWSTR lpFileName);
NP_STD(BOOL) np_CloseHandle(HANDLE hObject);
NP_STD(HANDLE) np_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
NP_STD(HANDLE) np_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
NP_STD(BOOL) np_DeleteFileA(LPCSTR lpFileName);
NP_STD(BOOL) np_DeleteFileW(LPCWSTR lpFileName);
NP_STD(BOOL) np_GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
NP_STD(BOOL) np_GetFileInformationByHandleEx(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize);
NP_STD(DWORD) np_GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags);
NP_STD(DWORD) np_GetFileType(HANDLE hFile);
NP_STD(DWORD) np_GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart);
NP_STD(BOOL) np_GetVolumePathNameW(LPCWSTR lpszFileName, LPWSTR lpszVolumePathName, DWORD cchBufferLength);
NP_STD(BOOL) np_GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes);
NP_STD(HANDLE) np_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
NP_STD(HANDLE) np_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
NP_STD(BOOL) np_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
NP_STD(BOOL) np_FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
NP_STD(BOOL) np_FindClose(HANDLE hFindFile);
NP_STD(BOOL) np_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
NP_STD(DWORD) np_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
NP_STD(HANDLE) np_CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
NP_STD(HANDLE) np_CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName);
NP_STD(LPVOID) np_MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
NP_STD(BOOL) np_UnmapViewOfFile(LPCVOID lpBaseAddress);
#else
NP_DECL(int) np_stat(const char *path, struct stat *buf);
NP_DECL(int) np_fstat(int fd, struct stat *buf);
NP_DECL(int) np_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags);
NP_DECL(int) np_lstat(const char *path, struct stat *buf);
NP_DECL(int) np_access(const char *pathname, int mode);
NP_DECL(int) np_faccessat(int dirfd, const char *pathname, int mode, int flags);
NP_DECL(int) np_statvfs(const char *path, struct statvfs *buf);
NP_DECL(int) np_fstatvfs(int fd, struct statvfs *buf);
NP_DECL(ssize_t) np_readv(int fd, const struct iovec *iov, int iovcnt);
NP_DECL(ssize_t) np_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
NP_DECL(ssize_t) np_preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
NP_DECL(ssize_t) np_writev(int fd, const struct iovec *iov, int iovcnt);
NP_DECL(ssize_t) np_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
NP_DECL(ssize_t) np_pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
NP_DECL(ssize_t) np_pwrite(int fd, const void *buf, size_t count, off_t offset);
NP_DECL(DIR*) np_opendir(const char* name);
NP_DECL(DIR*) np_fdopendir(int fd);
NP_DECL(struct dirent*) np_readdir(DIR *dirp);
NP_DECL(int) np_closedir(DIR *dirp);
NP_DECL(void) np_rewinddir(DIR *dirp);
#endif

#if !defined(NUITKAPYTHON_EMBED_BUILD) && !defined(NP_STDIO_ALREADY_LOADED)

#if (defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)) || defined(__APPLE__)
#define NP_EMBED_CAT(a, ...) NP_EMBED_PRIMITIVE_CAT(a, __VA_ARGS__)
#define NP_EMBED_PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

#define NP_EMBED_IIF(c) NP_EMBED_PRIMITIVE_CAT(NP_EMBED_IIF_, c)
#define NP_EMBED_IIF_0(t, ...) __VA_ARGS__
#define NP_EMBED_IIF_1(t, ...) t

#define NP_EMBED_COMPL(b) NP_EMBED_PRIMITIVE_CAT(NP_EMBED_COMPL_, b)
#define NP_EMBED_COMPL_0 1
#define NP_EMBED_COMPL_1 0

#define NP_EMBED_INC(x) NP_EMBED_PRIMITIVE_CAT(NP_EMBED_INC_, x)
#define NP_EMBED_INC_0 1
#define NP_EMBED_INC_1 2
#define NP_EMBED_INC_2 3
#define NP_EMBED_INC_3 4
#define NP_EMBED_INC_4 5
#define NP_EMBED_INC_5 6
#define NP_EMBED_INC_6 7
#define NP_EMBED_INC_7 8
#define NP_EMBED_INC_8 9
#define NP_EMBED_INC_9 10
#define NP_EMBED_INC_10 11
#define NP_EMBED_INC_11 12
#define NP_EMBED_INC_12 13
#define NP_EMBED_INC_13 14
#define NP_EMBED_INC_14 15
#define NP_EMBED_INC_15 16
#define NP_EMBED_INC_16 17
#define NP_EMBED_INC_17 18
#define NP_EMBED_INC_18 19
#define NP_EMBED_INC_19 20

#define NP_EMBED_DEC(x) NP_EMBED_PRIMITIVE_CAT(NP_EMBED_DEC_, x)
#define NP_EMBED_DEC_0 0
#define NP_EMBED_DEC_1 0
#define NP_EMBED_DEC_2 1
#define NP_EMBED_DEC_3 2
#define NP_EMBED_DEC_4 3
#define NP_EMBED_DEC_5 4
#define NP_EMBED_DEC_6 5
#define NP_EMBED_DEC_7 6
#define NP_EMBED_DEC_8 7
#define NP_EMBED_DEC_9 8
#define NP_EMBED_DEC_10 9
#define NP_EMBED_DEC_11 10
#define NP_EMBED_DEC_12 11
#define NP_EMBED_DEC_13 12
#define NP_EMBED_DEC_14 13
#define NP_EMBED_DEC_15 14
#define NP_EMBED_DEC_16 15
#define NP_EMBED_DEC_17 16
#define NP_EMBED_DEC_18 17
#define NP_EMBED_DEC_19 18
#define NP_EMBED_DEC_20 19

#define NP_EMBED_CHECK_N(x, n, ...) n
#define NP_EMBED_CHECK(...) NP_EMBED_CHECK_N(__VA_ARGS__, 0,)
#define NP_EMBED_PROBE(x) x, 1,

#define NP_EMBED_IS_PAREN(x) NP_EMBED_CHECK(NP_EMBED_IS_PAREN_PROBE x)
#define NP_EMBED_IS_PAREN_PROBE(...) NP_EMBED_PROBE(~)

#define NP_EMBED_NOT(x) NP_EMBED_CHECK(NP_EMBED_PRIMITIVE_CAT(NP_EMBED_NOT_, x))
#define NP_EMBED_NOT_0 NP_EMBED_PROBE(~)

#define NP_EMBED_BOOL(x) NP_EMBED_COMPL(NP_EMBED_NOT(x))
#define NP_EMBED_IF(c) NP_EMBED_IIF(NP_EMBED_BOOL(c))

#define NP_EMBED_EAT(...)
#define NP_EMBED_EXPAND(...) __VA_ARGS__
#define NP_EMBED_WHEN(c) NP_EMBED_IF(c)(NP_EMBED_EXPAND, NP_EMBED_EAT)

#define NP_EMBED_EMPTY()
#define NP_EMBED_DEFER(id) id NP_EMBED_EMPTY()
#define NP_EMBED_OBSTRUCT(...) __VA_ARGS__ NP_EMBED_DEFER(NP_EMBED_EMPTY)()

#define NP_EMBED_NUM_ARGS1(_20,_19,_18,_17,_16,_15,_14,_13,_12,_11,_10,_9,_8,_7,_6,_5,_4,_3,_2,_1, n, ...) n
#define NP_EMBED_NUM_ARGS0(...) NP_EMBED_NUM_ARGS1(__VA_ARGS__,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define NP_EMBED_NUM_ARGS(...) NP_EMBED_IF(NP_EMBED_DEC(NP_EMBED_NUM_ARGS0(__VA_ARGS__)))(NP_EMBED_NUM_ARGS0(__VA_ARGS__),NP_EMBED_IF(NP_EMBED_IS_PAREN(__VA_ARGS__ ()))(0,1))
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

#if (defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)) || defined(__APPLE__)
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
#ifdef __cplusplus
ALWAYS_INLINE NP_DECL(int) openat(int dirfd, const char *pathname, int flags, mode_t mode = 0) {
#else
ALWAYS_INLINE NP_DECL(int) openat(int dirfd, const char *pathname, int flags, mode_t mode) {
#endif
  return np_openat(dirfd, pathname, flags, mode);
}
#ifndef __cplusplus
// C does not support optional parameters so we are forced to rely on the mess below
// because GCC does not support inlining vararg functions. :(
#define NP_EMBED_open0() open()
#define NP_EMBED_open1(a) open(a)
#define NP_EMBED_open2(a, b) open(a, b, 0)
#define NP_EMBED_open3(a, b, c) open(a, b, c)
#define NP_EMBED_open4(a, b, c, d) open(a, b, c, d)
#define NP_EMBED_open5(a, b, c, d, e) open(a, b, c, d, e)
#define NP_EMBED_open6(a, b, c, d, e, f) open(a, b, c, d, e, f)
#define NP_EMBED_open7(a, b, c, d, e, f, g) open(a, b, c, d, e, f, g)
#define open(...) NP_EMBED_CAT( NP_EMBED_open, NP_EMBED_NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )
#define _open(...) NP_EMBED_CAT( NP_EMBED_open, NP_EMBED_NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )

#define NP_EMBED_openat0() openat()
#define NP_EMBED_openat1(a) openat(a)
#define NP_EMBED_openat2(a, b) openat(a, b, 0)
#define NP_EMBED_openat3(a, b, c) openat(a, b, c)
#define NP_EMBED_openat4(a, b, c, d) openat(a, b, c, d)
#define NP_EMBED_openat5(a, b, c, d, e) openat(a, b, c, d, e)
#define NP_EMBED_openat6(a, b, c, d, e, f) openat(a, b, c, d, e, f)
#define NP_EMBED_openat7(a, b, c, d, e, f, g) openat(a, b, c, d, e, f, g)
#define openat(...) NP_EMBED_CAT( NP_EMBED_openat, NP_EMBED_NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )
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
#ifndef _WIN32
ALWAYS_INLINE NP_DECL(int) openat(int dirfd, const char *pathname, int flags, ... /* mode_t mode */ ) {
    va_list args;
    va_start(args, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(args, int); // mode_t is promoted to int in varargs
    }
    va_end(args);
    return np_openat(dirfd, pathname, flags, mode);
}
#endif
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

#ifndef _WIN32
ALWAYS_INLINE NP_DECL(ssize_t) getline(char **lineptr, size_t *n, void *stream) {
    return np_getline(lineptr, n, stream);
}
#endif

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
ALWAYS_INLINE NP_DECL(long) lseek(int fd, long offset, int whence) {
    return (long)np_lseeki64(fd, offset, whence);
}
ALWAYS_INLINE NP_DECL(long) _lseek(int fd, long offset, int whence) {
    return (long)np_lseeki64(fd, offset, whence);
}
ALWAYS_INLINE NP_DECL(__int64) _lseeki64(int fd, __int64 offset, int whence) {
    return np_lseeki64(fd, offset, whence);
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
ALWAYS_INLINE NP_DECL(off_t) lseek(int fd, off_t offset, int whence) {
    return np_lseek(fd, offset, whence);
}
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

#ifdef _WIN32
ALWAYS_INLINE NP_DECL(int) _stat32(const char *path, struct __stat32 *buffer) {
    return np__stat32(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _stat64(const char *path, struct __stat64 *buffer) {
    return np__stat64(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _stat32i64(const char *path, struct _stat32i64 *buffer) {
    return np__stat32i64(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _stat64i32(const char *path, struct _stat64i32 *buffer) {
    return np__stat64i32(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _wstat32(const wchar_t *path, struct __stat32 *buffer) {
    return np__wstat32(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _wstat64(const wchar_t *path, struct __stat64 *buffer) {
    return np__wstat64(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _wstat32i64(const wchar_t *path, struct _stat32i64 *buffer) {
    return np__wstat32i64(path, buffer);
}

ALWAYS_INLINE NP_DECL(int) _wstat64i32(const wchar_t *path, struct _stat64i32 *buffer) {
    return np__wstat64i32(path, buffer);
}

ALWAYS_INLINE NP_STD(DWORD) GetFileAttributesA(_In_ LPCSTR lpFileName) {
    return np_GetFileAttributesA(lpFileName);
}

ALWAYS_INLINE NP_STD(BOOL) GetFileAttributesExA(
        _In_ LPCSTR lpFileName,
        _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
        _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation) {
    return np_GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

ALWAYS_INLINE NP_STD(BOOL) GetFileAttributesExW(
        _In_ LPCWSTR lpFileName,
        _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
        _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation) {
    return np_GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

ALWAYS_INLINE NP_STD(DWORD) GetFileAttributesW(_In_ LPCWSTR lpFileName) {
    return np_GetFileAttributesW(lpFileName);
}

ALWAYS_INLINE NP_STD(BOOL) CloseHandle(
    _In_ HANDLE hObject
) {
    return np_CloseHandle(hObject);
}

ALWAYS_INLINE NP_STD(HANDLE) CreateFileA(
        _In_     LPCSTR                lpFileName,
        _In_     DWORD                 dwDesiredAccess,
        _In_     DWORD                 dwShareMode,
        _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        _In_     DWORD                 dwCreationDisposition,
        _In_     DWORD                 dwFlagsAndAttributes,
        _In_opt_ HANDLE                hTemplateFile
) {
    return np_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

ALWAYS_INLINE NP_STD(HANDLE) CreateFileW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile
) {
    return np_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

ALWAYS_INLINE NP_STD(BOOL) DeleteFileA(
        _In_ LPCSTR lpFileName
) {
    return np_DeleteFileA(lpFileName);
}

ALWAYS_INLINE NP_STD(BOOL) DeleteFileW(
        _In_ LPCWSTR lpFileName
) {
    return np_DeleteFileW(lpFileName);
}

ALWAYS_INLINE NP_STD(BOOL) GetFileInformationByHandle(
    _In_ HANDLE hFile,
    _Out_ LPBY_HANDLE_FILE_INFORMATION lpFileInformation
) {
    return np_GetFileInformationByHandle(hFile, lpFileInformation);
}

ALWAYS_INLINE NP_STD(BOOL) GetFileInformationByHandleEx(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _Out_writes_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize
) {
    return np_GetFileInformationByHandleEx(hFile, FileInformationClass, lpFileInformation, dwBufferSize);
}

ALWAYS_INLINE NP_STD(DWORD) GetFileType(
    _In_ HANDLE hFile
) {
    return np_GetFileType(hFile);
}

ALWAYS_INLINE NP_STD(DWORD) GetFinalPathNameByHandleW(
    _In_ HANDLE hFile,
    _Out_writes_(cchFilePath) LPWSTR lpszFilePath,
    _In_ DWORD cchFilePath,
    _In_ DWORD dwFlags
) {
    return np_GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

/**
 * @brief Inline wrapper for GetFullPathNameW to call our implementation.
 */
ALWAYS_INLINE NP_STD(DWORD) GetFullPathNameW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD nBufferLength,
    _Out_writes_to_(nBufferLength, return + 1) LPWSTR lpBuffer,
    _Out_opt_ LPWSTR *lpFilePart
) {
    return np_GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

/**
 * @brief Inline wrapper for GetVolumePathNameW to call our implementation.
 */
ALWAYS_INLINE NP_STD(BOOL) GetVolumePathNameW(
    _In_ LPCWSTR lpszFileName,
    _Out_writes_to_(cchBufferLength, return != 0 ? wcslen(lpszVolumePathName) + 1 : 0) LPWSTR lpszVolumePathName,
    _In_ DWORD cchBufferLength
) {
    return np_GetVolumePathNameW(lpszFileName, lpszVolumePathName, cchBufferLength);
}

/**
 * @brief Inline wrapper for GetDiskFreeSpaceExW to call our implementation.
 */
ALWAYS_INLINE NP_STD(BOOL) GetDiskFreeSpaceExW(
    _In_opt_ LPCWSTR lpDirectoryName,
    _Out_opt_ PULARGE_INTEGER lpFreeBytesAvailableToCaller,
    _Out_opt_ PULARGE_INTEGER lpTotalNumberOfBytes,
    _Out_opt_ PULARGE_INTEGER lpTotalNumberOfFreeBytes
) {
    return np_GetDiskFreeSpaceExW(lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

ALWAYS_INLINE NP_STD(HANDLE) FindFirstFileA(
        _In_  LPCSTR             lpFileName,
        _Out_ LPWIN32_FIND_DATAA lpFindFileData
) {
    return np_FindFirstFileA(lpFileName, lpFindFileData);
}

/**
 * @brief Inline wrapper for FindFirstFileW to call our implementation.
 */
ALWAYS_INLINE NP_STD(HANDLE) FindFirstFileW(
    _In_ LPCWSTR lpFileName,
    _Out_ LPWIN32_FIND_DATAW lpFindFileData
) {
    return np_FindFirstFileW(lpFileName, lpFindFileData);
}

ALWAYS_INLINE NP_STD(BOOL) FindNextFileA(
        _In_  HANDLE             hFindFile,
        _Out_ LPWIN32_FIND_DATAA lpFindFileData
) {
    return np_FindNextFileA(hFindFile, lpFindFileData);
}

/**
 * @brief Inline wrapper for FindNextFileW to call our implementation.
 */
ALWAYS_INLINE NP_STD(BOOL) FindNextFileW(
    _In_ HANDLE hFindFile,
    _Out_ LPWIN32_FIND_DATAW lpFindFileData
) {
    return np_FindNextFileW(hFindFile, lpFindFileData);
}

/**
 * @brief Inline wrapper for FindClose to call our implementation.
 */
ALWAYS_INLINE NP_STD(BOOL) FindClose(
    _Inout_ HANDLE hFindFile
) {
    return np_FindClose(hFindFile);
}

/**
 * @brief Retrieves the size of the specified file, in bytes.
 * @param hFile A handle to the file.
 * @param lpFileSize A pointer to a variable that receives the file size.
 * @return Nonzero on success, zero on failure.
 */
ALWAYS_INLINE NP_STD(BOOL) GetFileSizeEx(
        _In_  HANDLE         hFile,
        _Out_ PLARGE_INTEGER lpFileSize
) {
    return np_GetFileSizeEx(hFile, lpFileSize);
}

/**
 * @brief Retrieves the size of the specified file.
 * @param hFile A handle to the file.
 * @param lpFileSizeHigh A pointer to the high-order doubleword of the file size.
 * @return The low-order doubleword of the file size, or INVALID_FILE_SIZE.
 */
ALWAYS_INLINE NP_STD(DWORD) GetFileSize(
        _In_      HANDLE  hFile,
        _Out_opt_ LPDWORD lpFileSizeHigh
) {
    return np_GetFileSize(hFile, lpFileSizeHigh);
}

ALWAYS_INLINE NP_STD(HANDLE) CreateFileMappingA(
        _In_     HANDLE                hFile,
        _In_opt_ LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
        _In_     DWORD                 flProtect,
        _In_     DWORD                 dwMaximumSizeHigh,
        _In_     DWORD                 dwMaximumSizeLow,
        _In_opt_ LPCSTR                lpName
) {
    return np_CreateFileMappingA(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
}

ALWAYS_INLINE NP_STD(HANDLE) CreateFileMappingW(
        _In_     HANDLE                hFile,
        _In_opt_ LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
        _In_     DWORD                 flProtect,
        _In_     DWORD                 dwMaximumSizeHigh,
        _In_     DWORD                 dwMaximumSizeLow,
        _In_opt_ LPCWSTR               lpName
) {
    return np_CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
}


/**
 * @brief Maps a view of a file mapping into the address space of a calling process.
 * @return The starting address of the mapped view on success, NULL on failure.
 */
ALWAYS_INLINE NP_STD(LPVOID) MapViewOfFile(
        _In_ HANDLE hFileMappingObject,
        _In_ DWORD  dwDesiredAccess,
        _In_ DWORD  dwFileOffsetHigh,
        _In_ DWORD  dwFileOffsetLow,
        _In_ SIZE_T dwNumberOfBytesToMap
) {
    return np_MapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
}

/**
 * @brief Unmaps a mapped view of a file from the calling process's address space.
 * @return Nonzero on success, zero on failure.
 */
ALWAYS_INLINE NP_STD(BOOL) UnmapViewOfFile(
        _In_ LPCVOID lpBaseAddress
) {
    return np_UnmapViewOfFile(lpBaseAddress);
}

#else
ALWAYS_INLINE NP_DECL(int) stat(const char *path, struct stat *buf) {
    return np_stat(path, buf);
}

ALWAYS_INLINE NP_DECL(int) fstat(int fd, struct stat *buf) {
    return np_fstat(fd, buf);
}

ALWAYS_INLINE NP_DECL(int) fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    return np_fstatat(dirfd, pathname, buf, flags);
}

ALWAYS_INLINE NP_DECL(int) lstat(const char *path, struct stat *buf) {
    return np_lstat(path, buf);
}

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

ALWAYS_INLINE NP_DECL(int) access(const char *pathname, int mode) {
    return np_access(pathname, mode);
}

ALWAYS_INLINE NP_DECL(int) faccessat(int dirfd, const char *pathname, int mode, int flags) {
    return np_faccessat(dirfd, pathname, mode, flags);
}

ALWAYS_INLINE NP_DECL(int) statvfs(const char *path, struct statvfs *buf) {
    return np_statvfs(path, buf);
}

ALWAYS_INLINE NP_DECL(int) fstatvfs(int fd, struct statvfs *buf) {
    return np_fstatvfs(fd, buf);
}

ALWAYS_INLINE NP_DECL(ssize_t) readv(int fd, const struct iovec *iov, int iovcnt) {
    return np_readv(fd, iov, iovcnt);
}

ALWAYS_INLINE NP_DECL(ssize_t) preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    return np_preadv(fd, iov, iovcnt, offset);
}

#ifdef __linux // preadv2 and pwritev2 are GNU extensions
ALWAYS_INLINE NP_DECL(ssize_t) preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
    return np_preadv2(fd, iov, iovcnt, offset, flags);
}
#endif

ALWAYS_INLINE NP_DECL(ssize_t) writev(int fd, const struct iovec *iov, int iovcnt) {
    return np_writev(fd, iov, iovcnt);
}

ALWAYS_INLINE NP_DECL(ssize_t) pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    return np_pwritev(fd, iov, iovcnt, offset);
}

#ifdef __linux // preadv2 and pwritev2 are GNU extensions
ALWAYS_INLINE NP_DECL(ssize_t) pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
    return np_pwritev2(fd, iov, iovcnt, offset, flags);
}
#endif

ALWAYS_INLINE NP_DECL(ssize_t) pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return np_pwrite(fd, buf, count, offset);
}

ALWAYS_INLINE NP_DECL(DIR *) opendir(const char *name) {
    return np_opendir(name);
}

ALWAYS_INLINE NP_DECL(DIR *) fdopendir(int fd) {
    return np_fdopendir(fd);
}

ALWAYS_INLINE NP_DECL(struct dirent *) readdir(DIR *dirp) {
    return np_readdir(dirp);
}

ALWAYS_INLINE NP_DECL(int) closedir(DIR *dirp) {
    return np_closedir(dirp);
}

ALWAYS_INLINE NP_DECL(void) rewinddir(DIR *dirp) {
    np_rewinddir(dirp);
}
#endif

#endif

#ifdef __cplusplus
}

#if defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)
#ifndef _GLIBCXX_CSTDIO
#define _GLIBCXX_CSTDIO 1

#include <bits/c++config.h>
namespace std
{
  using ::FILE;
  using ::fpos_t;

  using ::clearerr;
  using ::fclose;
  using ::feof;
  using ::ferror;
  using ::fflush;
  using ::fgetc;
  using ::fgetpos;
  using ::fgets;
  using ::fopen;
  using ::fprintf;
  using ::fputc;
  using ::fputs;
  using ::fread;
  using ::freopen;
  using ::fscanf;
  using ::fseek;
  using ::fsetpos;
  using ::ftell;
  using ::fwrite;
  using ::getc;
  using ::getchar;
#if __cplusplus <= 201103L
  // LWG 2249
  using ::gets;
#endif
  using ::perror;
  using ::printf;
  using ::putc;
  using ::putchar;
  using ::puts;
  using ::remove;
  using ::rename;
  using ::rewind;
  using ::scanf;
  using ::setbuf;
  using ::setvbuf;
  using ::sprintf;
  using ::sscanf;
  using ::tmpfile;
#if _GLIBCXX_USE_TMPNAM
  using ::tmpnam;
#endif
  using ::ungetc;
  using ::vfprintf;
  using ::vprintf;
  using ::vsprintf;
} // namespace

#if _GLIBCXX_USE_C99_STDIO

namespace __gnu_cxx
{
#if _GLIBCXX_USE_C99_CHECK || _GLIBCXX_USE_C99_DYNAMIC
  extern "C" int
  (snprintf)(char * __restrict, std::size_t, const char * __restrict, ...)
  throw ();
  extern "C" int
  (vfscanf)(FILE * __restrict, const char * __restrict, __gnuc_va_list);
  extern "C" int (vscanf)(const char * __restrict, __gnuc_va_list);
  extern "C" int
  (vsnprintf)(char * __restrict, std::size_t, const char * __restrict,
	      __gnuc_va_list) throw ();
  extern "C" int
  (vsscanf)(const char * __restrict, const char * __restrict, __gnuc_va_list)
  throw ();
#endif

#if !_GLIBCXX_USE_C99_DYNAMIC
  using ::snprintf;
  using ::vfscanf;
  using ::vscanf;
  using ::vsnprintf;
  using ::vsscanf;
#endif
} // namespace __gnu_cxx

namespace std
{
  using ::__gnu_cxx::snprintf;
  using ::__gnu_cxx::vfscanf;
  using ::__gnu_cxx::vscanf;
  using ::__gnu_cxx::vsnprintf;
  using ::__gnu_cxx::vsscanf;
} // namespace std

#endif // _GLIBCXX_USE_C99_STDIO

#endif
#endif
#endif

#endif
#endif
