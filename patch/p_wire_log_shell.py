# -*- coding: utf-8 -*-
"""把内核日志（SVC 0x19）与控制台服务（SVC 0x1A）接到分发层与两个 SDK。

全部走字节级修改：GBK 文件只使用 ASCII 锚点与 ASCII 新增文本，
避免整文件转码导致 Keil 注释乱码；行尾按命中行自身的风格补齐。
每处锚点必须唯一命中，否则中止且不写盘。
"""
import io
import os
import re
import sys

ROOT = r"D:\工作\git_project\svcrtos_new"


def rd(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return f.read()


def wr(rel, data):
    with open(os.path.join(ROOT, rel), "wb") as f:
        f.write(data)
    print("[ok] %s (%d bytes)" % (rel, len(data)))


def eol_at(data, pos):
    return b"\r\n" if data[pos:pos + 2] == b"\r\n" else b"\n"


def line_eol(data, m):
    nl = data.find(b"\n", m.end())
    if nl < 0:
        return b"\n"
    return b"\r\n" if data[nl - 1:nl] == b"\r" else b"\n"


def insert_after_regex(data, pattern, text, unique=True, enc="ascii"):
    """在首个正则命中行的行尾之后插入 text（text 用 \\n 书写）。"""
    pattern = pattern.replace(b"$", b"\r?$")
    m = re.search(pattern, data, re.M)
    if m is None:
        raise SystemExit("anchor not found: %r" % pattern)
    if unique and len(re.findall(pattern, data, re.M)) > 1:
        raise SystemExit("anchor not unique: %r" % pattern)
    eol = line_eol(data, m)
    pos = m.end() + len(eol)
    ins = text.replace("\n", eol.decode("ascii")).encode(enc) + eol
    return data[:pos] + ins + data[pos:]


def insert_before_regex(data, pattern, text, enc="utf-8"):
    pattern = pattern.replace(b"$", b"\r?$")
    m = re.search(pattern, data, re.M)
    if m is None:
        raise SystemExit("anchor not found: %r" % pattern)
    if len(re.findall(pattern, data, re.M)) > 1:
        raise SystemExit("anchor not unique: %r" % pattern)
    eol = line_eol(data, m)
    ins = text.replace("\n", eol.decode("ascii")).encode(enc) + eol
    return data[:m.start()] + ins + data[m.start():]


def replace_once(data, old, new, rel):
    if data.count(old) != 1:
        raise SystemExit("[%s] %r hits %d" % (rel, old, data.count(old)))
    return data.replace(old, new, 1)


# ============================================================
# 1) kernelsrc/include/svcrt_shell.h
# ============================================================
SHELL_H_DECL = u"""
/**
* @brief 往内核控制台注册一条用户态（App / 驱动）命令
* @param cmd 用户态描述符；name / help 会被复制进内核 RAM，
*            所以镜像卸载后不会留下指向已擦除 Flash 的悬垂指针
* @return SVCRT_USHELL_OK 或负错误码（见 svcrt_ushell.h）
* @details 供内核分发层（SVC 0x1A 子命令 1）调用；用户态请用
*          svcrt_shell_cmd_register()，不要直接调用本函数。
*/
int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd);

/**
* @brief 注销一条用户态命令（按名字，不区分大小写）
* @return SVCRT_USHELL_OK 或负错误码
*/
int32 svcrt_shell_ext_unregister(const char *name);

/**
* @brief 向控制台输出一段有界长度的文本（不追加换行）
* @param msg 文本首地址
* @param len 输出长度；由分发层先探测上限，避免在内核侧做无界 strlen
* @return SVCRT_USHELL_OK 或负错误码
* @details SVC 处理运行在用户任务的 PSP 上（App 栈 4K、驱动栈 1K），
*          因此这里用内核静态缓冲分段输出，不在调用者栈上开临时数组。
*/
int32 svcrt_shell_print_n(const char *msg, uint32 len);
"""

SHELL_H_STUB = u"""
int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd)
{
    (void)cmd;
    return SVCRT_USHELL_ERR_OFF;
}

int32 svcrt_shell_ext_unregister(const char *name)
{
    (void)name;
    return SVCRT_USHELL_ERR_OFF;
}

int32 svcrt_shell_print_n(const char *msg, uint32 len)
{
    (void)msg;
    (void)len;
    return SVCRT_USHELL_ERR_OFF;
}
"""


def patch_shell_h():
    rel = "kernelsrc/include/svcrt_shell.h"
    d = rd(rel)
    if b"svcrt_ulog.h" not in d and b"svcrt_ushell.h" not in d:
        d = replace_once(d, b'#include "svcrt_types.h"',
                         b'#include "svcrt_types.h"\n#include "svcrt_ushell.h"', rel)
    d = insert_after_regex(d, rb"^int32 svcrt_shell_uart_open\(void\);$",
                           SHELL_H_DECL, enc="utf-8")
    wr(rel, d)


# ============================================================
# 2) kernelsrc/shell/svcrt_shell.c
# ============================================================
SHELL_H_EXT = u'''
/* ============================================================
 * 用户命令（SVC 0x1A）：注册表 + 内核跳板
 *
 * 内核不改 ark_shell（上游原样引入），而是在命令表里追加一条指向本文件的
 * 跳板命令：
 *   - 命令名与帮助文本注册时复制进内核 RAM，镜像卸载后不留下悬垂指针；
 *   - 跳板执行前先确认处理器仍落在某个「已装载」镜像的区间内，
 *     镜像被卸载后命令自动失效并给出提示，而不是跳到已擦除的 Flash 上。
 * ============================================================ */

#define SVCRT_USHELL_MAX_CMDS   (8)

/* 控制台分段输出缓冲：SVC 处理跑在用户任务的 PSP 上，栈很浅 */
#define SVCRT_USHELL_OUT_CHUNK  (64)

typedef struct
{
    char     name[SVCRT_USHELL_NAME_MAX + 1];
    char     help[SVCRT_USHELL_HELP_MAX + 1];
    int    (*func)(int argc, char **argv);
    uint32   max_args;
    uint32   in_use;
} svcrt_ushell_entry_t;

static svcrt_ushell_entry_t g_ushell[SVCRT_USHELL_MAX_CMDS];
static char g_ushell_out[SVCRT_USHELL_OUT_CHUNK + 1];

/* 命令名比较：不区分大小写，且要求两边同时结束 */
static int ushell_name_eq(const char *a, const char *b)
{
    if((a == 0) || (b == 0))
    {
        return 0;
    }

    while((*a != '\\0') && (*b != '\\0'))
    {
        uint32 ca = (uint32)(uint8)*a;
        uint32 cb = (uint32)(uint8)*b;

        if((ca >= 'A') && (ca <= 'Z'))
        {
            ca += 32u;
        }
        if((cb >= 'A') && (cb <= 'Z'))
        {
            cb += 32u;
        }
        if(ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }

    return ((*a == '\\0') && (*b == '\\0')) ? 1 : 0;
}

static void ushell_copy(char *dst, uint32 cap, const char *src)
{
    uint32 i = 0u;

    if((dst == 0) || (cap == 0u))
    {
        return;
    }

    if(src != 0)
    {
        while((src[i] != '\\0') && ((i + 1u) < cap))
        {
            dst[i] = src[i];
            i++;
        }
    }

    dst[i] = '\\0';
}

/* 处理器是否还活着：必须仍落在某个已装载镜像的 Flash 区间内 */
static int ushell_func_alive(const void *p)
{
    const svcrt_partition_table_t *pt = svcrt_ptable_get();
    uint32 addr = (uint32)p;
    uint32 i;

    if((p == 0) || (pt == 0))
    {
        return 0;
    }

    for(i = 0u; (i < pt->slot_max) && (i < SVCRT_SLOT_ARRAY_MAX); i++)
    {
        if((pt->slot_type[i] == SVCRT_SLOT_FREE) || (pt->slot_base[i] == 0u))
        {
            continue;
        }
        if((pt->slot_state[i] != SVCRT_APP_SLOT_LOADED) &&
           (pt->slot_state[i] != SVCRT_APP_SLOT_RUNNING))
        {
            continue;
        }
        if((addr >= pt->slot_base[i]) &&
           (addr < (pt->slot_base[i] + pt->slot_size[i])))
        {
            return 1;
        }
    }

    return 0;
}

/* 所有用户命令共用同一个跳板：用 argv[0] 反查注册表项 */
static int svcrt_shell_ext_trampoline(int argc, char **argv)
{
    uint32 i;

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (argv == 0) || (argc <= 0) || (argv[0] == 0))
        {
            continue;
        }

        if(ushell_name_eq(g_ushell[i].name, argv[0]) == 0)
        {
            continue;
        }

        if(ushell_func_alive((const void *)g_ushell[i].func) == 0)
        {
            ark_shell_printf("%s: handler no longer available (image unloaded?)\\r\\n",
                             g_ushell[i].name);
            return -1;
        }

        return g_ushell[i].func(argc, argv);
    }

    sh_out("user command not available\\r\\n");
    return -1;
}

int32 svcrt_shell_ext_register(const svcrt_ushell_cmd_t *cmd)
{
    uint32 i;
    uint32 len;

    if((cmd == 0) || (cmd->name == 0) || (cmd->func == 0))
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    len = (uint32)strlen(cmd->name);
    if((len == 0u) || (len > (uint32)SVCRT_USHELL_NAME_MAX))
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    if(g_cmd_count >= ARK_SHELL_MAX_COMMANDS)
    {
        return SVCRT_USHELL_ERR_FULL;
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use != 0u) && (ushell_name_eq(g_ushell[i].name, cmd->name) != 0))
        {
            return SVCRT_USHELL_ERR_DUP;
        }
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if(g_ushell[i].in_use == 0u)
        {
            break;
        }
    }
    if(i >= SVCRT_USHELL_MAX_CMDS)
    {
        return SVCRT_USHELL_ERR_FULL;
    }

    ushell_copy(g_ushell[i].name, (uint32)sizeof(g_ushell[i].name), cmd->name);
    ushell_copy(g_ushell[i].help, (uint32)sizeof(g_ushell[i].help), cmd->help);
    g_ushell[i].func = cmd->func;
    g_ushell[i].max_args = cmd->max_args;
    g_ushell[i].in_use = 1u;

    g_cmd_table[g_cmd_count].name = g_ushell[i].name;
    g_cmd_table[g_cmd_count].func = svcrt_shell_ext_trampoline;
    g_cmd_table[g_cmd_count].help = g_ushell[i].help;
    g_cmd_table[g_cmd_count].max_args = (int)cmd->max_args;
    g_cmd_count++;

    return SVCRT_USHELL_OK;
}

int32 svcrt_shell_ext_unregister(const char *name)
{
    uint32 i;
    uint32 k;

    if(name == 0)
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    for(i = 0u; i < SVCRT_USHELL_MAX_CMDS; i++)
    {
        if((g_ushell[i].in_use == 0u) || (ushell_name_eq(g_ushell[i].name, name) == 0))
        {
            continue;
        }

        /* 摘掉命令表里的跳板条目：表内顺序即 help 的显示顺序，
         * 所以用后项前移而不是留空洞 */
        for(k = 0u; (k < (uint32)g_cmd_count) && (k < (uint32)ARK_SHELL_MAX_COMMANDS); k++)
        {
            if((g_cmd_table[k].func == svcrt_shell_ext_trampoline) &&
               (g_cmd_table[k].name == g_ushell[i].name))
            {
                uint32 j;

                for(j = k; (j + 1u) < (uint32)g_cmd_count; j++)
                {
                    g_cmd_table[j] = g_cmd_table[j + 1u];
                }
                g_cmd_count--;
                break;
            }
        }

        g_ushell[i].in_use = 0u;
        g_ushell[i].func = 0;
        g_ushell[i].name[0] = '\\0';
        g_ushell[i].help[0] = '\\0';
        return SVCRT_USHELL_OK;
    }

    return SVCRT_USHELL_ERR_NOTFND;
}

int32 svcrt_shell_print_n(const char *msg, uint32 len)
{
    uint32 off = 0u;

    if(msg == 0)
    {
        return SVCRT_USHELL_ERR_PARAM;
    }

    while(off < len)
    {
        uint32 n = len - off;
        uint32 i;

        if(n > (uint32)SVCRT_USHELL_OUT_CHUNK)
        {
            n = (uint32)SVCRT_USHELL_OUT_CHUNK;
        }

        for(i = 0u; i < n; i++)
        {
            g_ushell_out[i] = msg[off + i];
        }
        g_ushell_out[n] = '\\0';

        sh_out(g_ushell_out);
        off += n;
    }

    return SVCRT_USHELL_OK;
}

/* ============================================================
 * log：查看 / 调整运行期日志级别（内核与所有 App、驱动同时生效）
 * ============================================================ */
static int cmd_log(int argc, char *argv[])
{
    const char *names[5] = { "off", "error", "warning", "info", "debug" };

    if(argc >= 2)
    {
        uint32 lvl;

        if((parse_u32(argv[1], &lvl) != 0) || (lvl > (uint32)SVCRT_LOG_DEBUG))
        {
            sh_out("usage: log <0..4>   (0=off 1=error 2=warn 3=info 4=debug)\\r\\n");
            return -1;
        }

        svcrt_log_set_level(lvl);
    }

    ark_shell_printf("log level: %u (%s)\\r\\n",
                     svcrt_log_get_level(), names[svcrt_log_get_level() & 7u]);

    return 0;
}

/* ============================================================
 * pool：池内还能装下多少（与 SVC 0x12 子命令 5 同一数据源）
 * ============================================================ */
static int cmd_pool(int argc, char *argv[])
{
    uint32 largest = 0u;
    uint32 total = svcrt_loader_pool_free(&largest);

    (void)argc;
    (void)argv;

    ark_shell_printf("\\r\\npool free : %u B (%uK)\\r\\n", total, total / 1024u);
    ark_shell_printf("largest   : %u B (%uK)\\r\\n", largest, largest / 1024u);

    return 0;
}
'''


def patch_shell_c():
    rel = "kernelsrc/shell/svcrt_shell.c"
    d = rd(rel)

    if b"svcrt_log.h" not in d:
        d = insert_after_regex(d, rb'^#include "svcrt_ptable.h"',
                               '#include "svcrt_log.h"')

    d = insert_before_regex(d, rb"^static int register_kernel_commands\(void\)$",
                            SHELL_H_EXT)

    d = replace_once(d, b"    int need = 6;", b"    int need = 8;", rel)

    d = insert_before_regex(
        d, rb"^    g_cmd_table\[idx\]\.name = NULL;$",
        u'''    g_cmd_table[idx++] = ARK_SHELL_CMD("log", cmd_log,
        "Get or set runtime log level: log [0..4]", 2);
    g_cmd_table[idx++] = ARK_SHELL_CMD("pool", cmd_pool,
        "Free space left in the image pool", 1);
''')

    d = replace_once(d, b'"List kernel tasks", 1);', b'"List kernel tasks", 3);', rel)

    d = insert_after_regex(d, rb"^#else   /\* SHELL_ENABLE == 0 \*/$",
                           SHELL_H_STUB, enc="utf-8")
    return d


# ============================================================
# 3) kernelsrc/include/svcrt_def.h（GBK）
# ============================================================
SVC_DEFS = '''#define SVCRT_SVC_LOG               (0x19)
#define SVCRT_SVC_SHELL             (0x1A)'''


def patch_def_h():
    rel = "kernelsrc/include/svcrt_def.h"
    d = rd(rel)
    d = insert_after_regex(d, rb"^#define SVCRT_SVC_TIMER_CTRL.*$", SVC_DEFS)

    # 镜像头里的 load_addr 说法已过时：运行期落点由池分配决定
    d = d.replace(b"load_addr", b"load_addr")
    wr(rel, d)


# ============================================================
# 4) kernelsrc/src/svcrt_task.c（GBK）：SVC 分发
# ============================================================
TASK_INC = '''#include "svcrt_log.h"
#include "svcrt_shell.h"'''

TASK_CASES = '''    case SVCRT_SVC_LOG:
        /* User log: r0=level, r1=tag, r2=msg. Both strings must sit in the
         * caller's own RAM - kernel RAM and kernel flash are rejected. */
        {
            const char *tag = (const char *)SVCRT_SVC_ARG(p_svc_ctx, 1);
            const char *msg = (const char *)SVCRT_SVC_ARG(p_svc_ctx, 2);

            if((svcrt_kernel_ptr_ram_ok(tag, 1u) == 0u) ||
               (svcrt_kernel_ptr_ram_ok(msg, 1u) == 0u))
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }

            SVCRT_SVC_RET(p_svc_ctx,
                          (uint32)svcrt_log_svc(SVCRT_SVC_ARG(p_svc_ctx, 0), tag, msg));
        }
        break;

    case SVCRT_SVC_SHELL:
        /* User console service:
         *   p[0] = 1 register   (p[1] = svcrt_ushell_cmd_t * in caller RAM)
         *   p[0] = 2 unregister (p[1] = name string)
         *   p[0] = 3 print      (p[1] = text string)
         * The descriptor lives in the caller's RAM and the handler must stay
         * inside the caller's own firmware window - the same policy the timer
         * callbacks use, so nobody can borrow shell privilege to run someone
         * else's code. */
        {
            uint32 *q = (uint32 *)SVCRT_SVC_ARG(p_svc_ctx, 0);

            if(svcrt_kernel_svc_args_ok(q, 16u) == 0u)
            {
                SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                break;
            }

            switch(q[0])
            {
                case 1:
                    {
                        const svcrt_ushell_cmd_t *cmd = (const svcrt_ushell_cmd_t *)q[1];

                        if((svcrt_kernel_ptr_ram_ok(cmd, (uint32)sizeof(svcrt_ushell_cmd_t)) == 0u) ||
                           (svcrt_kernel_ptr_ram_ok(cmd->name, 1u) == 0u) ||
                           (svcrt_kernel_ptr_ram_ok(cmd->help, 1u) == 0u) ||
                           (svcrt_kernel_cb_own_region_ok((const void *)cmd->func) == 0u))
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_shell_ext_register(cmd));
                    }
                    break;

                case 2:
                    {
                        const char *name = (const char *)q[1];

                        if(svcrt_kernel_user_name_ok((const void *)name, 16u) == 0u)
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_shell_ext_unregister(name));
                    }
                    break;

                case 3:
                    {
                        const char *msg = (const char *)q[1];
                        uint32 len;

                        if(svcrt_kernel_ptr_ram_ok(msg, 1u) == 0u)
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        /* Bound the length here: the shell side copies into its
                         * own static buffer, so nothing is allocated on the
                         * caller's (shallow) task stack. */
                        for(len = 0u; len < 128u; len++)
                        {
                            if(msg[len] == '\\0')
                            {
                                break;
                            }
                        }
                        if(len >= 128u)
                        {
                            SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                            break;
                        }

                        SVCRT_SVC_RET(p_svc_ctx, (uint32)svcrt_shell_print_n(msg, len));
                    }
                    break;

                default:
                    SVCRT_SVC_RET(p_svc_ctx, (uint32)(-1));
                    break;
            }
        }
        break;

'''


def patch_task_c():
    rel = "kernelsrc/src/svcrt_task.c"
    d = rd(rel)

    if b"svcrt_shell.h" not in d:
        d = insert_after_regex(d, rb'^#include "[^"]+"', TASK_INC, unique=False)

    d = insert_before_regex(d, rb"^    case SVCRT_SVC_APP_MGR:$", TASK_CASES)
    wr(rel, d)


# ============================================================
# 5) 两个 SDK 的 oslib（app: GBK+CRLF / driver: GBK+LF）
# ============================================================
OSLIB_CODE = '''
/* ============================================================
 * Log and console services (SVC 0x19 / 0x1A)
 *
 * The formatting itself is done on this side (see svcrt_ulog.h), so the
 * kernel only has to filter by level and push the bytes out. One level
 * setting therefore applies to the kernel and to every App / driver.
 * ============================================================ */
SVCRT_SVC_DECL_3(int32, 0x19, svcrt_call_log, uint32, uint32, uint32);
SVCRT_SVC_DECL_1(int32, 0x1A, svcrt_call_shell_svc, uint32 *);

int32 svcrt_log_print(uint32 level, const char *tag, const char *msg)
{
    return svcrt_call_log(level, (uint32)tag, (uint32)msg);
}

int32 svcrt_shell_cmd_register(const svcrt_ushell_cmd_t *cmd)
{
    uint32 p[3];
    p[0] = 1; p[1] = (uint32)cmd; p[2] = 0;
    return svcrt_call_shell_svc(p);
}

int32 svcrt_shell_cmd_unregister(const char *name)
{
    uint32 p[3];
    p[0] = 2; p[1] = (uint32)name; p[2] = 0;
    return svcrt_call_shell_svc(p);
}

int32 svcrt_shell_print(const char *msg)
{
    uint32 p[3];
    p[0] = 3; p[1] = (uint32)msg; p[2] = 0;
    return svcrt_call_shell_svc(p);
}
'''


def patch_oslib(rel):
    d = rd(rel)
    eol = b"\r\n" if b"\r\n" in d else b"\n"

    if b"svcrt_ulog.h" not in d:
        d = insert_after_regex(d, rb'^#include "[^"]+"', '#include "svcrt_ulog.h"',
                               unique=False)

    if not d.endswith(eol):
        d += eol

    d += OSLIB_CODE.replace("\n", eol.decode("ascii")).encode("ascii")
    wr(rel, d)


# ============================================================
# 6) 头文件 include 链接
# ============================================================
def patch_include_svcrt_ulog(rel):
    d = rd(rel)
    if b"svcrt_ulog.h" in d:
        print("[skip] %s already includes svcrt_ulog.h" % rel)
        return
    d = insert_after_regex(d, rb'^#include "[^"]+"', '#include "svcrt_ulog.h"',
                           unique=False)
    wr(rel, d)


if __name__ == "__main__":
    patch_shell_h()
    wr("kernelsrc/shell/svcrt_shell.c", patch_shell_c())
    patch_def_h()
    patch_task_c()
    patch_oslib("kernelsrc/sdk/app_sdk/svcrt_oslib.c")
    patch_oslib("kernelsrc/sdk/driver_sdk/svcrt_drv_oslib.c")
    patch_include_svcrt_ulog("kernelsrc/include/svcrt.h")
    patch_include_svcrt_ulog("kernelsrc/sdk/driver_sdk/svcrt_driver_sdk.h")
    print("ALL DONE")
