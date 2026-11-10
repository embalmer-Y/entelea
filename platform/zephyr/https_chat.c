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
#include "ca_roots_decl.h"
#include "https_chat.h"

#if defined(CONFIG_MBEDTLS_DEBUG)
void entelea_mbedtls_dbg(void *ctx, int level, const char *file,
			 int line, const char *str);
#endif

#define HTTPS_PORT "443"

static uint64_t now_ms(void)
{
	return (uint64_t)k_uptime_get();
}

/* ---- BIO：fd 直连 send/recv（mbedtls_net_* 是主机 POSIX 面） ---- */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t r = send(fd, buf, len, 0);

	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t r = recv(fd, buf, len, 0);

	if (r == 0) {
		return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
	}
	return r < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)r;
}

/* 转义+控制字符面：false=超界（调用方显式拒）。（host 版同源） */
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

/* 值感知键查找（host 版同源：引号段闭合处后随 ':' 且段内文本==key）。 */
static const char *find_key(const char *body, const char *key)
{
	size_t klen = strlen(key);

	for (const char *p = body; *p != '\0'; p++) {
		if (*p != '"') {
			continue;
		}
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
		p = q;
	}
	return NULL;
}

/* 0=OK；-1 未找到/非串；-2 body 截断；-3 超 out_max。（host 版同源） */
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
		return -1;
	}
	p++;
	size_t o = 0;

	while (p[o] != '\0' && p[o] != '"') {
		if (p[o] == '\\' && p[o + 1] != '\0') {
			o++;
		}
		if (p[o] == '\0') {
			return -2;
		}
		if (o + 1 >= out_max) {
			return -3;
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
	unsigned long v;

	if (p == NULL) {
		return false;
	}
	v = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (unsigned long)(*p - '0');
		p++;
	}
	*out = (uint32_t)v;
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
		return ENTELEA_E_PARAM;
	}
	if (cfg->touched != NULL) {
		(*cfg->touched)++;
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

	{ /* connect（独立 30s 界，DNS+TCP） */
		int gai = getaddrinfo(cfg->host, HTTPS_PORT, &hints, &res);

		if (gai != 0 || res == NULL) {
			stage = "dns";
			goto cleanup;
		}
		fd = socket(res->ai_family, res->ai_socktype, 0);
		if (fd < 0) {
			stage = "socket";
			goto cleanup;
		}
		{ /* 1s 量子+整体 deadline（host 版同面） */
			struct timeval tv = { .tv_sec = 1 };
			uint64_t deadline = now_ms() + 30000;

			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			while (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
				if (errno == EAGAIN || errno == EINPROGRESS) {
					if (now_ms() >= deadline) {
						stage = "connect_timeout";
						r = ENTELEA_E_TIMEOUT;
						goto cleanup;
					}
					continue;
				}
				stage = "connect";
				goto cleanup;
			}
		}
	}
	mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
	{
		uint64_t deadline_ms = now_ms() + timeout_ms;
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
			stage = "handshake";
			printk("[https] handshake: -0x%x\n", (unsigned)-hs);
			goto cleanup; /* 含证书验证拒=E_IO */
		}
	}

	{ /* HTTP POST 组包（host 版同面） */
		char esc[384];

		if (!json_escape(msgs[0].content, esc, sizeof(esc))) {
			stage = "prompt_overflow";
			r = ENTELEA_E_RANGE;
			goto cleanup;
		}
		int body_len = snprintk(body, ENTELEA_HTTPS_BODY_MAX,
					"{\"model\":\"%s\",\"messages\":"
					"[{\"role\":\"user\",\"content\":\"%s\"}],"
					"\"max_tokens\":256}",
					cfg->model, esc);
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
			int w = mbedtls_ssl_write(&ssl,
						  (const unsigned char *)req + sent,
						  req_len - sent);

			if (w <= 0) {
				stage = "ssl_write";
				goto cleanup;
			}
			sent += (size_t)w;
		}
	}
	size_t got = 0;

	while (got + 1 < ENTELEA_HTTPS_BODY_MAX) {
		int rd = mbedtls_ssl_read(&ssl,
					  (unsigned char *)body + got,
					  ENTELEA_HTTPS_BODY_MAX - 1 - got);

		if (rd == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rd == 0) {
			break;
		}
		if (rd == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET ||
		    rd == MBEDTLS_ERR_SSL_WANT_READ ||
		    rd == MBEDTLS_ERR_SSL_WANT_WRITE) {
			continue; /* ticket/WANT_*（host 版同面） */
		}
		if (rd < 0) {
			/* Zephyr socket 的 SO_RCVTIMEO 到期同样归
			 * NET_RECV_FAILED 面——errno==EAGAIN 区分。 */
			if (errno == EAGAIN) {
				continue;
			}
			stage = "ssl_read";
			printk("[https] read: -0x%x errno=%d\n",
			       (unsigned)-rd, errno);
			goto cleanup;
		}
		got += (size_t)rd;
	}
	body[got] = '\0';
	{ /* 状态=首行解析（host 版同面） */
		int code = 0;
		const char *sp = body;
		int sc = 0;

		/* "HTTP/1.x NNN"：跳过 8 字符协议段取 3 位状态码（板上无
		 * sscanf 浮点面——手写解析）。 */
		if (got < 12 || memcmp(body, "HTTP/1.", 7) != 0) {
			printk("[https] got=%u head=%02x%02x%02x%02x\n",
			       (unsigned)got, body[0], body[1], body[2],
			       body[3]);
			stage = "status_parse";
			goto cleanup;
		}
		sp = body + 8; /* 协议段后跳空白（host 版 sscanf 等价面） */
		while (*sp == ' ' || *sp == '\t') {
			sp++;
		}
		sc = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
		if (sp[0] < '0' || sp[0] > '9' || sp[1] < '0' || sp[1] > '9' ||
		    sp[2] < '0' || sp[2] > '9' || sc < 100 || sc > 599) {
			stage = "status_parse";
			goto cleanup;
		}
		code = sc;
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
	}
	const char *json = strstr(body, "{");

	if (json == NULL) {
		stage = "no_json";
		goto cleanup;
	}
	{
		int ec = extract_quoted(json, "content", out, out_max);

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

		(void)extract_uint(json, "prompt_tokens", &pt);
		(void)extract_uint(json, "completion_tokens", &ct);
		usage->prompt_tokens = pt;
		usage->completion_tokens = ct;
	}
	r = ENTELEA_E_TIMEOUT; /* 防漏置 */
	if (out[0] != '\0') {
		r = ENTELEA_OK;
	} else {
		stage = "empty_content";
		r = ENTELEA_E_RANGE;
	}

cleanup:
	if (r != ENTELEA_OK) {
		printk("[https] fail stage=%s\n", stage);
	}
	mbedtls_ssl_close_notify(&ssl);
	if (fd >= 0) {
		close(fd);
	}
	if (res != NULL) {
		freeaddrinfo(res);
	}
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
