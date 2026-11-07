// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/net/dns_resolve.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/sys/time.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/clock.h>

#include "network.h"

#define TEST_CA_TAG 43201

struct tls_test_config {
	struct net_sockaddr_in server;
	uint16_t dns_port;
	uint16_t untrusted_port;
	uint16_t expired_port;
	uint8_t ca[1536];
	size_t ca_length;
	char hostname[64];
};

struct verify_state {
	uint32_t flags;
	unsigned int calls;
};

enum tls_test_action {
	TLS_TEST_ECHO,
	TLS_TEST_TIMEOUT,
	TLS_TEST_DROP,
};

static struct tls_test_config config;

static int read_config(void)
{
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	k_timepoint_t end = sys_timepoint_calc(K_SECONDS(30));
	struct timespec clock = {0};
	uint8_t header[27];
	uint64_t seconds;
	int ret;

	printk("P4L TLS_CONFIG_WAIT\n");
	ret = p4_uart_read_exact(uart, header, sizeof(header), end);
	if (ret) {
		return ret;
	}
	if (memcmp(header, "P4T1", 4) || !header[4] || header[4] == 127 || header[4] >= 224 ||
	    !header[26] || header[26] >= sizeof(config.hostname)) {
		return -EINVAL;
	}
	config.server.sin_family = NET_AF_INET;
	memcpy(&config.server.sin_addr, header + 4, 4);
	config.dns_port = sys_get_be16(header + 8);
	config.server.sin_port = sys_cpu_to_be16(sys_get_be16(header + 10));
	config.untrusted_port = sys_get_be16(header + 12);
	config.expired_port = sys_get_be16(header + 14);
	seconds = sys_get_be64(header + 16);
	config.ca_length = sys_get_be16(header + 24);
	if (!config.dns_port || !config.server.sin_port || !config.untrusted_port ||
	    !config.expired_port || !config.ca_length || config.ca_length > sizeof(config.ca) ||
	    seconds < 1577836800 || seconds > 4102444800ULL) {
		return -EINVAL;
	}
	clock.tv_sec = seconds;
	if ((uint64_t)clock.tv_sec != seconds) {
		return -ERANGE;
	}
	ret = p4_uart_read_exact(uart, (uint8_t *)config.hostname, header[26], end);
	if (ret) {
		return ret;
	}
	for (size_t i = 0; i < header[26]; i++) {
		char c = config.hostname[i];

		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
			return -EINVAL;
		}
	}
	ret = p4_uart_read_exact(uart, config.ca, config.ca_length, end);
	if (ret) {
		return ret;
	}
	return sys_clock_settime(SYS_CLOCK_REALTIME, &clock);
}

static int configure_dns(void)
{
	struct net_sockaddr_in server = config.server;
	const struct net_sockaddr *servers[] = {
		(const struct net_sockaddr *)&server,
		NULL,
	};
	int ret;

	server.sin_port = sys_cpu_to_be16(config.dns_port);
	ret = dns_resolve_close(dns_resolve_get_default());
	if (ret) {
		return ret;
	}
	return dns_resolve_init(dns_resolve_get_default(), NULL, servers);
}

static int dns_test(void)
{
	struct zsock_addrinfo hints = {
		.ai_family = NET_AF_INET,
		.ai_socktype = NET_SOCK_STREAM,
	};
	struct zsock_addrinfo *result = NULL;
	unsigned int count = 0;
	int ret;

	ret = zsock_getaddrinfo(config.hostname, NULL, &hints, &result);
	if (ret) {
		printk("P4L FAIL DNS query rc=%d\n", ret);
		return -EIO;
	}
	for (struct zsock_addrinfo *item = result; item; item = item->ai_next) {
		if (item->ai_family != NET_AF_INET ||
		    item->ai_addrlen != sizeof(struct net_sockaddr_in) ||
		    memcmp(&net_sin(item->ai_addr)->sin_addr, &config.server.sin_addr, 4)) {
			ret = -EBADMSG;
			break;
		}
		count++;
	}
	zsock_freeaddrinfo(result);
	if (ret || count != 1) {
		return ret ? ret : -EBADMSG;
	}
	result = NULL;
	ret = zsock_getaddrinfo("missing.p03.entelea.test", NULL, &hints, &result);
	if (result) {
		zsock_freeaddrinfo(result);
	}
	if (ret != DNS_EAI_FAIL) {
		printk("P4L FAIL DNS NXDOMAIN rc=%d\n", ret);
		return -EBADMSG;
	}
	printk("P4L DNS_OK exact_address=1 negative_rc=%d\n", ret);
	return 0;
}

static int observe_certificate(void *ctx, mbedtls_x509_crt *certificate, int depth, uint32_t *flags)
{
	struct verify_state *state = ctx;

	ARG_UNUSED(certificate);
	ARG_UNUSED(depth);
	/* Observe the backend's flags without clearing or replacing its verification. */
	state->flags |= *flags;
	state->calls++;
	return 0;
}

static int tls_socket(const char *hostname, struct verify_state *state)
{
	sec_tag_t tag = TEST_CA_TAG;
	int verify = ZSOCK_TLS_PEER_VERIFY_REQUIRED;
	struct zsock_tls_cert_verify_cb callback = {
		.cb = observe_certificate,
		.ctx = state,
	};
	int fd;
	int ret;

	fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TLS_1_2);
	if (fd < 0) {
		return -errno;
	}
	if (zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, &tag, sizeof(tag)) ||
	    zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_PEER_VERIFY, &verify, sizeof(verify)) ||
	    zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, hostname, strlen(hostname)) ||
	    zsock_setsockopt(fd, ZSOCK_SOL_TLS, ZSOCK_TLS_CERT_VERIFY_CALLBACK, &callback,
			     sizeof(callback))) {
		ret = -errno;
		if (zsock_close(fd)) {
			printk("P4L FAIL TLS setup close rc=%d\n", -errno);
		}
		return ret;
	}
	return fd;
}

static int socket_exact(int fd, uint8_t *data, size_t size, bool sending)
{
	k_timepoint_t end = sys_timepoint_calc(K_SECONDS(5));
	size_t offset = 0;

	while (offset < size) {
		int64_t remaining = k_ticks_to_ms_ceil64(sys_timepoint_timeout(end).ticks);
		struct timeval timeout = {
			.tv_sec = remaining / 1000,
			.tv_usec = remaining % 1000 * 1000,
		};
		ssize_t count;

		if (remaining <= 0) {
			return -ETIMEDOUT;
		}
		if (zsock_setsockopt(fd, ZSOCK_SOL_SOCKET,
				     sending ? ZSOCK_SO_SNDTIMEO : ZSOCK_SO_RCVTIMEO, &timeout,
				     sizeof(timeout))) {
			return -errno;
		}
		count = sending ? zsock_send(fd, data + offset, size - offset, 0)
				: zsock_recv(fd, data + offset, size - offset, 0);
		if (count <= 0) {
			return count < 0 ? -errno : -ECONNRESET;
		}
		offset += count;
	}
	return 0;
}

static int echo_test(int fd)
{
	uint8_t payload[256];
	uint8_t reply[256];
	int ret;

	for (int sequence = 0; sequence < 4; sequence++) {
		for (size_t i = 0; i < sizeof(payload); i++) {
			payload[i] = (i * 31 + sequence) & 0xff;
		}
		ret = socket_exact(fd, payload, sizeof(payload), true);
		if (!ret) {
			ret = socket_exact(fd, reply, sizeof(reply), false);
		}
		if (ret || memcmp(payload, reply, sizeof(payload))) {
			return ret ? ret : -EBADMSG;
		}
	}
	printk("P4L TLS_ECHO_OK frames=4 bytes=1024\n");
	return 0;
}

static int drop_test(int fd)
{
	struct timeval timeout = {.tv_sec = 5};
	uint8_t marker[] = {'P', '4', 'F', 'D'};
	uint8_t reply;
	ssize_t count;
	int ret;

	ret = socket_exact(fd, marker, sizeof(marker), true);
	if (ret) {
		return ret;
	}
	if (zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &timeout, sizeof(timeout))) {
		return -errno;
	}
	count = zsock_recv(fd, &reply, sizeof(reply), 0);
	if (count != 0) {
		ret = count < 0 ? -errno : -EBADMSG;
		printk("P4L FAIL TLS controlled EOF rc=%d\n", ret);
		return ret;
	}
	printk("P4L TLS_DROP_OK authenticated_eof=1\n");
	return 0;
}

static int connect_test(uint16_t port, const char *hostname, uint32_t expected_flags,
			enum tls_test_action action)
{
	struct verify_state state = {0};
	struct net_sockaddr_in server = config.server;
	int fd;
	int ret;
	int64_t started;
	int64_t elapsed;

	fd = tls_socket(hostname, &state);
	if (fd < 0) {
		printk("P4L FAIL TLS socket setup rc=%d\n", fd);
		return fd;
	}
	server.sin_port = sys_cpu_to_be16(port);
	started = k_uptime_get();
	ret = zsock_connect(fd, (struct net_sockaddr *)&server, sizeof(server));
	elapsed = k_uptime_get() - started;
	if (action == TLS_TEST_TIMEOUT) {
		if (ret >= 0 || errno != ETIMEDOUT || state.calls ||
		    elapsed < CONFIG_NET_SOCKETS_TLS_CONNECT_TIMEOUT - 100 ||
		    elapsed > CONFIG_NET_SOCKETS_TLS_CONNECT_TIMEOUT + 2000) {
			printk("P4L FAIL TLS timeout rc=%d errno=%d elapsed=%lld calls=%u\n", ret,
			       errno, (long long)elapsed, state.calls);
			ret = -EIO;
		} else {
			printk("P4L TLS_TIMEOUT_OK elapsed_ms=%lld\n", (long long)elapsed);
			ret = 0;
		}
	} else if (expected_flags) {
		if (ret >= 0 || !state.calls || state.flags != expected_flags) {
			printk("P4L FAIL TLS reject rc=%d flags=%x expected=%x\n", ret, state.flags,
			       expected_flags);
			ret = -EACCES;
		} else {
			printk("P4L TLS_REJECT_OK flags=%x\n", state.flags);
			ret = 0;
		}
	} else if (ret < 0) {
		ret = -errno;
		printk("P4L FAIL TLS connect rc=%d flags=%x calls=%u\n", ret, state.flags,
		       state.calls);
	} else if (!state.calls || state.flags) {
		ret = -EACCES;
	} else if (action == TLS_TEST_DROP) {
		ret = drop_test(fd);
	} else {
		ret = echo_test(fd);
	}
	if (zsock_close(fd)) {
		printk("P4L FAIL TLS close rc=%d\n", -errno);
		if (!ret) {
			ret = -EIO;
		}
	}
	return ret;
}

static int fault_tests(uint16_t port)
{
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint8_t ack[4];
	int ret;

	printk("P4L TLS_STALL_CONFIG_WAIT\n");
	ret = p4_uart_read_exact(uart, ack, sizeof(ack), sys_timepoint_calc(K_SECONDS(30)));
	if (ret || memcmp(ack, "P4F1", sizeof(ack))) {
		return ret ? ret : -EINVAL;
	}
	ret = connect_test(port, config.hostname, 0, TLS_TEST_TIMEOUT);
	if (!ret) {
		ret = connect_test(port, config.hostname, 0, TLS_TEST_ECHO);
	}
	if (!ret) {
		ret = connect_test(port, config.hostname, 0, TLS_TEST_DROP);
	}
	if (!ret) {
		ret = connect_test(port, config.hostname, 0, TLS_TEST_ECHO);
	}
	return ret;
}

int p4_tls_test(void)
{
	bool credential_added = false;
	bool dns_configured = false;
	uint16_t port;
	int cleanup;
	int ret;

	memset(&config, 0, sizeof(config));
	ret = read_config();
	if (ret) {
		goto out;
	}
	ret = tls_credential_add(TEST_CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE, config.ca,
				 config.ca_length);
	if (ret) {
		goto out;
	}
	credential_added = true;
	ret = configure_dns();
	if (ret) {
		goto out;
	}
	dns_configured = true;
	ret = dns_test();
	port = sys_be16_to_cpu(config.server.sin_port);
	if (!ret) {
		ret = connect_test(port, config.hostname, 0, TLS_TEST_ECHO);
	}
	if (!ret) {
		ret = connect_test(port, "wrong.p03.entelea.test", MBEDTLS_X509_BADCERT_CN_MISMATCH,
				   TLS_TEST_ECHO);
	}
	if (!ret) {
		ret = connect_test(config.untrusted_port, config.hostname,
				   MBEDTLS_X509_BADCERT_NOT_TRUSTED, TLS_TEST_ECHO);
	}
	if (!ret) {
		ret = connect_test(config.expired_port, config.hostname,
				   MBEDTLS_X509_BADCERT_EXPIRED, TLS_TEST_ECHO);
	}
	if (!ret) {
		ret = fault_tests(port);
	}
out:
	if (dns_configured) {
		cleanup = dns_resolve_close(dns_resolve_get_default());
		if (cleanup) {
			printk("P4L FAIL DNS cleanup rc=%d\n", cleanup);
			if (!ret) {
				ret = cleanup;
			}
		}
	}
	if (credential_added) {
		cleanup = tls_credential_delete(TEST_CA_TAG, TLS_CREDENTIAL_CA_CERTIFICATE);
		if (cleanup) {
			printk("P4L FAIL CA cleanup rc=%d\n", cleanup);
			if (!ret) {
				ret = cleanup;
			}
		}
	}
	mbedtls_platform_zeroize(&config, sizeof(config));
#if defined(CONFIG_THREAD_ANALYZER)
	thread_analyzer_print(0);
#endif
	printk("P4L %s dns_tls_echo_certificates\n", ret ? "FAIL" : "PASS");
	return ret;
}
