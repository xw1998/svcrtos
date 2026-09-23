/**
* @file svcrt_sign.h
* @brief App image signature check (HMAC-SHA256).  Optional: SVCRT_USE_IMAGE_SIGN.
* @details The image header has always carried a `signature[64]` placeholder that
*          nothing read.  This module finally fills it in: the packer computes an
*          HMAC-SHA256 over the image and stores it in the first 32 bytes of that
*          field; the kernel recomputes it while installing and refuses an image
*          whose value does not match.
*
*          Scope and honest limits:
*            - It is a **symmetric** scheme with one pre-shared key
*              (SVCRT_IMAGE_SIGN_KEY).  It answers "was this image produced by
*              somebody who holds the key, and is it unchanged?".  It is NOT a
*              trust chain: whoever can read the key out of a device can sign a
*              new image.  A public-key scheme (device holds only a public key)
*              is the real fix and is not implemented here.
*            - It is checked at install time only.  Image already in Flash are
*              trusted on the next boot; the point of the check is to keep a bad
*              image out, not to re-audit the pool.
*            - The signature covers the same bytes the CRC does, plus the
*              signature field itself zeroed: header fields that change after
*              install (crc32, state, runtime_ram_base) are excluded, so an
*              installed image keeps a valid signature after being moved.
*
* @author xw
*/
#ifndef __SVCRT_SIGN_H__
#define __SVCRT_SIGN_H__

#include "svcrt_types.h"
#include "svcrt_app_image.h"

/* Fallback only: svcrt_features.h is the authoritative place this switch is
 * declared.  This keeps the header usable on its own (e.g. from the packer's
 * generated checks) without a second definition when features.h did the job. */
#ifndef SVCRT_USE_IMAGE_SIGN
#define SVCRT_USE_IMAGE_SIGN      0
#endif

/** @brief Signature length in bytes (HMAC-SHA256 output).
 *  @note Stored in signature[0..32); signature[32..64) stays zero. */
#define SVCRT_SIGN_LEN            (32u)

/** @brief Pre-shared key length in bytes. */
#define SVCRT_SIGN_KEY_LEN        (32u)

#if (SVCRT_USE_IMAGE_SIGN == 1)

/**
* @brief Compute an image's signature into out[SVCRT_SIGN_LEN].
* @param image  start of the image (header + reloc table + payload)
* @param p_hdr  its parsed header
* @param out    receives SVCRT_SIGN_LEN bytes
*/
void svcrt_sign_image(const uint8 *image, const svcrt_app_header_t *p_hdr,
                      uint8 out[SVCRT_SIGN_LEN]);

/**
* @brief Check the signature stored in p_hdr->signature against the image.
* @return 0 = matches, -1 = does not (comparison is constant time).
*/
int32 svcrt_sign_verify(const uint8 *image, const svcrt_app_header_t *p_hdr);

/** Streaming MAC, for the install path that never holds the whole nominal
 *  image in RAM.  Feed the header (as written to Flash), then the nominal
 *  reloc table and payload bytes exactly as they arrive over the wire, then
 *  compare against p_hdr->signature.  Same coverage as svcrt_sign_image();
 *  one such transfer is in flight at a time, so the state is module-static. */
void  svcrt_sign_stream_begin(const uint8 *image);
void  svcrt_sign_stream_update(const uint8 *data, uint32 len);
int32 svcrt_sign_stream_end(const svcrt_app_header_t *p_hdr);

#endif /* SVCRT_USE_IMAGE_SIGN */

#endif /* __SVCRT_SIGN_H__ */
