# SEAMLESS_V1 —— 无缝升级例程的在位版本（1.0.0）

控制回路以**小程序**形态活在 RAM 里，周期 10 ms；稳态不写任何文件，每 100 ms 读一次
`/sx_req.bin`。新版本请求接管时，它把现场写进 `/sx_state.bin` 一次、打印一行、返回，
内核随后归还它的两块内存。

完整说明（协议布局、实测数字、判据与反例、边界）见
[`docs/无缝升级例程.md`](../../../../docs/无缝升级例程.md)。这里只留动手要用的部分。

## 打包

```bash
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/SEAMLESS_V1/MDK-ARM/seamless_v1.uvprojx \
        --type miniapp --no-autostart --out build/seamless_v1.svcapp
```

`--no-autostart` 不能省：小程序默认按「自启」打包，`pack_app.py` 对小程序会直接报错
（`--autostart 对小程序无效`）。小程序不走固定槽位、不占 `/app` 配置，所以也不需要
`--dev-slot`。

## 上板

```bash
py -3 tools/fs_put.py --port COM3 --path /seamless_v1.svcm build/seamless_v1.svcapp
# 设备侧
fs rm /sx_state.bin
fs rm /sx_req.bin
mini run /seamless_v1.svcm
```

状态行形如：

```
[v1] v=1.0.0 t=3396130 it=129 sp=120.000 pv=68.069 u=71.614 gap=17 over=0 pr=7
```

`gap` = 迄今最大迭代间隔（ms），`over` = 间隔超过 2×周期 的迭代数，
`pr` = 轮询那一次文件读的最长耗时（ms）。

## 和 V2 的关系

`Src/seamless_proto.h` 与 `SEAMLESS_V2` 下的那一份**逐字节相同**，它是两个版本之间的接口：
改必须同一提交改两份，并把记录 magic 的尾字符一起加一。
