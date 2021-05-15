#include "pathcch.private.h"

#include <Shlwapi.h>

static BOOL is_drive_spec(const WCHAR *str)
{
	return ((str[0] >= 'A' && str[0] <= 'Z') || (str[0] >= 'a' && str[0] <= 'z')) && str[1] == ':';
}

static BOOL is_prefixed_unc(const WCHAR *string)
{
	return !_wcsnicmp(string, L"\\\\?\\UNC\\", 8);
}

static BOOL is_prefixed_disk(const WCHAR *string)
{
	return !wcsncmp(string, L"\\\\?\\", 4) && is_drive_spec(string + 4);
}

static BOOL is_prefixed_volume(const WCHAR *string)
{
	const WCHAR *guid;
	INT i = 0;

	if (_wcsnicmp(string, L"\\\\?\\Volume", 10)) return FALSE;

	guid = string + 10;

	while (i <= 37)
	{
		switch (i)
		{
		case 0:
			if (guid[i] != '{') return FALSE;
			break;
		case 9:
		case 14:
		case 19:
		case 24:
			if (guid[i] != '-') return FALSE;
			break;
		case 37:
			if (guid[i] != '}') return FALSE;
			break;
		default:
			if (!isxdigit(guid[i])) return FALSE;
			break;
		}
		i++;
	}

	return TRUE;
}

/* Get the next character beyond end of the segment.
   Return TRUE if the last segment ends with a backslash */
static BOOL get_next_segment(const WCHAR *next, const WCHAR **next_segment)
{
	while (*next && *next != '\\') next++;
	if (*next == '\\')
	{
		*next_segment = next + 1;
		return TRUE;
	}
	else
	{
		*next_segment = next;
		return FALSE;
	}
}

/* Find the last character of the root in a path, if there is one, without any segments */
static const WCHAR *get_root_end(const WCHAR *path)
{
	/* Find path root */
	if (is_prefixed_volume(path))
		return path[48] == '\\' ? path + 48 : path + 47;
	else if (is_prefixed_unc(path))
		return path + 7;
	else if (is_prefixed_disk(path))
		return path[6] == '\\' ? path + 6 : path + 5;
	/* \\ */
	else if (path[0] == '\\' && path[1] == '\\')
		return path + 1;
	/* \ */
	else if (path[0] == '\\')
		return path;
	/* X:\ */
	else if (is_drive_spec(path))
		return path[2] == '\\' ? path + 2 : path + 1;
	else
		return NULL;
}

HRESULT DLLAPI PathAllocCanonicalize(const WCHAR *path_in, DWORD flags, WCHAR **path_out)
{
	WCHAR *buffer, *dst;
	const WCHAR *src;
	const WCHAR *root_end;
	SIZE_T buffer_size, length;

	if (!path_in || !path_out
		|| ((flags & PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS) && (flags & PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS))
		|| (flags & (PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS | PATHCCH_FORCE_DISABLE_LONG_NAME_PROCESS)
			&& !(flags & PATHCCH_ALLOW_LONG_PATHS))
		|| ((flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH) && (flags & PATHCCH_ALLOW_LONG_PATHS)))
	{
		if (path_out) *path_out = NULL;
		return E_INVALIDARG;
	}

	length = lstrlenW(path_in);
	if ((length + 1 > MAX_PATH && !(flags & (PATHCCH_ALLOW_LONG_PATHS | PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH)))
		|| (length + 1 > PATHCCH_MAX_CCH))
	{
		*path_out = NULL;
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
	}

	/* PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH implies PATHCCH_DO_NOT_NORMALIZE_SEGMENTS */
	if (flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH) flags |= PATHCCH_DO_NOT_NORMALIZE_SEGMENTS;

	/* path length + possible \\?\ addition + possible \ addition + NUL */
	buffer_size = (length + 6) * sizeof(WCHAR);
	buffer = LocalAlloc(LMEM_ZEROINIT, buffer_size);
	if (!buffer)
	{
		*path_out = NULL;
		return E_OUTOFMEMORY;
	}

	src = path_in;
	dst = buffer;

	root_end = get_root_end(path_in);
	if (root_end) root_end = buffer + (root_end - path_in);

	/* Copy path root */
	if (root_end)
	{
		memcpy(dst, src, (root_end - buffer + 1) * sizeof(WCHAR));
		src += root_end - buffer + 1;
		if (PathCchStripPrefix(dst, length + 6) == S_OK)
		{
			/* Fill in \ in X:\ if the \ is missing */
			if (is_drive_spec(dst) && dst[2] != '\\')
			{
				dst[2] = '\\';
				dst[3] = 0;
			}
			dst = buffer + lstrlenW(buffer);
			root_end = dst;
		}
		else
			dst += root_end - buffer + 1;
	}

	while (*src)
	{
		if (src[0] == '.')
		{
			if (src[1] == '.')
			{
				/* Keep one . after * */
				if (dst > buffer && dst[-1] == '*')
				{
					*dst++ = *src++;
					continue;
				}

				/* Keep the .. if not surrounded by \ */
				if ((src[2] != '\\' && src[2]) || (dst > buffer && dst[-1] != '\\'))
				{
					*dst++ = *src++;
					*dst++ = *src++;
					continue;
				}

				/* Remove the \ before .. if the \ is not part of root */
				if (dst > buffer && dst[-1] == '\\' && (!root_end || dst - 1 > root_end))
				{
					*--dst = '\0';
					/* Remove characters until a \ is encountered */
					while (dst > buffer)
					{
						if (dst[-1] == '\\')
						{
							*--dst = 0;
							break;
						}
						else
							*--dst = 0;
					}
				}
				/* Remove the extra \ after .. if the \ before .. wasn't deleted */
				else if (src[2] == '\\')
					src++;

				src += 2;
			}
			else
			{
				/* Keep the . if not surrounded by \ */
				if ((src[1] != '\\' && src[1]) || (dst > buffer && dst[-1] != '\\'))
				{
					*dst++ = *src++;
					continue;
				}

				/* Remove the \ before . if the \ is not part of root */
				if (dst > buffer && dst[-1] == '\\' && (!root_end || dst - 1 > root_end)) dst--;
				/* Remove the extra \ after . if the \ before . wasn't deleted */
				else if (src[1] == '\\')
					src++;

				src++;
			}

			/* If X:\ is not complete, then complete it */
			if (is_drive_spec(buffer) && buffer[2] != '\\')
			{
				root_end = buffer + 2;
				dst = buffer + 3;
				buffer[2] = '\\';
				/* If next character is \, use the \ to fill in */
				if (src[0] == '\\') src++;
			}
		}
		/* Copy over */
		else
			*dst++ = *src++;
	}
	/* End the path */
	*dst = 0;

	/* Strip multiple trailing . */
	if (!(flags & PATHCCH_DO_NOT_NORMALIZE_SEGMENTS))
	{
		while (dst > buffer && dst[-1] == '.')
		{
			/* Keep a . after * */
			if (dst - 1 > buffer && dst[-2] == '*')
				break;
			/* If . follow a : at the second character, remove the . and add a \ */
			else if (dst - 1 > buffer && dst[-2] == ':' && dst - 2 == buffer + 1)
				*--dst = '\\';
			else
				*--dst = 0;
		}
	}

	/* If result path is empty, fill in \ */
	if (!*buffer)
	{
		buffer[0] = '\\';
		buffer[1] = 0;
	}

	/* Extend the path if needed */
	length = lstrlenW(buffer);
	if (((length + 1 > MAX_PATH && is_drive_spec(buffer))
		|| (is_drive_spec(buffer) && flags & PATHCCH_ENSURE_IS_EXTENDED_LENGTH_PATH))
		&& !(flags & PATHCCH_FORCE_ENABLE_LONG_NAME_PROCESS))
	{
		memmove(buffer + 4, buffer, (length + 1) * sizeof(WCHAR));
		buffer[0] = '\\';
		buffer[1] = '\\';
		buffer[2] = '?';
		buffer[3] = '\\';
	}

	/* Add a trailing backslash to the path if needed */
	if (flags & PATHCCH_ENSURE_TRAILING_SLASH)
		PathCchAddBackslash(buffer, buffer_size);

	*path_out = buffer;
	return S_OK;
}

HRESULT DLLAPI PathAllocCombine(const WCHAR *path1, const WCHAR *path2, DWORD flags, WCHAR **out)
{
	SIZE_T combined_length, length2;
	WCHAR *combined_path;
	BOOL from_path2 = FALSE;
	HRESULT hr;

	if ((!path1 && !path2) || !out)
	{
		if (out) *out = NULL;
		return E_INVALIDARG;
	}

	if (!path1 || !path2) return PathAllocCanonicalize(path1 ? path1 : path2, flags, out);

	/* If path2 is fully qualified, use path2 only */
	if (is_drive_spec(path2) || (path2[0] == '\\' && path2[1] == '\\'))
	{
		path1 = path2;
		path2 = NULL;
		from_path2 = TRUE;
	}

	length2 = path2 ? lstrlenW(path2) : 0;
	/* path1 length + path2 length + possible backslash + NULL */
	combined_length = lstrlenW(path1) + length2 + 2;

	combined_path = HeapAlloc(GetProcessHeap(), 0, combined_length * sizeof(WCHAR));
	if (!combined_path)
	{
		*out = NULL;
		return E_OUTOFMEMORY;
	}

	lstrcpyW(combined_path, path1);
	PathCchStripPrefix(combined_path, combined_length);
	if (from_path2) PathCchAddBackslashEx(combined_path, combined_length, NULL, NULL);

	if (path2 && path2[0])
	{
		if (path2[0] == '\\' && path2[1] != '\\')
		{
			PathCchStripToRoot(combined_path, combined_length);
			path2++;
		}

		PathCchAddBackslashEx(combined_path, combined_length, NULL, NULL);
		lstrcatW(combined_path, path2);
	}

	hr = PathAllocCanonicalize(combined_path, flags, out);
	HeapFree(GetProcessHeap(), 0, combined_path);
	return hr;
}

HRESULT DLLAPI PathCchAddBackslash(WCHAR *path, SIZE_T size)
{
	return PathCchAddBackslashEx(path, size, NULL, NULL);
}

HRESULT DLLAPI PathCchAddBackslashEx(WCHAR *path, SIZE_T size, WCHAR **endptr, SIZE_T *remaining)
{
    BOOL needs_termination;
    SIZE_T length;

    length = lstrlenW(path);
    needs_termination = size && length && path[length - 1] != '\\';

    if (length >= (needs_termination ? size - 1 : size))
    {
        if (endptr) *endptr = NULL;
        if (remaining) *remaining = 0;
        return STRSAFE_E_INSUFFICIENT_BUFFER;
    }

    if (!needs_termination)
    {
        if (endptr) *endptr = path + length;
        if (remaining) *remaining = size - length;
        return S_FALSE;
    }

    path[length++] = '\\';
    path[length] = 0;

    if (endptr) *endptr = path + length;
    if (remaining) *remaining = size - length;

    return S_OK;
}

HRESULT DLLAPI PathCchAddExtension(WCHAR *path, SIZE_T size, const WCHAR *extension)
{
    const WCHAR *existing_extension, *next;
    SIZE_T path_length, extension_length, dot_length;
    BOOL has_dot;
    HRESULT hr;

    if (!path || !size || size > PATHCCH_MAX_CCH || !extension) return E_INVALIDARG;

    next = extension;
    while (*next)
    {
        if ((*next == '.' && next > extension) || *next == ' ' || *next == '\\') return E_INVALIDARG;
        next++;
    }

    has_dot = extension[0] == '.';

    hr = PathCchFindExtension(path, size, &existing_extension);
    if (FAILED(hr)) return hr;
    if (*existing_extension) return S_FALSE;

    path_length = wcsnlen_s(path, size);
    dot_length = has_dot ? 0 : 1;
    extension_length = lstrlenW(extension);

    if (path_length + dot_length + extension_length + 1 > size) return STRSAFE_E_INSUFFICIENT_BUFFER;

    /* If extension is empty or only dot, return S_OK with path unchanged */
    if (!extension[0] || (extension[0] == '.' && !extension[1])) return S_OK;

    if (!has_dot)
    {
        path[path_length] = '.';
        path_length++;
    }

    lstrcpyW(path + path_length, extension);
    return S_OK;
}

HRESULT DLLAPI PathCchAppend(WCHAR *path1, SIZE_T size, const WCHAR *path2)
{
    return PathCchAppendEx(path1, size, path2, PATHCCH_NONE);
}

HRESULT DLLAPI PathCchAppendEx(WCHAR *path1, SIZE_T size, const WCHAR *path2, DWORD flags)
{
    HRESULT hr;
    WCHAR *result;

    if (!path1 || !size) return E_INVALIDARG;

    /* Create a temporary buffer for result because we need to keep path1 unchanged if error occurs.
     * And PathCchCombineEx writes empty result if there is error so we can't just use path1 as output
     * buffer for PathCchCombineEx */
    result = HeapAlloc(GetProcessHeap(), 0, size * sizeof(WCHAR));
    if (!result) return E_OUTOFMEMORY;

    /* Avoid the single backslash behavior with PathCchCombineEx when appending */
    if (path2 && path2[0] == '\\' && path2[1] != '\\') path2++;

    hr = PathCchCombineEx(result, size, path1, path2, flags);
    if (SUCCEEDED(hr)) memcpy(path1, result, size * sizeof(WCHAR));

    HeapFree(GetProcessHeap(), 0, result);
    return hr;
}

HRESULT DLLAPI PathCchCanonicalize(WCHAR *out, SIZE_T size, const WCHAR *in)
{
	/* Not X:\ and path > MAX_PATH - 4, return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) */
	if (lstrlenW(in) > MAX_PATH - 4 && !(is_drive_spec(in) && in[2] == '\\'))
		return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

	return PathCchCanonicalizeEx(out, size, in, PATHCCH_NONE);
}

HRESULT DLLAPI PathCchCanonicalizeEx(WCHAR *out, SIZE_T size, const WCHAR *in, DWORD flags)
{
	WCHAR *buffer;
	SIZE_T length;
	HRESULT hr;

	if (!size) return E_INVALIDARG;

	hr = PathAllocCanonicalize(in, flags, &buffer);
	if (FAILED(hr)) return hr;

	length = lstrlenW(buffer);
	if (size < length + 1)
	{
		/* No root and path > MAX_PATH - 4, return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) */
		if (length > MAX_PATH - 4 && !(in[0] == '\\' || (is_drive_spec(in) && in[2] == '\\')))
			hr = HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
		else
			hr = STRSAFE_E_INSUFFICIENT_BUFFER;
	}

	if (SUCCEEDED(hr))
	{
		memcpy(out, buffer, (length + 1) * sizeof(WCHAR));

		/* Fill a backslash at the end of X: */
		if (is_drive_spec(out) && !out[2] && size > 3)
		{
			out[2] = '\\';
			out[3] = 0;
		}
	}

	LocalFree(buffer);
	return hr;
}

HRESULT DLLAPI PathCchCombine(WCHAR *out, SIZE_T size, const WCHAR *path1, const WCHAR *path2)
{
	return PathCchCombineEx(out, size, path1, path2, PATHCCH_NONE);
}

HRESULT DLLAPI PathCchCombineEx(WCHAR *out, SIZE_T size, const WCHAR *path1, const WCHAR *path2, DWORD flags)
{
	HRESULT hr;
	WCHAR *buffer;
	SIZE_T length;

	if (!out || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

	hr = PathAllocCombine(path1, path2, flags, &buffer);
	if (FAILED(hr))
	{
		out[0] = 0;
		return hr;
	}

	length = lstrlenW(buffer);
	if (length + 1 > size)
	{
		out[0] = 0;
		LocalFree(buffer);
		return STRSAFE_E_INSUFFICIENT_BUFFER;
	}
	else
	{
		memcpy(out, buffer, (length + 1) * sizeof(WCHAR));
		LocalFree(buffer);
		return S_OK;
	}
}

HRESULT DLLAPI PathCchFindExtension(const WCHAR *path, SIZE_T size, const WCHAR **extension)
{
    const WCHAR *lastpoint = NULL;
    SIZE_T counter = 0;

    if (!path || !size || size > PATHCCH_MAX_CCH)
    {
        *extension = NULL;
        return E_INVALIDARG;
    }

    while (*path)
    {
        if (*path == '\\' || *path == ' ')
            lastpoint = NULL;
        else if (*path == '.')
            lastpoint = path;

        path++;
        counter++;
        if (counter == size || counter == PATHCCH_MAX_CCH)
        {
            *extension = NULL;
            return E_INVALIDARG;
        }
    }

    *extension = lastpoint ? lastpoint : path;
    return S_OK;
}

BOOL DLLAPI PathCchIsRoot(const WCHAR *path)
{
	const WCHAR *root_end;
	const WCHAR *next;
	BOOL is_unc;

	if (!path || !*path) return FALSE;

	root_end = get_root_end(path);
	if (!root_end) return FALSE;

	if ((is_unc = is_prefixed_unc(path)) || (path[0] == '\\' && path[1] == '\\' && path[2] != '?'))
	{
		next = root_end + 1;
		/* No extra segments */
		if ((is_unc && !*next) || (!is_unc && !*next)) return TRUE;

		/* Has first segment with an ending backslash but no remaining characters */
		if (get_next_segment(next, &next) && !*next) return FALSE;
		/* Has first segment with no ending backslash */
		else if (!*next)
			return TRUE;
		/* Has first segment with an ending backslash and has remaining characters*/
		else
		{
			next++;
			/* Second segment must have no backslash and no remaining characters */
			return !get_next_segment(next, &next) && !*next;
		}
	}
	else if (*root_end == '\\' && !root_end[1])
		return TRUE;
	else
		return FALSE;
}

HRESULT DLLAPI PathCchRemoveBackslash(WCHAR *path, SIZE_T path_size)
{
    WCHAR *path_end;
    SIZE_T free_size;

    return PathCchRemoveBackslashEx(path, path_size, &path_end, &free_size);
}

HRESULT DLLAPI PathCchRemoveBackslashEx(WCHAR *path, SIZE_T path_size, WCHAR **path_end, SIZE_T *free_size)
{
    const WCHAR *root_end;
    SIZE_T path_length;

    if (!path_size || !path_end || !free_size)
    {
        if (path_end) *path_end = NULL;
        if (free_size) *free_size = 0;
        return E_INVALIDARG;
    }

    path_length = wcsnlen_s(path, path_size);
    if (path_length == path_size && !path[path_length]) return E_INVALIDARG;

    root_end = get_root_end(path);
    if (path_length > 0 && path[path_length - 1] == '\\')
    {
        *path_end = path + path_length - 1;
        *free_size = path_size - path_length + 1;
        /* If the last character is beyond end of root */
        if (!root_end || path + path_length - 1 > root_end)
        {
            path[path_length - 1] = 0;
            return S_OK;
        }
        else
            return S_FALSE;
    }
    else
    {
        *path_end = path + path_length;
        *free_size = path_size - path_length;
        return S_FALSE;
    }
}

HRESULT DLLAPI PathCchRemoveExtension(WCHAR *path, SIZE_T size)
{
	const WCHAR *extension;
	WCHAR *next;
	HRESULT hr;

	if (!path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

	hr = PathCchFindExtension(path, size, &extension);
	if (FAILED(hr)) return hr;

	next = path + (extension - path);
	while (next - path < size && *next) *next++ = 0;

	return next == extension ? S_FALSE : S_OK;
}

HRESULT DLLAPI PathCchRemoveFileSpec(WCHAR *path, SIZE_T size)
{
	const WCHAR *root_end = NULL;
	SIZE_T length;
	WCHAR *last;

	if (!path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

	if (PathCchIsRoot(path)) return S_FALSE;

	PathCchSkipRoot(path, &root_end);

	/* The backslash at the end of UNC and \\* are not considered part of root in this case */
	if (root_end && root_end > path && root_end[-1] == '\\'
		&& (is_prefixed_unc(path) || (path[0] == '\\' && path[1] == '\\' && path[2] != '?')))
		root_end--;

	length = lstrlenW(path);
	last = path + length - 1;
	while (last >= path && (!root_end || last >= root_end))
	{
		if (last - path >= size) return E_INVALIDARG;

		if (*last == '\\')
		{
			*last-- = 0;
			break;
		}

		*last-- = 0;
	}

	return last != path + length - 1 ? S_OK : S_FALSE;
}

HRESULT DLLAPI PathCchRenameExtension(WCHAR *path, SIZE_T size, const WCHAR *extension)
{
    HRESULT hr;

    hr = PathCchRemoveExtension(path, size);
    if (FAILED(hr)) return hr;

    hr = PathCchAddExtension(path, size, extension);
    return FAILED(hr) ? hr : S_OK;
}

HRESULT DLLAPI PathCchSkipRoot(const WCHAR *path, const WCHAR **root_end)
{
    if (!path || !path[0] || !root_end
        || (!_wcsnicmp(path, L"\\\\?", 3) && !is_prefixed_volume(path) && !is_prefixed_unc(path)
            && !is_prefixed_disk(path)))
        return E_INVALIDARG;

    *root_end = get_root_end(path);
    if (*root_end)
    {
        (*root_end)++;
        if (is_prefixed_unc(path))
        {
            get_next_segment(*root_end, root_end);
            get_next_segment(*root_end, root_end);
        }
        else if (path[0] == '\\' && path[1] == '\\' && path[2] != '?')
        {
            /* Skip share server */
            get_next_segment(*root_end, root_end);
            /* If mount point is empty, don't skip over mount point */
            if (**root_end != '\\') get_next_segment(*root_end, root_end);
        }
    }

    return *root_end ? S_OK : E_INVALIDARG;
}

HRESULT DLLAPI PathCchStripPrefix(WCHAR *path, SIZE_T size)
{
	if (!path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

	if (is_prefixed_unc(path))
	{
		/* \\?\UNC\a -> \\a */
		if (size < lstrlenW(path + 8) + 3) return E_INVALIDARG;
		lstrcpyW(path + 2, path + 8);
		return S_OK;
	}
	else if (is_prefixed_disk(path))
	{
		/* \\?\C:\ -> C:\ */
		if (size < lstrlenW(path + 4) + 1) return E_INVALIDARG;
		lstrcpyW(path, path + 4);
		return S_OK;
	}
	else
		return S_FALSE;
}

HRESULT DLLAPI PathCchStripToRoot(WCHAR *path, SIZE_T size)
{
	const WCHAR *root_end;
	WCHAR *segment_end;
	BOOL is_unc;

	if (!path || !*path || !size || size > PATHCCH_MAX_CCH) return E_INVALIDARG;

	/* \\\\?\\UNC\\* and \\\\* have to have at least two extra segments to be striped,
	 * e.g. \\\\?\\UNC\\a\\b\\c -> \\\\?\\UNC\\a\\b
	 *      \\\\a\\b\\c         -> \\\\a\\b         */
	if ((is_unc = is_prefixed_unc(path)) || (path[0] == '\\' && path[1] == '\\' && path[2] != '?'))
	{
		root_end = is_unc ? path + 8 : path + 3;
		if (!get_next_segment(root_end, &root_end)) return S_FALSE;
		if (!get_next_segment(root_end, &root_end)) return S_FALSE;

		if (root_end - path >= size) return E_INVALIDARG;

		segment_end = path + (root_end - path) - 1;
		*segment_end = 0;
		return S_OK;
	}
	else if (PathCchSkipRoot(path, &root_end) == S_OK)
	{
		if (root_end - path >= size) return E_INVALIDARG;

		segment_end = path + (root_end - path);
		if (!*segment_end) return S_FALSE;

		*segment_end = 0;
		return S_OK;
	}
	else
		return E_INVALIDARG;
}

BOOL DLLAPI PathIsUNCEx(const WCHAR *path, const WCHAR **server)
{
    const WCHAR *result = NULL;

    if (is_prefixed_unc(path))
        result = path + 8;
    else if (path[0] == '\\' && path[1] == '\\' && path[2] != '?')
        result = path + 2;

    if (server) *server = result;
    return !!result;
}
