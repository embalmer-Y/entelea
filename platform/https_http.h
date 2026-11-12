/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_HTTP_H
#define ENTELEA_HTTPS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static unsigned char https_http_lower(unsigned char c)
{
	return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool https_http_equal(const char *begin, const char *end,
			     const char *expected)
{
	while (begin < end && *expected != '\0') {
		if (https_http_lower((unsigned char)*begin++) !=
		    https_http_lower((unsigned char)*expected++)) {
			return false;
		}
	}
	return begin == end && *expected == '\0';
}

static bool https_http_token(const char *value, size_t capacity, bool empty_ok)
{
	const char *end = memchr(value, '\0', capacity);

	if (end == NULL || (!empty_ok && end == value)) {
		return false;
	}
	for (const char *p = value; p < end; p++) {
		if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7f) {
			return false;
		}
	}
	return true;
}

static char *https_http_field_colon(char *begin, char *end)
{
	char *colon = memchr(begin, ':', (size_t)(end - begin));

	if (colon == NULL || colon == begin) {
		return NULL;
	}
	for (char *p = begin; p < colon; p++) {
		unsigned char c = (unsigned char)*p;

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
		      (c >= 'A' && c <= 'Z') ||
		      strchr("!#$%&'*+-.^_`|~", c) != NULL)) {
			return NULL;
		}
	}
	for (char *p = colon + 1; p < end; p++) {
		unsigned char c = (unsigned char)*p;

		if ((c < 0x20 && c != '\t') || c == 0x7f) {
			return NULL;
		}
	}
	return colon;
}

static bool https_http_dechunk(char *payload, char *end)
{
	char *p = payload;
	char *dst = payload;

	for (;;) {
		size_t length = 0;
		size_t digits = 0;

		while (p < end) {
			unsigned char c = (unsigned char)*p;
			unsigned int digit;

			if (c >= '0' && c <= '9') {
				digit = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				digit = c - 'a' + 10U;
			} else if (c >= 'A' && c <= 'F') {
				digit = c - 'A' + 10U;
			} else {
				break;
			}
			if (length > (SIZE_MAX - digit) / 16U) {
				return false;
			}
			length = length * 16U + digit;
			digits++;
			p++;
		}
		if (digits == 0) {
			return false;
		}
		if (p < end && *p == ';') {
			while (p < end && *p != '\r') {
				if (((unsigned char)*p < 0x20 && *p != '\t') ||
				    (unsigned char)*p == 0x7f) {
					return false;
				}
				p++;
			}
		}
		if (end - p < 2 || p[0] != '\r' || p[1] != '\n') {
			return false;
		}
		p += 2;
		if (length == 0) {
			for (;;) {
				char *line = p;

				while (end - p >= 2 &&
				       !(p[0] == '\r' && p[1] == '\n')) {
					p++;
				}
				if (end - p < 2) {
					return false;
				}
				if (line == p) {
					p += 2;
					if (p != end) {
						return false;
					}
					*dst = '\0';
					return true;
				}
				if (https_http_field_colon(line, p) == NULL) {
					return false;
				}
				p += 2;
			}
		}
		if (length > (size_t)(end - p)) {
			return false;
		}
		memmove(dst, p, length);
		dst += length;
		p += length;
		if (end - p < 2 || p[0] != '\r' || p[1] != '\n') {
			return false;
		}
		p += 2;
	}
}

/* Buffer has room for a terminator at response[length]. */
static bool https_http_payload(char *response, size_t length,
			       bool peer_closed_cleanly, int *status, char **payload)
{
	char *end = response + length;
	char *line_end;
	char *p;
	bool have_length = false;
	bool chunked = false;
	size_t content_length = 0;

	if (memchr(response, '\0', length) != NULL) {
		return false;
	}
	response[length] = '\0';
	line_end = strstr(response, "\r\n");
	if (line_end == NULL || line_end - response < 12 ||
	    memcmp(response, "HTTP/1.", 7) != 0 ||
	    (response[7] != '0' && response[7] != '1') ||
	    response[8] != ' ' ||
	    response[9] < '0' || response[9] > '9' ||
	    response[10] < '0' || response[10] > '9' ||
	    response[11] < '0' || response[11] > '9' ||
	    (line_end - response > 12 && response[12] != ' ')) {
		return false;
	}
	*status = (response[9] - '0') * 100 +
		  (response[10] - '0') * 10 + (response[11] - '0');
	if (*status < 100 || *status > 599) {
		return false;
	}
	for (char *q = response + 12; q < line_end; q++) {
		unsigned char c = (unsigned char)*q;

		if ((c < 0x20 && c != '\t') || c == 0x7f) {
			return false;
		}
	}
	p = line_end + 2;
	for (;;) {
		line_end = strstr(p, "\r\n");
		if (line_end == NULL) {
			return false;
		}
		if (line_end == p) {
			p += 2;
			break;
		}
		char *colon = https_http_field_colon(p, line_end);

		if (colon == NULL) {
			return false;
		}
		char *value = colon + 1;
		char *value_end = line_end;

		while (value < value_end && (*value == ' ' || *value == '\t')) {
			value++;
		}
		while (value_end > value &&
		       (value_end[-1] == ' ' || value_end[-1] == '\t')) {
			value_end--;
		}
		if (https_http_equal(p, colon, "Content-Length")) {
			if (have_length || value == value_end) {
				return false;
			}
			for (char *q = value; q < value_end; q++) {
				if (*q < '0' || *q > '9') {
					return false;
				}
				unsigned int digit = (unsigned int)(*q - '0');

				if (content_length > (SIZE_MAX - digit) / 10U) {
					return false;
				}
				content_length = content_length * 10U + digit;
			}
			have_length = true;
		} else if (https_http_equal(p, colon, "Transfer-Encoding")) {
			if (chunked || !https_http_equal(value, value_end, "chunked")) {
				return false;
			}
			chunked = true;
		} else if (https_http_equal(p, colon, "Content-Encoding") &&
			   !https_http_equal(value, value_end, "identity")) {
			return false;
		}
		p = line_end + 2;
	}
	if (chunked && have_length) {
		return false;
	}
	if (*status == 200) {
		if (chunked) {
			if (!https_http_dechunk(p, end)) {
				return false;
			}
		} else if (have_length && content_length != (size_t)(end - p)) {
			return false;
		} else if (!have_length && !peer_closed_cleanly) {
			return false;
		}
	}
	*payload = p;
	return true;
}

#endif /* ENTELEA_HTTPS_HTTP_H */
