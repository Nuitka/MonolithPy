#include <Shlwapi.h>
#include "windows.h"

#define DLLAPI	__stdcall

#define PATHCCH_NONE                            0x00
#define PATHCCH_ALLOW_LONG_PATHS                0x01
#define PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS  0x02
#define PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS 0x04
#define PATHCCH_DO_NOT_NORMALIZE_SEGMENTS       0x08
#define PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH  0x10
#define PATHCCH_ENSURE_TRAILING_SLASH           0x20

#define PATHCCH_MAX_CCH 0x8000

#define STRSAFE_E_INSUFFICIENT_BUFFER ((HRESULT)0x8007007AL)

HRESULT DLLAPI PathAllocCanonicalize(const WCHAR *path_in, DWORD flags, WCHAR **path_out);
HRESULT DLLAPI PathAllocCombine(const WCHAR *path1, const WCHAR *path2, DWORD flags, WCHAR **out);
HRESULT DLLAPI PathCchAddBackslash(WCHAR *path, SIZE_T size);
HRESULT DLLAPI PathCchAddBackslashEx(WCHAR *path, SIZE_T size, WCHAR **endptr, SIZE_T *remaining);
HRESULT DLLAPI PathCchAddExtension(WCHAR *path, SIZE_T size, const WCHAR *extension);
HRESULT DLLAPI PathCchAppend(WCHAR *path1, SIZE_T size, const WCHAR *path2);
HRESULT DLLAPI PathCchAppendEx(WCHAR *path1, SIZE_T size, const WCHAR *path2, DWORD flags);
HRESULT DLLAPI PathCchCanonicalize(WCHAR *out, SIZE_T size, const WCHAR *in);
HRESULT DLLAPI PathCchCanonicalizeEx(WCHAR *out, SIZE_T size, const WCHAR *in, DWORD flags);
HRESULT DLLAPI PathCchCombine(WCHAR *out, SIZE_T size, const WCHAR *path1, const WCHAR *path2);
HRESULT DLLAPI PathCchCombineEx(WCHAR *out, SIZE_T size, const WCHAR *path1, const WCHAR *path2, DWORD flags);
HRESULT DLLAPI PathCchFindExtension(const WCHAR *path, SIZE_T size, const WCHAR **extension);
BOOL DLLAPI PathCchIsRoot(const WCHAR *path);
HRESULT DLLAPI PathCchRemoveBackslash(WCHAR *path, SIZE_T path_size);
HRESULT DLLAPI PathCchRemoveBackslashEx(WCHAR *path, SIZE_T path_size, WCHAR **path_end, SIZE_T *free_size);
HRESULT DLLAPI PathCchRemoveExtension(WCHAR *path, SIZE_T size);
HRESULT DLLAPI PathCchRemoveFileSpec(WCHAR *path, SIZE_T size);
HRESULT DLLAPI PathCchRenameExtension(WCHAR *path, SIZE_T size, const WCHAR *extension);
HRESULT DLLAPI PathCchSkipRoot(const WCHAR *path, const WCHAR **root_end);
HRESULT DLLAPI PathCchStripPrefix(WCHAR *path, SIZE_T size);
HRESULT DLLAPI PathCchStripToRoot(WCHAR *path, SIZE_T size);
BOOL DLLAPI PathIsUNCEx(const WCHAR *path, const WCHAR **server);
