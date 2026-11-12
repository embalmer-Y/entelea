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
#include <time.h>
#include <mbedtls/net_sockets.h>
#include <entelea/d12.h>
#include <entelea/model.h>
#include <entelea/netmon.h>
#include "../https_json.h"
#include "../secure_zero.h"
#include "clock.h"
#include "https_chat.h"

static int failures;

#define CHECK(expr) do { \
	if (!(expr)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
		failures++; } \
} while (0)

static bool json_str(const char *doc, const char *key, char *out, size_t n)
{
	const char *p = https_json_member(https_json_skip_space(doc), key);

	if (p == NULL) {
		return false;
	}
	if (*p == '[') {
		p = https_json_skip_space(p + 1);
	}
	return https_json_decode_string(&p, out, n) == 0 && out[0] != '\0';
}

static bool parse_base_url(const char *url, char *host, size_t hn,
			   char *prefix, size_t pn)
{
	if (strncmp(url, "https://", 8) != 0) return false;
	const char *h = url + 8, *slash = strchr(h, '/');

	if (!slash) {
		int host_len = snprintf(host, hn, "%s", h);
		int prefix_len = snprintf(prefix, pn, "/");

		if (host_len < 0 || (size_t)host_len >= hn ||
		    prefix_len < 0 || (size_t)prefix_len >= pn) return false;
	}
	else {
		size_t hl = (size_t)(slash - h);
		if (hl >= hn) return false;
		memcpy(host, h, hl); host[hl] = '\0';
		int prefix_len = snprintf(prefix, pn, "%s", slash);

		if (prefix_len < 0 || (size_t)prefix_len >= pn) return false;
	}
	/* 去尾斜杠（路径前缀归一）。 */
	size_t l = strlen(prefix);
	if (l > 1 && prefix[l - 1] == '/') prefix[l - 1] = '\0';
	return host[0] != '\0';
}

static bool report_observation(entelea_clock_t *clock, entelea_dep_kind_t kind,
			       bool good)
{
	entelea_instant_t now;

	return entelea_clock_now(clock, &now) == ENTELEA_OK &&
	       entelea_dep_report(kind, good, now.ms) == ENTELEA_OK;
}

static bool network_observation(const char *host)
{
	mbedtls_net_context net;

	mbedtls_net_init(&net);
	int ret = mbedtls_net_connect(&net, host, "443", MBEDTLS_NET_PROTO_TCP);

	mbedtls_net_free(&net);
	return ret == 0;
}

static bool report_network_pair(entelea_clock_t *clock, const char *host)
{
	const struct timespec gap = {.tv_sec = 1, .tv_nsec = 0};

	for (int i = 0; i < 2; i++) {
		bool good = network_observation(host);

		if (!report_observation(clock, ENTELEA_DEP_NET, good) || !good) {
			return false;
		}
		if (i == 0 && nanosleep(&gap, NULL) != 0) {
			(void)entelea_dep_mark_down(ENTELEA_DEP_NET);
			return false;
		}
	}
	return true;
}

int main(int argc, char **argv)
{
	static char doc[4096];
	static entelea_https_cfg_t cfg;
	char api_key[128] = "", base_url[128] = "", model[48] = "";
	char out[4096] = "";
	int exit_status = 1;
	const char *cred_path = argc > 1 ? argv[1]
		: "/home/emb/project/.cloud-credentials.json";
	FILE *f = fopen(cred_path, "r");

	CHECK(f != NULL);
	if (f == NULL) {
		return 1;
	}
	size_t n = fread(doc, 1, sizeof(doc) - 1, f);
	bool complete = !ferror(f);

	if (n == sizeof(doc) - 1) {
		complete = complete && fgetc(f) == EOF && !ferror(f);
	}
	if (fclose(f) != 0) {
		complete = false;
	}

	doc[n] = '\0';
	CHECK(complete && n > 0 && memchr(doc, '\0', n) == NULL &&
	      https_json_valid(doc));
	if (failures != 0) {
		goto cleanup;
	}

	CHECK(json_str(doc, "llm_api_key", api_key, sizeof(api_key)));
	CHECK(json_str(doc, "llm_base_url", base_url, sizeof(base_url)));
	CHECK(json_str(doc, "allowed_models", model, sizeof(model)));
	if (failures != 0) {
		goto cleanup;
	}
	printf("[cloud] cred loaded (key=%zuB url=%zuB model=%zuB)\n",
	       strlen(api_key), strlen(base_url), strlen(model));

	int touched = 0;

	CHECK(parse_base_url(base_url, cfg.host, sizeof(cfg.host),
			     cfg.path_prefix, sizeof(cfg.path_prefix)));
	if (failures != 0) {
		goto cleanup;
	}
	snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", api_key);
	snprintf(cfg.model, sizeof(cfg.model), "%s", model);
	cfg.touched = &touched;

	static const entelea_model_transport_t ops = {
		.chat = entelea_https_chat,
	};
	static entelea_clock_t d12_clock;

	/* RF1 G5：model_ask 逐尝试过 D12 闸（r16 起依赖——未绑时钟=闭锁）。 */
	CHECK(entelea_platform_clock_init(&d12_clock) == ENTELEA_OK);
	CHECK(entelea_d12_bind_clock(&d12_clock) == ENTELEA_OK);
	entelea_model_bind(&ops, &cfg);
	if (failures != 0 || !report_network_pair(&d12_clock, cfg.host)) {
		fprintf(stderr, "[cloud] network observation failed\n");
		goto cleanup;
	}

	/* 两次恢复探测加一次普通调用；实际费用以累计用量及调用次数记录。 */
	const entelea_model_msg_t msgs[] = {
		{ .role = "user",
		  .content = "Reply with exactly the word: ok. "
			     "Output nothing else." },
	};
	entelea_model_usage_t usage = { 0 };
	entelea_result_t r;

	/* 两次真实、逐次过闸的探测才产生两条 MODEL 恢复观测。 */
	for (int i = 0; i < 2; i++) {
		int before = touched;

		r = entelea_model_probe(msgs, 1, out, sizeof(out), 30000, &usage);
		if (touched == before || r != ENTELEA_OK || out[0] == '\0' ||
		    !report_observation(&d12_clock, ENTELEA_DEP_MODEL, true)) {
			(void)entelea_dep_mark_down(ENTELEA_DEP_MODEL);
			fprintf(stderr, "[cloud] model recovery probe failed rc=%d\n",
				(int)r);
			goto cleanup;
		}
	}
	CHECK(entelea_model_ready());
	if (failures != 0) {
		goto cleanup;
	}
	r = entelea_model_ask(msgs, 1, out, sizeof(out), 30000, &usage);

	printf("[cloud] ask rc=%d out='%.64s' usage=%u+%u touched=%d\n",
	       (int)r, out, usage.prompt_tokens, usage.completion_tokens,
	       touched);
	CHECK(r == ENTELEA_OK);
	CHECK(out[0] != '\0'); /* 非空 content（字面不设硬断——模型面） */
	CHECK(touched >= 3);
	CHECK(entelea_d12_model_calls() == (uint32_t)touched); /* 逐尝试过闸 */
	CHECK(usage.prompt_tokens > 0);

	/* 模拟失联对照：NET DOWN → ask 拒 E_STATE 且不触传输。 */
	int before = touched;
	CHECK(report_observation(&d12_clock, ENTELEA_DEP_NET, false));
	CHECK(!entelea_model_ready());
	r = entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL);
	CHECK(r == ENTELEA_E_STATE);
	CHECK(touched == before);

	/* 恢复面：两次实际网络观察。 */
	CHECK(report_network_pair(&d12_clock, cfg.host));
	CHECK(entelea_model_ready());

	if (failures == 0) {
		printf("HOST CLOUD PASS (linux https transport)\n");
		printf("USAGE_TOKENS %u %u\n", usage.prompt_tokens,
		       usage.completion_tokens);
		exit_status = 0;
	} else {
		printf("HOST CLOUD FAIL (%d)\n", failures);
	}
cleanup:
	entelea_model_bind(NULL, NULL);
	entelea_secure_zero(doc, sizeof(doc));
	entelea_secure_zero(api_key, sizeof(api_key));
	entelea_secure_zero(base_url, sizeof(base_url));
	entelea_secure_zero(model, sizeof(model));
	entelea_secure_zero(out, sizeof(out));
	entelea_secure_zero(&cfg, sizeof(cfg));
	return exit_status;
}
