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
// path around whatever has been built since. For now the dead walk and do not
// dig: the walk prices only what legs can cross - a climb of kWalkClimb is a
// jump and a drop of kWalkDrop a fall, and anything more is a wall - so the
// field finds the stairs and the ramps, the way every walker takes. The veto
// asks the field's own question the other way round: with the cell a player
// wants to build at taken as unwalkable, is there still a route a
// jump-fearing, fall-counting walker could take from a spawn corner to the
// core?  If not, and there was one before, that block would seal the last
// way in, and is refused. When the map is already sealed (or the field is
// out of date), the veto fails open and the tower stands or falls as the
// player builds it; a sealed world stays sealed until the dead dig again,
// which is a change kept for later on purpose. The walk the field keeps can
// be read out cell by cell - trace_route - which the host paints on the
// ground so the way in can be watched while it proves itself.
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
    // of at most kWalkDrop walked. That is the field the zombies walk now -
    // no spade in it: they take the stairs, the ramps and the notches, and a
    // wall more than a jump stands. kFall and kDigPenalty are the prices of
    // the dig that is turned off while the walk proves itself; the flood
    // still knows how to charge them, and turning them back on is one flag.
    // Cliffs taller than a fall and rooms without headroom stay unreachable.
    // z grows downward, so a climb is the negative side of the delta.
    static constexpr int kWalkDrop = 5, kWalkClimb = 2;
    static constexpr int kFall = 63, kDigPenalty = 12;
    // How many of the four corners a wave comes from: one while the route
    // is watched, more when the walk has proved itself. The corners
    // beyond it stay in the config but raise nothing.
    int spawn_corners = 1;
    int active_corners() const {
        return spawn_corners < 1 ? 1 : (spawn_corners > 4 ? 4 : spawn_corners);
    }
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

    // One treaded cell of a route: the cell and the ground of it. The
    // ground is the top solid voxel of the column, z counting down.
    struct RouteCell {
        int x, y;
    };
    // The anchor the last wave walked in from at `corner`: the nearest
    // cell of the walk to the corner's asked-for centre, kept from the
    // wave's placing. False when the corner never found its footing (a
    // corner with no anchor raises nothing).
    bool corner_anchor(int corner, int& x, int& y) const;
    // The way in from a cell, read off the walk: the cells one at a time,
    // each a legal crossing downhill, until the core's doorstep. The route
    // handed back is the pack's line of march - the host paints it so the
    // way can be watched. False when the cell is not on the walk at all.
    bool trace_route(int x, int y, std::vector<RouteCell>& out);

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
    // Where the wave last walked in from, one anchor a corner (-1 for none):
    // the route the host paints is traced from here.
    int anchor_x_[4] = {-1, -1, -1, -1};
    int anchor_y_[4] = {0};
};

}  // namespace horde
