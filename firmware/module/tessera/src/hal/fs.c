/* SPDX-License-Identifier: Apache-2.0 */
/* ts-fs 能力面（DEC-47④⑤，MD1.2e）：无句柄文件 API + fs_paths 前缀白名单。
 * 双层裁决（均 fail-closed + PERM_DENIED 留痕）：
 *   ① class/op 位图（"fs:read:0" 等——inst 占位 0）；
 *   ② 路径前缀白名单（manifest fs_paths → ts_fs_paths_bind 绑定到 ctx；
 *      前缀匹配边界 = 前缀后须 '/' 或恰好等长——防 "/SD:/app" 授权
 *      "/SD:/app-secret" 类前缀绕过）。
 * 后端 = Zephyr fs API（V1 = 已挂载 FAT；未挂载 → TS_E_IO 如实）。
 * 写走审计：printk 留痕（app 归因）——安全审计环属输出通道面（合同 2），
 * fs 写不属输出通道，ring 扩展随真实消费需求再裁（登记待办）。 */
#include <string.h>
#include <ts/core.h>
#include <ts/hal.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* fs_paths 白名单（V1 单活跃 APP = 单条目存储；CSV 覆盖式） */
static char bound_paths[CONFIG_TS_HAL_FS_PATHS_MAX];
static uint16_t bound_app = 0xFFFF; /* 0xFFFF = 未绑定（V1 单活跃：app_id 匹配即授权域） */

ts_res_t ts_fs_paths_bind(uint16_t app_id, const char *csv)
{
	if (csv == NULL || csv[0] == '\0') {
		return TS_E_PARAM;
	}
	size_t n = strlen(csv);

	if (n >= sizeof(bound_paths)) {
		return TS_E_PARAM;
	}
	memcpy(bound_paths, csv, n + 1);
	bound_app = app_id;
	return TS_OK;
}

ts_res_t ts_fs_paths_bind_ctx(ts_ctx_t c, const char *csv)
{
	return ts_fs_paths_bind(c.app_id, csv);
}

/* 前缀匹配（边界纪律：前缀后须 '/' 或恰好等长——防前缀绕过）。
 * CG5-B 提取为公共助手：APP 面（ts_fs_path_allowed）与宿主资源推送面
 * （appmgr res registry 消费）同一语义——单一事实源。 */
bool ts_fs_prefix_match(const char *csv, const char *path)
{
	if (csv == NULL || path == NULL || path[0] != '/') {
		return false;
	}
	const char *p = csv;

	while (*p != '\0') {
		const char *seg = strchr(p, ';');
		size_t sl = (seg != NULL) ? (size_t)(seg - p) : strlen(p);

		if (sl > 0 && strncmp(path, p, sl) == 0 &&
		    (path[sl] == '\0' || path[sl] == '/')) {
			return true;
		}
		if (seg == NULL) {
			break;
		}
		p = seg + 1;
	}
	return false;
}

ts_res_t ts_fs_path_allowed(ts_ctx_t c, const char *path)
{
	if (path == NULL || path[0] != '/') {
		return TS_E_PARAM;
	}
	if (bound_app == 0xFFFF || c.app_id != bound_app || bound_paths[0] == '\0') {
		return TS_E_PERM; /* 未绑定 = 拒绝（fail-closed） */
	}
	return ts_fs_prefix_match(bound_paths, path) ? TS_OK : TS_E_PERM;
}

/* 双层裁决共用入口 */
static ts_res_t fs_gate(ts_ctx_t c, ts_perm_op_t op, const char *path)
{
	ts_res_t r = ts_perm_check(c, TS_PERM_CLASS_FS, op, 0);

	if (r != TS_OK) {
		return r;
	}
	return ts_fs_path_allowed(c, path);
}

ts_res_t ts_fs_list(ts_ctx_t c, const char *dir, char *out, uint16_t cap, uint16_t *out_len)
{
	if (dir == NULL || out == NULL || out_len == NULL || cap == 0) {
		return TS_E_PARAM;
	}
	ts_res_t g = fs_gate(c, TS_PERM_OP_LIST, dir);

	if (g != TS_OK) {
		return g;
	}
	struct fs_dir_t d;

	fs_dir_t_init(&d);
	if (fs_opendir(&d, dir) != 0) {
		return TS_E_IO;
	}
	uint16_t n = 0;
	struct fs_dirent ent;

	while (fs_readdir(&d, &ent) == 0 && ent.name[0] != '\0') {
		size_t nl = strlen(ent.name);

		if (n > 0 && n + 1 < cap) {
			out[n++] = ';';
		}
		for (size_t i = 0; i < nl; i++) {
			if (n >= cap - 1) {
				break; /* 截断（调用方按容量取用；如需续读缩范围） */
			}
			out[n++] = ent.name[i];
		}
		if (n >= cap - 1) {
			break;
		}
	}
	out[n] = '\0';
	*out_len = n;
	fs_closedir(&d);
	return TS_OK;
}

ts_res_t ts_fs_read(ts_ctx_t c, const char *path, uint32_t off, uint8_t *buf, uint16_t *len)
{
	if (path == NULL || buf == NULL || len == NULL || *len == 0) {
		return TS_E_PARAM;
	}
	ts_res_t g = fs_gate(c, TS_PERM_OP_READ, path);

	if (g != TS_OK) {
		return g;
	}
	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return TS_E_IO;
	}
	if (off != 0 && fs_seek(&f, off, FS_SEEK_SET) != 0) {
		fs_close(&f);
		return TS_E_IO;
	}
	ssize_t r = fs_read(&f, buf, *len);

	fs_close(&f);
	if (r < 0) {
		return TS_E_IO;
	}
	*len = (uint16_t)r;
	return TS_OK;
}

ts_res_t ts_fs_write(ts_ctx_t c, const char *path, uint32_t off, const uint8_t *data, uint16_t len)
{
	if (path == NULL || (data == NULL && len > 0)) {
		return TS_E_PARAM;
	}
	ts_res_t g = fs_gate(c, TS_PERM_OP_WRITE, path);

	if (g != TS_OK) {
		return g;
	}
	return ts_fs_host_write(path, off, data, len);
}

/* ---- 宿主原语（CG5-B · Q-39③ 方案 A）--------------------------------------
 * sys/fs-* 命令面的后端直达通道：**无 APP 权限门**（宿主面授权 = appmgr
 * res registry〔随包 needs.resources 前缀白名单〕+ 租约，在 net cmd 层）；
 * 后端语义与 APP 面完全同一（部分写 = 失败；未挂载 = TS_E_IO 如实）。
 * printk 留痕同型（host 归因）。 */
ts_res_t ts_fs_host_write(const char *path, uint32_t off, const uint8_t *data, uint16_t len)
{
	if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL ||
	    (data == NULL && len > 0)) {
		return TS_E_PARAM;
	}
	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_CREATE | FS_O_WRITE) != 0) {
		return TS_E_IO;
	}
	if (off != 0 && fs_seek(&f, off, FS_SEEK_SET) != 0) {
		fs_close(&f);
		return TS_E_IO;
	}
	ssize_t r = fs_write(&f, data, len);

	fs_close(&f);
	if (r >= 0 && r != (ssize_t)len) {
		return TS_E_IO; /* 部分写 = 失败（无句柄面不做续写） */
	}
	printk("[fs] host write path=%s len=%u off=%u\n", path, (unsigned)len, (unsigned)off);
	return (r < 0) ? TS_E_IO : TS_OK;
}

ts_res_t ts_fs_host_read(const char *path, uint32_t off, uint8_t *buf, uint16_t *len)
{
	if (path == NULL || buf == NULL || len == NULL || *len == 0) {
		return TS_E_PARAM;
	}
	struct fs_file_t f;

	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ) != 0) {
		return TS_E_IO;
	}
	if (off != 0 && fs_seek(&f, off, FS_SEEK_SET) != 0) {
		fs_close(&f);
		return TS_E_IO;
	}
	ssize_t r = fs_read(&f, buf, *len);

	fs_close(&f);
	if (r < 0) {
		return TS_E_IO;
	}
	*len = (uint16_t)r;
	return TS_OK;
}

ts_res_t ts_fs_host_delete(const char *path)
{
	if (path == NULL) {
		return TS_E_PARAM;
	}
	int r = fs_unlink(path);

	return (r == 0) ? TS_OK : TS_E_IO;
}

ts_res_t ts_fs_delete(ts_ctx_t c, const char *path)
{
	if (path == NULL) {
		return TS_E_PARAM;
	}
	ts_res_t g = fs_gate(c, TS_PERM_OP_DELETE, path);

	if (g != TS_OK) {
		return g;
	}
	int r = fs_unlink(path);

	if (r == -ENOENT) {
		return TS_E_NOTFOUND;
	}
	printk("[fs] delete app=%u path=%s\n", (unsigned)c.app_id, path);
	return (r == 0) ? TS_OK : TS_E_IO;
}