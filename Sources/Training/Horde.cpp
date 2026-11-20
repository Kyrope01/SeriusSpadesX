// The horde's brain, the body of it. See Horde.h for what it is and how the
// veto, the field and the waves decide; the comments here are about the how.

#include "Horde.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

// Bits above the ground: a walker is just under two cells tall, so the two
// voxels above the ground must be open. z counts down: the ground is the
// topmost solid voxel, the sky above it the lower z. SolidMap's columns are
// 64-bit words, bit z set where solid.
int ground_of(uint64_t col) {
    if (col == 0) return -1;  // a column of nothing: no ground to stand on
    // The topmost solid z: the least significant set bit.
#if defined(_MSC_VER)
    unsigned long z = 0;
    _BitScanForward64(&z, col);
    return static_cast<int>(z);
#else
    return static_cast<int>(__builtin_ctzll(col));
#endif
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
    return dist_[cell_index(x, y)];
}

// --- the ground and the walkable cells --------------------------------------

// Cached on the flood of the field, filled lazily elsewhere: ground levels
// only change when the map does, which flags the field dirty and re-floods
// soon after; a level asked for between an edit and the refresh serves a
// step a tick early, nothing worse.
int Horde::ground(int x, int y) {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return -1;
    if (ground_[cell_index(x, y)] != -2) return ground_[cell_index(x, y)];
    const int g = ground_of(map_.column(x, y));
    ground_[cell_index(x, y)] = static_cast<int16_t>(g);
    return g;
}

bool Horde::walkable(int x, int y) {
    const int g = ground(x, y);
    if (g < 2) return false;  // no ground, or no headroom above it
    const uint64_t col = map_.column(x, y);
    return ((col >> (g - 1)) & 3u) == 0;  // the two voxels above are open
}

// --- the field ---------------------------------------------------------------

// A BFS from the core over walkable cells whose grounds differ by at most a
// step: the distance of every cell the walking dead could cross. `block_x`
// and `block_y` name one column held out of the walk, the veto's candidate;
// -1 for none. 4-neighbours: corners are where zombies jam on pillars, not
// where they cross.
void Horde::flood(std::vector<int32_t>& dist, int block_x, int block_y) {
    dist.assign(static_cast<size_t>(kMapW) * kMapH, kUnreachable);
    const int cx = cfg_.core_x, cy = cfg_.core_y;
    if (cx < 0 || cx >= kMapW || cy < 0 || cy >= kMapH) return;
    if (block_x == cx && block_y == cy) return;
    if (!walkable(cx, cy)) return;
    std::vector<int32_t> queue;
    queue.reserve(4096);
    queue.push_back((cy << 9) | cx);  // x < 512 fits beside y in an int
    dist[cell_index(cx, cy)] = 0;
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const int x = queue[qi] & 511, y = queue[qi] >> 9;
        const int d = dist[cell_index(x, y)];
        const int g = ground(x, y);
        for (int k = 0; k < 4; ++k) {
            const int nx = x + kDx[k], ny = y + kDy[k];
            if (nx < 0 || nx >= kMapW || ny < 0 || ny >= kMapH) continue;
            if (nx == block_x && ny == block_y) continue;
            if (dist[cell_index(nx, ny)] != kUnreachable) continue;
            if (!walkable(nx, ny)) continue;
            if (std::abs(ground(nx, ny) - g) > 1) continue;  // a step, not a ledge
            dist[cell_index(nx, ny)] = d + 1;
            queue.push_back((ny << 9) | nx);
        }
    }
}

// Is any cell of any spawn corner's box on the field?
bool Horde::reach_spawn(const std::vector<int32_t>& dist) const {
    for (int k = 0; k < 4; ++k) {
        const int sx = cfg_.spawn_x[k], sy = cfg_.spawn_y[k];
        const int n = cfg_.spawn_size;
        int found = 0;
        for (int dy = -n / 2; dy <= n / 2 && !found; ++dy)
            for (int dx = -n / 2; dx <= n / 2; ++dx) {
                const int x = sx + dx, y = sy + dy;
                if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) continue;
                if (dist[cell_index(x, y)] != kUnreachable) {
                    found = 1;
                    break;
                }
            }
        if (found) return true;
    }
    return false;
}

void Horde::recompute() {
    ground_.assign(static_cast<size_t>(kMapW) * kMapH, -2);  // -2: not yet known
    flood(dist_, -1, -1);
    field_dirty_ = false;
}

void Horde::on_map_edited() { field_dirty_ = true; }

// --- walking ----------------------------------------------------------------

bool Horde::next_step(int x, int y, int& tx, int& ty) {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return false;
    if (!walkable(x, y)) return false;
    const int here = dist_[cell_index(x, y)];
    static const int kDx[4] = {1, -1, 0, 0};
    static const int kDy[4] = {0, 0, 1, -1};
    int best = -1, bx = -1, by = -1;
    for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k], ny = y + kDy[k];
        if (nx < 0 || nx >= kMapW || ny < 0 || ny >= kMapH) continue;
        const int nd = dist_[cell_index(nx, ny)];
        if (nd < 0) continue;
        if (here >= 0 && nd >= here) continue;  // downhill only
        if (best < 0 || nd < best) {
            best = nd;
            bx = nx;
            by = ny;
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
        if (sx != 0 && walkable(x + sx, y)) { tx = x + sx; ty = y; return true; }
        if (sy != 0 && walkable(x, y + sy)) { tx = x; ty = y + sy; return true; }
        if (sx != 0 && walkable(x, y + (cy > y ? 1 : -1))) {
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

// --- the veto ----------------------------------------------------------------

// Two floods, one with the candidate column held out. The map is not touched
// here: the candidate only hides that column from the walk. The field comes
// out of it freshly computed either way (the same work the debounce would
// soon do), so a refused block leaves a working field for the next question.
bool Horde::can_build(int x, int y, int z) {
    if (x < 0 || x >= kMapW || y < 0 || y >= kMapH) return true;
    if (z < 0 || z >= SolidMap::SIZE_Z) return true;
    std::vector<int32_t> with_block;
    flood(with_block, x, y);
    std::vector<int32_t> without_block;
    flood(without_block, -1, -1);
    dist_ = without_block;  // refreshed while we are here
    field_dirty_ = false;   // its own ground cache, its own flood: consistent
    if (reach_spawn(with_block)) return true;
    // Fail open: sealed before this block too - the tower stands or falls on
    // its own, refusing nothing that changes nothing.
    if (!reach_spawn(without_block)) return true;
    // This block would have sealed the last way in.
    return false;
}

// --- the waves ----------------------------------------------------------------

void Horde::place_wave(double now) {
    const int n = cfg_.wave_zombies(wave_);
    pending_.clear();
    for (int i = 0; i < n; ++i) {
        const int corner = i % 4;
        const int cx = cfg_.spawn_x[corner], cy = cfg_.spawn_y[corner];
        // The first walkable cell of the corner's box, scanning out from its
        // centre: deterministic, so a wave is reproducible in tests.
        int px = -1, py = -1, pg = -1;
        for (int r = 0; r <= cfg_.spawn_size / 2 && px < 0; ++r)
            for (int dy = -r; dy <= r && px < 0; ++dy)
                for (int dx = -r; dx <= r && px < 0; ++dx) {
                    const int x = cx + dx, y = cy + dy;
                    if (!walkable(x, y)) continue;
                    px = x;
                    py = y;
                    pg = ground(x, y);
                }
        if (px < 0) continue;  // a corner with nowhere to stand: skip it
        // Zombies stagger out a few cells apart so they do not march in one
        // column: wave member i/4 stands back behind its corner's first cell,
        // on the side away from the core.
        const int back = i / 4;
        const int away = cfg_.core_y > cy ? -1 : 1;
        SpawnAt s{px, py + back * away, pg, now + i * cfg_.spawn_interval};
        pending_.push_back(s);
    }
    std::sort(pending_.begin(), pending_.end(),
              [](const SpawnAt& a, const SpawnAt& b) { return a.at < b.at; });
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
