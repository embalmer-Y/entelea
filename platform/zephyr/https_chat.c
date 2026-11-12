/* SPDX-License-Identifier: Apache-2.0 */
/* r15 板上模型传输真实现（platform/zephyr 装配面——框架不含网络栈；
 * 与 platform/linux/https_chat.c 同源合同）。移植差异面（其余逐行
 * 同源）：socket=getaddrinfo+Zephyr POSIX、CA=嵌入根数组、BIO=自管
 * fd 回调、时钟=k_uptime_get。F-2 打磨全部保留：值感知 JSON 键查找
 * （模型可在 reasoning 文本写字面 "content":）、TLS1.3 ticket 续读、
 * SO_RCVTIMEO 1s 量子+整体 deadline、首行状态码（401/403→E_PERM、
 * 429→E_BUSY）、截断显式（-2/-3→E_RANGE 不冒充成功）。 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/posix/unistd.h>
#include <mbedtls/debug.h>
#include <mbedtls/error.h>
#include <mbedtls/psa_util.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include "../https_http.h"
#include "../https_json.h"
#include "../https_time.h"
#include "../secure_zero.h"
#include "ca_roots_decl.h"
#include "https_chat.h"

#if defined(CONFIG_MBEDTLS_DEBUG)
void entelea_mbedtls_dbg(void *ctx, int level, const char *file,
			 int line, const char *str);
#endif

#define HTTPS_PORT "443"

static bool now_ms(uint64_t *out)
{
	int64_t now = k_uptime_get();

	if (now < 0) {
		return false;
	}
	*out = (uint64_t)now;
	return true;
}

/* ---- BIO：fd 直连 send/recv（mbedtls_net_* 是主机 POSIX 面） ---- */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t r = send(fd, buf, len, 0);

	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
		      errno == EINTR)) {
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	}
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t r = recv(fd, buf, len, 0);

	if (r == 0) {
		return 0;
	}
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
		      errno == EINTR)) {
		return MBEDTLS_ERR_SSL_WANT_READ;
	}
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r;
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
		return ENTELEA_E_PARAM;
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
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt ca;
	struct addrinfo *res = NULL;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	static char body[ENTELEA_HTTPS_BODY_MAX]; /* 8K 静态（闸单线程） */
	static char req[512 + ENTELEA_HTTPS_BODY_MAX];
	int fd = -1;
	entelea_result_t r = ENTELEA_E_IO;
	const char *stage = "init";

	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	mbedtls_x509_crt_init(&ca);
	if (psa_crypto_init() != PSA_SUCCESS) {
		stage = "psa_init";
		goto cleanup; /* RNG 面=Zephyr mbedtls 模块 PSA */
	}
	for (size_t i = 0; i < entelea_ca_root_count; i++) {
		if (mbedtls_x509_crt_parse(&ca,
					   (const unsigned char *)entelea_ca_roots[i],
					   strlen(entelea_ca_roots[i]) + 1) != 0) {
			stage = "ca_parse";
			goto cleanup;
		}
	}
	if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
					MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
		stage = "ssl_defaults";
		goto cleanup;
	}
#if defined(CONFIG_MBEDTLS_DEBUG)
	mbedtls_debug_set_threshold(2);
	mbedtls_ssl_conf_dbg(&conf, entelea_mbedtls_dbg, NULL);
#endif
	mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
	mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	if (mbedtls_ssl_setup(&ssl, &conf) != 0 ||
	    mbedtls_ssl_set_hostname(&ssl, cfg->host) != 0) {
		stage = "ssl_setup";
		goto cleanup;
	}

	deadline_rc = https_timer_check(&timer, now_ms);
	if (deadline_rc != ENTELEA_OK) {
		stage = "deadline";
		r = deadline_rc;
		goto cleanup;
	}
	{ /* DNS 可能阻塞；返回后检查同一时间预算。 */
		int gai = getaddrinfo(cfg->host, HTTPS_PORT, &hints, &res);

		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "dns_deadline";
			r = deadline_rc;
			goto cleanup;
		}
		if (gai != 0 || res == NULL) {
			stage = "dns";
			r = ENTELEA_E_NOT_SENT;
			goto cleanup;
		}
		fd = socket(res->ai_family, res->ai_socktype, 0);
		if (fd < 0) {
			stage = "socket";
			r = ENTELEA_E_NOT_SENT;
			goto cleanup;
		}
		{
			struct timeval tv = { .tv_sec = 1 };

			if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
				       sizeof(tv)) != 0 ||
			    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv,
				       sizeof(tv)) != 0) {
				stage = "socket_timeout";
				goto cleanup;
			}
			for (;;) {
				deadline_rc = https_timer_check(&timer, now_ms);
				if (deadline_rc != ENTELEA_OK) {
					stage = "connect_deadline";
					r = deadline_rc;
					goto cleanup;
				}
				int connect_rc = connect(fd, res->ai_addr, res->ai_addrlen);
				int connect_errno = errno;

				deadline_rc = https_timer_check(&timer, now_ms);
				if (deadline_rc != ENTELEA_OK) {
					stage = "connect_deadline";
					r = deadline_rc;
					goto cleanup;
				}
				if (connect_rc == 0 || connect_errno == EISCONN) {
					break;
				}
				if (connect_errno == EAGAIN || connect_errno == EINPROGRESS ||
				    connect_errno == EALREADY || connect_errno == EINTR) {
					continue;
				}
				stage = "connect";
				r = ENTELEA_E_NOT_SENT;
				goto cleanup;
			}
		}
	}
	mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
	{
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
			stage = "handshake";
			printk("[https] handshake: -0x%x\n", (unsigned)-hs);
			goto cleanup; /* 含证书验证拒=E_IO */
		}
	}

	{ /* HTTP POST 组包（host 版同面） */
		char esc[384];
		char model_esc[sizeof(cfg->model) * 6];

		if (!https_json_escape(msgs[0].content, esc, sizeof(esc)) ||
		    !https_json_escape(cfg->model, model_esc, sizeof(model_esc))) {
			stage = "prompt_overflow";
			r = ENTELEA_E_RANGE;
			goto cleanup;
		}
		int body_len = snprintk(body, ENTELEA_HTTPS_BODY_MAX,
					"{\"model\":\"%s\",\"messages\":"
					"[{\"role\":\"user\",\"content\":\"%s\"}],"
					"\"max_tokens\":256}",
					model_esc, esc);
		if (body_len <= 0 || (size_t)body_len >= sizeof(body)) {
			stage = "body_overflow";
			r = ENTELEA_E_RANGE;
			goto cleanup;
		}
		int req_len = snprintk(req, sizeof(req),
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
	}
	size_t got = 0;
	bool peer_closed_cleanly = false;

	while (got + 1 < ENTELEA_HTTPS_BODY_MAX) {
		deadline_rc = https_timer_check(&timer, now_ms);
		if (deadline_rc != ENTELEA_OK) {
			stage = "read_deadline"; /* 慢速/静默对端不得无限挂起 */
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
			stage = "ssl_read";
			printk("[https] read: -0x%x\n", (unsigned)-rd);
			goto cleanup;
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
		printk("[https] status=%d\n", code);
		stage = "http_status";
		goto cleanup;
	}
	{
		int ec = https_json_chat_content(json, out, out_max);

		if (ec == -1) {
			stage = "no_content";
			r = ENTELEA_E_RANGE;
			goto cleanup;
		}
		if (ec != 0) {
			stage = ec == -2 ? "content_truncated" : "content_overflow";
			r = ENTELEA_E_RANGE;
			goto cleanup;
		}
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
	if (out[0] != '\0') {
		r = ENTELEA_OK;
	} else {
		stage = "empty_content";
		r = ENTELEA_E_RANGE;
	}

cleanup:
	if (r != ENTELEA_OK) {
		out[0] = '\0';
		printk("[https] fail stage=%s\n", stage);
	}
	if (fd >= 0) {
		close(fd);
	}
	if (res != NULL) {
		freeaddrinfo(res);
	}
	entelea_secure_zero(req, sizeof(req));
	entelea_secure_zero(body, sizeof(body));
	mbedtls_ssl_free(&ssl);
	mbedtls_ssl_config_free(&conf);
	mbedtls_x509_crt_free(&ca);
	return r;
}

#if defined(CONFIG_MBEDTLS_DEBUG)
void entelea_mbedtls_dbg(void *ctx, int level, const char *file,
			 int line, const char *str)
{
	ARG_UNUSED(ctx); ARG_UNUSED(level);
	printk("[mbed] %s:%d %s\n", file, line, str);
}
#endif
