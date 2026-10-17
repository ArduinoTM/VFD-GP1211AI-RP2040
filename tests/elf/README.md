# tests/elf —— 入库的 ELF 反汇编/符号文本（供"产物级核对"离线复跑）

第三方评审 N5 指出：`check_park_gpio.js` 与 `compare_elf_functions.js` 原来需要
`arm-none-eabi-objdump/nm` + 固件 ELF 才能跑，外部读者复跑不了。这里把**生成结果**随仓库入库，
两个脚本都能**只用 node** 复跑（`--a/--b` 可直接给 `.txt/.txt.gz`）。

| 文件 | 来源 | 用途 |
|---|---|---|
| `pio-v1.4.0.txt.gz` / `.nm.gz` | 发布 **v1.4.0**（`5fe858e`）的 pio 单核构建 | 行为中性对照的 **A 侧** |
| `pio-v1.5.0.txt.gz` / `.nm.gz` | 发布 **v1.5.0**（`89ad18a`）的 pio 单核构建 | 对照的 **B 侧** |
| `tick-v1.5.1.txt.gz` / `.nm.gz` | 发布 **v1.5.1** 的 tick 单核构建 | 停泊/LED 的 GPIO 写入核对 |

## 复跑（不需要任何嵌入式工具链）

```bash
node tests/tools/check_park_gpio.js            # 用 tick-v1.5.1.txt.gz，退出码 0 = 全部判据通过
node tests/tools/compare_elf_functions.js      # 用 pio v1.4.0 → v1.5.0 对照对，退出码 0 = watch 清单逐指令相同
```

两条都已接进 `tests/run_host_tests.ps1`（第 ⑨ 步），CI 上随宿主测试一起执行。

## 重新生成（改了固件后刷新）

```bash
arm-none-eabi-objdump -d --no-show-raw-insn <elf> | gzip > tests/elf/<名字>.txt.gz
arm-none-eabi-nm -S --defined-only --numeric-sort <elf> | gzip > tests/elf/<名字>.nm.gz
```

取 A/B 两侧时必须是**同引擎、同配置**的构建（单核/双核、诊断开/关不同都会让对照失去意义）。