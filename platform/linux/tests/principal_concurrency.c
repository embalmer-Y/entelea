/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <entelea/events.h>
#include <entelea/principal.h>

static sem_t resolved, release_reader, writer_started, writer_done;
static entelea_principal_t original, live, replacement;
static entelea_perm_table_t privileged;
static entelea_result_t reader_result, bind_result;
static bool check_msg;

entelea_result_t
entelea_evt_publish_copy(const entelea_event_t *evt)
{
	(void) evt;
	return ENTELEA_OK;
}

static void
require(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void
wait_for(sem_t *sem)
{
	int r;

	do {
		r = sem_wait(sem);
	} while (r != 0 && errno == EINTR);
	require(r == 0, "sem_wait");
}

static void
after_resolve(void)
{
	require(sem_post(&resolved) == 0, "signal resolved");
	wait_for(&release_reader);
}

static void *
reader(void *unused)
{
	(void) unused;
	if (check_msg) {
		reader_result = entelea_perm_check_msg(original, "target.one");
	} else {
		reader_result = entelea_perm_check(original, ENTELEA_PERM_POWER,
						   ENTELEA_PERM_WRITE, 0);
	}
	return NULL;
}

static void *
writer(void *unused)
{
	(void) unused;
	require(sem_post(&writer_started) == 0, "signal writer started");
	entelea_principal_unbind(&live);
	bind_result = entelea_principal_bind(&replacement, 11, &privileged);
	require(sem_post(&writer_done) == 0, "signal writer finished");
	return NULL;
}

static void
run_interleaving(bool msg)
{
	pthread_t reader_thread, writer_thread;
	entelea_perm_table_t empty;
	struct timespec deadline;
	int wait_result;

	check_msg = msg;
	entelea_perm_test_reset();
	entelea_perm_table_init(&empty);
	entelea_perm_table_init(&privileged);
	require(entelea_perm_parse("power:write:0", &privileged) == ENTELEA_OK,
		"parse power grant");
	require(entelea_perm_parse("msg:send:target.one", &privileged) ==
			ENTELEA_OK,
		"parse message grant");
	require(entelea_principal_bind(&live, 11, &empty) == ENTELEA_OK,
		"bind original principal");
	original = live;
	require(sem_init(&resolved, 0, 0) == 0 &&
			sem_init(&release_reader, 0, 0) == 0 &&
			sem_init(&writer_started, 0, 0) == 0 &&
			sem_init(&writer_done, 0, 0) == 0,
		"init semaphores");
	entelea_perm_test_after_resolve(after_resolve);
	require(pthread_create(&reader_thread, NULL, reader, NULL) == 0,
		"start reader");
	wait_for(&resolved);
	require(pthread_create(&writer_thread, NULL, writer, NULL) == 0,
		"start writer");
	wait_for(&writer_started);
	require(clock_gettime(CLOCK_REALTIME, &deadline) == 0,
		"read deadline clock");
	deadline.tv_sec++;
	do {
		wait_result = sem_timedwait(&writer_done, &deadline);
	} while (wait_result != 0 && errno == EINTR);
	bool writer_completed_while_reader_paused = wait_result == 0;

	require(writer_completed_while_reader_paused || errno == ETIMEDOUT,
		"wait for writer");
	require(sem_post(&release_reader) == 0, "release reader");
	require(pthread_join(reader_thread, NULL) == 0 &&
			pthread_join(writer_thread, NULL) == 0,
		"join workers");
	entelea_perm_test_after_resolve(NULL);
	require(bind_result == ENTELEA_OK, "bind replacement principal");
	require(reader_result == ENTELEA_E_PERM,
		msg ? "stale message check inherited replacement grant" :
		      "stale capability check inherited replacement grant");
	require(!writer_completed_while_reader_paused,
		"rebinding crossed an in-flight permission decision");
	require(entelea_perm_check(original, ENTELEA_PERM_POWER,
				   ENTELEA_PERM_WRITE, 0) == ENTELEA_E_PERM,
		"old principal remains invalid");
	require(entelea_perm_check(replacement, ENTELEA_PERM_POWER,
				   ENTELEA_PERM_WRITE, 0) == ENTELEA_OK,
		"replacement principal remains valid");
	entelea_principal_unbind(&replacement);
	require(sem_destroy(&resolved) == 0 &&
			sem_destroy(&release_reader) == 0 &&
			sem_destroy(&writer_started) == 0 &&
			sem_destroy(&writer_done) == 0,
		"destroy semaphores");
}

static int
run_native_guess_probe(void)
{
	entelea_perm_table_t table;
	entelea_principal_t actual;
	const entelea_principal_t guessed = { ._handle = 1,
					      ._gen = 1,
					      .app_id = 42 };

	entelea_perm_table_init(&table);
	require(entelea_perm_parse("gpio:read:0", &table) == ENTELEA_OK,
		"parse test grant");
	require(entelea_principal_bind(&actual, 42, &table) == ENTELEA_OK,
		"bind test principal");
	entelea_result_t result = entelea_perm_check(guessed, ENTELEA_PERM_GPIO,
						     ENTELEA_PERM_READ, 0);

	if (result == ENTELEA_OK) {
		fprintf(stderr, "FAIL: guessed same-address native principal "
				"accepted\n");
		return 1;
	}
	require(result == ENTELEA_E_PERM, "unexpected native guess result");
	puts("native guess rejected in this scenario; not an isolation proof");
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc == 1) {
		run_interleaving(false);
		run_interleaving(true);
	} else if (argc == 2 && strcmp(argv[1], "msg") == 0) {
		run_interleaving(true);
	} else if (argc == 2 && strcmp(argv[1], "native-guess") == 0) {
		return run_native_guess_probe();
	} else {
		fprintf(stderr,
			"usage: principal-concurrency [msg|native-guess]\n");
		return 2;
	}
	puts("principal concurrency: PASS");
	return 0;
}
