/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Plain C corpus replay and deterministic mutation driver for tests/fuzz/.
 * Used when building without clang's libFuzzer (make test, make sanitize,
 * and fallback make fuzz). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct seed {
	uint8_t *data;
	size_t size;
};

struct corpus {
	struct seed *items;
	size_t count;
	size_t cap;
};

static int corpus_push(struct corpus *c, uint8_t *data, size_t size)
{
	if (c->count == c->cap) {
		size_t ncap = c->cap ? c->cap * 2 : 32;
		struct seed *n = realloc(c->items, ncap * sizeof(*n));
		if (!n)
			return -1;
		c->items = n;
		c->cap = ncap;
	}
	c->items[c->count].data = data;
	c->items[c->count].size = size;
	c->count++;
	return 0;
}

static void corpus_free(struct corpus *c)
{
	for (size_t i = 0; i < c->count; i++)
		free(c->items[i].data);
	free(c->items);
}

static int load_file(const char *path, struct corpus *c)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "fuzz_main: %s: %s\n", path, strerror(errno));
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return 0;
	}
	size_t sz = (size_t)st.st_size;
	uint8_t *buf = malloc(sz ? sz : 1);
	if (!buf) {
		close(fd);
		return -1;
	}
	size_t off = 0;
	while (off < sz) {
		ssize_t n = read(fd, buf + off, sz - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			free(buf);
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	close(fd);
	if (corpus_push(c, buf, off) < 0) {
		free(buf);
		return -1;
	}
	return 0;
}

static int load_path(const char *path, struct corpus *c)
{
	struct stat st;
	if (stat(path, &st) < 0) {
		fprintf(stderr, "fuzz_main: %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (!S_ISDIR(st.st_mode))
		return load_file(path, c);

	DIR *d = opendir(path);
	if (!d) {
		fprintf(stderr, "fuzz_main: %s: %s\n", path, strerror(errno));
		return -1;
	}
	struct dirent *de;
	char full[4096];
	int rc = 0;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		int n = snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(full))
			continue;
		if (load_file(full, c) < 0) {
			rc = -1;
			break;
		}
	}
	closedir(d);
	return rc;
}

static uint64_t rng_next(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	*s = x;
	return x;
}

static const char *const interesting_tokens[] = {
	"$(", "$((", "${", "}", "))", ")", "'", "\"", "$'", "\\\n",
	"<<'E'\n", "<<-E\n", ";;", "&&", "||", ">|", "<&-", "2>&1",
	"[[:alpha:]]", "[!a-z]", "*a*a*", ":-", ":=", ":?", ":+",
	"##", "%%", "${#", "if ", "then ", "fi", "case ", "esac",
	"for ", "in ", "do ", "done", "while ", "until ", "{ ", " }",
	"9223372036854775807", "-9223372036854775808", "/0", "<<63",
};

static size_t mutate_buf(uint8_t *buf, size_t len, size_t cap, uint64_t *rng)
{
	unsigned op = (unsigned)(rng_next(rng) % 6);
	if (op == 0 && len > 0) {
		/* Bit flip */
		size_t pos = (size_t)(rng_next(rng) % len);
		buf[pos] ^= (uint8_t)(1u << (rng_next(rng) % 8));
	} else if (op == 1 && len > 0) {
		/* Byte replace */
		size_t pos = (size_t)(rng_next(rng) % len);
		buf[pos] = (uint8_t)(rng_next(rng) & 0xff);
	} else if (op == 2 && len > 1) {
		/* Delete slice */
		size_t pos = (size_t)(rng_next(rng) % len);
		size_t del = 1 + (size_t)(rng_next(rng) % (len - pos));
		if (del > 16)
			del = 16;
		memmove(buf + pos, buf + pos + del, len - pos - del);
		len -= del;
	} else if (op == 3 && len + 1 < cap) {
		/* Insert byte */
		size_t pos = (size_t)(rng_next(rng) % (len + 1));
		memmove(buf + pos + 1, buf + pos, len - pos);
		buf[pos] = (uint8_t)(rng_next(rng) & 0xff);
		len++;
	} else if (op == 4) {
		/* Insert shell token */
		const char *tok = interesting_tokens[
			rng_next(rng) % (sizeof(interesting_tokens) / sizeof(interesting_tokens[0]))];
		size_t tlen = strlen(tok);
		if (len + tlen < cap) {
			size_t pos = (size_t)(rng_next(rng) % (len + 1));
			memmove(buf + pos + tlen, buf + pos, len - pos);
			memcpy(buf + pos, tok, tlen);
			len += tlen;
		}
	} else if (len > 2 && len * 2 < cap) {
		/* Duplicate a small chunk */
		size_t pos = (size_t)(rng_next(rng) % len);
		size_t clen = 1 + (size_t)(rng_next(rng) % (len - pos));
		if (clen > 32)
			clen = 32;
		if (len + clen < cap) {
			size_t dst = (size_t)(rng_next(rng) % (len + 1));
			uint8_t tmp[32];
			memcpy(tmp, buf + pos, clen);
			memmove(buf + dst + clen, buf + dst, len - dst);
			memcpy(buf + dst, tmp, clen);
			len += clen;
		}
	}
	return len;
}

int main(int argc, char **argv)
{
	struct corpus c = {0};
	unsigned long mutations = 0;
	uint64_t seed = 0x54414d434c414dULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--mutate") == 0 && i + 1 < argc) {
			mutations = strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
			seed = strtoull(argv[++i], NULL, 0);
			if (seed == 0)
				seed = 1;
		} else {
			if (load_path(argv[i], &c) < 0) {
				corpus_free(&c);
				return 1;
			}
		}
	}

	int saved_err = dup(STDERR_FILENO);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		(void)dup2(devnull, STDERR_FILENO);
		close(devnull);
	}

	for (size_t i = 0; i < c.count; i++)
		LLVMFuzzerTestOneInput(c.items[i].data, c.items[i].size);

	if (mutations > 0 && c.count > 0) {
		enum { MUT_CAP = 2048 };
		uint8_t *buf = malloc(MUT_CAP);
		if (buf) {
			for (unsigned long m = 0; m < mutations; m++) {
				const struct seed *s = &c.items[rng_next(&seed) % c.count];
				size_t len = s->size < MUT_CAP ? s->size : MUT_CAP - 1;
				memcpy(buf, s->data, len);
				unsigned steps = 1 + (unsigned)(rng_next(&seed) % 4);
				for (unsigned k = 0; k < steps; k++)
					len = mutate_buf(buf, len, MUT_CAP, &seed);
				LLVMFuzzerTestOneInput(buf, len);
			}
			free(buf);
		}
	}

	if (saved_err >= 0) {
		(void)dup2(saved_err, STDERR_FILENO);
		close(saved_err);
	}

	printf("%s: replayed %zu corpus files", argv[0], c.count);
	if (mutations > 0)
		printf(" + %lu mutations", mutations);
	printf(", 0 crashes\n");

	corpus_free(&c);
	return 0;
}
