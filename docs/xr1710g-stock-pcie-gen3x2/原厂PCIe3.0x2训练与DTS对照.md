# XR1710G：原厂 PCIe 3.0 x2 训练与 B17 DTS 对照

核对日期：2026-11-11（北京时间）。结论来自用户工程备份、公开原厂 5.1.1.12 的离线反汇编，以及已完成的 B17 启动/压测日志。本次没有操作路由器，没有修改构建源码或固件。

**原厂把双 lane 模式选择、PHY 预校准和复位时序放在厂商驱动中；DTS 不直接指定 Gen3 x2。B17 使用另一套 binding，在 DTS 中显式选择主 Gen3 x2，再由驱动设置 SCU lane 复用、两组 MAC 和 PHY。已核对的几组关键训练参数相同；不能因此称整套寄存器时序逐项完全相同。**

## 参考对象和可追溯性

文中提到的原始镜像、反汇编、DTB、哈希清单及实机日志属于离线证据，此分支附的是分析记录与模拟测试。

| 对象 | 固定来源与核对内容 |
| --- | --- |
| 工程机 | 用户提供的 `OpenWrt.mtd*.bin`，Linux 5.4.55；从原备份提取的 DTB、解压内核及恢复的 kallsyms。没有执行原厂二进制。 |
| 公开原厂 5.1.1.12 | [akorshun/xr1710g-backup-and-flash，固定提交 1da28af0821a53e64e36c19381c178ae9d5ce39d](https://github.com/akorshun/xr1710g-backup-and-flash/blob/1da28af0821a53e64e36c19381c178ae9d5ce39d/firmware/xr1710g-stock-5.1.1.12-tclinux.bin)。容器 SHA256：`4b3a3191007bb197061445cf605a6fd528100ab5c5b6b25d53b9d900408cc83f`。 |
| B17 | 实测源码 `e553ff6189cccdada2b572d451f02c251dd6ffb7`，基线 `a6dce40a91ca3f0ebeca9e8f51c8318f4d453eb3`，实际构建内核 Linux 6.18.60。 |
| B17 host 驱动 | 实际编译目录的 `drivers/pci/controller/pcie-mediatek-gen3.c`，离线分析副本保存为 `current-pcie-airoha.c`。不是另一个同名旧构建目录。 |
| B17 PHY 驱动 | 实际编译目录的 `drivers/phy/airoha/phy-airoha-pcie.c` 和寄存器头文件；保存在本目录。 |

工程解压内核 SHA256：`ec443e0335ead5089fa76b78fe3ce67079d24c2ef5cbb80d7c55644eae60530a`；公开原厂解压内核 SHA256：`4b74cbb519a6eedf229b43c9c8bb7fbca2c56a1620cf9a8fdedbe364345f291e`。这些是解压内核哈希，与完整固件容器哈希不同。

`inputs.json` 保存本次复制文件的来源与 SHA256；`checks.json` 保存独立核验结果。反汇编文件名中的地址是解压 Image 的文件偏移，不是运行时物理地址。

## 1. 原厂 DTS 实际配置

工程版和公开原厂版的三个 PCIe 节点、`pcie_phy` 节点逐字一致，已用脚本核对。下面是省略中断映射后的原厂摘录：

```dts
pcie0@1fc00000 {
        device_type = "pci";
        compatible = "ecnt,pcie-ecnt";
        reg = <0x00 0x1fc00000 0x00 0x20000>;
        reg-names = "pcie-mac";
        bus-range = <0x00 0xff>;
        ranges = <0x02000000 0x00 0x20000000
                  0x00 0x20000000 0x00 0x04000000>;
        /* #address-cells/#size-cells、interrupts 与 interrupt-map 等省略 */
};

pcie1@1fc20000 { /* 同类私有 binding，地址为 0x1fc20000 */ };
pcie2@1fc40000 { /* 同类私有 binding，地址为 0x1fc40000 */ };

pcie_phy {
        compatible = "econet,ecnt-pcie_phy";
};
```

这几个原厂节点没有 `num-lanes`、`max-link-speed`，也没有现有 Linux generic PHY 的 `phys`/`phy-names` 配置。原厂主/次控制器均有节点，不等于三个控制器必定作为三个独立链路启用；厂商 probe 会根据 SerDes 模式跳过被双 lane 主链路占用的独立 port。

**不能把“原厂 DTS 没写限速”解释为应该删除 B17 的 `max-link-speed`，也不能把原厂 `pcie1` 节点存在解释为 B17 应启用 `&pcie1`。** 两套驱动的配置接口不同。

完整原始节点摘录保存在 `original-pcie0-1fc00000.dts`、`original-pcie1-1fc20000.dts`、`original-pcie2-1fc40000.dts`、`original-pcie_phy.dts`。

## 2. 原厂怎样选择双 lane、初始化和启动训练

工程版主调用链可核对为：

```text
mtk_pcie_probe
  ├─ 查询 SerDes interface mode，设置 SCU PCIC lane 模式
  ├─ 设置 PBus 地址窗口和 reset 引脚 open-drain
  ├─ mt7512_pcie_reset
  │    ├─ SCU 复位/保持 PERST、等待
  │    ├─ 设置两组 MAC 的 RX detect / AEQ 时间
  │    ├─ pcie_phy_init(3)                 ← 3 是厂商模式选择，不是三个 lane
  │    │    └─ PCIe_G3_L0L1_init(1)
  │    │         ├─ PLL、时钟、RX/TX、SSC 等模拟/数字初始化
  │    │         ├─ Gen3：lane0、lane1 预校准
  │    │         └─ Gen2：lane0、lane1 预校准
  │    ├─ 释放 PHY 复位、配置两组 MAC 的 EQ / PIPE 参数
  │    └─ 释放 PERST，并等待端点准备
  ├─ 配置 RC，轮询 LINKUP
  └─ PCI 枚举
```

工程版关键位置：

- `mtk_pcie_probe`：`0x3411d4`；SerDes 模式判断约 `0x3417c0–0x341830`。
- `get_serdes_interface_sel`：`0x37c5f0`，读取厂商保存的 mode 表；它不是在本函数里解析 DTS 的 `num-lanes`。
- 主 port 的一种 mode 分支执行 `(GET_PCIC() & ~3) | 2`，另一分支 OR `3`；B17 双 lane 选择对应的 SCU 低位设置也是 `BIT(1)`。这里只据调用路径解释该选择，未完整追溯 mode 表在所有引导场景下的来源。
- 全局初始化/复位调用：`0x3419b0 -> mt7512_pcie_reset`。
- `mt7512_pcie_reset`：`0x340870`；`0x3409c4` 传参数 3，调用 PHY wrapper；具体厂商 `pcie_phy_init` 位于 `0x340130`。
- `PCIe_G3_L0L1_init`：`0x33c1a0`；四次预校准调用在 `0x33e66c / 0x33e67c / 0x33e6e4 / 0x33e6f0`。
- `mtk_pcie_probe` 中轮询 `MAC + 0x154` 的 bit8 为 LINKUP，约 100ms 超时；链路失败会输出 `MAC + 0x150` 的 LTSSM 值。

公开原厂 5.1.1.12 的对应函数同样有这些初始化结构，偏移不同：`mt7512_pcie_reset = 0x36e970`、`PCIe_G3_L0L1_init = 0x369fd0`、`Rx_PR_Fw_Pre_Cal = 0x3698d0`。

### 预校准与链路训练是两个阶段

`Rx_PR_Fw_Pre_Cal` 搜索片内接收器的 IDAC/FLL 参数，属于 PHY 预校准。之后释放端点 PERST，由控制器与 MT7996 的硬件 LTSSM 交换训练序列并协商宽度和速度。

按 PCIe 协议，链路先以基础速率建立，再在支持的情况下切换高速并完成 Gen3 均衡；CPU 代码不是逐个发送 TS1/TS2 训练包。这里只把该过程作为协议背景，**没有原厂实机 LTSSM 轨迹，不能据静态反汇编宣称复原了每个硬件训练状态或原厂实际协商结果**。

原厂还提供 `pcie_api_set_speed`：核对双方 Link Capabilities、修改双方 Link Control 2 的 Target Link Speed，给 Root Port 的 Link Control 置 Retrain Link bit5，再等待。该接口存在不等于每次启动都调用它；已检查的主 probe/reset 直接路径没有调用此 API。

## 3. 已核对的关键参数

| 参数/位置 | 工程版、公开原厂 | B17 | 判断 |
| --- | --- | --- | --- |
| RX detect，MAC0/1 `+0x10044` | `0x23020133` | 由相同字段生成 `0x23020133` | 相同 |
| RX AEQ，`MAC0 +0x15030/+0x15130` | `0x50500032` | `rx_aeq` 两 lane 写 `0x50500032` | 相同 |
| EQ preset，MAC0/1 `+0x100` | `0x41474147` | `0x41474147` | 相同；每 lane downstream=0x47、upstream=0x41 |
| PIPE/EQ control，MAC0/1 `+0x338` | `0x1018020f` | `0x1018020f` | 相同 |
| Gen3 FLL 目标/窗口 | 41600 ±100 | 相同 | 目标值不是直接以 MHz 表示的链路频率 |
| Gen3 cycle count | 26000 | 相同 | 相同 |
| Gen2 FLL 目标/窗口 | 41941 ±100 | 相同 | 相同 |
| Gen2 cycle count | 32767 | 相同 | 相同 |
| IDAC 粗调 | 1～7 | 1～7 | B17 本地补丁已修改；补丁前上游为 0～6 |
| IDAC 细调 | bit7→bit0 | bit7→bit0 | 相同结构 |
| 预校准覆盖 | 两 lane 各做 Gen3、Gen2 | 相同 | DTS 限 Gen2 也不等于跳过 PHY 的 Gen3 预校准 |

两个原厂内核的粗调常量数组均已从原字节重新核验为 `1,2,3,4,5,6,7`：工程版偏移 `0x7b2748`，公开原厂偏移 `0x7b3588`。详见 `checks.json`。

### 等待时间与时序差异

工程版 reset 中的 `__const_udelay` 使用 AArch64 `udelay` 常量换算，`0x418958 / 0x10c7 = 1000us`。循环次数分别可核对到：

- 初始复位保持：100 ×1ms，约100ms。
- PHY 初始化后：30 ×1ms，约30ms。
- EQ/PIPE 设置后、释放 PERST 前：10 ×1ms，约10ms。
- 释放 PERST 后：1000 ×1ms，约1秒；此后还有 LINKUP 轮询。

B17 的 host 路径有 PHY reset assert 后100ms、PHY reset deassert 后100ms、refclk使能后100ms、PERST释放后1000ms；PHY init 内部另有30ms。等待时间的**位置与组合并非逐项相同**，不能只用“都有100ms”证明启动顺序完全一致。

原厂工程版与公开版的 SCU 复位掩码自身也有差异，例如 `0x834` 初始工程版包含额外的 bit29 操作，公开版对应路径不完全一样。因而不应把工程机的所有原始 SCU 写入直接移植到现有 reset controller 驱动。

## 4. B17 DTS 如何表达相同的主链路拓扑

下面取自 B17 原分区机型，只省略无线 EEPROM、NPU 及其他设备内容。`&pcie0` 与 `&pcie2` 所需的默认 `phys`、时钟、PBus 窗口等由 `an7581.dtsi` 继承。

```dts
&pcie0 {
        status = "okay";
        reg = <0x0 0x1fc00000 0x0 0x1670>,
              <0x0 0x1fc20000 0x0 0x1670>;
        reg-names = "pcie-mac", "sec-pcie-mac";
        resets = <&scuclk EN7581_PCIE0_RST>,
                 <&scuclk EN7581_PCIE1_RST>,
                 <&scuclk EN7581_PCIC_PERSTOUT0_RST>,
                 <&scuclk EN7581_PCIC_PERSTOUT1_RST>;
        reset-names = "phy-lane0", "phy-lane1",
                      "perstout", "sec-perstout";
        num-lanes = <2>;
        max-link-speed = <3>;
        airoha,scu = <&scuclk>;
        pinctrl-names = "default";
        pinctrl-0 = <&pcie0_rst_pins>;
};

&pcie1 {
        status = "disabled";
};

&pcie2 {
        status = "okay";
        max-link-speed = <2>;
        airoha,wait-for-pcie0-perst;
        airoha,scu = <&scuclk>;
        pinctrl-names = "default";
        pinctrl-0 = <&pcie2_rst_pins>;
};

&usb1_phy {
        airoha,pcie-analog-init;
};
```

这里的 `pcie0 + pcie1` 两组 MAC 资源由一个主机控制器统一管理，组成主 x2；`pcie1` 禁用的是独立 host 节点，**不会把主链路第二 lane 禁掉**。`pcie2` 是另一条辅助 HIF 链路，当前配置 Gen2 x1；它不与主链路相加成为 x3。

`airoha,pcie-analog-init` 给 PCIe2 所用的另一套 USB/PCIe 复用 PHY 做本地参考原厂的四项模拟初始化，不是主 Gen3 x2 的开关。当前主 PHY 来自 `an7581.dtsi` 的 `pcie_phy: phy@1fa5a000`。

B17 host 的实际动作是：从 DTS 读取 lane=2、max speed=3，SCU PCIC 设为双 lane 模式，先初始化/上电 PHY 再释放 PHY reset，给两组 MAC 写入速率支持和 Target Link Speed，给两组 MAC 写相同 EQ/PIPE 设置，然后释放 PERST。独立 PCIe2 最多等待主 PCIe0 PERST 释放3秒，避免提前起链路。

此平台 host 已配置 `SKIP_PCIE_RSTB`，避开普通 MediaTek 启动函数中直接操作 `PCIE_PE_RSTB` 的路径；源码注明 EN7581 此路径有偶发 link-down 硬件问题。这项现有平台处理应该保留，不能为“贴近原厂”再无条件叠加普通 MAC reset。

本次还反编译了 B17 的实际构建 DTB，保存为 `current-actual.dts`；DTB SHA256 为 `5ea844e11f537e1591adf90a8b3a5c0498211087a1a42d1a9603ea11087928f5`。dtc 原有告警保存为 `dtc-warnings.txt`，没有把反编译成功描述为完整 binding 校验通过。

## 5. 与 B17 实机记录对照

从已归档的 `router-final-complete-logs.tar.gz` 中重新筛出启动消息：

| lane / 代际 | IDAC | FL_OUT | LOCK | 尝试次数 |
| --- | --- | --- | --- | --- |
| lane0 Gen3 | 0x4ef | 41629 | 1 | 1 |
| lane1 Gen3 | 0x532 | 41596 | 1 | 1 |
| lane0 Gen2 | 0x512 | 41954 | 1 | 1 |
| lane1 Gen2 | 0x571 | 41931 | 1 | 1 |

均落在对应目标窗口。主链路启动配置日志为 `Gen3 x2`，辅助链路在主 PERST 释放后起链路；已有实机验收读取双方 PCI config/sysfs 为主 `8.0 GT/s x2`、辅助 `5.0 GT/s x1`，整夜727份遥测主链路保持 `8.0 GT/s x2`。

因此，B17 已有证据说明主链路实际上协商到 Gen3 x2；不只是 DTS 写了目标值。相反，本次没有运行原厂固件或拿到原厂实机 PCI 配置，不能把原厂静态代码提供 Gen3 双 lane 能力当作它在所有机器上都必然以该速度运行的证据。

本次 IDAC 都在0x4xx～0x5xx范围，上游原0～6粗调范围也覆盖这些粗调档。**这次启动日志不能证明“加入第7档”是稳定性改善的原因**。锁定成功也不是长期误码/信号裕量测量。

## 6. 对后续修改的判断

1. 当前没有证据要求再次改变 B17 的 `num-lanes`、主 Gen3 或辅助 Gen2 设置；两组 MAC、SCU 双 lane、第二独立 host 禁用的关系正确，且实测已协商到目标速度。
2. 原厂 IDAC 1～7和 PCIe2 四项模拟配置在 B17 中已有本地参考实现；本轮新增核对没有形成必须再应用的新补丁。
3. EQ preset、PIPE 控制、RX detect、AEQ 等已核对数值一致，不能再把它们写成“B17 缺失的原厂参数”。
4. 更深入的剩余工作是主 PHY 的 PLL/SSC/CDR 寄存器序列逐项核对，以及故障前后 LTSSM/AER/MCU/NPU 同时序记录。原厂有 eyescan/relink 辅助符号，不代表正常启动必定运行眼图扫描或故障后必定调用重训练。
5. 历史 `MCU timeout -> CmpltTO` 既可能来自链路，也可能来自端点固件失去响应；当前静态对照和无故障长测仍不足以把原始 Wi-Fi 消失归因于 PCIe PHY。

这份记录是本次新整理的分析文档，没有自动更新 PR #92，也没有引入新的运行代码修改。
