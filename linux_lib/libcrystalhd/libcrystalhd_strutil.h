#ifndef LIBCRYSTALHD_STRUTIL_H
#define LIBCRYSTALHD_STRUTIL_H

#include <cstddef>
#include <cstdio>
#include <cstring>

inline bool dts_copy_string(char *dst, size_t dst_len, const char *src)
{
    if (!dst || !dst_len)
        return false;

    if (!src)
        src = "";

    int written = ::snprintf(dst, dst_len, "%s", src);
    return written >= 0 && static_cast<size_t>(written) < dst_len;
}

inline bool dts_append_path(char *base, size_t base_len, const char *suffix)
{
    if (!base || !suffix)
        return false;

    size_t cur = ::strnlen(base, base_len);
    if (cur >= base_len)
        return false;

    size_t avail = base_len - cur;
    int written = ::snprintf(base + cur, avail, "%s", suffix);

    return written >= 0 && static_cast<size_t>(written) < avail;
}

#endif  // LIBCRYSTALHD_STRUTIL_H
