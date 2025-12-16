/*
# mp_embed
# Inspired by c-embed by nicholas mcdonald 2022
*/

#ifndef MONOLITHPYEMBED
#define MONOLITHPYEMBED

#if !defined(__ASSEMBLER__) && !defined(BYPASS_MP_EMBED)

#ifdef __linux
# define _GNU_SOURCE
#endif  /* __linux */

#ifdef FOPEN_MAX
// This means that we were loaded too late so we can't intercept the necessary calls.
// Don't even try in that case.
#define MP_STDIO_ALREADY_LOADED
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#ifdef _WIN32
#define NOMINMAX
#if defined(_M_IX86)
#define _X86_
#elif defined(_M_X64)
#define _AMD64_
#elif defined(_M_ARM)
#define _ARM_
#elif defined(_M_ARM64)
#define _ARM64_
#endif  /* _M_IX86 || _M_X64 || _M_ARM || _M_ARM64 */

#include <wchar.h>
#include <basetsd.h>
#include <fileapi.h>
#include <handleapi.h>
#include <windef.h>
#include <WinBase.h>
typedef int BOOL;
typedef const char *LPCSTR;
typedef const wchar_t *LPCWSTR;
typedef void *LPVOID;
typedef SSIZE_T ssize_t;
#else  /* _WIN32 */
    #include <dirent.h>
    #include <limits.h>
    #include <unistd.h>
    #include <libgen.h>
    #include <sys/statvfs.h>
#endif  /* _WIN32 */
#else  /* FOPEN_MAX */

#ifdef _WIN32
#define _CRTIMP
#endif  /* _WIN32 */
#ifndef MONOLITHPY_EMBED_BUILD
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
#endif  /* __linux */
#endif  /* MONOLITHPY_EMBED_BUILD */
#ifdef __linux
#include <features.h>
#undef __USE_EXTERN_INLINES
#define _BITS_STDIO_H
#endif  /* __linux */
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#define NOMINMAX
#include <wchar.h>
#include <basetsd.h>
#include <stdint.h>
#if defined(_M_IX86)
#define _X86_
#elif defined(_M_X64)
#define _AMD64_
#elif defined(_M_ARM)
#define _ARM_
    #elif defined(_M_ARM64)
    #define _ARM64_
#endif  /* _M_IX86 || _M_X64 || _M_ARM || _M_ARM64 */

#include <fileapi.h>
#include <handleapi.h>
#include <windef.h>
#include <WinBase.h>
typedef int BOOL;
typedef const char *LPCSTR;
typedef const wchar_t *LPCWSTR;
typedef void *LPVOID;
typedef SSIZE_T ssize_t;
#else  /* _WIN32 */
#include <limits.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/uio.h>
#include <sys/statvfs.h>
#endif  /* _WIN32 */
#ifndef MONOLITHPY_EMBED_BUILD
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
#endif  /* __linux */
#endif  /* MONOLITHPY_EMBED_BUILD */
#endif  /* FOPEN_MAX */

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#ifdef __cplusplus
#define MP_BOOL bool
#else
#define MP_BOOL _Bool
#endif
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
#define MP_DECL(x) _ACRTIMP x __cdecl
#define MP_STD(x) _ACRTIMP x __stdcall
#else  /* _WIN32 */
#define MP_DECL(x) x
#endif  /* _WIN32 */

#if defined(_MSC_VER)
#define ALWAYS_INLINE static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#ifdef __cplusplus
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#else  /* __cplusplus */
#define ALWAYS_INLINE __attribute__((always_inline)) static inline
#endif  /* __cplusplus */
#else  /* _MSC_VER || __GNUC__ || __clang__ */
#ifdef __cplusplus
#define ALWAYS_INLINE inline
#else  /* __cplusplus */
#define ALWAYS_INLINE static inline
#endif  /* __cplusplus */
#endif  /* _MSC_VER || __GNUC__ || __clang__ */

#ifdef _WIN32
#define MP_FOREIGN_PTR e == NULL || *(void**)e == NULL || (((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE)
#else  /* _WIN32 */
#define MP_FOREIGN_PTR ((EFILE*)e)->handle_type != EHANDLE_VIRTUAL && ((EFILE*)e)->handle_type != EHANDLE_NATIVE
#endif  /* _WIN32 */

#if !defined(MONOLITHPY_EMBED_BUILD) && !defined(MP_STDIO_ALREADY_LOADED)
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
#else  /* _USE_32BIT_TIME_T */
    #define _stat       _stat64i32
    #define _stati64    _stat64
    #define _wstat      _wstat64i32
    #define _wstati64   _wstat64
#endif  /* _USE_32BIT_TIME_T */
#endif  /* _WIN32 */
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
#else  /* !_POSIX_C_SOURCE || _DARWIN_C_SOURCE */
    time_t          st_atime;       /* [XSI] Time of last access */
	long            st_atimensec;   /* nsec of last access */
	time_t          st_mtime;       /* [XSI] Last data modification time */
	long            st_mtimensec;   /* last data modification nsec */
	time_t          st_ctime;       /* [XSI] Time of last status change */
	long            st_ctimensec;   /* nsec of last status change */
	time_t		    st_birthtime;       /*  File creation time(birth)  */
	long		    st_birthtimensec;   /* nsec of File creation time */
#endif  /* !_POSIX_C_SOURCE || _DARWIN_C_SOURCE */
    off_t st_size;
    blkcnt_t st_blocks;
    blksize_t st_blksize;
    __uint32_t st_flags;
    __uint32_t st_gen;
    __int32_t st_lspare;
    __int64_t st_qspare[2];
};

#else  /* !__DARWIN_64_BIT_INO_T */

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
#else  /* !_POSIX_C_SOURCE || _DARWIN_C_SOURCE */
	time_t          st_atime;       /* [XSI] Time of last access */
	long            st_atimensec;   /* nsec of last access */
	time_t          st_mtime;       /* [XSI] Last data modification time */
	long            st_mtimensec;   /* last data modification nsec */
	time_t          st_ctime;       /* [XSI] Time of last status change */
	long            st_ctimensec;   /* nsec of last status change */
#endif  /* !_POSIX_C_SOURCE || _DARWIN_C_SOURCE */
	off_t           st_size;        /* [XSI] file size, in bytes */
	blkcnt_t        st_blocks;      /* [XSI] blocks allocated for file */
	blksize_t       st_blksize;     /* [XSI] optimal blocksize for I/O */
	__uint32_t      st_flags;       /* user defined flags for file */
	__uint32_t      st_gen;         /* file generation number */
	__int32_t       st_lspare;      /* RESERVED: DO NOT USE! */
	__int64_t       st_qspare[2];   /* RESERVED: DO NOT USE! */
};

#endif  /* __DARWIN_64_BIT_INO_T */

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
#endif  /* __APPLE__ */
#ifdef __linux
struct stat
{
  __dev_t st_dev;		/* Device.  */
#ifndef __x86_64__
  unsigned short int __pad1;
#endif  /* __x86_64__ */
#if defined __x86_64__ || !defined __USE_FILE_OFFSET64
  __ino_t st_ino;		/* File serial number.	*/
#else  /* __x86_64__ || !__USE_FILE_OFFSET64 */
  __ino_t __st_ino;			/* 32bit file serial number.	*/
#endif  /* __x86_64__ || !__USE_FILE_OFFSET64 */
#ifndef __x86_64__
  __mode_t st_mode;			/* File mode.  */
    __nlink_t st_nlink;			/* Link count.  */
#else  /* __x86_64__ */
  __nlink_t st_nlink;		/* Link count.  */
  __mode_t st_mode;		/* File mode.  */
#endif  /* __x86_64__ */
  __uid_t st_uid;		/* User ID of the file's owner.	*/
  __gid_t st_gid;		/* Group ID of the file's group.*/
#ifdef __x86_64__
  int __pad0;
#endif  /* __x86_64__ */
  __dev_t st_rdev;		/* Device number, if device.  */
#ifndef __x86_64__
  unsigned short int __pad2;
#endif  /* __x86_64__ */
#if defined __x86_64__ || !defined __USE_FILE_OFFSET64
  __off_t st_size;			/* Size of file, in bytes.  */
#else  /* __x86_64__ || !__USE_FILE_OFFSET64 */
  __off64_t st_size;			/* Size of file, in bytes.  */
#endif  /* __x86_64__ || !__USE_FILE_OFFSET64 */
  __blksize_t st_blksize;	/* Optimal block size for I/O.  */
#if defined __x86_64__  || !defined __USE_FILE_OFFSET64
  __blkcnt_t st_blocks;		/* Number 512-byte blocks allocated. */
#else  /* __x86_64__ || !__USE_FILE_OFFSET64 */
  __blkcnt64_t st_blocks;		/* Number 512-byte blocks allocated. */
#endif  /* __x86_64__ || !__USE_FILE_OFFSET64 */
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
#else  /* __USE_XOPEN2K8 */
  __time_t st_atime;			/* Time of last access.  */
  __syscall_ulong_t st_atimensec;	/* Nscecs of last access.  */
  __time_t st_mtime;			/* Time of last modification.  */
  __syscall_ulong_t st_mtimensec;	/* Nsecs of last modification.  */
  __time_t st_ctime;			/* Time of last status change.  */
  __syscall_ulong_t st_ctimensec;	/* Nsecs of last status change.  */
#endif  /* __USE_XOPEN2K8 */
#ifdef __x86_64__
  __syscall_slong_t __glibc_reserved[3];
#else  /* __x86_64__ */
# ifndef __USE_FILE_OFFSET64
  unsigned long int __glibc_reserved4;
  unsigned long int __glibc_reserved5;
# else  /* __USE_FILE_OFFSET64 */
  __ino64_t st_ino;			/* File serial number.	*/
# endif  /* __USE_FILE_OFFSET64 */
#endif  /* __x86_64__ */
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
#else  /* __USE_FILE_OFFSET64 */
  __fsblkcnt64_t f_blocks;
  __fsblkcnt64_t f_bfree;
  __fsblkcnt64_t f_bavail;
  __fsfilcnt64_t f_files;
  __fsfilcnt64_t f_ffree;
  __fsfilcnt64_t f_favail;
#endif  /* __USE_FILE_OFFSET64 */
  unsigned long int f_fsid;
#ifdef _STATVFSBUF_F_UNUSED
  int __f_unused;
#endif  /* _STATVFSBUF_F_UNUSED */
  unsigned long int f_flag;
  unsigned long int f_namemax;
  unsigned int f_type;
  int __f_spare[5];
};
#endif  /* __linux */
#endif  /* !MONOLITHPY_EMBED_BUILD && !MP_STDIO_ALREADY_LOADED */

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

MP_DECL(EFILE*) mp_fopen(const char* file, const char* mode);
#ifdef _WIN32
MP_DECL(int) mp_open(const char *pathname, int flags, int mode);
MP_DECL(EFILE*) mp_wfopen(const wchar_t *wfile, const wchar_t *mode);
MP_DECL(int) mp_wopen(const wchar_t *pathname, int flags, int mode);
#else  /* _WIN32 */
MP_DECL(int) mp_open(const char *pathname, int flags, mode_t mode);
MP_DECL(int) mp_openat(int dirfd, const char* pathname, int flags, mode_t mode);
#endif  /* _WIN32 */
MP_DECL(int) mp_fclose(void* e);
MP_DECL(int) mp_close(int fd);
MP_DECL(EFILE*) mp_tmpfile(void);
MP_DECL(MP_BOOL) mp_feof(void* e);
MP_DECL(size_t) mp_fread(void* ptr, size_t size, size_t count, void* stream);
#ifdef _WIN32
MP_DECL(int) mp_read(int fd, void *buf, unsigned int count);
#else  /* _WIN32 */
MP_DECL(ssize_t) mp_read(int fd, void *buf, size_t count);
#endif  /* _WIN32 */
MP_DECL(ssize_t) mp_pread(int fd, void *buf, size_t count, off_t offset);
MP_DECL(int) mp_fgetpos(void* e, fpos_t* pos);
/* File Opening and Closing */
MP_DECL(EFILE*) mp_freopen(const char *filename, const char *mode, void *stream);
MP_DECL(EFILE*) mp_fdopen(int fd, const char *mode);

/* File Input Functions */
MP_DECL(int) mp_fgetc(void *stream);
MP_DECL(char*) mp_fgets(char *str, int n, void *stream);
#ifndef _WIN32
MP_DECL(ssize_t) mp_getline(char **lineptr, size_t *n, void *stream);
#endif  /* _WIN32 */
MP_DECL(int) mp_fscanf(void *stream, const char *format, ...);
MP_DECL(int) mp_getc_unlocked(void *stream);  /* Unlocked version of egetc */

/* File Output Functions */
MP_DECL(int) mp_fputc(int character, void *stream);
MP_DECL(int) mp_fputs(const char *str, void *stream);
MP_DECL(int) mp_fprintf(void *stream, const char *format, ... );
MP_DECL(int) mp_vfprintf(void *e, const char *format, va_list args);
MP_DECL(size_t) mp_fwrite(const void *ptr, size_t size, size_t count, void *stream);

/* File Buffering */
MP_DECL(void) mp_setbuf(void *stream, char *buffer);
MP_DECL(int) mp_setvbuf(void *stream, char *buffer, int mode, size_t size);

/* File Positioning */
MP_DECL(int) mp_fseek(void *stream, long int offset, int origin);
MP_DECL(long int) mp_ftell(void *stream);
MP_DECL(int) mp_fseeko64(void *stream, int64_t offset, int origin);
MP_DECL(int64_t) mp_ftello64(void *stream);
MP_DECL(void) mp_rewind(void *stream);
MP_DECL(int) mp_fsetpos(void *stream, const fpos_t *pos);
MP_DECL(int) mp_ungetc(int character, void *stream);
#ifdef _WIN32
MP_DECL(__int64) mp_lseeki64(int fd, __int64 offset, int whence);
#else  /* _WIN32 */
MP_DECL(ssize_t) mp_lseek(int fd, off_t offset, int whence);
#endif  /* _WIN32 */

/* Error Handling & Other Utilities */
MP_DECL(void) mp_clearerr(void *stream);
MP_DECL(int) mp_ferror(void *stream);
MP_DECL(int) mp_fileno(void *stream);
MP_DECL(int) mp_fflush(void *stream);

/* Locking Functions */
MP_DECL(void) mp_flockfile(void *e);
MP_DECL(void) mp_funlockfile(void *e);
MP_DECL(int) mp_ftrylockfile(void *e);

#ifdef _WIN32
MP_DECL(int) mp__stat32(const char *path, struct __stat32 *buffer);
MP_DECL(int) mp__stat64(const char *path, struct __stat64 *buffer);
MP_DECL(int) mp__stat32i64(const char *path, struct _stat32i64 *buffer);
MP_DECL(int) mp__stat64i32(const char *path, struct _stat64i32 *buffer);
MP_DECL(int) mp__wstat32(const wchar_t *path, struct __stat32 *buffer);
MP_DECL(int) mp__wstat64(const wchar_t *path, struct __stat64 *buffer);
MP_DECL(int) mp__wstat32i64(const wchar_t *path, struct _stat32i64 *buffer);
MP_DECL(int) mp__wstat64i32(const wchar_t *path, struct _stat64i32 *buffer);
MP_DECL(DWORD) mp_GetFileAttributesA(LPCSTR lpFileName);
MP_DECL(BOOL) mp_GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
MP_DECL(BOOL) mp_GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
MP_DECL(DWORD) mp_GetFileAttributesW(LPCWSTR lpFileName);
MP_STD(BOOL) mp_CloseHandle(HANDLE hObject);
MP_STD(HANDLE) mp_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
MP_STD(HANDLE) mp_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
MP_STD(BOOL) mp_DeleteFileA(LPCSTR lpFileName);
MP_STD(BOOL) mp_DeleteFileW(LPCWSTR lpFileName);
MP_STD(BOOL) mp_GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
MP_STD(BOOL) mp_GetFileInformationByHandleEx(HANDLE hFile, FILE_INFO_BY_HANDLE_CLASS FileInformationClass, LPVOID lpFileInformation, DWORD dwBufferSize);
MP_STD(DWORD) mp_GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags);
MP_STD(DWORD) mp_GetFileType(HANDLE hFile);
MP_STD(DWORD) mp_GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, LPWSTR *lpFilePart);
MP_STD(BOOL) mp_GetVolumePathNameW(LPCWSTR lpszFileName, LPWSTR lpszVolumePathName, DWORD cchBufferLength);
MP_STD(BOOL) mp_GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller, PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes);
MP_STD(HANDLE) mp_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
MP_STD(HANDLE) mp_FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
MP_STD(BOOL) mp_FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
MP_STD(BOOL) mp_FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
MP_STD(BOOL) mp_FindClose(HANDLE hFindFile);
MP_STD(BOOL) mp_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);
MP_STD(DWORD) mp_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
MP_STD(HANDLE) mp_CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName);
MP_STD(HANDLE) mp_CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect, DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName);
MP_STD(LPVOID) mp_MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh, DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);
MP_STD(BOOL) mp_UnmapViewOfFile(LPCVOID lpBaseAddress);
#else  /* _WIN32 */
MP_DECL(int) mp_stat(const char *path, struct stat *buf);
MP_DECL(int) mp_fstat(int fd, struct stat *buf);
MP_DECL(int) mp_fstatat(int dirfd, const char *pathname, struct stat *buf, int flags);
MP_DECL(int) mp_lstat(const char *path, struct stat *buf);
MP_DECL(int) mp_access(const char *pathname, int mode);
MP_DECL(int) mp_faccessat(int dirfd, const char *pathname, int mode, int flags);
MP_DECL(int) mp_statvfs(const char *path, struct statvfs *buf);
MP_DECL(int) mp_fstatvfs(int fd, struct statvfs *buf);
MP_DECL(ssize_t) mp_readv(int fd, const struct iovec *iov, int iovcnt);
MP_DECL(ssize_t) mp_preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset);
MP_DECL(ssize_t) mp_preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
MP_DECL(ssize_t) mp_writev(int fd, const struct iovec *iov, int iovcnt);
MP_DECL(ssize_t) mp_pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset);
MP_DECL(ssize_t) mp_pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags);
MP_DECL(ssize_t) mp_pwrite(int fd, const void *buf, size_t count, off_t offset);
MP_DECL(DIR*) mp_opendir(const char* name);
MP_DECL(DIR*) mp_fdopendir(int fd);
MP_DECL(struct dirent*) mp_readdir(DIR *dirp);
MP_DECL(int) mp_closedir(DIR *dirp);
MP_DECL(void) mp_rewinddir(DIR *dirp);
#endif  /* _WIN32 */

#if !defined(MONOLITHPY_EMBED_BUILD) && !defined(MP_STDIO_ALREADY_LOADED)

#if (defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)) || defined(__APPLE__)
#define MP_EMBED_CAT(a, ...) MP_EMBED_PRIMITIVE_CAT(a, __VA_ARGS__)
#define MP_EMBED_PRIMITIVE_CAT(a, ...) a ## __VA_ARGS__

#define MP_EMBED_IIF(c) MP_EMBED_PRIMITIVE_CAT(MP_EMBED_IIF_, c)
#define MP_EMBED_IIF_0(t, ...) __VA_ARGS__
#define MP_EMBED_IIF_1(t, ...) t

#define MP_EMBED_COMPL(b) MP_EMBED_PRIMITIVE_CAT(MP_EMBED_COMPL_, b)
#define MP_EMBED_COMPL_0 1
#define MP_EMBED_COMPL_1 0

#define MP_EMBED_INC(x) MP_EMBED_PRIMITIVE_CAT(MP_EMBED_INC_, x)
#define MP_EMBED_INC_0 1
#define MP_EMBED_INC_1 2
#define MP_EMBED_INC_2 3
#define MP_EMBED_INC_3 4
#define MP_EMBED_INC_4 5
#define MP_EMBED_INC_5 6
#define MP_EMBED_INC_6 7
#define MP_EMBED_INC_7 8
#define MP_EMBED_INC_8 9
#define MP_EMBED_INC_9 10
#define MP_EMBED_INC_10 11
#define MP_EMBED_INC_11 12
#define MP_EMBED_INC_12 13
#define MP_EMBED_INC_13 14
#define MP_EMBED_INC_14 15
#define MP_EMBED_INC_15 16
#define MP_EMBED_INC_16 17
#define MP_EMBED_INC_17 18
#define MP_EMBED_INC_18 19
#define MP_EMBED_INC_19 20

#define MP_EMBED_DEC(x) MP_EMBED_PRIMITIVE_CAT(MP_EMBED_DEC_, x)
#define MP_EMBED_DEC_0 0
#define MP_EMBED_DEC_1 0
#define MP_EMBED_DEC_2 1
#define MP_EMBED_DEC_3 2
#define MP_EMBED_DEC_4 3
#define MP_EMBED_DEC_5 4
#define MP_EMBED_DEC_6 5
#define MP_EMBED_DEC_7 6
#define MP_EMBED_DEC_8 7
#define MP_EMBED_DEC_9 8
#define MP_EMBED_DEC_10 9
#define MP_EMBED_DEC_11 10
#define MP_EMBED_DEC_12 11
#define MP_EMBED_DEC_13 12
#define MP_EMBED_DEC_14 13
#define MP_EMBED_DEC_15 14
#define MP_EMBED_DEC_16 15
#define MP_EMBED_DEC_17 16
#define MP_EMBED_DEC_18 17
#define MP_EMBED_DEC_19 18
#define MP_EMBED_DEC_20 19

#define MP_EMBED_CHECK_N(x, n, ...) n
#define MP_EMBED_CHECK(...) MP_EMBED_CHECK_N(__VA_ARGS__, 0,)
#define MP_EMBED_PROBE(x) x, 1,

#define MP_EMBED_IS_PAREN(x) MP_EMBED_CHECK(MP_EMBED_IS_PAREN_PROBE x)
#define MP_EMBED_IS_PAREN_PROBE(...) MP_EMBED_PROBE(~)

#define MP_EMBED_NOT(x) MP_EMBED_CHECK(MP_EMBED_PRIMITIVE_CAT(MP_EMBED_NOT_, x))
#define MP_EMBED_NOT_0 MP_EMBED_PROBE(~)

#define MP_EMBED_BOOL(x) MP_EMBED_COMPL(MP_EMBED_NOT(x))
#define MP_EMBED_IF(c) MP_EMBED_IIF(MP_EMBED_BOOL(c))

#define MP_EMBED_EAT(...)
#define MP_EMBED_EXPAND(...) __VA_ARGS__
#define MP_EMBED_WHEN(c) MP_EMBED_IF(c)(MP_EMBED_EXPAND, MP_EMBED_EAT)

#define MP_EMBED_EMPTY()
#define MP_EMBED_DEFER(id) id MP_EMBED_EMPTY()
#define MP_EMBED_OBSTRUCT(...) __VA_ARGS__ MP_EMBED_DEFER(MP_EMBED_EMPTY)()

#define MP_EMBED_NUM_ARGS1(_20,_19,_18,_17,_16,_15,_14,_13,_12,_11,_10,_9,_8,_7,_6,_5,_4,_3,_2,_1, n, ...) n
#define MP_EMBED_NUM_ARGS0(...) MP_EMBED_NUM_ARGS1(__VA_ARGS__,20,19,18,17,16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0)
#define MP_EMBED_NUM_ARGS(...) MP_EMBED_IF(MP_EMBED_DEC(MP_EMBED_NUM_ARGS0(__VA_ARGS__)))(MP_EMBED_NUM_ARGS0(__VA_ARGS__),MP_EMBED_IF(MP_EMBED_IS_PAREN(__VA_ARGS__ ()))(0,1))
#endif  /* __GNUC__ && !__llvm__ && !__INTEL_COMPILER || __APPLE__ */

// Preprocessor Translation
#define FILE EFILE

#ifdef __linux
/* Standard streams.  */
extern EFILE *stdin;		/* Standard input stream.  */
extern EFILE *stdout;		/* Standard output stream.  */
extern EFILE *stderr;		/* Standard error output stream.  */
#endif  /* __linux */

/* File Opening and Closing */
ALWAYS_INLINE MP_DECL(EFILE*) fopen(const char* file, const char* mode) {
  return mp_fopen(file, mode);
}
ALWAYS_INLINE MP_DECL(EFILE*) _fopen(const char* file, const char* mode) {
  return mp_fopen(file, mode);
}

#if (defined(__GNUC__) && !defined(__llvm__) && !defined(__INTEL_COMPILER)) || defined(__APPLE__)
#ifdef __cplusplus
ALWAYS_INLINE MP_DECL(int) open(const char *pathname, int flags, mode_t mode = 0) {
#else  /* __cplusplus */
ALWAYS_INLINE MP_DECL(int) open(const char *pathname, int flags, mode_t mode) {
#endif  /* __cplusplus */
  return mp_open(pathname, flags, mode);
}
#ifdef __cplusplus
ALWAYS_INLINE MP_DECL(int) _open(const char *pathname, int flags, mode_t mode = 0) {
#else  /* __cplusplus */
ALWAYS_INLINE MP_DECL(int) _open(const char *pathname, int flags, mode_t mode) {
#endif  /* __cplusplus */
  return mp_open(pathname, flags, mode);
}
#ifdef __cplusplus
ALWAYS_INLINE MP_DECL(int) openat(int dirfd, const char *pathname, int flags, mode_t mode = 0) {
#else  /* __cplusplus */
ALWAYS_INLINE MP_DECL(int) openat(int dirfd, const char *pathname, int flags, mode_t mode) {
#endif  /* __cplusplus */
  return mp_openat(dirfd, pathname, flags, mode);
}
#ifndef __cplusplus
// C does not support optional parameters so we are forced to rely on the mess below
// because GCC does not support inlining vararg functions. :(
#define MP_EMBED_open0() open()
#define MP_EMBED_open1(a) open(a)
#define MP_EMBED_open2(a, b) open(a, b, 0)
#define MP_EMBED_open3(a, b, c) open(a, b, c)
#define MP_EMBED_open4(a, b, c, d) open(a, b, c, d)
#define MP_EMBED_open5(a, b, c, d, e) open(a, b, c, d, e)
#define MP_EMBED_open6(a, b, c, d, e, f) open(a, b, c, d, e, f)
#define MP_EMBED_open7(a, b, c, d, e, f, g) open(a, b, c, d, e, f, g)
#define open(...) MP_EMBED_CAT( MP_EMBED_open, MP_EMBED_NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )
#define _open(...) MP_EMBED_CAT( MP_EMBED_open, MP_EMBED_NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )

#define MP_EMBED_openat0() openat()
#define MP_EMBED_openat1(a) openat(a)
#define MP_EMBED_openat2(a, b) openat(a, b, 0)
#define MP_EMBED_openat3(a, b, c) openat(a, b, c)
#define MP_EMBED_openat4(a, b, c, d) openat(a, b, c, d)
#define MP_EMBED_openat5(a, b, c, d, e) openat(a, b, c, d, e)
#define MP_EMBED_openat6(a, b, c, d, e, f) openat(a, b, c, d, e, f)
#define MP_EMBED_openat7(a, b, c, d, e, f, g) openat(a, b, c, d, e, f, g)
#define openat(...) MP_EMBED_CAT( MP_EMBED_openat, MP_EMBED_NUM_ARGS( __VA_ARGS__ ) )( __VA_ARGS__ )
#endif  /* !__cplusplus */
#else  /* __GNUC__ && !__llvm__ && !__INTEL_COMPILER || __APPLE__ */
ALWAYS_INLINE MP_DECL(int) open(const char *pathname, int flags, ... /* mode_t mode */ ) {
    va_list args;
    va_start(args, flags);
#ifdef _WIN32
    int mode = 0;
#else  /* _WIN32 */
    mode_t mode = 0;
#endif  /* _WIN32 */
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return mp_open(pathname, flags, mode);
}
ALWAYS_INLINE MP_DECL(int) _open(const char *pathname, int flags, ... /* mode_t mode */ ) {
    va_list args;
    va_start(args, flags);
#ifdef _WIN32
    int mode = 0;
#else  /* _WIN32 */
    mode_t mode = 0;
#endif  /* _WIN32 */
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return mp_open(pathname, flags, mode);
}
#ifndef _WIN32
ALWAYS_INLINE MP_DECL(int) openat(int dirfd, const char *pathname, int flags, ... /* mode_t mode */ ) {
    va_list args;
    va_start(args, flags);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(args, int); // mode_t is promoted to int in varargs
    }
    va_end(args);
    return mp_openat(dirfd, pathname, flags, mode);
}
#endif  /* _WIN32 */
#endif  /* __GNUC__ && !__llvm__ && !__INTEL_COMPILER || __APPLE__ */

ALWAYS_INLINE MP_DECL(EFILE*) fdopen(int fd, const char *mode) {
  return mp_fdopen(fd, mode);
}
ALWAYS_INLINE MP_DECL(EFILE*) _fdopen(int fd, const char *mode) {
  return mp_fdopen(fd, mode);
}

ALWAYS_INLINE MP_DECL(EFILE*) freopen(const char *filename, const char *mode, void *stream) {
  return mp_freopen(filename, mode, stream);
}
ALWAYS_INLINE MP_DECL(EFILE*) _freopen(const char *filename, const char *mode, void *stream) {
  return mp_freopen(filename, mode, stream);
}

ALWAYS_INLINE MP_DECL(int) fclose(FILE* e) {
  return mp_fclose(e);
}
ALWAYS_INLINE MP_DECL(int) _fclose(FILE* e) {
  return mp_fclose(e);
}

ALWAYS_INLINE MP_DECL(int) close(int fd) {
  return mp_close(fd);
}
ALWAYS_INLINE MP_DECL(int) _close(int fd) {
  return mp_close(fd);
}

#ifdef _WIN32
ALWAYS_INLINE MP_DECL(int) wopen(const wchar_t *pathname, int flags, ... /* int mode */ ) {
    va_list args;
    va_start(args, flags);
    int mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return mp_wopen(pathname, flags, mode);
}
ALWAYS_INLINE MP_DECL(int) _wopen(const wchar_t *pathname, int flags, ... /* int mode */ ) {
    va_list args;
    va_start(args, flags);
    int mode = 0;
    if (flags & O_CREAT) {
        mode = va_arg(args, int);
    }
    va_end(args);
    return mp_wopen(pathname, flags, mode);
}

ALWAYS_INLINE MP_DECL(EFILE*) wfopen(const wchar_t* file, const wchar_t* mode) {
    return mp_wfopen(file, mode);
}
ALWAYS_INLINE MP_DECL(EFILE*) _wfopen(const wchar_t* file, const wchar_t* mode) {
    return mp_wfopen(file, mode);
}
#endif  /* _WIN32 */

ALWAYS_INLINE MP_DECL(EFILE*) tmpfile(void) {
  return mp_tmpfile();
}

/* File Input Functions */
ALWAYS_INLINE MP_DECL(char*) fgets(char *str, int n, void *stream) {
    return mp_fgets(str, n, stream);
}
ALWAYS_INLINE MP_DECL(char*) _fgets(char *str, int n, void *stream) {
    return mp_fgets(str, n, stream);
}

#ifndef _WIN32
ALWAYS_INLINE MP_DECL(ssize_t) getline(char **lineptr, size_t *n, void *stream) {
    return mp_getline(lineptr, n, stream);
}
#endif  /* _WIN32 */

ALWAYS_INLINE MP_DECL(int) getc(void *stream) {
    return mp_fgetc(stream);
}
ALWAYS_INLINE MP_DECL(int) fgetc(void *stream) {
    return mp_fgetc(stream);
}
ALWAYS_INLINE MP_DECL(int) _fgetc(void *stream) {
    return mp_fgetc(stream);
}

// Need to use a macro for this one due to varargs.
#define fscanf mp_fscanf
#define _fscanf mp_fscanf

ALWAYS_INLINE MP_DECL(size_t) fread(void* ptr, size_t size, size_t count, FILE* stream) {
    return mp_fread(ptr, size, count, stream);
}
ALWAYS_INLINE MP_DECL(size_t) _fread(void* ptr, size_t size, size_t count, FILE* stream) {
    return mp_fread(ptr, size, count, stream);
}

#ifdef _WIN32
ALWAYS_INLINE MP_DECL(int) read(int fd, void *buf, unsigned int count) {
#else  /* _WIN32 */
ALWAYS_INLINE MP_DECL(ssize_t) read(int fd, void *buf, size_t count) {
#endif  /* _WIN32 */
    return mp_read(fd, buf, count);
}
#ifdef _WIN32
ALWAYS_INLINE MP_DECL(int) _read(int fd, void *buf, unsigned int count) {
#else  /* _WIN32 */
ALWAYS_INLINE MP_DECL(ssize_t) _read(int fd, void *buf, size_t count) {
#endif  /* _WIN32 */
    return mp_read(fd, buf, count);
}

#ifndef _WIN32
ALWAYS_INLINE MP_DECL(ssize_t) pread(int fd, void *buf, size_t count, off_t offset) {
    return mp_pread(fd, buf, count, offset);
}
ALWAYS_INLINE MP_DECL(int) getc_unlocked(void *stream) {
    return mp_getc_unlocked(stream);
}
#endif  /* _WIN32 */

ALWAYS_INLINE MP_DECL(int) ungetc(int character, void *stream) {
    return mp_ungetc(character, stream);
}
ALWAYS_INLINE MP_DECL(int) _ungetc(int character, void *stream) {
    return mp_ungetc(character, stream);
}

/* File Output Functions */
ALWAYS_INLINE MP_DECL(int) putc(int character, void *stream) {
    return mp_fputc(character, stream);
}
ALWAYS_INLINE MP_DECL(int) _putc(int character, void *stream) {
    return mp_fputc(character, stream);
}
ALWAYS_INLINE MP_DECL(int) fputc(int character, void *stream) {
    return mp_fputc(character, stream);
}
ALWAYS_INLINE MP_DECL(int) _fputc(int character, void *stream) {
    return mp_fputc(character, stream);
}

ALWAYS_INLINE MP_DECL(int) fputs(const char *str, void *stream) {
    return mp_fputs(str, stream);
}
ALWAYS_INLINE MP_DECL(int) _fputs(const char *str, void *stream) {
    return mp_fputs(str, stream);
}

// Need to use a macro for this one due to varargs.
#define fprintf mp_fprintf
#define _fprintf mp_fprintf
#define vfprintf mp_vfprintf
#define _vfprintf mp_vfprintf


ALWAYS_INLINE MP_DECL(size_t) fwrite(const void *ptr, size_t size, size_t count, void *stream) {
    return mp_fwrite(ptr, size, count, stream);
}
ALWAYS_INLINE MP_DECL(size_t) _fwrite(const void *ptr, size_t size, size_t count, void *stream) {
    return mp_fwrite(ptr, size, count, stream);
}

/* File Buffering */
ALWAYS_INLINE MP_DECL(void) setbuf(void *stream, char *buffer) {
    mp_setbuf(stream, buffer);
}
ALWAYS_INLINE MP_DECL(void) _setbuf(void *stream, char *buffer) {
    mp_setbuf(stream, buffer);
}
ALWAYS_INLINE MP_DECL(int) setvbuf(void *stream, char *buffer, int mode, size_t size) {
    return mp_setvbuf(stream, buffer, mode, size);
}
ALWAYS_INLINE MP_DECL(int) _setvbuf(void *stream, char *buffer, int mode, size_t size) {
    return mp_setvbuf(stream, buffer, mode, size);
}

/* File Positioning */
ALWAYS_INLINE MP_DECL(int) fseek(void *stream, long int offset, int origin) {
    return mp_fseek(stream, offset, origin);
}
ALWAYS_INLINE MP_DECL(int) _fseek(void *stream, long int offset, int origin) {
    return mp_fseek(stream, offset, origin);
}

ALWAYS_INLINE MP_DECL(long) ftell(void *stream) {
    return mp_ftell(stream);
}
ALWAYS_INLINE MP_DECL(long) _ftell(void *stream) {
    return mp_ftell(stream);
}
#ifdef _WIN32
ALWAYS_INLINE MP_DECL(int) _fseeki64(void *stream, int64_t offset, int origin) {
    return mp_fseeko64(stream, offset, origin);
}
ALWAYS_INLINE MP_DECL(int64_t) _ftelli64(void *stream) {
    return mp_ftello64(stream);
}
ALWAYS_INLINE MP_DECL(long) lseek(int fd, long offset, int whence) {
    return (long)mp_lseeki64(fd, offset, whence);
}
ALWAYS_INLINE MP_DECL(long) _lseek(int fd, long offset, int whence) {
    return (long)mp_lseeki64(fd, offset, whence);
}
ALWAYS_INLINE MP_DECL(__int64) _lseeki64(int fd, __int64 offset, int whence) {
    return mp_lseeki64(fd, offset, whence);
}
#else  /* _WIN32 */
#if defined _FILE_OFFSET_BITS && _FILE_OFFSET_BITS == 64
ALWAYS_INLINE MP_DECL(int) fseeko(void *stream, int64_t offset, int origin) {
    return mp_fseeko64(stream, offset, origin);
}
ALWAYS_INLINE MP_DECL(int64_t) ftello(void *stream) {
    return mp_ftello64(stream);
}
#else  /* _FILE_OFFSET_BITS == 64 */
ALWAYS_INLINE MP_DECL(int) fseeko(void *stream, long int offset, int origin) {
    return mp_fseek(stream, offset, origin);
}
ALWAYS_INLINE MP_DECL(long int) ftello(void *stream) {
    return mp_ftell(stream);
}
#endif  /* _FILE_OFFSET_BITS == 64 */
ALWAYS_INLINE MP_DECL(off_t) lseek(int fd, off_t offset, int whence) {
    return mp_lseek(fd, offset, whence);
}
#endif  /* _WIN32 */

ALWAYS_INLINE MP_DECL(void) rewind(void *stream) {
    mp_rewind(stream);
}
ALWAYS_INLINE MP_DECL(void) _rewind(void *stream) {
    mp_rewind(stream);
}

ALWAYS_INLINE MP_DECL(int) fgetpos(void *e, fpos_t* pos) {
    return mp_fgetpos(e, pos);
}
ALWAYS_INLINE MP_DECL(int) _fgetpos(void *e, fpos_t* pos) {
    return mp_fgetpos(e, pos);
}

ALWAYS_INLINE MP_DECL(int) fsetpos(void *e, fpos_t* pos) {
    return mp_fsetpos(e, pos);
}
ALWAYS_INLINE MP_DECL(int) _fsetpos(void *e, fpos_t* pos) {
    return mp_fsetpos(e, pos);
}

/* Error Handling & Other Utilities */
ALWAYS_INLINE MP_DECL(void) clearerr(void *stream) {
    mp_clearerr(stream);
}
ALWAYS_INLINE MP_DECL(void) _clearerr(void *stream) {
    mp_clearerr(stream);
}

ALWAYS_INLINE MP_DECL(MP_BOOL) feof(void *e) {
    return mp_feof(e);
}
ALWAYS_INLINE MP_DECL(MP_BOOL) _feof(void *e) {
    return mp_feof(e);
}

ALWAYS_INLINE MP_DECL(int) ferror(void *stream) {
    return mp_ferror(stream);
}
ALWAYS_INLINE MP_DECL(int) _ferror(void *stream) {
    return mp_ferror(stream);
}

ALWAYS_INLINE MP_DECL(int) fileno(void *stream) {
    return mp_fileno(stream);
}
ALWAYS_INLINE MP_DECL(int) _fileno(void *stream) {
    return mp_fileno(stream);
}

ALWAYS_INLINE MP_DECL(int) fflush(void *stream) {
    return mp_fflush(stream);
}
ALWAYS_INLINE MP_DECL(int) _fflush(void *stream) {
    return mp_fflush(stream);
}

#ifdef _WIN32
ALWAYS_INLINE MP_DECL(int) _stat32(const char *path, struct __stat32 *buffer) {
    return mp__stat32(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _stat64(const char *path, struct __stat64 *buffer) {
    return mp__stat64(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _stat32i64(const char *path, struct _stat32i64 *buffer) {
    return mp__stat32i64(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _stat64i32(const char *path, struct _stat64i32 *buffer) {
    return mp__stat64i32(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _wstat32(const wchar_t *path, struct __stat32 *buffer) {
    return mp__wstat32(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _wstat64(const wchar_t *path, struct __stat64 *buffer) {
    return mp__wstat64(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _wstat32i64(const wchar_t *path, struct _stat32i64 *buffer) {
    return mp__wstat32i64(path, buffer);
}

ALWAYS_INLINE MP_DECL(int) _wstat64i32(const wchar_t *path, struct _stat64i32 *buffer) {
    return mp__wstat64i32(path, buffer);
}

ALWAYS_INLINE MP_STD(DWORD) GetFileAttributesA(_In_ LPCSTR lpFileName) {
    return mp_GetFileAttributesA(lpFileName);
}

ALWAYS_INLINE MP_STD(BOOL) GetFileAttributesExA(
        _In_ LPCSTR lpFileName,
        _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
        _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation) {
    return mp_GetFileAttributesExA(lpFileName, fInfoLevelId, lpFileInformation);
}

ALWAYS_INLINE MP_STD(BOOL) GetFileAttributesExW(
        _In_ LPCWSTR lpFileName,
        _In_ GET_FILEEX_INFO_LEVELS fInfoLevelId,
        _Out_writes_bytes_(sizeof(WIN32_FILE_ATTRIBUTE_DATA)) LPVOID lpFileInformation) {
    return mp_GetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
}

ALWAYS_INLINE MP_STD(DWORD) GetFileAttributesW(_In_ LPCWSTR lpFileName) {
    return mp_GetFileAttributesW(lpFileName);
}

ALWAYS_INLINE MP_STD(BOOL) CloseHandle(
    _In_ HANDLE hObject
) {
    return mp_CloseHandle(hObject);
}

ALWAYS_INLINE MP_STD(HANDLE) CreateFileA(
        _In_     LPCSTR                lpFileName,
        _In_     DWORD                 dwDesiredAccess,
        _In_     DWORD                 dwShareMode,
        _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
        _In_     DWORD                 dwCreationDisposition,
        _In_     DWORD                 dwFlagsAndAttributes,
        _In_opt_ HANDLE                hTemplateFile
) {
    return mp_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

ALWAYS_INLINE MP_STD(HANDLE) CreateFileW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_opt_ LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    _In_ DWORD dwCreationDisposition,
    _In_ DWORD dwFlagsAndAttributes,
    _In_opt_ HANDLE hTemplateFile
) {
    return mp_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

ALWAYS_INLINE MP_STD(BOOL) DeleteFileA(
        _In_ LPCSTR lpFileName
) {
    return mp_DeleteFileA(lpFileName);
}

ALWAYS_INLINE MP_STD(BOOL) DeleteFileW(
        _In_ LPCWSTR lpFileName
) {
    return mp_DeleteFileW(lpFileName);
}

ALWAYS_INLINE MP_STD(BOOL) GetFileInformationByHandle(
    _In_ HANDLE hFile,
    _Out_ LPBY_HANDLE_FILE_INFORMATION lpFileInformation
) {
    return mp_GetFileInformationByHandle(hFile, lpFileInformation);
}

ALWAYS_INLINE MP_STD(BOOL) GetFileInformationByHandleEx(
    _In_ HANDLE hFile,
    _In_ FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    _Out_writes_bytes_(dwBufferSize) LPVOID lpFileInformation,
    _In_ DWORD dwBufferSize
) {
    return mp_GetFileInformationByHandleEx(hFile, FileInformationClass, lpFileInformation, dwBufferSize);
}

ALWAYS_INLINE MP_STD(DWORD) GetFileType(
    _In_ HANDLE hFile
) {
    return mp_GetFileType(hFile);
}

ALWAYS_INLINE MP_STD(DWORD) GetFinalPathNameByHandleW(
    _In_ HANDLE hFile,
    _Out_writes_(cchFilePath) LPWSTR lpszFilePath,
    _In_ DWORD cchFilePath,
    _In_ DWORD dwFlags
) {
    return mp_GetFinalPathNameByHandleW(hFile, lpszFilePath, cchFilePath, dwFlags);
}

/**
 * @brief Inline wrapper for GetFullPathNameW to call our implementation.
 */
ALWAYS_INLINE MP_STD(DWORD) GetFullPathNameW(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD nBufferLength,
    _Out_writes_to_(nBufferLength, return + 1) LPWSTR lpBuffer,
    _Out_opt_ LPWSTR *lpFilePart
) {
    return mp_GetFullPathNameW(lpFileName, nBufferLength, lpBuffer, lpFilePart);
}

/**
 * @brief Inline wrapper for GetVolumePathNameW to call our implementation.
 */
ALWAYS_INLINE MP_STD(BOOL) GetVolumePathNameW(
    _In_ LPCWSTR lpszFileName,
    _Out_writes_to_(cchBufferLength, return != 0 ? wcslen(lpszVolumePathName) + 1 : 0) LPWSTR lpszVolumePathName,
    _In_ DWORD cchBufferLength
) {
    return mp_GetVolumePathNameW(lpszFileName, lpszVolumePathName, cchBufferLength);
}

/**
 * @brief Inline wrapper for GetDiskFreeSpaceExW to call our implementation.
 */
ALWAYS_INLINE MP_STD(BOOL) GetDiskFreeSpaceExW(
    _In_opt_ LPCWSTR lpDirectoryName,
    _Out_opt_ PULARGE_INTEGER lpFreeBytesAvailableToCaller,
    _Out_opt_ PULARGE_INTEGER lpTotalNumberOfBytes,
    _Out_opt_ PULARGE_INTEGER lpTotalNumberOfFreeBytes
) {
    return mp_GetDiskFreeSpaceExW(lpDirectoryName, lpFreeBytesAvailableToCaller, lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
}

ALWAYS_INLINE MP_STD(HANDLE) FindFirstFileA(
        _In_  LPCSTR             lpFileName,
        _Out_ LPWIN32_FIND_DATAA lpFindFileData
) {
    return mp_FindFirstFileA(lpFileName, lpFindFileData);
}

/**
 * @brief Inline wrapper for FindFirstFileW to call our implementation.
 */
ALWAYS_INLINE MP_STD(HANDLE) FindFirstFileW(
    _In_ LPCWSTR lpFileName,
    _Out_ LPWIN32_FIND_DATAW lpFindFileData
) {
    return mp_FindFirstFileW(lpFileName, lpFindFileData);
}

ALWAYS_INLINE MP_STD(BOOL) FindNextFileA(
        _In_  HANDLE             hFindFile,
        _Out_ LPWIN32_FIND_DATAA lpFindFileData
) {
    return mp_FindNextFileA(hFindFile, lpFindFileData);
}

/**
 * @brief Inline wrapper for FindNextFileW to call our implementation.
 */
ALWAYS_INLINE MP_STD(BOOL) FindNextFileW(
    _In_ HANDLE hFindFile,
    _Out_ LPWIN32_FIND_DATAW lpFindFileData
) {
    return mp_FindNextFileW(hFindFile, lpFindFileData);
}

/**
 * @brief Inline wrapper for FindClose to call our implementation.
 */
ALWAYS_INLINE MP_STD(BOOL) FindClose(
    _Inout_ HANDLE hFindFile
) {
    return mp_FindClose(hFindFile);
}

/**
 * @brief Retrieves the size of the specified file, in bytes.
 * @param hFile A handle to the file.
 * @param lpFileSize A pointer to a variable that receives the file size.
 * @return Nonzero on success, zero on failure.
 */
ALWAYS_INLINE MP_STD(BOOL) GetFileSizeEx(
        _In_  HANDLE         hFile,
        _Out_ PLARGE_INTEGER lpFileSize
) {
    return mp_GetFileSizeEx(hFile, lpFileSize);
}

/**
 * @brief Retrieves the size of the specified file.
 * @param hFile A handle to the file.
 * @param lpFileSizeHigh A pointer to the high-order doubleword of the file size.
 * @return The low-order doubleword of the file size, or INVALID_FILE_SIZE.
 */
ALWAYS_INLINE MP_STD(DWORD) GetFileSize(
        _In_      HANDLE  hFile,
        _Out_opt_ LPDWORD lpFileSizeHigh
) {
    return mp_GetFileSize(hFile, lpFileSizeHigh);
}

ALWAYS_INLINE MP_STD(HANDLE) CreateFileMappingA(
        _In_     HANDLE                hFile,
        _In_opt_ LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
        _In_     DWORD                 flProtect,
        _In_     DWORD                 dwMaximumSizeHigh,
        _In_     DWORD                 dwMaximumSizeLow,
        _In_opt_ LPCSTR                lpName
) {
    return mp_CreateFileMappingA(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
}

ALWAYS_INLINE MP_STD(HANDLE) CreateFileMappingW(
        _In_     HANDLE                hFile,
        _In_opt_ LPSECURITY_ATTRIBUTES lpFileMappingAttributes,
        _In_     DWORD                 flProtect,
        _In_     DWORD                 dwMaximumSizeHigh,
        _In_     DWORD                 dwMaximumSizeLow,
        _In_opt_ LPCWSTR               lpName
) {
    return mp_CreateFileMappingW(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, lpName);
}


/**
 * @brief Maps a view of a file mapping into the address space of a calling process.
 * @return The starting address of the mapped view on success, NULL on failure.
 */
ALWAYS_INLINE MP_STD(LPVOID) MapViewOfFile(
        _In_ HANDLE hFileMappingObject,
        _In_ DWORD  dwDesiredAccess,
        _In_ DWORD  dwFileOffsetHigh,
        _In_ DWORD  dwFileOffsetLow,
        _In_ SIZE_T dwNumberOfBytesToMap
) {
    return mp_MapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh, dwFileOffsetLow, dwNumberOfBytesToMap);
}

/**
 * @brief Unmaps a mapped view of a file from the calling process's address space.
 * @return Nonzero on success, zero on failure.
 */
ALWAYS_INLINE MP_STD(BOOL) UnmapViewOfFile(
        _In_ LPCVOID lpBaseAddress
) {
    return mp_UnmapViewOfFile(lpBaseAddress);
}

#else  /* _WIN32 */
ALWAYS_INLINE MP_DECL(int) stat(const char *path, struct stat *buf) {
    return mp_stat(path, buf);
}

ALWAYS_INLINE MP_DECL(int) fstat(int fd, struct stat *buf) {
    return mp_fstat(fd, buf);
}

ALWAYS_INLINE MP_DECL(int) fstatat(int dirfd, const char *pathname, struct stat *buf, int flags) {
    return mp_fstatat(dirfd, pathname, buf, flags);
}

ALWAYS_INLINE MP_DECL(int) lstat(const char *path, struct stat *buf) {
    return mp_lstat(path, buf);
}

/* Locking Functions */
ALWAYS_INLINE MP_DECL(void) flockfile(void *stream) {
    mp_flockfile(stream);
}
ALWAYS_INLINE MP_DECL(void) funlockfile(void *stream) {
    mp_funlockfile(stream);
}
ALWAYS_INLINE MP_DECL(int) ftrylockfile(void *stream) {
    return mp_ftrylockfile(stream);
}

ALWAYS_INLINE MP_DECL(int) access(const char *pathname, int mode) {
    return mp_access(pathname, mode);
}

ALWAYS_INLINE MP_DECL(int) faccessat(int dirfd, const char *pathname, int mode, int flags) {
    return mp_faccessat(dirfd, pathname, mode, flags);
}

ALWAYS_INLINE MP_DECL(int) statvfs(const char *path, struct statvfs *buf) {
    return mp_statvfs(path, buf);
}

ALWAYS_INLINE MP_DECL(int) fstatvfs(int fd, struct statvfs *buf) {
    return mp_fstatvfs(fd, buf);
}

ALWAYS_INLINE MP_DECL(ssize_t) readv(int fd, const struct iovec *iov, int iovcnt) {
    return mp_readv(fd, iov, iovcnt);
}

ALWAYS_INLINE MP_DECL(ssize_t) preadv(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    return mp_preadv(fd, iov, iovcnt, offset);
}

#ifdef __linux // preadv2 and pwritev2 are GNU extensions
ALWAYS_INLINE MP_DECL(ssize_t) preadv2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
    return mp_preadv2(fd, iov, iovcnt, offset, flags);
}
#endif  /* _WIN32 */

ALWAYS_INLINE MP_DECL(ssize_t) writev(int fd, const struct iovec *iov, int iovcnt) {
    return mp_writev(fd, iov, iovcnt);
}

ALWAYS_INLINE MP_DECL(ssize_t) pwritev(int fd, const struct iovec *iov, int iovcnt, off_t offset) {
    return mp_pwritev(fd, iov, iovcnt, offset);
}

#ifdef __linux // preadv2 and pwritev2 are GNU extensions
ALWAYS_INLINE MP_DECL(ssize_t) pwritev2(int fd, const struct iovec *iov, int iovcnt, off_t offset, int flags) {
    return mp_pwritev2(fd, iov, iovcnt, offset, flags);
}
#endif  /* _WIN32 */

ALWAYS_INLINE MP_DECL(ssize_t) pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return mp_pwrite(fd, buf, count, offset);
}

ALWAYS_INLINE MP_DECL(DIR *) opendir(const char *name) {
    return mp_opendir(name);
}

ALWAYS_INLINE MP_DECL(DIR *) fdopendir(int fd) {
    return mp_fdopendir(fd);
}

ALWAYS_INLINE MP_DECL(struct dirent *) readdir(DIR *dirp) {
    return mp_readdir(dirp);
}

ALWAYS_INLINE MP_DECL(int) closedir(DIR *dirp) {
    return mp_closedir(dirp);
}

ALWAYS_INLINE MP_DECL(void) rewinddir(DIR *dirp) {
    mp_rewinddir(dirp);
}
#endif  /* _WIN32 */

#endif  /* !MONOLITHPY_EMBED_BUILD && !MP_STDIO_ALREADY_LOADED */

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
#endif  /* __cplusplus <= 201103L */
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
#endif  /* _GLIBCXX_USE_TMPNAM */
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
#endif  /* _GLIBCXX_USE_C99_STDIO */

#if !_GLIBCXX_USE_C99_DYNAMIC
  using ::snprintf;
  using ::vfscanf;
  using ::vscanf;
  using ::vsnprintf;
  using ::vsscanf;
#endif  /* !_GLIBCXX_USE_C99_DYNAMIC */
} // namespace __gnu_cxx

namespace std
{
  using ::__gnu_cxx::snprintf;
  using ::__gnu_cxx::vfscanf;
  using ::__gnu_cxx::vscanf;
  using ::__gnu_cxx::vsnprintf;
  using ::__gnu_cxx::vsscanf;
} // namespace std

#endif  /* _GLIBCXX_USE_C99_STDIO */

#endif  /* _GLIBCXX_CSTDIO */
#endif  /* __cplusplus */

#endif  /* !MONOLITHPY_EMBED_BUILD && !MP_STDIO_ALREADY_LOADED */
#endif  /* !__ASSEMBLER__ && !BYPASS_MP_EMBED */

#endif  /* MONOLITHPYEMBED */
