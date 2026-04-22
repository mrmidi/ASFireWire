# SBP-2 开发路线图

> 目标：在 ASFW 驱动中实现完整的 SBP-2（Serial Bus Protocol 2）协议栈，支持 FireWire 扫描仪及其他 SBP-2 设备的发现、登录、命令传输与后续系统集成。

## 现状

- **已完成**：
  - `AddressSpaceManager` — 地址空间分配、DMA 后端、远程读写响应、UserClient 四方法 API（选择器 46-49）、`PacketRouter` tCode 路由集成
  - **阶段一**：SBP-2 核心数据结构 — `SBP2WireFormats.hpp` 和 `SBP2PageTable.hpp`
  - **阶段二的最小发现链路**：驱动已基于 `Unit_Spec_Id == 0x010483` 将设备归类为 `DeviceKind::Storage`，Swift 侧已解析 `deviceKind` 并可筛出 SBP-2 storage unit
  - **阶段三**：`SBP2LoginSession` / `SBP2ManagementORB` 已实现核心登录状态机、状态块处理、超时重试与任务管理 ORB
  - **阶段四**：`SBP2CommandORB`、页表、Fetch Agent 操作与事务跟踪已接入登录会话
  - **最小 UserClient / Swift 调试闭环**：`SBP2SessionRegistry`、`SBP2Handler`、Swift `DriverConnector+SBP2.swift`、`SBP2DebugViewModel`、`SBP2DebugView` 已接通 `create session -> start login -> get state -> submit INQUIRY -> fetch result -> release`
  - **基础测试**：已有 `AddressSpaceManagerTests`、`SBP2LoginSessionTests`、`SBP2ORBTests`，Swift 侧新增 `DeviceDiscoveryWireParsingTests`
- **进行中**：
  - **阶段二**：更完整的 SBP-2 设备建模、metadata 与通知路径
  - **阶段五**：当前仅完成面向调试的 `INQUIRY` vertical slice，尚未抽象为通用 SCSI 命令层
  - **阶段六**：总线重置恢复、资源清理、真机 smoke、完整 DriverKit 构建收口
- **未完成**：
  - 扫描仪特定命令与工作流
  - 面向产品功能的块读写 / 扫描业务 UI
  - 生产级健壮性与更广泛硬件回归

---

## 阶段一：SBP-2 协议数据结构与常量 ✅

**目标**：定义 SBP-2 规范中的核心数据结构，不涉及运行时逻辑。

### 交付物

- [x] `Protocols/SBP2/SBP2WireFormats.hpp` — Management ORB、LoginResponse、StatusBlock、CommandBlockORB、PageTableEntry 等 wire-format 类型
- [x] `Protocols/SBP2/SBP2PageTable.hpp` — scatter-gather 页表构建器，含 direct-address 快捷路径
- [x] 关键结构体布局与 SBP-2 规范一致，并在代码中保留规范语义

### 备注

实现没有拆分为 `SBP2Constants.hpp` + `SBP2Types.hpp`，而是统一放在 `SBP2WireFormats.hpp` 中。常量定义主要以内联 `constexpr` 的形式存在。

---

## 阶段二：SBP-2 设备发现与分类 🟡

**目标**：让驱动能从 Config ROM 中识别 SBP-2 设备，并把最小可用信息传到 Swift 调试层。

### 交付物

- [x] 扩展 `DeviceRegistry::ClassifyDevice()` — 识别 `Unit_Spec_Id == 0x010483`，返回现有的 `DeviceKind::Storage`
- [x] 在 discovery wire format 中带出 `deviceKind`
- [x] 在 Swift 端 `FWDeviceInfo` 中反映 `deviceKind`，并支持 `storageUnits` / `isSBP2Storage` 过滤
- [ ] 细化 SBP-2 设备建模 —— 评估是否需要在 `DeviceKind::Storage` 之上增加 scanner 细分，或通过额外 metadata 区分
- [ ] 扩展 discovery 元数据 —— 增加更完整的 LUN / Management Agent / 设备能力信息
- [ ] 在 `DeviceManager` 中增加 SBP-2 设备通知路径（类似音频设备的 observer 模式）

### 验收标准

- 驱动日志中可见 SBP-2 设备被归类为 `DeviceKind::Storage`
- Swift 应用能通过 `getDiscoveredDevices` 看到 storage 设备及其 unit / ROM offset 信息
- 不影响现有音频设备分类

---

## 阶段三：Management Agent 与登录协议 ✅

**目标**：实现 SBP-2 登录/重连的核心会话机制，并为上层调试流提供最小 session 生命周期 API。

### 交付物

- [x] `Protocols/SBP2/SBP2LoginSession.hpp/cpp` — Login / Reconnect / Logout 状态机、状态块处理、超时与重试逻辑
- [x] `Protocols/SBP2/SBP2ManagementORB.hpp/cpp` — AbortTask、AbortTaskSet、LogicalUnitReset、TargetReset
- [x] ORB / 状态 FIFO 地址空间分配 —— 通过 `AddressSpaceManager` 集成实现
- [x] `Protocols/SBP2/SBP2SessionRegistry.hpp/cpp` — 面向 UserClient 的会话注册表与目标解析
- [x] UserClient 最小 session API —— `createSBP2Session`、`startSBP2Login`、`getSBP2SessionState`、`releaseSBP2Session`
- [x] `ControllerCore` / `UserClientRuntimeState` wiring —— address-space manager 与 session registry 已显式注入
- [ ] 系统级 bus-reset / reconnect 收口 —— `SBP2LoginSession` 类内逻辑存在，但还未完成主生命周期整合验证

### 数据流

```text
Swift App → UserClient → SBP2SessionRegistry → SBP2LoginSession → AddressSpaceManager → Async TX → FireWire Device
                                                                                                  ↓
Swift App ← UserClient ← Session State / StatusFIFO ← AddressSpaceManager ← Async RX ← Status Block
```

### 验收标准

- `SBP2LoginSessionTests` 已提供基础单元测试覆盖
- 代码路径可查询登录状态、generation、loginID、lastError、reconnectPending
- 真机 login / reconnect smoke 仍待完成

---

## 阶段四：Fetch Agent 与命令传输 ✅

**目标**：通过 Fetch Agent 提交命令 ORB，并把最小调试命令链路暴露给 Swift。

### 交付物

- [x] Fetch Agent 管理 —— `ResetFetchAgent()`、`RingDoorbell()`、ORB Pointer 写入与状态跟踪
- [x] `Protocols/SBP2/SBP2CommandORB.hpp/cpp` — 命令 ORB、数据描述符、回调与页表支持
- [x] `Protocols/SBP2/SBP2PageTable.hpp` — scatter-gather 与 direct-address 路径
- [x] SBP-2 事务跟踪 —— 由 `SBP2LoginSession` 集成管理
- [x] UserClient / Swift 最小命令 API —— `submitSBP2Inquiry`、`getSBP2InquiryResult`
- [ ] 通用命令提交接口 —— 当前仍以最小 INQUIRY 调试接口为主，未抽象为通用命令层

### 验收标准

- Swift 调试页可以沿 `submit INQUIRY -> fetch result` 路径读取响应数据
- `SBP2ORBTests` 已覆盖基础 ORB / 传输辅助逻辑
- 真机 INQUIRY smoke、队列化多命令与大数据传输仍待补充验证

---

## 阶段五：SCSI 命令层与扫描仪适配 🟡

**目标**：在 SBP-2 命令传输之上实现更通用的 SCSI 命令层，并逐步演进到扫描仪适配。

### 交付物

- [x] 最小 `INQUIRY` vertical slice —— 目前已作为调试流的一部分接入 `SBP2SessionRegistry` / Swift Debug UI
- [ ] `Protocols/SBP2/SCSICommandSet.hpp/cpp` —— 通用 SCSI 命令抽象（`INQUIRY`、`TEST_UNIT_READY`、`REQUEST_SENSE`、`READ_CAPACITY` 等）
- [ ] 扫描仪特定命令与能力建模（如目标设备需要）
- [ ] 面向业务流程的 Swift 会话 API，而不仅是调试入口

### 验收标准

- 调试 UI 能展示 `INQUIRY` 的 vendor / product / revision 与原始响应数据
- 真机上完成一次稳定的 `login + inquiry` smoke
- 通用 SCSI 命令集与扫描仪工作流仍待实现

---

## 阶段六：健壮性与系统集成 🟡

**目标**：补齐错误恢复、总线重置处理、资源管理与完整构建验证，使 SBP-2 从调试闭环走向可持续维护。

### 交付物

- [ ] 总线重置恢复 —— 将 `HandleBusReset()` / `Reconnect()` 真正接入驱动主生命周期并做硬件验证
- [ ] 资源清理 —— 连接断开时释放 ORB、地址空间、DMA 缓冲区，并验证无泄漏
- [ ] 错误恢复 —— 完善重试策略、命令失败收敛、Fetch Agent 重置边缘路径
- [ ] 并发与多 LUN 场景 —— 锁保护与生命周期规则
- [ ] Swift 应用集成 —— 从 Debug 页面演进到更稳定的产品级入口
- [ ] 文档更新 —— 在仓库文档中补充 SBP-2 架构与调试说明
- [ ] 构建收口 —— 解决 `SBP2ManagementORB.cpp` / `SBP2CommandORB.cpp` 中 `IODispatchQueue::DispatchAsyncAfter` 相关的 DriverKit 构建问题
- [ ] 扩充测试 —— bus reset、reconnect、硬件 smoke、更多 Swift / C++ 回归用例

### 验收标准

- 热插拔与总线重置不会导致驱动崩溃或会话永久失效
- 真机 smoke 能稳定完成 `create session -> login -> inquiry -> release`
- 完整 DriverKit scheme 可通过当前工程构建

---

## 文件结构预览

```text
ASFWDriver/Protocols/SBP2/
  ├── AddressSpaceManager.hpp/cpp    # ✅ 已完成
  ├── SBP2WireFormats.hpp            # ✅ 已完成
  ├── SBP2PageTable.hpp              # ✅ 已完成
  ├── SBP2LoginSession.hpp/cpp       # ✅ 已完成
  ├── SBP2ManagementORB.hpp/cpp      # ✅ 已完成
  ├── SBP2CommandORB.hpp/cpp         # ✅ 已完成
  └── SBP2SessionRegistry.hpp/cpp    # ✅ 已完成（会话注册与 INQUIRY 调试闭环）

ASFWDriver/UserClient/Handlers/
  └── SBP2Handler.hpp                # ✅ 已包含地址空间 + session/inquiry selectors

ASFW/
  ├── DriverConnector+Discovery.swift   # ✅ 已解析 deviceKind
  ├── DriverConnector+SBP2.swift        # ✅ 已完成最小 session / inquiry API
  ├── ViewModels/SBP2DebugViewModel.swift
  └── Views/SBP2DebugView.swift

tests/
  ├── AddressSpaceManagerTests.cpp
  ├── SBP2LoginSessionTests.cpp
  └── SBP2ORBTests.cpp

ASFWTests/
  └── DeviceDiscoveryWireParsingTests.swift
```

## 依赖关系

```text
阶段一（类型定义）✅
  ├── 阶段二（设备分类）🟡
  └── 阶段三（登录协议）✅
        └── 阶段四（Fetch Agent / 命令传输）✅
              └── 阶段五（SCSI 命令层）🟡
                    当前先冻结在 INQUIRY vertical slice
                    └── 阶段六（健壮性与系统集成）🟡
```

## 风险与注意事项

1. **参考实现稀缺**：仍需持续参考 Linux `firewire-sbp2` 与 Apple `IOFireWireSBP2` 的行为差异
2. **扫描仪兼容性**：目标扫描仪可能使用厂商特定命令集，不能简单类比存储设备
3. **OHCI 描述符限制**：Fetch Agent 写操作与命令提交流程仍需更多硬件侧验证
4. **DMA 一致性**：大数据传输时页表映射与 OHCI 硬件同步仍是高风险点
5. **DriverKit API 差异**：当前完整 DriverKit 构建仍被 `IODispatchQueue::DispatchAsyncAfter` 用法阻塞，需要先修正 ORB 定时实现
6. **硬件验证缺口**：当前已打通软件调试闭环，但真机 `login + inquiry` smoke 还未形成稳定证据
7. **生命周期收口不足**：bus reset / reconnect 的类内能力已存在，但系统级接线与恢复策略仍待验证
