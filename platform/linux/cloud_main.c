/* SPDX-License-Identifier: Apache-2.0 */
/* F-2 真云闸（Linux host 面）：model transport 真实现绑定 → 依赖
 * 联动 UP → 真调用 router（TLS+chat）→ content/usage 断言 →
 * 失联对照（NET DOWN → ask 拒 E_STATE 且**不触传输**——touched
 * 计数证明）→ 恢复面。凭据经 ENTELEA_CLOUD_CRED（0600 json，仓库外）
 * 运行时读取——不入二进制不入日志（打印仅键名与长度）。
 * C5 阻塞的 MCU 面见 r06-validation F 批节（到场项）。 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <entelea/model.h>
#include <entelea/netmon.h>
#include "https_chat.h"

static int failures;

#define CHECK(expr) do { \
	if (!(expr)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
		failures++; } \
} while (0)

/* 最小 JSON 字段提取（"key":"value" / "key":[.. 第一个字符串 ..]）。 */
static bool json_str(const char *doc, const char *key, char *out, size_t n)
{
	char pat[32];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(doc, pat);
	if (!p) return false;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t') p++;
	if (*p == '"') { p++; size_t o = 0;
		while (*p && *p != '"' && o + 1 < n) out[o++] = *p++;
		out[o] = '\0'; return o > 0; }
	if (*p == '[') {
		const char *close = strchr(p, ']'); /* P3-2：限定元素域内 */

		if (!close) {
			return false;
		}
		p++;
		while (p < close && *p != '"') {
			p++;
		}
		if (p >= close) {
			return false; /* 空数组/首元素非串 */
		}
		p++;
		size_t o = 0;
		while (*p && *p != '"' && o + 1 < n) {
			out[o++] = *p++;
		}
		out[o] = '\0';
		return o > 0;
	}
	return false;
}

static bool parse_base_url(const char *url, char *host, size_t hn,
			   char *prefix, size_t pn)
{
	if (strncmp(url, "https://", 8) != 0) return false;
	const char *h = url + 8, *slash = strchr(h, '/');

	if (!slash) { snprintf(host, hn, "%s", h); snprintf(prefix, pn, "/"); }
	else {
		size_t hl = (size_t)(slash - h);
		if (hl >= hn) return false;
		memcpy(host, h, hl); host[hl] = '\0';
		snprintf(prefix, pn, "%s", slash);
	}
	/* 去尾斜杠（路径前缀归一）。 */
	size_t l = strlen(prefix);
	if (l > 1 && prefix[l - 1] == '/') prefix[l - 1] = '\0';
	return host[0] != '\0';
}

int main(int argc, char **argv)
{
	const char *cred_path = argc > 1 ? argv[1]
		: "/home/emb/project/.cloud-credentials.json";
	FILE *f = fopen(cred_path, "r");

	CHECK(f != NULL);
	static char doc[4096];
	size_t n = f ? fread(doc, 1, sizeof(doc) - 1, f) : 0;

	doc[n] = '\0';
	if (f) fclose(f);
	CHECK(n > 0);

	char api_key[128] = "", base_url[128] = "", model[48] = "";

	CHECK(json_str(doc, "llm_api_key", api_key, sizeof(api_key)));
	CHECK(json_str(doc, "llm_base_url", base_url, sizeof(base_url)));
	CHECK(json_str(doc, "allowed_models", model, sizeof(model)));
	printf("[cloud] cred loaded (key=%zuB url=%s model=%s)\n",
	       strlen(api_key), base_url, model);

	static entelea_https_cfg_t cfg;
	int touched = 0;

	CHECK(parse_base_url(base_url, cfg.host, sizeof(cfg.host),
			     cfg.path_prefix, sizeof(cfg.path_prefix)));
	snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", api_key);
	snprintf(cfg.model, sizeof(cfg.model), "%s", model);
	cfg.touched = &touched;

	static const entelea_model_transport_t ops = {
		.chat = entelea_https_chat,
	};
	entelea_model_bind(&ops, &cfg);

	/* 依赖联动：NET/MODEL 双 UP（防抖 ×2）→ ready。 */
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	(void)entelea_dep_report(ENTELEA_DEP_MODEL, true, 0);
	(void)entelea_dep_report(ENTELEA_DEP_MODEL, true, 0);
	CHECK(entelea_model_ready());

	/* 真调用（1 次——额度纪律）。prompt 强化从命性；断言面=非空
	 * content+usage（模型输出字面不受控——review 后如实）。 */
	const entelea_model_msg_t msgs[] = {
		{ .role = "user",
		  .content = "Reply with exactly the word: ok. "
			     "Output nothing else." },
	};
	char out[4096] = "";
	entelea_model_usage_t usage = { 0 };
	entelea_result_t r = entelea_model_ask(msgs, 1, out, sizeof(out),
					       30000, &usage);

	printf("[cloud] ask rc=%d out='%.64s' usage=%u+%u touched=%d\n",
	       (int)r, out, usage.prompt_tokens, usage.completion_tokens,
	       touched);
	CHECK(r == ENTELEA_OK);
	CHECK(out[0] != '\0'); /* 非空 content（字面不设硬断——模型面） */
	CHECK(touched == 1);
	CHECK(usage.prompt_tokens > 0);

	/* 失联对照：NET DOWN → ask 拒 E_STATE 且不触传输。 */
	int before = touched;
	(void)entelea_dep_report(ENTELEA_DEP_NET, false, 0);
	CHECK(!entelea_model_ready());
	r = entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL);
	CHECK(r == ENTELEA_E_STATE);
	CHECK(touched == before);

	/* 恢复面：双 good 防抖回 UP。 */
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	CHECK(!entelea_model_ready()); /* 1 次 good=DEGRADED（中间态可见） */
	(void)entelea_dep_report(ENTELEA_DEP_NET, true, 0);
	CHECK(entelea_model_ready());

	if (failures == 0) {
		printf("HOST CLOUD PASS (linux https transport)\n");
		printf("USAGE_TOKENS %u %u\n", usage.prompt_tokens,
		       usage.completion_tokens);
		return 0;
	}
	printf("HOST CLOUD FAIL (%d)\n", failures);
	return 1;
}
