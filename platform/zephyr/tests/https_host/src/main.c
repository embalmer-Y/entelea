/* SPDX-License-Identifier: Apache-2.0 */
/* Offline probe of the actual HTTPS transports; all sockets are wrapped. */
#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include "https_chat.h"

const char *const entelea_ca_roots[] = {NULL};
const size_t entelea_ca_root_count = 0;
static int64_t fake_ms;
static const char *mode;
static unsigned reads;
static unsigned writes;
static bool request_preserved;
static const unsigned char *written_request;
static size_t written_request_len;
static bool request_contained_test_secret;
static bool request_wiped_at_free;
static int raw_eof_result = -1;
static void *bio_ctx;
static mbedtls_ssl_recv_t *bio_recv;
static mbedtls_ssl_send_t *bio_send;

int64_t k_uptime_get(void)
{
	if (mode != NULL && strcmp(mode, "clock_fail") == 0) {
		return -1;
	}
	return fake_ms;
}

#if defined(ENTELEA_HTTPS_HOST_LINUX)
int __real_clock_gettime(clockid_t clock_id, struct timespec *ts);

int __wrap_clock_gettime(clockid_t clock_id, struct timespec *ts)
{
	if (clock_id != CLOCK_MONOTONIC) {
		return __real_clock_gettime(clock_id, ts);
	}
	if (mode != NULL && strcmp(mode, "clock_fail") == 0) {
		errno = EIO;
		return -1;
	}
	ts->tv_sec = fake_ms / 1000;
	ts->tv_nsec = (fake_ms % 1000) * 1000000;
	return 0;
}

int __wrap_mbedtls_net_connect(mbedtls_net_context *net, const char *host,
			       const char *port, int proto)
{
	(void)host;
	(void)port;
	(void)proto;
	if (strcmp(mode, "dns") == 0 || strcmp(mode, "connect") == 0) {
		return MBEDTLS_ERR_NET_CONNECT_FAILED;
	}
	if (strcmp(mode, "dns_delay") == 0) {
		fake_ms += 3500;
	} else if (strcmp(mode, "connect_delay") == 0) {
		fake_ms += 2500;
	}
	net->fd = 10;
	return 0;
}

void __wrap_mbedtls_net_free(mbedtls_net_context *net)
{
	net->fd = -1;
}

int __wrap_mbedtls_x509_crt_parse_file(mbedtls_x509_crt *chain,
				       const char *path)
{
	(void)chain;
	(void)path;
	return 0;
}
#endif

int __wrap_getaddrinfo(const char *node, const char *service,
		       const struct addrinfo *hints, struct addrinfo **out)
{
	static struct sockaddr address;
	static struct addrinfo result;

	(void)node;
	(void)service;
	(void)hints;
	if (strcmp(mode, "dns") == 0) {
		return EAI_FAIL;
	}
	if (strcmp(mode, "dns_delay") == 0) {
		fake_ms += 3500;
	}
	memset(&result, 0, sizeof(result));
	result.ai_family = AF_INET;
	result.ai_socktype = SOCK_STREAM;
	result.ai_addr = &address;
	result.ai_addrlen = sizeof(address);
	*out = &result;
	return 0;
}

void __wrap_freeaddrinfo(struct addrinfo *result)
{
	(void)result;
}

int __wrap_socket(int domain, int type, int protocol)
{
	(void)domain;
	(void)type;
	(void)protocol;
	return 10;
}

int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)fd;
	(void)addr;
	(void)len;
	if (strcmp(mode, "connect") == 0) {
		errno = ECONNREFUSED;
		return -1;
	}
	if (strcmp(mode, "connect_delay") == 0) {
		fake_ms += 2500;
	}
	return 0;
}

int __wrap_setsockopt(int fd, int level, int name, const void *value,
		      socklen_t len)
{
	(void)fd;
	(void)level;
	(void)name;
	(void)value;
	(void)len;
	if (strcmp(mode, "setsockopt") == 0) {
		errno = ENOPROTOOPT;
		return -1;
	}
	return 0;
}

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
	(void)fd;
	(void)buf;
	(void)len;
	(void)flags;
	errno = strcmp(mode, "send_eagain") == 0 ? EAGAIN : EACCES;
	return -1;
}

ssize_t __wrap_recv(int fd, void *buf, size_t len, int flags)
{
	(void)fd;
	(void)buf;
	(void)len;
	(void)flags;
	if (strcmp(mode, "raw_eof") == 0) {
		return 0;
	}
	errno = strcmp(mode, "recv_eagain") == 0 ? EAGAIN : EACCES;
	return -1;
}

int __wrap_close(int fd)
{
	(void)fd;
	return 0;
}

int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context *ssl)
{
	(void)ssl;
	if (strcmp(mode, "handshake_late") == 0) {
		fake_ms += 3000;
	}
	return 0;
}

void __real_mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *ctx,
				 mbedtls_ssl_send_t *send_fn,
				 mbedtls_ssl_recv_t *recv_fn,
				 mbedtls_ssl_recv_timeout_t *recv_timeout_fn);

void __wrap_mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *ctx,
				 mbedtls_ssl_send_t *send_fn,
				 mbedtls_ssl_recv_t *recv_fn,
				 mbedtls_ssl_recv_timeout_t *recv_timeout_fn)
{
	bio_ctx = ctx;
	bio_send = send_fn;
	bio_recv = recv_fn;
	__real_mbedtls_ssl_set_bio(ssl, ctx, send_fn, recv_fn, recv_timeout_fn);
}

int __wrap_mbedtls_ssl_write(mbedtls_ssl_context *ssl,
			     const unsigned char *buf, size_t len)
{
	(void)ssl;
	writes++;
	if (writes == 1) {
		written_request = buf;
		written_request_len = len;
		request_contained_test_secret =
			strstr((const char *)buf, "synthetic-test-secret") != NULL;
	}
	if (strcmp(mode, "unicode_input") == 0) {
		request_preserved = strstr((const char *)buf,
			"\xe4\xbd\xa0\xe5\xa5\xbd\\u0001") != NULL;
	}
	if (strcmp(mode, "write") == 0 ||
	    strcmp(mode, "secret_wipe_failure") == 0) {
		return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	}
	if (strcmp(mode, "send_eagain") == 0) {
		fake_ms += 1000;
		return bio_send(bio_ctx, buf, len);
	}
	return (int)len;
}

int __wrap_mbedtls_ssl_read(mbedtls_ssl_context *ssl,
			    unsigned char *buf, size_t len)
{
	(void)ssl;
	(void)len;
	fake_ms += 1000;
	reads++;
	if (strcmp(mode, "clock_rollback") == 0 && reads == 2) {
		fake_ms = 500;
	}
	if (strcmp(mode, "recv_eagain") == 0) {
		return bio_recv(bio_ctx, buf, len);
	}
	if (strcmp(mode, "raw_eof") == 0) {
		raw_eof_result = bio_recv(bio_ctx, buf, len);
		return raw_eof_result;
	}
	if (strcmp(mode, "drip") == 0) {
		buf[0] = 'x';
		return 1;
	}
	if (strcmp(mode, "stale_errno") == 0) {
		if (reads == 1) {
			errno = EAGAIN;
			return MBEDTLS_ERR_SSL_WANT_READ;
		}
		return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
	}
	if (strcmp(mode, "read") == 0) {
		return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
	}
	if (strcmp(mode, "success") == 0 ||
	    strcmp(mode, "secret_wipe_success") == 0 ||
	    strcmp(mode, "unicode_input") == 0 ||
	    strcmp(mode, "escaped") == 0 ||
	    strcmp(mode, "truncated") == 0 ||
	    strcmp(mode, "invalid_escape") == 0 ||
	    strcmp(mode, "missing_close") == 0 ||
	    strcmp(mode, "short_unicode") == 0 ||
	    strcmp(mode, "unpaired_surrogate") == 0 ||
	    strcmp(mode, "invalid_response_utf8") == 0 ||
	    strcmp(mode, "escaped_nul") == 0 ||
	    strcmp(mode, "bad_json_number") == 0 ||
	    strcmp(mode, "duplicate_content") == 0 ||
	    strcmp(mode, "wrong_content") == 0 ||
	    strcmp(mode, "chunked") == 0 ||
	    strcmp(mode, "chunked_truncated") == 0 ||
	    strcmp(mode, "bad_trailer") == 0 ||
	    strcmp(mode, "length_short") == 0 ||
	    strcmp(mode, "bad_response_name") == 0 ||
	    strcmp(mode, "bad_response_value") == 0 ||
	    strcmp(mode, "unclean_eof") == 0 ||
	    strcmp(mode, "unclean_length") == 0 ||
	    strcmp(mode, "unclean_chunked") == 0 ||
	    strcmp(mode, "header_decoy") == 0 ||
	    strcmp(mode, "leading_space") == 0 ||
	    strcmp(mode, "bad_header") == 0 ||
	    strcmp(mode, "invalid_utf8") == 0 ||
	    strcmp(mode, "eof_late") == 0 ||
	    strcmp(mode, "overflow") == 0) {
		static const char response[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}],"
			"\"usage\":{\"prompt_tokens\": 4,\"completion_tokens\": 2}}";
		static const char escaped[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":"
			"\"\\u4F60\\u597D \\\"A\\\"\\n\\uD83D\\uDE00\"}}]}";
		static const char truncated[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"unfinished";
		static const char invalid[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"bad\\q\"}}]}";
		static const char missing_close[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"unfinished\"";
		static const char short_unicode[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"\\u1";
		static const char unpaired_surrogate[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"\\uD800x\"}}]}";
		static const char invalid_response_utf8[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"\xf0\x9f\x8e\"}}]}";
		static const char escaped_nul[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"\\u0000\"}}]}";
		static const char bad_json_number[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}],"
			"\"usage\":{\"prompt_tokens\":01}}";
		static const char duplicate_content[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\","
			"\"content\":\"other\"}}]}";
		static const char wrong_content[] =
			"HTTP/1.1 200 OK\r\n\r\n"
			"{\"content\":\"decoy\",\"choices\":[{\"message\":"
			"{\"content\":\"real\"}}]}";
		static const char length_short[] =
			"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
		static const char header_decoy[] =
			"HTTP/1.1 200 OK\r\nX-Trace: {\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
		static const char bad_response_name[] =
			"HTTP/1.1 200 OK\r\nContent-Length : 999\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
		static const char bad_response_value[] =
			"HTTP/1.1 200 OK\r\nX-Trace: bad\x01value\r\n\r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
		static const char leading_space[] =
			"HTTP/1.1 200 OK\r\n\r\n \r\n"
			"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
		const char *src = response;
		size_t amount = sizeof(response) - 1;

		if (reads > 1) {
			if (strcmp(mode, "eof_late") == 0) {
				fake_ms += 2000;
			}
			return strcmp(mode, "unclean_eof") == 0 ||
			       strcmp(mode, "unclean_length") == 0 ||
			       strcmp(mode, "unclean_chunked") == 0
				? 0 : MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
		}
		if (strcmp(mode, "escaped") == 0) {
			src = escaped;
			amount = sizeof(escaped) - 1;
		} else if (strcmp(mode, "truncated") == 0) {
			src = truncated;
			amount = sizeof(truncated) - 1;
		} else if (strcmp(mode, "invalid_escape") == 0) {
			src = invalid;
			amount = sizeof(invalid) - 1;
		} else if (strcmp(mode, "missing_close") == 0) {
			src = missing_close;
			amount = sizeof(missing_close) - 1;
		} else if (strcmp(mode, "short_unicode") == 0) {
			src = short_unicode;
			amount = sizeof(short_unicode) - 1;
		} else if (strcmp(mode, "unpaired_surrogate") == 0) {
			src = unpaired_surrogate;
			amount = sizeof(unpaired_surrogate) - 1;
		} else if (strcmp(mode, "invalid_response_utf8") == 0) {
			src = invalid_response_utf8;
			amount = sizeof(invalid_response_utf8) - 1;
		} else if (strcmp(mode, "escaped_nul") == 0) {
			src = escaped_nul;
			amount = sizeof(escaped_nul) - 1;
		} else if (strcmp(mode, "bad_json_number") == 0) {
			src = bad_json_number;
			amount = sizeof(bad_json_number) - 1;
		} else if (strcmp(mode, "duplicate_content") == 0) {
			src = duplicate_content;
			amount = sizeof(duplicate_content) - 1;
		} else if (strcmp(mode, "wrong_content") == 0) {
			src = wrong_content;
			amount = sizeof(wrong_content) - 1;
		} else if (strcmp(mode, "length_short") == 0) {
			src = length_short;
			amount = sizeof(length_short) - 1;
		} else if (strcmp(mode, "header_decoy") == 0) {
			src = header_decoy;
			amount = sizeof(header_decoy) - 1;
		} else if (strcmp(mode, "bad_response_name") == 0) {
			src = bad_response_name;
			amount = sizeof(bad_response_name) - 1;
		} else if (strcmp(mode, "bad_response_value") == 0) {
			src = bad_response_value;
			amount = sizeof(bad_response_value) - 1;
		} else if (strcmp(mode, "leading_space") == 0) {
			src = leading_space;
			amount = sizeof(leading_space) - 1;
		}
		if (strcmp(mode, "chunked") == 0 ||
		    strcmp(mode, "chunked_truncated") == 0 ||
		    strcmp(mode, "unclean_chunked") == 0 ||
		    strcmp(mode, "bad_trailer") == 0) {
			const char *first = "{\"choices\":[{\"message\":";
			const char *second = "{\"content\":\"ok\"}}]}";
			int n = snprintf((char *)buf, len,
				"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
				"%zx\r\n%s\r\n%zx\r\n%s\r\n%s",
				strlen(first), first, strlen(second), second,
				strcmp(mode, "chunked") == 0 ||
				strcmp(mode, "unclean_chunked") == 0 ? "0\r\n\r\n" :
				strcmp(mode, "bad_trailer") == 0
					? "0\r\nBad\tName: x\r\n\r\n" : "");

			return n < 0 || (size_t)n >= len
				? MBEDTLS_ERR_SSL_BAD_INPUT_DATA : n;
		}
		if (strcmp(mode, "unclean_length") == 0) {
			const char *json = "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}";
			int n = snprintf((char *)buf, len,
				"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
				strlen(json), json);

			return n < 0 || (size_t)n >= len
				? MBEDTLS_ERR_SSL_BAD_INPUT_DATA : n;
		}
		if (len < amount) {
			return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
		}
		memcpy(buf, src, amount);
		if (strcmp(mode, "overflow") == 0) {
			memset(buf + amount, ' ', len - amount);
			amount = len;
		}
		return (int)amount;
	}
	return MBEDTLS_ERR_SSL_WANT_READ;
}

void __real_mbedtls_ssl_free(mbedtls_ssl_context *ssl);

void __wrap_mbedtls_ssl_free(mbedtls_ssl_context *ssl)
{
	if (written_request != NULL) {
		request_wiped_at_free = true;
		for (size_t i = 0; i < written_request_len; i++) {
			if (written_request[i] != 0) {
				request_wiped_at_free = false;
				break;
			}
		}
	}
	__real_mbedtls_ssl_free(ssl);
}

int __wrap_mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl)
{
	(void)ssl;
	return 0;
}

int main(int argc, char **argv)
{
	entelea_https_cfg_t cfg = {.host = "example.invalid", .model = "test"};
	entelea_model_msg_t msg = {.role = "user", .content = "ping"};
	char out[64] = "";
	entelea_model_usage_t usage = {0};

	if (argc != 2) {
		return 2;
	}
	mode = argv[1];
	if (strcmp(mode, "unicode_input") == 0) {
		msg.content = "\xe4\xbd\xa0\xe5\xa5\xbd\x01";
	} else if (strcmp(mode, "secret_wipe_success") == 0 ||
		   strcmp(mode, "secret_wipe_failure") == 0) {
		strcpy(cfg.api_key, "synthetic-test-secret");
	} else if (strcmp(mode, "invalid_utf8") == 0) {
		msg.content = "\xc0\xaf";
	} else if (strcmp(mode, "bad_header") == 0) {
		strcpy(cfg.api_key, "fake\r\ninjected: value");
	}
	entelea_result_t rc = entelea_https_chat(&cfg, &msg, 1, out, sizeof(out),
						3000, &usage);
	printf("mode=%s rc=%d fake_ms=%lld reads=%u writes=%u\n", mode, (int)rc,
	       (long long)fake_ms, reads, writes);
	if (strcmp(mode, "secret_wipe_success") == 0 ||
	    strcmp(mode, "secret_wipe_failure") == 0) {
		bool expected = strcmp(mode, "secret_wipe_success") == 0
			? rc == ENTELEA_OK : rc == ENTELEA_E_IO;

		return expected && request_contained_test_secret &&
		       request_wiped_at_free ? 0 : 1;
	}
	if (strcmp(mode, "stall") == 0 || strcmp(mode, "drip") == 0) {
		return rc == ENTELEA_E_TIMEOUT && fake_ms == 3000 && reads == 3
			? 0 : 1;
	}
	if (strcmp(mode, "dns") == 0 || strcmp(mode, "connect") == 0) {
		return rc == ENTELEA_E_NOT_SENT && writes == 0 ? 0 : 1;
	}
	if (strcmp(mode, "setsockopt") == 0) {
		return rc == ENTELEA_E_IO && writes == 0 ? 0 : 1;
	}
	if (strcmp(mode, "write") == 0 || strcmp(mode, "read") == 0 ||
	    strcmp(mode, "stale_errno") == 0) {
		return rc == ENTELEA_E_IO ? 0 : 1;
	}
	if (strcmp(mode, "success") == 0 ||
	    strcmp(mode, "unicode_input") == 0) {
		return rc == ENTELEA_OK && strcmp(out, "ok") == 0 &&
		       usage.prompt_tokens == 4 && usage.completion_tokens == 2 &&
		       (strcmp(mode, "success") == 0 || request_preserved)
			? 0 : 1;
	}
	if (strcmp(mode, "escaped") == 0) {
		return rc == ENTELEA_OK &&
		       strcmp(out, "\xe4\xbd\xa0\xe5\xa5\xbd \"A\"\n"
				   "\xf0\x9f\x98\x80") == 0 ? 0 : 1;
	}
	if (strcmp(mode, "truncated") == 0 ||
	    strcmp(mode, "invalid_escape") == 0 ||
	    strcmp(mode, "missing_close") == 0 ||
	    strcmp(mode, "short_unicode") == 0 ||
	    strcmp(mode, "unpaired_surrogate") == 0 ||
	    strcmp(mode, "invalid_response_utf8") == 0 ||
	    strcmp(mode, "escaped_nul") == 0 ||
	    strcmp(mode, "bad_json_number") == 0 ||
	    strcmp(mode, "duplicate_content") == 0 ||
	    strcmp(mode, "chunked_truncated") == 0 ||
	    strcmp(mode, "bad_trailer") == 0 ||
	    strcmp(mode, "length_short") == 0 ||
	    strcmp(mode, "bad_response_name") == 0 ||
	    strcmp(mode, "bad_response_value") == 0 ||
	    strcmp(mode, "unclean_eof") == 0 ||
	    strcmp(mode, "overflow") == 0) {
		return rc == ENTELEA_E_RANGE ? 0 : 1;
	}
	if (strcmp(mode, "raw_eof") == 0) {
		return rc == ENTELEA_E_RANGE && raw_eof_result == 0 ? 0 : 1;
	}
	if (strcmp(mode, "wrong_content") == 0) {
		return rc == ENTELEA_OK && strcmp(out, "real") == 0 ? 0 : 1;
	}
	if (strcmp(mode, "chunked") == 0 ||
	    strcmp(mode, "unclean_length") == 0 ||
	    strcmp(mode, "unclean_chunked") == 0 ||
	    strcmp(mode, "header_decoy") == 0 ||
	    strcmp(mode, "leading_space") == 0) {
		return rc == ENTELEA_OK && strcmp(out, "ok") == 0 ? 0 : 1;
	}
	if (strcmp(mode, "bad_header") == 0) {
		return rc == ENTELEA_E_PARAM && writes == 0 ? 0 : 1;
	}
	if (strcmp(mode, "invalid_utf8") == 0) {
		return rc == ENTELEA_E_RANGE && writes == 0 ? 0 : 1;
	}
	if (strcmp(mode, "connect_delay") == 0) {
		return rc == ENTELEA_E_TIMEOUT && fake_ms == 3500 &&
		       reads == 1 ? 0 : 1;
	}
	if (strcmp(mode, "dns_delay") == 0 ||
	    strcmp(mode, "handshake_late") == 0) {
		return rc == ENTELEA_E_TIMEOUT && writes == 0 ? 0 : 1;
	}
	if (strcmp(mode, "eof_late") == 0) {
		return rc == ENTELEA_E_TIMEOUT && out[0] == '\0' ? 0 : 1;
	}
	if (strcmp(mode, "clock_fail") == 0) {
		return rc == ENTELEA_E_STATE && writes == 0 ? 0 : 1;
	}
	if (strcmp(mode, "clock_rollback") == 0) {
		return rc == ENTELEA_E_STATE && reads == 2 ? 0 : 1;
	}
	if (strcmp(mode, "recv_eagain") == 0 ||
	    strcmp(mode, "send_eagain") == 0) {
		return rc == ENTELEA_E_TIMEOUT && fake_ms == 3000 ? 0 : 1;
	}
	return 2;
}
