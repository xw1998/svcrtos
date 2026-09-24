# SEAMLESS_V2 —— 无缝升级例程的接管版本（1.1.0）

比 1.0.0 快一倍（周期 5 ms、增益重调）。它启动后先读 `/sx_state.bin`：

- 记录说别人（owner 不同）在控制 → 写 `/sx_req.bin` 请求交权，每 50 ms 轮询状态等 `RELEASED`，
  上限 5000 ms；等到就接管（迭代数、受控量、积分器、gap 统计全部接着走），等不到就打印原因、
  **撤回自己的请求** 并退出，把控制权留给在位版本。
- 记录说自己（owner 相同）在控制 → 那是自己上一跑留下的，接着它继续（并说明这一点）。
- 没有记录 → 自己从零开始控制。

完整说明见 [`docs/无缝升级例程.md`](../../../../docs/无缝升级例程.md)。

## 打包与上板

```bash
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/SEAMLESS_V2/MDK-ARM/seamless_v2.uvprojx \
        --type miniapp --no-autostart --out build/seamless_v2.svcapp

py -3 tools/fs_put.py --port COM3 --path /seamless_v2.svcm build/seamless_v2.svcapp
# 设备侧（V1 正在跑的时候）
mini run /seamless_v2.svcm
```

新镜像写进卷**不需要**先停 V1：`fs put` 只写外部 NOR，不动片内 Flash。

状态行比 1.0.0 多一列 `last=`（刚过去那一轮的间隔）：

```
[v2] v=1.1.0 t=3727928 it=1139 sp=80.000 pv=77.036 u=83.595 gap=148 over=99 pr=9 last=5
```

`gap`/`over`/`pr` 与 1.0.0 同义，且是**从在位版本继承**的：接管不重置统计。
