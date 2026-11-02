/*
 * idxtest NAME.IDX                 self check: seek == brute force
 * idxtest NAME.IDX QUERY [N]       print the first N results
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/index.h"
#include "../src/fold.h"
#include "../host/hostio.h"

int main(int argc, char **argv)
{
	host_files_t f;
	idx_t x;
	idx_cursor_t c;
	idx_record_t r;
	char q[256];
	int e;

	if (argc < 2 || host_files_open(&f, argv[1]) < 0 || (e = idx_open(&x, &f.io)) < 0) {
		fprintf(stderr, "cannot open index\n");
		return 1;
	}
	if (argc >= 3) {
		int n = argc > 3 ? atoi(argv[3]) : 10;
		fold_utf8(argv[2], strlen(argv[2]), q, sizeof(q));
		idx_seek(&x, q, &c);
		while (n-- > 0 && idx_next(&x, &c, &r) == 1)
			printf("%7u %s%s -> %u\n", r.number, r.title,
			       r.flags & IDX_FLAG_REDIRECT ? " (redirect)" : "", r.target);
		return 0;
	}

	/* load every record in order, check sortedness and seek results */
	{
		char **key = malloc(sizeof(char *) * x.records);
		uint32_t i, n = 0, bad = 0, t;
		idx_cursor_t start;
		idx_seek_number(&x, 0, &start);
		c = start;
		while (idx_next(&x, &c, &r) == 1) {
			char k[256];
			fold_utf8(r.title, strlen(r.title), k, sizeof(k));
			if (r.number != n) {
				printf("numbering mismatch at %u\n", n);
				bad++;
			}
			if (n && strcmp(key[n - 1], k) > 0) {
				printf("unsorted at %u: %s > %s\n", n, key[n - 1], k);
				bad++;
			}
			key[n++] = strdup(k);
		}
		if (n != x.records) {
			printf("record count %u != header %u\n", n, x.records);
			bad++;
		}
		srand(42);
		for (t = 0; t < 20000; ++t) {
			uint32_t lo = 0, hi = n, want;
			if (t % 3 == 0) {
				int L = rand() % 4 + 1, j;
				for (j = 0; j < L; ++j)
					q[j] = (char)("abcdeghilmnoprstuy 0123"[rand() % 23]);
				q[L] = 0;
			} else {
				const char *s = key[rand() % n];
				size_t L = strlen(s), cut = (size_t)rand() % (L + 1);
				while (cut < L && ((unsigned char)s[cut] & 0xc0) == 0x80)
					cut++;
				memcpy(q, s, cut);
				q[cut] = 0;
				if (t % 7 == 0 && cut)
					q[cut - 1]++;	/* just after a real key */
			}
			while (lo < hi) {
				uint32_t mid = (lo + hi) / 2;
				if (strcmp(key[mid], q) < 0)
					lo = mid + 1;
				else
					hi = mid;
			}
			want = lo;
			idx_seek(&x, q, &c);
			if (c.number != want) {
				printf("seek mismatch for [%s]: got %u want %u\n", q, c.number, want);
				if (++bad > 10)
					break;
			}
			/* stepping back and forward lands in the same place */
			if (c.number > 0 && c.number < n) {
				idx_cursor_t d = c;
				idx_prev(&x, &d);
				idx_next(&x, &d, &r);
				if (d.number != c.number || d.page != c.page || d.off != c.off) {
					printf("prev/next mismatch at %u\n", c.number);
					bad++;
				}
			}
			idx_seek_number(&x, want < n ? want : 0, &c);
			if (want < n && c.number != want) {
				printf("seek_number mismatch %u\n", want);
				bad++;
			}
		}
		printf("%s: %u records, %u pages, 20000 random seeks, %u problems\n",
		       argv[1], n, x.pages, bad);
		return bad != 0;
	}
}
