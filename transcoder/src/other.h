/* $XTermId: other.h,v 1.13 2025/09/12 07:52:28 tom Exp $ */

/*
Copyright 2006-2010,2025 by Thomas E. Dickey
Copyright (c) 2002 by Tomohiro KUBOTA

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

#ifndef LUIT_OTHER_H
#define LUIT_OTHER_H 1

#include <luit.h>		/* include this, for self-contained headers */

#include <luitconv.h>
#include <trace.h>

#undef UCHAR
#define UCHAR unsigned char

#undef UINT
#define UINT unsigned int

#define UChar(n) ((UCHAR)(n))

typedef struct {
    FontMapPtr mapping;
    FontMapReversePtr reverse;
    int buf;
} aux_gbk;

typedef struct {
    UCHAR buf[4];
    int buf_ptr, len;
} aux_utf8;

typedef struct {
    FontMapPtr x0208mapping;
    FontMapPtr x0201mapping;
    FontMapReversePtr x0208reverse;
    FontMapReversePtr x0201reverse;
    int buf;
} aux_sjis;

typedef struct {
    FontMapPtr mapping;
    FontMapReversePtr reverse;
    int buf;
} aux_hkscs;

typedef struct {
    FontMapPtr cs0_mapping;	/* gb18030.2000-0 */
    FontMapReversePtr cs0_reverse;

    FontMapPtr cs1_mapping;	/* gb18030.2000-1 */
    FontMapReversePtr cs1_reverse;

    int linear;			/* set to '1' if stack_gb18030 linearized a 4bytes seq */
    int buf[3];
    int buf_ptr;
} aux_gb18030;

/* PATCH(fork, cp932): CP932 (Windows-31J), looked up by its 2-byte codes
 * in one table rather than converted to JIS X 0208 like aux_sjis (see
 * other_fork.c) */
typedef struct {
    FontMapPtr mapping;
    FontMapReversePtr reverse;
    int buf;
} aux_cp932;

/* PATCH(fork, eucjp): EUC-JP from the three tables VS Code reads it with
 * (JIS X 0208 with its extensions, JIS X 0201 katakana after SS2, JIS X
 * 0212 after SS3), as an "other" charset; see other_fork.c. */
typedef struct {
    FontMapPtr x0208mapping;
    FontMapPtr x0201mapping;
    FontMapPtr x0212mapping;
    FontMapReversePtr x0208reverse;
    FontMapReversePtr x0201reverse;
    FontMapReversePtr x0212reverse;
    int buf[2];
    int buf_ptr;
} aux_eucjp;

typedef union {
    aux_gbk gbk;
    aux_utf8 utf8;
    aux_sjis sjis;
    aux_hkscs hkscs;
    aux_gb18030 gb18030;
    aux_cp932 cp932;
    aux_eucjp eucjp;
} OtherState, *OtherStatePtr;

/* PATCH(fork, invalid sequences): what a stack function returns when the
 * bytes so far can't be a character. copyOut() (iso2022.c) then shows
 * U+FFFD for the first byte and reads the rest again, as VS Code's editor
 * does. -1 still means "more bytes needed". */
#define OTHER_INVALID (-2)

/* PATCH(fork, invalid sequences): what a code without a character decodes
 * to, as in VS Code's editor */
#define UNICODE_REPLACEMENT_CHAR 0xFFFD

int init_gbk(OtherStatePtr);
UINT mapping_gbk(UINT, OtherStatePtr);
UINT reverse_gbk(UINT, OtherStatePtr);
int stack_gbk(UINT, OtherStatePtr);

int init_utf8(OtherStatePtr);
UINT mapping_utf8(UINT, OtherStatePtr);
UINT reverse_utf8(UINT, OtherStatePtr);
int stack_utf8(UINT, OtherStatePtr);

int init_sjis(OtherStatePtr);
UINT mapping_sjis(UINT, OtherStatePtr);
UINT reverse_sjis(UINT, OtherStatePtr);
int stack_sjis(UINT, OtherStatePtr);

int init_hkscs(OtherStatePtr);
UINT mapping_hkscs(UINT, OtherStatePtr);
UINT reverse_hkscs(UINT, OtherStatePtr);
int stack_hkscs(UINT, OtherStatePtr);

/* PATCH(fork, cp932): see other_fork.c */
int init_cp932(OtherStatePtr);
UINT mapping_cp932(UINT, OtherStatePtr);
UINT reverse_cp932(UINT, OtherStatePtr);
int stack_cp932(UINT, OtherStatePtr);

/* PATCH(fork, gb18030): GB18030 with its 4-byte codes, on aux_gb18030 and
 * upstream's stack_gb18030() (see other_fork.c) */
int init_gb18030x(OtherStatePtr);
UINT mapping_gb18030x(UINT, OtherStatePtr);
UINT reverse_gb18030x(UINT, OtherStatePtr);

/* PATCH(fork, fallback): GBK, GB 2312, CP949 (EUC-KR) and Big5-HKSCS from
 * the fork's tables, on aux_gbk/aux_hkscs and upstream's
 * stack_gbk/stack_hkscs (see other_fork.c) */
int init_gbkx(OtherStatePtr);
UINT mapping_gbkx(UINT, OtherStatePtr);
UINT reverse_gbkx(UINT, OtherStatePtr);
int init_gb2312x(OtherStatePtr);
UINT mapping_gb2312x(UINT, OtherStatePtr);
UINT reverse_gb2312x(UINT, OtherStatePtr);
int init_cp949(OtherStatePtr);
UINT mapping_cp949(UINT, OtherStatePtr);
UINT reverse_cp949(UINT, OtherStatePtr);

int init_hkscsx(OtherStatePtr);
UINT mapping_hkscsx(UINT, OtherStatePtr);
UINT reverse_hkscsx(UINT, OtherStatePtr);
/* PATCH(fork, big5): Big5 (CP950) the same way, with stack_hkscs */
int init_big5x(OtherStatePtr);
UINT mapping_big5x(UINT, OtherStatePtr);
UINT reverse_big5x(UINT, OtherStatePtr);

/* PATCH(fork, eucjp): see aux_eucjp */
int init_eucjpx(OtherStatePtr);
UINT mapping_eucjpx(UINT, OtherStatePtr);
UINT reverse_eucjpx(UINT, OtherStatePtr);
int stack_eucjp(UINT, OtherStatePtr);

int init_gb18030(OtherStatePtr);
UINT mapping_gb18030(UINT, OtherStatePtr);
UINT reverse_gb18030(UINT, OtherStatePtr);
int stack_gb18030(UINT, OtherStatePtr);

#endif /* LUIT_OTHER_H */
