# -*- coding: utf-8 -*-
"""
修复 12：环形缓冲区索引不是 volatile，且没有说明并发约束

svcrt_fifo_read/write 会被多个上下文调用（任务与 ISR），而 wt_idx/rd_idx 没有
volatile 修饰：编译器可以把它们缓存在寄存器里并在循环中复用，
在“ISR 写、任务读”这类用法下会读到过期值或把更新延后写回，
表现为数据丢失或 FIFO 提前判空/判满。

修复：索引加 volatile；并在头文件补上并发约束说明
（单生产者-单消费者模型；跨上下文共享时不能有两个写者或两个读者）。
"""
import os

p = os.path.join(r'D:\工作\git_project\svcrtos_new', 'kernelsrc', 'include', 'svcrt_fifo.h')
raw = open(p, 'rb').read()

OLD = """typedef struct {
    uint16 magic;
    uint16 wt_idx;
    uint16 rd_idx;
    uint16 size;
    uint8  data[1];
} svcrt_fifo_t;"""

NEW = """/* @note 并发约束：本 FIFO 采用单生产者-单消费者模型。
 *       wt_idx / rd_idx 会被 ISR 与任务两侧同时访问，因此必须是 volatile，
 *       否则编译器可能在循环中把它们缓存在寄存器里，
 *       导致读到过期值、或把更新延后写回（表现为丢数据/提前判空判满）。
 *       若确实需要多写者或多读者，必须在调用方自行加临界区。 */
typedef struct {
    uint16          magic;
    volatile uint16 wt_idx;     /* 写索引：由生产者更新，消费者读取 */
    volatile uint16 rd_idx;     /* 读索引：由消费者更新，生产者读取 */
    uint16          size;       /* 数据区字节数（不含头部 8 字节） */
    uint8           data[1];
} svcrt_fifo_t;"""

eol = '\r\n' if b'\r\n' in raw else '\n'
o = OLD.replace('\n', eol).encode('gbk')
n = NEW.replace('\n', eol).encode('gbk')
assert raw.count(o) == 1, raw.count(o)
open(p, 'wb').write(raw.replace(o, n))

# 补上创建接口的返回值说明
p2 = os.path.join(r'D:\工作\git_project\svcrtos_new', 'kernelsrc', 'src', 'svcrt_fifo.c')
raw2 = open(p2, 'rb').read()
OLD2 = """int32 svcrt_fifo_write(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{
    int32 cnt;
    uint16 next;
    if(fifo->magic != SVCRT_FIFO_MAGIC)"""
NEW2 = """/* @return 实际写入的字节数（FIFO 满时可能小于 len）。
 * @note   本函数不做临界区保护，按单生产者-单消费者模型使用：
 *         要么只在任务里调用，要么生产者固定为 ISR；混用时由调用方加锁。 */
int32 svcrt_fifo_write(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{
    int32 cnt;
    uint16 next;
    if(fifo->magic != SVCRT_FIFO_MAGIC)"""
o2 = OLD2.replace('\n', eol).encode('gbk')
n2 = NEW2.replace('\n', eol).encode('gbk')
assert raw2.count(o2) == 1, raw2.count(o2)
raw2 = raw2.replace(o2, n2)

OLD3 = """int32 svcrt_fifo_read(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{"""
NEW3 = """/* @return 实际读出的字节数（FIFO 空时可能小于 len）。
 * @note   同 svcrt_fifo_write：单消费者模型，不内建临界区。 */
int32 svcrt_fifo_read(svcrt_fifo_t *fifo, uint8 *pdata, int32 len)
{"""
o3 = OLD3.replace('\n', eol).encode('gbk')
n3 = NEW3.replace('\n', eol).encode('gbk')
assert raw2.count(o3) == 1, raw2.count(o3)
raw2 = raw2.replace(o3, n3)

open(p2, 'wb').write(raw2)
print('DONE')
