# XR1710G 原厂主 PCIe Gen3 x2 启动参考实现

本目录是根据 XR1710G 公开原厂 5.1.1.12 与工程机内核反汇编重建的 C11 代码。覆盖主 PCIe 双 lane 冷启动中已经核对的部分：选择 x2 拓扑、SCU 复位、两组 MAC 的 RX detect / AEQ / EQ / PIPE 设置、两个 lane 的 Gen3/Gen2 接收预校准、释放 PERST、配置 Root Port、轮询 LINKUP。

入口为 `xr_stock_pcie_boot_x2()`，`stock_pcie.h` 定义寄存器读写、延时和共享 PHY 初始化回调。这里的寄存器编号按 bank 计算：MAC0/1 对应原厂 DTS 的 `0x1fc00000 / 0x1fc20000`，CSR_2L / PMA0 / PMA1 对应 `0x1fa5a000 / 0x1fa5b000 / 0x1fa5c000`。实现没有真实 MMIO 后端。

| 步骤 | 工程内核 Image 偏移 | 公开原厂 5.1.1.12 Image 偏移 |
| --- | --- | --- |
| SerDes 分支及 SCU x2 模式 | `mtk_pcie_probe`，`0x3417c0–0x341830`；SSTR/PCIC 偏移由 `0x41120 / 0x41150` 的读函数核对 | `mtk_pcie_probe`，`0x36f9dc–0x36fa4c` |
| SCU 复位、MAC 参数和释放 PERST | `mt7512_pcie_reset`，`0x340870–0x340b4c` 的双 lane 分支 | `mt7512_pcie_reset`，`0x36e970–0x36ec44`；随后另有 LTSSM 诊断采样 |
| 双 lane 预校准调用与结束时序 | `PCIe_G3_L0L1_init`，调用点 `0x33e66c / 0x33e67c / 0x33e6e4 / 0x33e6f0`；CDR 脉冲 `0x33e424–0x33e4d4` | `PCIe_G3_L0L1_init`，函数入口 `0x369fd0` |
| IDAC 搜索、锁定检查及结果写回 | `Rx_PR_Fw_Pre_Cal`，入口 `0x33baa0`；最终检查 `0x33bea0–0x33bf28`；Gen2 额外写回 `0x33bfc4–0x33bfe8` | `Rx_PR_Fw_Pre_Cal`，入口 `0x3698d0`；最终检查 `0x369cd0–0x369d58`；Gen2 额外写回 `0x369df4–0x369e18` |
| Root Port 设置与 LINKUP 轮询 | `0x341328–0x3413bc` | `0x36f544` 起的对应 probe 路径 |

偏移是解压后内核 Image 的文件偏移。仓库提供重建代码与分析说明；原厂镜像、工程备份及反汇编转储保留在各自来源处。公开镜像取自 [5.1.1.12 固定提交](https://github.com/akorshun/xr1710g-backup-and-flash/blob/1da28af0821a53e64e36c19381c178ae9d5ce39d/firmware/xr1710g-stock-5.1.1.12-tclinux.bin)，工程版来自用户提供的工程备份。原厂 DTS、镜像哈希和 B17 配置对照见 [原厂 PCIe 训练与 DTS 对照](原厂PCIe3.0x2训练与DTS对照.md)。

当前只接受两个无线 SerDes 模式都为 0 的主 x2 分支，排除 `HIR=0xe、package=5` 的限速路径。共享 PLL、参考时钟、SSC 等前半段尚未逐条还原，必须由 `prepare_shared_phy()` 提供；模拟测试只实现了这个回调的替身。PBus、引脚配置、PCI 枚举、辅助链路独立训练和单 lane 重训练不在本实现内。零售版释放 PERST 后的约 60,000 轮 LTSSM 采样未移植，参考实现统一采用工程版约 1 秒的等待；轮询也使用固定 10 μs 步进近似原厂约 100 ms 的时钟期限。代码返回成功仅表示 MAC 报告 LINKUP；实机需读 Root Port 和端点的 PCIe Link Status 确认双方为 8.0 GT/s x2。

B17 已使用相同的 RX detect、AEQ、EQ、PIPE 值，以及 IDAC 粗调 1～7。本次核对发现原厂在最终阶段固定采样 10 次，B17 首次锁定即退出；原厂还将 Gen2 IDAC 写到每个 lane 的 `PMA+0x174`，B17 预校准函数无对应写入。参考实现保留两项行为，以便后续做单变量对照。现有证据没有证明这两项可以解决 Wi-Fi 消失。

在 Linux 或 WSL 中，从仓库根目录运行：

```sh
bash docs/xr1710g-stock-pcie-gen3x2/run-tests.sh
```

脚本用 C11 严格编译选项和 UBSan 检查，验证双 lane 校准顺序、第 7 档、10 次最终采样、两种 SCU 复位分支、链路成功和超时、无效模式与共享 PHY 初始化失败。结果见 [验证结果](验证结果.txt)。模拟寄存器验证覆盖控制流，不提供真实硬件时序或稳定性证据。
