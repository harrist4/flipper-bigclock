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
