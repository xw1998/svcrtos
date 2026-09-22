/**
* @file posix_demo.c
* @brief An ordinary POSIX C program, built as an SVCrtOS App without edits.
* @details Everything here is what a small console program does on a desktop:
*          it includes <stdio.h>, <string.h>, <stdlib.h>, <ctype.h> for the
*          pure C part and <unistd.h>, <pthread.h>, <semaphore.h> for the rest.
*          There is no svcrt.h, no device name, no address and no partition -
*          the App side compatibility layer answers those names.
*
*          The same source builds on Linux with `cc posix_demo.c -pthread`
*          (where the headers come from glibc) and here with the Keil
*          toolchain (where they come from kernelsrc/sdk/posix). That is the
*          point of the file: it is a porting proof, not a target specific
*          driver demo.
*
*          What it does NOT use, on purpose:
*            - fopen/fread: there is no C library file layer here. Files are
*              reached through the VFS, not through stdio.
*            - scanf: this board has no stdin owner, so the compatibility
*              layer reports end of input instead of inventing one.
*
*          Built for the dev slot 3 window (16 KB RAM): 2 threads of 1 KB
*          stack plus the App's own data fit with room to spare.
* @author xw
* @date 2026.09.22
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#define TOPIC_MAX   4
#define WORKER_MAX  2

typedef struct
{
    char     name[12];
    unsigned score;
} topic_t;

static sem_t g_gate;

/**
* @brief qsort comparison: ascending score.
*/
static int by_score(const void *a, const void *b)
{
    const topic_t *ta = (const topic_t *)a;
    const topic_t *tb = (const topic_t *)b;

    if(ta->score < tb->score)
    {
        return -1;
    }
    if(ta->score > tb->score)
    {
        return 1;
    }
    return 0;
}

/** @brief In place upper casing, using ctype.h like any portable program. */
static void to_upper(char *s)
{
    while(*s != '\0')
    {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

/**
* @brief Thread body: wait on the gate, then tick a few times.
* @note  Nothing here knows it is running on an RTOS. The thread was created
*        by pthread_create, it waits on a POSIX semaphore, and sleep() is
*        unistd's.
*/
static void *worker(void *arg)
{
    const char *who = (const char *)arg;
    int round;

    sem_wait(&g_gate);
    printf("[%s] released, running\n", who);

    for(round = 1; round <= 3; round++)
    {
        printf("[%s] round %d\n", who, round);
        sleep(1);
    }
    printf("[%s] done\n", who);
    return (void *)0;
}

int main(void)
{
    topic_t   topic[TOPIC_MAX];
    pthread_t th[WORKER_MAX];
    char      line[64];
    char     *heap;
    unsigned  i;

    printf("\n");
    printf("POSIX demo: an unmodified POSIX C program as an SVCrtOS App\n");

    /* ---------------------------------------------------- string.h + ctype.h */
    strcpy(line, "hello svcrtos");
    to_upper(line);
    printf("string  : %s (%u chars)\n", line, (unsigned)strlen(line));
    if(strcmp(line, "HELLO SVCRTOS") == 0)
    {
        printf("strcmp  : as expected\n");
    }

    /* -------------------------------------------------------- stdlib.h: qsort */
    strcpy(topic[0].name, "kernel");
    topic[0].score = 30;
    strcpy(topic[1].name, "vfs");
    topic[1].score = 12;
    strcpy(topic[2].name, "loader");
    topic[2].score = 21;
    strcpy(topic[3].name, "app");
    topic[3].score = 5;

    qsort(topic, TOPIC_MAX, sizeof(topic[0]), by_score);
    for(i = 0; i < TOPIC_MAX; i++)
    {
        printf("sorted  : %-8s %u\n", topic[i].name, topic[i].score);
    }

    /* ------------------------------------------------------- stdlib.h: malloc.
     * A desktop program allocates without a second thought; whether that
     * works here depends on the App's scatter giving the C library a heap,
     * which is what this line reports on. */
    heap = (char *)malloc(24);
    if(heap != NULL)
    {
        strcpy(heap, "malloc works");
        printf("heap    : %s\n", heap);
        free(heap);
    }
    else
    {
        printf("heap    : malloc returned NULL\n");
    }

    /* ----------------------------------------------------------- unistd.h */
    printf("sleep   : 1 s\n");
    sleep(1);

    /* ------------------------------------------------- pthread.h + semaphore.h */
    if(sem_init(&g_gate, 0, 0) != 0)
    {
        printf("sem     : init FAILED\n");
    }
    if(pthread_create(&th[0], NULL, worker, (void *)"A") != 0)
    {
        printf("pthread : create A FAILED\n");
    }
    if(pthread_create(&th[1], NULL, worker, (void *)"B") != 0)
    {
        printf("pthread : create B FAILED\n");
    }

    printf("gate    : releasing both threads\n");
    sem_post(&g_gate);
    sem_post(&g_gate);

    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    sem_destroy(&g_gate);
    printf("threads : joined\n");

    printf("done    : entering heartbeat\n");
    for(;;)
    {
        printf("heartbeat\n");
        sleep(5);
    }
    return 0;
}
