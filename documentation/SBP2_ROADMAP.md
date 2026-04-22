# SBP-2 Scanner-First Bring-up Roadmap

> 目标：把 FireWire 扫描仪调通到“可稳定发命令、可持续拿到协议证据”的状态。当前阶段以通用 SBP-2 unit bring-up 为主，不预设块设备语义，也不提前承诺扫描业务 UI。

## 当前阶段目标

本阶段成功标准：

- 扫描仪可在 discovery 中作为通用 `SBP-2 unit` 被看见
- Swift 调试页可创建 session、发起 login、轮询状态
- 调试页可执行标准探测命令：`INQUIRY`、`TEST UNIT READY`、`REQUEST SENSE`
- 调试页可执行 `raw CDB passthrough`
- 命令结果可回显 transport status、SBP-2 status、payload、sense
- DriverKit scheme 可以重新构建

本阶段明确不做：

- 图像采集工作流
- 扫描参数 UI
- 厂商协议高层封装
- `DeviceKind::Scanner` 新分类

---

## 现状

### 已完成

- **scanner-first 重定向**
  - 路线从 `storage-only` 调整为“通用 SBP-2 unit + scanner-first bring-up”
  - 不再依赖 `storageDevices` 作为调试入口

- **阶段一：SBP-2 基础协议与页表**
  - `SBP2WireFormats.hpp`
  - `SBP2PageTable.hpp`

- **阶段二：通用 SBP-2 unit 发现链路**
  - `FWUnit` 解析并暴露 `Management_Agent_Offset`、`LUN`、`Unit Characteristics`、`Fast Start`
  - discovery wire format 已携带上述字段
  - Swift `FWDeviceInfo` / `FWUnitInfo` 已暴露：
    - `hasSBP2Unit`
    - `sbp2Units`
    - `managementAgentOffset`
    - `lun`
    - `unitCharacteristics`
    - `fastStart`
  - 现有 `storageUnits` 暂时保留为兼容别名，但不再作为 SBP-2 Debug 唯一数据源

- **阶段三：session / login 生命周期**
  - `SBP2LoginSession`
  - `SBP2ManagementORB`
  - `SBP2SessionRegistry`
  - UserClient 生命周期 API：
    - `createSBP2Session`
    - `startSBP2Login`
    - `getSBP2SessionState`
    - `releaseSBP2Session`

- **阶段四：通用命令层基础版**
  - 新增 `SCSICommandSet.hpp`
  - `SBP2SessionRegistry` 已从 `INQUIRY-only` 演进为通用命令提交/取回结果
  - 保留 `submitSBP2Inquiry` / `getSBP2InquiryResult` 作为兼容包装
  - 新增标准 helper：
    - `INQUIRY`
    - `TEST UNIT READY`
    - `REQUEST SENSE`
  - 新增 `raw CDB passthrough`
  - 统一命令结果对象，包含：
    - `transportStatus`
    - `sbpStatus`
    - `payload`
    - `senseData`

- **阶段四：Swift 调试闭环**
  - SBP-2 Debug 页已改为通用 `SBP-2 Device / Unit`
  - 可执行：
    - `Create Session`
    - `Start Login`
    - `Release`
    - `INQUIRY`
    - `TEST UNIT READY`
    - `REQUEST SENSE`
    - `Raw CDB`
  - 结果页可展示 vendor / product / revision、sense 摘要、原始 payload 与状态码

- **阶段五前置：DriverKit 构建收口**
  - 新增 `SBP2DelayedDispatch.hpp`
  - 已移除对 `IODispatchQueue::DispatchAsyncAfter` 的直接依赖
  - 当前工程可重新通过 `xcodebuild`

- **基础验证**
  - `AddressSpaceManagerTests`
  - `SBP2LoginSessionTests`
  - `SBP2ORBTests`
  - `SBP2SessionRegistryTests`
  - `xcodebuild build -project ASFW.xcodeproj -scheme ASFW -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY='' -quiet`

### 进行中

- 真机 smoke：扫描仪实机 `discover -> session -> login -> inquiry -> TUR -> request sense -> raw cdb -> release`
  - 2026-04-22 当前安装方式：`./install-debug-asfw.sh` 安装到 `/Applications/ASFW.app`
  - 当前 active dext hash 与 app 内嵌 dext hash 一致：`506348c677a978b0d2d449b3ae348d4d00e94f5741c9b2affba46596fe8d9c37`
  - `systemextensionsctl` 仍显示一个旧 ASFW 条目处于 `terminated waiting to uninstall on reboot`，最终合并前建议重启清理一次
  - ASFW app 已能在 SBP-2 Debug 页发现 Nikon 设备：GUID `0x0090B54001FFFFFF`，node `0`，generation `2`，`1 SBP-2 unit`
  - 当前阻塞：该 unit 显示 `Mgmt Agent: n/a`，因此 session/login/command smoke 尚未进入可验证状态
- bus reset / reconnect 硬化
- in-flight 命令失败收敛与资源清理验证

### 未完成

- 修复/解释 Nikon SBP-2 unit 未暴露 `Management_Agent_Offset` 的问题
- 修复 Swift discovery wire parsing 测试失败
- 扫描仪厂商特定命令归纳
- 扫描业务 API / UI
- 更广泛的真机兼容性回归

### 最新验证记录（2026-04-22）

参考最新研究报告：

- `/Users/gly/workspace/github/moderncoolscan/docs/protocol/asfw-vs-apple-bus-init-comparison.md`

关键结论：

- Nikon management agent 地址计算与 Apple 一致：ROM `Management_Agent_Offset = 0x00c000` 时，CSR 地址应为 `0xF0030000`
- 当前问题不应优先归因于地址计算；更可能是设备在 ASFWDriver 初始化时没有完全激活 SBP-2 management agent CSR 空间
- 最高优先级诊断分两条线：
  - 验证 ASFWDriver block read/write 事务是否对已知 CSR 地址和其它 SBP-2 设备可靠
  - 收敛 ASFWDriver 与 Apple IOFireWireFamily 的 bus init 时序差异，尤其是初始双 bus reset 与 Self-ID 后立即 discovery

已执行：

- `cmake --build build/tests_build --target AddressSpaceManagerTests SBP2LoginSessionTests SBP2ORBTests SBP2SessionRegistryTests`
- `ctest --test-dir build/tests_build -R 'AddressSpaceManager|SBP2' --output-on-failure`

结果：

- SBP-2 host tests 通过：21/21
- 覆盖范围包括 address space、login session、ORB timer/status、session registry、标准 SCSI helper 与 REQUEST SENSE payload/sense 回收

已执行：

- `xcodebuild test -project ASFW.xcodeproj -scheme ASFW -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY='' -only-testing:ASFWTests/DeviceDiscoveryWireParsingTests -quiet`

结果：

- 失败：`DeviceDiscoveryWireParsingTests/parsesStorageDeviceKindAndUnitROMOffset`
- 失败：`DeviceDiscoveryWireParsingTests/parsesSBP2UnitMetadataEvenWhenDeviceKindIsNotStorage`
- 现象：测试进程在 `#expect(device.sbp2Units[0].managementAgentOffset == 0x80)` 附近失败
- 初步判断：测试 fixture 或解析路径仍需与当前 SBP-2 spec/swVersion 识别规则对齐，不能作为合并前绿灯

真机 UI smoke：

- 通过：ASFW app 可连接当前 active dext，并在 SBP-2 Debug 页列出 Nikon SBP-2 unit
- 通过：unit 基本字段可见：ROM offset `6`，Spec ID `0x00609E`，LUN `0x60000`，Unit Characteristics `0x000104D8`
- 阻塞：`Management_Agent_Offset` 未显示，当前为 `n/a`
- 未验证：`Create Session -> Start Login -> INQUIRY -> TUR -> REQUEST SENSE -> Raw CDB -> Release`
- 未验证原因：session 创建依赖 `Management_Agent_Offset`，当前 discovery 元数据不足

代码侧对照：

- `ControllerCore::EnableInterruptsAndStartBus()` 当前仍在 `linkEnable + BIBimageValid` 后执行显式 PHY long reset；这与研究报告指出的“初始双 bus reset”风险一致
- `BusResetCoordinator::StepComplete()` 当前只在 `previousScanHadBusyNodes_` 时延迟 discovery；普通路径没有 Apple `kScanBusDelay = 100ms` 等效等待
- `SBP2LoginSession` 和 `SBP2ManagementORB` 的 login / management ORB 提交依赖 `WriteBlock`，因此 block transaction 可靠性是 SBP-2 login 前置门槛

---

## 分阶段状态

## 阶段 0：目标与命名收口 ✅

**目标**：把工作重点明确为 scanner-first bring-up，而不是继续沿 storage 路线扩张。

### 结果

- [x] 路线图与实现目标调整为“通用 SBP-2 unit”
- [x] 明确当前阶段只做协议 bring-up，不做扫描业务层
- [x] 保留现有 `DeviceKind::Storage` 兼容逻辑，不新增 `DeviceKind::Scanner`

---

## 阶段 1：去掉 storage-only 入口限制 ✅

**目标**：让任意具备 SBP-2 unit 的设备都能进入调试链路。

### 结果

- [x] Swift discovery model 新增 `hasSBP2Unit` / `sbp2Units`
- [x] `FWUnitInfo` 新增 SBP-2 元数据字段
- [x] Device Discovery 页和 SBP-2 Debug 页都改为基于通用 SBP-2 unit 工作
- [x] 调试文案改为 `SBP-2 Device / Unit`

### 验收标准

- Swift 应用可以看到带 SBP-2 unit 的设备，而不要求它先被标记为 storage
- 调试入口不再显示 storage-only 语义

---

## 阶段 2：DriverKit 构建基线 ✅

**目标**：先让工程重新可构建，再继续做真机 bring-up。

### 结果

- [x] `SBP2CommandORB`、`SBP2ManagementORB`、`SBP2LoginSession` 统一改为兼容的延时调度封装
- [x] 规避 `DispatchAsyncAfter` 在 DriverKit 构建中的缺失问题
- [x] 完整工程已重新通过 Xcode 构建

### 验收标准

- `xcodebuild` 能完成构建
- 主机侧 ORB / session 测试仍通过

---

## 阶段 3：通用命令层与 raw CDB ✅

**目标**：把 `INQUIRY` 从专用调试路径提升为通用命令框架的一个实例。

### 结果

- [x] 新增 `SCSICommandSet` 最小抽象
- [x] `SBP2SessionRegistry` 支持通用命令提交与结果获取
- [x] 兼容保留 inquiry 专用 API
- [x] 命令结果统一输出 transport status、SBP-2 status、payload、sense
- [x] 新增 `raw CDB passthrough`

### 当前范围

- 标准 helper 当前仅覆盖 bring-up 必需的三条命令
- `READ_CAPACITY` 不属于当前扫描仪 bring-up 的 P0 范围

---

## 阶段 4：扫描仪最小调试闭环 ✅

**目标**：围绕扫描仪 bring-up 提供足够强的调试入口，而不是产品界面。

### 结果

- [x] 调试页可创建 / 登录 / 释放 session
- [x] 调试页可执行标准探测命令
- [x] 调试页可执行任意 raw CDB
- [x] 可查看命令结果、payload、sense、状态码

### 验收标准

- 软件层面已具备完整调试闭环
- 后续只差真机 smoke 与恢复硬化
- 2026-04-22 真机 UI 已确认可发现 Nikon SBP-2 unit，但因 `Management_Agent_Offset` 缺失，尚未验证 session/login/command 闭环

---

## 阶段 5：恢复与生命周期硬化 🟡

**目标**：把当前调试闭环推进到可持续真机验证的平台。

### 已完成

- [x] bus reset 时在飞命令会收敛到失败态
- [x] reconnect 链路已接入主生命周期
- [x] `ReleaseSession` 路径会清理命令状态与缓存结果

### 待完成

- [ ] 修复 Nikon SBP-2 unit discovery 中 `Management_Agent_Offset` 缺失，或确认该设备 ROM 的正确管理代理来源
- [ ] 用已知 CSR 地址验证 ASFWDriver block read/write：先测 Nikon `0xF0000400`，再用已知 FireWire 硬盘作对照
- [ ] 尝试去掉 `EnableInterruptsAndStartBus()` 中 linkEnable 后的显式 PHY long reset，并验证 Nikon management agent 是否出现
- [ ] 在 Self-ID 完成到 discovery callback 之间加入 Apple 等效 100ms scan delay，并验证 Nikon management agent 是否出现
- [ ] 评估 2 节点拓扑是否应跳过 gap count 优化，避免 ROM/management-agent bring-up 期间再次 reset
- [ ] 真机验证 bus reset 期间拒绝新命令提交
- [ ] 真机验证 reconnect 成功后可继续发命令
- [ ] 验证断开设备 / owner 释放 / 重复创建释放不会残留 DMA、地址空间或旧结果
- [ ] 收集稳定 smoke 证据：generation、loginID、target node、SBP-2 status、sense、raw CDB 往返数据

---

## 下一步执行顺序

1. **block transaction 基线验证**
   - Nikon：
     - `mcs tx read --node 0 --address 0xF0000400`
     - `mcs tx block-read --node 0 --address 0xF0000400 --length 8`
   - 如果可用，再用同一 Thunderbolt adapter 接 FireWire 硬盘重复 block-read
   - 若所有设备 block-read 都失败，优先排查 ASFWDriver async block transaction/AT descriptor/response parsing

2. **bus init 时序 A/B 测试**
   - 去掉 linkEnable 后紧接的显式 PHY long reset，只依赖 `linkEnable + BIBimageValid` 自动 reset
   - 加入 Self-ID 后 100ms discovery delay
   - 记录 Nikon 是否开始暴露 `Management_Agent_Offset` / management agent CSR

3. **真机 smoke 固化**
   - 先修复/确认 Nikon unit 的 `Management_Agent_Offset`，否则无法创建 SBP-2 session
   - 扫描仪接入后按固定顺序执行：
     - discover
     - create session
     - start login
     - inquiry
     - test unit ready
     - request sense
     - raw cdb
     - release
   - 保存日志中的 generation、loginID、transport status、SBP-2 status、sense

4. **bus reset / reconnect 验证**
   - reset 期间确认新命令被拒绝
   - in-flight 命令确认进入失败态
   - reconnect 后确认 session 可继续使用

5. **scanner-specific 命令摸底**
   - 优先通过 raw CDB 记录厂商命令与返回
   - 在拿到稳定证据前，不新增高层协议封装

6. **补强回归**
   - 按需增加 session 清理、reset 收敛、raw CDB 错误路径测试

---

## 风险与注意事项

1. **扫描仪未必遵循块设备语义**
   - 当前实现已避免把 scanner bring-up 绑定到 storage 语义，但后续仍需根据真机证据决定命令集合

2. **raw CDB 是 bring-up 必需能力**
   - 对扫描仪而言，这不是调试锦上添花，而是识别厂商协议的基础能力

3. **真机验证仍是主要缺口**
   - 当前软件路径已打通，但是否能稳定工作仍取决于硬件侧 login、命令完成与 reset 行为

4. **DriverKit 构建虽已恢复，运行时行为仍需实机确认**
   - 构建通过不等于扫描仪协议行为已稳定
