# Cluster Horizontal Offset — Geometry & Pipeline Research

**Date:** 2026-11-24  
**Branch:** `feature/cluster-horizontal-offset`  
**Status:** Research complete — **STOP, awaiting approval before any implementation**

---

## 1. Complete Image Pipeline (Source → Scene)

| Stage | Component | File:Line | Input → Output Dimensions | Notes |
|-------|-----------|-----------|---------------------------|-------|
| **1. Phone stream** | CarPlay / Android Auto | — | CarPlay: 1088×480<br>Android Auto: 1280×720 | Phone renders at these sizes |
| **2. src_crop config** | `waze-on-cluster.conf:81` | `src_crop = 1280x720:0,0,1280,564` | 1280×720 → **1280×564** (map region) | Only for Android Auto; CarPlay uses whole frame |
| **3. Decoder/VPP** | `decoder_msdk.cpp:493-502` | `seq_w`=1280, `seq_h`=720 → VPP In Crop: (0,0,1280,564) → VPP Out: 1088×480 | **1088×480** (tex_size) | VPP scales cropped region to tex_size |
| **4. Texture atlas** | `yuv_atlas.cpp` / `ImageLayer` | 1088×480 NV12 → RGBA8 buffer | **1088×480** | One R8 buffer per plane (Y, U, V) |
| **5. map_quad()** | `map_geometry.cpp:7-27` | scene=1088×480, tex=1088×480 → `QuadRect{0,0,1088,480}` | **QuadRect centered in scene** | Fits texture preserving aspect ratio |
| **6. MapFeed::quad()** | `map_feed.cpp:265-276` | Takes map_quad, adds `cluster_offset_x`, **clamps to scene bounds** | `placed_x_` = actual offset after clamp | **BUG: returns cfg_.cluster_offset_x (requested) not placed_x_ (actual)** |
| **7. VideoLayer::place()** | `layer.cpp:382-390` | `g_->place(quad_, drawn_params().rect)` | Calls `Graph::place()` → `lay_out()` | Moves quad via `placed` node translation |
| **8. Renderer (RAMSES 27)** | `graph_ramses27.cpp:1421-1430` | `q.placed->setTranslation(r.x + t.px, h - r.y - t.py, 0)` | Quad positioned in scene pixels | `turned` node handles rotation/scale about pivot |

### Key Dimensions Summary

```
Android Auto source:     1280 × 720  (map in top 564px, UI below)
src_crop (configured):   1280 × 564  (X=0, Y=0, W=1280, H=564)
VPP output (tex_size):   1088 × 480  (exactly cluster map slot)
Cluster scene (slot 0x100): 1088 × 480
map_quad result:         x=0, y=0, w=1088, h=480  (fills scene exactly)
```

**Critical observation:** The texture (1088×480) exactly fills the cluster scene (1088×480). There are **zero extra source pixels** horizontally. Any horizontal shift of the quad creates empty/black edges.

---

## 2. Vehicle Arrow Independence

**Confirmed: Map CAN shift independently of vehicle arrow.**

| Component | Location | Role |
|-----------|----------|------|
| **Map layer** | waze-on-cluster video layer | Decoded H.264 stream, positioned via `cluster_offset_x` |
| **Vehicle arrow** | `CarModelViewMetadata` (cluster-render ABI) | Separate KOMBI overlay, NOT in video stream |
| **Guide hold** | `holds.cpp:103` | `t.follows = "map"` — follows map layer position |

The vehicle arrow is a native KOMBI UI element (SOME/IP metadata), drawn on top of the map slot. The map video layer sits underneath. Shifting the map layer does **not** move the arrow.

---

## 3. Overscan / Extra Source Pixels — Availability Analysis

### Current State: **NO horizontal overscan available**

| Source | Stream Size | Map Region | Extra Horizontal Pixels |
|--------|-------------|------------|-------------------------|
| **CarPlay** | 1088×480 | 1088×480 (full) | **0** |
| **Android Auto** | 1280×720 | 1280×564 (top) | **0** (map is full width of stream) |

The Android Auto stream is 1280px wide; the map uses the full 1280px width (src_crop W=1280). The extra 156px vertically (720-564) is UI/turn card space, **not horizontal overscan**.

### What Would Be Needed

To shift horizontally without empty edges, the source must provide **>1088 pixels of map content horizontally**. Options:
1. **Phone renders wider map** (e.g., 1400×480) → src_crop captures wider region → VPP scales to 1088×480 → horizontal "overscan" buffer exists
2. **Decoder crops differently** — shift VPP input crop X offset at runtime (requires VPP reconfiguration per frame)
3. **Renderer crops texture** — sample texture with UV offset (shader change)

**Current implementation cannot do any of these.** The src_crop is per-sequence (set at decoder init), not per-frame.

---

## 4. Architecture Evaluation

### Requirements (All Must Be Met)
1. ✅ Map moves independently of vehicle arrow
2. ✅ Map fully covers 1088×480 viewport
3. ❌ No black/transparent/empty edges at any offset
4. ✅ Original scale unchanged (1:1 texture:scene)
5. ✅ Original aspect ratio unchanged
6. ✅ HUD unaffected

---

### Architecture A: Move QuadRect within Scene Bounds (Current Implementation)

**How it works:** `MapFeed::quad()` takes centered `QuadRect{0,0,1088,480}`, adds `cluster_offset_x`, clamps to `[0, scene_w - quad.w]`. The quad is translated via `placed` node.

| Requirement | Met? | Notes |
|-------------|------|-------|
| 1. Independent of arrow | ✅ | Arrow is separate overlay |
| 2. Covers viewport | ❌ | At offset ≠ 0, empty edges appear |
| 3. No empty edges | ❌ | **FAILS** — no overscan pixels |
| 4. Scale unchanged | ✅ | Texture not rescaled |
| 5. Aspect unchanged | ✅ | No distortion |
| 6. HUD unaffected | ✅ | Separate feed |

**Verdict:** **FAILS requirement 3.** Only works if source has extra horizontal pixels.

---

### Architecture B: Expand Source Capture / Crop Area (Runtime VPP Input Crop Shift)

**How it works:** Modify decoder's VPP input crop X offset per frame to "pan" within a wider source image. Requires source >1088px wide map content.

| Requirement | Met? | Notes |
|-------------|------|-------|
| 1. Independent of arrow | ✅ | |
| 2. Covers viewport | ✅ | If source wide enough |
| 3. No empty edges | ✅ | If source provides overscan |
| 4. Scale unchanged | ✅ | VPP still outputs 1088×480 |
| 5. Aspect unchanged | ✅ | |
| 6. HUD unaffected | ✅ | |

**Feasibility:** **Requires phone app changes** to render wider map. Current Android Auto (1280px) and CarPlay (1088px) don't provide this. VPP reconfiguration per frame is expensive (restart decoder).

---

### Architecture C: Shift Source Pre-Renderer (Shader UV Offset)

**How it works:** Keep quad filling scene (0,0,1088,480). In fragment shader, sample texture with UV offset: `uv.x += offset / 1088.0`. Texture clamp mode = `ClampToEdge` extends edge pixels.

| Requirement | Met? | Notes |
|-------------|------|-------|
| 1. Independent of arrow | ✅ | |
| 2. Covers viewport | ✅ | Quad always fills scene |
| 3. No empty edges | ⚠️ | **ClampToEdge stretches last pixel** — looks wrong for map |
| 4. Scale unchanged | ✅ | |
| 5. Aspect unchanged | ✅ | |
| 6. HUD unaffected | ✅ | |

**Verdict:** **FAILS requirement 3 visually.** ClampToEdge stretches edge pixels into a smear. Would need `Wrap::Repeat` with mirrored overscan content, or custom border handling — neither available with current texture.

---

### Architecture D: Renderer Cropping / Viewport Shift (View Frustum Shift)

**How it works:** Use a `View` (3D camera) instead of 2D quad. Shift camera frustum horizontally to "pan" across a larger virtual texture. Requires texture larger than viewport.

| Requirement | Met? | Notes |
|-------------|------|-------|
| 1. Independent of arrow | ✅ | |
| 2. Covers viewport | ✅ | If texture > viewport |
| 3. No empty edges | ✅ | If texture provides overscan |
| 4. Scale unchanged | ✅ | |
| 5. Aspect unchanged | ✅ | |
| 6. HUD unaffected | ✅ | |

**Feasibility:** Same as B — requires larger source texture. Current cluster-render uses 2D quads for map layer; switching to View adds complexity.

---

### Comparison Table

| Architecture | Req 1 | Req 2 | Req 3 | Req 4 | Req 5 | Req 6 | Source Change Needed | Complexity |
|--------------|-------|-------|-------|-------|-------|-------|---------------------|------------|
| **A: Move QuadRect** | ✅ | ❌* | ❌ | ✅ | ✅ | ✅ | None | Low (done) |
| **B: VPP Crop Shift** | ✅ | ✅ | ✅† | ✅ | ✅ | ✅ | Phone renders wider | High |
| **C: Shader UV Offset** | ✅ | ✅ | ⚠️ | ✅ | ✅ | ✅ | Shader + wider texture | Medium |
| **D: View Frustum** | ✅ | ✅ | ✅† | ✅ | ✅ | ✅ | Wider texture | High |

*At offset=0 only  
†If source provides overscan pixels

---

## 5. Recommended Architecture: **Modified Architecture B — Phone-Side Overscan + VPP Crop**

### Why This Is the Only Complete Solution

1. **Requirements 2 & 3 are mutually exclusive with current source dimensions** — you cannot cover 1088px viewport while shifting horizontally without source pixels beyond 1088px.

2. **The phone (CarPlay/Android Auto app) controls the rendered map size.** The patch advertises `tex_size=1088x480` to the phone (PATCH.md Edit 4). The phone renders exactly that.

3. **To get overscan:** Patch must advertise a **wider `tex_size`** (e.g., `1400x480`). Phone renders wider map. Decoder's `src_crop` captures wider region. VPP scales to 1088×480. At runtime, shift VPP input crop X to pan.

### Implementation Path

| Step | Component | Change |
|------|-----------|--------|
| 1 | Patch (phone side) | Advertise wider `tex_size` (e.g., 1400×480) |
| 2 | `waze-on-cluster.conf` | Update `tex_size = 1400x480`, `src_crop = 1400x480:0,0,1400,480` |
| 3 | `decoder_msdk.cpp` | Add runtime VPP input crop X offset control (per-frame or per-cluster_offset change) |
| 4 | `map_feed.cpp` | Instead of moving quad, compute VPP crop X from `cluster_offset_x`, trigger decoder restart |
| 5 | `map_geometry.cpp` | `map_quad()` unchanged (still centers 1088×480 output in scene) |

**Critical constraint:** VPP reconfiguration requires decoder restart (`MFXVideoVPP_Close` + `Init`). Acceptable for occasional offset changes (user drags map), not per-frame.

---

## 6. Implementation Detail Checks

### placed_x_ Initialization
| File | Line | Detail |
|------|------|--------|
| `map_feed.h` | ~150 | `int placed_x_ = 0;` (member, default 0) |
| `map_feed.cpp` | 271 | `placed_x_ = std::max(0, std::min(placed_x, scene_w - quad.w));` — set in `quad()` |

**At startup:** `quad()` called from `MapFeed` constructor → `placed_x_ = 0` (centered). Correct.

### Zero Offset Behavior
- `cluster_offset_x = 0` → `map_quad()` returns `{0,0,1088,480}` → `placed_x_ = 0` → quad at scene left edge (which IS center since quad.w = scene_w)
- **Correct:** Map centered, no shift.

### Renderer Reconnect Behavior
| Scenario | Behavior |
|----------|----------|
| Renderer restarts | `MapFeed::attach()` → `rebuild()` → `quad()` recomputes from `cfg_.cluster_offset_x` ✅ |
| Decoder restarts (sequence change) | `Decoder::State::setup_decoder()` → VPP reinitialized with same `src_crop` ✅ |
| Hosted feed (app hosts map) | `hosted_vpp_scale=1` uses VPP to crop host's part — **offset would need separate handling** |

### Hosted Feed (`hosted_vpp_scale=1`)
- `decoder_msdk.cpp:96-103`: VPP crops host's part of picture, scales to shown size
- Cluster offset **not currently applied** to hosted feed path
- Would need `part_rect()` adjustment in `convert()` (line 767) to incorporate offset

### Status API Reliability — **BUG CONFIRMED**
```cpp
// map_feed.h:189 — returns REQUESTED offset
int cluster_offset_x() const { return cfg_.cluster_offset_x; }

// map_feed.cpp:271 — ACTUAL offset after clamping
placed_x_ = std::max(0, std::min(placed_x, scene_w - quad.w));
```
- At boundaries: requested ≠ actual (e.g., request -200, actual 0)
- `MapFeeds::status()` → `FeedStatus` → `status_line()` JSON reports **requested**
- **Fix:** Add `actual_cluster_offset_x()` returning `placed_x_`, expose both in status

---

## 7. Build Environment Verification

| Tool | Available? | Notes |
|------|------------|-------|
| `g++` | ❌ | Not in PATH |
| `clang++` | ❌ | Not in PATH |
| `make` | ❌ | Not in PATH |
| `ninja` | ❌ | Not in PATH |
| `cmake` | ❌ | Not in PATH |

**Docker/CI:** Not checked in this environment. Project likely uses Docker for cross-compilation to MGU (ARM). See `CONTRIBUTING.md` for build instructions.

**Targets:** 
- `cluster-render` (RAMSES 19 for iDrive 7, RAMSES 27 for iDrive 8)
- `waze-on-cluster` (decoder + map feed + control)
- Apps: `robertn_waze_cluster` (JS app for MGU Store)

**Cannot verify compilation in this environment.** Need target toolchain or CI access.

---

## 8. Remaining Unknowns Requiring Hardware Testing

| Unknown | How to Test |
|---------|-------------|
| **Phone renders wider map** | Patch advertises 1400×480 → check CarPlay/Android Auto renders map at that width |
| **VPP handles dynamic crop X** | Runtime `vpp.vpp.In.CropX` change → decoder restart → measure latency, visual glitches |
| **ClampToEdge visual quality** | If using shader UV offset (Arch C), test edge stretching on real cluster |
| **Guide hold follows shifted map** | `holds.cpp:103` follows "map" layer → verify turn guidance box moves with map |
| **HUD map unaffected** | Verify `hud` feed in `MapFeeds` ignores `cluster_offset_x` |
| **Hosted feed offset** | Test with `hosted_vpp_scale=1` — does map in host app shift correctly? |
| **Performance** | Decoder restart on offset change — acceptable latency? (<100ms?) |
| **Persist across reboot** | Verify `cluster_offset_x` written to `map.state.conf` via patchd (currently broken) |

---

## 9. Blocking Issues to Fix Before Any Architecture Works

1. **Status API reports wrong offset** (`map_feed.h:189` returns requested not actual)
2. **`cluster_offset_x` not persisted** (`patchd.js` MAP_KEYS missing, no command handler)
3. **App has no UI for cluster offset** (`waze_cluster_control.js` only has HUD controls)
4. **Hosted feed ignores cluster offset** (separate code path in decoder)

---

## 10. Next Steps (Awaiting Approval)

1. **Fix the 3 blocking issues above** (status reporting, persistence, app UI) — these are independent of architecture choice
2. **Decide on architecture** — current source dimensions make Architecture A (only implemented one) fail requirement 3. Architecture B requires phone-side changes.
3. **If Architecture B chosen:** Coordinate with phone app team to render wider map; implement VPP crop shift in decoder
4. **Hardware testing** on iDrive 7 MGU with cluster-render

**Do NOT proceed with implementation until architecture is approved and phone-side changes are confirmed feasible.**