/* SPDX-License-Identifier: Apache-2.0 */
/* F-2 real cloud gate (Linux host surface): bind the real model transport
 * implementation -> dependency linkage UP -> real router call (TLS+chat)
 * -> content/usage assertions -> connectivity-loss control (NET DOWN: ask
 * is refused with E_STATE and does **not** touch the transport, as shown by
 * the touched counter) -> recovery surface. Credentials are read at runtime
 * from ENTELEA_CLOUD_CRED (0600 json, outside the repository); they are
 * never embedded in the binary or logged (only key names and lengths are
 * printed).
 * The MCU surface blocked by C5 is in the r06 validation record, F batch
 * section (deleted; anchor in the ledger; on-site items). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mbedtls/net_sockets.h>
#include <entelea/guard.h>
#include <entelea/model.h>
#include <entelea/netmon.h>
#include "../https_json.h"
#include "../secure_zero.h"
#include "clock.h"
#include "guard_ports.h"
#include "https_chat.h"

static int failures;

#define CHECK(expr)                                                           \
	do {                                                                  \
		if (!(expr)) {                                                \
			printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr); \
			failures++;                                           \
		}                                                             \
	} while (0)

static bool
json_str(const char *doc, const char *key, char *out, size_t n)
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

static bool
parse_base_url(const char *url, char *host, size_t hn, char *prefix, size_t pn)
{
	if (strncmp(url, "https://", 8) != 0)
		return false;
	const char *h = url + 8, *slash = strchr(h, '/');

	if (!slash) {
		int host_len = snprintf(host, hn, "%s", h);
		int prefix_len = snprintf(prefix, pn, "/");

		if (host_len < 0 || (size_t) host_len >= hn || prefix_len < 0 ||
		    (size_t) prefix_len >= pn)
			return false;
	} else {
		size_t hl = (size_t) (slash - h);
		if (hl >= hn)
			return false;
		memcpy(host, h, hl);
		host[hl] = '\0';
		int prefix_len = snprintf(prefix, pn, "%s", slash);

		if (prefix_len < 0 || (size_t) prefix_len >= pn)
			return false;
	}
	/* Strip the trailing slash (normalize the path prefix). */
	size_t l = strlen(prefix);
	if (l > 1 && prefix[l - 1] == '/')
		prefix[l - 1] = '\0';
	return host[0] != '\0';
}

static bool
report_observation(entelea_clock_t *clock, entelea_dep_kind_t kind, bool good)
{
	entelea_instant_t now;

	return entelea_clock_now(clock, &now) == ENTELEA_OK &&
	       entelea_dep_report(kind, good, now.ms) == ENTELEA_OK;
}

static bool
network_observation(const char *host)
{
	mbedtls_net_context net;

	mbedtls_net_init(&net);
	int ret = mbedtls_net_connect(&net, host, "443", MBEDTLS_NET_PROTO_TCP);

	mbedtls_net_free(&net);
	return ret == 0;
}

static bool
report_network_pair(entelea_clock_t *clock, const char *host)
{
	const struct timespec gap = { .tv_sec = 1, .tv_nsec = 0 };

	for (int i = 0; i < 2; i++) {
		bool good = network_observation(host);

		if (!report_observation(clock, ENTELEA_DEP_NET, good) ||
		    !good) {
			return false;
		}
		if (i == 0 && nanosleep(&gap, NULL) != 0) {
			(void) entelea_dep_mark_down(ENTELEA_DEP_NET);
			return false;
		}
	}
	return true;
}

int
main(int argc, char **argv)
{
	static char doc[4096];
	static entelea_https_cfg_t cfg;
	char api_key[128] = "", base_url[128] = "", model[48] = "";
	char out[4096] = "";
	int exit_status = 1;
	const char *cred_path =
		argc > 1 ? argv[1] :
			   "/home/emb/project/.cloud-credentials.json";
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
	static entelea_clock_t guard_clock;
	static entelea_lock_t guard_lock;
	static entelea_wallclock_t guard_wall;
	static entelea_guard_store_t guard_store;

	/* RF1 G5: model_ask passes the D12 gate on each attempt (since r16;
	 * any unbound port fails closed).
	 * Host persistence is in-process memory: the daily count is not kept
	 * after the process exits (see guard_ports.c). */
	CHECK(entelea_platform_clock_init(&guard_clock) == ENTELEA_OK);
	CHECK(entelea_guard_bind_clock(&guard_clock) == ENTELEA_OK);
	CHECK(entelea_platform_guard_lock_init(&guard_lock) == ENTELEA_OK);
	CHECK(entelea_guard_bind_lock(&guard_lock) == ENTELEA_OK);
	CHECK(entelea_platform_guard_wallclock_init(&guard_wall) == ENTELEA_OK);
	CHECK(entelea_guard_bind_wallclock(&guard_wall) == ENTELEA_OK);
	CHECK(entelea_platform_guard_store_init(&guard_store) == ENTELEA_OK);
	CHECK(entelea_guard_bind_store(&guard_store) == ENTELEA_OK);
	entelea_model_bind(&ops, &cfg);
	if (failures != 0 || !report_network_pair(&guard_clock, cfg.host)) {
		fprintf(stderr, "[cloud] network observation failed\n");
		goto cleanup;
	}

	/* Two recovery probes plus one normal call. Actual cost is recorded
	 * in cumulative usage and call count. */
	const entelea_model_msg_t msgs[] = {
		{ .role = "user",
		  .content = "Reply with exactly the word: ok. "
			     "Output nothing else." },
	};
	entelea_model_usage_t usage = { 0 };
	entelea_result_t r;

	/* Only two real probes, each passing the gate, yield two MODEL
	 * recovery observations. */
	for (int i = 0; i < 2; i++) {
		int before = touched;

		r = entelea_model_probe(msgs, 1, out, sizeof(out), 30000,
					&usage);
		if (touched == before || r != ENTELEA_OK || out[0] == '\0' ||
		    !report_observation(&guard_clock, ENTELEA_DEP_MODEL,
					true)) {
			(void) entelea_dep_mark_down(ENTELEA_DEP_MODEL);
			fprintf(stderr,
				"[cloud] model recovery probe failed rc=%d\n",
				(int) r);
			goto cleanup;
		}
	}
	CHECK(entelea_model_ready());
	if (failures != 0) {
		goto cleanup;
	}
	r = entelea_model_ask(msgs, 1, out, sizeof(out), 30000, &usage);

	printf("[cloud] ask rc=%d out='%.64s' usage=%u+%u touched=%d\n",
	       (int) r, out, usage.prompt_tokens, usage.completion_tokens,
	       touched);
	CHECK(r == ENTELEA_OK);
	CHECK(out[0] != '\0'); /* Non-empty content (model-dependent text) */
	CHECK(touched >= 3);
	CHECK(entelea_guard_model_calls() == (uint32_t) touched); /* Per try */
	CHECK(usage.prompt_tokens > 0);

	/* Simulated connectivity-loss control: with NET DOWN, ask is refused
	 * with E_STATE and does not touch the transport. */
	int before = touched;
	CHECK(report_observation(&guard_clock, ENTELEA_DEP_NET, false));
	CHECK(!entelea_model_ready());
	r = entelea_model_ask(msgs, 1, out, sizeof(out), 5000, NULL);
	CHECK(r == ENTELEA_E_STATE);
	CHECK(touched == before);

	/* Recovery surface: two real network observations. */
	CHECK(report_network_pair(&guard_clock, cfg.host));
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
