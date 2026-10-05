/*
 * gb18030_ranges.h -- GB18030 4-byte range table declaration (fork-local)
 */
#ifndef GB18030_RANGES_H
#define GB18030_RANGES_H

typedef struct {
    unsigned unicode_start;
    unsigned unicode_end;
    unsigned linear_start;
} Gb18030Range;

extern const Gb18030Range gb18030_bmp_ranges[];
extern const unsigned gb18030_bmp_ranges_count;

#endif
