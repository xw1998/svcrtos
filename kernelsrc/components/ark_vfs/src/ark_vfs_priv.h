/**
* @file ark_vfs_priv.h
* @brief Internal helpers shared by the core and the bundled filesystems.
*
* Not installed, not part of the public API.  Everything here exists because
* the library refuses to call into libc: a bare metal target may not have a
* string.h worth the name, and pulling one in would turn "portable" into
* "portable as long as your toolchain ships newlib".
*/
#ifndef __ARK_VFS_PRIV_H__
#define __ARK_VFS_PRIV_H__

#include "ark_vfs.h"

/* ---- tiny string/memory helpers (no libc) ---- */

static inline void ark_p_memcpy(void *dst, const void *src, ark_size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n-- > 0u)
    {
        *d++ = *s++;
    }
}

static inline void ark_p_memset(void *dst, int c, ark_size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n-- > 0u)
    {
        *d++ = (uint8_t)c;
    }
}

static inline ark_size_t ark_p_strlen(const char *s)
{
    ark_size_t n = 0u;

    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static inline int ark_p_strcmp(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/** @brief Compare the first @p n bytes. */
static inline int ark_p_strncmp(const char *a, const char *b, ark_size_t n)
{
    while ((n > 0u) && (*a != '\0') && (*a == *b))
    {
        a++;
        b++;
        n--;
    }
    if (n == 0u)
    {
        return 0;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/** @brief Copy a NUL terminated string into a fixed buffer.
 *  @return length copied, or a negative error code if it does not fit. */
static inline int32_t ark_p_strcpy(char *dst, ark_size_t cap, const char *src)
{
    ark_size_t i = 0u;

    if ((dst == NULL) || (src == NULL) || (cap == 0u))
    {
        return ARK_E_INVAL;
    }
    while (src[i] != '\0')
    {
        if ((i + 1u) >= cap)
        {
            dst[0] = '\0';
            return ARK_E_NAMETOOLONG;
        }
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return (int32_t)i;
}

/** @brief Does @p name end with @p suffix? */
static inline int ark_p_ends_with(const char *name, const char *suffix)
{
    ark_size_t ln = ark_p_strlen(name);
    ark_size_t ls = ark_p_strlen(suffix);

    if (ls > ln)
    {
        return 0;
    }
    return (ark_p_strcmp(name + (ln - ls), suffix) == 0) ? 1 : 0;
}

/** @brief Exact match against one of a NUL separated, double-NUL ended list. */
static inline int ark_p_name_in(const char *name, const char *list, ark_size_t list_len)
{
    ark_size_t i = 0u;

    while (i < list_len)
    {
        const char *cand = list + i;

        if (ark_p_strcmp(name, cand) == 0)
        {
            return 1;
        }
        i += ark_p_strlen(cand) + 1u;
    }
    return 0;
}

#endif /* __ARK_VFS_PRIV_H__ */
