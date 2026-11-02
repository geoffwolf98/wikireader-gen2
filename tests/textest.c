/* textest: print the linear form of TeX formulas, one per line on stdin.
 * ^(..) superscript, _(..) subscript, *..* italic */
#include <stdio.h>
#include <string.h>
#include "tex.h"
static int cur;
static void out(void *ctx, const char *s, uint32_t n, int st)
{
	(void)ctx;
	if ((st & 3) != (cur & 3)) {
		if (cur & 3) putchar(')');
		if (st & 1) fputs("^(", stdout);
		if (st & 2) fputs("_(", stdout);
	}
	cur = st;
	if (!(st & TEX_UPRIGHT)) putchar('*');
	fwrite(s, 1, n, stdout);
	if (!(st & TEX_UPRIGHT)) putchar('*');
}
int main(void)
{
	char line[4096];
	while (fgets(line, sizeof(line), stdin)) {
		line[strcspn(line, "\n")] = 0;
		cur = 0;
		tex_render(line, (uint32_t)strlen(line), out, NULL);
		if (cur & 3) putchar(')');
		putchar('\n');
	}
	return 0;
}
