#pragma once
// The horde's brain: waves of walking dead that head for a core the player
// defends. Ported from the mode's Lua draft (xrs_td_horde.lua) - the same
// state machine and rules - onto this server's own map, for the training
// course's host to run. The brains only: waves, the flow field and the build
// veto. Physics, players and packets stay in Server.cpp, which steers the
// zombies it owns by asking this where to step.
//
// The field is one walk from the core over the map's standing surfaces, the
// top of every solid run that leaves headroom above it, recomputed a short
// debounce after every edit; a zombie steps to the neighbouring surface whose
// distance is lowest, which is a cheapest path around whatever has been
// built since - a wall's top is one surface, and the floor of a tunnel
// through it is another, so the walk goes under as well as over, and no
// gate an arch leaves standing closes a town. For now the dead walk, and do not
// dig: the walk prices only what legs can cross, and the crossing is
// reversible - a step of kWalkClimb up or down is a jump, and anything
// more is a wall. The walk also keeps to the open: a cell hedged by a
// wall on either side of the step is priced dearer than one with a
// shoulder's room both ways, so a way the legs could hug against a wall -
// where a walker jams on its corner - is walked only where the map leaves
// no other way in, and so is a change of level: each block of climb or
// drop a crossing spends is priced dearer than a level step, so where the
// ground offers a flat way around its folds the march takes it and stays
// one pack. On open, level ground the price is one a step, as ever. The
// dead themselves still take no falling damage, but
// the plan never feeds them into a fall it cannot lead them out of - so
// the field takes the stairs and the ramps, and neither climbs nor pours
// down a cliff the map shows it. The veto
// asks the field's own question the other way round: with the cell a player
// wants to build at taken as unwalkable, is there still a route a walker
// could take from a spawn corner to the
// core?  If not, and there was one before, that block would seal the last
// way in, and is refused. When the map is already sealed (or the field is
// out of date), the veto fails open and the tower stands or falls as the
// player builds it; a sealed world stays sealed until the dead dig again,
// which is a change kept for later on purpose. The walk the field keeps can
// be read out cell by cell - trace_route, each cell with the surface it was
// walked on - which the host paints at the walk's own height, never on the
// treetops above it, so the way in can be watched while it proves itself.
#include <algorithm>
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
    // The spawn points the dead come from: the centre of each one's reach.
    // Ten of them now - the map's four corners, and six more spread along
    // its edges - so the later waves can open more fronts than the corners
    // alone. A wave takes as many of them, of the ranked order, as its
    // own number says; a point whose reach the walk never touches raises
    // nothing, whatever its number.
    static constexpr int kSpawnSites = 10;
    int spawn_x[kSpawnSites] = {32, 479, 32, 479, 181, 330, 181, 330, 32, 479};
    int spawn_y[kSpawnSites] = {32, 32, 479, 479, 32, 32, 479, 479, 255, 255};
    // The spawn box width of earlier days; the clearing is five by five now.
    int spawn_size = 12;
    double prep_seconds = 20.0;
    double intermission_seconds = 12.0;
    // Zombies trickle out, not all at once - though a wave that stands
    // around being born is a wave that looks stalled: quicker than it
    // once kept, so the horde reads as one body rising and walking.
    double spawn_interval = 0.25;
    // The rounds keep coming while the ramp still has something new to
    // show: eight waves never reached the ninth point, so twenty is the
    // count now - the points all open by ten, and the packs grow on.
    int wave_count_max = 20;
    int core_hp = 150;
    // How much of the core's health a cleared wave mends back.
    int core_mend = 25;
    // How long after an edit the field is recomputed.
    double field_period = 0.5;
    // How big a wave is: the waves open fronts as they number. A wave
    // takes one spawn point more than the one before it, as many of the
    // ranked order as the terrain holds, until the points reach their
    // cap - and past the cap it is the pack at each point that grows,
    // a few more to a point, for the ways in are as many as they ever
    // were and only the crowds on them deepen. The knobs are the
    // balance phase's to turn: the cap, the pack's base, its growth.
    // The whole of it is capped at kWaveMaxZombies, the world's own
    // budget of the dead - what the slots cannot stand is skipped at
    // the raising, one at a time, and never a wave that waits on it.
    int spawn_point_cap = kSpawnSites;
    static constexpr int kPackBase = 20;
    static constexpr int kPackGrowth = 5;
    static constexpr int kWaveMaxZombies = 250;
    int wave_points(int wave) const {
        const int cap = spawn_point_cap < 1
            ? 1 : (spawn_point_cap > kSpawnSites ? kSpawnSites : spawn_point_cap);
        const int w = (std::max)(1, wave);
        return w < cap ? w : cap;
    }
    int wave_pack(int wave) const {
        const int w = (std::max)(1, wave);
        const int cap = wave_points(wave);
        return kPackBase + kPackGrowth * (w > cap ? w - cap : 0);
    }
    int wave_zombies(int wave) const {
        return (std::min)(kWaveMaxZombies, wave_points(wave) * wave_pack(wave));
    }
    // How the dead cross: the walk is reversible - a step of at most
    // kWalkClimb, up or down, and anything more is a wall. The dead still
    // take no falling damage, but the route never feeds them into a drop
    // it cannot lead them out of: the stairs, the ramps and the notches
    // are the way, and cliffs and pits wall both ways. kFall and
    // kDigPenalty belong to the dig, turned off while the walk proves
    // itself: kFall is how deep a single fall may pour, kDigPenalty the
    // price of the spade's block; the flood still knows how to charge it,
    // and turning it back on is one flag. Rooms without headroom stay
    // unreachable.
    // z grows downward, so a climb is the negative side of the delta.
    // kWalkDrop no longer bounds a crossing: it bounds a spawn. The anchor
    // of a wave stands at its ring's own ground level, never on a perch
    // more than kWalkDrop above it - a wave begun on a canopy lands in
    // foliage its legs cannot cross.
    static constexpr int kWalkDrop = 5, kWalkClimb = 2;
    static constexpr int kFall = 63, kDigPenalty = 12;
    // A crossing that changes floor is a crossing apart: the pack walks
    // best on one level, and every block of climb or drop it spends is
    // priced at kLevelShy steps, so the flood keeps to the flat wherever
    // the map leaves a way around its hills and dips - a fold in the
    // ground is taken only where it is the crossing, and the walk around
    // it is worth kLevelShy steps a block. On level ground the price is
    // nothing: a step a cell, as ever.
    static constexpr int kLevelShy = 8;
    // How many spawn points a wave comes from is the wave's own number,
    // read at wave_points: one while the route is watched, more as the
    // walk proves itself. The points are ranked by the way the walk
    // reads into each and the wave takes the first few of that order -
    // anything else is a coin toss the terrain wins.
    // How far out from its asked-for centre a corner's spawn point may be
    // sought, in rings: the anchor of a wave is the FAR END of the walk
    // inside that reach - the frontier, wherever the borderlands stop the
    // legs - so the corners a real map names in mountains and sea only
    // point at which quarter's border the wave walks in from.
    static constexpr int kSpawnSearch = 200;
};

// One zombie due at `at`, born standing on a floor of its corner's
// clearing - the site's own floor, or one of the square's around it.
// `corner` is the corner it comes from: the wave walks as one pack per
// corner, the first out in front and the rest following it.
struct SpawnAt {
    int x, y, z;
    double at;
    int corner;
};

// One treaded cell of a route: the cell on the map the line of march is
// painted on, and the surface of it the walk truly runs on. The host builds
// its route one of these at a time, and paints at that height - never on
// the top of whatever column happens to stand over the road.
struct RouteCell {
    int x, y, z;
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
    // How many zombies the last wave could truly stand up: a corner with no
    // way in the walk can take raises nothing, and the host says so when
    // the count falls short of the wave's price.
    int placed() const { return placed_; }
    // The corner of `rank` in the order the last wave walked its corners
    // in: rank 0 is where the wave comes from, anchored, its route truly
    // ending at the core's doorstep, the shortest first. When the terrain
    // only offers a dead end, that reads as the rank's corner too - the
    // walk takes whatever way the map leaves it.
    int ranked_corner(int rank) const {
        if (rank < 0 || rank > Config::kSpawnSites - 1) return 0;
        return rank_corner_[rank];
    }
    // The settings this runs with, for the host's announcements.
    const Config& config() const { return cfg_; }

    // The cells of the zombies due by `now`, taken out of the queue.
    bool take_due(std::vector<SpawnAt>& out, double now);
    // A zombie of the walking count went down.
    void on_zombie_killed();
    // The core took a hit. Its health floors at zero and Defeat follows.
    void on_core_damaged(int amount);
    // A wave wiped by the host's own hand: nothing owed, nothing walking,
    // the core mended whole - and a Defeat called off with it, the
    // waiting back, so the next wave comes when the clock has it.
    // Victory is held: the rounds the settings name are the rounds kept.
    void clear_wave(double now);
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
    // The same advice for a walker whose feet are at z: the read is taken
    // from the floor the body truly stands on, not the cell's cheapest one
    // - which can be a tunnel's floor the legs can never reach from where
    // they are, and a walker sent chasing it paces between the cell it
    // holds and the cell it is told to want, forever. Every neighbour's
    // candidate floor is measured against THAT footing, so the step
    // handed back is one the legs can make without letting go of the
    // ground. False on the same terms as the two-argument read; the host
    // falls back to that one when the feet are in the air or on a floor
    // the walk has no opinion about.
    bool next_step(int x, int y, int z, int& tx, int& ty);
    // Whether a cell is within `cells` of the core, where the zombies bite.
    bool near_core(int x, int y, int cells) const;

    // The anchor the last wave walked in from at `corner`: the far end of
    // the walk within kSpawnSearch rings of the corner's asked-for centre,
    // at its own ground level, kept from the wave's placing. False when
    // the corner's reach holds no footing (a corner with no anchor raises
    // nothing).
    bool corner_anchor(int corner, int& x, int& y) const;
    // The way in from a cell, read off the walk: the cells one at a time,
    // each a legal crossing downhill, until the core's doorstep, each with
    // the surface the walk runs on it at. The route handed back is the
    // pack's line of march - the host paints it so the way can be watched.
    // False when the cell is not on the walk at all.
    bool trace_route(int x, int y, std::vector<RouteCell>& out);

    // For tests: the field's distance from the core, or -1 unreachable.
    int distance(int x, int y) const;
    // The field's distance of the floor a body at z stands on - the walk's
    // own measure of whether a walker is getting anywhere, read at the
    // feet. The cell's cheapest floor can be one the legs never touch;
    // this one is the floor they are on, or -1 when no floor of the
    // column near z is on the walk.
    int distance(int x, int y, int z) const;

private:
    // The most standing surfaces one column may carry: a wall's top and the
    // floor of the tunnel through it are two; anything stacking deeper than
    // this is not stood on today.
    static constexpr int kSurfMax = 4;

    // The clearing a wave is born in: this many cells each way around the
    // site (so five by five), four voxels high.
    static constexpr int kRoomHalf = 2;

    void recompute();
    int ground(int x, int y);
    // One cell's standing surfaces: every solid run top with the two voxels
    // above it open. fill_surfaces scans the column once; the rest read the
    // cache. surface() gives the k-th surface's z, surf_count how many the
    // column carries; best_state gives the surface with the lowest field
    // distance (or -1 when the cell is off the walk).
    void fill_surfaces(int x, int y);
    int surf_count(int x, int y);
    int surface(int x, int y, int k);
    int best_state(int x, int y) const;
    // The standing surface of the column the walk has reached that meets
    // the feet at z: of the floors that carry the body, the one this side
    // of its soles. -1 when no floor the field reaches is near the
    // asking height. Ties keep the cheaper floor.
    int near_state(int x, int y, int z) const;
    // A body's standing room at the given floor: the walk demands two open
    // voxels, the dead stand four tall and ask leave to step. A birthplace
    // asks it of its own column and of every side, so no wave member rises
    // inside a wall.
    bool clear_of(int x, int y, int z) const;
    // The clearing a wave is born in: five cells square, four voxels
    // high, every column of the square open above the floor at z. The
    // road names its site; the square gives the wave its room - the map
    // has no voxels past its edges, so a border cell's missing side is sky.
    bool box_clear(int x, int y, int z) const;
    bool reach_spawn(const std::vector<int32_t>& dist) const;
    void flood(std::vector<int32_t>& dist, int block_x, int block_y, bool dig);
    // The anchors of the four corners and the ranking of their ways in:
    // the reading the waves place themselves by, and the one the host
    // paints the way in from while the world waits or walks. place_wave
    // asks it before the wave stands up; tick keeps it fresh through the
    // waiting phases, wherever the field under it was recomputed.
    void scout();
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
    // The field: a distance per standing surface (kSurfMax of them a cell),
    // and the z of every surface the columns carry, scanned once a recompute
    // (surf_n_ of -1: this column is not scanned yet).
    std::vector<int32_t> dist_;
    std::vector<int16_t> surf_z_;
    std::vector<int8_t> surf_n_;
    bool field_dirty_ = false;
    double field_at_ = 0.0;
    // Whether the anchors and the ranking still stand on the field as it
    // was last recomputed: false once the field moved, scouted again by
    // the next tick's waiting phase or the next wave's placing.
    bool scouted_ = false;
    // Where the wave last walked in from, one anchor a spawn point (-1
    // for none): the route the host paints is traced from here.
    int anchor_x_[Config::kSpawnSites] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    int anchor_y_[Config::kSpawnSites] = {0};
    // The spawn points in the order the last wave walked them (rank 0
    // first), and how many zombies that wave truly stood up.
    int rank_corner_[Config::kSpawnSites] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    int placed_ = 0;
};

}  // namespace horde
