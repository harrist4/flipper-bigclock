## 2026-02-17
- Adjusted spacing of minute progress bars
- Adjusted height of digits
- Adjusted AM/PM positions
- Added 24H mode, with OK button toggle and saved state

## 2026-03-01
- Added pinched 7-segment rendering option (feature available in code, style-selectable)
- Added lozenge 7-segment style with tapered ends and centerline-aligned segment geometry
- Added long-OK segment style cycling (Classic -> Pinched -> Lozenge)
- Persisted segment style across launches (`segment_style.bin`)
- Updated brightness HUD: removed +/- glyphs and expanded meter to full right-side height
- Improved backlight exit behavior and state restore flow
- Dimming/brightness control is still in-progress and needs another focused pass later

## 2026-03-02
- Increased digit width to 24px and set inter-digit spacing to 4px for fuller edge-to-edge layout
- Restored 6px colon width while keeping 2px side spacing around the colon
- Kept AM/PM column at 10px and refined horizontal placement to match the right-side visual edge
- Updated progress bar geometry to 11px width with corrected column alignment
- Removed top/bottom lozenge inset so digits use full vertical height
- Corrected middle horizontal segment placement to true vertical centering

## 2026-03-08
- Removed pinched style from active style cycle; style order is now `Classic -> Lozenge -> Font`
- Added bitmap font digit renderer as a third style (`SegmentStyleFont`)
- Added editable `font_digits` table for manual shaping of digits `0..9` using `.`/`X` row maps
- Kept strict digit-index mapping (`font_digits[d]` maps directly to digit `d`)
- Added compile-time table-shape guards for font bitmap dimensions (`10 x 64 x 24`)
- Added debug-only font validation and malformed-font marker rendering path
- Confirmed deploy flow via `ufbt launch` with updated digit set
- Updated project docs (`README.md`) to reflect current controls, style modes, and font workflow
- Expanded in-file comments/documentation in `bigclock.c` without changing behavior
