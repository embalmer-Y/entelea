/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real diagnostic with synthetic credentials and no networking. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mbedtls/net_sockets.h>
#include <entelea/guard.h>
#include <entelea/time.h>

#ifndef CLOUD_DIAGNOSTIC_SOURCE
#define CLOUD_DIAGNOSTIC_SOURCE "../cloud_main.c"
#endif
#define main entelea_cloud_main_under_test
#include CLOUD_DIAGNOSTIC_SOURCE
#undef main

static int mock_network_calls;
static int mock_model_calls;
static bool mock_unbound;
static entelea_https_cfg_t *mock_cfg;

void
mbedtls_net_init(mbedtls_net_context *net)
{
	net->fd = -1;
}
void
mbedtls_net_free(mbedtls_net_context *net)
{
	net->fd = -1;
}
int
mbedtls_net_connect(mbedtls_net_context *net, const char *host,
		    const char *port, int proto)
{
	(void) net;
	(void) host;
	(void) port;
	(void) proto;
	mock_network_calls++;
	return MBEDTLS_ERR_NET_CONNECT_FAILED;
}

entelea_result_t
entelea_platform_clock_init(entelea_clock_t *clock)
{
	(void) clock;
	return ENTELEA_OK;
}

entelea_result_t
entelea_clock_now(entelea_clock_t *clock, entelea_instant_t *out)
{
	(void) clock;
	*out = (entelea_instant_t) { .ms = 1, .valid = true };
	return ENTELEA_OK;
}

entelea_result_t
entelea_guard_bind_clock(const entelea_clock_t *clock)
{
	(void) clock;
	return ENTELEA_OK;
}

uint32_t
entelea_guard_model_calls(void)
{
	return 0;
}

entelea_result_t
entelea_dep_report(entelea_dep_kind_t kind, bool good, uint64_t now_ms)
{
	(void) kind;
	(void) good;
	(void) now_ms;
	return ENTELEA_OK;
}

entelea_result_t
entelea_dep_mark_down(entelea_dep_kind_t kind)
{
	(void) kind;
	return ENTELEA_OK;
}

void
entelea_model_bind(const entelea_model_transport_t *ops, void *user)
{
	if (ops != NULL) {
		mock_cfg = user;
	} else {
		mock_unbound = true;
	}
}

bool
entelea_model_ready(void)
{
	return false;
}

entelea_result_t
entelea_model_ask(const entelea_model_msg_t *msgs, uint8_t n, char *out,
		  size_t out_max, uint32_t timeout,
		  entelea_model_usage_t *usage)
{
	(void) msgs;
	(void) n;
	(void) out;
	(void) out_max;
	(void) timeout;
	(void) usage;
	mock_model_calls++;
	return ENTELEA_E_STATE;
}

entelea_result_t
entelea_model_probe(const entelea_model_msg_t *msgs, uint8_t n, char *out,
		    size_t out_max, uint32_t timeout,
		    entelea_model_usage_t *usage)
{
	return entelea_model_ask(msgs, n, out, out_max, timeout, usage);
}

entelea_result_t
entelea_https_chat(void *user, const entelea_model_msg_t *msgs, uint8_t n,
		   char *out, size_t out_max, uint32_t timeout,
		   entelea_model_usage_t *usage)
{
	(void) user;
	return entelea_model_ask(msgs, n, out, out_max, timeout, usage);
}

static bool
run_case(const char *doc, size_t size, bool valid)
{
	char path[] = "/tmp/entelea-cloud-probe-XXXXXX";
	char output[8192];
	int fd = mkstemp(path);
	FILE *fixture;
	FILE *capture;
	int saved_out, saved_err, rc;
	size_t n;

	if (fd < 0 || (fixture = fdopen(fd, "wb")) == NULL) {
		if (fd >= 0) {
			close(fd);
			unlink(path);
		}
		return false;
	}
	bool wrote = fwrite(doc, 1, size, fixture) == size;
	int close_result = fclose(fixture);

	if (!wrote || close_result != 0) {
		unlink(path);
		return false;
	}
	capture = tmpfile();
	saved_out = dup(STDOUT_FILENO);
	saved_err = dup(STDERR_FILENO);
	if (capture == NULL || saved_out < 0 || saved_err < 0) {
		unlink(path);
		if (capture != NULL)
			fclose(capture);
		if (saved_out >= 0)
			close(saved_out);
		if (saved_err >= 0)
			close(saved_err);
		return false;
	}
	fflush(NULL);
	if (dup2(fileno(capture), STDOUT_FILENO) < 0 ||
	    dup2(fileno(capture), STDERR_FILENO) < 0) {
		(void) dup2(saved_out, STDOUT_FILENO);
		(void) dup2(saved_err, STDERR_FILENO);
		close(saved_out);
		close(saved_err);
		fclose(capture);
		unlink(path);
		return false;
	}
	failures = 0;
	mock_network_calls = 0;
	mock_model_calls = 0;
	mock_unbound = false;
	mock_cfg = NULL;
	char *argv[] = { "cloud", path, NULL };

	rc = entelea_cloud_main_under_test(2, argv);
	fflush(NULL);
	(void) dup2(saved_out, STDOUT_FILENO);
	(void) dup2(saved_err, STDERR_FILENO);
	close(saved_out);
	close(saved_err);
	rewind(capture);
	n = fread(output, 1, sizeof(output) - 1, capture);
	output[n] = '\0';
	fclose(capture);
	unlink(path);

	if (rc != 1 || mock_model_calls != 0 || !mock_unbound ||
	    mock_network_calls != (valid ? 1 : 0) ||
	    (mock_cfg != NULL) != valid ||
	    strstr(output, "synthetic-secret") != NULL ||
	    strstr(output, "synthetic.example") != NULL ||
	    strstr(output, "private-path") != NULL ||
	    strstr(output, "synthetic-model") != NULL) {
		return false;
	}
	if (valid) {
		const unsigned char *p = (const unsigned char *) mock_cfg;

		for (size_t i = 0; i < sizeof(*mock_cfg); i++) {
			if (p[i] != 0)
				return false;
		}
	}
	return true;
}

int
main(void)
{
	static const char valid[] =
		"{\"llm_api_key\":\"synthetic-secret\","
		"\"llm_base_url\":\"https://synthetic.example/private-path\","
		"\"allowed_models\":[\"synthetic-model\"]}";
	static const char malformed[] =
		"{\"llm_api_key\":\"synthetic-secret\","
		"\"llm_base_url\":\"https://synthetic.example/private-path\","
		"\"allowed_models\":[\"synthetic-model\"]";
	static const char wrong_array[] =
		"{\"llm_api_key\":\"synthetic-secret\","
		"\"llm_base_url\":\"https://synthetic.example/private-path\","
		"\"allowed_models\":[42,\"synthetic-model\"]}";
	static const char duplicate[] =
		"{\"llm_api_key\":\"synthetic-secret\",\"llm_api_key\":"
		"\"other\","
		"\"llm_base_url\":\"https://synthetic.example/private-path\","
		"\"allowed_models\":[\"synthetic-model\"]}";
	static const char embedded_nul[] =
		"{\"llm_api_key\":\"synthetic-secret\"}\0"
		"{\"llm_base_url\":\"https://synthetic.example/private-path\"}";
	char long_key[140];
	char oversized[512];

	memset(long_key, 'x', sizeof(long_key) - 1);
	long_key[sizeof(long_key) - 1] = '\0';
	int amount = snprintf(oversized, sizeof(oversized),
			      "{\"llm_api_key\":\"%s\",\"llm_base_url\":"
			      "\"https://synthetic.example/private-path\","
			      "\"allowed_models\":[\"synthetic-model\"]}",
			      long_key);
	if (amount < 0 || (size_t) amount >= sizeof(oversized) ||
	    !run_case(valid, sizeof(valid) - 1, true) ||
	    !run_case(malformed, sizeof(malformed) - 1, false) ||
	    !run_case(wrong_array, sizeof(wrong_array) - 1, false) ||
	    !run_case(duplicate, sizeof(duplicate) - 1, false) ||
	    !run_case(embedded_nul, sizeof(embedded_nul) - 1, false) ||
	    !run_case(oversized, (size_t) amount, false)) {
		fprintf(stderr,
			"FAIL: offline cloud diagnostic input/cleanup\n");
		return 1;
	}
	puts("PASS: offline cloud diagnostic input/cleanup (6 cases)");
	return 0;
}
