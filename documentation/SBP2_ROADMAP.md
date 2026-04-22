# SBP-2 开发路线图

> 目标：在 ASFW 驱动中实现完整的 SBP-2（Serial Bus Protocol 2）协议栈，支持 FireWire 扫描仪（及其他 SBP-2 设备如存储设备）的发现、登录和数据传输。

## 现状

- **已完成**：
  - `AddressSpaceManager` — 地址空间分配、DMA 后端、远程读写响应、UserClient 四方法 API（选择器 46-49）、PacketRouter tCode 路由集成
  - **阶段一**：SBP-2 核心数据结构 — `SBP2WireFormats.hpp`（LoginORB、ReconnectORB、LogoutORB、LoginResponse、StatusBlock、CommandBlockORB、PageTableEntry 等全类型定义 + BE 转换）和 `SBP2PageTable.hpp`（scatter-gather 页表构建器）
  - **阶段三**：登录协议 — `SBP2LoginSession`（1711 行）实现完整 Login/Logout/Reconnect 状态机、状态块处理（solicited + unsolicited）、超时/重试逻辑，并具备类内 bus-reset/reconnect 逻辑
  - **阶段三附加**：`SBP2ManagementORB`（375 行）— 任务管理 ORB（AbortTask、AbortTaskSet、LogicalUnitReset、TargetReset）
  - **阶段四**：Fetch Agent 与命令传输 — `SBP2CommandORB`（349 行）集成于 LoginSession 内，含 ORB 链接、Doorbell 机制、Fetch Agent Reset、页表支持
  - **UserClient**：`SBP2Handler` — 地址空间操作（选择器 46-49）
  - **Swift**：`DriverConnector+SBP2.swift` — 地址空间 API 封装
- **未实现**：SBP-2 设备分类（阶段二）、UserClient 登录/命令 API、SCSI 命令层（阶段五）、生产健壮性与系统集成（阶段六）

## 阶段一：SBP-2 协议数据结构与常量 ✅

**目标**：定义 SBP-2 规范中的所有核心数据结构，不涉及运行时逻辑。

### 交付物

- [x] `Protocols/SBP2/SBP2WireFormats.hpp` — SBP-2 全部 wire-format 类型：
  - Management ORB 类型（LoginORB、ReconnectORB、LogoutORB）
  - LoginResponse、StatusBlock（含 src、resp、sbp_status、orb_offset、status_data）
  - CommandBlockORB（data_descriptor、options、data_size、command_block）
  - PageTableEntry（segment_length、segment_base_hi/lo）
  - BE16/BE32 转换辅助函数（`ToBE16`/`FromBE16`/`ToBE32`/`FromBE32`）
- [x] `Protocols/SBP2/SBP2PageTable.hpp` — 页表构建器（scatter-gather DMA → PTE 数组，含 direct-address 快捷路径）
- [x] 所有结构体的布局与 SBP-2 规范一致，通过代码注释引用规范章节

### 备注

文件组织与原计划不同：没有拆分为 `SBP2Constants.hpp` + `SBP2Types.hpp`，而是统一放在 `SBP2WireFormats.hpp` 中。常量定义分散在各类型的静态 constexpr 成员中。

---

## 阶段二：SBP-2 设备发现与分类

**目标**：让驱动能从 Config ROM 中识别 SBP-2 设备并正确分类。

### 交付物

- [ ] 扩展 `DeviceRegistry::ClassifyDevice()` — 识别 `Unit_Spec_Id == 0x010483`，返回现有的 `DeviceKind::Storage`
- [ ] 细化 SBP-2 设备建模 —— 评估是否需要在现有 `DeviceKind::Storage` 之上增加 `Scanner` 细分，或仅通过额外 metadata 区分
- [ ] 扩展 `DeviceRecord` — 增加 SBP-2 相关字段（`isSbp2Device`、LUN 列表、管理代理地址）
- [ ] 在 `DeviceManager` 中增加 SBP-2 设备通知路径（类似音频设备的 observer 模式）
- [ ] 在 Swift 端 `DeviceRecord` 中反映 SBP-2 分类结果

### 验收标准

- 连接 SBP-2 设备后，驱动日志中可见 `DeviceKind::Storage`（或 `Scanner`）分类
- Swift 应用能通过 `getDiscoveredDevices` API 看到 SBP-2 设备及其 LUN 信息
- 不影响现有音频设备分类

---

## 阶段三：Management Agent — 登录协议 ✅

**目标**：实现 SBP-2 登录/登出/重连协议，这是所有后续操作的前提。

### 交付物

- [x] `Protocols/SBP2/SBP2LoginSession.hpp/cpp` — 完整登录状态机（1711 行）：
  - `Login()` — 发送 Login ORB，接收登录状态，获取 Fetch Agent 地址和参数
  - `Logout()` — 发送 Logout ORB
  - `Reconnect()` — 总线重置后重连（保持会话）
  - `HandleBusReset()` — 总线重置通知，自动转换到 Suspended 状态；后续 `Reconnect()` 逻辑已在类内实现，但尚未接入系统主路径
  - 超时处理与重试逻辑（最多 32 次重试，1s 间隔）
  - 状态块接收与分发（solicited + unsolicited）
  - 地址空间自动分配（Login ORB、Login Response、Status Block、Reconnect ORB、Logout ORB）
  - Timer 基础设施（IODispatchQueue 延迟回调）
- [x] `Protocols/SBP2/SBP2ManagementORB.hpp/cpp` — 任务管理 ORB（375 行）：
  - AbortTask、AbortTaskSet、LogicalUnitReset、TargetReset
  - 独立的 per-ORB 状态 FIFO 地址空间
  - 完成/超时回调机制
- [x] ORB 内存分配 — 通过 `AddressSpaceManager` 集成实现，无独立 ORBAllocator
- [x] StatusFIFO — 通过 `AddressSpaceManager` 远程写回调机制接收，集成于 LoginSession
- [ ] UserClient API — Login/Logout 方法（选择器待分配）— **未完成**

### 数据流

```
Swift App → UserClient → ManagementAgent → AddressSpaceManager → Async TX → FireWire Device
                                                                          ↓
Swift App ← UserClient ← StatusFIFO ← AddressSpaceManager ← Async RX ← Status Block
```

### 验收标准

- C++ 单元测试覆盖 Login/Logout/Reconnect 的正常路径和错误路径 — **测试待补充**
- 使用真实 SBP-2 设备完成登录握手
- 登录后能获取 Fetch Agent 地址、reconnect_hold 等参数

---

## 阶段四：Fetch Agent 与命令传输 ✅

**目标**：通过 Fetch Agent 向设备提交命令 ORB，完成实际数据传输。

### 交付物

- [x] Fetch Agent 管理 — 集成于 `SBP2LoginSession` 中：
  - `ResetFetchAgent()` — 写入 `AGENT_RESET` 地址重置代理
  - `RingDoorbell()` — 写入 `DOORBELL` 地址唤醒代理
  - ORB Pointer 写入 — 写入 `ORB_POINTER` 地址提交 ORB
  - 状态跟踪（fetchAgentWriteInUse、doorbellInProgress）
  - ORB 链管理（lastORB_、deferredORB_）
- [x] `Protocols/SBP2/SBP2CommandORB.hpp/cpp` — 命令 ORB（349 行）：
  - 数据缓冲区描述符（输入/输出方向、长度）
  - 页表支持（通过 `SBP2PageTable` 处理大数据传输的分段映射）
  - ORB 链接（next_ORB 指针、Dummy ORB 标记）
  - 完成回调机制
- [x] `Protocols/SBP2/SBP2PageTable.hpp` — 页表构建器（163 行）：
  - Scatter-gather DMA 段转 PTE
  - Direct-address 快捷路径（单段且足够小时）
  - maxPageClipSize 分段
- [x] SBP-2 事务跟踪 — 集成于 LoginSession，通过 ORB 完成回调和超时管理实现
- [ ] UserClient API — 提交命令 ORB、获取完成状态 — **未完成**

### 验收标准

- 能向 SBP-2 设备提交 INQUIRY 命令并接收响应数据 — **待验证**
- 页表能正确处理超过 max_payload 的数据传输 — **代码已实现，待测试**
- ORB 链能正确排队多个命令 — **代码已实现，待测试**
- 错误恢复（超时、状态错误）能正确处理 — **代码已实现，待测试**

---

## 阶段五：SCSI 命令层与扫描仪适配

**目标**：在 SBP-2 之上实现 SCSI 命令传输，支持扫描仪特定操作。

### 交付物

- [ ] `Protocols/SBP2/SCSICommandSet.hpp/cpp` — 基础 SCSI 命令：
  - `INQUIRY` — 设备类型识别（扫描仪 = 类型 0x06）
  - `TEST_UNIT_READY` — 设备就绪检测
  - `REQUEST_SENSE` — 错误诊断
  - `READ_CAPACITY` — 容量查询
- [ ] `Protocols/SBP2/ScannerCommands.hpp/cpp` — 扫描仪特定命令（如适用）：
  - 基于 SCSI-3 SPC 扫描仪命令集
  - 图像参数设置（分辨率、色彩模式、扫描区域）
  - 数据读取（扫描图像获取）
- [ ] Swift 端扫描仪会话 API — 封装完整的扫描工作流

### 验收标准

- `INQUIRY` 能正确识别设备类型（扫描仪 vs 存储设备）
- 基本扫描仪操作（如果目标扫描仪支持）：参数设置 → 启动扫描 → 读取图像数据
- 数据完整性：传输的图像数据与预期一致

---

## 阶段六：健壮性与系统集成

**目标**：错误恢复、总线重置处理、资源管理和生产就绪。

### 交付物

- [ ] 总线重置恢复 — `SBP2LoginSession::HandleBusReset()` + `Reconnect()` 的类内逻辑已实现，但尚未接入驱动主生命周期 / UserClient 主路径，不能视为系统级完成
- [ ] 资源清理 — 连接断开时释放所有 ORB、地址空间、DMA 缓冲区（`DeallocateResources()` 已有骨架，需验证完整性）
- [ ] 错误恢复 — 重试策略、ORB 中止、Fetch Agent 重置（框架已就位，需完善边缘情况）
- [ ] 竞争条件防护 — 多 LUN 并发访问的锁保护
- [ ] UserClient 登录/命令 API — 阶段三、四的 UserClient 选择器封装
- [ ] Swift 应用集成 — 扫描仪设备的发现、连接、断开完整 UI 流程
- [ ] 文档更新 — CLAUDE.md 中补充 SBP-2 架构说明
- [ ] 单元测试 — LoginSession、CommandORB、ManagementORB、PageTable 的 C++ 单元测试

### 验收标准

- 热插拔测试：扫描仪连接/断开/重连不导致驱动崩溃或资源泄漏
- 总线重置测试：重置后会话能自动恢复（Reconnect 成功）
- 长时间运行稳定性测试
- 所有新增代码有对应的 C++ 单元测试

---

## 文件结构预览

```
ASFWDriver/Protocols/SBP2/
  ├── AddressSpaceManager.hpp/cpp    # ✅ 已完成
  ├── SBP2WireFormats.hpp            # ✅ 已完成（阶段一：全部 wire-format 类型 + 常量）
  ├── SBP2PageTable.hpp              # ✅ 已完成（阶段一+四：页表构建器）
  ├── SBP2LoginSession.hpp/cpp       # ✅ 已完成（阶段三+四：登录状态机 + Fetch Agent）
  ├── SBP2ManagementORB.hpp/cpp      # ✅ 已完成（阶段三：任务管理 ORB）
  ├── SBP2CommandORB.hpp/cpp         # ✅ 已完成（阶段四：命令 ORB）
  ├── SCSICommandSet.hpp/cpp         # 阶段五
  └── ScannerCommands.hpp/cpp        # 阶段五（可选）

ASFWDriver/UserClient/Handlers/
  ├── SBP2Handler.hpp/cpp            # ✅ 已完成（地址空间操作）
  └── SBP2SessionHandler.hpp/cpp     # 阶段六（登录/命令/状态 UserClient API）

ASFWDriver/UserClient/WireFormats/
  └── SBP2SessionWireFormats.hpp     # 阶段六

ASFW/
  ├── DriverConnector+SBP2.swift     # ✅ 已完成
  └── DriverConnector+SBP2Session.swift  # 阶段六

tests/
  ├── AddressSpaceManagerTests.cpp   # ✅ 已完成
  ├── SBP2LoginSessionTests.cpp      # 阶段六（待补充）
  ├── SBP2CommandORBTests.cpp        # 阶段六（待补充）
  ├── SBP2ManagementORBTests.cpp     # 阶段六（待补充）
  ├── SBP2PageTableTests.cpp         # 阶段六（待补充）
  └── SCSICommandTests.cpp           # 阶段五
```

## 依赖关系

```
阶段一（类型定义）✅
  ├── 阶段二（设备分类）— 无强依赖，可并行
  └── 阶段三（登录协议）✅
        └── 阶段四（Fetch Agent）✅
              └── 阶段五（SCSI 命令）— 依赖阶段四的命令传输
                    └── 阶段六（集成）— 依赖所有前序阶段
```

## 风险与注意事项

1. **参考实现稀缺**：项目 `documentation/` 目录下没有 SBP-2 规范文档，需要补充 Linux `firewire-sbp2` 和 Apple IOFireWireSBP2 的参考代码
2. **扫描仪兼容性**：FireWire 扫描仪（如 Nikon、Canon、Epson）可能使用厂商特定命令集，需逐型号验证
3. **OHCI 描述符限制**：SBP-2 的 Fetch Agent 写操作可能需要特殊的 AT 描述符配置
4. **DMA 一致性**：大数据传输时需确保页表映射的 DMA 缓冲区与 OHCI 硬件一致
5. **DriverKit 沙箱限制**：DriverKit 环境下某些内核级操作（如 IOMemoryDescriptor 操作）有额外约束
6. **测试覆盖缺口**：LoginSession、CommandORB、ManagementORB、PageTable 目前缺少单元测试（代码量大但未测试）
7. **UserClient API 缺口**：阶段三和四的核心逻辑已完成但尚未暴露 UserClient 选择器，Swift 应用无法直接调用登录/命令功能
