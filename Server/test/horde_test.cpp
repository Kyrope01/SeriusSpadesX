// The horde brain's checks (Sources/Training/Horde.cpp), built on request
// with the other zs_tests and run by CTest. No server, no ENet: a SolidMap
// built cell by cell, and the facts the waves, the field and the veto must
// hold on it.
//
// The map is a plain: ground with its top at z = 32. One solid voxel is no
// ground at all, so the ground plane is a slab, z = 32 and z = 33 solid in
// every column. A wall is the same three voxels higher, its top at z = 29: a
// walker on the plain (ground 32) cannot step up a run of three onto it - two
// is a jump - and it was built not to be tunneled under either: nothing here
// digs.

#include <cstdio>
#include <vector>

#include "SolidMap.h"
#include "Training/Horde.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (ok) {
        std::printf("  ok: %s\n", what);
    } else {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

constexpr int GROUND_TOP = 32;
constexpr int WALL_TOP = GROUND_TOP - 3;  // three stacked: a wall, not a jump

void slab(SolidMap& map, int x, int y) {
    map.set_block(x, y, GROUND_TOP + 1, true);
    map.set_block(x, y, GROUND_TOP, true);
}

void plain(SolidMap& map) {
    for (int y = 0; y < SolidMap::SIZE_Y; ++y)
        for (int x = 0; x < SolidMap::SIZE_X; ++x) slab(map, x, y);
}

// The square ring of Chebyshev radius `r` around (cx, cy), closed everywhere
// except up to two gap cells named. `g2x < 0` means no second gap.
struct Ring {
    int cx, cy, r;
    int g1x, g1y;
    int g2x = -1, g2y = -1;
};

void ring(SolidMap& map, const Ring& w) {
    for (int y = w.cy - w.r; y <= w.cy + w.r; ++y)
        for (int x = w.cx - w.r; x <= w.cx + w.r; ++x) {
            const int dx = x > w.cx ? x - w.cx : w.cx - x;
            const int dy = y > w.cy ? y - w.cy : w.cy - y;
            if (dx != w.r && dy != w.r) continue;  // the ring only
            if (x == w.g1x && y == w.g1y) continue;
            if (w.g2x >= 0 && x == w.g2x && y == w.g2y) continue;
            map.set_block(x, y, WALL_TOP + 2, true);
            map.set_block(x, y, WALL_TOP + 1, true);
            map.set_block(x, y, WALL_TOP, true);
        }
}

horde::Config horde_cfg(int cx, int cy) {
    horde::Config cfg;
    cfg.core_x = cx;
    cfg.core_y = cy;
    cfg.spawn_x[0] = 32; cfg.spawn_y[0] = 32;
    cfg.spawn_x[1] = 479; cfg.spawn_y[1] = 32;
    cfg.spawn_x[2] = 32; cfg.spawn_y[2] = 479;
    cfg.spawn_x[3] = 479; cfg.spawn_y[3] = 479;
    cfg.spawn_size = 12;
    cfg.prep_seconds = 10.0;
    cfg.intermission_seconds = 5.0;
    cfg.spawn_interval = 0.5;
    cfg.wave_count_max = 2;
    cfg.core_hp = 150;
    cfg.core_mend = 25;
    cfg.field_period = 0.5;
    return cfg;
}

void test_field() {
    std::printf("[field] distances and steps on the open plain\n");
    SolidMap map;
    plain(map);
    horde::Horde h(map, horde_cfg(256, 256));
    check(h.distance(256, 256) == 0, "the core is distance 0");
    check(h.distance(255, 256) == 1, "a step aside is distance 1");
    check(h.distance(32, 32) > 400, "a far corner is a long walk");
    check(h.distance(-1, 0) < 0 && h.distance(512, 0) < 0, "outside is nowhere");
    int tx = 0, ty = 0;
    check(h.next_step(100, 100, tx, ty), "next_step answers on open ground");
    check(h.distance(tx, ty) < h.distance(100, 100), "next_step heads downhill");
    check(!h.next_step(256, 256, tx, ty), "at the core, no step helps");

    // The crossing rules of the SPEC: a climb of two and a drop of five are
    // walked, one block more either way is a cliff. Piles and pits on the
    // plain say so.
    {
        SolidMap rough;
        plain(rough);
        // A pile of two at (200, 200): its top is a jump away.
        rough.set_block(200, 200, 31, true);
        rough.set_block(200, 200, 30, true);
        // A pile of three at (210, 210): a wall, however it is looked at.
        rough.set_block(210, 210, 31, true);
        rough.set_block(210, 210, 30, true);
        rough.set_block(210, 210, 29, true);
        // A shallow pit at (220, 220): four down is a walk-in.
        rough.set_block(220, 220, 37, true);
        rough.set_block(220, 220, 36, true);
        // A deep pit at (230, 230): six down is a fall nobody leaves.
        rough.set_block(230, 230, 39, true);
        rough.set_block(230, 230, 38, true);
        horde::Horde hh(rough, horde_cfg(256, 256));
        check(hh.distance(200, 200) >= 0, "a pile of two is jumped");
        check(hh.distance(210, 210) < 0, "a pile of three is a wall");
        check(hh.distance(220, 220) >= 0, "a pit four deep is walked into");
        check(hh.distance(230, 230) < 0, "a pit six deep is not crossed");
        int px = 0, py = 0;
        check(hh.next_step(220, 220, px, py) && px == 221 && py == 220,
              "from the pit, climbing four out is refused: it heads straight");
    }

    // The core's column, three blocks standing on it: its top is a walkable
    // cell nothing steps onto, but the field has to spread from the cells
    // beside it - that is where the horde stops to bite.
    {
        SolidMap marked;
        plain(marked);
        for (int k = 1; k <= 3; ++k)
            marked.set_block(256, 256, GROUND_TOP - k, true);
        horde::Horde hh(marked, horde_cfg(256, 256));
        check(hh.distance(256, 256) >= 0, "the column's top is on the field");
        check(hh.distance(255, 256) == 1, "beside the column is one step from the core");
        check(hh.distance(255, 255) == 2 || hh.distance(257, 256) == 2,
              "the field spreads around the column");
        check(hh.distance(32, 32) > 400, "the far corners are still a long walk");
    }
}

void test_veto() {
    std::printf("[veto] the last gap cannot close by itself\n");
    // One ring, two gaps: closing one still leaves the other.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{256, 256, 10, 266, 256, 256, 246});
        horde::Horde h(map, horde_cfg(256, 256));
        check(h.distance(32, 32) >= 0, "the plain crosses the ring through its gaps");
        check(h.can_build(266, 256, 31), "closing one of two gaps is allowed");
    }
    // One ring, one gap: the gap is the only way in.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{100, 100, 6, 106, 100});
        horde::Horde h(map, horde_cfg(100, 100));
        check(h.can_build(105, 98, 31), "a block well inside the ring is allowed");
        check(!h.can_build(106, 100, 31), "the last gap refuses its closing block");
        check(!h.can_build(106, 100, 0) && !h.can_build(106, 100, 63),
              "the veto asks the same question at every height");
    }
    // The ring shut for real: whatever is asked now, the map was sealed
    // before the block, and the veto fails open.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{100, 100, 6, -1, -1});
        horde::Horde h(map, horde_cfg(100, 100));
        h.on_map_edited();
        h.tick(2.0);  // past the debounce: the field agrees the ring is shut
        check(h.distance(130, 100) < 0, "outside the shut ring is off the field");
        check(h.can_build(130, 100, 31), "with the core already sealed, the veto fails open");
        int tx = 0, ty = 0;
        check(h.next_step(140, 100, tx, ty), "off the field, a zombie still walks");
        check(tx == 139 && ty == 100, "it falls back to heading straight for the core");
    }
}

void test_waves() {
    std::printf("[waves] the state machine walks its rounds\n");
    SolidMap map;
    plain(map);
    horde::Horde h(map, horde_cfg(256, 256));
    h.restart(0.0);
    check(h.phase() == horde::Phase::Prep, "restart arms Prep");
    h.tick(5.0);
    check(h.phase() == horde::Phase::Prep, "half the prep is still Prep");
    h.tick(10.5);
    check(h.phase() == horde::Phase::Wave && h.wave() == 1, "prep over: wave one");
    std::vector<horde::SpawnAt> due;
    check(h.take_due(due, 10.7), "the wave has zombies waiting");
    check(due.size() == 1 && h.zombies_up() == 1, "the first is due and counted");
    h.take_due(due, 100.0);
    check(h.zombies_up() == 12, "wave one is twelve zombies (base)");
    bool on_ground = true;
    for (const horde::SpawnAt& s : due) {
        if (s.z != GROUND_TOP) on_ground = false;  // due holds the last eleven
    }
    check(on_ground, "spawn cells stand on the ground they were picked for");
    check(!h.wave_cleared(), "twelve walking is not a cleared wave");
    h.on_core_damaged(50);
    check(h.phase() == horde::Phase::Wave && h.core_hp() == 100, "the core takes a hit");
    for (int i = 0; i < 12; ++i) h.on_zombie_killed();
    check(h.wave_cleared(), "all twelve down clears the wave");
    h.tick(100.5);
    check(h.phase() == horde::Phase::Intermission, "a cleared wave turns to Intermission");
    check(h.core_hp() == 125, "the core mends by 25 between waves");
    h.tick(106.0);
    check(h.phase() == horde::Phase::Wave && h.wave() == 2, "intermission over: wave two");
    h.take_due(due, 200.0);
    check(h.zombies_up() == 20, "wave two is twenty zombies (base + growth)");
    for (int i = 0; i < 20; ++i) h.on_zombie_killed();
    h.tick(200.5);
    check(h.phase() == horde::Phase::Victory, "the last wave cleared is Victory");
    h.restart(1000.0);
    check(h.phase() == horde::Phase::Prep && h.wave() == 0, "restart resets the rounds");
    h.tick(1010.5);
    check(h.phase() == horde::Phase::Wave, "the rounds go on after a restart");
    h.on_core_damaged(999);
    check(h.phase() == horde::Phase::Defeat && h.core_hp() == 0, "the core gone is Defeat");
}

void test_growth() {
    std::printf("[growth] the wave size curve\n");
    horde::Config cfg;
    check(cfg.wave_zombies(1) == 12, "wave one: base");
    check(cfg.wave_zombies(2) == 20, "wave two: base + growth");
    check(cfg.wave_zombies(3) == 28, "wave three: base + 2 growth");
    check(cfg.wave_zombies(20) == 100, "far out: capped");
}

}  // namespace

int main() {
    test_field();
    test_veto();
    test_waves();
    test_growth();
    if (g_failures == 0) {
        std::printf("horde: all checks passed\n");
        return 0;
    }
    std::printf("horde: %d failed\n", g_failures);
    return 1;
}
