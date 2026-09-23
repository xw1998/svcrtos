/**
* @file arpa/inet.h
* @brief Byte order and IPv4 text helpers for SVCrtOS Apps.
* @details All four conversions are userspace arithmetic - the kernel is not
 *          involved - so they behave exactly as the names promise and cost
 *          nothing but a few instructions.
* @note  inet_ntoa() returns a pointer to a per-call static buffer, as POSIX
 *        says it may. It is not reentrant and not thread safe: copy the
 *        result if you keep it.
* @author xw
*/
#ifndef __SVCRT_ARPA_INET_H__
#define __SVCRT_ARPA_INET_H__

#include "svcrt_posix_types.h"
#include "netinet/in.h"

uint16 svcrt_posix_htons(uint16 x);
uint16 svcrt_posix_ntohs(uint16 x);
uint32 svcrt_posix_htonl(uint32 x);
uint32 svcrt_posix_ntohl(uint32 x);

/** "127.0.0.1" -> network order address, or INADDR_NONE on a bad string. */
in_addr_t    svcrt_posix_inet_addr(const char *cp);
/** Network order address -> text, in a static buffer this call owns. */
char        *svcrt_posix_inet_ntoa(struct in_addr in);
/** af must be AF_INET; returns 1 on success, 0 on a bad address, -1 with
 *  errno=ENOSYS for a family this layer does not have. */
int          svcrt_posix_inet_pton(int af, const char *src, void *dst);
/** The reverse; dst_size must be at least 16 bytes for AF_INET. */
const char  *svcrt_posix_inet_ntop(int af, const void *src, char *dst,
                                   socklen_t dst_size);

#define htons(x)                    svcrt_posix_htons(x)
#define ntohs(x)                    svcrt_posix_ntohs(x)
#define htonl(x)                    svcrt_posix_htonl(x)
#define ntohl(x)                    svcrt_posix_ntohl(x)
#define inet_addr(cp)               svcrt_posix_inet_addr(cp)
#define inet_ntoa(in)               svcrt_posix_inet_ntoa(in)
#define inet_pton(af, src, dst)     svcrt_posix_inet_pton((af), (src), (dst))
#define inet_ntop(af, src, dst, n)  svcrt_posix_inet_ntop((af), (src), (dst), (n))

#endif /* __SVCRT_ARPA_INET_H__ */
