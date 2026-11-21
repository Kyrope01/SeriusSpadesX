// The horde brain's checks (Sources/Training/Horde.cpp), built on request
// with the other zs_tests and run by CTest. No server, no ENet: a SolidMap
// built cell by cell, and the facts the waves, the field and the veto must
// hold on it.
//
// The map is a plain: ground with its top at z = 32. One solid voxel is no
// ground at all, so the ground plane is a slab, z = 32 and z = 33 solid in
// every column. A wall is the same three voxels higher, its top at z = 29.
// One crossing is asked of it: the walk of the legs - a climb of two is a
// jump, and every fall is taken whole, for the dead take no falling damage;
// anything more than a jump up is a wall, for they do not dig. The veto
// asks the same walk the other way round, with the block a player wants
// held out of it, and the route the host paints is read off it cell by
// cell.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

    // The crossing rules of the SPEC: a climb of two is jumped and every
    // fall is taken whole, dead legs fear no landing; a climb of three is
    // a cliff. Piles and pits on the plain say so.
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
        // A shallow pit at (220, 220): four down is a walk-in. The slab is
        // taken out first - the blocks below are what a pit is floored with.
        rough.set_block(220, 220, 33, false);
        rough.set_block(220, 220, 32, false);
        rough.set_block(220, 220, 37, true);
        rough.set_block(220, 220, 36, true);
        // A deep pit at (230, 230): six down is a fall the field dares now,
        // and the fallback walks the dead back out of it.
        rough.set_block(230, 230, 33, false);
        rough.set_block(230, 230, 32, false);
        rough.set_block(230, 230, 39, true);
        rough.set_block(230, 230, 38, true);
        horde::Horde hh(rough, horde_cfg(256, 256));
        check(hh.distance(200, 200) >= 0, "a pile of two is jumped onto");
        check(hh.distance(210, 210) < 0, "a pile of three is a wall: no dig, no way");
        check(hh.distance(220, 220) >= 0, "a pit four deep is walked into");
        check(hh.distance(230, 230) >= 0, "a pit six deep is fallen into: the dead fear no landing");
        // The walk's edge is walked as well: from either pit the field
        // leads out again, downhill or by the fallback's straight line.
        int px = 0, py = 0;
        check(hh.next_step(220, 220, px, py) &&
                  hh.distance(px, py) < hh.distance(220, 220),
              "from the pit, the field leads it out");
        check(hh.next_step(230, 230, px, py) &&
                  (px != 230 || py != 230) && hh.distance(px, py) < hh.distance(230, 230),
              "from the deep pit, the fallback walks it out");
    }

    // The core's column, three blocks standing on it: its top is a walkable
    // cell nothing steps onto - no jump reaches it now - but the field has
    // to spread from the cells beside it - that is where the horde stops to
    // bite.
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
    // The ring shut for real: no climb, no dig, the walk is sealed out of
    // the core, and the map was sealed before the block, so the veto fails
    // open and the fallback walks a straight line.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{100, 100, 6, -1, -1});
        horde::Horde h(map, horde_cfg(100, 100));
        h.on_map_edited();
        h.tick(2.0);  // past the debounce: the recomputed field is shut out
        check(h.distance(130, 100) < 0, "the shut ring walls the core off");
        check(h.can_build(130, 100, 31), "with the core already sealed, the veto fails open");
        int tx = 0, ty = 0;
        check(h.next_step(140, 100, tx, ty) && (tx != 140 || ty != 100),
              "off the shut field, the fallback still moves a leg");
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
    bool one_corner = true;
    for (const horde::SpawnAt& s : due) {
        if (s.z != GROUND_TOP) on_ground = false;  // due holds the last eleven
        if (s.corner != h.ranked_corner(0)) one_corner = false;
    }
    check(on_ground, "spawn cells stand on the ground they were picked for");
    check(one_corner && h.ranked_corner(0) == 3,
          "one corner open: every zombie walks out of the corner the walk ranks first");
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

void test_route() {
    std::printf("[route] the way in, painted cell by cell\n");
    // The plain: from corner zero's anchor the route runs downhill to the
    // core, one cell a step, and is exactly the walk's distance long.
    {
        SolidMap map;
        plain(map);
        horde::Horde h(map, horde_cfg(256, 256));
        h.restart(0.0);
        h.tick(10.5);  // prep over: the wave is placed, its anchor kept
        int ax = -1, ay = -1;
        check(h.corner_anchor(0, ax, ay), "corner zero keeps its anchor");
        check(h.distance(ax, ay) >= 0, "the anchor stands on the field");
        std::vector<horde::RouteCell> cells;
        check(h.trace_route(ax, ay, cells), "the route traces from the anchor");
        check(!cells.empty() && cells.front().x == ax && cells.front().y == ay,
              "the route starts on the anchor");
        check(static_cast<int>(cells.size()) == h.distance(ax, ay) + 1,
              "the route is exactly the walk's distance long");
        bool steps_ok = true;
        for (size_t i = 1; i < cells.size(); ++i) {
            const int dx = cells[i].x - cells[i - 1].x;
            const int dy = cells[i].y - cells[i - 1].y;
            if (std::abs(dx) + std::abs(dy) != 1) steps_ok = false;
            if (h.distance(cells[i].x, cells[i].y) !=
                h.distance(cells[i - 1].x, cells[i - 1].y) - 1)
                steps_ok = false;
        }
        check(steps_ok, "every step of the route is one downhill cell");
        check(h.distance(cells.back().x, cells.back().y) == 0,
              "the route ends at the core");
        check(!h.trace_route(-1, 0, cells), "off the map, no route traces");
    }
    // A ring with one gap around the core: the only way in passes the gap,
    // and the traced route goes through it.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{256, 256, 10, 266, 256});
        horde::Horde h(map, horde_cfg(256, 256));
        h.restart(0.0);
        h.tick(10.5);
        int ax = -1, ay = -1;
        check(h.corner_anchor(0, ax, ay), "the gap ring still leaves an anchor");
        std::vector<horde::RouteCell> cells;
        check(h.trace_route(ax, ay, cells), "the route traces to the ringed core");
        bool passes_gap = false;
        for (const horde::RouteCell& c : cells)
            if (c.x == 266 && c.y == 256) passes_gap = true;
        check(passes_gap, "the route enters through the gap");
    }
    // Two walls the walk cannot cross: a cell shut off the field traces
    // nothing, and a core sealed away still anchors its corner - wherever
    // the field still stands, in the ring's inside or out on the plain.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{100, 100, 6, -1, -1});  // the core sealed in
        horde::Horde h(map, horde_cfg(100, 100));
        std::vector<horde::RouteCell> cells;
        check(!h.trace_route(130, 100, cells), "a cell off the field traces nothing");
        h.restart(0.0);
        h.tick(10.5);
        int ax = 0, ay = 0;
        check(h.corner_anchor(0, ax, ay), "a sealed core still anchors its corner");
        check(std::max(std::abs(ax - 100), std::abs(ay - 100)) <= 5,
              "the anchor stands where the field still does: inside the ring");
        check(h.ranked_corner(0) == 0,
              "the only corner on the field is the one the wave takes");
    }
    // Nothing anywhere: a core sealed away and every corner far beyond the
    // field's reach - no anchor for anyone, and the wave stands down
    // instead of clearing itself in silence.
    {
        SolidMap map;
        plain(map);
        ring(map, Ring{100, 100, 6, -1, -1});
        horde::Config cfg = horde_cfg(100, 100);
        cfg.spawn_x[0] = 460; cfg.spawn_y[0] = 460;  // the near corner gone too
        horde::Horde h(map, cfg);
        h.restart(0.0);
        h.tick(10.5);
        check(h.placed() == 0, "every corner out of the walk's reach: nothing stands up");
        std::vector<horde::SpawnAt> due;
        check(!h.take_due(due, 100.0) && h.zombies_up() == 0, "nothing streams");
        check(h.wave_cleared(), "the wave stands down");
    }
    {
        SolidMap map;
        plain(map);
        // A wall 200 cells out around corner zero: the search never sees a
        // cell of the field, so the corner stays unanchored - and the wave
        // turns to a corner the walk can truly reach instead of standing
        // down.
        ring(map, Ring{32, 32, 200, -1, -1});
        horde::Horde h(map, horde_cfg(256, 256));
        h.restart(0.0);
        h.tick(10.5);
        int ax = 0, ay = 0;
        check(!h.corner_anchor(0, ax, ay), "a corner the field never reaches holds no anchor");
        check(h.ranked_corner(0) == 3,
              "a sealed corner is passed over for one the walk reaches");
        check(h.placed() == 12, "the wave stands up elsewhere");
        std::vector<horde::SpawnAt> due;
        check(h.take_due(due, 100.0), "the wave still streams");
        bool from_rank_three = true;
        for (const horde::SpawnAt& s : due)
            if (s.corner != 3) from_rank_three = false;
        check(from_rank_three && h.zombies_up() == 12,
              "the whole wave walks in from the corner the walk found");
        check(!h.wave_cleared(), "a wave that found its way in is not cleared");
    }
    // A mesa twenty high with the core on top: the field pours off the
    // cliff - the dead fear no landing - and floods the plain, the plain
    // corners anchor, and the traced route dies at the cliff's foot, where
    // the horde stops to bite. The wave stands up all the same: a way in
    // the walk can reach is a way in, even when the last climb is its.
    {
        SolidMap map;
        plain(map);
        for (int y = 255; y <= 257; ++y)
            for (int x = 255; x <= 257; ++x)
                for (int k = 1; k <= 20; ++k)
                    map.set_block(x, y, GROUND_TOP - k, true);
        horde::Horde h(map, horde_cfg(256, 256));
        check(h.distance(258, 256) >= 0, "the field pours off a cliff of twenty");
        h.restart(0.0);
        h.tick(10.5);
        int ax = -1, ay = -1;
        check(h.corner_anchor(0, ax, ay), "a plain corner anchors under the mesa");
        std::vector<horde::RouteCell> cells;
        check(h.trace_route(ax, ay, cells), "the route traces to the mesa");
        check(!cells.empty() && h.distance(cells.back().x, cells.back().y) > 1 &&
                  h.distance(cells.back().x, cells.back().y) < h.distance(ax, ay),
              "the route ends at the cliff's foot, a bite from the core");
        check(h.placed() == 12, "the wave stands up off the mesa all the same");
        std::vector<horde::SpawnAt> due;
        check(h.take_due(due, 100.0) && h.zombies_up() == 12,
              "the wave streams in from the plain");
        check(!h.wave_cleared(), "a wave at the cliff's foot is not cleared");
    }
    // The perch the wave refuses: a tower the field cannot climb stands on
    // the corner's asked-for centre, its anchor ring holding both the
    // mesa's top - poured onto, walkable, and on the field - and the plain
    // below it. The wave begins on the plain, at the ring's ground level,
    // never on a perch its legs would be trusted to fall from.
    {
        SolidMap map;
        plain(map);
        for (int y = 255; y <= 257; ++y)
            for (int x = 255; x <= 257; ++x)
                for (int k = 1; k <= 20; ++k)
                    map.set_block(x, y, GROUND_TOP - k, true);
        for (int k = 1; k <= 24; ++k)
            map.set_block(258, 258, GROUND_TOP - k, true);  // unclimbable
        horde::Config cfg = horde_cfg(256, 256);
        cfg.spawn_x[0] = 258;
        cfg.spawn_y[0] = 258;
        horde::Horde h(map, cfg);
        h.restart(0.0);
        h.tick(10.5);
        int ax = -1, ay = -1;
        check(h.corner_anchor(0, ax, ay), "the corner anchors past its tower");
        check(ax == 258 && ay == 257,
              "the anchor stands on the plain, not on the mesa's poured top");
    }
}

}  // namespace

int main() {
    test_field();
    test_veto();
    test_waves();
    test_route();
    test_growth();
    if (g_failures == 0) {
        std::printf("horde: all checks passed\n");
        return 0;
    }
    std::printf("horde: %d failed\n", g_failures);
    return 1;
}
