/**
* @file svcrt_version.h
* @brief SVCrtOS release version, defined in exactly one place.
* @details The kernel, the shell and any application that includes svcrt.h read
*          these macros, so the version never has to be retyped as a string.
*          The git tag and the README badge carry the same three numbers and
*          must be changed together with this file.
*
*          Scheme: MAJOR.MINOR.PATCH, matching the tag name one for one
*          (tag 0.0.2  ->  0 . 0 . 2).
* @note    The three numbers are written without parentheses and without a
*          suffix on purpose: SVCRT_VERSION_STRING stringifies them, and the
*          preprocessor keeps any parentheses it sees in the produced string
*          (a value written as (0u) would render as "(0u)"). The unsigned casts
*          that the packed word needs live in SVCRT_VERSION_NUM instead.
* @author xw
* @date 2026.09.24
*/

#ifndef __SVCRT_VERSION_H__
#define __SVCRT_VERSION_H__

/** @defgroup version Kernel release version
 *  @{
 */

#define SVCRT_VERSION_MAJOR     0   /*!< Major, matches the first tag field.  */
#define SVCRT_VERSION_MINOR     0   /*!< Minor, matches the second tag field. */
#define SVCRT_VERSION_PATCH     2   /*!< Patch, matches the third tag field.  */

/** The three numbers packed into one word: major << 24 | minor << 16 | patch << 8. */
#define SVCRT_VERSION_NUM       (((unsigned)SVCRT_VERSION_MAJOR << 24) | \
                                 ((unsigned)SVCRT_VERSION_MINOR << 16) | \
                                 ((unsigned)SVCRT_VERSION_PATCH << 8))

/* Stringification helpers. The string is derived from the numbers above, so the
 * two can never drift apart and no macro has to be edited twice. */
#define SVCRT_VERSION_STR_(x)   #x
#define SVCRT_VERSION_STR(x)    SVCRT_VERSION_STR_(x)
#define SVCRT_VERSION_STRING    SVCRT_VERSION_STR(SVCRT_VERSION_MAJOR) "." \
                                SVCRT_VERSION_STR(SVCRT_VERSION_MINOR) "." \
                                SVCRT_VERSION_STR(SVCRT_VERSION_PATCH)

/** @} */

#endif /* __SVCRT_VERSION_H__ */
