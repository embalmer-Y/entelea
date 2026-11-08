# R01 公共机制合同

这里只定义实际公共底座；不实现任务、授权、包验签或语言运行时。
方向评审及未决事项以[执行规划](../../docs/rebuild-plan.md)第7节为准。

## 类型、身份和时间

- `entelea_result_t` 为 int32；0成功，-1至-12错误，-13为显式pending，
  不得以“所有负数失败”解释。具体API仅在返回OK后使用其输出。
- `source.id` 是非零来源标识，不是权限。模型/推导不能冒充观测；
  原始来源链由上层保存。principal不透明且只由可信宿主构造，
  Wasm/Lua都不能将输入整数转换为可信principal。
- 单调instant包括非零clock_id和valid；跨时基不能比较。时基ID由平台/
  宿主分配，跨重启重新分配或拒绝使用旧截止时间。墙钟使用独立数据类型。
- clock每实例注入同步read_ms，调用者串行访问；不创建锁/全局服务。
  底层必须只在OK时给出有效值，pending也不代表读到时间。
  检测同实例时钟回退，不更新上一有效值；错误返回且out保持不变。
- deadline使用绝对毫秒，同一时基；加法溢出拒绝，now>=deadline表示到期。
  0毫秒合法；valid=false与0不同。无绑定报UNINIT，不伪造默认时间。

## App接入边界（非运行时实现）

Wasm/Lua是显式载荷种类；识别种类不等于支持装载，旧TSAP线格式未改。
payload_version由对应格式/后端验证，不能用文件后缀推导权限。
budget声明内存、执行额度/单位、同源截止时间，启用前须由后端确认
能强制执行；额度须非零且有界，上层负责检查，不因结构体存在自动批准。
runtime_steps在不同VM间不可等价比较，CPU_US也不等于墙钟；
Lua计量/有界停止的实测在R05，不假设其与Wasm指令计量同实现。
deadline不能替代执行预算；R01不提供线程/解释器中断机制。
权限通过同一可信principal调用执行门，由R04实现；非受信请求无权构造它。

receipt区分accepted/终态及none/confirmed/unknown效果。
cancelled不保证effect=none，unknown不能自动重试非幂等动作。
R01只定义表示，不运行任务状态机，也不声称已实现权限隔离。
内存对象不是线协议；不直接序列化C enum/pointer/sizeof结构体。

## CBOR

无堆分配；definite map<=16/array<=64读取，字符串借用/复制及整型/bool。
编码整数采用最短头部；map键排序由消费方完成，不宣称自动canonical map。
接收非最短整数编码与历史一致；非确定性/严格签名schema需上层另外验证。
skip最多4层，字符串<=4096，接受整数、字符串、map/array及bool/null；
拒绝tag、浮点、其他simple、不定长/保留头。tstr不验证UTF-8、
不验证map重复键；完整schema/签名验包仍在R05。
一个读取操作失败时游标/输出保持不变；容器open只读取头，
容器完整性由遍历/skip或schema消费验证，调用者最终检查rem。
写失败保持pos和buffer不变；容量判断用减法防溢出。
调用者必须给真实可读/可写长度，输出不能与输入或pos/r/out互相重叠。
空字符串允许NULL且长度0；bstr引用只在原输入buffer活着时有效。

## SHA-256

沿用固定A-SHA算法和FIPS180-4常量；init/update/final有显式结果。
长度为size_t，不再一次性截为uint32_t；总字节数必须<=UINT64_MAX/8，
超界在读入数据前拒绝。NULL仅在输入长度0时允许。
init后串行使用；final消耗上下文，后续update/final报错，init重新开始。
新接口失败不修改上下文/输出；调用者保证上下文已初始化且未篡改，
输入不与ctx/out重叠。SHA不是验签，不能独自证明包可信。

## 旧子集兼容与明确行为调整

compat提供旧CBOR/SHA真实ts_*链接符号、原参数/布局和err.h数值；
旧消费方需显式改为include ts/common.h，不复制业务internal.h。
主公共库不链接compat；compat单独目标仅为迁移测试/旧调用者使用。
非整个历史ABI证明：不同目标平台布局重新测，不创建旧core.h/appmgr.h替身。
旧void API不能返回失败；非法输入/超出合同/已final上下文明确报错并abort，
不静默成功。正式设备应使用可返回错误的新API，适用平台panic映射留R02。

合法支持输入编码/摘要不变；以下调整同时影响迁移子集，必须记录：
CBOR signed读取拒绝>INT64_MAX的正数（原来窄转）；skip按声明子集拒绝浮点/
其他simple（原代码注释与实现不一致）；失败不再推进游标或留下部分写；
sha拒绝长度溢出/重复final，一次性不截断。旧TSAP格式不因这些修正改变。
