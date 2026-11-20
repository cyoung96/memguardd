# memguardd

Linux 膨胀进程守卫(单文件 C,零依赖)。设计动机:多代理并行构建/测试的机器上,单个失控进程(测试无界增长、撕裂二进制、分配器泄漏)能把整盒内存吃穿并连带 OOM 杀掉无辜进程;本守护进程以**单 PID 定点击杀**兜底,绝不级联。

## 行为

- `sleep(sweep)` 定时扫 `/proc/*/statm`(每扫一次全量读,~1000 进程 ≈ 10ms)。
- 每扫读一次 `/proc/pressure/memory` 的 `some avg10` 作压力预检:压力 >10% 时击杀基线收紧到 base/4,闲期用全量 base。
  - 注:Linux 6.8 实测 PSI fd 的 `POLLIN` 恒就绪(注册触发器后仍电平触发),不能当事件阻塞原语;触发器重武装在窗口内返回 `EBUSY`。故弃事件驱动,PSI 只作压力信号。

## 击杀规则(每扫评估)

1. RSS ≥ `hard` → `kill(pid, SIGKILL)`
2. RSS ≥ `base` 且连续 `streak` 个间隔增长 ≥ `growth` → kill
3. `MemAvailable` < `emerg` → kill 最大单个非保护进程(5s 冷却)

保护集:自身、pid 1、祖先链、`-p` 追加。只用 `kill(pid)`,从不用 `killpg`/进程组。

## 构建/运行

```sh
cc -O2 -Wall -o memguardd memguardd.c
./memguardd -i 60 -b 8192 -g 1500 -s 2 -H 22528 -e 1024 -l /var/log/memguardd.log
```

| 参数 | 含义 | 上面示例值 |
|---|---|---|
| `-i` | 扫描间隔(秒) | 60 |
| `-b` | 击杀基线 RSS(MiB);PSI 压力期收紧到 1/4 | 8192 |
| `-g` | 每间隔增长阈值(MiB) | 1500 |
| `-s` | 连续增长次数(streak) | 2 |
| `-H` | 硬顶 RSS(MiB),触线即杀 | 22528 |
| `-e` | MemAvailable 紧急线(MiB) | 1024 |
| `-p` | 追加保护 pid(可多次) | — |
| `-l` | 日志文件 | — |

## 实测

- 常驻足迹:RSS ~2MB,CPU ≈ 0(每分钟一扫,一扫 ~10ms)。
- 4373MiB 持续膨胀体被定点击杀,其父 shell 存活(零级联)。
- 12GiB 稳态大进程不误杀(大而稳 ≠ 膨胀)。
- 55 分钟连续运行 55 次扫描,无误报。

## 已知交互

- 紧急规则杀「当前 RSS 最大的非保护进程」——并行编译风暴里那就是你的 rustc(实测:32GiB 机器上 all-targets 测试编译+两路并行构建把 MemAvailable 压到 ~700MiB,守卫连杀两个 2.4GiB 的 rustc 保盒)。这是设计行为:盒子存活优先于任何单个作业;要避免,错峰编译或调高 `-e`。
- cgroup memcg OOM(user slice)杀进程不留内核日志,只在 `memory.events` 的 `oom_kill` 计数;归因时先查那里。
- 判据纯 RSS/增长,不看 OOM score——被保护进程自己泄漏不在此守护范围。
