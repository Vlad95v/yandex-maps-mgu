# Cluster Horizontal Offset — Implementation Plan

**Date:** 2026-11-24  
**Branch:** `feature/cluster-source-crop-offset`

## Architecture: Source-Crop Offset (Wider CarPlay + Independent Cluster Crop)

### Core Idea
- CarPlay patch advertises **1440×636 @ 183×81mm** geometry (wider than 1088×480)
- Phone renders map at 1440×636
- Decoder outputs 1440×636 atlas (VPP 1:1, no scaling)
- **Cluster feed** crops 1088×480 from it at `crop_x = 176 - cluster_offset_x`
- **HUD feed** gets independent centered 1088×480 crop (no cluster crop leakage)
- Live offset changes by updating cluster's `crop_x` without decoder/renderer restart

### Key Constants
| Constant | Value | Meaning |
|----------|-------|---------|
| `SOURCE_W` | 1440 | CarPlay advertised width |
| `SOURCE_H` | 636 | CarPlay advertised height |
| `SOURCE_MMW` | 183 | Physical width (mm) |
| `SOURCE_MMH` | 81 | Physical height (mm) |
| `CROP_W` | 1088 | Cluster/HUD viewport width |
| `CROP_H` | 480 | Cluster/HUD viewport height |
| `MAX_OFFSET` | ±176 | Max horizontal offset (176 = (1440-1088)/2) |
| `OFFSET_STEP` | 2 | Step size for UI slider |

### Formula
```
cluster_crop_x = 176 - cluster_offset_x
```
- `cluster_offset_x = 0` → crop_x = 176 (centered)
- `cluster_offset_x = +176` → crop_x = 0 (left edge)
- `cluster_offset_x = -176` → crop_x = 352 (right edge)

HUD always uses `crop_x = 176` (centered), independent of cluster offset.

---

## Files to Modify

### 1. `native/waze-on-cluster/patch/carplay_patch.js`
- Add `RES_WIDE` constant: 1440×636 @ 183×81mm
- Update `resFor()` to accept "wide" as a preset
- Keep `RES_DEFAULT` (1088×480 @ 138×61mm) for backward compatibility

### 2. `native/waze-on-cluster/waze-on-cluster.conf`
- Add `tex_size = 1440x636` as new default (or keep 1088x480 as default, document both)
- Update `src_crop` if needed

### 3. `native/waze-on-cluster/src/map_feed.h`
- Add `actual_cluster_offset_x()` returning `placed_x_`
- Add `cluster_crop_x()` returning current crop X
- Update `FeedStatus` with `cluster_x_requested`, `cluster_x_actual`, `cluster_crop_x`, `source_w`, `source_h`, `output_w`, `output_h`

### 4. `native/waze-on-cluster/src/map_feed.cpp`
- Modify `quad()` to use crop-based approach for cluster feed
- When `cfg_.target == "map"` and `scaling_ == false` (cluster):
  - Configure AtlasScaler with crop_x = 176 - cluster_offset_x, crop_w = 1088, crop_h = 480
  - Source is 1440×636, output is 1088×480 (1:1, no scaling)
- HUD feed continues to use centered crop (crop_x = 176)
- Update `status()` to expose both requested and actual values

### 5. `native/waze-on-cluster/src/map_feeds.cpp`
- Update `set_cluster_offset()` to reconfigure cluster feed's AtlasScaler crop_x
- Add `feed_hud()` to use independent centered crop

### 6. `native/waze-on-cluster/patch/patchd.js`
- Add `"cluster_offset_x"` to `MAP_KEYS` (line 257)
- Add command handler for `{"cmd":"cluster","cluster_x":N}` (after line 2359)
- Update protocol docs (line 52)
- Validate range: -176 to +176, step 2

### 7. `apps/robertn_waze_cluster/src/waze_cluster_control.js`
- Add `clusterOffset()` getter (like `hudOffset()`)
- Add `clusterOffsetText()` (like `hudPositionText()`)
- Add `moveCluster(dir)`, `centreCluster()` (like `moveHud`, `centreHud`)
- Add `canChangeClusterOffset()`
- Add UI buttons/slider for -176 to +176, step 2

### 8. `native/waze-on-cluster/src/main.cpp`
- Update default `cluster_offset_x` range validation (-176..176, step 2)

### 9. Tests
- Unit tests for offset scenarios
- Integration test for persistence

---

## Implementation Order

1. **carplay_patch.js** - Add 1440×636 geometry
2. **map_feed.h/cpp** - Implement cluster crop logic, status reporting
3. **map_feeds.cpp** - Live offset updates without restart
4. **patchd.js** - Persistence and command handling
5. **waze_cluster_control.js** - App UI
6. **main.cpp** - Validation
7. **Tests** - Verify all scenarios
8. **Build verification**

---

## Verification Checklist

- [ ] CarPlay patch advertises 1440×636 @ 183×81mm
- [ ] Decoder outputs 1440×636 atlas for cluster
- [ ] Cluster feed crops 1088×480 at `crop_x = 176 - offset`
- [ ] HUD feed crops centered 1088×480 independently
- [ ] Live offset change updates crop_x without decoder/renderer restart
- [ ] Status reports: requested offset, actual offset, crop_x, source/output dims
- [ ] `cluster_offset_x` persisted in `map.state.conf` via patchd
- [ ] App UI slider -176 to +176 step 2 works
- [ ] All targets compile (cluster-render, waze-on-cluster, robertn_waze_cluster)
- [ ] Automated tests pass for: offset 0, +176, -176, boundary clamping, HUD independence