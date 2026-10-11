/*
 * Fails each allocation in turn while luit loads a built-in charset table
 * (luitLookupMapping() in transcoder/src/luitconv.c), and checks that the
 * load fails as a whole: no crash, no table, nothing left allocated, and
 * the same table as with every allocation succeeding once they succeed
 * again. (iconv's tables, which luit only uses when -prefer asks for
 * them, aren't checked.) Then checks that a charset whose
 * table couldn't be loaded falls back to the unknown charset in the
 * charset layer (charset.c), for the rest of the session.
 *
 * Built and run by tests/test_encodings.py from transcoder/src's sources
 * compiled with NO_LEAKS (so luitconv_leaks() frees every table) and with
 * malloc, calloc, realloc, strdup and free renamed to the counting
 * versions here, which can fail a given call. Prints each check that
 * fails and exits 1 if any did; ALLOC_FAULT_TRACE set in the environment
 * prints each allocation failed first, to find one that crashes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <iso2022.h>
#include <sys.h>

int luit_main(int, char **);	/* luit.c's main, renamed (unused) */

static long calls;		/* allocations asked for so far */
static long live;		/* allocations not freed yet */
static long fail_at;		/* the call to fail, 0 for none */
static int failures;

/* fails the call fail_at, setting errno as the real ones do */
static int
failNow(void)
{
    if (++calls != fail_at)
	return 0;
    errno = ENOMEM;
    return 1;
}

void *
test_malloc(size_t n)
{
    void *p = failNow() ? NULL : malloc(n);
    live += p != NULL;
    return p;
}

void *
test_calloc(size_t n, size_t size)
{
    void *p = failNow() ? NULL : calloc(n, size);
    live += p != NULL;
    return p;
}

void *
test_realloc(void *old, size_t n)
{
    void *p = failNow() ? NULL : realloc(old, n);
    live += p != NULL && old == NULL;
    return p;
}

char *
test_strdup(const char *s)
{
    char *p = failNow() ? NULL : strdup(s);
    live += p != NULL;
    return p;
}

void
test_free(void *p)
{
    live -= p != NULL;
    free(p);
}

typedef struct {
    const char *name;
    US_SIZE size;
} Table;

static const Table tables[] = {
    {"iso8859-2", us8BIT},	/* builtin.c */
    {"jisx0212.1990-0", us16BIT},	/* builtin_fork.c */
};

#define CODES 65536
static unsigned ref_code[CODES];
static unsigned char ref_found[CODES];

static void
fail(const Table *t, long n, const char *what)
{
    failures++;
    printf("NG %s (allocation %ld of the load): %s\n", t->name, n, what);
}

/* Records the table m maps to (record) or checks it's the same */
static int
sameTable(FontMapPtr m, int record)
{
    unsigned code;

    for (code = 0; code < CODES; code++) {
	unsigned out = 0;
	unsigned char found = (unsigned char) luitMapCodeValueFound(code, m, &out);

	if (record) {
	    ref_found[code] = found;
	    ref_code[code] = out;
	} else if (found != ref_found[code] || (found && out != ref_code[code])) {
	    return 0;
	}
    }
    return 1;
}

static FontMapPtr
load(const Table *t)
{
    return luitLookupMapping(t->name, umBUILTIN, t->size);
}

/* Which of the load's allocations to fail: every one of a small load, the
 * first, middle and last ones of a large one */
static int
tried(long n, long count)
{
    return count <= 600 || n <= 200 || n > count - 200
	|| (n > count / 2 - 100 && n <= count / 2 + 100);
}

static void
failEachAllocation(const Table *t)
{
    long before = live, count, n, run = 0;
    FontMapPtr m;

    calls = 0;
    if ((m = load(t)) == NULL) {
	fail(t, 0, "doesn't load with every allocation succeeding");
	return;
    }
    count = calls;
    (void) sameTable(m, 1);
    luitconv_leaks();
    if (live != before) {
	fail(t, 0, "frees less than it allocated");
	return;
    }
    for (n = 1; n <= count; n++) {
	if (!tried(n, count))
	    continue;
	run++;
	if (getenv("ALLOC_FAULT_TRACE") != NULL)
	    printf("%s: failing allocation %ld\n", t->name, n);
	calls = 0;
	fail_at = n;
	m = load(t);
	fail_at = 0;
	if (m != NULL)
	    fail(t, n, "loaded with an allocation failed");
	luitconv_leaks();
	if (live != before) {
	    fail(t, n, "left allocations behind");
	    before = live;
	}
	if ((m = load(t)) == NULL || !sameTable(m, 0))
	    fail(t, n, "a later load doesn't make the same table");
	luitconv_leaks();
    }
    printf("%s: failed %ld of its %ld allocations in turn\n", t->name, run, count);
}

/*
 * In the charset layer: JIS X 0212 has two entries in charset.c's table (a
 * 94x94 and a 96x96 charset), each tried once. Its table fails to load
 * (the first allocation after the charset record's) for each, so the
 * charset falls back to the unknown one, and isn't loaded again with
 * allocations succeeding. Run in a process of its own ("charset"), whose
 * stderr tests/test_encodings.py checks for that table's message.
 */
static void
charsetFallsBack(void)
{
    const CharsetRec *c;
    int k;

    for (k = 0; k < 2; k++) {
	calls = 0;
	fail_at = 2;
	c = getCharsetByName("JIS X 0212");
	fail_at = 0;
	if (c == NULL || !strcmp(c->name, "JIS X 0212")) {
	    failures++;
	    printf("NG a charset whose table failed doesn't fall back to the unknown one\n");
	    return;
	}
    }
    if ((c = getCharsetByName("JIS X 0212")) == NULL || !strcmp(c->name, "JIS X 0212")) {
	failures++;
	printf("NG a charset entry whose table failed is tried again\n");
    } else {
	printf("JIS X 0212: falls back to %s after its table failed\n", c->name);
    }
}

int
main(int argc, char **argv)
{
    size_t k;

    setvbuf(stdout, NULL, _IONBF, 0);	/* all of it, should a check crash */
    ignore_locale = 1;
    if (argc > 1 && !strcmp(argv[1], "charset")) {
	charsetFallsBack();
    } else {
	for (k = 0; k < sizeof(tables) / sizeof(tables[0]); k++)
	    failEachAllocation(&tables[k]);
    }
    printf("%s\n", failures ? "allocation failures not handled" : "every allocation failure handled");
    return failures ? 1 : 0;
}
