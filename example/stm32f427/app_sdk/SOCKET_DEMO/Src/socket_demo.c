/**
* @file socket_demo.c
* @brief A POSIX sockets program, built as an SVCrtOS App without edits.
* @details This is what a small TCP/UDP program looks like on a desktop: it
*          includes <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>, <sys/time.h>
*          and calls socket/bind/listen/accept/connect/send/recv/select/
*          getsockname/getpeername/shutdown/close. There is no svcrt.h here, no
*          SVC number, no ABI struct - the App side compatibility layer
*          (kernelsrc/sdk/posix) answers those names and forwards them over
*          SVC 0x1E.
*
*          The same source builds on Linux with `cc socket_demo.c` (headers
*          from glibc) and here with the Keil toolchain (headers from
*          kernelsrc/sdk/posix). That is the point of the file.
*
*          Where the packets go: this board has no Ethernet PHY, so the kernel
*          brings up lwIP's loopback interface (127.0.0.1/8). Connections,
*          data transfer, half close and select timeouts all run through the
*          real TCP state machine; only the wire is missing.
*
*          Self-check harness: each line prints ok/FAIL and a final pass/fail
*          count, so a console capture is the evidence.
* @author xw
* @date 2026.09.23
*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ECHO_PORT   34567
#define UDP_PORT    34568

static int g_pass;
static int g_fail;

static void chk(const char *what, int ok)
{
    if(ok)
    {
        g_pass++;
        printf("  ok   %s\n", what);
    }
    else
    {
        g_fail++;
        printf("  FAIL %s (errno=%d)\n", what, errno);
    }
}

/** @brief Give a socket a 3 s ceiling so a bug shows up as FAIL, not a hang. */
static void set_timeout(int fd)
{
    struct timeval tv;

    tv.tv_sec  = 3;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void fill_local(struct sockaddr_in *a, unsigned port)
{
    memset(a, 0, sizeof(*a));
    a->sin_family      = AF_INET;
    a->sin_port        = htons((unsigned short)port);
    a->sin_addr.s_addr = inet_addr("127.0.0.1");
}

static void test_names(void)
{
    struct in_addr in;
    char *s;

    printf("names\n");
    chk("htons/ntohs round trip", ntohs(htons(0x1234)) == 0x1234);
    in.s_addr = inet_addr("127.0.0.1");
    chk("inet_addr(127.0.0.1)", in.s_addr != INADDR_NONE);
    s = inet_ntoa(in);
    chk("inet_ntoa -> 127.0.0.1", s != NULL && strcmp(s, "127.0.0.1") == 0);
}

/** @brief TCP half close: client sends, server echoes, client shuts down. */
static void test_tcp_echo(void)
{
    int srv, cli, conn;
    struct sockaddr_in a;
    socklen_t alen;
    struct timeval tv;
    fd_set rset;
    char buf[16];
    int one = 1;
    int n;

    printf("tcp echo\n");

    srv = socket(AF_INET, SOCK_STREAM, 0);
    chk("socket(AF_INET,SOCK_STREAM)", srv >= 0);
    if(srv < 0)
    {
        return;
    }
    chk("setsockopt(SO_REUSEADDR)",
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    set_timeout(srv);

    fill_local(&a, ECHO_PORT);
    chk("bind(127.0.0.1)", bind(srv, (struct sockaddr *)&a, sizeof(a)) == 0);
    chk("listen(backlog 1)", listen(srv, 1) == 0);

    alen = sizeof(a);
    fill_local(&a, 0);
    chk("getsockname", getsockname(srv, (struct sockaddr *)&a, &alen) == 0);
    chk("getsockname port", ntohs(a.sin_port) == ECHO_PORT);

    cli = socket(AF_INET, SOCK_STREAM, 0);
    chk("socket(client)", cli >= 0);
    if(cli < 0)
    {
        close(srv);
        return;
    }
    set_timeout(cli);

    fill_local(&a, ECHO_PORT);
    chk("connect(127.0.0.1)", connect(cli, (struct sockaddr *)&a, sizeof(a)) == 0);

    conn = accept(srv, NULL, NULL);
    chk("accept", conn >= 0);
    if(conn < 0)
    {
        close(cli);
        close(srv);
        return;
    }
    set_timeout(conn);

    alen = sizeof(a);
    chk("getpeername(family)", getpeername(conn, (struct sockaddr *)&a, &alen) == 0 &&
        a.sin_family == AF_INET);

    n = (int)send(cli, "PING", 4, 0);
    chk("send 4 bytes", n == 4);

    FD_ZERO(&rset);
    FD_SET(conn, &rset);
    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    n = select(conn + 1, &rset, NULL, NULL, &tv);
    chk("select(readable) == 1", n == 1);
    chk("FD_ISSET(conn)", FD_ISSET(conn, &rset));

    n = (int)recv(conn, buf, sizeof(buf), 0);
    chk("recv 4 bytes", n == 4);
    chk("payload == PING", n == 4 && memcmp(buf, "PING", 4) == 0);

    chk("send echo", send(conn, "PONG", 4, 0) == 4);
    n = (int)recv(cli, buf, sizeof(buf), 0);
    chk("client recv echo", n == 4 && memcmp(buf, "PONG", 4) == 0);

    /* Nothing pending now: select must time out, not block forever. */
    FD_ZERO(&rset);
    FD_SET(cli, &rset);
    tv.tv_sec  = 0;
    tv.tv_usec = 300000;
    n = select(cli + 1, &rset, NULL, NULL, &tv);
    chk("select timeout == 0", n == 0);

    chk("shutdown(SHUT_WR)", shutdown(cli, SHUT_WR) == 0);
    n = (int)recv(conn, buf, sizeof(buf), 0);
    chk("peer sees EOF (recv == 0)", n == 0);

    chk("getsockopt(SO_ERROR) == 0",
        getsockopt(conn, SOL_SOCKET, SO_ERROR, &one, &alen) == 0 && one == 0);

    chk("close(client)", close(cli) == 0);
    chk("close(accepted)", close(conn) == 0);
    chk("close(listener)", close(srv) == 0);
}

static void test_udp(void)
{
    int us, uc;
    struct sockaddr_in a, from;
    socklen_t alen;
    char buf[8];
    int n;

    printf("udp\n");

    us = socket(AF_INET, SOCK_DGRAM, 0);
    chk("socket(SOCK_DGRAM)", us >= 0);
    if(us < 0)
    {
        return;
    }
    uc = socket(AF_INET, SOCK_DGRAM, 0);
    chk("socket(sender)", uc >= 0);
    if(uc < 0)
    {
        close(us);
        return;
    }
    set_timeout(us);
    set_timeout(uc);

    fill_local(&a, UDP_PORT);
    chk("bind(dgram)", bind(us, (struct sockaddr *)&a, sizeof(a)) == 0);

    fill_local(&a, UDP_PORT);
    chk("sendto 1 byte", sendto(uc, "U", 1, 0, (struct sockaddr *)&a, sizeof(a)) == 1);

    memset(&from, 0, sizeof(from));
    alen = sizeof(from);
    n = (int)recvfrom(us, buf, sizeof(buf), 0, (struct sockaddr *)&from, &alen);
    chk("recvfrom 1 byte", n == 1 && buf[0] == 'U');
    chk("recvfrom source port set", ntohs(from.sin_port) != 0);

    chk("close(dgram recv)", close(us) == 0);
    chk("close(dgram send)", close(uc) == 0);
}

static void test_scoreboard(void)
{
    int bad;

    printf("refusals\n");
    bad = socket(10 /* AF_INET6 */, SOCK_STREAM, 0);
    chk("socket(AF_INET6) refused", bad < 0);
    if(bad >= 0)
    {
        close(bad);
    }
}

int main(void)
{
    printf("\n");
    printf("socket demo: a POSIX sockets program as an SVCrtOS App\n");

    test_names();
    test_tcp_echo();
    test_udp();
    test_scoreboard();

    printf("result  : pass=%d fail=%d\n", g_pass, g_fail);

    printf("done    : entering heartbeat\n");
    for(;;)
    {
        printf("heartbeat  pass=%d fail=%d\n", g_pass, g_fail);
        sleep(5);
    }
}
