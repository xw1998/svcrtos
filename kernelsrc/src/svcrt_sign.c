/**
* @file svcrt_sign.c
* @brief App image signature: SHA-256 + HMAC + the image MAC (kernel side).
* @details See svcrt_sign.h for what this covers and what it does not.  The
*          hash is a straight FIPS 180-4 SHA-256; HMAC is RFC 2104.  Both are
*          compiled only when SVCRT_USE_IMAGE_SIGN is on, so a build that does
*          not want signature checking carries none of this code.
* @author xw
*/
#include "svcrt_sign.h"
#include "svcrt_config.h"    /* feature gate: SVCRT_USE_IMAGE_SIGN + key */

#if (SVCRT_USE_IMAGE_SIGN == 1)

/* ---------------------------------------------------------------- SHA-256 */

typedef struct
{
    uint32 state[8];
    unsigned long long bitlen;
    uint8  buf[64];
    uint32 buflen;
} svcrt_sha256_t;

#define SVCRT_ROR(x, n)  (((x) >> (n)) | ((x) << (32u - (n))))
#define SVCRT_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define SVCRT_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SVCRT_EP0(x)     (SVCRT_ROR(x,2u)  ^ SVCRT_ROR(x,13u) ^ SVCRT_ROR(x,22u))
#define SVCRT_EP1(x)     (SVCRT_ROR(x,6u)  ^ SVCRT_ROR(x,11u) ^ SVCRT_ROR(x,25u))
#define SVCRT_SG0(x)     (SVCRT_ROR(x,7u)  ^ SVCRT_ROR(x,18u) ^ ((x) >> 3u))
#define SVCRT_SG1(x)     (SVCRT_ROR(x,17u) ^ SVCRT_ROR(x,19u) ^ ((x) >> 10u))

static const uint32 svcrt_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void svcrt_sha256_block(svcrt_sha256_t *c, const uint8 *p)
{
    uint32 w[64];
    uint32 a;
    uint32 b;
    uint32 cc;
    uint32 d;
    uint32 e;
    uint32 f;
    uint32 g;
    uint32 h;
    uint32 i;

    for(i = 0u; i < 16u; i++)
    {
        w[i] = ((uint32)p[i * 4u] << 24) | ((uint32)p[(i * 4u) + 1u] << 16) |
               ((uint32)p[(i * 4u) + 2u] << 8) | (uint32)p[(i * 4u) + 3u];
    }
    for(i = 16u; i < 64u; i++)
    {
        w[i] = SVCRT_SG1(w[i - 2u]) + w[i - 7u] + SVCRT_SG0(w[i - 15u]) + w[i - 16u];
    }

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g = c->state[6]; h = c->state[7];

    for(i = 0u; i < 64u; i++)
    {
        uint32 t1 = h + SVCRT_EP1(e) + SVCRT_CH(e, f, g) + svcrt_sha256_k[i] + w[i];
        uint32 t2 = SVCRT_EP0(a) + SVCRT_MAJ(a, b, cc);

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

static void svcrt_sha256_init(svcrt_sha256_t *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0ull;
    c->buflen = 0u;
}

static void svcrt_sha256_update(svcrt_sha256_t *c, const uint8 *data, uint32 len)
{
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        c->buf[c->buflen++] = data[i];
        if(c->buflen == 64u)
        {
            svcrt_sha256_block(c, c->buf);
            c->bitlen += 512ull;
            c->buflen = 0u;
        }
    }
}

static void svcrt_sha256_final(svcrt_sha256_t *c, uint8 out[32])
{
    uint32 i = c->buflen;

    /* Pad: 0x80, then zeros, then the 64 bit big endian bit count. */
    c->bitlen += (unsigned long long)c->buflen * 8ull;
    c->buf[i++] = 0x80u;
    if(i > 56u)
    {
        while(i < 64u) { c->buf[i++] = 0u; }
        svcrt_sha256_block(c, c->buf);
        i = 0u;
    }
    while(i < 56u) { c->buf[i++] = 0u; }
    for(i = 0u; i < 8u; i++)
    {
        c->buf[56u + i] = (uint8)(c->bitlen >> (56u - (i * 8u)));
    }
    svcrt_sha256_block(c, c->buf);

    for(i = 0u; i < 8u; i++)
    {
        out[i * 4u]      = (uint8)(c->state[i] >> 24);
        out[(i * 4u) + 1u] = (uint8)(c->state[i] >> 16);
        out[(i * 4u) + 2u] = (uint8)(c->state[i] >> 8);
        out[(i * 4u) + 3u] = (uint8)(c->state[i]);
    }
}

/* ------------------------------------------------------------------- HMAC */

typedef struct
{
    svcrt_sha256_t inner;
    uint8          opad[64];
} svcrt_hmac_t;

static void svcrt_hmac_init(svcrt_hmac_t *h, const uint8 *key, uint32 klen)
{
    uint8 kpad[64];
    uint8 i;

    for(i = 0u; i < 64u; i++) { kpad[i] = 0u; }
    for(i = 0u; (i < klen) && (i < 64u); i++) { kpad[i] = key[i]; }

    svcrt_sha256_init(&h->inner);
    for(i = 0u; i < 64u; i++)
    {
        h->opad[i] = (uint8)(kpad[i] ^ 0x5cu);
        kpad[i]   = (uint8)(kpad[i] ^ 0x36u);
    }
    svcrt_sha256_update(&h->inner, kpad, 64u);
}

static void svcrt_hmac_update(svcrt_hmac_t *h, const uint8 *data, uint32 len)
{
    svcrt_sha256_update(&h->inner, data, len);
}

static void svcrt_hmac_final(svcrt_hmac_t *h, uint8 out[32])
{
    uint8 inner[32];
    svcrt_sha256_t outer;

    svcrt_sha256_final(&h->inner, inner);
    svcrt_sha256_init(&outer);
    svcrt_sha256_update(&outer, h->opad, 64u);
    svcrt_sha256_update(&outer, inner, 32u);
    svcrt_sha256_final(&outer, out);
}

/* -------------------------------------------------------------- image MAC */

/* The key is a compile time list of 32 bytes; see features.h / the board
 * config.  A build with the switch on but no key is refused at compile time
 * there, so reaching this initialiser means a key exists. */
static const uint8 svcrt_sign_key[SVCRT_SIGN_KEY_LEN] = SVCRT_IMAGE_SIGN_KEY;

void svcrt_sign_image(const uint8 *image, const svcrt_app_header_t *p_hdr,
                      uint8 out[SVCRT_SIGN_LEN])
{
    static const uint8 zero4[4]  = { 0u, 0u, 0u, 0u };
    static const uint8 zero64[64] = { 0u };
    svcrt_hmac_t h;
    uint32 rt_len = p_hdr->reloc_count * SVCRT_APP_RELOC_SIZE;

    svcrt_hmac_init(&h, svcrt_sign_key, SVCRT_SIGN_KEY_LEN);

    /* header, with the four fields that are either self-referential or set
     * after packing replaced by zeros: crc32 (28), signature (32..96),
     * state (100) and runtime_ram_base (252).  Same rule the CRC uses, plus
     * the signature field itself - a signature cannot cover its own value. */
    svcrt_hmac_update(&h, image, SVCRT_APP_OFF_CRC32);
    svcrt_hmac_update(&h, zero4, 4u);
    svcrt_hmac_update(&h, zero64, 64u);
    svcrt_hmac_update(&h, image + 96u, 4u);            /* flags */
    svcrt_hmac_update(&h, zero4, 4u);
    svcrt_hmac_update(&h, image + 104u, 148u);         /* 104..252 */
    svcrt_hmac_update(&h, zero4, 4u);
    /* reloc table + payload */
    svcrt_hmac_update(&h, image + SVCRT_APP_HEADER_SIZE, rt_len + p_hdr->image_size);

    svcrt_hmac_final(&h, out);
}

/* Streaming variant: the streaming installer holds no nominal copy of the
 * image (it patches each chunk as it lands), so the MAC has to be built up
 * from the bytes as they arrive.  Header handling is identical to
 * svcrt_sign_image(); only reloc table + payload come in later. */
static svcrt_hmac_t svcrt_sign_stream_ctx;

void svcrt_sign_stream_begin(const uint8 *image)
{
    static const uint8 zero4[4]  = { 0u, 0u, 0u, 0u };
    static const uint8 zero64[64] = { 0u };

    svcrt_hmac_init(&svcrt_sign_stream_ctx, svcrt_sign_key, SVCRT_SIGN_KEY_LEN);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, image, SVCRT_APP_OFF_CRC32);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, zero4, 4u);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, zero64, 64u);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, image + 96u, 4u);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, zero4, 4u);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, image + 104u, 148u);
    svcrt_hmac_update(&svcrt_sign_stream_ctx, zero4, 4u);
}

void svcrt_sign_stream_update(const uint8 *data, uint32 len)
{
    svcrt_hmac_update(&svcrt_sign_stream_ctx, data, len);
}

int32 svcrt_sign_stream_end(const svcrt_app_header_t *p_hdr)
{
    uint8  mac[SVCRT_SIGN_LEN];
    uint32 i;
    uint8  diff = 0u;

    svcrt_hmac_final(&svcrt_sign_stream_ctx, mac);
    for(i = 0u; i < SVCRT_SIGN_LEN; i++)
    {
        diff |= (uint8)(mac[i] ^ p_hdr->signature[i]);
    }
    return (diff == 0u) ? 0 : -1;
}

int32 svcrt_sign_verify(const uint8 *image, const svcrt_app_header_t *p_hdr)
{
    uint8  mac[SVCRT_SIGN_LEN];
    uint32 i;
    uint8  diff = 0u;

    svcrt_sign_image(image, p_hdr, mac);
    for(i = 0u; i < SVCRT_SIGN_LEN; i++)
    {
        diff |= (uint8)(mac[i] ^ p_hdr->signature[i]);
    }
    return (diff == 0u) ? 0 : -1;
}

#endif /* SVCRT_USE_IMAGE_SIGN */
