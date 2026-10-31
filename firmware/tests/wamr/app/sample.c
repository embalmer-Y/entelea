/* SPDX-License-Identifier: Apache-2.0 */
/* 样例 APP（M2b.2a 环境批）：最小自由固件 wasm——无标准库、无 WASI、零导入
 * （DEC-25 执行模式边界的活体证明：实例化成功即证明导入面为空）。
 * 导出与 manifest V1 约定对齐：health_ping 必选（TsapManifest v1 exports）；
 * on_input = 输入事件处理雏形（确定性纯函数，合同 9）。
 * 构建：./build.sh（clang --target=wasm32）→ sample.wasm（测试夹具，随源提交）。 */
__attribute__((export_name("health_ping")))
int health_ping(void)
{
	return 1;
}

__attribute__((export_name("on_input")))
int on_input(int v)
{
	return v * 2 + 1; /* 确定性：无随机/墙钟/环境依赖 */
}
