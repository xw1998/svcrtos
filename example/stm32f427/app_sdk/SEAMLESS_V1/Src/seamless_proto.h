/**
* @file seamless_proto.h
* @brief Wire format shared by the two firmware versions of the seamless upgrade demo.
* @details This header exists twice, byte for byte identical, under SEAMLESS_V1
*          and SEAMLESS_V2. The duplication is deliberate: it is the interface
*          *between* the two versions, so neither side may change it on its
*          own. The rule is one line long - change both copies in the same
*          commit, and bump the version character inside the magic at the same
*          time, so a new version can never mistake a record left behind by an
*          older one for its own.
*
*          Both records are fixed size and carry a magic plus a CRC32. Writing
*          a file is not atomic, so a reader can catch the exact instant a
*          record is half written. The CRC is what lets the reader reject that
*          half record instead of acting on it.
*
*          Everything the control loop exchanges is in thousandths (1 unit ==
*          0.001), so the whole demo stays in integer maths: no libm and no
*          float formatting are pulled in.
*
* @note Part of the seamless upgrade example; not part of the kernel.
*/

#ifndef __SEAMLESS_PROTO_H__
#define __SEAMLESS_PROTO_H__

#include "svcrt_types.h"

/* Each version uses a subset of these helpers (the incumbent only decodes a
 * request, the incoming version only encodes one), and a warning per unused
 * helper would be noise rather than information. */
#if defined(__GNUC__) || defined(__clang__)
#define SXP_UNUSED __attribute__((unused))
#else
#define SXP_UNUSED
#endif

/* Paths, in the same form `mini run` takes (absolute inside the volume). */
#define SXP_STATE_PATH   "/sx_state.bin"
#define SXP_REQ_PATH     "/sx_req.bin"

/* Record identity. The trailing digit is the wire version of the record. */
#define SXP_STATE_MAGIC  (0x31535853u)   /* 'S','X','S','1' little endian */
#define SXP_REQ_MAGIC    (0x31525853u)   /* 'S','X','R','1' little endian */

#define SXP_STATE_SIZE   (64u)
#define SXP_REQ_SIZE     (32u)

/* owner: which version wrote the record. This *is* the version identity, so
 * the request record can name the incumbent without a version string. */
#define SXP_OWNER_NONE   (0u)
#define SXP_OWNER_V1     (1u)
#define SXP_OWNER_V2     (2u)

/* Numeric versions, packed as (major << 16) | (minor << 8) | patch. */
#define SXP_VER_V1       (0x010000u)     /* 1.0.0 */
#define SXP_VER_V2       (0x010100u)     /* 1.1.0 */

/* phase: where the handover stands. */
#define SXP_PHASE_ACTIVE   (1u)          /* the writer is in control */
#define SXP_PHASE_RELEASED (3u)          /* the writer has stopped controlling */

/* cmd in the request record. */
#define SXP_CMD_HANDOVER (1u)

/* Scale of every milli field. */
#define SXP_MILLI        (1000)

/**
* @brief The handover state record: written by whoever is in control.
* @note 64 bytes. The layout below is the on-disk layout; the struct has only
*       4-byte members so the two agree without packing attributes, and a
*       size check at the bottom of this file keeps it that way.
*/
typedef struct {
    uint32 magic;        /*  0: SXP_STATE_MAGIC */
    uint32 crc;          /*  4: CRC32 over bytes 8..63 */
    uint32 seq;          /*  8: ++ on every write */
    uint32 owner;        /* 12: SXP_OWNER_x */
    uint32 phase;        /* 16: SXP_PHASE_x */
    uint32 tick_ms;      /* 20: timestamp of the newest control iteration */
    uint32 iterations;   /* 24: control iterations completed so far */
    int32  setpoint;     /* 28: reference, milli */
    int32  pv;           /* 32: controlled variable, milli */
    int32  integ;        /* 36: controller integrator, milli */
    int32  out;          /* 40: controller output, milli */
    uint32 gap_max_ms;   /* 44: largest inter-iteration gap seen so far */
    uint32 overruns;     /* 48: iterations whose gap exceeded 2 x period */
    uint32 period_ms;    /* 52: control period of the writer */
    uint32 reserved0;    /* 56: 0 */
    uint32 reserved1;    /* 60: 0 */
} sxp_state_t;

/**
* @brief The handover request record: written by the incoming version.
* @note 32 bytes.
*/
typedef struct {
    uint32 magic;         /*  0: SXP_REQ_MAGIC */
    uint32 crc;           /*  4: CRC32 over bytes 8..31 */
    uint32 from_version;  /*  8: version the requester found in control */
    uint32 to_version;    /* 12: version the requester carries */
    uint32 cmd;           /* 16: SXP_CMD_HANDOVER */
    uint32 req_tick_ms;   /* 20: when the request was written */
    uint32 reserved0;     /* 24: 0 */
    uint32 reserved1;     /* 28: 0 */
} sxp_req_t;

/* The record size is part of the contract; a struct that grew a field would
 * silently change the file layout without changing any number in the code. */
typedef char sxp_state_size_must_be_64[(sizeof(sxp_state_t) == SXP_STATE_SIZE) ? 1 : -1];
typedef char sxp_req_size_must_be_32[(sizeof(sxp_req_t) == SXP_REQ_SIZE) ? 1 : -1];

/**
* @brief CRC32 (reflected, polynomial 0xEDB88320), computed bit by bit.
* @note No table: 1 KiB of table would be a third of this example's code block
*       to save cycles nobody is counting here.
*/
SXP_UNUSED static uint32 sxp_crc32(const uint8 *p, uint32 n)
{
    uint32 crc = 0xFFFFFFFFu;
    uint32 i;
    uint32 b;

    for(i = 0u; i < n; i++)
    {
        crc ^= (uint32)p[i];
        for(b = 0u; b < 8u; b++)
        {
            if((crc & 1u) != 0u)
            {
                crc = (crc >> 1) ^ 0xEDB88320u;
            }
            else
            {
                crc = (crc >> 1);
            }
        }
    }

    return ~crc;
}

SXP_UNUSED static void sxp_copy(uint8 *dst, const uint8 *src, uint32 n)
{
    while(n != 0u)
    {
        *dst++ = *src++;
        n--;
    }
}

SXP_UNUSED static void sxp_zero(uint8 *dst, uint32 n)
{
    while(n != 0u)
    {
        *dst++ = 0u;
        n--;
    }
}

/* Explicit little-endian byte store: the buffer is a plain byte array, and a
 * cast to uint32* would lean on an alignment the caller never promised. */
SXP_UNUSED static void sxp_store_u32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

/* The records are plain 4-byte fields on a little-endian core, so the in-RAM
 * struct and the file bytes have the same order; encode/decode is a byte copy
 * plus the CRC. Everything else (alignment, endianness) is fixed by the target
 * the example is built for. */

SXP_UNUSED static void sxp_state_encode(const sxp_state_t *st, uint8 *buf)
{
    sxp_copy(buf, (const uint8 *)st, SXP_STATE_SIZE);
    sxp_store_u32(buf + 4, 0u);                            /* crc field is not covered */
    sxp_store_u32(buf + 4, sxp_crc32(buf + 8, SXP_STATE_SIZE - 8u));
}

SXP_UNUSED static int sxp_state_decode(const uint8 *buf, uint32 n, sxp_state_t *st)
{
    sxp_state_t tmp;

    if(n != SXP_STATE_SIZE)
    {
        return 0;
    }
    sxp_copy((uint8 *)&tmp, buf, SXP_STATE_SIZE);
    if(tmp.magic != SXP_STATE_MAGIC)
    {
        return 0;
    }
    if(tmp.crc != sxp_crc32(buf + 8, SXP_STATE_SIZE - 8u))
    {
        return 0;
    }
    *st = tmp;
    return 1;
}

SXP_UNUSED static void sxp_req_encode(const sxp_req_t *rq, uint8 *buf)
{
    sxp_copy(buf, (const uint8 *)rq, SXP_REQ_SIZE);
    sxp_store_u32(buf + 4, 0u);                            /* crc field is not covered */
    sxp_store_u32(buf + 4, sxp_crc32(buf + 8, SXP_REQ_SIZE - 8u));
}

SXP_UNUSED static int sxp_req_decode(const uint8 *buf, uint32 n, sxp_req_t *rq)
{
    sxp_req_t tmp;

    if(n != SXP_REQ_SIZE)
    {
        return 0;
    }
    sxp_copy((uint8 *)&tmp, buf, SXP_REQ_SIZE);
    if(tmp.magic != SXP_REQ_MAGIC)
    {
        return 0;
    }
    if(tmp.crc != sxp_crc32(buf + 8, SXP_REQ_SIZE - 8u))
    {
        return 0;
    }
    *rq = tmp;
    return 1;
}

/**
* @brief Version number carried by an owner id.
* @return 0 when the owner is neither version (a record this demo cannot name).
*/
SXP_UNUSED static uint32 sxp_owner_version(uint32 owner)
{
    if(owner == SXP_OWNER_V1)
    {
        return SXP_VER_V1;
    }
    if(owner == SXP_OWNER_V2)
    {
        return SXP_VER_V2;
    }
    return 0u;
}

#endif /* __SEAMLESS_PROTO_H__ */
