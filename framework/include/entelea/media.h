/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_MEDIA_H
#define ENTELEA_MEDIA_H

/* R09 optional media interface contract: see
 * framework/services/media/contract.md.
 * Image, PCM, and display are independently optional. Capture is not storage,
 * egress, or memory. The fake backend validates the interface and the
 * lifecycle, permission, and cancellation semantics; the real backend follows
 * in the board-level batch. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <entelea/principal.h>
#include <entelea/result.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENTELEA_MEDIA_FRAME_MAX 512 /* mechanism frame limit (suggested) */

typedef enum {
	ENTELEA_MEDIA_IDLE = 0,
	ENTELEA_MEDIA_CAPTURING,
	ENTELEA_MEDIA_FAILED,
} entelea_media_state_t;

typedef struct {
	uint64_t t_ms;
	uint32_t seq;
	uint8_t bytes[ENTELEA_MEDIA_FRAME_MAX];
	uint32_t len;
} entelea_media_frame_t;

/* Media classes. Each class has its own binding and instance; per plan L165,
 * a single global binding must not leak permissions across classes. */
typedef enum {
	ENTELEA_MEDIA_IMAGE = 0, /* image (camera frames) */
	ENTELEA_MEDIA_PCM, /* audio (PCM samples) */
	ENTELEA_MEDIA_DISPLAY, /* display (playback deferred to a later batch;
				  * binding/lifecycle mirror capture classes */
	ENTELEA_MEDIA_CLASS_COUNT,
} entelea_media_class_t;

/* Backend ops (a fake backend is injected in tests; the real backend is a
 * board-level implementation with the same shape). */
typedef struct {
	/* Starts capture (failure = E_IO); frame_ready polling surface. */
	entelea_result_t (*start)(void *user);
	/* Polls one frame (E_BUSY = no frame yet; failure sets FAILED). */
	entelea_result_t (*poll)(void *user, entelea_media_frame_t *out);
	/* Stops (idempotent). A read in progress during cancellation
	 * must not revive. */
	void (*stop)(void *user);
	entelea_media_state_t (*state)(void *user);
} entelea_media_backend_t;

/** [thread] Binds a backend. Each media class has its own slot; rebinding
 * while the current backend is not IDLE returns E_BUSY. After a stop, the
 * backend can be replaced without affecting other classes. A class outside
 * the range returns E_PARAM. The caller must serialize operations on the
 * same class. */
entelea_result_t
entelea_media_bind(entelea_media_class_t cls, entelea_media_backend_t backend,
		   void *user);

/** [thread] Starts capture: permission gate (av:read declaration), then the
 * backend start. This grants no saving or egress: this surface only captures,
 * and saving or egress are separate permissions on separate surfaces. */
entelea_result_t
entelea_media_capture_start(entelea_media_class_t cls, entelea_principal_t ctx);

/** [thread] Polls a frame. No frame returns E_BUSY (not a failure); a backend
 * failure sets the FAILED state. */
entelea_result_t
entelea_media_poll(entelea_media_class_t cls, entelea_media_frame_t *out);

/** [thread] Stops (idempotent). After a stop, poll returns E_STATE and does
 * not revive. */
void
entelea_media_capture_stop(entelea_media_class_t cls);

/** [any] State (for observation). An unbound class reports IDLE and does not
 * pretend to be available. */
entelea_media_state_t
entelea_media_state(entelea_media_class_t cls);

#ifdef ENTELEA_TEST
void
entelea_media_test_reset(void);
/* Fake backend injection (tests). */
extern entelea_media_backend_t entelea_media_fake_backend;
/* Fake backend control surface: state is independent per class, for
 * checking independence. */
void
entelea_media_fake_set_frames(entelea_media_class_t cls, const uint8_t *data,
			      uint32_t len, uint32_t count);
void
entelea_media_fake_fail_next(entelea_media_class_t cls);
uint32_t
entelea_media_fake_start_calls(entelea_media_class_t cls);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ENTELEA_MEDIA_H */
