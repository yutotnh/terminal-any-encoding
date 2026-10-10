/*
 * other_ja.c -- the fork's "other" charsets (fork-local, not upstream)
 *
 * CP932, GB18030, GBK, GB 2312, CP949 (EUC-KR), Big5, Big5-HKSCS and
 * EUC-JP, each decoded and encoded with the fork's generated tables
 * (builtin_ja.c, from iconv-lite like VS Code's editor) the way VS Code
 * reads and writes them. Codes are the raw bytes, so no coordinate
 * transform is needed. See docs/transcoder-design.md.
 *
 * Every mapping_* function returns U+FFFD for a code without a character,
 * and every reverse_* function returns 0 for a character without a code
 * (copyIn() in iso2022.c then rejects the input). Upstream's lookups fall
 * back to the code itself instead (luitMapCodeValue()/luitReverse()), so
 * these use luitMapCodeValueFound()/luitReverseFound() (luitconv.c).
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <other.h>
#include <sys.h>
#include <luitconv.h>

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
mapping_direct(unsigned int n, FontMapPtr mapping)
{
    unsigned found_value;

    if (n < 0x80)
	return n;
    if (luitMapCodeValueFound(n, mapping, &found_value))
	return found_value;
    return UNICODE_REPLACEMENT_CHAR;
}

static unsigned int
reverse_direct(unsigned int n, FontMapReversePtr reverse)
{
    unsigned found_value;

    if (n < 0x80)
	return n;
    if (luitReverseFound(n, reverse, &found_value))
	return found_value;
    return 0;
}

/*
 * CP932 (Windows-31J). Not upstream's SJIS charset: mapping_sjis() converts
 * Shift JIS to JIS X 0208 rows by formula, which only covers the 94 rows
 * of JIS X 0208 and gives rows outside it for the NEC-selected IBM
 * extensions (lead bytes 0xED-0xEE) and the IBM extensions (0xFA-0xFC).
 */
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
    s->cp932.buf = -1;
    return init_direct(&s->cp932.mapping, &s->cp932.reverse, "cp932-direct-0");
}

unsigned int
mapping_cp932(unsigned int n, OtherStatePtr s)
{
    /* 0x00-0x7F decode as ASCII, 0x5C included (not U+00A5 like upstream's
     * JIS X 0201 SJIS), as Windows, the WHATWG Encoding Standard and VS
     * Code's editor (iconv-lite) do. */
    if (n >= HALFWIDTH_SJIS_LO && n <= HALFWIDTH_SJIS_HI)
	return HALFWIDTH_10646 + (n - HALFWIDTH_SJIS_LO);
    return mapping_direct(n, s->cp932.mapping);
}

unsigned int
reverse_cp932(unsigned int n, OtherStatePtr s)
{
    /* U+00A5/U+203E still encode to 0x5C/0x7E, like the WHATWG Shift_JIS
     * encoder, so typing them isn't rejected. */
    if (n == YEN_10646)
	return YEN_SJIS;
    if (n == OVERLINE_10646)
	return OVERLINE_SJIS;
    if (n >= HALFWIDTH_10646 && n <= (HALFWIDTH_10646 + (HALFWIDTH_SJIS_HI - HALFWIDTH_SJIS_LO)))
	return HALFWIDTH_SJIS_LO + (n - HALFWIDTH_10646);
    return reverse_direct(n, s->cp932.reverse);
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
	/* any second byte makes a code, unmapped if invalid (see stack_gbk()
	 * in other.c) */
	int b = (int) ((unsigned) (s->cp932.buf << 8) + c);
	s->cp932.buf = -1;
	return b;
    }
}

/*
 * GB18030, with its 4-byte codes.
 *
 * Upstream's mapping_gb18030()/reverse_gb18030() (other.c) don't work:
 *   - nothing provides data for the tables they look up,
 *     "gb18030.2000-0" and "gb18030.2000-1"
 *   - mapping_gb18030() returns '?' for every linear index from 0xFFFF
 *     up, which covers the supplementary planes (U+10000 and up start at
 *     189000).
 *
 * stack_gb18030() (other.c) computes the linear index correctly and is
 * reused. mapping/reverse are built on the generated 2-byte table
 * (gb18030-2byte-0), the 4-byte range table for the BMP
 * (gb18030_ranges.c), and a formula for the supplementary planes.
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
    int found;
    unsigned int cp;

    /* ASCII and 2-byte codes. Only these pass 0x00-0x7F through: a linear
     * index that small is a 4-byte code (0 is U+0080), which upstream's
     * mapping_gb18030() passed through too. 0x80 is in the table (the euro
     * sign, as in VS Code and the WHATWG Encoding Standard). */
    if (!s->gb18030.linear)
	return mapping_direct(n, s->gb18030.cs0_mapping);

    /* 4-byte codes: n is the linear index stack_gb18030() computed */
    cp = gb18030_linear_to_codepoint(n, &found);
    return found ? cp : UNICODE_REPLACEMENT_CHAR;
}

unsigned int
reverse_gb18030x(unsigned int n, OtherStatePtr s)
{
    unsigned int code;
    unsigned int linear;
    int found;

    /* ASCII and 2-byte codes first, as upstream's reverse_gb18030() */
    code = reverse_direct(n, s->gb18030.cs0_reverse);
    if (code != 0)
	return code;

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

    return 0;
}

/*
 * GBK, GB 2312, CP949 (EUC-KR) and Big5-HKSCS: upstream's byte assembly
 * (stack_gbk/stack_hkscs in other.c) with the fork's tables. Single high
 * bytes (e.g. GBK's 0x80, the euro sign) are in the tables, as in
 * iconv-lite, rather than special-cased like upstream's reverse_gbk().
 *
 * GB 2312 and EUC-KR are read as VS Code reads them: GB 2312 with
 * iconv-lite's gb2312 table (GBK without its user-defined areas), EUC-KR
 * as CP949 (Unified Hangul Code, whose extra 2-byte codes EUC-KR lacks).
 * They share aux_gbk.
 */

int
init_gbkx(OtherStatePtr s)
{
    s->gbk.buf = -1;
    return init_direct(&s->gbk.mapping, &s->gbk.reverse, "gbk-0");
}

unsigned int
mapping_gbkx(unsigned int n, OtherStatePtr s)
{
    return mapping_direct(n, s->gbk.mapping);
}

unsigned int
reverse_gbkx(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->gbk.reverse);
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
    return mapping_direct(n, s->gbk.mapping);
}

unsigned int
reverse_gb2312x(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->gbk.reverse);
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
    return mapping_direct(n, s->gbk.mapping);
}

unsigned int
reverse_cp949(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->gbk.reverse);
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
    return mapping_direct(n, s->hkscs.mapping);
}

unsigned int
reverse_hkscsx(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->hkscs.reverse);
}

/*
 * Big5 as VS Code reads it (CP950), keyed on the raw bytes like Big5-HKSCS.
 * Not upstream's T_94192 charset in GR: there luit takes the C1 bytes 0x8E,
 * 0x8F and 0x9B, which are Big5 lead bytes, for SS2, SS3 and CSI.
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
    return mapping_direct(n, s->hkscs.mapping);
}

unsigned int
reverse_big5x(unsigned int n, OtherStatePtr s)
{
    return reverse_direct(n, s->hkscs.reverse);
}

/*
 * EUC-JP as VS Code reads it. Codes are the raw bytes: 0xA1A1-0xFEFE (JIS
 * X 0208, the table keyed on GL), 0x8EA1-0x8EDF (JIS X 0201 katakana) and
 * 0x8FA1A1-0x8FFEFE (JIS X 0212, keyed on GL). Not upstream's ISO 2022
 * setup (G1 in GR, G2/G3 by single shifts), whose decoder drops or passes
 * through invalid bytes and takes 0x9B for CSI; VS Code's editor shows
 * them as U+FFFD. Which table a character is sent from is decided when the
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
    return UNICODE_REPLACEMENT_CHAR;
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
    return 0;
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
