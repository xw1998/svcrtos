/**
* @file netinet/in.h
* @brief IPv4 addresses and struct sockaddr_in for SVCrtOS Apps.
* @details The layout is Linux's, not BSD's: an App is written against
*          Windows or Linux headers, so a struct sockaddr_in has to look the
*          way its source already assumes - sin_family, sin_port, sin_addr,
*          sin_zero, and no sin_len.
*
*          Nothing on this struct crosses the SVC boundary. The App layer
*          reduces an address to two scalars (host order IPv4 address, host
*          order port) and the kernel builds its own sockaddr_in from those,
*          which is what leaves both sides free to choose a layout.
*
*          sin_addr.s_addr and sin_port are in NETWORK byte order, exactly as
*          on Linux, so inet_addr() / htons() / ntohl() mean here what they
*          mean everywhere else.
* @author xw
*/
#ifndef __SVCRT_NETINET_IN_H__
#define __SVCRT_NETINET_IN_H__

#include "svcrt_posix_types.h"
#include "sys/socket.h"

#ifndef SVCRT_POSIX_NO_STD_TYPES
typedef uint32 in_addr_t;
typedef uint16 in_port_t;
#endif

struct in_addr
{
    uint32 s_addr;                  /**< IPv4 address, network byte order */
};

struct sockaddr_in
{
    sa_family_t    sin_family;
    in_port_t      sin_port;        /**< port, network byte order         */
    struct in_addr sin_addr;
    char           sin_zero[8];     /**< padding, unused                  */
};

/* Well known IPv4 addresses. INADDR_LOOPBACK keeps glibc's value and, like
 * glibc's, that value is a HOST order constant - assign it as
 * htonl(INADDR_LOOPBACK), or use inet_addr("127.0.0.1"). INADDR_ANY is zero
 * either way, so it needs no such care. */
#define INADDR_ANY        ((in_addr_t)0x00000000u)
#define INADDR_LOOPBACK   ((in_addr_t)0x7f000001u)
#define INADDR_BROADCAST  ((in_addr_t)0xffffffffu)
#define INADDR_NONE       ((in_addr_t)0xffffffffu)

/* Protocol numbers, as in any other <netinet/in.h>. They are only used to
 * validate socket()'s protocol argument. */
#define IPPROTO_IP    0
#define IPPROTO_TCP   6
#define IPPROTO_UDP   17

#endif /* __SVCRT_NETINET_IN_H__ */
