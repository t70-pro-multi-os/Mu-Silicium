# Visual row iterator (not yet wired into DeviceCheck)

The pure helper retains every printable UTF-16 code unit across rows and emits
at most Columns-1 cells, avoiding final-column implicit autowrap. Its scope is
narrow firmware debug text, not grapheme or wide-character cell measurement.
CRLF/CR/LF end rows; tabs expand to four-cell stops in the current visual row;
other C0 controls render as ?. Input text and logical log offsets stay unchanged.
Keep cursor snapshots for page starts; restart iteration when log/width changes.

From the firmware repository root, under the normal shared build lock:

```
cc -fsanitize=address,undefined -g -Wall -Wextra -Werror \
  Platforms/Teclast/t70proPkg/Applications/DeviceCheck/VisualRows.c \
  Platforms/Teclast/t70proPkg/Tests/DeviceCheck/test-visual-rows.c \
  -o /tmp/t70-visual-rows
/tmp/t70-visual-rows
```

## Pending production integration

Existing dirty T70DeviceCheck.c/INF are intentionally untouched. Coordinate
ownership before applying an anchor-guarded patch; working-tree merge safety
is separate from whether a coherent isolated staged patch can be committed.

1. Add source/include to INF/C without changing other dependencies.
2. Replace Page logical-line slicing with visual iteration over the entire RAM
   log. Budget physical rows and preserve a continuation cursor; do not mutate
   the log or truncate long tails. Print each row with explicit CRLF.
3. Feed checked QueryMode Columns and reserve enough physical rows for footer.
   Render footer via the same width rule; do not print a last-row newline that
   scrolls the screen. Very small geometry needs a minimal footer or rejection.
4. Change all First/last-page arithmetic (including export/P/RAM results) and
   Up/Down to visual-page offsets. Recompute after width/log changes.
5. Check export selection/confirmation direct Print calls too; they bypass Page
   and must fit narrow modes. Preserve confirmation and RO behavior.
6. Run actual Page host regression and 80-column AAVMF key/navigation test;
   helper-only success is not a product fix.

Known overlap: dirty Page footer, Rows-6 adjustment, and export/P branches that
set First overlap the required wiring. The existing Page body loop is unchanged
from HEAD, but replacing only it without coordinating First is not sufficient.
