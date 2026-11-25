# Cluster Horizontal Offset Feature — Investigation Report

**Date**: 2026-11-24  
**Branch**: `feature/cluster-horizontal-offset`  
**Status**: Investigation complete — **STOP, awaiting approval before any code changes**

---

## Executive Summary

Three issues were investigated. Two **critical bugs** found that prevent the cluster horizontal offset feature from working end-to-end:

1. **Cluster offset reporting is broken** — Status API reports *requested* offset (`cfg_.cluster_offset_x`) but actual applied offset after clamping is `placed_x_`. These differ at scene boundaries.
2. **`cluster_offset_x` is NOT persisted** — Missing from `patchd.js` MAP_KEYS (write path) and `patchd.js` command handler. The render reads it at startup via `load_map_state()` but patchd never writes it.
3. **No build environment** — No g++, clang++, make, or ninja available to verify compilation.

---

## Issue 1: Cluster Offset Reporting Path

### Trace: FeedConfig → MapFeed::quad() → MapFeed::set_cluster_offset() → MapFeed::cluster_offset_x() → MapFeeds::status() → FeedStatus → status_line() → app-side display

| Component | File:Line | What it does |
|-----------|-----------|--------------|
| **FeedConfig** | `map_feed.h:96` | `cluster_offset_x` member (int, default 0) |
| **MapFeed::quad()** | `map_feed.cpp:265-276` | Computes quad position using `map_geometry.h::map_quad()`, applies `cluster_offset_x`, **clamps to scene bounds**, stores actual displacement in `placed_x_` |
| **MapFeed::set_cluster_offset()** | `map_feed.cpp:322-335` | Updates `cfg_.cluster_offset_x`, recomputes quad via `rebuild()`, sends to renderer |
| **MapFeed::cluster_offset_x()** | `map_feed.h:189` | **Returns `cfg_.cluster_offset_x` (requested value), NOT `placed_x_` (actual)** |
| **MapFeeds::status()** | `map_feeds.cpp:222` | Returns `cluster_->cluster_offset_x()` → requested value |
| **FeedStatus** | `map_feed.h:140` | `cluster_offset_x` field populated from above |
| **status_line()** | `main.cpp:86` | JSON output: `"cluster_x": s.cluster_offset_x` |
| **App display** | `waze_cluster_control.js` | **No UI for cluster offset** — only HUD position buttons exist |

### **BUG: Requested vs Actual Offset Mismatch**
```cpp
// map_feed.cpp:271-276 — actual applied offset stored in placed_x_
placed_x_ = std::max(0, std::min(placed_x, scene_w - quad.w));

// map_feed.h:189 — returns REQUESTED offset
int cluster_offset_x() const { return cfg_.cluster_offset_x; }
```
**At scene boundaries** (offset > 0 or < -scene_w), `placed_x_` ≠ `cfg_.cluster_offset_x`. The status API lies about the true map position.

---

## Issue 2: Map State File Persistence

### Configuration & Read Path
| Aspect | Location | Details |
|--------|----------|---------|
| **Config key** | `waze-on-cluster.conf:159` | `map_state_file = /var/opt/waze-on-cluster/map.state.conf` |
| **Read function** | `map_state.cpp:48` | `load_map_state(cfg, path)` called from `main.cpp:197` |
| **Valid keys** | `map_state.h:20`, `map_state.cpp:16-18` | `kMapStateKeys[7]` = `cluster`, `hud`, `hud_width_pct`, `hud_offset_x`, `hud_offset_y`, `hud_keep_shown`, **`cluster_offset_x`** |
| **Validation** | `map_state.cpp:41-42` | `cluster_offset_x` range: **-1088..1088** |
| **Default** | `main.cpp:519` | `cfg->define("cluster_offset_x", "0")` |
| **Control command** | `main.cpp:143-155` | `{"cmd":"cluster","cluster_x":...}` validation -1088..1088 |

### Write Path (patchd.js) — **BROKEN**
| Aspect | Location | Status |
|--------|----------|--------|
| **MAP_KEYS** | `patchd.js:257` | `['cluster', 'hud', 'hud_width_pct', 'hud_offset_x', 'hud_offset_y', 'hud_keep_shown']` — **MISSING `cluster_offset_x`** |
| **Command handler** | `patchd.js:2354-2359` | Only handles `{"cmd":"cluster","on":0|1}` — **NO `cluster_x` handling** |
| **Protocol docs** | `patchd.js:52` | Documents only `{"cmd":"cluster","on":0|1}` |
| **Save mechanism** | `patchd.js:362-397` | `saveMapSettings()` → `flushMapSettings()` → `rewriteConf(MAP_STATE, ...)` debounced 500ms |

### Persistence Flow
1. App sends `{"cmd":"cluster","cluster_x":N}` to render control socket
2. Render applies immediately via `MapFeeds::set_cluster_offset()` → `MapFeed::set_cluster_offset()`
3. **App ALSO sends to patchd** to persist — **but patchd ignores `cluster_x`!**
4. On restart: `load_map_state()` reads `map.state.conf` → `cluster_offset_x` **never written** → falls back to config default (0)

---

## Issue 3: Build Environment Verification

```bash
$ which g++ clang++ make ninja
# Exit code 1 — none found
```
**No C++ build toolchain available** in this environment. Cannot verify compilation of current branch.

---

## Additional Findings

### App-Side UI Missing
- `waze_cluster_control.js` has full HUD position UI (move left/right/up/down, center, position display)
- **Zero cluster offset controls** — no buttons, no display, no `canChangeClusterOffset()`, no `moveCluster()`
- `robertn_waze_cluster.js` (main app) renders HUD position but no cluster position

### Vehicle Arrow Independence
- `CarModelViewMetadata` (cluster-render ABI) = KOMBI's separate vehicle arrow overlay
- **Map CAN shift independently** of vehicle arrow — confirmed not baked into video stream

### Source Picture Constraints
- Cluster scene: **1088×480** (slot 0x100)
- Android Auto `src_crop`: `1280x720:0,0,1280,564` — map in top 564px of 720px
- CarPlay: already at `tex_size` (1088×480)
- **Shifting creates empty edges** unless source has extra pixels beyond 1088×480

### Fixed During Investigation
- **Compilation error**: `map_state.h:20` declared `kMapStateKeys[6]` but `map_state.cpp:16` defined 7 elements — **fixed to `[7]`**

---

## Required Fixes (for approval)

### 1. Fix Status Reporting (`map_feed.h`/`map_feed.cpp`)
Add `actual_cluster_offset_x()` returning `placed_x_`, expose both in `FeedStatus` and `status_line()`:
```json
{ "cluster_x_requested": -200, "cluster_x_actual": -150 }
```

### 2. Add `cluster_offset_x` to patchd Persistence (`patchd.js`)
- Add `"cluster_offset_x"` to `MAP_KEYS` (line 257)
- Add command handler for `{"cmd":"cluster","cluster_x":N}` (after line 2359)
- Update protocol docs (line 52)

### 3. Add Cluster Offset UI to App (`waze_cluster_control.js`)
- Mirror HUD position controls: `moveCluster(dir)`, `centreCluster()`, `clusterOffset()`, `clusterPositionText()`
- Add buttons to `robertn_waze_cluster.js` page

---

## Risk Assessment

| Risk | Likelihood | Impact |
|------|------------|--------|
| Cluster offset not persisting across restart | **Certain** (not in MAP_KEYS) | User loses setting every reboot |
| Status API shows wrong position at edges | **Certain** (clamping vs requested) | User confusion, debugging difficulty |
| App cannot control cluster offset | **Certain** (no UI, no patchd handler) | Feature unusable from app |
| Build fails on target | Unknown (no toolchain) | Deployment blocker |

---

## Next Steps (Awaiting Approval)

1. **Approve fixes** → I will implement the three fixes above
2. **Verify build** → Need access to build environment (likely on target or CI)
3. **Test on hardware** → Requires iDrive 7 MGU with cluster-render

**Do NOT proceed with code changes until explicitly approved.**