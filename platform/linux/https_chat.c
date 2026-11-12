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
#include "../https_http.h"
#include "../https_json.h"
#include "../https_time.h"
#include "../secure_zero.h"
#include "https_chat.h"

#define HTTPS_PORT "443"
#define CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

static bool now_ms(uint64_t *out)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
	    ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L ||
	    (uint64_t)ts.tv_sec > (UINT64_MAX - 999U) / 1000U) {
		return false;
	}
	*out = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
	return true;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	mbedtls_net_context *net = ctx;
	ssize_t ret = send(net->fd, buf, len, MSG_NOSIGNAL);

	if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			errno == EINTR)) {
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	}
	if (ret < 0 && (errno == EPIPE || errno == ECONNRESET)) {
		return MBEDTLS_ERR_NET_CONN_RESET;
	}
	return ret < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : (int)ret;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	mbedtls_net_context *net = ctx;
	ssize_t ret = recv(net->fd, buf, len, 0);

	if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
			errno == EINTR)) {
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	if (ret < 0 && (errno == EPIPE || errno == ECONNRESET)) {
		return MBEDTLS_ERR_NET_CONN_RESET;
	}
	return ret < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : (int)ret;
}

entelea_result_t entelea_https_chat(void *user,
				    const entelea_model_msg_t *msgs, uint8_t n,
				    char *out, size_t out_max,
				    uint32_t timeout_ms,
				    entelea_model_usage_t *usage)
{
	const entelea_https_cfg_t *cfg = user;

	if (cfg == NULL ||
	    !https_http_token(cfg->host, sizeof(cfg->host), false) ||
	    !https_http_token(cfg->path_prefix, sizeof(cfg->path_prefix), true) ||
	    !https_http_token(cfg->api_key, sizeof(cfg->api_key), true) ||
	    memchr(cfg->model, '\0', sizeof(cfg->model)) == NULL ||
	    cfg->model[0] == '\0') {
		return ENTELEA_E_PARAM;
	}
	if (msgs == NULL || n != 1 || msgs[0].content == NULL ||
	    msgs[0].role == NULL || strcmp(msgs[0].role, "user") != 0 ||
	    out == NULL || out_max == 0) {
		return ENTELEA_E_PARAM; /* 多消息面显式拒（不静默丢） */
	}
	out[0] = '\0';
	if (usage != NULL) {
		*usage = (entelea_model_usage_t){0};
	}
	if (cfg->touched != NULL) {
		(*cfg->touched)++;
	}
	https_timer_t timer;
	entelea_result_t deadline_rc = https_timer_start(&timer, now_ms, timeout_ms);

	if (deadline_rc != ENTELEA_OK) {
		return deadline_rc;
	}
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	char req[512 + ENTELEA_HTTPS_BODY_MAX];
	static char body[ENTELEA_HTTPS_BODY_MAX]; /* 16K 静态（host 单线程闸） */
	entelea_result_t r = ENTELEA_E_IO;
	const char *stage = "init";

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
	mbedtls_ssl_set_bio(&ssl, &net, bio_send, bio_recv, NULL);

	deadline_rc = https_timer_check(&timer, now_ms);
	if (deadline_rc != ENTELEA_OK) {
		stage = "connect_deadline";
		r = deadline_rc;
		goto cleanup;
	}
	int connect_rc = mbedtls_net_connect(&net, cfg->host, HTTPS_PORT,
					    MBEDTLS_NET_PROTO_TCP);

	deadline_rc = https_timer_check(&timer, now_ms);
	if (deadline_rc != ENTELEA_OK) {
		stage = "connect_deadline";
		r = deadline_rc;
		goto cleanup;
	}
	if (connect_rc != 0) {
		stage = "connect";
		r = ENTELEA_E_NOT_SENT;
		goto cleanup;
	}
	{ /* 1s 量子轮询（整体界=deadline——review P2-1）。 */
		struct timeval tv = { .tv_sec = 1 };

		if (setsockopt(net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
			       sizeof(tv)) != 0 ||
		    setsockopt(net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
			       sizeof(tv)) != 0) {
			stage = "socket_timeout";
			goto cleanup;
		}
	}
	int hs;

	for (;;) {
		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "hs_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		hs = mbedtls_ssl_handshake(&ssl);
		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "hs_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		if (hs == 0) {
			break;
		}
		if (hs == MBEDTLS_ERR_SSL_WANT_READ ||
		    hs == MBEDTLS_ERR_SSL_WANT_WRITE) {
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
	char model_esc[sizeof(cfg->model) * 6];

	if (!https_json_escape(msgs[0].content, esc, sizeof(esc)) ||
	    !https_json_escape(cfg->model, model_esc, sizeof(model_esc))) {
		stage = "prompt_overflow";
		r = ENTELEA_E_RANGE;
		goto cleanup;
	}
	int body_len = snprintf(body, ENTELEA_HTTPS_BODY_MAX,
				"{\"model\":\"%s\",\"messages\":"
				"[{\"role\":\"user\",\"content\":\"%s\"}],"
				"\"max_tokens\":256}",
				model_esc, esc);
	if (body_len <= 0 || (size_t)body_len >= sizeof(body)) {
		stage = "body_overflow";
		r = ENTELEA_E_RANGE;
		goto cleanup;
	}
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
		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "write_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		int w = mbedtls_ssl_write(&ssl,
					  (const unsigned char *)req + sent,
					  req_len - sent);

		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "write_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		if (w == MBEDTLS_ERR_SSL_WANT_READ ||
		    w == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue;
		}
		if (w <= 0) {
			stage = "ssl_write";
			goto cleanup;
		}
		sent += (size_t)w;
	}
	size_t got = 0;
	bool peer_closed_cleanly = false;

	while (got + 1 < ENTELEA_HTTPS_BODY_MAX) {
		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "read_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		int rd = mbedtls_ssl_read(&ssl,
					  (unsigned char *)body + got,
					  ENTELEA_HTTPS_BODY_MAX - 1 - got);

		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "read_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
			peer_closed_cleanly = true;
			break;
		}
		if (rd == 0) {
			break;
		}
		if (rd == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
		    rd == MBEDTLS_ERR_SSL_WANT_READ ||
		    rd == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue; /* ticket/WANT_*：量子轮询（计入 deadline） */
		}
		if (rd < 0) {
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
	if (got + 1 == ENTELEA_HTTPS_BODY_MAX) {
		stage = "response_overflow";
		r = ENTELEA_E_RANGE;
		goto cleanup;
	}
	int code = 0;
	char *json = NULL;

	if (!https_http_payload(body, got, peer_closed_cleanly, &code, &json)) {
		stage = "http_framing";
		r = ENTELEA_E_RANGE;
		goto cleanup;
	}
	if (code == 401 || code == 403) {
		stage = "auth_rejected";
		r = ENTELEA_E_PERM;
		goto cleanup;
	}
	if (code == 429) {
		stage = "rate_limited";
		r = ENTELEA_E_BUSY;
		goto cleanup;
	}
	if (code != 200) {
		stage = "http_status";
		fprintf(stderr, "[https] status=%d\n", code);
		goto cleanup;
	}
	int ec = https_json_chat_content(json, out, out_max);

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

		(void)https_json_extract_uint(json, "prompt_tokens", &pt);
		(void)https_json_extract_uint(json, "completion_tokens", &ct);
		usage->prompt_tokens = pt;
		usage->completion_tokens = ct;
	}
	deadline_rc = https_timer_check(&timer, now_ms);
	if (deadline_rc != ENTELEA_OK) {
		stage = "response_deadline";
		r = deadline_rc;
		goto cleanup;
	}
	if (out[0] == '\0') {
		stage = "empty_content";
		r = ENTELEA_E_RANGE;
		goto cleanup;
	}
	r = ENTELEA_OK;

cleanup:
	if (r != ENTELEA_OK) {
		out[0] = '\0';
		fprintf(stderr, "[https] fail stage=%s\n", stage);
	}
	mbedtls_net_free(&net);
	entelea_secure_zero(req, sizeof(req));
	entelea_secure_zero(body, sizeof(body));
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_x509_crt_free(&ca);
	mbedtls_psa_crypto_free();
	return r;
}
