/* SPDX-License-Identifier: Apache-2.0 */
/* F-2 模型传输真实现（Linux host 面）：TLS（mbedtls 4.1，pin 同源
 * dependencies.json）+ HTTP/1.1 POST chat/completions + 值感知最小
 * JSON 提取（模型可在 reasoning 文本写字面 "content":——纯 strstr 会
 * 命中字符串值内部；状态机跟踪引号配对）。框架纪律：platform 装配面
 * （框架不含网络栈/TLS）；MCU 面 BLOCKED 于 C5（r06 F 批节）。凭据
 * 经调用方注入，不落盘不打日志。 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <entelea/model.h>
#include <entelea/result.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/psa_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include "https_chat.h"

#define HTTPS_PORT "443"
#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

/* 转义+控制字符面：false=超界（调用方显式拒）。 */
static bool json_escape(const char *in, char *out, size_t out_max)
{
	size_t o = 0;

	for (size_t i = 0; in[i] != '\0'; i++) {
		char c = in[i];
		size_t need = (c == '"' || c == '\\' || c < 0x20) ? 2 : 1;

		if (o + need + 1 >= out_max) {
			return false;
		}
		if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = c;
		} else if (c == '\n') {
			out[o++] = '\\';
			out[o++] = 'n';
		} else if (c == '\r') {
			out[o++] = '\\';
			out[o++] = 'r';
		} else if (c == '\t') {
			out[o++] = '\\';
			out[o++] = 't';
		} else if (c < 0x20) {
			out[o++] = ' ';
		} else {
			out[o++] = c;
		}
	}
	out[o] = '\0';
	return true;
}

/* 值感知键查找：JSON 键=字符串（引号段）+紧跟冒号。识别方式：扫描
 * 时跟踪字符串区（值内的字面 "key": 不会成为键——它不出现在"引号
 * 段后跟冒号"的结构位）；字符串段闭合处若后随 ':' 且段内文本==key
 * 则命中（返回冒号后位置）。 */
static const char *find_key(const char *body, const char *key)
{
	size_t klen = strlen(key);

	for (const char *p = body; *p != '\0'; p++) {
		if (*p != '"') {
			continue; /* 非串起点：跳过（结构字符/空白/数字） */
		}
		/* p=段起点：截取段内（处理转义）。 */
		const char *q = p + 1;
		bool esc = false;

		while (*q != '\0' && (esc || *q != '"')) {
			if (esc) {
				esc = false;
			} else if (*q == '\\') {
				esc = true;
			}
			q++;
		}
		if (*q == '\0') {
			return NULL; /* 未闭合（截断 body） */
		}
		/* 段 [p+1, q)：长度==klen 且字面相等且后随 ':' → 键。 */
		if ((size_t)(q - p - 1) == klen &&
		    strncmp(p + 1, key, klen) == 0) {
			const char *c = q + 1;

			while (*c == ' ' || *c == '\t') {
				c++;
			}
			if (*c == ':') {
				return c + 1;
			}
		}
		p = q; /* 跳过该串（值内字面不参与结构位匹配） */
	}
	return NULL;
}

/* 提取 quoted 值直写 out（out_max 界）。0=OK；-1 未找到/非串；
 * -2 body 截断（无闭引号）；-3 超 out_max。（review P2-2：截断显式，
 * 不静默冒充成功。） */
static int extract_quoted(const char *body, const char *key, char *out,
			  size_t out_max)
{
	const char *p = find_key(body, key);

	if (p == NULL) {
		return -1;
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p != '"') {
		return -1; /* null/数字——非字符串值 */
	}
	p++;
	size_t o = 0;

	while (p[o] != '\0' && p[o] != '"') {
		if (p[o] == '\\' && p[o + 1] != '\0') {
			o++; /* 转义对：保留源字符（最小提取面） */
		}
		if (p[o] == '\0') {
			return -2; /* 无闭引号=body 截断 */
		}
		if (o + 1 >= out_max) {
			return -3; /* 超 out_max=显式截断拒 */
		}
		out[o] = p[o];
		o++;
	}
	out[o] = '\0';
	return 0;
}

static bool extract_uint(const char *body, const char *key, uint32_t *out)
{
	const char *p = find_key(body, key);

	if (p == NULL) {
		return false;
	}
	*out = (uint32_t)strtoul(p, NULL, 10);
	return true;
}

entelea_result_t entelea_https_chat(void *user,
				    const entelea_model_msg_t *msgs, uint8_t n,
				    char *out, size_t out_max,
				    uint32_t timeout_ms,
				    entelea_model_usage_t *usage)
{
	const entelea_https_cfg_t *cfg = user;

	if (cfg == NULL || cfg->host[0] == '\0' || cfg->model[0] == '\0') {
		return ENTELEA_E_PARAM;
	}
	if (n != 1 || msgs[0].content == NULL) {
		return ENTELEA_E_PARAM; /* 多消息面显式拒（不静默丢） */
	}
	if (cfg->touched != NULL) {
		(*cfg->touched)++;
	}
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	char req[512 + ENTELEA_HTTPS_BODY_MAX];
	static char body[ENTELEA_HTTPS_BODY_MAX]; /* 16K 静态（host 单线程闸） */
	entelea_result_t r = ENTELEA_E_IO;
	const char *stage = "init";
	uint64_t deadline_ms;

	mbedtls_net_init(&net);
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_x509_crt_init(&ca);
	if (psa_crypto_init() != PSA_SUCCESS) {
		stage = "psa_init";
		goto cleanup; /* 4.x RNG 面=PSA（mini_client 同款） */
	}
	if (mbedtls_x509_crt_parse_file(&ca, cfg->ca_bundle[0]
					    ? cfg->ca_bundle
					    : CA_BUNDLE) != 0) {
		stage = "ca_parse";
		goto cleanup;
	}
	if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		stage = "ssl_defaults";
		goto cleanup;
	}
	mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	if (mbedtls_ssl_setup(&ssl, &conf) != 0 ||
	    mbedtls_ssl_set_hostname(&ssl, cfg->host) != 0) {
		stage = "ssl_setup";
		goto cleanup;
	}
	mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send,
			    mbedtls_net_recv, NULL);

	deadline_ms = now_ms() + 30000; /* connect 段独立 30s 界 */
	if (mbedtls_net_connect(&net, cfg->host, HTTPS_PORT,
				MBEDTLS_NET_PROTO_TCP) != 0) {
		stage = now_ms() >= deadline_ms ? "connect_timeout"
						: "connect";
		if (now_ms() >= deadline_ms) {
			r = ENTELEA_E_TIMEOUT;
		}
		goto cleanup;
	}
	{ /* 1s 量子轮询（整体界=deadline——review P2-1）。 */
		struct timeval tv = { .tv_sec = 1 };

		setsockopt(net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	deadline_ms = now_ms() + timeout_ms; /* connect 后起算读写界 */

	int hs;

	while ((hs = mbedtls_ssl_handshake(&ssl)) != 0) {
		if (hs == MBEDTLS_ERR_SSL_WANT_READ ||
		    hs == MBEDTLS_ERR_SSL_WANT_WRITE) {
			if (now_ms() >= deadline_ms) {
				stage = "hs_deadline";
				r = ENTELEA_E_TIMEOUT;
				goto cleanup;
			}
			continue;
		}
		{
			char eb[128];

			mbedtls_strerror(hs, eb, sizeof(eb));
			fprintf(stderr, "[https] handshake: %s (-0x%x)\n",
				eb, (unsigned)-hs);
			goto cleanup; /* TLS 失败（含证书验证拒）=E_IO */
		}
	}

	/* HTTP POST 组包。 */
	char esc[512];

	if (!json_escape(msgs[0].content, esc, sizeof(esc))) {
		stage = "prompt_overflow";
		r = ENTELEA_E_RANGE;
		goto cleanup;
	}
	int body_len = snprintf(body, ENTELEA_HTTPS_BODY_MAX,
				"{\"model\":\"%s\",\"messages\":"
				"[{\"role\":\"user\",\"content\":\"%s\"}],"
				"\"max_tokens\":256}",
				cfg->model, esc);
	int req_len = snprintf(req, sizeof(req),
			       "POST %s/chat/completions HTTP/1.1\r\n"
			       "Host: %s\r\n"
			       "Authorization: Bearer %s\r\n"
			       "Content-Type: application/json\r\n"
			       "Connection: close\r\n"
			       "Content-Length: %d\r\n\r\n%s",
			       cfg->path_prefix, cfg->host, cfg->api_key,
			       body_len, body);

	if (req_len <= 0 || (size_t)req_len >= sizeof(req)) {
		stage = "req_build";
		goto cleanup;
	}
	size_t sent = 0;

	while (sent < (size_t)req_len) {
		int w = mbedtls_ssl_write(&ssl,
					  (const unsigned char *)req + sent,
					  req_len - sent);

		if (w <= 0) {
			stage = "ssl_write";
			goto cleanup;
		}
		sent += (size_t)w;
	}
	size_t got = 0;

	while (got + 1 < ENTELEA_HTTPS_BODY_MAX) {
		if (now_ms() >= deadline_ms) {
			stage = "read_deadline";
			r = ENTELEA_E_TIMEOUT;
			goto cleanup;
		}
		int rd = mbedtls_ssl_read(&ssl,
					  (unsigned char *)body + got,
					  ENTELEA_HTTPS_BODY_MAX - 1 - got);

		if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rd == 0) {
			break;
		}
		if (rd == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
		    rd == MBEDTLS_ERR_SSL_WANT_READ ||
		    rd == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue; /* ticket/WANT_*：量子轮询（计入 deadline） */
		}
		if (rd < 0) {
			/* 阻塞 socket 的 SO_RCVTIMEO 到期被 mbedtls 归
			 * NET_RECV_FAILED——经 errno==EAGAIN 区分量子
			 * 超时与真 IO 错。 */
			if (rd == MBEDTLS_ERR_NET_RECV_FAILED &&
			    errno == EAGAIN) {
				continue;
			}
			if (rd == MBEDTLS_ERR_SSL_TIMEOUT) {
				stage = "ssl_timeout";
				r = ENTELEA_E_TIMEOUT;
				goto cleanup;
			}
			{
				char eb[128];

				stage = "ssl_read";
				mbedtls_strerror(rd, eb, sizeof(eb));
				fprintf(stderr,
					"[https] read: %s (-0x%x) errno=%d\n",
					eb, (unsigned)-rd, errno);
				goto cleanup;
			}
		}
		got += (size_t)rd;
	}
	body[got] = '\0';
	{ /* 状态=首行解析（防错误页文本误判 200——review P2-3）。 */
		int code = 0;

		if (sscanf(body, "HTTP/1.%*d %d", &code) != 1) {
			stage = "status_parse";
			goto cleanup;
		}
		if (code == 401 || code == 403) {
			stage = "auth_rejected";
			r = ENTELEA_E_PERM; /* 鉴权类：不自动重试（额度纪律） */
			goto cleanup;
		}
		if (code == 429) {
			stage = "rate_limited";
			r = ENTELEA_E_BUSY;
			goto cleanup;
		}
		if (code != 200) {
			stage = "http_status";
			fprintf(stderr, "[https] status: %.40s\n", body);
			goto cleanup;
		}
	}
	const char *json = strstr(body, "{");

	if (json == NULL) {
		stage = "no_json";
		goto cleanup;
	}
	int ec = extract_quoted(json, "content", out, out_max);

	if (ec == -1) {
		stage = "no_content";
		r = ENTELEA_E_RANGE; /* 无 content 字段（B-2 合同面） */
		goto cleanup;
	}
	if (ec != 0) {
		stage = ec == -2 ? "content_truncated" : "content_overflow";
		r = ENTELEA_E_RANGE; /* 截断显式（不冒充成功） */
		goto cleanup;
	}
	if (usage != NULL) {
		uint32_t pt = 0, ct = 0;

		(void)extract_uint(json, "prompt_tokens", &pt);
		(void)extract_uint(json, "completion_tokens", &ct);
		usage->prompt_tokens = pt;
		usage->completion_tokens = ct;
	}
	r = ENTELEA_OK;

cleanup:
	if (r != ENTELEA_OK) {
		fprintf(stderr, "[https] fail stage=%s\n", stage);
	}
	mbedtls_ssl_close_notify(&ssl);
	mbedtls_net_free(&net);
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_x509_crt_free(&ca);
	mbedtls_psa_crypto_free();
	return r;
}
