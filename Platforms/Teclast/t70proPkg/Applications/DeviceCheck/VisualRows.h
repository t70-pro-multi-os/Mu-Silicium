// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef T70_VISUAL_ROWS_H
#define T70_VISUAL_ROWS_H
#include <stddef.h>
#include <stdint.h>
/* Narrow debug text, one printable UTF-16 code unit per console cell.
 * Not Unicode grapheme/East Asian width support. CRLF/CR/LF terminate rows;
 * TAB expands to four-column stops; other C0 controls become question marks.
 * Caller preserves a cursor per visual page; no source mutation/allocation.
 * Columns 2..512, output capacity at least Columns; last column always unused.
 */
typedef struct { size_t Offset; unsigned Spaces; } T70_VISUAL_CURSOR;
/* 1=row, 0=end, -1=invalid (cursor unchanged). Out NUL terminated on success.
 * Empty explicit lines produce an empty row. Trailing newline adds no row.
 */
int T70VisualRow(const uint16_t *Text, size_t Length, unsigned Columns,
  T70_VISUAL_CURSOR *Cursor, uint16_t *Out, size_t Capacity, size_t *Written);
#endif
