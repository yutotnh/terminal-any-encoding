/*
 * Drives luit's input conversion (copyIn() in transcoder/src/iso2022.c)
 * through every way a rejected paste can be split into reads, and rejected
 * reads with other escapes in them, with the time faked, and checks what
 * the shell gets against the rules in docs/transcoder-design.md
 * ("Fallback design for conversion failures").
 * Built and run by tests/test_encodings.py against transcoder/src's
 * objects; prints each scenario that breaks a rule and exits 1 if any did.
 *
 * Input is ASCII but for the snowman (U+2603, which EUC-JP can't
 * represent), so what the shell gets is the same bytes: paste content is
 * the letters p-z, typed input "ok", and the only ESC, digits and '~' are
 * the paste markers', but in escapesInRejected(), which checks what the
 * shell gets as a whole.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <luit.h>
#include <iso2022.h>

int luit_main(int, char **);	/* luit.c's main, renamed (unused) */

#define START "\033[200~"
#define END "\033[201~"
#define SNOWMAN "\342\230\203"
#define BOUND 2000.0
#define PAUSE 50.0

typedef struct {
    double delay;		/* ms since the read before */
    char bytes[64];
} Read;

static Iso2022Ptr input_state;
static unsigned char shell[1 << 16];	/* what the shell got */
static size_t shell_len;
static int bells;
static int failures;
static int scenarios;

/* One input and output state for every scenario, as luit has: each
 * scenario starts from a forgotten paste state (resetPasteTracking()) and
 * the program turning bracketed paste on or off */
static void
setUp(int bracketed)
{
    static Iso2022Ptr output_state;
    static int devnull = -1;
    unsigned char out[] = "\033[?2004h";

    if (input_state == NULL) {
	devnull = open("/dev/null", O_WRONLY);
	input_state = allocIso2022();
	output_state = allocIso2022();
	if (initIso2022("euc-jp-2007", NULL, output_state) < 0
	    || mergeIso2022(input_state, output_state) < 0) {
	    fprintf(stderr, "couldn't set up EUC-JP\n");
	    exit(2);
	}
    }
    resetPasteTracking();
    if (!bracketed)
	out[7] = 'l';
    copyOut(output_state, devnull, out, (unsigned) strlen((char *) out));
    shell_len = 0;
    bells = 0;
}

/* Feeds the reads as luit's parent() does: held input (an ESC that may
 * start a paste marker) is passed on once it has been held for HOLD_MILLIS
 * with nothing more come */
static void
feed(Read *reads, int n)
{
    double now = 10000.0;
    int k;

    for (k = 0; k < n; k++) {
	size_t len = strlen(reads[k].bytes);
	if (inputHeld() && reads[k].delay >= HOLD_MILLIS) {
	    (void) flushHeldInput(input_state, now + HOLD_MILLIS);
	    shell_len += takeInput(shell + shell_len);
	}
	now += reads[k].delay;
	if (len == 0)
	    continue;
	if (copyIn(input_state, (unsigned char *) reads[k].bytes, (int) len, now))
	    bells++;
	shell_len += takeInput(shell + shell_len);
    }
    if (flushHeldInput(input_state, now + HOLD_MILLIS))
	shell_len += takeInput(shell + shell_len);
}

static void
fail(const char *rule, const char *what, const Read *reads, int n)
{
    int k;
    size_t i;

    failures++;
    if (getenv("PASTE_DRIVER_QUIET") != NULL)
	return;
    printf("NG %s (%s):", rule, what);
    for (k = 0; k < n; k++) {
	printf(" +%.0f\"", reads[k].delay);
	for (i = 0; reads[k].bytes[i]; i++) {
	    unsigned char c = (unsigned char) reads[k].bytes[i];
	    if (c == 033)
		printf("^[");
	    else if (c == '\n')
		printf("\\n");
	    else if (c >= 0x80)
		printf("\\%o", c);
	    else
		putchar(c);
	}
	putchar('"');
    }
    printf(" -> \"");
    for (i = 0; i < shell_len; i++) {
	if (shell[i] == 033)
	    putchar('^');
	else if (shell[i] >= 0x80)
	    printf("\\%o", shell[i]);
	else
	    putchar(shell[i]);
    }
    printf("\"\n");
}

/* The rules every scenario must keep: markers whole and in order (unless
 * markers_kept is 0), the shell out of any paste at the end, and the last
 * "ok" there */
static int
checkShell(const char *what, const Read *reads, int n, int markers_kept)
{
    size_t i;
    int inside = 0;

    for (i = 0; i < shell_len; i++) {
	unsigned char c = shell[i];
	if (!markers_kept) {
	    if (c == 033 && i + 6 <= shell_len && !memcmp(shell + i, START, 6))
		inside = 1;
	    else if (c == 033 && i + 6 <= shell_len && !memcmp(shell + i, END, 6))
		inside = 0;
	    else if (c >= 0x80) {
		fail("nothing unencodable", what, reads, n);
		return 0;
	    }
	    continue;
	}
	if (c == 033) {
	    if (i + 6 <= shell_len && !memcmp(shell + i, START, 6)) {
		if (inside) {
		    fail("markers in order", what, reads, n);
		    return 0;
		}
		inside = 1;
	    } else if (i + 6 <= shell_len && !memcmp(shell + i, END, 6)) {
		if (!inside) {
		    fail("markers in order", what, reads, n);
		    return 0;
		}
		inside = 0;
	    } else {
		fail("markers whole", what, reads, n);
		return 0;
	    }
	    i += 5;
	} else if ((c >= '0' && c <= '9') || c == '~' || c == '[') {
	    fail("markers whole", what, reads, n);
	    return 0;
	} else if (c >= 0x80) {
	    fail("nothing unencodable", what, reads, n);
	    return 0;
	}
    }
    if (inside) {
	fail("shell out of the paste at the end", what, reads, n);
	return 0;
    }
    if (shell_len < 2 || memcmp(shell + shell_len - 2, "ok", 2)) {
	fail("input after the drop goes through", what, reads, n);
	return 0;
    }
    return 1;
}

/* Splits text at the given offsets (ascending, -1 for none) into reads,
 * the one starting at each cut delayed by that cut's gap; the first read
 * comes at 0 */
static int
split(const char *text, const int *cuts, const double *gaps, int cut_count, Read *reads)
{
    int n = 0, k;
    size_t from = 0, len = strlen(text);
    double delay = 0.0;

    for (k = 0; k <= cut_count; k++) {
	size_t to;

	if (k < cut_count && cuts[k] < 0)
	    continue;
	to = (k < cut_count) ? (size_t) cuts[k] : len;
	reads[n].delay = delay;
	memcpy(reads[n].bytes, text + from, to - from);
	reads[n].bytes[to - from] = '\0';
	n++;
	from = to;
	if (k < cut_count)
	    delay = gaps[k];
    }
    return n;
}

static const double GAPS[] = { 0.0, 20.0, 100.0, 2100.0 };
#define GAP_COUNT ((int) (sizeof(GAPS) / sizeof(GAPS[0])))

/*
 * One rejected paste, "START pq SNOWMAN rs END", cut at up to three
 * places: in the start marker (or before the snowman, so that the start
 * and "pq" are forwarded first), in the end marker, with each part
 * coming after a gap; then "ok", typed after the bound.
 */
static void
rejectedPastes(void)
{
    const char *paste = START "pq" SNOWMAN "rs" END;
    size_t start_at = 0;
    size_t snow_at = strlen(START "pq");
    size_t end_at = strlen(START "pq" SNOWMAN "rs");
    int bracketed, c1, c2, g1, g2;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	/* c1: 0 none, 1-5 in the start marker, 6 just before the snowman;
	 * c2: 0 none, 1-5 in the end marker, 6 just before it */
	for (c1 = 0; c1 <= 6; c1++) {
	    for (c2 = 0; c2 <= 6; c2++) {
		for (g1 = 0; g1 < GAP_COUNT; g1++) {
		    for (g2 = 0; g2 < GAP_COUNT; g2++) {
			Read reads[8];
			int cuts[2];
			double gaps[2];
			int n;
			char what[96];
			size_t i;
			int dropped_content = 0;

			cuts[0] = c1 == 0 ? -1 : (int) (c1 == 6 ? snow_at : start_at + (size_t) c1);
			cuts[1] = c2 == 0 ? -1 : (int) (c2 == 6 ? end_at : end_at + (size_t) c2);
			gaps[0] = GAPS[g1];
			gaps[1] = GAPS[g2];
			n = split(paste, cuts, gaps, 2, reads);
			reads[n].delay = BOUND + 100.0;
			strcpy(reads[n].bytes, "ok");
			n++;

			snprintf(what, sizeof(what), "bracketed %s, cuts %d/%d, gaps %.0f/%.0f",
				 bracketed ? "on" : "off", c1, c2, GAPS[g1], GAPS[g2]);
			scenarios++;
			setUp(bracketed);
			feed(reads, n);
			/* Where luit can't keep the markers whole, by design:
			 * a lone ESC that nothing follows for HOLD_MILLIS
			 * outside a paste is an Escape key (the start marker
			 * cut after its ESC), and what comes after the bound
			 * goes through as it is */
			if (!checkShell(what, reads, n,
					!(c1 == 1 && GAPS[g1] >= HOLD_MILLIS)
					&& GAPS[g1] < BOUND && GAPS[g2] < BOUND))
			    continue;
			if (bells != 1) {
			    fail("one bell", what, reads, n);
			    continue;
			}
			/* What arrives inside the drop isn't passed on: with
			 * bracketed paste, everything up to the end marker
			 * within the bound; otherwise what comes under the
			 * pause after the snowman */
			for (i = 0; i < shell_len; i++) {
			    if (shell[i] == 'r' || shell[i] == 's')
				dropped_content = 1;
			}
			if (dropped_content) {
			    int late = 0;
			    double t = 0.0;
			    int k;
			    /* the time from the snowman's read to the read
			     * with "rs" */
			    for (k = 0; k < n; k++) {
				if (strstr(reads[k].bytes, SNOWMAN))
				    t = 0.0;
				else
				    t += reads[k].delay;
				if (strstr(reads[k].bytes, "r")) {
				    late = bracketed ? t >= BOUND : reads[k].delay >= PAUSE;
				    break;
				}
			    }
			    if (!late)
				fail("the rest of the paste is dropped", what, reads, n);
			}
		    }
		}
	    }
	}
    }
}

/*
 * The end marker cut twice: its first part in the rejected read, the rest
 * in two more reads after gaps, so that each part can be dropped,
 * forwarded or held.
 */
static void
endCutTwice(void)
{
    const char *paste = START "pq" SNOWMAN "rs" END;
    size_t end_at = strlen(START "pq" SNOWMAN "rs");
    int bracketed, a, b, g1, g2;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	for (a = 1; a <= 4; a++) {
	    for (b = a + 1; b <= 5; b++) {
		for (g1 = 0; g1 < GAP_COUNT; g1++) {
		    for (g2 = 0; g2 < GAP_COUNT; g2++) {
			Read reads[8];
			int cuts[2];
			double gaps[2];
			int n;
			char what[96];

			cuts[0] = (int) (end_at + (size_t) a);
			cuts[1] = (int) (end_at + (size_t) b);
			gaps[0] = GAPS[g1];
			gaps[1] = GAPS[g2];
			n = split(paste, cuts, gaps, 2, reads);
			reads[n].delay = BOUND + 100.0;
			strcpy(reads[n].bytes, "ok");
			n++;
			snprintf(what, sizeof(what), "bracketed %s, end cut at %d and %d, gaps %.0f/%.0f",
				 bracketed ? "on" : "off", a, b, GAPS[g1], GAPS[g2]);
			scenarios++;
			setUp(bracketed);
			feed(reads, n);
			(void) checkShell(what, reads, n,
					  GAPS[g1] + GAPS[g2] < BOUND);
		    }
		}
	    }
	}
    }
}

/*
 * The start marker's first part forwarded with input before it, the rest
 * in the rejected read: the shell gets the whole marker or none of it.
 */
static void
startForwardedPart(void)
{
    int bracketed, a, g;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	for (a = 2; a <= 5; a++) {
	    for (g = 0; g < GAP_COUNT; g++) {
		Read reads[4];
		char what[96];
		int n = 0;

		reads[n].delay = 0.0;
		snprintf(reads[n].bytes, sizeof(reads[n].bytes), "xy%.*s", a, START);
		n++;
		reads[n].delay = GAPS[g];
		snprintf(reads[n].bytes, sizeof(reads[n].bytes), "%s%s", START + a, "pq" SNOWMAN "rs" END);
		n++;
		reads[n].delay = BOUND + 100.0;
		strcpy(reads[n++].bytes, "ok");
		snprintf(what, sizeof(what), "bracketed %s, start cut at %d after input, gap %.0f",
			 bracketed ? "on" : "off", a, GAPS[g]);
		scenarios++;
		setUp(bracketed);
		feed(reads, n);
		(void) checkShell(what, reads, n, GAPS[g] < BOUND);
	    }
	}
    }
}

/*
 * A paste dropped up to the bound whose end marker then comes late, then
 * a paste that goes through, cut in its end marker: the second paste
 * arrives whole and closed.
 */
static void
pasteAfterBound(void)
{
    int bracketed, c, g;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	for (c = 0; c <= 5; c++) {
	    for (g = 0; g < GAP_COUNT; g++) {
		Read reads[8];
		int n = 0;
		char what[96];
		const char *second = START "uv" END;
		size_t cut = strlen(START "uv") + (size_t) c;

		reads[n].delay = 0.0;
		strcpy(reads[n++].bytes, START "pq" SNOWMAN);
		reads[n].delay = BOUND + 100.0;
		strcpy(reads[n++].bytes, "rs" END);
		reads[n].delay = 300.0;
		memcpy(reads[n].bytes, second, c ? cut : strlen(second));
		reads[n++].bytes[c ? cut : strlen(second)] = '\0';
		if (c) {
		    reads[n].delay = GAPS[g];
		    strcpy(reads[n++].bytes, second + cut);
		}
		reads[n].delay = BOUND + 100.0;
		strcpy(reads[n++].bytes, "ok");

		snprintf(what, sizeof(what), "bracketed %s, second paste cut %d, gap %.0f",
			 bracketed ? "on" : "off", c, GAPS[g]);
		scenarios++;
		setUp(bracketed);
		feed(reads, n);
		/* the first paste's rest comes after the bound: markers are
		 * checked from the second paste on (see below) */
		if (!checkShell(what, reads, n, 0))
		    continue;
		if (!strstr((char *) shell, START "uv" END) && shell_len < sizeof(shell)) {
		    shell[shell_len] = '\0';
		    if (!strstr((char *) shell, START "uv" END))
			fail("a paste after the bound arrives whole", what, reads, n);
		}
	    }
	}
    }
}

/*
 * A rejected read with escapes in it that aren't paste markers (keys, or
 * what only starts like a marker) before and after the snowman, alone or
 * in a paste: none of it reaches the shell, not even what's held at its
 * end and passed on later. Only the markers do.
 */
static void
escapesInRejected(void)
{
    /* not an ESC right before the snowman: upstream's parser takes the
     * byte after an ESC as a character of its own (U+00E2 for the
     * snowman's first), so nothing is rejected there */
    static const char *const parts[] = {
	"", "pq", "pq\n", "\033[D", "\033b", "\033\033[D",
	"\033[20x", "\033[2000", "\033[200", "\033[201", "pq\033"
    };
    const int count = (int) (sizeof(parts) / sizeof(parts[0]));
    int bracketed, wrapped, a, b;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	for (wrapped = 0; wrapped <= 1; wrapped++) {
	    for (a = 0; a < count - 1; a++) {
		for (b = 0; b < count; b++) {
		    Read reads[2];
		    char what[96];
		    const char *expect = wrapped ? START END "ok" : "ok";

		    reads[0].delay = 0.0;
		    snprintf(reads[0].bytes, sizeof(reads[0].bytes), "%s%s" SNOWMAN "%s%s",
			     wrapped ? START : "", parts[a], parts[b], wrapped ? END : "");
		    reads[1].delay = BOUND + 100.0;
		    strcpy(reads[1].bytes, "ok");
		    snprintf(what, sizeof(what), "bracketed %s, %s, escapes %d/%d in the rejected read",
			     bracketed ? "on" : "off", wrapped ? "pasted" : "typed", a, b);
		    scenarios++;
		    setUp(bracketed);
		    feed(reads, 2);
		    if (shell_len != strlen(expect) || memcmp(shell, expect, shell_len))
			fail("nothing of a rejected read but its markers", what, reads, 2);
		    else if (bells != 1)
			fail("one bell", what, reads, 2);
		}
	    }
	}
    }
}

/*
 * What starts like a marker at the end of a read that went through, and
 * goes on in a rejected read as something else: the first read's bytes go
 * through as they came, and nothing of the second.
 */
static void
heldIntoRejected(void)
{
    static const char *const heads[] = {
	"\033", "\033[", "\033[2", "\033[20", "\033[200", "\033[201"
    };
    static const char *const tails[] = { "x", "[D", "0x", "D" };
    static const double gaps[] = { 0.0, 5.0 };
    int bracketed, wrapped, h, t, g;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	for (wrapped = 0; wrapped <= 1; wrapped++) {
	    for (h = 0; h < (int) (sizeof(heads) / sizeof(heads[0])); h++) {
		for (t = 0; t < (int) (sizeof(tails) / sizeof(tails[0])); t++) {
		    for (g = 0; g < (int) (sizeof(gaps) / sizeof(gaps[0])); g++) {
			Read reads[3];
			char what[96], expect[64];

			reads[0].delay = 0.0;
			snprintf(reads[0].bytes, sizeof(reads[0].bytes), "%spq%s",
				 wrapped ? START : "", heads[h]);
			reads[1].delay = gaps[g];
			snprintf(reads[1].bytes, sizeof(reads[1].bytes), "%s" SNOWMAN "rs%s",
				 tails[t], wrapped ? END : "");
			reads[2].delay = BOUND + 100.0;
			strcpy(reads[2].bytes, "ok");
			snprintf(expect, sizeof(expect), "%s%sok", reads[0].bytes, wrapped ? END : "");
			snprintf(what, sizeof(what), "bracketed %s, %s, held %d into rejected %d, gap %.0f",
				 bracketed ? "on" : "off", wrapped ? "pasted" : "typed", h, t, gaps[g]);
			scenarios++;
			setUp(bracketed);
			feed(reads, 3);
			if (shell_len != strlen(expect) || memcmp(shell, expect, shell_len))
			    fail("nothing of a rejected read but its markers", what, reads, 3);
		    }
		}
	    }
	}
    }
}

/*
 * An escape left open before a paste marker (an Escape key, or one ending
 * the paste's content) is ended by the marker, as the marker's bytes would
 * end it: what comes right after the marker is converted as itself, the
 * snowman after a start marker rejected, a typed character after an end
 * marker passed on.
 */
static void
escapeBeforeMarker(void)
{
    static const double gaps[] = { 0.0, 20.0, 100.0 };
    int bracketed, cut, g;

    for (bracketed = 0; bracketed <= 1; bracketed++) {
	/* cut: 0 one read, 1 after the first ESC, 2 before the end marker */
	for (cut = 0; cut <= 2; cut++) {
	    for (g = 0; g < (int) (sizeof(gaps) / sizeof(gaps[0])); g++) {
		Read reads[4];
		char what[96];
		int n;
		const char *expect;
		const int cuts[] = { cut == 1 ? 1 : -1 };
		const int end_cuts[] = { cut == 2 ? (int) strlen(START "pq\033") : -1 };

		snprintf(what, sizeof(what), "bracketed %s, %s, cut %d, gap %.0f",
			 bracketed ? "on" : "off", "Escape before a paste", cut, gaps[g]);
		n = split("\033" START SNOWMAN "rs" END, cuts, &gaps[g], 1, reads);
		reads[n].delay = BOUND + 100.0;
		strcpy(reads[n++].bytes, "ok");
		expect = "\033" START END "ok";
		scenarios++;
		setUp(bracketed);
		feed(reads, n);
		if (shell_len != strlen(expect) || memcmp(shell, expect, shell_len))
		    fail("what follows a marker converted as itself", what, reads, n);
		else if (bells != 1)
		    fail("one bell", what, reads, n);

		snprintf(what, sizeof(what), "bracketed %s, %s, cut %d, gap %.0f",
			 bracketed ? "on" : "off", "ESC ending a paste", cut, gaps[g]);
		n = split(START "pq\033" END, end_cuts, &gaps[g], 1, reads);
		reads[n].delay = 300.0;
		strcpy(reads[n++].bytes, "\343\201\202ok");	/* U+3042 */
		expect = START "pq\033" END "\244\242ok";	/* in EUC-JP */
		scenarios++;
		setUp(bracketed);
		feed(reads, n);
		if (shell_len != strlen(expect) || memcmp(shell, expect, shell_len))
		    fail("what follows a marker converted as itself", what, reads, n);
		else if (bells != 0)
		    fail("no bell", what, reads, n);
	    }
	}
    }
}

/* A key held down after a rejection, 10 ms apart: dropped for the bound,
 * then let through */
static void
heldKey(void)
{
    Read reads[400];
    int n = 0, k;
    size_t a = 0, i;

    reads[n].delay = 0.0;
    strcpy(reads[n++].bytes, SNOWMAN);
    for (k = 0; k < 300; k++) {
	reads[n].delay = 10.0;
	strcpy(reads[n++].bytes, "a");
    }
    reads[n].delay = BOUND + 100.0;
    strcpy(reads[n++].bytes, "ok");
    scenarios++;
    setUp(0);
    feed(reads, n);
    for (i = 0; i < shell_len; i++)
	a += shell[i] == 'a';
    /* 300 a's over 3 s: those after the 2 s bound, about 100 */
    if (a < 90 || a > 110)
	fail("a held key is dropped for the bound", "held key", reads, 3);
}

int
main(void)
{
    ignore_locale = 1;		/* as luit's -encoding does */
    rejectedPastes();
    endCutTwice();
    startForwardedPart();
    pasteAfterBound();
    escapesInRejected();
    heldIntoRejected();
    escapeBeforeMarker();
    heldKey();
    printf("%d of %d scenarios kept every rule\n", scenarios - failures, scenarios);
    return failures ? 1 : 0;
}
