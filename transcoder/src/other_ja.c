/*
 * other_ja.c -- CP932 (Windows-31J) direct-lookup charset (fork-local)
 *
 * See docs/transcoder-design.md.
 *
 * Modeled on init_hkscs/mapping_hkscs/reverse_hkscs/stack_hkscs in
 * other.c (same aux_* shape: one FontMapPtr + one FontMapReversePtr +
 * a 1-byte lookahead buffer), rather than on mapping_sjis(): the
 * upstream SJIS->JIS coordinate formula in other.c only covers the
 * base 94-row JIS X 0208 grid and is not valid for the IBM-extension
 * lead-byte ranges (0xFA-0xFC) or NEC-selected-IBM range (0xED-0xEE)
 * used by CP932/Windows-31J -- confirmed by hand-checking the formula
 * against 0xFBFC (髙), which yields an out-of-range JIS row.
 * A flat table sidesteps that entirely.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <other.h>
#include <sys.h>
#include <luitconv.h>

#define HALFWIDTH_10646 0xFF61
#define HALFWIDTH_SJIS_LO 0xA1
#define HALFWIDTH_SJIS_HI 0xDF
#define YEN_SJIS   0x5C
#define YEN_10646  0x00A5
#define OVERLINE_SJIS  0x7E
#define OVERLINE_10646 0x203E

int
init_cp932(OtherStatePtr s)
{
    s->cp932.mapping = LookupMapping("cp932-direct-0", us16BIT);
    if (!s->cp932.mapping)
	return 0;

    s->cp932.reverse = LookupReverse(s->cp932.mapping);
    if (!s->cp932.reverse)
	return 0;

    s->cp932.buf = -1;
    return 1;
}

#define UNICODE_REPLACEMENT_CHAR 0xFFFD

/*
 * PATCH(fork, refactor): the fallback handling used to be copy-pasted into 8
 * places (mapping_/reverse_ for cp932, gb18030x, gbkx, hkscsx) -- the same
 * kind of drift that once left CP865's table unregistered. It's
 * factored into the two shapes below: decoding (unmapped output bytes ->
 * U+FFFD) and encoding (an unencodable
 * keystroke rejects the whole input chunk; see copyIn in iso2022.c).
 */
static unsigned int
apply_decode_fallback(unsigned int n GCC_UNUSED, const char *charset_name GCC_UNUSED)
{
    return UNICODE_REPLACEMENT_CHAR;
}

static unsigned int
apply_encode_fallback(unsigned int n, const char *charset_name GCC_UNUSED)
{
    if (!input_unencodable)
	input_unencodable_char = n;
    input_unencodable = 1;
    return 0;
}

unsigned int
mapping_cp932(unsigned int n, OtherStatePtr s)
{
    unsigned found_value;

    /* PATCH(fork, cp932): 0x00-0x7F decode as ASCII, 0x5C included (not
     * U+00A5 like upstream's JIS X 0201 SJIS), as Windows, the WHATWG
     * Encoding Standard and VS Code's editor (iconv-lite) do. */
    if (n < 0x80)
	return n;
    if (n >= HALFWIDTH_SJIS_LO && n <= HALFWIDTH_SJIS_HI)
	return HALFWIDTH_10646 + (n - HALFWIDTH_SJIS_LO);

    /* PATCH(fork, fallback): same policy as FontencCharsetRecode in charset.c.
     * Replace unmapped codes explicitly instead of relying on
     * luitMapCodeValue()'s identity fallback. */
    if (luitMapCodeValueFound(n, s->cp932.mapping, &found_value))
	return found_value;

    return apply_decode_fallback(n, "CP932");
}

unsigned int
reverse_cp932(unsigned int n, OtherStatePtr s)
{
    unsigned found_value;

    /* PATCH(fork, cp932): U+00A5/U+203E still encode to 0x5C/0x7E, like
     * the WHATWG Shift_JIS encoder, so typing them isn't rejected. */
    if (n == YEN_10646)
	return YEN_SJIS;
    if (n == OVERLINE_10646)
	return OVERLINE_SJIS;
    if (n < 0x80)
	return n;
    if (n >= HALFWIDTH_10646 && n <= (HALFWIDTH_10646 + (HALFWIDTH_SJIS_HI - HALFWIDTH_SJIS_LO)))
	return HALFWIDTH_SJIS_LO + (n - HALFWIDTH_10646);

    /* PATCH(fork, fallback): for the same reason as mapping_cp932(), decide
     * reliably with luitReverseFound() instead of the identity fallback.
     * When nothing is found, copyIn()'s OTHER branch (iso2022.c)
     * unconditionally continues regardless of the result (a separate
     * upstream bug), so the rejection has to be flagged from here
     * (apply_encode_fallback) -- the fallback code at the end of copyIn is
     * never reached. See docs/transcoder-design.md.
     */
    if (luitReverseFound(n, s->cp932.reverse, &found_value))
	return found_value;

    return apply_encode_fallback(n, "CP932");
}

int
stack_cp932(unsigned c, OtherStatePtr s)
{
    if (s->cp932.buf < 0) {
	if (c < 0x80 || (c >= HALFWIDTH_SJIS_LO && c <= HALFWIDTH_SJIS_HI))
	    return (int) c;
	/* SJIS lead byte ranges: 0x81-0x9F, 0xE0-0xFC */
	if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
	    s->cp932.buf = (int) c;
	    return -1;
	}
	return (int) c;
    } else {
	/* PATCH(fork, invalid sequences): as in stack_gbk() (other.c). */
	int b = (int) ((unsigned) (s->cp932.buf << 8) + c);
	s->cp932.buf = -1;
	return b;
    }
}

/*
 * GB18030 4-byte support
 *
 * upstream's mapping_gb18030()/reverse_gb18030() (other.c):
 *   - effectively don't work, since there's no builtin/fontenc/iconv data
 *     for the 2-byte part's xlfd names "gb18030.2000-0"/"gb18030.2000-1"
 *   - mapping_gb18030() has an upper bound, `if (n >= 0xFFFF) return '?';`,
 *     that unconditionally rejected the supplementary planes (U+10000 and
 *     up, whose linear values start at 189000, above 0xFFFF). Found by
 *     measurement.
 *
 * stack_gb18030()'s (other.c) own linear-index computation was correct
 * (that part of upstream is fine), so it's reused as-is. Only mapping/
 * reverse are reimplemented, on top of the generated 2-byte table
 * (gb18030-2byte-0), the 4-byte range table (gb18030_ranges.c, the BMP
 * gaps), and a single formula for the supplementary planes.
 */
#include "gb18030_ranges.h"

#define GB18030_SUPPLEMENTARY_LINEAR_START 189000u
#define GB18030_SUPPLEMENTARY_UNICODE_START 0x10000u
#define GB18030_SUPPLEMENTARY_LINEAR_END \
    (GB18030_SUPPLEMENTARY_LINEAR_START + (0x10FFFFu - GB18030_SUPPLEMENTARY_UNICODE_START))

int
init_gb18030x(OtherStatePtr s)
{
    s->gb18030.cs0_mapping = LookupMapping("gb18030-2byte-0", us16BIT);
    if (!s->gb18030.cs0_mapping)
	return 0;

    s->gb18030.cs0_reverse = LookupReverse(s->gb18030.cs0_mapping);
    if (!s->gb18030.cs0_reverse)
	return 0;

    s->gb18030.cs1_mapping = NULL;
    s->gb18030.cs1_reverse = NULL;
    s->gb18030.linear = 0;
    s->gb18030.buf_ptr = 0;
    return 1;
}

static unsigned int
gb18030_linear_to_codepoint(unsigned int linear, int *found)
{
    unsigned i;

    if (linear >= GB18030_SUPPLEMENTARY_LINEAR_START) {
	if (linear > GB18030_SUPPLEMENTARY_LINEAR_END) {
	    *found = 0;
	    return 0;
	}
	*found = 1;
	return GB18030_SUPPLEMENTARY_UNICODE_START
	    + (linear - GB18030_SUPPLEMENTARY_LINEAR_START);
    }
    for (i = 0; i < gb18030_bmp_ranges_count; ++i) {
	const Gb18030Range *r = &gb18030_bmp_ranges[i];
	unsigned len = r->unicode_end - r->unicode_start;
	if (linear >= r->linear_start && linear <= r->linear_start + len) {
	    *found = 1;
	    return r->unicode_start + (linear - r->linear_start);
	}
    }
    *found = 0;
    return 0;
}

static unsigned int
gb18030_codepoint_to_linear(unsigned int cp, int *found)
{
    unsigned i;

    if (cp >= GB18030_SUPPLEMENTARY_UNICODE_START && cp <= 0x10FFFFu) {
	*found = 1;
	return GB18030_SUPPLEMENTARY_LINEAR_START
	    + (cp - GB18030_SUPPLEMENTARY_UNICODE_START);
    }
    for (i = 0; i < gb18030_bmp_ranges_count; ++i) {
	const Gb18030Range *r = &gb18030_bmp_ranges[i];
	if (cp >= r->unicode_start && cp <= r->unicode_end) {
	    *found = 1;
	    return r->linear_start + (cp - r->unicode_start);
	}
    }
    *found = 0;
    return 0;
}

unsigned int
mapping_gb18030x(unsigned int n, OtherStatePtr s)
{
    unsigned found_value;
    int found;
    unsigned int cp;

    if (!s->gb18030.linear) {
	/* 2-byte part (including ASCII).
	 * PATCH note: upstream's mapping_gb18030 put an early "pass n<=0x80
	 * through" return at the top for both linear and non-linear input,
	 * which misbehaved when n was a linear index, catching even 0 (a
	 * legitimate value, U+0080) (found by measurement). It now applies
	 * only to the non-linear side. 0x80 itself is in the table (the euro
	 * sign, as in VS Code and the WHATWG Encoding Standard).
	 */
	if (n < 0x80)
	    return n;
	if (luitMapCodeValueFound(n, s->gb18030.cs0_mapping, &found_value))
	    return found_value;
    } else {
	/* 4-byte part: n is the linear index stack_gb18030() computed */
	cp = gb18030_linear_to_codepoint(n, &found);
	if (found)
	    return cp;
    }

    return apply_decode_fallback(n, "GB18030");
}

unsigned int
reverse_gb18030x(unsigned int n, OtherStatePtr s)
{
    unsigned found_value;
    unsigned int linear;
    int found;

    if (n < 0x80)
	return n;

    /* Try the 2-byte part first (same priority as upstream's reverse_gb18030) */
    if (luitReverseFound(n, s->gb18030.cs0_reverse, &found_value))
	return found_value;

    /* 4-byte part: compute the linear index and pack it into bytes (same
     * packing as upstream's reverse_gb18030; copyOut's c2>>24/16/8 checks
     * interpret it as a 4-byte write) */
    linear = gb18030_codepoint_to_linear(n, &found);
    if (found) {
	unsigned char bytes[4];
	unsigned int r = linear;

	bytes[3] = UChar(0x30 + r % 10);
	r /= 10;
	bytes[2] = UChar(0x81 + r % 126);
	r /= 126;
	bytes[1] = UChar(0x30 + r % 10);
	r /= 10;
	bytes[0] = UChar(0x81 + r);

	return ((unsigned int) bytes[0] << 24)
	    | ((unsigned int) bytes[1] << 16)
	    | ((unsigned int) bytes[2] << 8)
	    | (unsigned int) bytes[3];
    }

    /* PATCH(fork, fallback): copyIn's OTHER branch unconditionally
     * continues regardless of the result, so the rejection has to be
     * flagged here (same reason as reverse_cp932). */
    return apply_encode_fallback(n, "GB18030");
}

/*
 * GBK, GB 2312, CP949 (EUC-KR) and Big5-HKSCS: 2-byte charsets keyed on the
 * raw bytes, with the fallback policy applied.
 *
 * upstream's mapping_gbk()/mapping_hkscs() (other.c) call plain
 * MapCodeValue(), so for an unmapped character luitMapCodeValue()'s
 * identity fallback kicks in and silently mis-converts it into an
 * unrelated Unicode character. Like CP932/GB18030, these decide reliably
 * with luitMapCodeValueFound()/luitReverseFound() and handle unmapped codes
 * explicitly. Single high bytes (e.g. GBK's 0x80, the euro sign) are in the
 * tables, like in VS Code's iconv-lite, rather than special-cased.
 *
 * Byte assembly is upstream's (stack_gbk/stack_hkscs). GB 2312 and EUC-KR
 * are read as VS Code reads them: GB 2312 with iconv-lite's gb2312 table
 * (GBK without its user-defined areas), EUC-KR as CP949 (Unified Hangul
 * Code, whose extra 2-byte codes EUC-KR lacks). They share aux_gbk.
 */
static int
init_direct(FontMapPtr *mapping, FontMapReversePtr *reverse, const char *table)
{
    *mapping = LookupMapping(table, us16BIT);
    if (!*mapping)
	return 0;
    *reverse = LookupReverse(*mapping);
    return *reverse != NULL;
}

static unsigned int
mapping_direct(unsigned int n, FontMapPtr mapping, const char *name)
{
    unsigned found_value;

    if (n < 128)
	return n;
    if (luitMapCodeValueFound(n, mapping, &found_value))
	return found_value;
    return apply_decode_fallback(n, name);
}

static unsigned int
reverse_direct(unsigned int n, FontMapReversePtr reverse, const char *name)
{
    unsigned found_value;

    if (n < 128)
	return n;
    if (luitReverseFound(n, reverse, &found_value))
	return found_value;
    /* copyIn()'s OTHER branch (iso2022.c) unconditionally continues
     * regardless of the result, so the rejection has to be flagged here
     * (same reason as reverse_cp932). */
    return apply_encode_fallback(n, name);
}

int
init_gbkx(OtherStatePtr s)
{
    s->gbk.buf = -1;
    return init_direct(&s->gbk.mapping, &s->gbk.reverse, "gbk-0");
}

unsigned int
mapping_gbkx(unsigned int n, OtherStatePtr s)
{
    return mapping_direct(n, s->gbk.mapping, "GBK");
}

unsigned int
reverse_gbkx(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->gbk.reverse, "GBK");
}

int
init_gb2312x(OtherStatePtr s)
{
    s->gbk.buf = -1;
    return init_direct(&s->gbk.mapping, &s->gbk.reverse, "gb2312-0");
}

unsigned int
mapping_gb2312x(unsigned int n, OtherStatePtr s)
{
    return mapping_direct(n, s->gbk.mapping, "GB2312");
}

unsigned int
reverse_gb2312x(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->gbk.reverse, "GB2312");
}

int
init_cp949(OtherStatePtr s)
{
    s->gbk.buf = -1;
    return init_direct(&s->gbk.mapping, &s->gbk.reverse, "cp949-0");
}

unsigned int
mapping_cp949(unsigned int n, OtherStatePtr s)
{
    return mapping_direct(n, s->gbk.mapping, "CP949");
}

unsigned int
reverse_cp949(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->gbk.reverse, "CP949");
}

int
init_hkscsx(OtherStatePtr s)
{
    s->hkscs.buf = -1;
    return init_direct(&s->hkscs.mapping, &s->hkscs.reverse, "big5hkscs-0");
}

unsigned int
mapping_hkscsx(unsigned int n, OtherStatePtr s)
{
    return mapping_direct(n, s->hkscs.mapping, "Big5-HKSCS");
}

unsigned int
reverse_hkscsx(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->hkscs.reverse, "Big5-HKSCS");
}

/*
 * Big5 as VS Code reads it (CP950), keyed on the raw bytes like Big5-HKSCS.
 * It was a T_94192 charset in GR, where luit took the C1 bytes 0x8E, 0x8F
 * and 0x9B, which are Big5 lead bytes, for SS2, SS3 and CSI.
 */
int
init_big5x(OtherStatePtr s)
{
    s->hkscs.buf = -1;
    return init_direct(&s->hkscs.mapping, &s->hkscs.reverse, "big5.eten-0");
}

unsigned int
mapping_big5x(unsigned int n, OtherStatePtr s)
{
    return mapping_direct(n, s->hkscs.mapping, "Big5");
}

unsigned int
reverse_big5x(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->hkscs.reverse, "Big5");
}

/*
 * EUC-JP as VS Code reads it. Codes are the raw bytes: 0xA1A1-0xFEFE (JIS
 * X 0208, the table keyed on GL), 0x8EA1-0x8EDF (JIS X 0201 katakana) and
 * 0x8FA1A1-0x8FFEFE (JIS X 0212, keyed on GL). It was an ISO 2022 setup
 * (G1 in GR, G2/G3 by single shifts), whose decoder dropped or passed
 * through invalid bytes and took 0x9B for CSI; VS Code's editor shows them
 * as U+FFFD. Which table a character is sent from is decided when the
 * tables are generated (only one row encodes a character).
 */
#define EUC_GL(n) ((n) & 0x7F7F)

int
init_eucjpx(OtherStatePtr s)
{
    s->eucjp.buf_ptr = 0;
    return (init_direct(&s->eucjp.x0208mapping, &s->eucjp.x0208reverse, "jisx0208-2007-0")
	    && init_direct(&s->eucjp.x0201mapping, &s->eucjp.x0201reverse, "jisx0201.1976-0")
	    && init_direct(&s->eucjp.x0212mapping, &s->eucjp.x0212reverse, "jisx0212.1990-0"));
}

static int
euc_byte(unsigned n)
{
    return n >= 0xA1 && n <= 0xFE;
}

unsigned int
mapping_eucjpx(unsigned int n, OtherStatePtr s)
{
    unsigned found_value;
    unsigned hi = (n >> 8) & 0xFF, lo = n & 0xFF;

    if (n < 0x80)
	return n;
    if (n >> 16) {
	if ((n >> 16) == 0x8F && euc_byte(hi) && euc_byte(lo)
	    && luitMapCodeValueFound(EUC_GL(n), s->eucjp.x0212mapping, &found_value))
	    return found_value;
    } else if (hi == 0x8E) {
	if (luitMapCodeValueFound(lo, s->eucjp.x0201mapping, &found_value))
	    return found_value;
    } else if (euc_byte(hi) && euc_byte(lo)) {
	if (luitMapCodeValueFound(EUC_GL(n), s->eucjp.x0208mapping, &found_value))
	    return found_value;
    }
    return apply_decode_fallback(n, "EUC-JP");
}

unsigned int
reverse_eucjpx(unsigned int n, OtherStatePtr s)
{
    unsigned found_value;

    if (n < 0x80)
	return n;
    if (luitReverseFound(n, s->eucjp.x0208reverse, &found_value))
	return found_value | 0x8080;
    if (luitReverseFound(n, s->eucjp.x0201reverse, &found_value))
	return 0x8E00 | found_value;
    if (luitReverseFound(n, s->eucjp.x0212reverse, &found_value))
	return 0x8F8080 | found_value;
    /* same reason as reverse_cp932 */
    return apply_encode_fallback(n, "EUC-JP");
}

int
stack_eucjp(unsigned c, OtherStatePtr s)
{
    aux_eucjp *e = &s->eucjp;

    if (e->buf_ptr == 0) {
	if (c < 0x80)
	    return (int) c;
	e->buf[e->buf_ptr++] = (int) c;
	return -1;
    }
    if (e->buf_ptr == 1 && e->buf[0] == 0x8F) {
	if (!euc_byte(c)) {
	    e->buf_ptr = 0;
	    return OTHER_INVALID;
	}
	e->buf[e->buf_ptr++] = (int) c;
	return -1;
    }
    /* any other byte makes a code, unmapped if invalid (see stack_gbk) */
    {
	unsigned code = (unsigned) e->buf[0];
	if (e->buf_ptr == 2)
	    code = (code << 8) | (unsigned) e->buf[1];
	e->buf_ptr = 0;
	return (int) ((code << 8) | c);
    }
}
