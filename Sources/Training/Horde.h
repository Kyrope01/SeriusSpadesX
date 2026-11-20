#pragma once
// The horde's brain: waves of walking dead that head for a core the player
// defends. Ported from the mode's Lua draft (xrs_td_horde.lua) - the same
// state machine and rules - onto this server's own map, for the training
// course's host to run. The brains only: waves, the flow field and the build
// veto. Physics, players and packets stay in Server.cpp, which steers the
// zombies it owns by asking this where to step.
//
// The field is one walk from the core over walkable cells (its ground level
// and the headroom above), recomputed a short debounce after every edit; a
// zombie steps to the neighbour whose distance is lowest, which is a cheapest
// path around whatever has been built since. That walk prices a dig: a climb
// beyond the jump costs the blocks the spade must take off, so the field
// prefers the way round and only leads into walls thin enough to matter. The
// veto asks the field's stricter question the other way round: with the cell
// a player wants to build at taken as unwalkable, is there still a route a
// jump-fearing, fall-counting walker could take from a spawn corner to the
// core?  If not, and there was one before, that block would seal the last
// way in, and is refused. When the map is already sealed (or the field is
// out of date), the veto fails open and the tower stands or falls as the
// player builds it; the horde's own teeth are not the strict walk, and a
// sealed world is one the spades open eventually.
#include <cstdint>
#include <vector>

class SolidMap;

namespace horde {

enum class Phase {
    Prep,          // before wave one: build, while the field is fresh
    Wave,          // zombies walking
    Intermission,  // a wave cleared: the core mends a little
    Victory,       // the last wave cleared
    Defeat         // the core fell
};

struct Config {
    // The cell the zombies walk to.
    int core_x = 256, core_y = 256;
    // The corners zombies come from: the centre of each corner's spawn box.
    int spawn_x[4] = {32, 479, 32, 479};
    int spawn_y[4] = {32, 32, 479, 479};
    int spawn_size = 12;  // a spawn box is this many cells square
    double prep_seconds = 20.0;
    double intermission_seconds = 12.0;
    // Zombies trickle out, not all at once.
    double spawn_interval = 0.35;
    int wave_count_max = 8;
    int core_hp = 150;
    // How much of the core's health a cleared wave mends back.
    int core_mend = 25;
    // How long after an edit the field is recomputed.
    double field_period = 0.5;
    // How big a wave is: base + growth * (wave - 1), capped.
    int wave_zombies(int wave) const;
    static constexpr int kWaveBase = 12;
    static constexpr int kWaveGrowth = 8;
    static constexpr int kWaveMaxZombies = 100;
    // How the dead cross: a climb of at most kWalkClimb is jumped and a drop
    // of at most kWalkDrop walked - the strict crossing the build veto asks
    // with. The field the zombies actually walk prices more: a fall of at
    // most kFall, for they fear no landing, and any climb, with every block
    // over the jump costing kDigPenalty steps - the spade that eventually
    // digs it takes about that long - so the easy way round is walked first
    // and a wall is only dug through when it is thin. Cliffs taller than the
    // map and rooms without headroom stay unreachable either way. z grows
    // downward, so a climb is the negative side of the delta.
    static constexpr int kWalkDrop = 5, kWalkClimb = 2;
    static constexpr int kFall = 63, kDigPenalty = 12;
    // How far out from its asked-for centre a corner's spawn point may be
    // sought, in rings: the corners a real map names sit in border mountains
    // and sea, and only the field decides where a wave can walk in from.
    static constexpr int kSpawnSearch = 200;
};

// One zombie due at `at`, at a cell that was walkable when its wave began.
// `corner` is the spawn box it comes from: the wave walks as one pack per
// corner, the first out in front and the rest following it.
struct SpawnAt {
    int x, y, z;
    double at;
    int corner;
};

class Horde {
public:
    Horde(const SolidMap& map, const Config& cfg);

    // The clock the host runs on (world_time, seconds). Moves the phases and
    // recomputes the field when it is stale.
    void tick(double now);

    Phase phase() const { return phase_; }
    int wave() const { return wave_; }
    // Seconds until the current phase ends; -1 when it is not a countdown.
    double phase_left(double now) const;
    // Zombies of this wave that are walking or waiting, as the brain counts
    // them: raised by every spawn taken, lowered by every kill the host
    // reports.
    int zombies_up() const { return up_; }
    // Wave done when nothing waits and nothing walks.
    bool wave_cleared() const { return pending_.empty() && up_ <= 0; }
    int core_hp() const { return core_hp_; }
    // The settings this runs with, for the host's announcements.
    const Config& config() const { return cfg_; }

    // The cells of the zombies due by `now`, taken out of the queue.
    bool take_due(std::vector<SpawnAt>& out, double now);
    // A zombie of the walking count went down.
    void on_zombie_killed();
    // The core took a hit. Its health floors at zero and Defeat follows.
    void on_core_damaged(int amount);
    // Everything from the beginning: Prep again, the field as it stands.
    void restart(double now);

    // Whether a block at (x, y, z) may be built: false only when it would
    // seal every route from a spawn corner to the core.
    bool can_build(int x, int y, int z);
    // The map changed (built or destroyed): the field follows, debounced.
    void on_map_edited();

    // Where a zombie at cell (x, y) should step: the neighbour of the lowest
    // field distance, which heads to the core around obstacles. False when
    // no step helps (at the core, or nowhere reachable); a straight step
    // towards the core is tried first as the fallback, so a broken field
    // walk is a slow walk, not a standstill.
    bool next_step(int x, int y, int& tx, int& ty);
    // Whether a cell is within `cells` of the core, where the zombies bite.
    bool near_core(int x, int y, int cells) const;

    // For tests: the field's distance from the core, or -1 unreachable.
    int distance(int x, int y) const;

private:
    void recompute();
    bool walkable(int x, int y);
    int ground(int x, int y);
    bool reach_spawn(const std::vector<int32_t>& dist) const;
    void flood(std::vector<int32_t>& dist, int block_x, int block_y, bool dig);
    void begin_wave(double now);
    void place_wave(double now);

    const SolidMap& map_;
    Config cfg_;
    Phase phase_ = Phase::Prep;
    int wave_ = 0;
    int up_ = 0;
    int core_hp_;
    double phase_until_ = 0.0;
    std::vector<SpawnAt> pending_;
    // The field: a distance per cell, and the ground level it was figured on.
    std::vector<int32_t> dist_;
    std::vector<int16_t> ground_;
    bool field_dirty_ = false;
    double field_at_ = 0.0;
};

}  // namespace horde
