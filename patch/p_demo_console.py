# -*- coding: utf-8 -*-
"""APP_DEMO 与 DRV_DEMO 示例接入用户日志（SVC 0x19）与用户命令（SVC 0x1A）。"""
import os

ROOT = r"D:\工作\git_project\svcrtos_new"
APP = os.path.join(ROOT, "example", "stm32f427", "app_sdk", "APP_DEMO", "Src", "app_demo.c")
DRV = os.path.join(ROOT, "example", "stm32f427", "driver_sdk", "DRV_DEMO", "Src", "temp_drv.c")

APP_CODE = '''
/* ---- console services: user log (SVC 0x19) and user command (SVC 0x1A) - */

static int app_cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Console output also goes through SVC 0x1A: the App never owns the UART. */
    (void)svcrt_shell_printf("APP_DEMO t=%d ms cpu=%d%% timer_hits=%d\\r\\n",
                             (int)svcrt_get_time_ms(),
                             (int)svcrt_get_cpu_usage(),
                             (int)g_timer_hits);

    return 0;
}

/* The descriptor itself must sit in the App's own RAM - the kernel reads it
 * through SVC and checks the pointer against the caller's windows. The name
 * and help strings may live in the App's read-only data as well. */
static svcrt_ushell_cmd_t g_app_cmd =
{
    "appstat",
    app_cmd_status,
    "APP_DEMO status, registered by the App via SVC 0x1A",
    1
};

'''

APP_TESTS = '''    /* ---------------------------------------------------------------
     * 8b) console services: user log and user command
     * --------------------------------------------------------------- */
    r = svcrt_log_print(SVCRT_LOG_INFO, "APPDEMO", "user log service online\\r\\n");
    report("log.print", (r == 0), r);

    r = svcrt_log_printf(SVCRT_LOG_INFO, "APPDEMO",
                         "log.printf smoke: dec=%d hex=0x%08X str=%s\\r\\n",
                         1234, 0xA5A5u, "ok");
    report("log.printf", (r == 0), r);

    r = svcrt_shell_cmd_register(&g_app_cmd);
    report("shell.register", (r == 0), r);

    r = svcrt_shell_cmd_register(&g_app_cmd);       /* duplicate name */
    report("shell.register_dup", (r < 0), r);

    r = svcrt_shell_cmd_unregister("appstat");
    report("shell.unregister", (r == 0), r);

    r = svcrt_shell_cmd_unregister("appstat");      /* already gone */
    report("shell.unregister_miss", (r < 0), r);

    r = svcrt_shell_cmd_register(&g_app_cmd);       /* keep it for the console */
    report("shell.re_register", (r == 0), r);

'''

DRV_CODE = '''
/* ---- console services: user log (SVC 0x19) and user command (SVC 0x1A) - */

static int drv_cmd_temp(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    (void)svcrt_shell_printf("TEMP raw=%d (0.1 deg), unit=%d\\r\\n",
                             (int)temp_obj.last_temp, (int)temp_obj.unit);
    return 0;
}

static svcrt_ushell_cmd_t g_drv_cmd =
{
    "temp",
    drv_cmd_temp,
    "Simulated temperature driver, registered by the driver via SVC 0x1A",
    1
};

'''

DRV_OLD_MAIN = '''void DrvMain(void)
{
    svcrt_drv_register("TEMP", &temp_drv, 0);

    while(1)
    {
        /* 注册完成后驱动无需后台动作：睡下去让出 CPU 给 App。 */
        svcrt_task_wait(1000);
    }
}'''

DRV_NEW_MAIN = '''void DrvMain(void)
{
    uint8 cmd_done = 0u;

    svcrt_drv_register("TEMP", &temp_drv, 0);

    (void)svcrt_log_printf(SVCRT_LOG_INFO, "TEMPDRV", "temperature driver registered\\r\\n");

    while(1)
    {
        svcrt_task_wait(1000);

        /* The console command table is built by the shell task once the
         * scheduler is running, and drivers are started before it, so the
         * first console registration waits one period to be sure. */
        if(cmd_done == 0u)
        {
            int32 gr = svcrt_shell_cmd_register(&g_drv_cmd);

            (void)svcrt_log_printf(SVCRT_LOG_INFO, "TEMPDRV",
                                   "console command 'temp' register rc=%d\\r\\n", (int)gr);
            cmd_done = 1u;
        }
    }
}'''


def patch(path, edits, enc):
    with open(path, "rb") as f:
        d = f.read()
    txt = d.decode(enc)
    eol = "\r\n" if "\r\n" in txt else "\n"
    for old, new in edits:
        o = old.replace("\n", eol)
        n = new.replace("\n", eol)
        if txt.count(o) != 1:
            raise SystemExit("[%s] anchor hits=%d" % (os.path.basename(path), txt.count(o)))
        txt = txt.replace(o, n, 1)
    with open(path, "wb") as f:
        f.write(txt.encode(enc, "strict"))
    print("[ok] %s" % os.path.basename(path))


patch(APP,
      [("\nvoid AppMain(void)", APP_CODE + "\nvoid AppMain(void)"),
       ("    /* ---------------------------------------------------------------\n"
        "     * 9) cleanup of the objects this App created",
        APP_TESTS +
        "    /* ---------------------------------------------------------------\n"
        "     * 9) cleanup of the objects this App created")],
      "utf-8")

patch(DRV,
      [("\nvoid DrvMain(void)", DRV_CODE + "\nvoid DrvMain(void)"),
       (DRV_OLD_MAIN, DRV_NEW_MAIN)],
      "gbk")
