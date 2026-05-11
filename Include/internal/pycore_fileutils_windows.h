#ifndef Py_INTERNAL_FILEUTILS_WINDOWS_H
#define Py_INTERNAL_FILEUTILS_WINDOWS_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#ifdef MS_WINDOWS

#if !defined(NTDDI_WIN10_NI) || !(NTDDI_VERSION >= NTDDI_WIN10_NI)
typedef struct _FILE_STAT_BASIC_INFORMATION {
    LARGE_INTEGER FileId;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG FileAttributes;
    ULONG ReparseTag;
    ULONG NumberOfLinks;
    ULONG DeviceType;
    ULONG DeviceCharacteristics;
    ULONG Reserved;
    LARGE_INTEGER VolumeSerialNumber;
    FILE_ID_128 FileId128;
} FILE_STAT_BASIC_INFORMATION;

typedef enum _FILE_INFO_BY_NAME_CLASS {
    FileStatByNameInfo,
    FileStatLxByNameInfo,
    FileCaseSensitiveByNameInfo,
    FileStatBasicByNameInfo,
    MaximumFileInfoByNameClass
} FILE_INFO_BY_NAME_CLASS;
#endif

typedef BOOL (WINAPI *PGetFileInformationByName)(
    PCWSTR FileName,
    FILE_INFO_BY_NAME_CLASS FileInformationClass,
    PVOID FileInfoBuffer,
    ULONG FileInfoBufferSize
);

static inline BOOL _Py_GetFileInformationByName(
    PCWSTR FileName,
    FILE_INFO_BY_NAME_CLASS FileInformationClass,
    PVOID FileInfoBuffer,
    ULONG FileInfoBufferSize
) {
    /* MonolithPy: always report the fast path as unsupported so Python falls
       back to win32_xstat_slow_impl, which gets routed through the vfs. */
    (void)FileName; (void)FileInformationClass;
    (void)FileInfoBuffer; (void)FileInfoBufferSize;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

static inline BOOL _Py_GetFileInformationByName_ErrorIsTrustworthy(int error)
{
    switch(error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_NOT_READY:
        case ERROR_BAD_NET_NAME:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_NAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return TRUE;
        case ERROR_NOT_SUPPORTED:
            return FALSE;
    }
    return FALSE;
}

#endif

#endif
