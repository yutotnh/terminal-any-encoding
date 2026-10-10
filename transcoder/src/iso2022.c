/* $XTermId: iso2022.c,v 1.44 2025/09/12 08:20:14 tom Exp $ */

/*
Copyright 2011-2023,2025 by Thomas E. Dickey
Copyright (c) 2001 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <iso2022.h>

#include <unistd.h>
#include <errno.h>

#include <sys.h>

#define BUFFERED_INPUT_SIZE 4
static unsigned char buffered_input[BUFFERED_INPUT_SIZE];
static int buffered_input_count = 0;

static void terminateEsc(Iso2022Ptr, int, unsigned char *, unsigned);
static void terminate(Iso2022Ptr, int);

#define OUTBUF_FREE(is, count) ((is)->outbuf_count + (count) <= BUFFER_SIZE)
#define OUTBUF_MAKE_FREE(is, fd, count) \
    if(!OUTBUF_FREE((is), (count))) outbuf_flush((is), (fd))

#ifdef OPT_TRACE
static void
trace_charset(const char *tag, const CharsetRec * ptr)
{
    if (ptr != NULL) {
	TRACE(("%s:", NonNull(tag)));
	TRACE((" name:%s", NonNull(ptr->name)));
	TRACE((" type:%d", ptr->type));
	if (ptr->final)
	    TRACE((" final:%c", ptr->final));
	if (ptr->data != NULL)
	    TRACE((" data"));
	if (ptr->recode != NULL)
	    TRACE((" recode"));
	if (ptr->reverse != NULL)
	    TRACE((" reverse"));
	if (ptr->other_stack != NULL)
	    TRACE((" other_stack"));
	if (ptr->other_aux != NULL)
	    TRACE((" other_aux"));
	if (ptr->other_recode != NULL)
	    TRACE((" other_recode"));
	if (ptr->other_reverse != NULL)
	    TRACE((" other_reverse"));
	TRACE(("\n"));
    }
}

static void
trace_iso2022(const char *tag, const Iso2022Ptr ptr)
{
    TRACE(("%s:\n", NonNull(tag)));
    trace_charset("\tGL()", GL(ptr));
    trace_charset("\tGR()", GR(ptr));
    trace_charset("\tG0()", G0(ptr));
    trace_charset("\tG1()", G1(ptr));
    trace_charset("\tG2()", G2(ptr));
    trace_charset("\tG3()", G3(ptr));
    trace_charset("\tOTHER()", OTHER(ptr));
}

#else
#define trace_iso2022(tag, ptr)	/* nothing */
#endif

static void
outbuf_flush(Iso2022Ptr is, int fd)
{
    int rc;
    unsigned i = 0;

    if (olog >= 0)
	IGNORE_RC(write(olog, is->outbuf, is->outbuf_count));

    while (i < is->outbuf_count) {
	rc = (int) write(fd, is->outbuf + i, is->outbuf_count - i);
	if (rc > 0) {
	    i += (unsigned) rc;
	} else {
	    if (rc < 0 && errno == EINTR)
		continue;
	    else if ((rc == 0) || ((rc < 0) && (errno == EAGAIN))) {
		if (waitForOutput(fd) == IO_Closed)
		    break;
		continue;
	    } else
		break;
	}
    }
    is->outbuf_count = 0;
}

static void
outbufOne(Iso2022Ptr is, int fd, unsigned c)
{
    OUTBUF_MAKE_FREE(is, fd, 1);
    is->outbuf[is->outbuf_count++] = UChar(c);
}

/* Discards null codepoints */
static void
outbufUTF8(Iso2022Ptr is, int fd, unsigned c)
{
    if (c == 0)
	return;

    if (c <= 0x7F) {
	OUTBUF_MAKE_FREE(is, fd, 1);
	is->outbuf[is->outbuf_count++] = UChar(c);
    } else if (c <= 0x7FF) {
	OUTBUF_MAKE_FREE(is, fd, 2);
	is->outbuf[is->outbuf_count++] = UChar(0xC0 | ((c >> 6) & 0x1F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | (c & 0x3F));
    } else if (c <= 0xFFFF) {
	OUTBUF_MAKE_FREE(is, fd, 3);
	is->outbuf[is->outbuf_count++] = UChar(0xE0 | ((c >> 12) & 0x0F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 6) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | (c & 0x3F));
    } else if (c <= 0x1FFFFF) {
	OUTBUF_MAKE_FREE(is, fd, 4);
	is->outbuf[is->outbuf_count++] = UChar(0xF0 | ((c >> 18) & 0x07));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 12) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 6) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | (c & 0x3F));
    } else if (c <= 0x03FFFFFF) {
	OUTBUF_MAKE_FREE(is, fd, 5);
	is->outbuf[is->outbuf_count++] = UChar(0xF8 | ((c >> 24) & 0x03));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 18) & 0x3f));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 12) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 6) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | (c & 0x3F));
    } else if (c <= 0x7FFFFFFF) {
	OUTBUF_MAKE_FREE(is, fd, 6);
	is->outbuf[is->outbuf_count++] = UChar(0xFC | ((c >> 30) & 0x01));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 24) & 0x3f));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 18) & 0x3f));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 12) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | ((c >> 6) & 0x3F));
	is->outbuf[is->outbuf_count++] = UChar(0x80 | (c & 0x3F));
    } else {
	/* "21 bits ought to be enough for anybody!" -- The Unicode Consortium */
	Warning("ignoring character beyond UTF-8's 31-bit range: 0x%X.\n", c);
    }
}

static void
buffer(Iso2022Ptr is, unsigned c)
{
    if (is->buffered == NULL) {
	is->buffered_len = 10;
	is->buffered = malloc(is->buffered_len);
	if (is->buffered == NULL)
	    FatalError("Couldn't allocate buffered.\n");
    }

    if (is->buffered_count >= is->buffered_len) {
	is->buffered = realloc(is->buffered, 2 * is->buffered_len + 1);
	if (is->buffered == NULL) {
	    FatalError("Couldn't grow buffered.\n");
	}
	is->buffered_len = 2 * is->buffered_len + 1;
    }

    is->buffered[is->buffered_count++] = UChar(c);
}

static void
outbuf_buffered_carefully(Iso2022Ptr is, int fd)
{
    /* This should never happen in practice */
    unsigned i = 0;

    while (i < is->buffered_count) {
	OUTBUF_MAKE_FREE(is, fd, 1);
	is->outbuf[is->outbuf_count++] = is->buffered[i++];
    }
    is->buffered_count = 0;
}

static void
outbuf_buffered(Iso2022Ptr is, int fd)
{
    if (is->buffered_count > BUFFER_SIZE)
	outbuf_buffered_carefully(is, fd);

    OUTBUF_MAKE_FREE(is, fd, is->buffered_count);
    memcpy(is->outbuf + is->outbuf_count, is->buffered, is->buffered_count);
    is->outbuf_count += is->buffered_count;
    is->buffered_count = 0;
}

static void
discard_buffered(Iso2022Ptr is)
{
    is->buffered_count = 0;
}

Iso2022Ptr
allocIso2022(void)
{
    Iso2022Ptr is;
    is = TypeCalloc(Iso2022Rec);
    if (!is)
	return NULL;
    is->glp = is->grp = NULL;
    G0(is) = G1(is) = G2(is) = G3(is) = OTHER(is) = NULL;

    is->parserState = P_NORMAL;
    is->shiftState = S_NORMAL;

    is->inputFlags = IF_EIGHTBIT | IF_SS | IF_SSGR;
    is->outputFlags = OF_SS | OF_LS | OF_SELECT;

    is->buffered = NULL;
    is->buffered_len = 0;
    is->buffered_count = 0;

    is->buffered_ku = -1;
    is->other_pending_count = 0;

    is->outbuf = malloc((size_t) BUFFER_SIZE);
    if (!is->outbuf) {
	free(is);
	return NULL;
    }
    is->outbuf_count = 0;

    return is;
}

#ifdef NO_LEAKS
void
destroyIso2022(Iso2022Ptr is)
{
    if (is->buffered)
	free(is->buffered);
    if (is->outbuf)
	free(is->outbuf);
    free(is);
}
#endif

static int
identifyCharset(Iso2022Ptr i, const CharsetRec * *p)
{
    if (p == &G0(i)) {
	return 0;
    } else if (p == &G1(i)) {
	return 1;
    } else if (p == &G2(i)) {
	return 2;
    } else if (p == &G3(i)) {
	return 3;
    } else {
	abort();
	/* NOTREACHED */
    }
}

#define G_name(n) ((i != NULL && i->g[n] != NULL) ? NonNull(i->g[n]->name) : "unset")

void
reportIso2022(const char *tag, Iso2022Ptr i)
{
    Message("%s: ", tag);
    if (OTHER(i) != NULL) {
	Message("%s, non-ISO-2022 encoding.\n", OTHER(i)->name);
	return;
    }
    Message("G0 is %s, ", G_name(0));
    Message("G1 is %s, ", G_name(1));
    Message("G2 is %s, ", G_name(2));
    Message("G3 is %s.\n", G_name(3));
    Message("GL is G%d, ", identifyCharset(i, i->glp));
    Message("GR is G%d.\n", identifyCharset(i, i->grp));
}

int
initIso2022(const char *locale, const char *charset, Iso2022Ptr i)
{
    int gl = 0, gr = 2;
    const CharsetRec *g0 = NULL;
    const CharsetRec *g1 = NULL;
    const CharsetRec *g2 = NULL;
    const CharsetRec *g3 = NULL;
    const CharsetRec *other = NULL;
    int rc;

    TRACE(("initIso2022(locale=%s, charset=%s)\n", NonNull(locale), NonNull(charset)));
    rc = getLocaleState(locale, charset, &gl, &gr, &g0, &g1, &g2, &g3, &other);
    if (rc < 0) {
	if (charset) {
	    Warning("couldn't find charset %s; "
		    "using ISO 8859-1.\n", charset);
	} else if (ignore_locale) {
	    Warning("couldn't find charset data for %s; "
		    "using ISO 8859-1.\n", locale);
	} else {
	    Warning("couldn't find charset data for locale %s; "
		    "using ISO 8859-1.\n", locale);
	}
    }

    if (G0(i) == NULL) {
	if (g0)
	    G0(i) = g0;
	else
	    G0(i) = getCharsetByName("ASCII");
    }

    if (G1(i) == NULL) {
	if (g1)
	    G1(i) = g1;
	else
	    G1(i) = getUnknownCharset(T_94);
    }

    if (G2(i) == NULL) {
	if (g2)
	    G2(i) = g2;
	else
	    G2(i) = getCharsetByName("ISO 8859-1");
    }

    if (G3(i) == NULL) {
	if (g3)
	    G3(i) = g3;
	else
	    G3(i) = getUnknownCharset(T_94);
    }

    if (OTHER(i) == NULL) {
	if (other)
	    OTHER(i) = other;
	else
	    OTHER(i) = NULL;
    }

    if (i->glp == NULL) {
	i->glp = &i->g[gl];
    }

    if (i->grp == NULL) {
	i->grp = &i->g[gr];
    }
    trace_iso2022("...initIso2022", i);
    return 0;
}

int
mergeIso2022(Iso2022Ptr d, Iso2022Ptr s)
{
    if (G0(d) == NULL)
	G0(d) = G0(s);
    if (G1(d) == NULL)
	G1(d) = G1(s);
    if (G2(d) == NULL)
	G2(d) = G2(s);
    if (G3(d) == NULL)
	G3(d) = G3(s);
    if (OTHER(d) == NULL)
	OTHER(d) = OTHER(s);
    if (d->glp == NULL)
	d->glp = &(d->g[identifyCharset(s, s->glp)]);
    if (d->grp == NULL)
	d->grp = &(d->g[identifyCharset(s, s->grp)]);
    trace_iso2022("...mergeIso2022", d);
    return 0;
}

static int
utf8Count(unsigned c)
{
    /* All return values must be less than BUFFERED_INPUT_SIZE */
    if ((c & 0x80) == 0)
	return 1;
    else if ((c & 0x40) == 0)
	return 1;		/* incorrect UTF-8 */
    else if ((c & 0x60) == 0x40)
	return 2;
    else if ((c & 0x70) == 0x60)
	return 3;
    else if ((c & 0x78) == 0x70)
	return 4;
    else
	return 1;
}

static int
fromUtf8(unsigned char *b)
{
    if ((b[0] & 0x80) == 0)
	return b[0];
    else if ((b[0] & 0x40) == 0)
	return -1;		/* incorrect UTF-8 */
    else if ((b[0] & 0x60) == 0x40)
	return ((b[0] & 0x1F) << 6) | (b[1] & 0x3F);
    else if ((b[0] & 0x70) == 0x60)
	return (((b[0] & 0x0F) << 12) |
		((b[1] & 0x3F) << 6) |
		((b[2] & 0x3F)));
    else if ((b[0] & 0x78) == 0x70)
	/* PATCH(fork, utf-8): a 4-byte lead carries 3 bits; upstream masked
	 * 2, so U+100000-U+10FFFF (lead 0xF4) became U+0000-U+FFFF. */
	return (((b[0] & 0x07) << 18) |
		((b[1] & 0x3F) << 12) |
		((b[2] & 0x3F) << 6) |
		((b[3] & 0x3F)));
    else
	return -1;
}

/* PATCH(fork, input rejection): after a rejection, the rest of a paste is
 * dropped too (see copyIn()): until input pauses for DROP_PAUSE_MILLIS, or,
 * in a bracketed paste (xterm's mode 2004), up to the paste's end marker;
 * either way for at most DROP_MAX_MILLIS from the rejection. luit can't
 * know that an end marker will come (VS Code's
 * terminal.integrated.ignoreBracketedPasteMode, a reset, a lost
 * connection), so whatever decides how the drop ends, the bound ends it.
 * A rest arriving later gets through, as luit can't tell it from typing.
 *
 * Paste markers in input aren't passed on as they come: input that may be
 * one (ESC [ 2 0 0 ~ or ESC [ 2 0 1 ~ so far) is held until it is one, or
 * isn't (then it's input like any other), and luit passes on a whole
 * marker by its own rules (pasteMarker()). So the shell never gets part of
 * one. A lone ESC at the end of a read is held for HOLD_MILLIS at most
 * (flushHeldInput()): it's what the Escape key sends. */
static const unsigned char PASTE_START[] = "\033[200~";
static const unsigned char PASTE_END[] = "\033[201~";
#define DROP_PAUSE_MILLIS 50.0
#define DROP_MAX_MILLIS 2000.0

typedef enum {
    DROP_NONE,
    DROP_TO_PAUSE,
    DROP_TO_END
} DropMode;

static DropMode dropping = DROP_NONE;
static double drop_started = 0.0;	/* when the rejection was */
static double drop_last = 0.0;	/* when input was last dropped */
/* the program has bracketed paste on (trackPasteMode()) */
static int paste_mode = 0;
/* the terminal has sent a paste's start marker but not its end marker,
 * since when */
static int in_paste = 0;
static double paste_started = 0.0;
/* the shell got a paste's start marker but not its end marker */
static int paste_open = 0;
/* input that may be a paste marker, held, and whether it came with input
 * that was rejected or dropped (then it's dropped too if it isn't one) */
static unsigned char held[PASTE_MARKER_LEN];
static size_t held_len = 0;
static double held_since = 0.0;
static int held_dropped = 0;

/* what copyIn() has rejected in the chunk it's converting */
static int chunk_rejected = 0;
static unsigned chunk_rejected_char = 0;

/* PATCH(fork, input backpressure): converted input waiting for the pty.
 * Writes to it are non-blocking, and upstream ignored a short write, so
 * whatever didn't fit (the program wasn't reading fast enough, or the pty's
 * buffer is small, as on macOS) was lost. luit now keeps the rest and
 * doesn't read more input until it's gone (see parent() in luit.c), while
 * still reading the program's output, so neither side can deadlock.
 * copyIn() only runs once this is empty, and queues at most one chunk's
 * conversion and what earlier chunks held (see INPUT_PENDING_MAX). */
static unsigned char input_pending[INPUT_PENDING_MAX];
static size_t input_pending_len = 0;

/* PATCH(fork, input rejection): the first character copyIn() couldn't
 * encode in the chunk it last rejected (Unicode) */
unsigned input_unencodable_char = 0;

static void
queueInput(const unsigned char *p, size_t n)
{
    assert(input_pending_len + n <= sizeof(input_pending));
    memcpy(input_pending + input_pending_len, p, n);
    input_pending_len += n;
}

int
inputPending(void)
{
    return input_pending_len > 0;
}

/* Moves all held-back input into dst, which has room for
 * INPUT_PENDING_MAX bytes, and returns its length */
size_t
takeInput(unsigned char *dst)
{
    size_t n = input_pending_len;

    memcpy(dst, input_pending, n);
    input_pending_len = 0;
    return n;
}

/* Writes as much held-back input as fd takes now; with block, waits until
 * all of it is written. Returns -1 if fd can't be written to any more. */
int
flushInput(int fd, int block)
{
    size_t done = 0;
    int rc = 0;

    while (done < input_pending_len) {
	ssize_t n = write(fd, input_pending + done, input_pending_len - done);
	if (n > 0) {
	    done += (size_t) n;
	} else if (n < 0 && errno == EINTR) {
	    continue;
	} else if (n < 0 && errno == EAGAIN && block) {
	    if (waitForOutput(fd) == IO_Closed) {
		rc = -1;
		break;
	    }
	} else {
	    if (!(n < 0 && errno == EAGAIN))
		rc = -1;
	    break;
	}
    }
    if (rc < 0)
	done = input_pending_len;	/* nothing will take it */
    memmove(input_pending, input_pending + done, input_pending_len - done);
    input_pending_len -= done;
    return rc;
}

/* PATCH(fork, input rejection): puts the input parser where it would be
 * after bytes it didn't see: dropped input, which ends with the drop, or a
 * paste marker, which ends an escape sequence (an Escape key before it) and
 * a cut UTF-8 sequence */
static void
resetInputParser(Iso2022Ptr is)
{
    is->parserState = P_NORMAL;
    buffered_input_count = 0;
}

/* Ends a drop */
static void
endDrop(Iso2022Ptr is)
{
    dropping = DROP_NONE;
    resetInputParser(is);
}

/* Ends a drop at its bound or, if it goes until a pause, after one */
static void
checkDrop(Iso2022Ptr is, double now)
{
    if (dropping != DROP_NONE && now - drop_started >= DROP_MAX_MILLIS)
	endDrop(is);
    else if (dropping == DROP_TO_PAUSE && now - drop_last >= DROP_PAUSE_MILLIS)
	endDrop(is);
}

/* PATCH(fork, input rejection): follows the program turning bracketed
 * paste on and off in its output: ESC [ ? Pm h / l with 2004 among the
 * parameters (ESC [ ? 2004 h, also combined as in ESC [ ? 1049 ; 2004 h),
 * and a reset (ESC c), which turns it off. The state carries over between
 * reads. */
static enum {
    OUT_TEXT,
    OUT_ESC,
    OUT_CSI,
    OUT_PRIVATE
} out_state = OUT_TEXT;
static unsigned out_param = 0;
static int out_has_2004 = 0;

static void
pasteModeOff(void)
{
    paste_mode = 0;
    in_paste = 0;		/* the program is done with pastes */
    paste_open = 0;
}

static void
trackPasteMode(const unsigned char *buf, size_t n)
{
    const unsigned char *s = buf, *end = buf + n;

    while (s < end) {
	unsigned char b;

	if (out_state == OUT_TEXT) {
	    s = memchr(s, ESC, (size_t) (end - s));
	    if (s == NULL)
		return;
	    out_state = OUT_ESC;
	    s++;
	    continue;
	}
	b = *s++;
	switch (out_state) {
	case OUT_ESC:
	    if (b == 'c')
		pasteModeOff();
	    out_state = (b == '[') ? OUT_CSI : (b == ESC) ? OUT_ESC : OUT_TEXT;
	    break;
	case OUT_CSI:
	    out_state = (b == '?') ? OUT_PRIVATE : (b == ESC) ? OUT_ESC : OUT_TEXT;
	    out_param = 0;
	    out_has_2004 = 0;
	    break;
	case OUT_PRIVATE:
	    if (b >= '0' && b <= '9') {
		if (out_param < 100000)
		    out_param = out_param * 10 + (unsigned) (b - '0');
	    } else if (b == ';') {
		out_has_2004 |= out_param == 2004;
		out_param = 0;
	    } else {
		out_has_2004 |= out_param == 2004;
		if (out_has_2004 && b == 'h')
		    paste_mode = 1;
		else if (out_has_2004 && b == 'l')
		    pasteModeOff();
		out_state = (b == ESC) ? OUT_ESC : OUT_TEXT;
	    }
	    break;
	case OUT_TEXT:
	    break;
	}
    }
}

/* PATCH(fork, input rejection): forgets the paste state, for
 * tests/paste_driver.c, which runs scenarios one after another on one
 * input state */
void
resetPasteTracking(void)
{
    dropping = DROP_NONE;
    in_paste = 0;
    paste_open = 0;
    held_len = 0;
}

/*
 * PATCH(fork, input rejection): converts keyboard input (UTF-8) and queues
 * it -- or, if any character in it can't be encoded, queues none of it and
 * returns 1, with the character in input_unencodable_char. Substituting or
 * dropping just that character would change what the shell runs
 * (`rm <emoji>*` would become `rm ?*` or `rm *`).
 */
static int
convertUnit(Iso2022Ptr is, unsigned char *buf, int count)
{
    unsigned char *c;
    int codepoint, rem;
    unsigned char out[CONVERTED_CHUNK_MAX];
    size_t outlen = 0;
    int rejected = 0;

    assert(count <= BUFFER_SIZE);

    c = buf;
    rem = count;

#define NEXT do {c++; rem--;} while(0)

    while (rem > 0) {
	codepoint = -1;
	if (is->parserState == P_ESC) {
	    assert(buffered_input_count == 0);
	    codepoint = *c;
	    NEXT;
	    if (*c == CSI_7)
		is->parserState = P_CSI;
	    else if (IS_FINAL_ESC(codepoint))
		is->parserState = P_NORMAL;
	} else if (is->parserState == P_CSI) {
	    assert(buffered_input_count == 0);
	    codepoint = *c;
	    NEXT;
	    if (IS_FINAL_CSI(codepoint))
		is->parserState = P_NORMAL;
	} else if (!(*c & 0x80)) {
	    if (buffered_input_count > 0) {
		buffered_input_count = 0;
		continue;
	    } else {
		codepoint = *c;
		NEXT;
		if (codepoint == ESC)
		    is->parserState = P_ESC;
	    }
	} else if ((*c & 0x40)) {
	    if (buffered_input_count > 0) {
		buffered_input_count = 0;
		continue;
	    } else {
		buffered_input[buffered_input_count] = *c;
		buffered_input_count++;
		NEXT;
	    }
	} else {
	    if (buffered_input_count <= 0) {
		buffered_input_count = 0;
		NEXT;
		continue;
	    } else {
		buffered_input[buffered_input_count] = *c;
		buffered_input_count++;
		NEXT;
		if (buffered_input_count >= utf8Count(buffered_input[0])) {
		    codepoint = fromUtf8(buffered_input);
		    buffered_input_count = 0;
		    if (codepoint == CSI)
			is->parserState = P_CSI;
		}
	    }
	}
#undef NEXT

	if (codepoint >= 0) {
	    int i;
	    unsigned ucode = (unsigned) codepoint;
	    unsigned char obuf[4];

#define EMIT(n) do { \
	    assert(outlen + (size_t) (n) <= sizeof(out)); \
	    memcpy(out + outlen, obuf, (size_t) (n)); \
	    outlen += (size_t) (n); \
	} while(0)

#define WRITE_1(i) do { \
	    obuf[0] = UChar(i); \
	    EMIT(1); \
	} while(0)
#define WRITE_2(i) do { \
	    obuf[0] = UChar(((i) >> 8) & 0xFF); \
	    obuf[1] = UChar((i) & 0xFF); \
	    EMIT(2); \
	} while(0)

#define WRITE_3(i) do { \
	    obuf[0] = UChar(((i) >> 16) & 0xFF); \
	    obuf[1] = UChar(((i) >>  8) & 0xFF); \
	    obuf[2] = UChar((i) & 0xFF); \
	    EMIT(3); \
	} while(0)

#define WRITE_4(i) do { \
	    obuf[0] = UChar(((i) >> 24) & 0xFF); \
	    obuf[1] = UChar(((i) >> 16) & 0xFF); \
	    obuf[2] = UChar(((i) >>  8) & 0xFF); \
	    obuf[3] = UChar((i) & 0xFF); \
	    EMIT(4); \
       } while(0)

#define WRITE_1_P_8bit(p, i) { \
	    obuf[0] = UChar(p); \
	    obuf[1] = UChar(i); \
	    EMIT(2); \
	}

#define WRITE_1_P_7bit(p, i) { \
	    obuf[0] = ESC; \
	    obuf[1] = UChar((p) - 0x40); \
	    obuf[2] = UChar(i); \
	    EMIT(3); \
	}

#define WRITE_1_P(p,i) do { \
	if(is->inputFlags & IF_EIGHTBIT) \
	    WRITE_1_P_8bit(p,i) else \
	    WRITE_1_P_7bit(p,i) \
	} while(0)

#define WRITE_2_P_8bit(p, i) { \
	    obuf[0] = UChar(p); \
	    obuf[1] = UChar(((i) >> 8) & 0xFF); \
	    obuf[2] = UChar((i) & 0xFF); \
	    EMIT(3); \
	}

#define WRITE_2_P_7bit(p, i) { \
	    obuf[0] = ESC; \
	    obuf[1] = UChar((p) - 0x40); \
	    obuf[2] = UChar(((i) >> 8) & 0xFF); \
	    obuf[3] = UChar((i) & 0xFF); \
	    EMIT(4); \
	}

#define WRITE_2_P(p,i) do { \
	    if(is->inputFlags & IF_EIGHTBIT) \
		WRITE_2_P_8bit(p,i) \
	    else \
		WRITE_2_P_7bit(p,i) \
	} while(0)

#define WRITE_1_P_S(p,i,s) do { \
	    obuf[0] = UChar(p); \
	    obuf[1] = UChar((i) & 0xFF); \
	    obuf[2] = UChar(s); \
	    EMIT(3); \
	} while(0)

#define REJECT() do { \
	    if (!rejected) \
		input_unencodable_char = ucode; \
	    rejected = 1; \
	} while(0)

#define WRITE_2_P_S(p,i,s) do { \
	    obuf[0] = UChar(p); \
	    obuf[1] = UChar(((i) >> 8) & 0xFF); \
	    obuf[2] = UChar((i) & 0xFF); \
	    obuf[3] = UChar(s); \
	    EMIT(4); \
	} while(0)

	    if (ucode < 0x20 ||
		(OTHER(is) == NULL && CHARSET_REGULAR(GR(is)) &&
		 (ucode >= 0x80 && ucode < 0xA0))) {
		WRITE_1(ucode);
		continue;
	    }
	    if (OTHER(is) != NULL
		&& OTHER(is)->other_reverse != NULL) {
		unsigned int c2;
		c2 = OTHER(is)->other_reverse(ucode, OTHER(is)->other_aux);
		if (c2 >> 24)
		    WRITE_4(c2);
		else if (c2 >> 16)
		    WRITE_3(c2);
		else if (c2 >> 8)
		    WRITE_2(c2);
		else if (c2)
		    WRITE_1(c2);
		else		/* PATCH(fork, input rejection) */
		    REJECT();
		continue;
	    }
	    i = (GL(is)->reverse) (ucode, GL(is));
	    if (i >= 0) {
		switch (GL(is)->type) {
		case T_94:
		case T_96:
		case T_128:
		    if (i >= 0x20)
			WRITE_1(i);
		    break;
		case T_9494:
		case T_9696:
		case T_94192:
		    if (i >= 0x2020)
			WRITE_2(i);
		    break;
		default:
		    abort();
		    /* NOTREACHED */
		}
		continue;
	    }
	    if (is->inputFlags & IF_EIGHTBIT) {
		i = GR(is)->reverse(ucode, GR(is));
		if (i >= 0) {
		    switch (GR(is)->type) {
		    case T_94:
		    case T_96:
		    case T_128:
			/* we allow C1 characters if T_128 in GR */
			WRITE_1(i | 0x80);
			break;
		    case T_9494:
		    case T_9696:
			WRITE_2(i | 0x8080);
			break;
		    case T_94192:
			WRITE_2(i | 0x8000);
			break;
		    default:
			abort();
			/* NOTREACHED */
		    }
		    continue;
		}
	    }
	    /* PATCH(fork, fallback) + PATCH(fork, T_128 control range): the
	     * G2/G3 lookups go on to the next character only when they wrote
	     * one. Upstream continued after them unconditionally, so with
	     * single shifts on (IF_SS), a character neither G2 nor G3 has never
	     * reached the LS lookup or the rejection after it, and vanished.
	     * The same happened when reverse() found a code but it was too low
	     * to write (a T_128 code that lands in 0x00-0x1F after the shift,
	     * e.g. CP852's 0x80-0x9F). See "Known upstream bugs and fixes" in
	     * docs/transcoder-design.md. */
	    if (is->inputFlags & IF_SS) {
		i = G2(is)->reverse(ucode, G2(is));
		if (i >= 0) {
		    int wrote = 0;
		    /* PATCH(fork, single shifts): upstream switched on GR's type
		     * here and below, so a G2/G3 set of another size than GR (EUC-JP's
		     * 1-byte JIS X 0201 katakana in G2, next to 2-byte JIS X 0208 in
		     * GR) was never written: typed half-width katakana was dropped. */
		    switch (G2(is)->type) {
		    case T_94:
		    case T_96:
		    case T_128:
			if (i >= 0x20) {
			    if ((is->inputFlags & IF_EIGHTBIT) &&
				(is->inputFlags & IF_SSGR))
				i |= 0x80;
			    WRITE_1_P(SS2, i);
			    wrote = 1;
			}
			break;
		    case T_9494:
		    case T_9696:
			if (i >= 0x2020) {
			    if ((is->inputFlags & IF_EIGHTBIT) &&
				(is->inputFlags & IF_SSGR))
				i |= 0x8080;
			    WRITE_2_P(SS2, i);
			    wrote = 1;
			}
			break;
		    case T_94192:
			if (i >= 0x2020) {
			    if ((is->inputFlags & IF_EIGHTBIT) &&
				(is->inputFlags & IF_SSGR))
				i |= 0x8000;
			    WRITE_2_P(SS2, i);
			    wrote = 1;
			}
			break;
		    default:
			abort();
			/* NOTREACHED */
		    }
		    if (wrote)
			continue;
		}
	    }
	    if (is->inputFlags & IF_SS) {
		i = G3(is)->reverse(ucode, G3(is));
		if (i >= 0) {
		    int wrote = 0;
		    switch (G3(is)->type) {
		    case T_94:
		    case T_96:
		    case T_128:
			if (i >= 0x20) {
			    if ((is->inputFlags & IF_EIGHTBIT) &&
				(is->inputFlags & IF_SSGR))
				i |= 0x80;
			    WRITE_1_P(SS3, i);
			    wrote = 1;
			}
			break;
		    case T_9494:
		    case T_9696:
			if (i >= 0x2020) {
			    if ((is->inputFlags & IF_EIGHTBIT) &&
				(is->inputFlags & IF_SSGR))
				i |= 0x8080;
			    WRITE_2_P(SS3, i);
			    wrote = 1;
			}
			break;
		    case T_94192:
			if (i >= 0x2020) {
			    if ((is->inputFlags & IF_EIGHTBIT) &&
				(is->inputFlags & IF_SSGR))
				i |= 0x8000;
			    WRITE_2_P(SS3, i);
			    wrote = 1;
			}
			break;
		    default:
			abort();
			/* NOTREACHED */
		    }
		    if (wrote)
			continue;
		}
	    }
	    if (is->inputFlags & IF_LS) {
		i = GR(is)->reverse(ucode, GR(is));
		if (i >= 0) {
		    switch (GR(is)->type) {
		    case T_94:
		    case T_96:
		    case T_128:
			WRITE_1_P_S(LS1, i, LS0);
			break;
		    case T_9494:
		    case T_9696:
			WRITE_2_P_S(LS1, i, LS0);
			break;
		    case T_94192:
			WRITE_2_P_S(LS1, i, LS0);
			break;
		    default:
			abort();
			/* NOTREACHED */
		    }
		    continue;
		}
	    }
	    /* PATCH(fork, fallback) + PATCH(fork, input rejection): every
	     * reverse lookup -- GL/GR/G2/G3/LS -- failed. Upstream wrote
	     * nothing and moved on, so the keystroke vanished; the chunk is
	     * rejected as a whole instead (see the comment above copyIn). */
	    REJECT();
	}
#undef WRITE_1
#undef WRITE_2
#undef WRITE_1_P
#undef WRITE_1_P_7bit
#undef WRITE_1_P_8bit
#undef WRITE_2_P
#undef WRITE_2_P_7bit
#undef WRITE_2_P_8bit
#undef REJECT
#undef EMIT
    }

    if (!rejected && outlen > 0)
	queueInput(out, outlen);
    return rejected;
}

/* PATCH(fork, task command line): converts one chunk of input that isn't
 * keyboard input (encodeLastArg()'s command line) as copyIn() converts
 * text, and queues it, or returns 1 as it does. Escapes, paste markers
 * among them, are text there, and the paste state is left alone. */
int
copyInText(Iso2022Ptr is, unsigned char *buf, int count)
{
    assert(input_pending_len == 0);
    return convertUnit(is, buf, count);
}

/* PATCH(fork, input rejection): input between paste markers: dropped,
 * rejected (which starts a drop) or converted and queued */
static void
inputText(Iso2022Ptr is, unsigned char *p, size_t n, double now)
{
    if (n == 0)
	return;
    checkDrop(is, now);
    if (dropping != DROP_NONE) {
	drop_last = now;
    } else if (convertUnit(is, p, (int) n)) {
	if (!chunk_rejected)
	    chunk_rejected_char = input_unencodable_char;
	chunk_rejected = 1;
	dropping = (in_paste && paste_mode) ? DROP_TO_END : DROP_TO_PAUSE;
	drop_started = drop_last = now;
    }
}

/* PATCH(fork, input rejection): a whole paste marker in input. The shell
 * gets a start marker unless it's dropped, and an end marker unless the
 * start was dropped: it ends a paste the shell got the start of, or one
 * whose start went through as text (cut by more than HOLD_MILLIS after its
 * ESC, or by more than DROP_MAX_MILLIS later, see flushHeldInput()), which
 * the shell may have taken for one. */
static void
pasteMarker(Iso2022Ptr is, int end, double now)
{
    resetInputParser(is);
    checkDrop(is, now);
    if (!end) {
	if (!in_paste)
	    paste_started = now;
	in_paste = 1;
	if (dropping != DROP_NONE) {
	    drop_last = now;
	} else if (!paste_open) {
	    queueInput(PASTE_START, PASTE_MARKER_LEN);
	    paste_open = 1;
	}
	return;
    }
    if (paste_open || !in_paste)
	queueInput(PASTE_END, PASTE_MARKER_LEN);
    in_paste = 0;
    paste_open = 0;
    if (dropping == DROP_TO_END)
	endDrop(is);
    else if (dropping != DROP_NONE)
	drop_last = now;
}

/* PATCH(fork, input rejection): whether held input can still become a
 * paste marker with b */
static int
heldContinues(unsigned char b)
{
    if (held_len == 4)		/* ESC [ 2 0, then 0 to start or 1 to end */
	return b == '0' || b == '1';
    return b == PASTE_START[held_len];
}

/* Passes the first n bytes held, from earlier reads, on as input like any
 * other, or drops them with the input they came with, and forgets what's
 * held */
static void
releaseHeld(Iso2022Ptr is, size_t n, double now)
{
    unsigned char bytes[PASTE_MARKER_LEN];

    held_len = 0;
    if (held_dropped)
	return;
    memcpy(bytes, held, n);
    inputText(is, bytes, n, now);
}

int
inputHeld(void)
{
    return held_len > 0;
}

/* PATCH(fork, input rejection): passes on what's held once it has been for
 * HOLD_MILLIS if it's a lone ESC (an Escape key, most likely), or for
 * DROP_MAX_MILLIS if it's more (no key sends ESC [ 2 0 alone), returning 1
 * if it did. Not in a paste: there what's held is the paste's, most likely
 * the start of its end marker, which the terminal always sends, and passing
 * it on as text would leave the shell in the paste. A shell in the paste
 * takes an Escape key as text anyway, so what's held there waits for what
 * comes next; in a paste the shell didn't get the start of (dropped), for
 * DROP_MAX_MILLIS from its start at most, in case no end marker comes. */
int
flushHeldInput(Iso2022Ptr is, double now)
{
    double hold = (held_len == 1) ? HOLD_MILLIS : DROP_MAX_MILLIS;

    if (held_len == 0 || now - held_since < hold || paste_open
	|| (in_paste && now - paste_started < DROP_MAX_MILLIS))
	return 0;
    releaseHeld(is, held_len, now);
    return 1;
}

/* PATCH(fork, input rejection): the clock of copyIn() and flushHeldInput():
 * `now` less the time input waited for the pty to take what was converted
 * before, so the rest of a paste waiting for a busy program isn't late.
 * parent() in luit.c passes the time through here whenever that wait may
 * have started or ended. */
static double input_waited = 0.0;
static double input_waiting_since = -1.0;

double
inputClock(double now)
{
    if (input_pending_len > 0) {
	if (input_waiting_since < 0.0)
	    input_waiting_since = now;
    } else if (input_waiting_since >= 0.0) {
	input_waited += now - input_waiting_since;
	input_waiting_since = -1.0;
    }
    return now - input_waited;
}

/*
 * PATCH(fork, input rejection): converts one chunk of keyboard input and
 * queues it (flushInput() writes it, takeInput() hands it over), `now`
 * being the time in milliseconds. The chunk is handled in parts between
 * paste markers, which are handled on their own (see pasteMarker()): a part
 * with a character that can't be encoded is rejected as a whole (see
 * convertUnit()), escapes in it included, and what follows it dropped (see
 * DROP_MAX_MILLIS). What's held at the chunk's end (see flushHeldInput())
 * goes with the part before it: if it isn't a marker, it's dropped if that
 * part was, or else passed on by itself, and what the next chunk added to
 * it goes with that chunk. Returns 1 if anything was rejected (the caller
 * rings the bell), with the first character that couldn't be encoded in
 * input_unencodable_char.
 */
int
copyIn(Iso2022Ptr is, unsigned char *buf, int count, double now)
{
    size_t n = (size_t) count, from = 0, held_at = 0, k;
    size_t carried = held_len;	/* held from earlier chunks */
    int held_here = 0;		/* what's held starts at buf + held_at */

    assert(count <= BUFFER_SIZE);
    assert(input_pending_len == 0);
    chunk_rejected = 0;

    for (k = 0; k < n; k++) {
	unsigned char b = buf[k];

	if (held_len > 0) {
	    if (heldContinues(b)) {
		held[held_len++] = b;
		held_since = now;
		if (held_len == PASTE_MARKER_LEN) {
		    if (held_here)
			inputText(is, buf + from, held_at - from, now);
		    held_len = 0;
		    held_here = 0;
		    pasteMarker(is, held[4] == '1', now);
		    from = k + 1;
		}
		continue;
	    }
	    /* not a marker after all */
	    if (held_here) {
		held_len = 0;	/* part of the text from `from` */
		held_here = 0;
	    } else {
		releaseHeld(is, carried, now);	/* buf from 0 is text */
	    }
	}
	if (b == ESC) {
	    held[0] = b;
	    held_len = 1;
	    held_since = now;
	    held_at = k;
	    held_here = 1;
	}
    }
    if (held_len == 0) {
	inputText(is, buf + from, n - from, now);
    } else if (held_here) {
	inputText(is, buf + from, held_at - from, now);
	checkDrop(is, now);
	held_dropped = dropping != DROP_NONE;
	if (held_dropped)
	    drop_last = now;
    }
    if (chunk_rejected)
	input_unencodable_char = chunk_rejected_char;
    return chunk_rejected;
}

#define PAIR(a,b) ((unsigned) ((a) << 8) | (b))

/*
 * PATCH(fork, invalid sequences): decodes one byte with OTHER's stack
 * function. Like VS Code's editor (iconv-lite), bytes that can't make a
 * character show U+FFFD for their first byte, and decoding goes on from the
 * second; upstream dropped them silently. Returns 0 if the byte has to be
 * read again (after the bytes before it).
 */
static int
otherByte(Iso2022Ptr is, int fd, unsigned char b)
{
    const CharsetRec *other = OTHER(is);
    int c = other->other_stack(b, other->other_aux);
    unsigned count = is->other_pending_count + 1;
    unsigned char rest[sizeof(is->other_pending)];
    unsigned i;

    if (c == -1) {
	if (is->other_pending_count < sizeof(is->other_pending))
	    is->other_pending[is->other_pending_count++] = b;
	return 1;
    }
    if (c != OTHER_INVALID) {
	unsigned ucode = other->other_recode((unsigned) c, other->other_aux);
	/* An unmapped 4-byte GB18030 sequence stays one U+FFFD, as in the
	 * WHATWG Encoding Standard: every one of them is well-formed. */
	if (ucode != UNICODE_REPLACEMENT_CHAR || count == 1 || count == 4) {
	    outbufUTF8(is, fd, ucode);
	    is->other_pending_count = 0;
	    return 1;
	}
    }
    outbufUTF8(is, fd, UNICODE_REPLACEMENT_CHAR);
    if (count == 1)
	return 1;
    count -= 2;
    memcpy(rest, is->other_pending + 1, count);
    is->other_pending_count = 0;
    for (i = 0; i < count; i++) {
	while (!otherByte(is, fd, rest[i])) {
	    /* read rest[i] again */
	}
    }
    return 0;
}

void
copyOut(Iso2022Ptr is, int fd, unsigned char *buf, unsigned count)
{
    unsigned char *s = buf;

    if (ilog >= 0)
	IGNORE_RC(write(ilog, buf, (size_t) count));
    trackPasteMode(buf, (size_t) count);	/* PATCH(fork, input rejection) */

    while (s < buf + count) {
	switch (is->parserState) {
	case P_NORMAL:
	  resynch:
	    if (is->buffered_ku < 0) {
		if (*s == ESC && is->other_pending_count == 0) {
		    buffer(is, *s++);
		    is->parserState = P_ESC;
		} else if (OTHER(is) != NULL
			   && OTHER(is)->other_recode != NULL
			   && OTHER(is)->other_stack != NULL
			   && OTHER(is)->other_aux != NULL) {
		    if (otherByte(is, fd, *s))
			s++;
		    is->shiftState = S_NORMAL;
		} else if (*s == CSI && CHARSET_REGULAR(GR(is))) {
		    buffer(is, *s++);
		    is->parserState = P_CSI;
		} else if ((*s == SS2 ||
			    *s == SS3 ||
			    *s == LS0 ||
			    *s == LS1) &&
			   CHARSET_REGULAR(GR(is))) {
		    buffer(is, *s++);
		    terminate(is, fd);
		    is->parserState = P_NORMAL;
		} else if (*s <= 0x20 && is->shiftState == S_NORMAL) {
		    /* Pass through C0 when GL is not regular */
		    outbufOne(is, fd, *s);
		    s++;
		} else {
		    const CharsetRec *charset;
		    unsigned char code = 0;
		    if (*s <= 0x7F) {
			switch (is->shiftState) {
			case S_NORMAL:
			    charset = GL(is);
			    break;
			case S_SS2:
			    charset = G2(is);
			    break;
			case S_SS3:
			    charset = G3(is);
			    break;
			default:
			    abort();
			    /* NOTREACHED */
			}
			code = *s;
		    } else {
			switch (is->shiftState) {
			case S_NORMAL:
			    charset = GR(is);
			    break;
			case S_SS2:
			    charset = G2(is);
			    break;
			case S_SS3:
			    charset = G3(is);
			    break;
			default:
			    abort();
			    /* NOTREACHED */
			}
			code = UChar(*s - 0x80);
		    }

		    switch (charset->type) {
		    case T_94:
			if (code >= 0x21 && code <= 0x7E)
			    outbufUTF8(is, fd, charset->recode(code, charset));
			else
			    outbufUTF8(is, fd, *s);
			s++;
			is->shiftState = S_NORMAL;
			break;
		    case T_96:
			if (code >= 0x20)
			    outbufUTF8(is, fd, charset->recode(code, charset));
			else
			    outbufUTF8(is, fd, *s);
			is->shiftState = S_NORMAL;
			s++;
			break;
		    case T_128:
			outbufUTF8(is, fd, charset->recode(code, charset));
			is->shiftState = S_NORMAL;
			s++;
			break;
		    default:
			/* First byte of a multibyte sequence */
			is->buffered_ku = *s;
			s++;
		    }
		}
	    } else {		/* buffered_ku */
		const CharsetRec *charset;
		unsigned char ku_code;
		unsigned code = 0;
		if (is->buffered_ku <= 0x7F) {
		    switch (is->shiftState) {
		    case S_NORMAL:
			charset = GL(is);
			break;
		    case S_SS2:
			charset = G2(is);
			break;
		    case S_SS3:
			charset = G3(is);
			break;
		    default:
			abort();
			/* NOTREACHED */
		    }
		    ku_code = UChar(is->buffered_ku);
		    if (*s < 0x80)
			code = *s;
		} else {
		    switch (is->shiftState) {
		    case S_NORMAL:
			charset = GR(is);
			break;
		    case S_SS2:
			charset = G2(is);
			break;
		    case S_SS3:
			charset = G3(is);
			break;
		    default:
			abort();
			/* NOTREACHED */
		    }
		    ku_code = UChar(is->buffered_ku - 0x80);
		    if (*s >= 0x80)
			code = UChar(*s - 0x80);
		}
		switch (charset->type) {
		case T_94:
		case T_96:
		case T_128:
		    abort();
		    /* NOTREACHED */
		    break;
		case T_9494:
		    if (code >= 0x21 && code <= 0x7E) {
			outbufUTF8(is, fd,
				   charset->recode(PAIR(ku_code, code), charset));
			is->buffered_ku = -1;
			is->shiftState = S_NORMAL;
		    } else {
			is->buffered_ku = -1;
			is->shiftState = S_NORMAL;
			goto resynch;
		    }
		    s++;
		    break;
		case T_9696:
		    if (code >= 0x20) {
			outbufUTF8(is, fd,
				   charset->recode(PAIR(ku_code, code), charset));
			is->buffered_ku = -1;
			is->shiftState = S_NORMAL;
		    } else {
			is->buffered_ku = -1;
			is->shiftState = S_NORMAL;
			goto resynch;
		    }
		    s++;
		    break;
		case T_94192:
		    /* Use *s, not code */
		    if (((*s >= 0x21) && (*s <= 0x7E)) ||
			((*s >= 0xA1) && (*s <= 0xFE))) {
			unsigned ucode = PAIR(ku_code, *s);
			outbufUTF8(is, fd,
				   charset->recode(ucode, charset));
			is->buffered_ku = -1;
			is->shiftState = S_NORMAL;
		    } else {
			is->buffered_ku = -1;
			is->shiftState = S_NORMAL;
			goto resynch;
		    }
		    s++;
		    break;
		default:
		    abort();
		    /* NOTREACHED */
		}
	    }
	    break;
	case P_ESC:
	    assert(is->buffered_ku == -1);
	    if (*s == CSI_7) {
		buffer(is, *s++);
		is->parserState = P_CSI;
	    } else if (IS_FINAL_ESC(*s)) {
		buffer(is, *s++);
		terminate(is, fd);
		is->parserState = P_NORMAL;
	    } else {
		buffer(is, *s++);
	    }
	    break;
	case P_CSI:
	    if (IS_FINAL_CSI(*s)) {
		buffer(is, *s++);
		terminate(is, fd);
		is->parserState = P_NORMAL;
	    } else {
		buffer(is, *s++);
	    }
	    break;
	default:
	    abort();
	    /* NOTREACHED */
	}
    }
    outbuf_flush(is, fd);
}

static void
terminate(Iso2022Ptr is, int fd)
{
    if (is->outputFlags & OF_PASSTHRU) {
	outbuf_buffered(is, fd);
	return;
    }

    switch (is->buffered[0]) {
    case SS2:
	if (is->outputFlags & OF_SS)
	    is->shiftState = S_SS2;
	discard_buffered(is);
	return;
    case SS3:
	if (is->outputFlags & OF_SS)
	    is->shiftState = S_SS3;
	discard_buffered(is);
	return;
    case LS0:
	if (is->outputFlags & OF_LS)
	    is->glp = &G0(is);
	discard_buffered(is);
	return;
    case LS1:
	if (is->outputFlags & OF_LS)
	    is->glp = &G1(is);
	discard_buffered(is);
	return;
    case ESC:
	assert(is->buffered_count >= 2);
	switch (is->buffered[1]) {
	case SS2_7:
	    if (is->outputFlags & OF_SS)
		is->shiftState = S_SS2;
	    discard_buffered(is);
	    return;
	case SS3_7:
	    if (is->outputFlags & OF_SS)
		is->shiftState = S_SS3;
	    discard_buffered(is);
	    return;
	case LS2_7:
	    if (is->outputFlags & OF_SS)
		is->glp = &G2(is);
	    discard_buffered(is);
	    return;
	case LS3_7:
	    if (is->outputFlags & OF_LS)
		is->glp = &G3(is);
	    discard_buffered(is);
	    return;
	case LS1R_7:
	    if (is->outputFlags & OF_LS)
		is->grp = &G1(is);
	    discard_buffered(is);
	    return;
	case LS2R_7:
	    if (is->outputFlags & OF_LS)
		is->grp = &G2(is);
	    discard_buffered(is);
	    return;
	case LS3R_7:
	    if (is->outputFlags & OF_LS)
		is->grp = &G3(is);
	    discard_buffered(is);
	    return;
	default:
	    terminateEsc(is, fd,
			 is->buffered + 1,
			 (unsigned) (is->buffered_count - 1));
	    break;
	}
	return;
    default:
	outbuf_buffered(is, fd);
    }
}

static void
terminateEsc(Iso2022Ptr is, int fd, unsigned char *s_start, unsigned count)
{
    const CharsetRec *charset;

    /* ISO 2022 doesn't allow 2C, but Emacs/MULE uses it in 7-bit
       mode */

    if ((s_start[0] == 0x28 || s_start[0] == 0x29 ||
	 s_start[0] == 0x2A || s_start[0] == 0x2B ||
	 s_start[0] == 0x2C || s_start[0] == 0x2D ||
	 s_start[0] == 0x2E || s_start[0] == 0x2F) &&
	count >= 2) {
	if (is->outputFlags & OF_SELECT) {
	    if (s_start[0] <= 0x2B)
		charset = getCharset(s_start[1], T_94);
	    else
		charset = getCharset(s_start[1], T_96);
	    switch (s_start[0]) {
	    case 0x28:
	    case 0x2C:
		G0(is) = charset;
		break;
	    case 0x29:
	    case 0x2D:
		G1(is) = charset;
		break;
	    case 0x2A:
	    case 0x2E:
		G2(is) = charset;
		break;
	    case 0x2B:
	    case 0x2F:
		G3(is) = charset;
		break;
	    }
	}
	discard_buffered(is);
    } else if (s_start[0] == 0x24 && count == 2) {
	if (is->outputFlags & OF_SELECT) {
	    charset = getCharset(s_start[1], T_9494);
	    G0(is) = charset;
	}
	discard_buffered(is);
    } else if (s_start[0] == 0x24 && count >= 2 &&
	       (s_start[1] == 0x28 || s_start[1] == 0x29 ||
		s_start[1] == 0x2A || s_start[1] == 0x2B ||
		s_start[1] == 0x2D || s_start[1] == 0x2E ||
		s_start[1] == 0x2F) &&
	       count >= 3) {
	if (is->outputFlags & OF_SELECT) {
	    if (s_start[1] <= 0x2B)
		charset = getCharset(s_start[2], T_9494);
	    else
		charset = getCharset(s_start[2], T_9696);
	    switch (s_start[1]) {
	    case 0x28:
		G0(is) = charset;
		break;
	    case 0x29:
	    case 0x2D:
		G1(is) = charset;
		break;
	    case 0x2A:
	    case 0x2E:
		G2(is) = charset;
		break;
	    case 0x2B:
	    case 0x2F:
		G3(is) = charset;
		break;
	    }
	}
	discard_buffered(is);
    } else
	outbuf_buffered(is, fd);
}

#ifdef NO_LEAKS
void
iso2022_leaks(void)
{
}
#endif
