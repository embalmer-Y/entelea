/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_HTTPS_JSON_H
#define ENTELEA_HTTPS_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static size_t
https_json_utf8_length(const unsigned char *p)
{
	unsigned char c = p[0];
	unsigned char b1 = p[1];

	if (c >= 0xc2 && c <= 0xdf) {
		return b1 >= 0x80 && b1 <= 0xbf ? 2 : 0;
	}
	if (c >= 0xe0 && c <= 0xef) {
		if (b1 < 0x80 || b1 > 0xbf || (c == 0xe0 && b1 < 0xa0) ||
		    (c == 0xed && b1 > 0x9f)) {
			return 0;
		}
		unsigned char b2 = p[2];

		return b2 >= 0x80 && b2 <= 0xbf ? 3 : 0;
	}
	if (c >= 0xf0 && c <= 0xf4) {
		if (b1 < 0x80 || b1 > 0xbf || (c == 0xf0 && b1 < 0x90) ||
		    (c == 0xf4 && b1 > 0x8f)) {
			return 0;
		}
		unsigned char b2 = p[2];

		if (b2 < 0x80 || b2 > 0xbf) {
			return 0;
		}
		unsigned char b3 = p[3];

		return b3 >= 0x80 && b3 <= 0xbf ? 4 : 0;
	}
	return 0;
}

static inline bool
https_json_escape(const char *in, char *out, size_t out_max)
{
	static const char hex[] = "0123456789abcdef";
	size_t o = 0;

	if (out_max == 0) {
		return false;
	}
	for (size_t i = 0; in[i] != '\0'; i++) {
		unsigned char c = (unsigned char) in[i];
		size_t need =
			c >= 0x80 ?
				https_json_utf8_length(
					(const unsigned char *) in + i) :
				(c < 0x20 ? 6 :
					    (c == '"' || c == '\\' ? 2 : 1));

		if (need == 0 || need > out_max - o - 1) {
			return false;
		}
		if (c >= 0x80) {
			for (size_t j = 0; j < need; j++) {
				out[o++] = in[i++];
			}
			i--;
		} else if (c < 0x20) {
			out[o++] = '\\';
			out[o++] = 'u';
			out[o++] = '0';
			out[o++] = '0';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0x0f];
		} else if (c == '"' || c == '\\') {
			out[o++] = '\\';
			out[o++] = (char) c;
		} else {
			out[o++] = (char) c;
		}
	}
	out[o] = '\0';
	return true;
}

static const char *
https_json_skip_space(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	return p;
}

static bool
https_json_hex4(const char **cursor, uint32_t *value)
{
	const char *p = *cursor;
	uint32_t n = 0;

	for (int i = 0; i < 4; i++) {
		unsigned char c = (unsigned char) p[i];
		uint32_t digit;

		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10U;
		} else if (c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10U;
		} else {
			return false;
		}
		n = (n << 4) | digit;
	}
	*cursor = p + 4;
	*value = n;
	return true;
}

/* 0=OK, -1=non-string, -2=invalid/truncated, -3=output full. */
static int
https_json_decode_string(const char **cursor, char *out, size_t out_max)
{
	const char *p = *cursor;
	size_t o = 0;

	if (out != NULL && out_max == 0) {
		return -3;
	}
	if (*p++ != '"') {
		return -1;
	}
	while (*p != '\0') {
		uint32_t c = (unsigned char) *p++;
		unsigned char bytes[4];
		size_t count = 1;

		if (c == '"') {
			if (out != NULL) {
				out[o] = '\0';
			}
			*cursor = p;
			return 0;
		}
		if (c < 0x20) {
			return -2;
		}
		if (c == '\\') {
			c = (unsigned char) *p++;
			switch (c) {
			case '"':
			case '\\':
			case '/':
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			case 'u':
				if (!https_json_hex4(&p, &c)) {
					return -2;
				}
				if (c >= 0xd800 && c <= 0xdbff) {
					uint32_t low;

					if (p[0] != '\\' || p[1] != 'u') {
						return -2;
					}
					p += 2;
					if (!https_json_hex4(&p, &low) ||
					    low < 0xdc00 || low > 0xdfff) {
						return -2;
					}
					c = 0x10000 + ((c - 0xd800) << 10) +
					    (low - 0xdc00);
				} else if (c >= 0xdc00 && c <= 0xdfff) {
					return -2;
				}
				if (c == 0 && out != NULL) {
					return -2;
				}
				if (c <= 0x7f) {
					bytes[0] = (unsigned char) c;
				} else if (c <= 0x7ff) {
					bytes[0] = (unsigned char) (0xc0 |
								    (c >> 6));
					bytes[1] = (unsigned char) (0x80 |
								    (c & 0x3f));
					count = 2;
				} else if (c <= 0xffff) {
					bytes[0] = (unsigned char) (0xe0 |
								    (c >> 12));
					bytes[1] = (unsigned char) (0x80 |
								    ((c >> 6) &
								     0x3f));
					bytes[2] = (unsigned char) (0x80 |
								    (c & 0x3f));
					count = 3;
				} else {
					bytes[0] = (unsigned char) (0xf0 |
								    (c >> 18));
					bytes[1] = (unsigned char) (0x80 |
								    ((c >> 12) &
								     0x3f));
					bytes[2] = (unsigned char) (0x80 |
								    ((c >> 6) &
								     0x3f));
					bytes[3] = (unsigned char) (0x80 |
								    (c & 0x3f));
					count = 4;
				}
				break;
			default:
				return -2;
			}
			if (count == 1) {
				bytes[0] = (unsigned char) c;
			}
		} else if (c >= 0x80) {
			const unsigned char *start =
				(const unsigned char *) p - 1;

			count = https_json_utf8_length(start);
			if (count == 0) {
				return -2;
			}
			for (size_t i = 0; i < count; i++) {
				bytes[i] = start[i];
			}
			p += count - 1;
		} else {
			bytes[0] = (unsigned char) c;
		}
		if (out != NULL && count > out_max - o - 1) {
			return -3;
		}
		if (out != NULL) {
			for (size_t i = 0; i < count; i++) {
				out[o++] = (char) bytes[i];
			}
		}
	}
	return -2;
}

static bool
https_json_parse_value(const char **cursor, unsigned int depth)
{
	const char *p = https_json_skip_space(*cursor);

	if (depth > 24) {
		return false;
	}
	if (*p == '"') {
		if (https_json_decode_string(&p, NULL, 0) != 0) {
			return false;
		}
	} else if (*p == '{') {
		p = https_json_skip_space(p + 1);
		if (*p != '}') {
			for (;;) {
				if (https_json_decode_string(&p, NULL, 0) !=
				    0) {
					return false;
				}
				p = https_json_skip_space(p);
				if (*p++ != ':') {
					return false;
				}
				if (!https_json_parse_value(&p, depth + 1)) {
					return false;
				}
				p = https_json_skip_space(p);
				if (*p == '}') {
					break;
				}
				if (*p++ != ',') {
					return false;
				}
				p = https_json_skip_space(p);
			}
		}
		p++;
	} else if (*p == '[') {
		p = https_json_skip_space(p + 1);
		if (*p != ']') {
			for (;;) {
				if (!https_json_parse_value(&p, depth + 1)) {
					return false;
				}
				p = https_json_skip_space(p);
				if (*p == ']') {
					break;
				}
				if (*p++ != ',') {
					return false;
				}
			}
		}
		p++;
	} else if (strncmp(p, "true", 4) == 0 || strncmp(p, "null", 4) == 0) {
		p += 4;
	} else if (strncmp(p, "false", 5) == 0) {
		p += 5;
	} else {
		if (*p == '-') {
			p++;
		}
		if (*p == '0') {
			p++;
		} else if (*p >= '1' && *p <= '9') {
			do {
				p++;
			} while (*p >= '0' && *p <= '9');
		} else {
			return false;
		}
		if (*p == '.') {
			p++;
			if (*p < '0' || *p > '9') {
				return false;
			}
			do {
				p++;
			} while (*p >= '0' && *p <= '9');
		}
		if (*p == 'e' || *p == 'E') {
			p++;
			if (*p == '+' || *p == '-') {
				p++;
			}
			if (*p < '0' || *p > '9') {
				return false;
			}
			do {
				p++;
			} while (*p >= '0' && *p <= '9');
		}
	}
	*cursor = p;
	return true;
}

static bool
https_json_valid(const char *body)
{
	const char *p = https_json_skip_space(body);

	return *p == '{' && https_json_parse_value(&p, 0) &&
	       *https_json_skip_space(p) == '\0';
}

static const char *
https_json_member(const char *object, const char *name)
{
	const char *found = NULL;
	const char *p;

	if (object == NULL || *object != '{') {
		return NULL;
	}
	p = https_json_skip_space(object + 1);
	while (*p != '}') {
		char key[32];
		const char *decoded = p;
		int key_result =
			https_json_decode_string(&decoded, key, sizeof(key));

		if (https_json_decode_string(&p, NULL, 0) != 0) {
			return NULL;
		}
		p = https_json_skip_space(p);
		if (*p++ != ':') {
			return NULL;
		}
		const char *value = https_json_skip_space(p);

		p = value;
		if (!https_json_parse_value(&p, 0)) {
			return NULL;
		}
		if (key_result == 0 && strcmp(key, name) == 0) {
			if (found != NULL) {
				return NULL;
			}
			found = value;
		}
		p = https_json_skip_space(p);
		if (*p == '}') {
			break;
		}
		if (*p++ != ',') {
			return NULL;
		}
		p = https_json_skip_space(p);
	}
	return found;
}

static inline int
https_json_chat_content(const char *body, char *out, size_t out_max)
{
	if (!https_json_valid(body)) {
		return -2;
	}
	body = https_json_skip_space(body);
	const char *choices = https_json_member(body, "choices");

	if (choices == NULL || *choices != '[') {
		return -1;
	}
	const char *first = https_json_skip_space(choices + 1);
	const char *message = https_json_member(first, "message");
	const char *content = https_json_member(message, "content");

	if (content == NULL) {
		return -1;
	}
	return https_json_decode_string(&content, out, out_max);
}

static inline bool
https_json_extract_uint(const char *body, const char *key, uint32_t *out)
{
	body = https_json_skip_space(body);
	const char *usage = https_json_member(body, "usage");
	const char *p = https_json_member(usage, key);
	uint32_t n = 0;

	if (p == NULL) {
		return false;
	}
	p = https_json_skip_space(p);
	if (*p < '0' || *p > '9') {
		return false;
	}
	do {
		uint32_t digit = (uint32_t) (*p++ - '0');

		if (n > (UINT32_MAX - digit) / 10U) {
			return false;
		}
		n = n * 10U + digit;
	} while (*p >= '0' && *p <= '9');
	p = https_json_skip_space(p);
	if (*p != ',' && *p != '}' && *p != ']') {
		return false;
	}
	*out = n;
	return true;
}

#endif /* ENTELEA_HTTPS_JSON_H */
