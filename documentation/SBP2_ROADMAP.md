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
  - 代码侧已修复 `Management_Agent_Offset` 解析与相关 bus init/reset 时序；下一步需要重新安装 dext 并在真机确认该 unit 是否开始显示有效 `Mgmt Agent`
- bus reset / reconnect 硬化
- in-flight 命令失败收敛与资源清理验证

### 未完成

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

- `./build/tests_build/ASFWConfigROMTests '--gtest_filter=-LinuxReferenceData/ConfigROMReferenceCrcTests.*' --gtest_brief=1`
- `./build/tests_build/BusManagerGapOptimizationTests --gtest_brief=1`
- `./build/tests_build/BusResetCoordinatorTests --gtest_brief=1`
- `./build/tests_build/SBP2LoginSessionTests --gtest_brief=1`
- `./build/tests_build/SBP2ORBTests --gtest_brief=1`
- `./build/tests_build/SBP2SessionRegistryTests --gtest_brief=1`
- `./build/tests_build/ASFWPacketTests --gtest_brief=1`
- `xcodebuild test -project ASFW.xcodeproj -scheme ASFW -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY='' -only-testing:ASFWTests/DeviceDiscoveryWireParsingTests -quiet`
- `xcodebuild build -project ASFW.xcodeproj -scheme ASFW -destination 'platform=macOS' CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY='' -quiet`

结果：

- 已修复 SBP-2 ROM key 解码：`keyType=CSR offset + keyId=0x14`（combined key `0x54`）现在解析为 `Management_Agent_Offset`；`immediate + 0x14` 继续解析为 LUN
- 已补主机测试覆盖 Nikon-like entry：`0x5400C000 -> managementAgentOffset=0x00C000`
- 已修复 Swift discovery fixture：`specId=0x00609E`、`swVersion=0x010483`
- 已去掉 `EnableInterruptsAndStartBus()` 中 `linkEnable + BIBimageValid` 后的显式 PHY long reset
- 已在 `BusResetCoordinator::StepComplete()` 中加入最小 `100ms` discovery delay
- 已对 2 节点 `local=root` 拓扑跳过普通 `TargetGap` 优化，避免无意义 gap retool/reset
- host / Swift / 工程构建验证通过；当前真机已确认 discovery 侧修复生效，剩余缺口收敛到 block transaction / management agent CSR / session login

真机 smoke（2026-04-22）：

- 通过：`install-debug-asfw.sh --refresh` 已将 active dext 切换到新 build；本轮验证 active hash=`81b195440272b2bec0ee5e96ea73520dd040604120043b8f433e23c199bbad19`
- 通过：新 dext 初始上电后总线为空；执行 `mcs-cli diag bus-reset` 后 Nikon 节点出现，`generation=2`，`Local/Root/IRM=1/1/1`，`cycleMaster=1`
- 通过：`mcs-cli info --node 0 --verbose` 现在明确显示 `ManagementAgent csr_offset=0x00c000`，并在 unit directory 中识别出 `key=management_agent (0x14) type=csr_offset value=0x00c000`
- 通过：Nikon node 信息稳定可见：`guid=0x0090b54001ffffff`、`specId=0x00609e`、`swVersion=0x010483`、`logicalUnit=0x060000`
- 失败：Config ROM 基线里 `mcs-cli tx read --node 0 --addr 0xf0000400 --len 4` 返回 `00 00 00 00`，而 `--len 8` 失败为 `asyncBlockRead status=5`；从 dext 日志可见其真实响应为 `rCode=0x07 (AddressError)`
- 失败：management agent CSR 直读不通；`mcs-cli tx read --node 0 --addr 0xf0030000 --len 4` 当前返回 `asyncRead status=5`，从 dext 日志可见真实响应为 `rCode=0x06 (TypeError)`；`--len 8` 仍失败
- 失败：`mcs-cli sbp2 probe --node 0` 已能算出 `csr_addr=0xf0030000`，但 `MANAGEMENT_AGENT` 读取失败；`STATE_CLEAR/STATE_SET/NODE_IDS` 也返回 `asyncRead status=1`
- 失败：`mcs-cli sbp2 login --node 0` 超时于 `sbp2 status timeout after 100 polls`
- 失败：`mcs-cli sbp2 inquiry --node 0` 在地址空间分配阶段失败，错误为 `allocateAddressRange failed: 0xe00002db`（`kIOReturnNoSpace`）
- 未完成：`TEST UNIT READY` / `REQUEST SENSE` / `Raw CDB` / `Release` 无法继续，因为 login 没有成功建立 session
- 通过：已修复 user client `Stop/free` 路径未释放 owner 绑定 SBP-2 资源的问题；真机上用“启动 `mcs-cli sbp2 login` 后半路杀进程，再立即重试同命令”的方式回归，第二次不再命中 `allocateAddressRange failed: 0xe00002db`，而是继续进入 `sbp2 status timeout after 100 polls`

离线协议排查（2026-04-23）：

- 已对照 Apple `IOFireWireSBP2Login` / `IOFireWireSBP2ManagementORB` 确认：SBP-2 ORB 内嵌 bus address 的 node 字段应使用完整 16-bit local-bus node id，而不是仅 6-bit 物理 node id
- 已修复 ASFW 当前实现中 `SBP2LoginSession`、`SBP2ManagementORB`、`SBP2CommandORB`、`SBP2PageTable` 对该字段的编码，统一改为 Apple 等效的 `0xffc0 | localPhyId`
- 已新增 host tests 覆盖 login ORB、management ORB、command ORB direct descriptor 的 node 编码
- 当前待真机确认：此前 Nikon “反复读取 login ORB 但从不写 login response/status，最终 `sbp2 status timeout after 100 polls`” 是否就是由这个 node 编码错误触发

代码侧对照：

- `ControllerCore::EnableInterruptsAndStartBus()` 已改为只依赖 `linkEnable + BIBimageValid` 自动 reset，不再追加显式 PHY long reset
- `BusResetCoordinator::StepComplete()` 已对 discovery callback 统一加入最小 `100ms` delay；busy-node 路径取 `max(100ms, currentDiscoveryDelayMs_)`
- `BusManager::EvaluateGapPolicy()` 已对 2 节点 `local=root` 拓扑跳过普通 `TargetGap` 优化，避免无意义 retool/reset
- `SBP2LoginSession` 和 `SBP2ManagementORB` 的 login / management ORB 提交依赖 `WriteBlock`，因此 block transaction 可靠性是 SBP-2 login 前置门槛
- `SBP2LoginSession` / `SBP2ManagementORB` / `SBP2CommandORB` / `SBP2PageTable` 已统一使用完整 16-bit local-bus node id 来编码 ORB 中的 response/status/data bus address

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
- 2026-04-22 真机 UI 已确认可发现 Nikon SBP-2 unit；当前待验证的是包含 parser/timing 修复的新 dext 是否已解锁 `Management_Agent_Offset` 与 session/login/command 闭环

---

## 阶段 5：恢复与生命周期硬化 🟡

**目标**：把当前调试闭环推进到可持续真机验证的平台。

### 已完成

- [x] bus reset 时在飞命令会收敛到失败态
- [x] reconnect 链路已接入主生命周期
- [x] `ReleaseSession` 路径会清理命令状态与缓存结果

### 待完成

- [x] 修复 Nikon SBP-2 unit discovery 中 `Management_Agent_Offset` 解析路径（combined key `0x54`）
- [ ] 用已知 CSR 地址验证 ASFWDriver block read/write：Nikon `0xF0000400` 上 quadlet read 可返回 4 bytes，但 block-read 失败为 `status=5`；仍需 FireWire 硬盘对照
- [x] 去掉 `EnableInterruptsAndStartBus()` 中 linkEnable 后的显式 PHY long reset（代码已改，待真机确认效果）
- [x] 在 Self-ID 完成到 discovery callback 之间加入 Apple 等效 100ms scan delay（代码已改，待真机确认效果）
- [x] 对 2 节点 `local=root` 拓扑跳过普通 gap count 优化（代码已改，待真机确认效果）
- [x] 真机确认 Nikon unit 开始暴露 `Management_Agent_Offset`
- [ ] 真机确认 management agent CSR (`0xF0030000`) 可读并可用于 session/login
- [ ] 真机确认 full-node-id ORB 修复后，Nikon 会开始向 login response / status FIFO 地址写回数据
- [ ] 真机验证 bus reset 期间拒绝新命令提交
- [ ] 真机验证 reconnect 成功后可继续发命令
- [~] 验证断开设备 / owner 释放 / 重复创建释放不会残留 DMA、地址空间或旧结果
  - 已确认 user client 异常退出后不会再遗留固定地址分配冲突
  - 仍需覆盖断开设备、bus reset 与旧 transaction result 清理
- [ ] 收集稳定 smoke 证据：当前仅有 `generation=2`、`target node=0`；`loginID` / `SBP-2 status` / `sense` / `raw CDB` 仍被 login 前阻塞

---

## 下一步执行顺序

1. **重装包含 full-node-id ORB 修复的新 dext，并优先复测 login**
   - 固定顺序：
     - 重新安装 / 激活最新 dext
     - `mcs-cli diag bus-reset`
     - `mcs-cli list`
     - `mcs-cli sbp2 login --node 0`
   - 同步抓 dext 日志，重点确认：
     - Nikon 是否仍只反复读取 login ORB
     - 是否开始写 `login response` / `status FIFO`
     - `sbp2 status timeout after 100 polls` 是否消失或转化为新的更靠后的失败点

2. **若 login 仍失败，再继续收敛 async block transaction / management agent CSR 读路径**
   - 已确认 discovery 已给出 `Management_Agent_Offset=0x00c000 -> csr_addr=0xF0030000`
   - 已从 dext 日志确认：`0xF0000400` block read 的真实响应是 `rCode=0x07 (AddressError)`
   - 已从 dext 日志确认：`0xF0030000` quadlet read 的真实响应是 `rCode=0x06 (TypeError)`
   - 下一步优先对照 Apple / Linux 行为判断：这些 rCode 是目标设备的合法拒绝，还是 ASFW block request 线上的格式/时序问题

3. **补 FireWire 硬盘对照，区分“全局 block-read 坏”还是“Nikon 特有”**
   - 在同一 Thunderbolt adapter 上对已知 FireWire 硬盘重复：
     - `mcs tx read --node <disk_node> --address 0xF0000400`
     - `mcs tx block-read --node <disk_node> --address 0xF0000400 --length 8`
   - 若硬盘也失败，优先排查 ASFWDriver async block transaction / AT descriptor / response parsing
   - 若仅 Nikon 失败，优先继续 bus timing / 设备初始化路径

4. **继续补 owner 生命周期与清理回归**
   - `mcs-cli sbp2 inquiry --node 0` 之前出现的 `allocateAddressRange failed: 0xe00002db`，已定位并修复为 user client `Stop/free` 未释放 owner 绑定资源
   - 仍需补更系统的断连 / reset / 重复 create-release 回归，确认不会残留 DMA、地址空间或旧结果

5. **在 login 真正跑通后重新执行完整 SBP-2 smoke**
   - 固定顺序：
     - discover
     - create session
     - start login
     - inquiry
     - test unit ready
     - request sense
     - raw cdb
     - release
   - 保存 generation、loginID、transport status、SBP-2 status、sense、raw CDB payload

6. **bus reset / reconnect 验证**
   - reset 期间确认新命令被拒绝
   - in-flight 命令确认进入失败态
   - reconnect 后确认 session 可继续使用

7. **补强回归**
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
