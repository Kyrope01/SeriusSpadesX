// The horde's brain, the body of it. See Horde.h for what it is and how the
// veto, the field and the waves decide; the comments here are about the how.

#include "Horde.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <queue>
#include <utility>

#include "SolidMap.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace horde {

namespace {

constexpr int kMapW = SolidMap::SIZE_X, kMapH = SolidMap::SIZE_Y;
constexpr int kUnreachable = -1;

size_t cell_index(int x, int y) {
    return static_cast<size_t>(y) * kMapW + x;
}

}  // namespace

int Config::wave_zombies(int wave) const {
    const int n = kWaveBase + kWaveGrowth * ((std::max)(1, wave) - 1);
    return (std::min)(kWaveMaxZombies, n);
}

// Starts in Prep with the countdown unarmed (zero, so the first tick would
// begin wave one); the host calls restart(now) to arm it. The field is
// computed right away - it is asked for before the first tick.
Horde::Horde(const SolidMap& map, const Config& cfg) : map_(map), cfg_(cfg) {
    core_hp_ = cfg_.core_hp;
    recompute();
}

int Horde::distance(int x, int y) const {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return kUnreachable;
    // The cell's best-reached surface, whatever floor it stands on.
    int best = kUnreachable;
    const int32_t* d = &dist_[cell_index(x, y) * kSurfMax];
    for (int k = 0; k < kSurfMax; ++k)
        if (d[k] != kUnreachable && (best == kUnreachable || d[k] < best))
            best = d[k];
    return best;
}

// --- the standing surfaces ---------------------------------------------------

// A column is not one floor. Every solid run top with two open voxels above
// it is a surface the dead can stand on - the outside of a wall top is one,
// and so is the floor of a tunnel through it - and the field fills them all,
// so the walk goes under as well as over, and no gate an arch leaves open
// closes a town. The surfaces are scanned once a recompute and cached; a
// column asked for between an edit and the refresh serves a step a tick
// early, nothing worse.
void Horde::fill_surfaces(int x, int y) {
    const size_t i = cell_index(x, y);
    const uint64_t col = map_.column(x, y);
    int n = 0;
    int prev_solid = 0;
    for (int z = 0; z < SolidMap::SIZE_Z; ++z) {
        const int solid = static_cast<int>((col >> z) & 1u);
        if (solid && !prev_solid) {
            // A solid run opens at z: its top voxel, standable only with
            // two open voxels above it. z counts down, so the sky sits at
            // the lower z and the pair above the surface is z-2 and z-1.
            if (z >= 2 && ((col >> (z - 2)) & 3u) == 0) {
                surf_z_[i * kSurfMax + n] = static_cast<int16_t>(z);
                if (++n == kSurfMax) break;
            }
        }
        prev_solid = solid;
    }
    surf_n_[i] = static_cast<int8_t>(n);
}

int Horde::surf_count(int x, int y) {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return 0;
    const size_t i = cell_index(x, y);
    if (surf_n_[i] < 0) fill_surfaces(x, y);
    return surf_n_[i];
}

int Horde::surface(int x, int y, int k) {
    if (k < 0 || k >= surf_count(x, y)) return -1;
    return surf_z_[cell_index(x, y) * kSurfMax + k];
}

// The surface the field reaches best: the walk's own floor of the cell,
// what a zombie there stands on and the paint truly runs at. Ties keep
// the lowest slot; -1 when no surface of the cell is on the walk.
int Horde::best_state(int x, int y) const {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return -1;
    int best = kUnreachable, bk = -1;
    const int32_t* d = &dist_[cell_index(x, y) * kSurfMax];
    for (int k = 0; k < kSurfMax; ++k)
        if (d[k] != kUnreachable && (best == kUnreachable || d[k] < best)) {
            best = d[k];
            bk = k;
        }
    return bk;
}

// The column's first surface, its topmost floor: the level the anchors
// filter their rings on.
int Horde::ground(int x, int y) {
    return surface(x, y, 0);
}

// --- the field ---------------------------------------------------------------

// A walk from the core over the map's standing surfaces: the distance of
// every floor the walking dead could reach, the floor of a tunnel under
// its arch as much as the arch's top. Without `dig` it is the crossing
// both the build veto asks with and the zombies themselves walk:
// kWalkClimb a jump, and every fall taken whole - the host leaves its
// dead their falling damage, so no drop is a wall to them - nothing dug,
// so stairs, ramps and notches are the way up, cliffs and pits are doors
// down, and only a climb more than a jump stands in the way. With `dig`
// set it is the richer walk a spade-wielding zombie could make: the same
// falls, and any climb, each block beyond the jump priced at kDigPenalty
// steps - about the time its spade takes that block off - kept for the
// day the dead dig again (the canopy of a forest is one such day's work).
// `block_x` and `block_y` name one column held out of the walk, the
// veto's candidate; -1 for none. 4-neighbours: corners are where zombies
// jam on pillars, not where they cross. The edges are not all one step
// wide, so the walk is a Dijkstra, not the breadth-first queue it once
// was. Every surface of a cell is a state of its own: which floor a
// walker stands on is decided where it crosses, not where it starts.
void Horde::flood(std::vector<int32_t>& dist, int block_x, int block_y, bool dig) {
    dist.assign(static_cast<size_t>(kMapW) * kMapH * kSurfMax, kUnreachable);
    const int cx = cfg_.core_x, cy = cfg_.core_y;
    if (cx < 0 || cx >= kMapW || cy < 0 || cy >= kMapH) return;
    if (block_x == cx && block_y == cy) return;
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    using QE = std::pair<int32_t, int32_t>;  // (steps so far, state id)
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    const auto state = [&](int x, int y, int k) -> size_t {
        return cell_index(x, y) * kSurfMax + k;
    };
    // The core may carry a column, the marker the clients draw: its ground
    // sits steps above the plain, and no walk starts there. The cells beside
    // it are seeded alongside it, so the field still spreads from the
    // neighbourhood the horde only has to reach to bite.
    auto seed = [&](int x, int y, int d) {
        if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return;
        if (x == block_x && y == block_y) return;
        for (int k = 0; k < surf_count(x, y); ++k) {
            const size_t s = state(x, y, k);
            if (dist[s] == kUnreachable) {
                dist[s] = d;
                open.emplace(d, static_cast<int32_t>(s));
            }
        }
    };
    seed(cx, cy, 0);
    for (int k = 0; k < 4; ++k) seed(cx + kDx[k], cy + kDy[k], 1);
    while (!open.empty()) {
        const QE top = open.top();
        open.pop();
        const int d = top.first;
        const size_t s = static_cast<size_t>(top.second);
        if (d != dist[s]) continue;  // a cheaper way came later
        const size_t i = s / kSurfMax;
        const int k = static_cast<int>(s % kSurfMax);
        const int x = static_cast<int>(i) % kMapW;
        const int y = static_cast<int>(i) / kMapW;
        const int z = surf_z_[i * kSurfMax + k];
        for (int q = 0; q < 4; ++q) {
            const int nx = x + kDx[q], ny = y + kDy[q];
            if (nx < 0 || nx >= kMapW || ny < 0 || ny >= kMapH) continue;
            if (nx == block_x && ny == block_y) continue;
            const size_t ni = cell_index(nx, ny);
            const int n = surf_count(nx, ny);
            for (int m = 0; m < n; ++m) {
                // z grows downward: dz is a drop and -dz a climb. Each of
                // the neighbour's surfaces is a crossing of its own - the
                // road under the arch and the road over its top are one
                // move apart only in a bird's flight, never in the legs'.
                const int dz = surf_z_[ni * kSurfMax + m] - z;
                int nd = d + 1;
                if (dig) {
                    if (dz > Config::kFall) continue;
                    if (dz < -Config::kWalkClimb)
                        nd += Config::kDigPenalty * (-dz - Config::kWalkClimb);
                } else {
                    if (dz > Config::kFall || dz < -Config::kWalkClimb) continue;
                }
                const size_t ns = state(nx, ny, m);
                if (dist[ns] != kUnreachable && dist[ns] <= nd) continue;
                dist[ns] = nd;
                open.emplace(nd, static_cast<int32_t>(ns));
            }
        }
    }
}

// Can a wave still walk in? The anchors are the far end of the walk within
// kSpawnSearch rings of the corners, so that is the reach the veto
// protects: with the candidate block held out, does the walk still stand
// on one cell a wave could raise from, anywhere in a corner's reach?
// Every corner the map names is asked whole, whatever order the waves have
// been walking them in; a way in nobody can anchor is not the veto's to
// protect.
bool Horde::reach_spawn(const std::vector<int32_t>& dist) const {
    const auto on_walk = [&](int x, int y) {
        if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return false;
        const int32_t* d = &dist[cell_index(x, y) * kSurfMax];
        for (int k = 0; k < kSurfMax; ++k)
            if (d[k] != kUnreachable) return true;
        return false;
    };
    for (int c = 0; c < 4; ++c) {
        const int cx = cfg_.spawn_x[c], cy = cfg_.spawn_y[c];
        for (int r = 0; r < Config::kSpawnSearch; ++r) {
            for (int y = cy - r; y <= cy + r; ++y) {
                if (y == cy - r || y == cy + r) {
                    for (int x = cx - r; x <= cx + r; ++x)
                        if (on_walk(x, y)) return true;
                } else if (on_walk(cx - r, y) || on_walk(cx + r, y)) {
                    return true;
                }
            }
        }
    }
    return false;
}

void Horde::recompute() {
    const size_t cells = static_cast<size_t>(kMapW) * kMapH;
    surf_z_.assign(cells * kSurfMax, -1);
    surf_n_.assign(cells, -1);  // -1: not yet scanned
    // The walk without the spade: what legs can cross, and stairs when they
    // cannot. Turning the dig back on is the `true` in this call.
    flood(dist_, -1, -1, false);
    field_dirty_ = false;
}

void Horde::on_map_edited() { field_dirty_ = true; }

// --- walking ----------------------------------------------------------------

bool Horde::next_step(int x, int y, int& tx, int& ty) {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return false;
    if (surf_count(x, y) <= 0) return false;  // nowhere to stand
    const int bk = best_state(x, y);
    const int here =
        bk < 0 ? kUnreachable : dist_[cell_index(x, y) * kSurfMax + bk];
    const int z = surface(x, y, bk < 0 ? 0 : bk);
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    int best = -1, bx = -1, by = -1;
    for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k], ny = y + kDy[k];
        if (nx < 0 || nx >= kMapW || ny < 0 || ny >= kMapH) continue;
        const int base = cell_index(nx, ny) * kSurfMax;
        for (int m = 0; m < surf_count(nx, ny); ++m) {
            const int nd = dist_[base + m];
            if (nd < 0) continue;
            if (here >= 0 && nd >= here) continue;  // downhill only
            // The field prices the crossing: without the dig it takes only
            // what legs cross - a jump up, any fall down, a climb more than
            // a jump refused - so the neighbour handed back is always a
            // step the walker can make, and the surface handed down with
            // it is one the legs truly reach.
            const int dz = surface(nx, ny, m) - z;
            if (dz > Config::kFall || dz < -Config::kWalkClimb) continue;
            if (best < 0 || nd < best) {
                best = nd;
                bx = nx;
                by = ny;
            }
        }
    }
    // At the core's own cell, no step helps; that is the zombie's business.
    if (here == 0) return false;
    // Off the field: head straight for the core, terrain permitting - a slow
    // walk around a broken field, not a standstill.
    if (bx < 0) {
        const int cx = cfg_.core_x, cy = cfg_.core_y;
        if (x == cx && y == cy) return false;
        const int sx = std::abs(cx - x) >= std::abs(cy - y) ? (cx > x ? 1 : -1) : 0;
        const int sy = sx == 0 ? (cy > y ? 1 : -1) : 0;
        if (sx != 0 && surf_count(x + sx, y) > 0) {
            tx = x + sx;
            ty = y;
            return true;
        }
        if (sy != 0 && surf_count(x, y + sy) > 0) {
            tx = x;
            ty = y + sy;
            return true;
        }
        if (sx != 0 && surf_count(x, y + (cy > y ? 1 : -1)) > 0) {
            tx = x;
            ty = y + (cy > y ? 1 : -1);
            return true;
        }
        return false;
    }
    tx = bx;
    ty = by;
    return true;
}

bool Horde::near_core(int x, int y, int cells) const {
    return std::abs(x - cfg_.core_x) <= cells && std::abs(y - cfg_.core_y) <= cells;
}

bool Horde::corner_anchor(int corner, int& x, int& y) const {
    if (corner < 0 || corner >= 4) return false;
    if (anchor_x_[corner] < 0) return false;
    x = anchor_x_[corner];
    y = anchor_y_[corner];
    return true;
}

// The way in, one tread a cell: from wherever it starts the walk goes
// downhill on the field, each lower surface a legal crossing (the flood
// only ever joins surfaces a jump or a fall connect), until no lower
// surface is left - the core itself when its column is not in the way,
// and the doorstep beside it when it is, which is where the horde stops
// to bite. Each tread carries the surface it was walked on at, for the
// host paints the line of march at the walk's own height, never on the
// top of whatever column happens to stand over the road. The field is
// all unit prices, so a traced route is exactly the walk's distance
// long, one cell of it a step; the cap is a guard against a broken
// field, not a route that is expected.
bool Horde::trace_route(int x, int y, std::vector<RouteCell>& out) {
    out.clear();
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return false;
    int k = best_state(x, y);
    if (k < 0) return false;
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    const size_t guard = static_cast<size_t>(kMapW) * kMapH;
    while (out.size() < guard) {
        const int z = surface(x, y, k);
        out.push_back(RouteCell{x, y, z});
        const int here = dist_[cell_index(x, y) * kSurfMax + k];
        int best = -1, bx = -1, by = -1, bk = -1;
        for (int q = 0; q < 4; ++q) {
            const int nx = x + kDx[q], ny = y + kDy[q];
            if (nx < 0 || nx >= kMapW || ny < 0 || ny >= kMapH) continue;
            const int base = cell_index(nx, ny) * kSurfMax;
            for (int m = 0; m < surf_count(nx, ny); ++m) {
                const int nd = dist_[base + m];
                if (nd < 0 || nd >= here) continue;  // downhill only
                const int dz = surface(nx, ny, m) - z;
                if (dz > Config::kFall || dz < -Config::kWalkClimb) continue;
                if (best < 0 || nd < best) {
                    best = nd;
                    bx = nx;
                    by = ny;
                    bk = m;
                }
            }
        }
        if (bx < 0) break;  // the doorstep: no lower crossing from here
        x = bx;
        y = by;
        k = bk;
    }
    return true;
}

// --- the veto ----------------------------------------------------------------

// Two floods, one with the candidate column held out. The map is not touched
// here: the candidate only hides that column from the walk. The walk the
// zombies use is the same one the veto asks with, so the second flood - the
// map as it stands - is written straight into the field, kept fresh by the
// very question the veto came to ask (the same work the debounce would soon
// do).
bool Horde::can_build(int x, int y, int z) {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return true;
    if (z < 0 || z >= SolidMap::SIZE_Z) return true;
    std::vector<int32_t> with_block;
    flood(with_block, x, y, false);
    flood(dist_, -1, -1, false);  // the walk's answer, not the veto's
    field_dirty_ = false;
    if (reach_spawn(with_block)) return true;
    // Fail open: sealed before this block too - the tower stands or falls on
    // its own, refusing nothing that changes nothing.
    if (!reach_spawn(dist_)) return true;
    // This block would have sealed the last way in.
    return false;
}

// --- the waves ----------------------------------------------------------------

// A wave walks in from the far end of the walk. Every corner is asked for
// its anchor: the scan sweeps the rings around its asked-for centre out to
// kSpawnSearch and keeps the ground-level cell the field reaches LAST -
// the walk's frontier. The nearest cell of the walk could sit inside the
// very town the core is defended in, a jump from the gate, and such a
// wave never rings the walls nor walks a way worth painting; the frontier
// is where the legs run out, the feet of the border cliffs and the
// outside of whatever walls the map raises around the core, and the wave
// walks in from there. The corners a real map names
// sit in border mountains and sea, and a corner whose whole reach the
// walk never touches has no anchor, and its share of the wave is simply
// not raised.
//
// The corners are then RANKED by the way the walk reads into each: the
// route traced from an anchor either ends at the core's doorstep or dies
// against a climb the legs cannot make - in a walk that cannot dig, a
// cliff on the way in is a dead end, however freely the field spills over
// its drop on the far side. A route that ends where the horde bites wins
// its corner the wave's first turn, the shortest way first of all; a dead
// end waits behind whatever works; no anchor waits last. The wave comes
// from the first corners of that order: systematically the way in the
// terrain leaves, never a corner number the settings happened to name.
// Back cells hold the anchor's rule, for rows of dozens reach past cliffs
// onto ground their corner's first cell cannot see; the nearest fielded
// cell of each row is taken, and a strip with none crowds onto the anchor.
// The anchors are kept - the host traces the route it paints from them.
void Horde::place_wave(double now) {
    if (field_dirty_) recompute();
    const int n = cfg_.wave_zombies(wave_);
    const int nc = cfg_.active_corners();
    auto stands = [&](int x, int y) {
        if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return false;
        if (surf_count(x, y) <= 0) return false;
        const int32_t* d = &dist_[cell_index(x, y) * kSurfMax];
        for (int k = 0; k < kSurfMax; ++k)
            if (d[k] >= 0) return true;
        return false;
    };
    // The anchors, all four corners: the far end of the walk, the frontier.
    // The scan sweeps the rings around each corner's asked-for centre out
    // to kSpawnSearch and keeps the cell the field reaches LAST, the walk's
    // longest way in, wherever the terrain stops the legs. A ring can
    // stand at many heights - a skirt of cliff over the plain, a canopy
    // over open ground - and a wave begun on a perch lands in foliage its
    // legs cannot cross, so only the cells at the ring's own ground
    // level, the height most of its cells stand at, are candidates, never
    // one perched more than kWalkDrop above it. Ties keep the first cell
    // the scan reads.
    std::vector<int> rungs_x, rungs_y, rungs_g;
    const auto note = [&](int x, int y) {
        if (!stands(x, y)) return;
        rungs_x.push_back(x);
        rungs_y.push_back(y);
        rungs_g.push_back(ground(x, y));
    };
    for (int c = 0; c < 4; ++c) {
        anchor_x_[c] = -1;
        anchor_y_[c] = 0;
        const int cx = cfg_.spawn_x[c], cy = cfg_.spawn_y[c];
        int best_d = -1;
        for (int r = 0; r < Config::kSpawnSearch; ++r) {
            rungs_x.clear();
            rungs_y.clear();
            rungs_g.clear();
            // The ring's cells in reading order: the full rows on the
            // frame's top and bottom, the side pair between them.
            for (int y = cy - r; y <= cy + r; ++y) {
                if (y == cy - r || y == cy + r) {
                    for (int x = cx - r; x <= cx + r; ++x) note(x, y);
                } else {
                    note(cx - r, y);
                    note(cx + r, y);
                }
            }
            if (rungs_x.empty()) continue;
            // The ring's ground level: the height most of its candidates
            // stand at, ties to the lower ground.
            std::vector<int> levels(rungs_g);
            std::sort(levels.begin(), levels.end());
            int level = levels.front(), best_n = 0, run_n = 0;
            for (size_t k = 0; k < levels.size(); ++k) {
                if (k > 0 && levels[k] != levels[k - 1]) run_n = 0;
                ++run_n;
                if (run_n >= best_n) {
                    best_n = run_n;
                    level = levels[k];
                }
            }
            for (size_t k = 0; k < rungs_x.size(); ++k) {
                if (std::abs(rungs_g[k] - level) > Config::kWalkDrop) continue;
                const int d = distance(rungs_x[k], rungs_y[k]);
                if (d > best_d) {
                    best_d = d;
                    anchor_x_[c] = rungs_x[k];
                    anchor_y_[c] = rungs_y[k];
                }
            }
        }
    }
    // The ranking: each anchored corner's way in, read out loud.
    struct Ranked {
        int corner, end, dist;
        bool anchored;
    };
    std::vector<RouteCell> trace;
    Ranked ways[4];
    for (int c = 0; c < 4; ++c) {
        ways[c].corner = c;
        ways[c].anchored = anchor_x_[c] >= 0;
        ways[c].end = ways[c].dist = 0;
        if (ways[c].anchored) {
            trace_route(anchor_x_[c], anchor_y_[c], trace);
            ways[c].dist = distance(anchor_x_[c], anchor_y_[c]);
            ways[c].end = trace.empty()
                              ? kUnreachable
                              : distance(trace.back().x, trace.back().y);
        }
    }
    std::sort(ways, ways + 4, [](const Ranked& a, const Ranked& b) {
        if (a.anchored != b.anchored) return a.anchored;
        if (!a.anchored) return a.corner < b.corner;
        // A way in that arrives beats one that dies against a cliff.
        const bool a_arrives = a.end <= 1, b_arrives = b.end <= 1;
        if (a_arrives != b_arrives) return a_arrives;
        if (a.end != b.end) return a.end < b.end;
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.corner < b.corner;  // the map reads NE, NW, SE, SW last
    });
    for (int c = 0; c < 4; ++c) rank_corner_[c] = ways[c].corner;
    // The anchored ways sort to the front, so the active ranks are the
    // first few of them, however many the corners could raise.
    int ranks = 0;
    for (int r = 0; r < nc && ways[r].anchored; ++r) ++ranks;
    // The wave stands up only where the walk truly comes in.
    pending_.clear();
    for (int i = 0; i < n && ranks > 0; ++i) {
        const Ranked& way = ways[i % ranks];
        const int corner = way.corner;
        const int px = anchor_x_[corner], py = anchor_y_[corner];
        // Zombies stagger out a few cells apart so they do not march in one
        // column: wave member i/ranks stands back behind the anchor, on the
        // side away from the core.
        const int back = i / ranks;
        const int away = cfg_.core_y > py ? -1 : 1;
        int sx = px, sy = py + back * away;
        if (back > 0 && !stands(sx, sy)) {
            int found = 0;
            for (int r = 0; r <= cfg_.spawn_size && !found; ++r)
                for (int dy = -r; dy <= r && !found; ++dy)
                    for (int dx = -r; dx <= r && !found; ++dx) {
                        const int wx = px + dx, wy = py + back * away + dy;
                        if (!stands(wx, wy)) continue;
                        sx = wx;
                        sy = wy;
                        found = 1;
                    }
            if (!found) { sx = px; sy = py; }
        }
        // The spawn stands on the floor the walk itself runs on - the
        // anchor's best-reached surface, not the top of the column, so a
        // wave raised beside a tunnel's mouth stands in the road, not on
        // the wall's cap above it.
        const auto stand_z = [&](int x, int y) {
            const int k = best_state(x, y);
            return surface(x, y, k < 0 ? 0 : k);
        };
        SpawnAt s{sx, sy, back > 0 ? stand_z(sx, sy) : stand_z(px, py),
                  now + i * cfg_.spawn_interval, corner};
        pending_.push_back(s);
    }
    std::sort(pending_.begin(), pending_.end(),
              [](const SpawnAt& a, const SpawnAt& b) { return a.at < b.at; });
    placed_ = static_cast<int>(pending_.size());
}

void Horde::begin_wave(double now) {
    wave_ += 1;
    phase_ = Phase::Wave;
    place_wave(now);
}

bool Horde::take_due(std::vector<SpawnAt>& out, double now) {
    out.clear();
    bool any = false;
    while (!pending_.empty() && pending_.front().at <= now) {
        out.push_back(pending_.front());
        pending_.erase(pending_.begin());
        any = true;
    }
    if (any) up_ += static_cast<int>(out.size());
    return any;
}

void Horde::on_zombie_killed() {
    if (up_ > 0) up_ -= 1;
}

void Horde::on_core_damaged(int amount) {
    core_hp_ = (std::max)(0, core_hp_ - amount);
    if (core_hp_ == 0) {
        phase_ = Phase::Defeat;
        pending_.clear();
    }
}

void Horde::restart(double now) {
    phase_ = Phase::Prep;
    wave_ = 0;
    up_ = 0;
    core_hp_ = cfg_.core_hp;
    pending_.clear();
    phase_until_ = now + cfg_.prep_seconds;
    recompute();
}

double Horde::phase_left(double now) const {
    if (phase_ == Phase::Prep || phase_ == Phase::Intermission)
        return phase_until_ - now;
    return -1.0;
}

void Horde::tick(double now) {
    if (field_dirty_ && now - field_at_ >= cfg_.field_period) {
        recompute();
        field_at_ = now;
    }
    switch (phase_) {
        case Phase::Prep:
            if (now >= phase_until_) begin_wave(now);
            break;
        case Phase::Wave:
            if (wave_cleared()) {
                if (wave_ >= cfg_.wave_count_max) {
                    phase_ = Phase::Victory;
                    break;
                }
                phase_ = Phase::Intermission;
                phase_until_ = now + cfg_.intermission_seconds;
                core_hp_ = (std::min)(cfg_.core_hp, core_hp_ + cfg_.core_mend);
            }
            break;
        case Phase::Intermission:
            if (now >= phase_until_) begin_wave(now);
            break;
        case Phase::Victory:
        case Phase::Defeat:
            break;
    }
}

}  // namespace horde
