#include "Server.h"

#include "Grenade.h"
#include "Horde.h"
#include "MapGen.h"
#include "Physics.h"
#include "Protocol.h"
#include "SolidMap.h"
#include "TrainingCourse.h"
#include "TrainingModes.h"
#include "Vec3.h"
#include "VoxelMap.h"

#include <enet/enet.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace host {

namespace {

using proto::ByteReader;
using proto::ByteWriter;
using proto::PacketId;

// pyspades/constants.py, the ones this file uses and their neighbours (kept
// whole so a row reads like the original).
[[maybe_unused]] constexpr int kWeaponKill = 0, kHeadshotKill = 1, kMeleeKill = 2, kGrenadeKill = 3,
                               kFallKill = 4, kTeamChangeKill = 5, kClassChangeKill = 6;
[[maybe_unused]] constexpr int kBuildBlock = 0, kDestroyBlock = 1, kSpadeDestroy = 2, kGrenadeDestroy = 3;
constexpr int kToolSpade = 0, kToolBlock = 1, kToolWeapon = 2;
[[maybe_unused]] constexpr int kTorso = 0, kHead = 1, kArms = 2, kLegs = 3, kMelee = 4;
constexpr int kErrorWrongVersion = 3, kErrorFull = 4;
constexpr float kMaxBlockDistance = 6.0f;
constexpr float kMaxDigDistance = 6.0f;
constexpr float kMeleeDistance = 3.0f;
constexpr int kBuildTolerance = 5;
constexpr int kMeleeDamage = 100;
// Player ids are a byte and 255 means nobody.
constexpr int kSlots = 255;
constexpr double kTick = 1.0 / 60.0;
constexpr size_t kMapChunk = 8192;
// Teamplay: how long a relayed ping lasts, and how often one player may send.
constexpr float kPingDuration = 10.0f;
constexpr double kPingInterval = 1.0;
// A target back on its lane after it falls.
constexpr float kTargetRespawn = 2.0f;
// On the course a person is always blue; the targets are green.
constexpr int kPeople = 0, kTargets = 1;
// A zombie fresh out of the pile walks through its own for this long,
// until the wave has settled into single file. Two is enough: they are
// born a cell apart in the clearing, and the longer window only ever
// bought the pile its twenty seconds of standing still.
constexpr double kZombieSettle = 2.0;
// The dead's own answer to the walk's two-block step - a crossing the
// field prices as one jump, though the hop it gives on its own rises a
// block and a half and dies pressed against the face it meant to clear.
// A stuck walker flings itself high enough to stand on what it could
// not climb: the vault the reading promised it, paid for in one lunge.
constexpr float kDeadPounce = -0.45f;
constexpr double kPounceWindup = 1.0;    // this long stuck before the first fling
constexpr double kPounceAgain = 1.2;     // and this long between two of them
// Half-second windows a leader may pace without reaching a lower floor of
// the walk before the pacing itself is read as stuck. The yo-yo moves -
// a half-second's worth in every window - so the moved measure never
// calls it; the walk's distance read at the feet does. Three
// seconds of going nowhere by the field's own count, and the hop and
// the fling the standstill gets are let loose again.
constexpr int kZombieYoyoWindows = 6;
// The beat the way in's vapour is laid afresh on: the reading re-read,
// the beds re-said, an obstacle answered within the beat - the trail
// follows the ground, and the ground is moved on while the walk is on.
constexpr double kRouteBlink = 0.8;
// The swing of its colour: one held this long, then the other, read off
// the clock so every send agrees with the last. Slower than it first
// swung: a trail that changes its mind at half this pace reads
// steadier, heavier - a lantern, not a strobe.
constexpr double kRouteColour = 1.6;

// The catch of the pack: a follower runs once its leader stands more than
// this far ahead. Every heel of the line walks at the same pace as the one
// it holds, so the ground never closes a gap on its own - whatever a hop, a
// fling or a wave's first streaming out of the pile put between two walkers
// stays between them, and the column rides strung out at whatever spacing
// history happened to leave it. The run is the physics' own sprint, a third
// again the walk: the leeway closes at that rate while the column keeps its
// pace, and inside the leeway the follower walks the trail as it always has.
// A run cannot climb - the sprint spends the step a walker otherwise takes
// in its stride - so it is kept for the floor its ahead stands on as well:
// at a stair, a bank, a sill, the pack crosses the way it always did.
constexpr float kPackCatch = 0.8f;

// A print stepped past at this width of its side is trodden too. The
// half-pace disc catches a walker that comes to its print - but a heel at
// a run carries its own turning: it can cross the trail's end skimming
// past the disc without entering it, and a print it has already overtaken,
// standing behind it now, is owed a step of doubling back - a zigzag in
// the open that spends the whole run's gain and more. A print the walker
// has passed inside this width of its plane is spent like one stood on.
// The footsteps keep their exactness where it matters: a wall is met
// before its print's plane is crossed, so the print on the doorway's far
// side stays owed until the door is truly walked through.
constexpr float kPrintShoulder = 1.0f;

// The reach a print is spent within - the width of the miss a walker may
// make and still have paid for it. A trail bends, and the one that takes
// a bend at a run carries its turn wide of the very print the bend stood
// on: within half a pace of it the leg never came, and outside the
// shoulder's plane the leg never crossed, so the print stood owed
// behind a walker already past it - a step of doubling back, then
// written into that walker's own prints for every heel behind it to
// walk over again, the one zigzag become the whole line's. So the
// half-pace disc was a stamp collector's: exact, and exacting. The
// print is spent within this reach instead, and the miss a bend made
// costs the walk nothing. The prints keep their exactness where it
// counts: the trail still falls every half a pace, and the doorway's
// print stands a pace past the wall's far face, so a door is walked
// through still, and not crossed at the wall.
constexpr float kPrintTouch = 0.7f;

// A print owed abreast of the walker, or behind it, is the print of a
// miss - a bend carried wide, a bank that threw the leg off the trail -
// and paying it in full costs the line the walk back and the walk again
// over the same ground, both printed for every heel behind to replay:
// one miss become the whole line's zigzag. So the print is aimed at
// only while it stands ahead of the walker by this much of the bearing
// to the one it follows; owed at less, the walker has strayed, and it
// is the field that leads it until the line takes it back.
constexpr float kPrintAside = 0.5f;

// The heel's own sight: a follower this close to the one it follows is
// not stuck - it is parked. The line's heel holds its place by stirring
// on the spot, a print owed it every half a pace, and the held heel is
// the shape the line itself has settled into. The window, reading that
// stirring as a standstill, hopped it on the spot and flung it through
// the backs of its own - the vault that clears a wall shoving aside a
// walker that was standing where it belonged - and the line zigged and
// zagged at the price of all the run had gained on it. So within this
// sight of the one ahead the window is shelved: the follower parks, and
// its ahead walks on past the sight the window takes it back - the first
// hop and then the vault, for a wall behind a heel still gives way.
constexpr float kHeelSight = 3.0f;

// The number of the next heel print: from one, so a zero marks an empty
// slot and never a print. The count outlives every walk the world keeps.
uint32_t heel_stamp() {
    static uint32_t n = 0;
    if (++n == 0) n = 1;
    return n;
}

double now_sec() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

bool finite3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool near3(const Vec3& a, const Vec3& b, float d) {
    return std::fabs(a.x - b.x) < d && std::fabs(a.y - b.y) < d && std::fabs(a.z - b.z) < d;
}

// --- the guns: pyspades/weapon.py -------------------------------------------
//
// The magazine is modelled because the replies depend on it: a reload that
// finishes is announced to its owner with the new clip and reserve, and that
// packet is how the client learns the reload is over.
struct GunSpec {
    float delay;
    int clip, stock;
    float reload_time;
    bool slow;
    int damage[4];
};
constexpr GunSpec kGuns[3] = {
    {0.5f, 10, 50, 2.5f, false, {49, 100, 33, 33}},    // rifle
    {0.11f, 30, 120, 2.5f, false, {29, 75, 18, 18}},   // SMG
    {1.0f, 6, 48, 0.5f, true, {27, 37, 16, 16}},       // shotgun
};
constexpr int kClipTolerance = 10;

struct Gun {
    int type = 0;
    int ammo = 10, stock = 50;
    // Rounds fired, ever, for the aim scenarios' accuracy (see fired_by).
    int fired = 0;
    bool shoot = false, reloading = false;
    double shoot_time = 0, next_shot = 0, reload_at = 0;

    const GunSpec& spec() const { return kGuns[type]; }
    void reset() {
        shoot = false;
        reloading = false;
        ammo = spec().clip;
        stock = spec().stock;
    }
    void restock() { stock = spec().stock; }
    int get_ammo(double now, bool no_max = false) const {
        int a = ammo;
        if (shoot) a = ammo - std::max(0, static_cast<int>(std::ceil((now - shoot_time) / spec().delay)));
        return no_max ? a : std::max(0, a);
    }
    bool is_empty(double now) const { return get_ammo(now, true) < -kClipTolerance || !shoot; }
    void set_shoot(bool v, double now) {
        if (v == shoot) return;
        if (v) {
            if (ammo <= 0) return;
            if (reloading && !spec().slow) return;
            shoot_time = std::max(now, next_shot);
            reloading = false;
        } else {
            const int before = ammo;
            ammo = get_ammo(now, true);
            next_shot = shoot_time + spec().delay * (before - ammo);
            // No more than the magazine held: a trigger held on an empty one
            // fires nothing.
            fired += std::min(std::max(0, before), std::max(0, before - ammo));
        }
        shoot = v;
    }
    // Rounds fired by `now`, those of a trigger still held included.
    int fired_by(double now) const {
        if (!shoot) return fired;
        return fired + std::min(std::max(0, ammo), std::max(0, ammo - get_ammo(now, true)));
    }
    void reload(double now) {
        if (reloading) return;
        const int a = get_ammo(now);
        if (!stock || a >= spec().clip) return;
        if (spec().slow && shoot && a) return;
        set_shoot(false, now);
        reloading = true;
        ammo = a;
        reload_at = now + spec().reload_time;
    }
    // True when a reload step finished just now: the owner is told.
    bool update(double now) {
        if (!reloading || now < reload_at) return false;
        reloading = false;
        if (spec().slow) {
            ammo += 1;
            stock -= 1;
            reload(now);
        } else {
            const int left = std::max(0, stock - (spec().clip - ammo));
            ammo += stock - left;
            stock = left;
        }
        return true;
    }
};

// world_c.cpp cube_line: the block line every client draws, voxel for voxel.
std::vector<Cell> cube_line(int x1, int y1, int z1, int x2, int y2, int z2) {
    std::vector<Cell> out;
    Cell c{x1, y1, z1};
    const int ddx = x2 - x1, ddy = y2 - y1, ddz = z2 - z1;
    const int sx = ddx < 0 ? -1 : 1, sy = ddy < 0 ? -1 : 1, sz = ddz < 0 ? -1 : 1;
    const long kFar = 0x3fffffff / 512;
    long dx, dy, dz, dxi, dyi, dzi;
    auto la = [](long v) { return v < 0 ? -v : v; };
    if (la(ddx) >= la(ddy) && la(ddx) >= la(ddz)) {
        dxi = 1024; dx = 512;
        dyi = !ddy ? kFar : la(ddx * 1024L / ddy); dy = dyi / 2;
        dzi = !ddz ? kFar : la(ddx * 1024L / ddz); dz = dzi / 2;
    } else if (la(ddy) >= la(ddz)) {
        dyi = 1024; dy = 512;
        dxi = !ddx ? kFar : la(ddy * 1024L / ddx); dx = dxi / 2;
        dzi = !ddz ? kFar : la(ddy * 1024L / ddz); dz = dzi / 2;
    } else {
        dzi = 1024; dz = 512;
        dxi = !ddx ? kFar : la(ddz * 1024L / ddx); dx = dxi / 2;
        dyi = !ddy ? kFar : la(ddz * 1024L / ddy); dy = dyi / 2;
    }
    if (sx >= 0) dx = dxi - dx;
    if (sy >= 0) dy = dyi - dy;
    if (sz >= 0) dz = dzi - dz;
    for (;;) {
        out.push_back(c);
        if (out.size() >= 64) return out;
        if (c.x == x2 && c.y == y2 && c.z == z2) return out;
        if (dz <= dx && dz <= dy) {
            c.z += sz;
            if (c.z < 0 || c.z >= 64) return out;
            dz += dzi;
        } else if (dx < dy) {
            c.x += sx;
            if (c.x < 0 || c.x >= 512) return out;
            dx += dxi;
        } else {
            c.y += sy;
            if (c.y < 0 || c.y >= 512) return out;
            dy += dyi;
        }
    }
}

std::string sanitise_name(std::string s) {
    std::string out;
    for (char ch : s) {
        if (ch == '%' || ch == '#' || ch == '\r' || ch == '\n' || ch == 0) continue;
        out.push_back(ch);
    }
    if (out.size() > 15) out.resize(15);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

struct Player {
    ENetPeer* peer = nullptr;
    int id = -1;
    // Map transfer: the compressed map, how much is sent, and what was
    // broadcast meanwhile that this player must see after it.
    std::shared_ptr<const std::vector<uint8_t>> map_data;
    // The map revision this player needs at least: edits after it reach them
    // through `saved`, edits before it must be in the snapshot they are sent.
    uint64_t need_rev = 0;
    size_t map_off = 0;
    bool loading = false;
    std::vector<std::vector<uint8_t>> saved;

    bool named = false;
    std::string name;
    int team = -2;          // -2 not chosen yet, -1 spectator, 0 blue, 1 green
    int weapon = 0;
    int tool = kToolWeapon;
    uint32_t color = 0x707070;   // 0xRRGGBB
    int kills = 0;

    bool alive = false;
    int hp = 0;
    int grenades = 3, blocks = 50;
    Gun gun;
    physics::Body body;
    physics::Input input;
    bool primary = false, secondary = false;
    Vec3 line_start{};
    bool line_started = false;
    double respawn_at = -1;
    double last_ping = -1e9;

    bool client_info = false;
    // A target the server owns and walks along this lane of the range itself
    // (no connection behind it); -1 for everybody else.
    int lane = -1;
    bool to_far = true;      // which end of the lane it is walking to
    bool ext_teamplay = false;
    bool ext_flashlight = false;   // claimed Flashlight (0x32)
    bool ext_outfit = false;      // claimed Player Outfit (0x35)
    bool ext_smoke = false;       // claimed Smoke (0x3F)
    // The walk and jump flags the zombies' clients were last told, and the
    // bite whose swing packet has yet to be closed. Real players tell the
    // clients these things themselves; the horde is told for them.
    uint8_t net_input = 0;
    bool swing_shown = false;
    double swing_until = 0.0;

    // Aim: a target of the scenario, which the server moves itself.
    bool aim_bot = false;
    int far_lane = -1;             // Long range's lane
    bool has_spawn_at = false;     // where it comes back: a ground voxel
    Vec3 spawn_at{};
    double appeared_at = 0;        // when it last spawned, for the time to kill
    double respawn_delay = 0;
    int strafe = 0;                // -1, 0 or 1: which way it is stepping
    double strafe_until = 0, next_jump = 0, recover_at = 0;
    float home_y = 0;
    // A person: when they were last restocked, and the shot their last hit
    // belonged to (a shotgun's pellets arrive together).
    double last_restock = -1e9;
    double last_hit_at = -1e9;
    bool head_this_shot = false;
    // Tutorial: the steps done (bits of training::Step) and the one pinged.
    int steps_done = 0;
    int pinged_step = -1;
    bool lamp = false;             // its flashlight is lit

    // Horde: one of the wave's walkers. Horde.h holds the brain, this holds
    // its legs and its teeth.
    bool zombie = false;
    double born_at = -1e9;       // when it rose, for the pile's settle window
    double next_bite = 0.0;      // a bite a second once it has one to bite
    Vec3 walk_ref{};             // where it last stood, to catch a stuck one
    double walk_ref_at = 0.0;
    double free_slot_at = 0.0;   // dead: when its slot goes back to the wave
    // Its pack: the zombie it follows in single file, or -1 when it reads
    // the field itself - the pack's leader, or the last of a broken line.
    int ahead_id = -1;
    // When it stopped making progress: a stuck one jumps for a way out;
    // the spade comes out only where the dig walk says it pays.
    double stuck_at = -1.0;
    // The walk's distance read at its feet, kept window by window: the
    // moving-stuck's tell. A leader that has not stood on a lower floor
    // of the walk for kZombieYoyoWindows is pacing - the yo-yo - and is
    // owed the standstill's way out. Followers keep no score: a heel that
    // holds its place in the line stirs on the spot by design.
    int foot_dist = -1;
    int stall_windows = 0;
    // When it last flung itself at the step it could not climb.
    double pounce_at = -1e9;
    // When its spade last took a block off what it faces: the pace of the
    // dig, one voxel a dig_seconds, and the tell that tells the unstick
    // its windows apart - a digger stands by design, and the hop and the
    // fling would only spoil its work.
    double spade_at = -1e9;
    // Its heel prints: the places its feet were set, half a pace apart,
    // kept in a ring in the order they were walked - the pack's memory
    // of the way it holds. The line does not follow the body in front;
    // it follows the ground that body has walked over, so the file
    // crosses a threshold where its leader crossed it and turns where
    // its leader turned, and never takes the open air's straight line
    // across a bend no leg ever made. No print is set aloft: a fling
    // crosses ground no following foot may tread.
    static constexpr int kHeelN = 64;   // paces of prints, in a ring
    float heel_x[kHeelN] = {};
    float heel_y[kHeelN] = {};
    uint32_t heel_seq[kHeelN] = {};    // the print's number, 0 = an empty slot
    int heel_head = 0;                  // where the next print is set
    float heel_ref_x = 0.0f, heel_ref_y = 0.0f;   // the last print's place

    bool spectator() const { return team == -1; }
    bool playing() const { return named && (team == 0 || team == 1); }
};

struct Grenade {
    int owner;
    int team;
    Vec3 pos;
    double explode_at;
};

struct Server::Impl {
    HostConfig cfg;
    ENetHost* host = nullptr;
    bool enet_up = false;
    VoxelMap map;
    SolidMap phys_map;
    std::mt19937 rng{std::random_device{}()};
    std::array<std::unique_ptr<Player>, kSlots> players{};
    std::vector<Grenade> grenades;
    double world_time = 0;      // the physics clock: advanced in fixed ticks
    // When the next world update is due: a deadline, not "a period since the
    // last", so a 60Hz update on a 120Hz loop goes out every second pass.
    double next_network = 0;
    // The map as sent to joiners, and the revision it was taken at. map_rev
    // counts edits. Re-encoding the map takes a tenth of a second or more, so
    // it is done on a worker from a snapshot, never on this thread.
    std::shared_ptr<const std::vector<uint8_t>> map_cache;
    uint64_t map_rev = 1, cache_rev = 0;
    std::unique_ptr<VoxelMap> snapshot;
    std::thread map_job;
    std::atomic<bool> job_done{false};
    bool job_running = false;
    uint64_t job_rev = 0;
    std::shared_ptr<const std::vector<uint8_t>> job_result;
    const uint32_t team_color[2] = {0x0000C4, 0x00C400};

    // Aim: the run as the server keeps it; published to cfg.aim every tick.
    training::AimStatus aim;
    double phase_until = 0;
    int shots_base = 0;          // the person's rounds fired when the run began
    int bot_count = 0;
    double next_tutorial_check = 0;
    const uint32_t fog = 0x80E8FF;

    void log(const char* fmt, ...) {
        if (!cfg.log) return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        cfg.log(buf);
    }

    // --- sending ----------------------------------------------------------
    void send(Player& p, const ByteWriter& bw, bool reliable = true) {
        if (!p.peer) return;
        ENetPacket* pkt = make_packet(bw, reliable);
        if (enet_peer_send(p.peer, 0, pkt) < 0) enet_packet_destroy(pkt);
    }
    static ENetPacket* make_packet(const ByteWriter& bw, bool reliable) {
        return enet_packet_create(bw.data.data(), bw.data.size(),
                                  reliable ? ENET_PACKET_FLAG_RELIABLE
                                           : (ENET_PACKET_FLAG_UNSEQUENCED |
                                              ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT));
    }
    // pyspades broadcast_contained: everybody with an id, except `sender`, on
    // `team` if given. A player still loading the map gets it after the map if
    // `save`, and never otherwise. One packet for everybody: ENet counts its
    // references, so a relay is not a copy per recipient.
    void broadcast(const ByteWriter& bw, bool save, int sender = -1, int team = -2,
                   bool reliable = true) {
        ENetPacket* pkt = nullptr;
        for (auto& pp : players) {
            if (!pp || pp->id == sender) continue;
            if (team != -2 && pp->team != team) continue;
            if (pp->loading) {
                if (save) pp->saved.push_back(bw.data);
            } else if (pp->peer) {
                if (!pkt) pkt = make_packet(bw, reliable);
                enet_peer_send(pp->peer, 0, pkt);
            }
        }
        if (pkt && pkt->referenceCount == 0) enet_packet_destroy(pkt);
    }
    void system_chat(const std::string& text, int to = -1) {
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::ChatMessage));
        bw.write_u8(255);
        bw.write_u8(static_cast<uint8_t>(proto::ChatType::System));
        bw.write_cstring(text);
        if (to >= 0 && players[to]) send(*players[to], bw);
        else broadcast(bw, false);
    }

    static void write_color(ByteWriter& bw, uint32_t rgb) {
        bw.write_u8(static_cast<uint8_t>(rgb));
        bw.write_u8(static_cast<uint8_t>(rgb >> 8));
        bw.write_u8(static_cast<uint8_t>(rgb >> 16));
    }

    // --- the map ----------------------------------------------------------
    // The horde's map: the .vxl the settings name, or the built course.
    bool load_map(std::string& err) {
        if (horde_mode() && !cfg.map_file.empty()) {
            std::ifstream in(cfg.map_file, std::ios::binary);
            if (!in) {
                err = "cannot open the map file: " + cfg.map_file;
                return false;
            }
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            if (data.empty()) {
                err = "the map file is empty: " + cfg.map_file;
                return false;
            }
            if (!map.load_vxl(data.data(), data.size(), err)) return false;
            map.to_solid_map(phys_map);
            return true;
        }
        generate_training_map(map);
        generate_aim_arena(map, static_cast<int>(cfg.scenario));
        map.to_solid_map(phys_map);
        return true;
    }
    // The nearest land column to (x, y): the one asked for when its bedrock
    // layer is solid, and the first land found scanning rings out otherwise
    // - a core or a spawn offshore would stand in the water.
    void nearest_land(int& x, int& y) const {
        const auto land = [this](int cx, int cy) { return map.solid(cx, cy, 62); };
        const int cx = std::min(std::max(x, 2), 509), cy = std::min(std::max(y, 2), 509);
        x = cx;
        y = cy;
        if (land(cx, cy)) return;
        for (int r = 1; r <= 509; ++r) {
            for (int dx = -r; dx <= r; ++dx) {
                const int px = cx + dx;
                if (px < 0 || px > 511 || cx != px && false) continue;
                if (cy - r >= 0 && land(px, cy - r)) { x = px; y = cy - r; return; }
                if (cy + r <= 511 && land(px, cy + r)) { x = px; y = cy + r; return; }
            }
            for (int dy = -r + 1; dy <= r - 1; ++dy) {
                const int py = cy + dy;
                if (py < 0 || py > 511) continue;
                if (cx - r >= 0 && land(cx - r, py)) { x = cx - r; y = py; return; }
                if (cx + r <= 511 && land(cx + r, py)) { x = cx + r; y = py; return; }
            }
        }
    }
    bool aim_mode() const { return cfg.mode == training::Mode::Aim; }
    bool horde_mode() const { return cfg.mode == training::Mode::Horde; }
    // The core's column, built at the start: the cells out of the ground it
    // stands on, and the ground cell it stands over, which all belong to it.
    int core_top_z = 0, core_ground_z = 0;
    bool core_column_cell(int x, int y, int z) const {
        if (!horde_mode()) return false;
        if (x != horde_cfg.core_x || y != horde_cfg.core_y) return false;
        return z >= core_top_z && z <= core_ground_z - 1;
    }
    // The core stands three blocks out of the ground, in the colour of its
    // team: everything a client needs to see where to defend. The intel
    // drawn on top of it comes from StateData, and the dig and blast vetoes
    // below keep it standing.
    void build_core_column() {
        const int x = horde_cfg.core_x, y = horde_cfg.core_y;
        core_ground_z = map.get_z(x, y);
        for (int k = 1; k <= 3; ++k)
            map.set(x, y, core_ground_z - k, 0x7F3080FFu);
        core_top_z = core_ground_z - 3;
    }
    // The horde's brain, and what its host remembers between its ticks.
    horde::Config horde_cfg;
    std::unique_ptr<horde::Horde> horde;
    horde::Phase horde_told = horde::Phase::Prep;
    int horde_wave_told = 0;
    double core_alarm_at = 0.0;   // "under attack": one line every few seconds
    // The packs: one per spawn point, a single path each - the last joined
    // zombie of a point is the one the next of its pack follows.
    int pack_tail[horde::Config::kSpawnSites] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // When each spawn point's ping next goes out, while that point still
    // streams its wave: one marker every few seconds survives a client's
    // map load, which a single early ping never did.
    double corner_ping_next[horde::Config::kSpawnSites] = {};
    // When the way in's vapour is next laid again: the trail is said in
    // beds of ground vapour, one a blink, and the world is built on
    // while the dead walk it - so the reading is traced over and over
    // and the vapour never says a way in the walk has stopped walking.
    double route_smoke_next = 0.0;
    void serve_maps() {
        if (job_running && job_done.load()) {
            map_job.join();
            job_running = false;
            map_cache = job_result;
            cache_rev = job_rev;
        }
        bool waiting = false;
        for (auto& pp : players) {
            if (!pp || !pp->loading || pp->map_data) continue;
            if (map_cache && cache_rev >= pp->need_rev) start_map(*pp);
            else waiting = true;
        }
        if (!waiting || job_running) return;
        if (!snapshot) snapshot = std::make_unique<VoxelMap>();
        *snapshot = map;
        job_rev = map_rev;
        job_done = false;
        job_running = true;
        map_job = std::thread([this]() {
            job_result = std::make_shared<const std::vector<uint8_t>>(snapshot->save_compressed());
            job_done = true;
        });
    }
    void start_map(Player& p) {
        p.map_data = map_cache;
        p.map_off = 0;
        ByteWriter start;
        start.write_u8(static_cast<uint8_t>(PacketId::MapStart));
        start.write_u32(static_cast<uint32_t>(p.map_data->size()));
        send(p, start);
    }
    void touched(const std::vector<Cell>& cells, bool solid) {
        for (const Cell& c : cells) phys_map.set_block(c.x, c.y, c.z, solid);
        if (cells.empty()) return;
        ++map_rev;
        if (horde) horde->on_map_edited();   // the field follows every edit
    }

    // pyspades get_random_location: a land column in the box, on its top.
    Vec3 random_location(int x1, int y1, int x2, int y2) {
        int x, y;
        map.random_point(x1, y1, x2, y2, rng, x, y);
        return Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(map.get_z(x, y))};
    }

    // --- StateData --------------------------------------------------------
    // Territory control with no territories: the course has nothing to
    // capture, so it is never won. The horde is capture the flag, with
    // nothing to capture: team 0's flag stands for the core, and the
    // server never picks an intel up, so it stands there the whole game.
    // The client draws an intel - Models/MapObjects/Intel.kv6 - at the
    // flag's place, which is what the core is drawn as.
    Vec3 core_intel_pos() const {
        // The top of the core's column: after it is built, get_z is its plane.
        return Vec3{static_cast<float>(horde_cfg.core_x) + 0.5f,
                    static_cast<float>(horde_cfg.core_y) + 0.5f,
                    static_cast<float>(map.get_z(horde_cfg.core_x, horde_cfg.core_y))};
    }
    Vec3 under_core() const {
        // Inside the bedrock the core stands on, where nothing sees it: the
        // rest of the mode's list, which a client would draw at its zero.
        return Vec3{static_cast<float>(horde_cfg.core_x) + 0.5f,
                    static_cast<float>(horde_cfg.core_y) + 0.5f, 62.9f};
    }
    ByteWriter state_data(int player_id) {
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::StateData));
        bw.write_u8(static_cast<uint8_t>(player_id));
        write_color(bw, fog);
        write_color(bw, team_color[0]);
        write_color(bw, team_color[1]);
        bw.write_fixed_string("Blue", 10);
        bw.write_fixed_string(horde_mode() ? "Zombies" : "Green", 10);
        if (horde_mode()) {
            bw.write_u8(0);                    // Capture the Flag
            bw.write_u8(0);                    // captures: team 0
            bw.write_u8(0);                    // captures: the horde
            bw.write_u8(10);                   // captures to win: never reached
            bw.write_u8(0);                    // nobody holds an intel
            bw.write_vec(core_intel_pos());    // team 0's flag: the core
            bw.write_vec(under_core());        // team 1's flag, buried
            bw.write_vec(under_core());        // team 0's base, buried
            bw.write_vec(under_core());        // team 1's base, buried
        } else {
            bw.write_u8(1);
            bw.write_u8(0);
        }
        for (size_t i = 0; i < 16; ++i)
            for (int k = 0; k < 13; ++k) bw.write_u8(0);
        return bw;
    }

    // --- connecting -------------------------------------------------------
    void on_connect(ENetPeer* peer, uint32_t version) {
        if (version != proto::PROTOCOL_VERSION) {
            enet_peer_disconnect(peer, kErrorWrongVersion);
            return;
        }
        int id = -1;
        int count = 0;
        for (int i = 0; i < kSlots; ++i) {
            if (players[i]) ++count;
            else if (id < 0) id = i;
        }
        if (id < 0 || count >= cfg.max_players + bot_count) {
            enet_peer_disconnect(peer, kErrorFull);
            return;
        }
        // ENet's packet throttle drops unreliable packets when the round trip
        // grows, which is a guess about a congested internet link. On
        // loopback it only makes things worse: a deceleration of zero holds the
        // throttle open, and ENet passes the setting to the client's end too.
        enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                     ENET_PEER_PACKET_THROTTLE_SCALE, 0);
        peer->packetThrottle = ENET_PEER_PACKET_THROTTLE_SCALE;
        auto p = std::make_unique<Player>();
        p->peer = peer;
        p->id = id;
        p->loading = true;
        peer->data = p.get();
        // What arrives after the map: everybody already here, then the state.
        for (auto& o : players) {
            if (!o || !o->named) continue;
            ByteWriter bw;
            bw.write_u8(static_cast<uint8_t>(PacketId::ExistingPlayer));
            bw.write_u8(static_cast<uint8_t>(o->id));
            bw.write_u8(static_cast<uint8_t>(static_cast<int8_t>(o->team)));
            bw.write_u8(static_cast<uint8_t>(o->weapon));
            bw.write_u8(static_cast<uint8_t>(o->tool));
            bw.write_u32(static_cast<uint32_t>(o->kills));
            write_color(bw, o->color);
            bw.write_cstring(o->name);
            p->saved.push_back(bw.data);
        }
        p->saved.push_back(state_data(id).data);
        p->need_rev = map_rev;
        players[id] = std::move(p);
        Player& pr = *players[id];
        // MapStart goes first, before anything but the handshake; it waits for
        // a map at least as new as this moment (serve_maps).
        if (map_cache && cache_rev >= pr.need_rev) start_map(pr);
        send_handshake(pr);
        log("player #%d connecting", id);
    }
    void send_handshake(Player& p) {
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::VersionHandshakeInit));
        bw.write_u32(42);
        send(p, bw);
    }
    // Map chunks this pass may still send, across every joining player.
    int chunk_budget = 0;
    void continue_map(Player& p) {
        if (!p.loading || !p.map_data) return;
        // Loopback has no loss and no queue worth speaking of: keep a few
        // hundred KB in flight and the whole map is over in well under a
        // second, where pyspades' ten chunks a frame take several.
        int sent = 0;
        while (p.map_off < p.map_data->size() && sent < 32 && chunk_budget > 0 &&
               p.peer->reliableDataInTransit < 512 * 1024) {
            --chunk_budget;
            const size_t n = std::min(kMapChunk, p.map_data->size() - p.map_off);
            ByteWriter bw;
            bw.write_u8(static_cast<uint8_t>(PacketId::MapChunk));
            bw.write_bytes(p.map_data->data() + p.map_off, n);
            send(p, bw);
            p.map_off += n;
            ++sent;
        }
        if (p.map_off < p.map_data->size()) return;
        p.map_data.reset();
        p.loading = false;
        for (auto& d : p.saved) {
            ENetPacket* pkt = enet_packet_create(d.data(), d.size(), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(p.peer, 0, pkt);
        }
        p.saved.clear();
    }

    void on_disconnect(Player& p) {
        if (p.named) {
            ByteWriter bw;
            bw.write_u8(static_cast<uint8_t>(PacketId::PlayerLeft));
            bw.write_u8(static_cast<uint8_t>(p.id));
            broadcast(bw, true, p.id);
            log("%s left", p.name.c_str());
        }
        players[p.id].reset();
    }

    // --- a player's life --------------------------------------------------
    void set_weapon(Player& p, int weapon) {
        p.weapon = std::min(std::max(weapon, 0), 2);
        p.gun = Gun{};
        p.gun.type = p.weapon;
        p.gun.reset();
    }

    void refill(Player& p) {
        p.hp = 100;
        p.grenades = 3;
        p.blocks = 50;
        p.gun.restock();
    }

    // Training never runs out: a person nearly out of reserve ammo, grenades or
    // blocks is given more (Restock: health, grenades, blocks and reserve to
    // full on the client too). Only when nearly out, since a restock plays its
    // sound; not in the middle of a reload, which carries on with what it had.
    void keep_stocked(Player& p) {
        if (!p.peer || !p.alive || p.aim_bot || p.zombie || p.gun.reloading) return;
        const bool low = p.gun.stock <= p.gun.spec().clip || p.grenades <= 0 || p.blocks <= 10;
        if (!low || world_time - p.last_restock < 1.5) return;
        p.last_restock = world_time;
        refill(p);
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::Restock));
        bw.write_u8(static_cast<uint8_t>(p.id));
        send(p, bw);
    }

    // A target on its lane of the range, a person at the start.
    Vec3 spawn_location(Player& p) {
        if (p.zombie && p.has_spawn_at) return p.spawn_at;
        if (p.aim_bot && p.has_spawn_at) return p.spawn_at;
        if (horde_mode())
            return random_location(horde_cfg.core_x - 2, horde_cfg.core_y - 2,
                                   horde_cfg.core_x + 3, horde_cfg.core_y + 3);
        if (aim_mode() && !p.aim_bot)
            return Vec3{static_cast<float>(training::kAimSpawnX), static_cast<float>(training::kAimSpawnY),
                        static_cast<float>(map.get_z(training::kAimSpawnX, training::kAimSpawnY))};
        if (p.lane >= 0) {
            const int x = training::kLaneX[p.lane];
            return Vec3{static_cast<float>(x), 256.0f, static_cast<float>(map.get_z(x, 256))};
        }
        return random_location(training::kSpawnX - 2, training::kSpawnY - 2, training::kSpawnX + 3,
                               training::kSpawnY + 3);
    }

    void spawn(Player& p) {
        p.respawn_at = -1;
        if (p.team == -2) return;
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::CreatePlayer));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_u8(static_cast<uint8_t>(p.weapon));
        bw.write_u8(static_cast<uint8_t>(static_cast<int8_t>(p.team)));
        if (!p.spectator()) {
            Vec3 at = spawn_location(p);
            at.x += 0.5f;
            at.y += 0.5f;
            at.z -= 2.4f;
            p.body = physics::Body{};
            p.body.pos = at;
            p.body.eye = at;
            p.body.ori = Vec3{p.team == 0 ? 1.0f : -1.0f, 0, 0};
            p.input = physics::Input{};
            p.net_input = 0;
            p.swing_shown = false;
            p.stuck_at = -1.0;
            p.foot_dist = -1;
            p.stall_windows = 0;
            p.alive = true;
            p.tool = kToolWeapon;
            if (p.zombie) p.tool = kToolSpade;   // it never fires a shot
            p.gun.reset();
            refill(p);
            bw.write_vec(at);
        } else {
            p.alive = false;
            bw.write_vec(Vec3{0, 0, 0});
        }
        bw.write_cstring(p.name);
        broadcast(bw, true);
        if (!p.client_info) send_handshake(p);
        p.appeared_at = world_time;
        if (p.aim_bot) {
            p.home_y = p.body.pos.y;
            p.strafe_until = 0;
            if (cfg.highlight_targets) send_mark(p);
        } else if (p.zombie) {
            p.walk_ref = p.body.pos;
            p.walk_ref_at = world_time;
            p.foot_dist = -1;
            p.stall_windows = 0;
            // Its hands, not a gun: a spade, drawn by the grunts' Tool model.
            ByteWriter tool;
            tool.write_u8(static_cast<uint8_t>(PacketId::SetTool));
            tool.write_u8(static_cast<uint8_t>(p.id));
            tool.write_u8(static_cast<uint8_t>(kToolSpade));
            broadcast(tool, true);
        }
    }

    float respawn_seconds(const Player& p) const {
        if (p.aim_bot) return static_cast<float>(p.respawn_delay);
        return p.lane >= 0 ? kTargetRespawn : cfg.respawn_sec;
    }
    void schedule_respawn(Player& p) {
        if (p.respawn_at < 0) p.respawn_at = world_time + respawn_seconds(p);
    }

    void kill(Player& p, Player* by, int kill_type) {
        if (!p.alive) return;
        if (p.zombie && horde) {
            horde->on_zombie_killed();
            // The pack does not lose its way with a fallen mate: whoever
            // followed it follows whoever it followed.
            const int fallen_ahead = p.ahead_id;
            for (auto& qq : players)
                if (qq && qq->zombie && qq->ahead_id == p.id) qq->ahead_id = fallen_ahead;
            p.ahead_id = -1;
        }
        if (p.aim_bot) {
            if (by && !by->aim_bot && aim.phase == training::AimPhase::Running) {
                aim.kills += 1;
                aim.ttk_sum += static_cast<float>(world_time - p.appeared_at);
                aim.ttk_count += 1;
            }
            place_aim_bot(p, true);
        }
        p.alive = false;
        p.hp = 0;
        p.gun.reset();
        p.lamp = false; // every client puts it out on the Kill Action
        float delay = respawn_seconds(p);
        if (p.zombie) delay = 2.0f;   // its slot is freed, not a respawn
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::KillAction));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_u8(static_cast<uint8_t>(by ? by->id : p.id));
        bw.write_u8(static_cast<uint8_t>(kill_type));
        bw.write_u8(static_cast<uint8_t>(std::min(255, static_cast<int>(delay) + 1)));
        broadcast(bw, true);
        if (by && by != &p) by->kills += 1;
        if (p.zombie) {
            p.respawn_at = -1;
            p.free_slot_at = world_time + delay;
        } else {
            p.respawn_at = world_time + delay;
        }
    }

    void set_hp(Player& p, int value, Player* by, int kill_type, const Vec3* source) {
        p.hp = std::max(0, std::min(100, value));
        if (p.hp <= 0) {
            kill(p, by, kill_type);
            return;
        }
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::SetHP));
        bw.write_u8(static_cast<uint8_t>(p.hp));
        bw.write_u8(kill_type == kFallKill ? 0 : 1);
        Vec3 src{0, 0, 0};
        if (source) src = *source;
        else if (by && by != &p) src = by->body.pos;
        bw.write_vec(src);
        send(p, bw);
    }

    void hit(Player& victim, int amount, Player* by, int kill_type) {
        if (!victim.alive) return;
        if (by && by != &victim && by->team == victim.team) return;
        set_hp(victim, victim.hp - amount, by, kill_type, nullptr);
    }

    // --- the range's targets ----------------------------------------------
    // The server's own players, with no connection behind them: they walk a
    // line and fall over, which needs no AI.
    void add_targets() {
        for (int lane = 0; lane < training::kLaneCount; ++lane) {
            int id = -1;
            for (int i = 0; i < kSlots && id < 0; ++i)
                if (!players[i]) id = i;
            if (id < 0) return;
            auto p = std::make_unique<Player>();
            p->id = id;
            p->named = true;
            p->name = "Target " + std::to_string(lane + 1);
            p->team = kTargets;
            p->lane = lane;
            p->client_info = true; // nobody to send a handshake to
            players[id] = std::move(p);
            spawn(*players[id]);
        }
    }
    // Across the corridor and back: face the end it is going to and walk.
    void steer_target(Player& p) {
        auto end = [&] { return p.to_far ? training::kLaneY1 : training::kLaneY0; };
        if (std::fabs(p.body.pos.y - end()) < 0.75f) p.to_far = !p.to_far;
        p.body.ori = Vec3{0.0f, end() > p.body.pos.y ? 1.0f : -1.0f, 0.0f};
        p.input = physics::Input{};
        p.input.mf = true;
    }

    // --- the aim scenarios --------------------------------------------------
    Player* person() {
        for (auto& pp : players)
            if (pp && pp->peer && pp->playing()) return pp.get();
        return nullptr;
    }
    float uniform(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
    int bots_for(training::Scenario sc) const {
        switch (sc) {
            case training::Scenario::Gridshot: return 3;
            case training::Scenario::Flick: return 1;
            case training::Scenario::Tracking: return 1;
            case training::Scenario::StrafeDuel: return 2;
            case training::Scenario::LongRange: return training::kFarLaneCount;
            default: return 1;
        }
    }
    void add_aim_bots() {
        const int n = bots_for(cfg.scenario);
        for (int k = 0; k < n; ++k) {
            int id = -1;
            for (int i = 0; i < kSlots && id < 0; ++i)
                if (!players[i]) id = i;
            if (id < 0) return;
            auto p = std::make_unique<Player>();
            p->id = id;
            p->named = true;
            p->name = "Target " + std::to_string(k + 1);
            p->team = kTargets;
            p->aim_bot = true;
            if (cfg.scenario == training::Scenario::LongRange) p->far_lane = k;
            p->client_info = true;
            players[id] = std::move(p);
            ++bot_count;
            place_aim_bot(*players[id], false);
            spawn(*players[id]);
        }
    }
    // A ground voxel at `x`, `y` in the arena, kept off its walls.
    Vec3 arena_ground(float fx, float fy) {
        const int x = std::min(std::max(static_cast<int>(fx), training::kArenaX0 + 2), training::kArenaX1 - 2);
        const int y = std::min(std::max(static_cast<int>(fy), training::kArenaY0 + 2), training::kArenaY1 - 2);
        return Vec3{static_cast<float>(x), static_cast<float>(y), static_cast<float>(map.get_z(x, y))};
    }
    // Somewhere around the player's spot: `deg` either side of straight ahead,
    // between `d0` and `d1` blocks out.
    Vec3 arena_arc(float deg, float d0, float d1) {
        const float a = uniform(-deg, deg) * 3.14159265f / 180.0f, d = uniform(d0, d1);
        return arena_ground(training::kAimSpawnX + std::cos(a) * d, training::kAimSpawnY + std::sin(a) * d);
    }
    bool clear_of_bots(const Player& self, const Vec3& at, float gap) {
        for (auto& pp : players) {
            if (!pp || pp.get() == &self || !pp->aim_bot || !pp->alive) continue;
            if (std::fabs(pp->body.pos.x - at.x) < gap && std::fabs(pp->body.pos.y - at.y) < gap) return false;
        }
        return true;
    }
    // Where an aim target comes back, and after how long; `pinged` sends the
    // spot ahead of it in Flick.
    void place_aim_bot(Player& p, bool pinged) {
        using training::Scenario;
        Vec3 at{};
        double delay = 0.0;
        for (int tries = 0; tries < 20; ++tries) {
            switch (cfg.scenario) {
                case Scenario::Gridshot: {
                    const int st = std::uniform_int_distribution<int>(0, training::kStandCount - 1)(rng);
                    at = arena_ground(uniform(training::kStandX[st] + 1.0f,
                                              training::kStandX[st] + training::kStandDepth - 1.0f),
                                      uniform(training::kStandY0 + 1.0f, training::kStandY1 - 1.0f));
                    break;
                }
                case Scenario::Flick: at = arena_arc(70.0f, 10.0f, 32.0f); break;
                case Scenario::Tracking: at = arena_ground(training::kAimSpawnX + 20.0f, training::kAimSpawnY); break;
                case Scenario::StrafeDuel: at = arena_arc(45.0f, 15.0f, 35.0f); break;
                case Scenario::LongRange:
                    at = arena_ground(static_cast<float>(training::kFarLaneX[std::max(0, p.far_lane)]),
                                      uniform(training::kFarLaneY0, training::kFarLaneY1));
                    break;
                default: at = arena_arc(45.0f, 15.0f, 30.0f); break;
            }
            if (clear_of_bots(p, at, 3.0f)) break;
        }
        switch (cfg.scenario) {
            case Scenario::Gridshot: delay = 0.0; break;
            case Scenario::Flick: delay = uniform(0.35f, 0.7f); break;
            case Scenario::StrafeDuel: delay = 0.6; break;
            case Scenario::LongRange: delay = 1.0; break;
            default: delay = 0.5; break;
        }
        p.spawn_at = at;
        p.has_spawn_at = true;
        p.respawn_delay = delay;
        if (pinged && cfg.scenario == Scenario::Flick)
            send_ping(Vec3{at.x + 0.5f, at.y + 0.5f, at.z - 1.5f}, static_cast<float>(delay) + 0.2f, 0xFF9020, "");
    }
    // The aim targets' own minds: still, strafing and jumping, or walking.
    void steer_aim_bot(Player& p) {
        using training::Scenario;
        p.input = physics::Input{};
        const Player* who = person();
        // Facing the player, always.
        if (who) {
            float dx = who->body.pos.x - p.body.pos.x, dy = who->body.pos.y - p.body.pos.y;
            const float l = std::sqrt(dx * dx + dy * dy);
            if (l > 1e-3f) p.body.ori = Vec3{dx / l, dy / l, 0.0f};
        }
        switch (cfg.scenario) {
            case Scenario::Tracking:
            case Scenario::StrafeDuel: {
                if (world_time >= p.strafe_until) {
                    const float stop = cfg.scenario == Scenario::StrafeDuel ? 0.3f : 0.1f;
                    p.strafe = uniform(0.0f, 1.0f) < stop ? 0 : (uniform(0.0f, 1.0f) < 0.5f ? -1 : 1);
                    p.strafe_until = world_time + uniform(0.35f, 1.4f);
                }
                // Back towards where it started when it strays too far: whichever
                // way it steps, turned round if that takes it further out.
                const float off = p.body.pos.y - p.home_y;
                // Judged now and then, as a turn takes the body a moment.
                if (std::fabs(off) > 9.0f && world_time >= p.recover_at) {
                    if (p.strafe == 0) p.strafe = 1;
                    else if ((off > 0.0f) == (p.body.vel.y > 0.0f)) p.strafe = -p.strafe;
                    p.strafe_until = world_time + 0.6;
                    p.recover_at = world_time + 0.4;
                }
                p.input.ml = p.strafe < 0;
                p.input.mr = p.strafe > 0;
                if (world_time >= p.next_jump) {
                    p.input.jump = p.body.vel.z >= 0 && p.body.vel.z < 0.017f;
                    p.next_jump = world_time + uniform(1.5f, 4.0f);
                }
                break;
            }
            case Scenario::LongRange: {
                const float end = p.to_far ? training::kFarLaneY1 : training::kFarLaneY0;
                if (std::fabs(p.body.pos.y - end) < 0.75f) p.to_far = !p.to_far;
                p.body.ori = Vec3{0.0f, end > p.body.pos.y ? 1.0f : -1.0f, 0.0f};
                p.input.mf = true;
                break;
            }
            default: break;  // still
        }
    }
    // --- the horde ---------------------------------------------------------
    // The brain's body: the players whose walk and fall it counts. A zombie
    // is a player like a range target is, with no connection behind it.
    void add_zombie(const horde::SpawnAt& at) {
        int id = -1;
        for (int i = 0; i < kSlots && id < 0; ++i)
            if (!players[i]) id = i;
        if (id < 0) {
            horde->on_zombie_killed();   // nowhere to stand: the wave skipped it
            return;
        }
        auto p = std::make_unique<Player>();
        p->id = id;
        p->named = true;
        p->name = "Zombie";
        p->team = kTargets;
        p->zombie = true;
        p->born_at = world_time;
        // Single file behind its corner's pack: a zombie with nobody to
        // follow is the pack's leader, and reads the field itself.
        const int corner = at.corner;
        const Player* tail =
            pack_tail[corner] >= 0 ? players[static_cast<size_t>(pack_tail[corner])].get() : nullptr;
        p->ahead_id = (tail && tail->zombie && tail->alive) ? tail->id : -1;
        pack_tail[corner] = id;
        p->client_info = true;
        p->has_spawn_at = true;
        p->spawn_at = Vec3{static_cast<float>(at.x), static_cast<float>(at.y),
                           static_cast<float>(at.z)};
        players[id] = std::move(p);
        ++bot_count;
        spawn(*players[id]);
    }
    // The fallen keep their slot a moment so their fall is seen, then the rest
    // of the wave may have it.
    void free_dead_zombies() {
        for (auto& pp : players) {
            if (!pp || !pp->zombie || pp->alive || pp->free_slot_at <= 0) continue;
            if (world_time >= pp->free_slot_at) {
                pp.reset();
                --bot_count;
            }
        }
    }
    // One frame of teeth, or of the spade: the swing the clients animate,
    // closed again in the tick after.
    void show_swing(Player& p, double span) {
        p.swing_shown = true;
        p.swing_until = world_time + span;
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::WeaponInput));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_u8(1);
        broadcast(bw, false);
    }

    // The next print the follower owes a step to: the oldest print of the
    // one ahead that its own feet have not stood on yet. A print is
    // stood on when the walker comes within kPrintTouch of it, or past
    // it at
    // the shoulder's width, and it is spent
    // when the walker has come nearer a later print - a print overtaken
    // or flung over is behind the walk and owed no step. The ring is
    // walked newest first, the nearest distance of everything later
    // carried down through the ages as it goes: one pass, and the last
    // print still standing is the oldest one owed. -1: nothing owed -
    // the line's heel is held the plain way, at the body.
    // Trodden beside: a print the walker has stepped past, inside the
    // shoulder's width of its own plane. The plane is drawn between the
    // print's neighbours in the walk, the one before it and the one after
    // it, as far as the ring still holds either; the sequence numbers
    // sort true neighbours from the ring's wrapped end.
    static bool trodden_beside(const Player& a, int slot, float x, float y) {
        const uint32_t s = a.heel_seq[slot];
        const int before = (slot + Player::kHeelN - 1) % Player::kHeelN;
        const int after = (slot + 1) % Player::kHeelN;
        const bool has_before = a.heel_seq[before] != 0 && a.heel_seq[before] < s;
        const bool has_after = a.heel_seq[after] != 0 && a.heel_seq[after] > s;
        if (!has_before && !has_after) return false;
        float ax = a.heel_x[slot], ay = a.heel_y[slot];
        float bx = ax, by = ay;
        if (has_before) { ax = a.heel_x[before]; ay = a.heel_y[before]; }
        if (has_after) { bx = a.heel_x[after]; by = a.heel_y[after]; }
        const float dx = bx - ax, dy = by - ay;
        const float n = std::sqrt(dx * dx + dy * dy);
        if (n < 1e-3f) return false;
        const float rx = x - a.heel_x[slot], ry = y - a.heel_y[slot];
        if (rx * dx + ry * dy <= 0.0f) return false;   // not past it yet
        const float lat = std::fabs(rx * dy - ry * dx) / n;
        return lat < kPrintShoulder;
    }

    static int ahead_print(const Player& a, float x, float y) {
        int chosen = -1;
        float nearer_later = -1.0f;
        for (int k = 0; k < Player::kHeelN; ++k) {
            const int slot =
                (a.heel_head + Player::kHeelN - 1 - k) % Player::kHeelN;
            const uint32_t s = a.heel_seq[slot];
            if (s == 0) continue;                   // an empty slot is no print
            const float ddx = a.heel_x[slot] - x;
            const float ddy = a.heel_y[slot] - y;
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 >= kPrintTouch * kPrintTouch && !trodden_beside(a, slot, x, y) &&
                (nearer_later < 0.0f || nearer_later >= d2))
                chosen = slot;          // nothing later lies nearer: still owed
            nearer_later = nearer_later < 0.0f ? d2 : std::min(nearer_later, d2);
        }
        return chosen;
    }

    // The spade's lead: the dig walk's advice, for a walker the legs'
    // advice has nothing for, or nothing worth its while - `leg_led` says
    // which question this is. The crossing the dig walk hands back may
    // be a step the legs make after all, a drop walked straight off (the
    // dead fear no landing), or a climb beyond the jump: and a climb is
    // the spade's own work. The walker plants itself facing the column
    // it cannot take and empties the crossing's top off it, one voxel a
    // dig_seconds - the pace the walk's own price of it says - until what
    // is left is a jump the hop and the fling can clear, and the field,
    // freshly recomputed around the lowered wall, hands the whole pack
    // the way over. Only the pack's head digs: one spade a crossing, one
    // breach for the line to pour through; a follower steer-led by the
    // dig walk walks and drops as advised, but waits at the wall for the
    // breach its head opens. False when the licence is off, the walker
    // is aloft, the dig walk is silent, or the legs' way is nearly as
    // soon; the walker is left to the legs' own answers.
    bool dig_lead(Player& p, bool leg_led) {
        if (!horde_mode() || !horde_cfg.dig || p.body.airborne) return false;
        const int cx = static_cast<int>(std::floor(p.body.pos.x));
        const int cy = static_cast<int>(std::floor(p.body.pos.y));
        const int fz = static_cast<int>(std::floor(p.body.pos.z + 2.25f));
        if (leg_led && !horde->dig_worth(cx, cy, fz)) return false;
        int tx = cx, ty = cy, tz = fz;
        if (!horde->next_dig_step(cx, cy, fz, tx, ty, tz)) return false;
        const float dx = static_cast<float>(tx) + 0.5f - p.body.pos.x;
        const float dy = static_cast<float>(ty) + 0.5f - p.body.pos.y;
        const float dl = std::sqrt(dx * dx + dy * dy);
        if (dl > 1e-3f) p.body.ori = Vec3{dx / dl, dy / dl, 0.0f};
        // The crossing against the jump: a climb the legs cannot make is
        // the column to be taken down, from its own top - the crossing's
        // surface voxel, the one the walk prices its block at. The core's
        // column is never touched, whatever its height.
        const int climb = tz < fz ? fz - tz : 0;
        if (climb > horde::Config::kWalkClimb) {
            p.input.mf = false;   // it works standing, not pressed at its wall
            if (p.ahead_id >= 0) return true;   // the line waits on its head
            if (world_time < p.spade_at + horde_cfg.dig_seconds) return true;
            if (!map.solid(tx, ty, tz)) return true;   // the field has not caught up yet
            if (core_column_cell(tx, ty, tz)) return true;
            std::vector<Cell> removed;
            map.destroy_point(tx, ty, tz, &removed);
            touched(removed, false);
            ByteWriter bw;
            bw.write_u8(static_cast<uint8_t>(PacketId::BlockAction));
            bw.write_u8(static_cast<uint8_t>(p.id));
            bw.write_u8(static_cast<uint8_t>(kDestroyBlock));
            bw.write_i32(tx);
            bw.write_i32(ty);
            bw.write_i32(tz);
            broadcast(bw, true);
            p.spade_at = world_time;
            show_swing(p, horde_cfg.dig_seconds);
            return true;
        }
        // A step, or a drop: walked, not spaded.
        p.input.mf = true;
        return true;
    }

    // A zombie's mind: its pack walks one path - it holds at the heel of the
    // zombie in front, and the one at the head reads the field, towards the
    // core. The field is the walk without the spade - stairs, ramps and
    // notches, never a wall hacked through - and a stuck one jumps for the
    // step it could not make.
    void steer_zombie(Player& p) {
        p.input = physics::Input{};
        const int cx = static_cast<int>(std::floor(p.body.pos.x));
        const int cy = static_cast<int>(std::floor(p.body.pos.y));
        // The floor its feet are on: the lowest probe of the standing box,
        // truncated to the voxel - the same reading the surfaces are. The
        // advice is taken from HERE, so the field is read from the ground
        // the zombie truly stands on, not from whatever bargain its column
        // keeps under it; the yo-yo of a leader sent after a floor its
        // legs can never reach is advised out of existence.
        const int fz = static_cast<int>(std::floor(p.body.pos.z + 2.25f));
        // The print of the heel: every half-pace of ground truly stood
        // on is set into the ring, in the order it was walked. Nothing
        // is printed in the air - a flung body crosses ground no
        // following leg may tread; the next print says where it walked
        // on from wherever it came down.
        if (!p.body.airborne) {
            const float prx = p.body.pos.x - p.heel_ref_x;
            const float pry = p.body.pos.y - p.heel_ref_y;
            if (prx * prx + pry * pry >= 0.25f) {
                p.heel_x[p.heel_head] = p.body.pos.x;
                p.heel_y[p.heel_head] = p.body.pos.y;
                p.heel_seq[p.heel_head] = heel_stamp();
                p.heel_ref_x = p.body.pos.x;
                p.heel_ref_y = p.body.pos.y;
                p.heel_head = (p.heel_head + 1) % Player::kHeelN;
            }
        }
        // The crowd: players pass through players in this game, so the horde
        // keeps itself apart - a soft push off any of its own inside one
        // block of reach, the weight fading with the room between them.
        // A fresh one is spared for its first seconds: it walks out of the
        // pile through its own, and they walk through it, until the wave
        // has its shape back.
        Vec3 sep{};
        for (const auto& qq : players) {
            if (!qq || qq.get() == &p || !qq->zombie || !qq->alive) continue;
            if (world_time - qq->born_at < kZombieSettle) continue;
            const float dx = p.body.pos.x - qq->body.pos.x;
            const float dy = p.body.pos.y - qq->body.pos.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 < 1e-6f) {   // stood in the same skin: part by number
                sep.x += (p.id & 1) != 0 ? 0.5f : -0.5f;
                sep.y += (p.id & 2) != 0 ? 0.5f : -0.5f;
                continue;
            }
            if (d2 >= 1.0f) continue;
            const float d = std::sqrt(d2);
            const float w = 1.0f - d;
            sep.x += dx / d * w;
            sep.y += dy / d * w;
        }
        if (horde->near_core(cx, cy, 2)) {
            // It bites, it does not walk - but it still shuffles its
            // neighbours out of its skin.
            const float sl = std::sqrt(sep.x * sep.x + sep.y * sep.y);
            if (sl > 0.15f) {
                p.body.ori = Vec3{sep.x / sl, sep.y / sl, 0.0f};
                p.input.mf = true;
            }
            return;
        }
        const Player* ahead =
            p.ahead_id >= 0 ? players[static_cast<size_t>(p.ahead_id)].get() : nullptr;
        if (ahead && ahead->id != p.id && ahead->zombie && ahead->alive) {
            // The heel held in the footsteps of the one ahead: a follower
            // is led by the prints its leader left - the threshold it
            // crossed, the wall it frayed along, the corner it took -
            // and never by the open air's own straight line, which
            // shaves the bend a threshold sat in and leaves the end of
            // the line stranded on the wrong side of a step the leader
            // never even saw. A spade's length of leeway once the wave
            // has its shape: close enough to hold the line, far enough
            // to let the one in front swing. A fresh one walks at the
            // heel instead - a block behind at most - for its first
            // seconds are the pile's, and the pile leaves as one body
            // or it stands and stands while the gap it waits on opens a
            // spade's length at a time.
            const float adx = ahead->body.pos.x - p.body.pos.x;
            const float ady = ahead->body.pos.y - p.body.pos.y;
            const float l = std::sqrt(adx * adx + ady * ady);
            const float leash = world_time - p.born_at < kZombieSettle ? 1.0f : 1.6f;
            const int print = ahead_print(*ahead, p.body.pos.x, p.body.pos.y);
            // The strayed one takes the pack's head's read: a print owed
            // behind or abreast, or nothing owed with the line's end past
            // the ring's whole depth, means the walker is off the trail -
            // and doubling back for the miss would engrave it into the
            // trail for every heel behind to walk again. So it reads the
            // field instead, the walk the whole of it shares, never the
            // open air's own bearing, and the corridor it marches is the
            // one the line marches: the nearest print ahead stands
            // unmasked, and the chase resumes on its own.
            bool strayed = print < 0 && l > leash;
            if (print >= 0 && !strayed) {
                const float dx = ahead->heel_x[print] - p.body.pos.x;
                const float dy = ahead->heel_y[print] - p.body.pos.y;
                const float dl = std::sqrt(dx * dx + dy * dy);
                // Squared against the bearing to the one it follows: the
                // cosine of the angle between the print's pull and the
                // line's own pull, cheap and free of any square root the
                // chase below has not already paid.
                if (dl > 1e-3f &&
                    (l <= 1e-3f ||
                     (dx * adx + dy * ady) / (dl * l) >= kPrintAside)) {
                    p.body.ori = Vec3{dx / dl, dy / dl, 0.0f};
                    p.input.mf = true;
                    // The catch: the run that draws a strung-out follower
                    // back onto its leader's heels - kept off the stairs
                    // and banks, where a run cannot take its own step.
                    p.input.sprint = l > kPackCatch &&
                                    std::fabs(ahead->body.pos.z - p.body.pos.z) < 0.5f;
                } else {
                    strayed = true;
                }
            }
            if (strayed) {
                int tx = cx, ty = cy;
                // The field read from the footing, as the head reads it -
                // and, the same mercy, the cell's own read in the air.
                const bool led =
                    (!p.body.airborne && horde->next_step(cx, cy, fz, tx, ty)) ||
                    horde->next_step(cx, cy, tx, ty);
                if (led) {
                    // The centre of the cell, never its corner.
                    const float tdx = static_cast<float>(tx) + 0.5f -
                                      p.body.pos.x;
                    const float tdy = static_cast<float>(ty) + 0.5f -
                                      p.body.pos.y;
                    const float tdl = std::sqrt(tdx * tdx + tdy * tdy);
                    if (tdl > 1e-3f) {
                        p.body.ori = Vec3{tdx / tdl, tdy / tdl, 0.0f};
                        p.input.mf = true;
                        p.input.sprint = l > kPackCatch &&
                                        std::fabs(ahead->body.pos.z - p.body.pos.z) < 0.5f;
                    }
                } else if (dig_lead(p, false)) {
                    // The field had nothing and the spade's walk had a
                    // way: the strayed one walks it - and where it
                    // climbs, it is the pack's head's work to open, not
                    // a heel's; the heel waits at the wall, held on the
                    // line of prints it still owes.
                } else if (print >= 0) {
                    // The field silent - at the core's own cell it always
                    // is - the print is paid rather than the walker stand:
                    // the walker stood on the walk, the trail holds it.
                    const float dx = ahead->heel_x[print] - p.body.pos.x;
                    const float dy = ahead->heel_y[print] - p.body.pos.y;
                    const float dl = std::sqrt(dx * dx + dy * dy);
                    if (dl > 1e-3f) {
                        p.body.ori = Vec3{dx / dl, dy / dl, 0.0f};
                        p.input.mf = true;
                        p.input.sprint = l > kPackCatch &&
                                        std::fabs(ahead->body.pos.z - p.body.pos.z) < 0.5f;
                    }
                } else if (l > leash) {
                    // Off the ring's depth with the field silent as well:
                    // the air line remains, a slow walk over broken ground
                    // still beats a standstill at the line's very end.
                    p.body.ori = Vec3{adx / l, ady / l, 0.0f};
                    p.input.mf = true;
                    p.input.sprint = l > kPackCatch &&
                                    std::fabs(ahead->body.pos.z - p.body.pos.z) < 0.5f;
                } else {
                    p.body.ori = ahead->body.ori;
                }
            } else if (print < 0) {
                p.body.ori = ahead->body.ori;
            }
        } else {
            // Nobody to follow: it is the pack's leader now, and the field
            // leads it - around the cliffs, when it can.
            p.ahead_id = -1;
            int tx = cx, ty = cy;
            // The field read from the footing - and when the feet are in
            // the air, or on a floor the walk has no opinion about, the
            // cell's own read instead, the one it has always had.
            const bool led =
                (!p.body.airborne && horde->next_step(cx, cy, fz, tx, ty)) ||
                horde->next_step(cx, cy, tx, ty);
            // The spade's second opinion, taken when it pays: the dig
            // walk's way home is worth the lead when the legs' own is
            // silent underfoot or longer by more than the spade's price,
            // and where it says climb, the head digs and the line waits.
            const bool digs = dig_lead(p, led);
            if (led && !digs) {
                // The cell handed back is a square the legs are steered into,
                // so it is its CENTRE they make for - never its corner. A
                // corner is the point four cells meet at, a line's width of
                // nowhere that belongs to no step: a walker aimed there
                // crosses the boundary as it arrives, becomes the next cell's
                // to advise, is handed a corner its own side of the line,
                // and paces across the crossing forever - there and back,
                // moving all the while, never a cell nearer. The centre is
                // one cell's own ground: once set foot in, the next advice
                // is read from inside it, and the walk goes on.
                const float dx = static_cast<float>(tx) + 0.5f - p.body.pos.x;
                const float dy = static_cast<float>(ty) + 0.5f - p.body.pos.y;
                const float l = std::sqrt(dx * dx + dy * dy);
                if (l > 1e-3f) {
                    p.body.ori = Vec3{dx / l, dy / l, 0.0f};
                    p.input.mf = true;
                }
            }
        }
        // The shuffle apart, folded into the facing: a walker veers a step
        // out of a knot, a standstill in reach of its own becomes the
        // shuffle outright. Politeness stands aside for a stuck walker -
        // shuffling one off a wall it leans on would read as progress and
        // unarm the hop, and the hop is the only way out it has. It stands
        // aside for the pack's head too: the field leads it, and the crowd
        // it parts must never bend it, for the line behind goes where it
        // goes. And it has not begun for a fresh one: it is still walking
        // out of the pile it rose in.
        const float sl = std::sqrt(sep.x * sep.x + sep.y * sep.y);
        if (p.ahead_id >= 0 && world_time - p.born_at >= kZombieSettle &&
            sl > 0.15f && p.stuck_at < 0.0) {
            const float ox = p.body.ori.x + sep.x * 0.7f;
            const float oy = p.body.ori.y + sep.y * 0.7f;
            const float ol = std::sqrt(ox * ox + oy * oy);
            if (ol > 1e-3f) {
                p.body.ori = Vec3{ox / ol, oy / ol, 0.0f};
                p.input.mf = true;
            }
        }
        // The unstick. Progress is measured flat, over the ground it stands
        // on: a hop against a step gains none, so the jump does not wait on
        // where in its bounce the clock samples it. The walk's own answer to
        // a standstill is the hop; the spade takes walls down only where
        // the dig walk says the price is paid in. And pacing is a
        // standstill to the walk - the yo-yo of a leader that crosses to a
        // floor it cannot hold and turns back, over and over, moves a
        // window's worth in every window and so slips the moved measure
        // whole. So the leader is scored by the field itself: the distance
        // read at its feet, kept window by window, and windows without a
        // lower floor underfoot are counted until they are the yo-yo's own
        // three seconds - then the same hop and the same fling are owed,
        // for whatever the pacing faces is the thing to vault, and the
        // vault is the only way out a pacer has. A follower keeps no
        // score: holding a heel's place in the line stirs on the spot by
        // design.
        // The unstick, shelved while the spade works: a digger stands by
        // design, and its standstill is the pace of its own work - the
        // windows wait out each stroke's dig_seconds before they score
        // again, so no hop and no fling spoils the work at the wall.
        if (world_time - p.walk_ref_at >= 0.5 &&
            world_time - p.spade_at >= horde_cfg.dig_seconds) {
            const float moved = std::fabs(p.body.pos.x - p.walk_ref.x) +
                                std::fabs(p.body.pos.y - p.walk_ref.y);
            p.walk_ref = p.body.pos;
            p.walk_ref_at = world_time;
            const int fd =
                p.body.airborne ? -1 : horde->distance(cx, cy, fz);
            if (p.ahead_id >= 0 || fd < 0) {
                p.foot_dist = -1;          // off the walk or a step in line
                p.stall_windows = 0;
            } else if (p.foot_dist < 0 || fd < p.foot_dist) {
                p.foot_dist = fd;          // a lower floor reached: it walks
                p.stall_windows = 0;
            } else {
                ++p.stall_windows;         // moving, maybe - nearer, never
            }
            if (moved < 0.35f ||
                p.stall_windows >= kZombieYoyoWindows) {
                // A heel held is a place kept, not a standstill: within
                // the sight of the one ahead, the window is shelved.
                if (ahead && ahead->id != p.id && ahead->zombie &&
                    ahead->alive) {
                    const float sdx = ahead->body.pos.x - p.body.pos.x;
                    const float sdy = ahead->body.pos.y - p.body.pos.y;
                    if (sdx * sdx + sdy * sdy <
                        kHeelSight * kHeelSight) {
                        p.stuck_at = -1.0;
                        return;
                    }
                }
                if (p.stuck_at < 0.0) p.stuck_at = world_time;
                // The fling. The reading crosses steps two blocks tall at
                // the price of a jump, and a jump alone was never going to
                // clear one pressed against its face - the hop rises a
                // block and a half and the wall takes the rest out of it.
                // So a walker still standing after what its own hop could
                // give lunges at the step instead: thrown clear of what it
                // faces, high enough to land standing on it, shoulder to
                // the way out until the next try. Every walker has the
                // same fling in it, the pack's head as much as its heel:
                // where the leader is flung the whole line goes after.
                //
                // The fling is the whole jump of its tick: the step's own
                // jump gate writes its smaller hop over any velocity the
                // key asks with, so on the tick the lunge flies the key
                // is held back - flung once, the hop resumes next window.
                bool flung = false;
                if (world_time - p.stuck_at >= kPounceWindup &&
                    world_time - p.pounce_at >= kPounceAgain && !p.body.airborne) {
                    p.body.vel.z = kDeadPounce;
                    p.body.vel.x += p.body.ori.x * 0.15f;
                    p.body.vel.y += p.body.ori.y * 0.15f;
                    p.pounce_at = world_time;
                    flung = true;
                }
                if (!flung) p.input.jump = true;
            } else {
                p.stuck_at = -1.0;
            }
        }
    }
    // Teeth: the core first, any person in reach second. A bite a second.
    void zombie_bite(Player& p) {
        if (world_time < p.next_bite) return;
        const int cx = static_cast<int>(std::floor(p.body.pos.x));
        const int cy = static_cast<int>(std::floor(p.body.pos.y));
        bool bit = false;
        if (horde->near_core(cx, cy, 2)) {
            horde->on_core_damaged(10);
            bit = true;
            if (world_time >= core_alarm_at) {
                core_alarm_at = world_time + 4.0;
                char text[96];
                std::snprintf(text, sizeof text, "The core is under attack! Core %d",
                             horde->core_hp());
                system_chat(text);
            }
        }
        for (auto& qq : players) {
            if (!qq || qq->zombie || !qq->alive || qq->spectator() || qq->team != kPeople) continue;
            if (!near3(p.body.pos, qq->body.pos, kMeleeDistance)) continue;
            hit(*qq, 34, &p, kMeleeKill);
            bit = true;
            break;
        }
        if (bit) {
            p.next_bite = world_time + 0.8;
            show_swing(p, 0.4);   // the teeth the clients animate
        }
    }
    // Where a wave comes from, while it comes: the same ping the tutorial's
    // goals use, re-sent for a corner every few seconds it still streams -
    // one marker at the pack's spawn, held long enough to be walked to,
    // renewed so a client mid-load still sees it once it lands.
    void ping_wave_spawn(const horde::SpawnAt& s) {
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::Teamplay));
        bw.write_u8(1);
        bw.write_u8(255);
        bw.write_vec(Vec3{static_cast<float>(s.x) + 0.5f, static_cast<float>(s.y) + 0.5f,
                          static_cast<float>(s.z) - 1.0f});
        bw.write_f32(20.0f);
        bw.write_u8(1 | 2);
        write_color(bw, 0x50FF50);
        bw.write_u8(0);
        const char* text = "The wave comes from here";
        for (const char* c = text; *c; ++c) bw.write_u8(static_cast<uint8_t>(*c));
        for (auto& o : players)
            if (o && o->peer && !o->loading && o->ext_teamplay && o->playing()) send(*o, bw);
    }

    // The way in, once over: the reading the vapour stands on - the
    // traced route of the wave's ranked-first corner, from its anchor to
    // the core's doorstep, or nothing when no corner anchors. While the
    // wave walks, the reading begins instead at the feet of the walk's
    // own head - the leader that has come nearest the core, on the
    // ground it stands on: with every beat the trail is re-said from
    // where the walk has got to, the part behind said no more, and the
    // vapour the leader has already crossed thins out on its own - the
    // trail looks less like a plan drawn ahead of time and more like a
    // thing pouring out of the walker's own feet, live.
    bool route_reading(std::vector<horde::RouteCell>& cells) {
        cells.clear();
        if (!horde) return false;
        if (horde->phase() == horde::Phase::Wave) {
            const Player* head = nullptr;
            int head_fd = -1;
            for (auto& q : players) {
                if (!q || !q->zombie || !q->alive || q->ahead_id >= 0) continue;
                if (q->body.airborne) continue;    // no print is set in the air
                const int qx = static_cast<int>(std::floor(q->body.pos.x));
                const int qy = static_cast<int>(std::floor(q->body.pos.y));
                const int qz = static_cast<int>(std::floor(q->body.pos.z + 2.25f));
                const int fd = horde->distance(qx, qy, qz);
                if (fd < 0 || (head != nullptr && fd >= head_fd)) continue;
                head = q.get();
                head_fd = fd;
            }
            if (head != nullptr) {
                if (horde->trace_route(static_cast<int>(std::floor(head->body.pos.x)),
                                       static_cast<int>(std::floor(head->body.pos.y)),
                                       cells) &&
                    !cells.empty())
                    return true;
                // The sealed way in: the spade's own reading, painted the
                // same, that the defenders see where the fences will be
                // gnawed.
                if (horde_cfg.dig &&
                    horde->trace_dig(static_cast<int>(std::floor(head->body.pos.x)),
                                     static_cast<int>(std::floor(head->body.pos.y)),
                                     cells) &&
                    !cells.empty())
                    return true;
            }
        }
        int ax = 0, ay = 0;
        if (!horde->corner_anchor(horde->ranked_corner(0), ax, ay)) return false;
        if (!horde->trace_route(ax, ay, cells) && horde_cfg.dig)
            horde->trace_dig(ax, ay, cells);   // the sealed wave's way: over the walls it will take down
        return !cells.empty();
    }

    // The way in, said as heavy vapour - the only word it is said in: a
    // continuous bed of it laid along the walk's full reading, one bed
    // every couple of treads so the trail reads unbroken from the wave's
    // anchor to the core's doorstep, over the dressed ground and across
    // the world's bed alike - the legs walk the bedrock as freely as any
    // ground, and no wave crosses unwarned. Each bed sits at its tread's
    // floor, a cloud hardly a block across that pools rather than
    // climbs, kept coming for its whole stay so the bed stays fresh;
    // its id is its place in the trail, so a resend lays its vapour
    // over the old in place, and a reading that no longer walks a place
    // simply stops saying it - the vapour hung there thins out on its
    // own, no clearing sweep to keep a stale way in on the air. The
    // colour blinks vivid orange and neon blue, a beat to the breath:
    // the one marker a defender cannot mistake for smoke of their own.
    // Sound it carries none: a line of hisses is a racket, not a marker.
    void smoke_route() {
        std::vector<horde::RouteCell> cells;
        if (!route_reading(cells)) return;
        constexpr int kStride = 2;          // a bed every couple of treads: a trail that reads unbroken
        constexpr int kMaxBeds = 240;       // ids 1..240, clear of 255
        constexpr uint16_t kStay = 7;       // 0.7 s it keeps coming: the heavier pour, one blink's worth and a little over
        constexpr uint8_t kThins = 2;       // 0.2 s the last of it takes to go: it leaves like it settles, slowly
        // Whose beat it is: vivid orange on the even breaths, neon blue on
        // the odd, read off the clock so every resend agrees with the last.
        const bool blue =
            static_cast<int64_t>(world_time / kRouteColour) % 2 != 0;
        const uint8_t r = blue ? 0x00 : 0xFF, g = blue ? 0xC8 : 0x80, b = blue ? 0xFF : 0x00;
        int bed = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i % kStride != 0 && i + 1 != cells.size()) continue;  // the last tread is said whatever rhythm it falls on
            if (bed >= kMaxBeds) break;
            const horde::RouteCell& c = cells[i];
            ++bed;
            ByteWriter bw;
            proto::write_smoke(bw, static_cast<uint8_t>(bed), proto::SmokeStyle::Ground,
                               Vec3{static_cast<float>(c.x) + 0.5f,
                                    static_cast<float>(c.y) + 0.5f,
                                    static_cast<float>(c.z) - 0.35f},
                               static_cast<uint8_t>(1), r, g, b, 0xE8,
                               kStay, kThins, static_cast<uint8_t>(0));
            for (auto& o : players)
                if (o && o->peer && !o->loading && o->ext_smoke && o->playing()) send(*o, bw);
        }
    }

    // The test bench's broom: every walker of the standing wave put down,
    // the core mended whole, and a defeat called off with it - the world
    // returns to its waiting, and the next wave comes when the wait is
    // over. The walkers fall where they stand, their slots go back, and
    // the wave the clock had coming is owed nobody: the counting starts
    // again at the next one.
    void clear_wave() {
        const bool defeated = horde->phase() == horde::Phase::Defeat;
        int down = 0;
        for (auto& qq : players)
            if (qq && qq->zombie && qq->alive) {
                kill(*qq, nullptr, kFallKill);
                ++down;
            }
        horde->clear_wave(world_time);
        if (horde_mode() && !cfg.map_file.empty()) build_core_column();
        char text[96];
        std::snprintf(text, sizeof text, "%d down; the core mends to %d",
                      down, horde->core_hp());
        system_chat(text);
        if (defeated)
            system_chat("The defeat is called off; the next wave is coming.");
    }

    // The wave's announcements, its spawns, and its dead slots given back.
    void horde_tick() {
        horde->tick(world_time);
        const horde::Phase ph = horde->phase();
        if (ph != horde_told) {
            horde_told = ph;
            switch (ph) {
                case horde::Phase::Wave:
                    if (horde->wave() > horde_wave_told) {
                        horde_wave_told = horde->wave();
                        // New packs, their pings due right away, and the
                        // way in's vapour laid before the first tread is walked.
                        for (int k = 0; k < horde::Config::kSpawnSites; ++k) {
                            pack_tail[k] = -1;
                            corner_ping_next[k] = 0.0;
                        }
                        route_smoke_next = 0.0;
                        char text[96];
                        if (horde->placed() == 0) {
                            // The walk priced every corner and none of them
                            // stands on it: without a spade there is no way
                            // in, and saying so beats a wave that clears
                            // itself in silence.
                            std::snprintf(text, sizeof text,
                                          "Wave %d: no way in the walk can take",
                                          horde->wave());
                        } else {
                            std::snprintf(text, sizeof text, "Wave %d: %d zombies coming",
                                          horde->wave(), horde->placed());
                        }
                        system_chat(text);
                    }
                    break;
                case horde::Phase::Intermission: {
                    route_smoke_next = 0.0;  // the next wave's way in, said again at once
                    char text[96];
                    std::snprintf(text, sizeof text, "Wave cleared; the core mends to %d",
                                  horde->core_hp());
                    system_chat(text);
                    break;
                }
                case horde::Phase::Victory:
                    system_chat("Every wave is down. Victory!");
                    break;
                case horde::Phase::Defeat:
                    system_chat("The core has fallen. Defeat.");
                    break;
                case horde::Phase::Prep:
                    route_smoke_next = 0.0;   // a restarted world says its way in at once
                    break;
            }
        }
        // The way in said in vapour, every blink: the trail on the ground
        // is the one marker of where the dead walk, so while the world
        // waits for them and while they walk it alike, the reading is
        // traced and the beds laid again each beat - the world is built
        // on under the reading's feet, and the vapour follows the true
        // way in, over the dressed ground and across the bedrock the
        // legs walk as freely as any ground.
        const bool waiting = ph == horde::Phase::Prep || ph == horde::Phase::Intermission;
        if ((waiting || ph == horde::Phase::Wave) && world_time >= route_smoke_next) {
            route_smoke_next = world_time + kRouteBlink;
            smoke_route();
        }
        std::vector<horde::SpawnAt> due;
        while (horde->take_due(due, world_time)) {
            for (const horde::SpawnAt& s : due) {
                // While the point streams, its spawn is pinged again every
                // so often: the marker survives a client's map load.
                const int c = s.corner;
                if (world_time >= corner_ping_next[c]) {
                    corner_ping_next[c] = world_time + 30.0;
                    ping_wave_spawn(s);
                }
                add_zombie(s);
            }
        }
        free_dead_zombies();
    }

    // Whether `who` is looking at `t`: their view's ray passing within a body's
    // half width of it, from the eye down to the knees.
    static bool aimed_at(const Player& who, const Player& t) {
        const Vec3& o = who.body.pos;
        const Vec3& d = who.body.ori;
        for (int i = 0; i <= 4; ++i) {
            const Vec3 c{t.body.pos.x, t.body.pos.y, t.body.pos.z + 0.45f * static_cast<float>(i)};
            const Vec3 v{c.x - o.x, c.y - o.y, c.z - o.z};
            const float along = v.x * d.x + v.y * d.y + v.z * d.z;
            if (along <= 0.0f) continue;
            const Vec3 q{v.x - d.x * along, v.y - d.y * along, v.z - d.z * along};
            if (q.x * q.x + q.y * q.y + q.z * q.z < 0.6f * 0.6f) return true;
        }
        return false;
    }
    void reset_run() {
        const int run = aim.run + 1;
        aim = training::AimStatus{};
        aim.scenario = cfg.scenario;
        aim.run = run;
        aim.phase = training::AimPhase::Waiting;
        for (auto& pp : players) {
            if (!pp || !pp->aim_bot) continue;
            place_aim_bot(*pp, false);
            if (pp->alive) {
                pp->alive = false;   // back at a fresh spot at once, without a kill
                spawn(*pp);
            } else {
                pp->respawn_at = world_time;
            }
        }
    }
    void aim_tick(float dt) {
        using training::AimPhase;
        // The client's restart, or a run nobody is left to play.
        bool restart = false;
        if (cfg.aim) {
            std::lock_guard<std::mutex> lk(cfg.aim->mutex);
            restart = cfg.aim->restart;
            cfg.aim->restart = false;
        }
        Player* who = person();
        if (restart) {
            reset_run();
            if (who && who->alive) spawn(*who);  // back on the spot
        }
        if (!who && aim.phase != AimPhase::Waiting) reset_run();
        switch (aim.phase) {
            case AimPhase::Waiting:
                if (who && who->alive) {
                    aim.phase = AimPhase::Countdown;
                    aim.weapon = who->weapon;
                    phase_until = world_time + training::kCountdownSeconds;
                }
                break;
            case AimPhase::Countdown:
                aim.countdown = static_cast<float>(std::max(0.0, phase_until - world_time));
                if (world_time >= phase_until) {
                    aim.phase = AimPhase::Running;
                    phase_until = world_time + training::kRunSeconds;
                    shots_base = who ? who->gun.fired_by(world_time) : 0;
                    // The targets' clocks start with the run.
                    for (auto& pp : players)
                        if (pp && pp->aim_bot) pp->appeared_at = world_time;
                }
                break;
            case AimPhase::Running:
                aim.elapsed += dt;
                aim.time_left = static_cast<float>(std::max(0.0, phase_until - world_time));
                if (who) {
                    aim.shots = std::max(0, who->gun.fired_by(world_time) - shots_base);
                    if (who->alive && cfg.scenario == training::Scenario::Tracking) {
                        for (auto& pp : players)
                            if (pp && pp->aim_bot && pp->alive && aimed_at(*who, *pp)) {
                                aim.on_target += dt;
                                break;
                            }
                    }
                }
                if (world_time >= phase_until) {
                    aim.phase = AimPhase::Finished;
                    aim.time_left = 0.0f;
                    log("aim run %d over: %d kills, %d of %d shots hit", aim.run, aim.kills, aim.hits, aim.shots);
                }
                break;
            case AimPhase::Finished: break;
        }
        if (cfg.aim) {
            std::lock_guard<std::mutex> lk(cfg.aim->mutex);
            cfg.aim->status = aim;
        }
    }

    // --- Teamplay from the server: pings and marks --------------------------
    void send_ping(const Vec3& at, float duration, uint32_t rgb, const std::string& reason) {
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::Teamplay));
        bw.write_u8(1);                       // Ping
        bw.write_u8(255);                     // the server's own
        bw.write_vec(at);
        bw.write_f32(duration);
        bw.write_u8(1 | 2);                   // in the world and on the minimap
        write_color(bw, rgb);
        bw.write_u8(0);
        for (char ch : reason) bw.write_u8(static_cast<uint8_t>(ch));
        for (auto& o : players)
            if (o && o->peer && !o->loading && o->ext_teamplay) send(*o, bw);
    }
    void send_mark(const Player& target) {
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::Teamplay));
        bw.write_u8(2);                       // ESP Mark
        bw.write_u8(static_cast<uint8_t>(target.id));
        bw.write_f32(std::numeric_limits<float>::infinity());
        bw.write_u8(1);                       // the body's outline
        bw.write_u8(0);
        write_color(bw, 0x20FF60);
        bw.write_u8(0);
        for (auto& o : players)
            if (o && o->peer && !o->loading && o->ext_teamplay) send(*o, bw);
    }

    // --- the tutorial's pings: where the next step is -------------------------
    void tutorial_tick() {
        if (world_time < next_tutorial_check) return;
        next_tutorial_check = world_time + 0.25;
        using namespace training;
        for (auto& pp : players) {
            if (!pp || !pp->peer || !pp->playing() || !pp->ext_teamplay) continue;
            Player& p = *pp;
            for (int st = 0; st < StepCount; ++st)
                if (p.alive && step_reached(st, p.body.pos.x, p.kills)) p.steps_done |= 1 << st;
            int next = -1;
            for (int st = 0; st < StepCount && next < 0; ++st)
                if (!(p.steps_done & (1 << st))) next = st;
            if (next == p.pinged_step) continue;
            p.pinged_step = next;
            struct Goal { float x, z; const char* what; };
            const float up = static_cast<float>(kFloorZ - kLedgeRise);
            const Goal goals[StepCount] = {
              {static_cast<float>(kRange.x0 + 6), kFloorZ - 1.0f, "Walk to the range"},
              {static_cast<float>(kLaneX[0]), kFloorZ - 2.5f, "Shoot 3 targets"},
              {static_cast<float>(kDigWall.x0), kFloorZ - 2.0f, "Dig through"},
              {static_cast<float>(kLedgeX + 1), up - 1.0f, "Build up here"},
              {static_cast<float>(kPlug.x0), up - 1.5f, "Blow it open"},
              {static_cast<float>(kFinish.x0 + 6), up - 1.0f, "Finish"},
            };
            ByteWriter bw;
            bw.write_u8(static_cast<uint8_t>(PacketId::Teamplay));
            bw.write_u8(1);
            bw.write_u8(255);
            if (next < 0) {
                bw.write_vec(Vec3{0, 0, 0});
                bw.write_f32(0.0f);           // all done: no more ping
            } else {
                bw.write_vec(Vec3{goals[next].x, 256.5f, goals[next].z});
                bw.write_f32(std::numeric_limits<float>::infinity());
            }
            bw.write_u8(1 | 2);
            write_color(bw, 0xFFC830);
            bw.write_u8(0);
            if (next >= 0)
                for (const char* c = goals[next].what; *c; ++c) bw.write_u8(static_cast<uint8_t>(*c));
            send(p, bw);
        }
    }

    // --- the world --------------------------------------------------------
    void world_tick() {
        const float dt = static_cast<float>(kTick);
        for (auto& pp : players) {
            if (!pp) continue;
            Player& p = *pp;
            if (p.gun.update(world_time)) {
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::WeaponReload));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(static_cast<uint8_t>(std::max(0, p.gun.ammo)));
                bw.write_u8(static_cast<uint8_t>(std::max(0, p.gun.stock)));
                send(p, bw);
            }
            if (!p.alive) {
                if (p.respawn_at >= 0 && world_time >= p.respawn_at) spawn(p);
                continue;
            }
            if (!p.playing()) continue;
            if (p.lane >= 0) steer_target(p);
            if (p.aim_bot) steer_aim_bot(p);
            if (p.zombie) {
                steer_zombie(p);
                // What a player's own client would tell everybody, the horde
                // tells for it: the clients animate its walk by it.
                const uint8_t f = (p.input.mf ? 1u : 0u) | (p.input.mb ? 2u : 0u) |
                                  (p.input.ml ? 4u : 0u) | (p.input.mr ? 8u : 0u) |
                                  (p.input.jump ? 16u : 0u);
                if (f != p.net_input) {
                    p.net_input = f;
                    ByteWriter bw;
                    bw.write_u8(static_cast<uint8_t>(PacketId::InputData));
                    bw.write_u8(static_cast<uint8_t>(p.id));
                    bw.write_u8(static_cast<uint8_t>(f));
                    broadcast(bw, false);
                }
                if (p.swing_shown && world_time >= p.swing_until) {
                    p.swing_shown = false;
                    ByteWriter bw;
                    bw.write_u8(static_cast<uint8_t>(PacketId::WeaponInput));
                    bw.write_u8(static_cast<uint8_t>(p.id));
                    bw.write_u8(0);
                    broadcast(bw, false);
                }
            }
            keep_stocked(p);
            p.input.holding_gun = p.tool == kToolWeapon;
            p.input.secondary_fire = p.secondary;
            const physics::StepResult r = physics::step(phys_map, p.body, p.input, dt);
            p.input.jump = false;
            // The fallen have no fear of falling: a cliff on the map's rim is
            // a walk down, not a death.
            if (r.fall_damage > 0 && !p.zombie)
                set_hp(p, p.hp - r.fall_damage, nullptr, kFallKill, nullptr);
            if (!p.alive) continue;
            if (p.zombie) zombie_bite(p);
            if (p.body.pos.z > 64.0f || !finite3(p.body.pos)) kill(p, nullptr, kFallKill);
        }
        if (aim_mode()) aim_tick(dt);
        else if (horde_mode()) horde_tick();
        else tutorial_tick();
        for (size_t i = 0; i < grenades.size();) {
            if (world_time >= grenades[i].explode_at) {
                const Grenade g = grenades[i];
                grenades.erase(grenades.begin() + static_cast<long>(i));
                explode(g);
            } else {
                ++i;
            }
        }
    }

    void explode(const Grenade& g) {
        Player* owner = players[g.owner].get();
        if (!owner || !owner->named || owner->team != g.team || owner->spectator()) return;
        const Vec3& pos = g.pos;
        if (pos.x < 0 || pos.x > 512 || pos.y < 0 || pos.y > 512 || pos.z < 0 || pos.z > 63) return;
        for (auto& pp : players) {
            if (!pp || !pp->alive) continue;
            if (pp.get() != owner && pp->team == owner->team) continue;
            const float dmg = nade::blast_damage(&phys_map, pos, pp->body.pos);
            if (static_cast<int>(dmg) <= 0) continue;
            set_hp(*pp, pp->hp - static_cast<int>(dmg), owner, kGrenadeKill, &pos);
        }
        const int x = static_cast<int>(std::floor(pos.x));
        const int y = static_cast<int>(std::floor(pos.y));
        const int z = static_cast<int>(std::floor(pos.z));
        std::vector<Cell> removed;
        for (int nx = x - 1; nx <= x + 1; ++nx)
            for (int ny = y - 1; ny <= y + 1; ++ny)
                for (int nz = z - 1; nz <= z + 1; ++nz) {
                    if (core_column_cell(nx, ny, nz)) continue;  // the core holds
                    map.destroy_point(nx, ny, nz, &removed);
                }
        touched(removed, false);
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::BlockAction));
        bw.write_u8(static_cast<uint8_t>(owner->id));
        bw.write_u8(kGrenadeDestroy);
        bw.write_i32(x);
        bw.write_i32(y);
        bw.write_i32(z);
        broadcast(bw, true);
    }

    void world_update() {
        int highest = -1;
        for (int i = 0; i < kSlots; ++i)
            if (players[i] && players[i]->named) highest = i;
        if (highest < 0) return;
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::WorldUpdate));
        for (int i = 0; i <= highest; ++i) {
            const Player* p = players[i].get();
            if (p && p->named && p->playing()) {
                bw.write_vec(p->body.pos);
                bw.write_vec(p->body.ori);
            } else {
                bw.write_vec(Vec3{0, 0, 0});
                bw.write_vec(Vec3{0, 0, 0});
            }
        }
        ENetPacket* pkt = nullptr;
        for (auto& pp : players) {
            if (!pp || pp->loading || !pp->peer) continue;
            if (!pkt) pkt = make_packet(bw, false);
            enet_peer_send(pp->peer, 0, pkt);
        }
        if (pkt && pkt->referenceCount == 0) enet_packet_destroy(pkt);
    }

    // --- packets from clients ---------------------------------------------
    void on_packet(Player& p, const uint8_t* data, size_t len) {
        if (len < 1) return;
        ByteReader br(data + 1, len - 1);
        const double now = world_time;
        switch (static_cast<PacketId>(data[0])) {
            case PacketId::ExistingPlayer: on_join(p, br); break;
            case PacketId::VersionHandshakeResponse: {
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::VersionGet));
                send(p, bw);
            } break;
            case PacketId::VersionResponse: {
                p.client_info = true;
                // Offered: kick reasons, Teamplay, the lights (flashlights and
                // the time of day), message types, the outfits and the smoke.
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::ExtensionInfo));
                bw.write_u8(7);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::ChatType));
                bw.write_u8(1);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::Teamplay));
                bw.write_u8(1);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::KickReason));
                bw.write_u8(1);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::Flashlight));
                bw.write_u8(1);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::DaytimeWeather));
                bw.write_u8(1);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::PlayerOutfit));
                bw.write_u8(1);
                bw.write_u8(static_cast<uint8_t>(proto::Extension::Smoke));
                bw.write_u8(1);
                send(p, bw);
            } break;
            case PacketId::ExtensionInfo: {
                proto::ExtensionInfoMsg m;
                if (!proto::read_extension_info(br, m)) break;
                const bool tp = m.offers(proto::Extension::Teamplay);
                if (tp && !p.ext_teamplay) {
                    ByteWriter bw;
                    bw.write_u8(static_cast<uint8_t>(PacketId::Teamplay));
                    bw.write_u8(0);                 // Config
                    bw.write_u8(1 | 2 | 4);         // team ESP, pings, compass
                    bw.write_f32(0.0f);             // north: toward y = 0
                    bw.write_f32(-1.0f);
                    send(p, bw);
                }
                p.ext_teamplay = tp;
                // The time of day first: a client that claimed it does not draw
                // the world until it knows whether it is night.
                if (m.offers(proto::Extension::DaytimeWeather)) {
                    ByteWriter sky;
                    proto::write_daytime_sky(sky, cfg.night ? proto::kMidnight : proto::kNoon);
                    send(p, sky);
                }
                const bool fl = m.offers(proto::Extension::Flashlight);
                if (fl && !p.ext_flashlight) {
                    // Whose lamps are lit, after the state the client is loading.
                    ByteWriter state;
                    proto::write_flashlight_state(state, lit_lamps());
                    if (p.loading) p.saved.push_back(state.data);
                    else send(p, state);
                }
                p.ext_flashlight = fl;
                const bool po = m.offers(proto::Extension::PlayerOutfit);
                if (po && !p.ext_outfit) {
                    // Team 1 wears the walking dead: outfit 64, which its
                    // holders draw from Models/Player/Custom as the grunts.
                    // An outfit shows at its player's next spawn, and every
                    // wave the horde raises is one.
                    ByteWriter outfits_bytes;
                    proto::write_team_outfit(outfits_bytes, 1, proto::kOutfitZombie);
                    if (p.loading) p.saved.push_back(outfits_bytes.data);
                    else send(p, outfits_bytes);
                }
                p.ext_outfit = po;
                // The way in's smoke, whatever phase is running: its columns
                // are sent on their own cycle the moment a smoke-claiming
                // client finishes loading, so nothing is owed on the claim.
                p.ext_smoke = m.offers(proto::Extension::Smoke);
            } break;
            case PacketId::Flashlight: on_flashlight(p, br); break;
            case PacketId::Teamplay: on_teamplay(p, br); break;
            case PacketId::ChatMessage: on_chat(p, br); break;
            case PacketId::ChangeWeapon: {
                if (!p.named || !br.can_read(2)) break;
                br.read_u8();
                const int w = br.read_u8();
                if (w > 2) break;
                set_weapon(p, w);
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::ChangeWeapon));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(static_cast<uint8_t>(w));
                broadcast(bw, true);
                kill(p, nullptr, kClassChangeKill);
            } break;
            case PacketId::ChangeTeam: {
                if (!p.named || !br.can_read(2)) break;
                br.read_u8();
                int t = static_cast<int8_t>(br.read_u8());
                t = (t == 0 || t == 1) ? kPeople : -1;
                if (t == p.team) break;
                const bool was_spectator = p.spectator();
                p.team = t;
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::ChangeTeam));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(static_cast<uint8_t>(static_cast<int8_t>(t)));
                broadcast(bw, true);
                if (was_spectator || !p.alive) schedule_respawn(p);
                else kill(p, nullptr, kTeamChangeKill);
            } break;
            default: break;
        }
        if (!p.alive) return;

        switch (static_cast<PacketId>(data[0])) {
            case PacketId::PositionData: {
                if (!br.can_read(12)) break;
                const Vec3 v = br.read_vec();
                if (!finite3(v)) break;
                // Taken as sent, at any rate: there is no internet between the
                // client and here.
                p.body.pos = v;
                p.body.eye = v;
            } break;
            case PacketId::OrientationData: {
                if (!br.can_read(12)) break;
                Vec3 o = br.read_vec();
                const float l = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z);
                if (!finite3(o) || l < 1e-4f) break;
                p.body.ori = Vec3{o.x / l, o.y / l, o.z / l};
            } break;
            case PacketId::InputData: {
                if (!br.can_read(2)) break;
                br.read_u8();
                uint8_t f = br.read_u8();
                p.input.mf = f & 1;
                p.input.mb = (f >> 1) & 1;
                p.input.ml = (f >> 2) & 1;
                p.input.mr = (f >> 3) & 1;
                bool jump = (f >> 4) & 1;
                if (jump && !(p.body.vel.z >= 0 && p.body.vel.z < 0.017f)) jump = false;
                p.input.jump = jump;
                const bool crouch = (f >> 5) & 1;
                if (crouch != p.body.crouching) physics::set_crouch(phys_map, p.body, crouch);
                p.input.crouch = crouch;
                p.input.sneak = (f >> 6) & 1;
                p.input.sprint = (f >> 7) & 1;
                if (!jump) f &= static_cast<uint8_t>(~0x10u);
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::InputData));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(f);
                broadcast(bw, false, p.id, -2, false);
            } break;
            case PacketId::WeaponInput: {
                if (!br.can_read(2)) break;
                br.read_u8();
                const uint8_t f = br.read_u8();
                const bool primary = f & 1, secondary = (f >> 1) & 1;
                if (primary != p.primary && p.tool == kToolWeapon) p.gun.set_shoot(primary, now);
                if (secondary != p.secondary && secondary && p.tool == kToolBlock) {
                    p.line_start = p.body.pos;
                    p.line_started = true;
                }
                p.primary = primary;
                p.secondary = secondary;
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::WeaponInput));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(f & 3);
                broadcast(bw, false, p.id);
            } break;
            case PacketId::WeaponReload: {
                p.gun.reload(now);
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::WeaponReload));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(0);
                bw.write_u8(0);
                broadcast(bw, false, p.id);
            } break;
            case PacketId::HitPacket: on_hit(p, br); break;
            case PacketId::GrenadePacket: on_grenade(p, br); break;
            case PacketId::SetTool: {
                if (!br.can_read(2) || !p.named) break;
                br.read_u8();
                const int tool = br.read_u8();
                if (tool > 3) break;
                if (p.tool == kToolWeapon) p.gun.set_shoot(false, now);
                p.tool = tool;
                if (tool == kToolWeapon) p.gun.set_shoot(p.primary, now);
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::SetTool));
                bw.write_u8(static_cast<uint8_t>(p.id));
                bw.write_u8(static_cast<uint8_t>(tool));
                broadcast(bw, true, p.id);
            } break;
            case PacketId::SetColor: {
                if (!br.can_read(4) || !p.named) break;
                br.read_u8();
                const uint32_t b = br.read_u8(), g = br.read_u8(), r = br.read_u8();
                p.color = b | (g << 8) | (r << 16);
                ByteWriter bw;
                bw.write_u8(static_cast<uint8_t>(PacketId::SetColor));
                bw.write_u8(static_cast<uint8_t>(p.id));
                write_color(bw, p.color);
                broadcast(bw, true, p.id);
            } break;
            case PacketId::BlockAction: on_block_action(p, br); break;
            case PacketId::BlockLine: on_block_line(p, br); break;
            default: break;
        }
    }

    void on_join(Player& p, ByteReader& br) {
        if (p.team == 0 || p.team == 1) return;   // only from limbo or spectating
        if (!br.can_read(3)) return;
        br.read_u8();
        const int team = static_cast<int8_t>(br.read_u8());
        const int weapon = br.read_u8();
        std::string name;
        if (br.can_read(1)) {
            br.read_u8();                         // tool
            if (br.can_read(7)) {
                br.read_u32();                    // kills
                br.read_u8(); br.read_u8(); br.read_u8();
                name = br.read_cstring();
            }
        }
        p.team = (team == 0 || team == 1) ? kPeople : -1;
        if (!p.named) {
            name = sanitise_name(name);
            if (name.empty()) name = "Deuce";
            std::string candidate = name;
            for (int i = 1;; ++i) {
                bool taken = false;
                for (auto& o : players) {
                    if (!o || !o->named || o.get() == &p) continue;
                    std::string a = o->name, b = candidate;
                    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                    if (a == b) { taken = true; break; }
                }
                if (!taken) break;
                candidate = name + std::to_string(i);
            }
            p.name = candidate;
            p.named = true;
            log("%s joined", p.name.c_str());
        }
        set_weapon(p, weapon > 2 ? 0 : weapon);
        // The dead are announced to a newcomer as fallen, or it draws them
        // standing about.
        for (auto& o : players) {
            if (!o || o.get() == &p || !o->named || o->alive || o->spectator()) continue;
            ByteWriter bw;
            bw.write_u8(static_cast<uint8_t>(PacketId::KillAction));
            bw.write_u8(static_cast<uint8_t>(o->id));
            bw.write_u8(static_cast<uint8_t>(o->id));
            bw.write_u8(kFallKill);
            bw.write_u8(0);
            send(p, bw);
        }
        spawn(p);
        // The targets' outlines, for one who joins after they were placed.
        if (cfg.highlight_targets)
            for (auto& o : players)
                if (o && o->aim_bot && o->alive) send_mark(*o);
        p.pinged_step = -1;   // the tutorial's next goal, pinged again
    }

    void on_hit(Player& p, ByteReader& br) {
        if (!br.can_read(2)) return;
        const int target = br.read_u8();
        const int value = br.read_u8();
        if (target >= kSlots || !players[target] || target == p.id) return;
        Player& victim = *players[target];
        if (!victim.alive || value > kMelee) return;
        const bool melee = value == kMelee;
        int amount;
        if (melee) {
            if (p.tool != kToolSpade && p.tool != kToolWeapon) return;
            if (!near3(p.body.pos, victim.body.pos, kMeleeDistance)) return;
            amount = kMeleeDamage;
        } else {
            if (p.tool != kToolWeapon || p.gun.is_empty(world_time)) return;
            amount = p.gun.spec().damage[value];
        }
        const int kill_type = melee ? kMeleeKill : value == kHead ? kHeadshotKill : kWeaponKill;
        if (victim.aim_bot) {
            // Only the run's own hits count, and only the run's targets take them.
            if (aim.phase != training::AimPhase::Running) return;
            if (!melee) {
                // A shot's pellets arrive together: one hit per shot, and one
                // headshot if any of them was.
                if (world_time - p.last_hit_at > 0.03) {
                    aim.hits += 1;
                    p.head_this_shot = false;
                }
                p.last_hit_at = world_time;
                if (value == kHead && !p.head_this_shot) {
                    aim.headshots += 1;
                    p.head_this_shot = true;
                }
            }
            if (cfg.scenario == training::Scenario::Tracking) return;  // it never falls
            if (training::one_shot(cfg.scenario)) {
                kill(victim, &p, kill_type);
                return;
            }
        }
        hit(victim, amount, &p, kill_type);
    }

    void on_grenade(Player& p, ByteReader& br) {
        if (!br.can_read(29)) return;
        br.read_u8();
        float fuse = br.read_f32();
        Vec3 pos = br.read_vec();
        Vec3 vel = br.read_vec();
        if (!std::isfinite(fuse) || !finite3(pos) || !finite3(vel)) return;
        if (p.grenades <= 0) return;
        p.grenades -= 1;
        if (!near3(pos, p.body.pos, 10.0f)) pos = p.body.pos;
        fuse = std::min(fuse, 3.0f);
        Vec3 rel{vel.x - p.body.vel.x, vel.y - p.body.vel.y, vel.z - p.body.vel.z};
        const float l = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        if (l > 2.0f) {
            rel = Vec3{rel.x / l * 2.0f, rel.y / l * 2.0f, rel.z / l * 2.0f};
            vel = Vec3{rel.x + p.body.vel.x, rel.y + p.body.vel.y, rel.z + p.body.vel.z};
        }
        // Where it goes off is known now: the same flight every client
        // simulates. The map may change under it in three seconds; pyspades'
        // moving object would see that and this does not, which costs a
        // grenade landing on a block somebody dug out mid-flight.
        const nade::Impact at = nade::simulate(&phys_map, pos, vel, std::max(fuse, 0.0f));
        grenades.push_back(Grenade{p.id, p.team, at.pos, world_time + std::max(fuse, 0.0f)});
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::GrenadePacket));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_f32(fuse);
        bw.write_vec(pos);
        bw.write_vec(vel);
        broadcast(bw, false, p.id);
    }

    void on_block_action(Player& p, ByteReader& br) {
        if (!br.can_read(14)) return;
        br.read_u8();
        const int value = br.read_u8();
        const proto::IVec3 c = br.read_ivec();
        const int x = c.x, y = c.y, z = c.z;
        if (value == kGrenadeDestroy || value > kGrenadeDestroy) return;
        if (z >= 62 || !VoxelMap::valid(x, y, z)) return;
        const Vec3& pos = p.body.pos;
        auto within = [&](float d) {
            return std::fabs(pos.x - x) < d && std::fabs(pos.y - y) < d && std::fabs(pos.z - z) < d;
        };
        if (value == kBuildBlock) {
            p.blocks -= 1;
            if (p.blocks < -kBuildTolerance || !within(kMaxBlockDistance)) return;
            if (horde && !horde->can_build(x, y, z)) {
                p.blocks += 1;   // a refusal spends no block
                system_chat("That block would seal the last way in.", p.id);
                return;
            }
            if (!map.build_point(x, y, z, p.color | 0x7F000000u)) return;
            touched({Cell{x, y, z}}, true);
        } else {
            if (p.tool == kToolWeapon && p.gun.is_empty(world_time)) return;
            if (!map.solid(x, y, z)) return;
            if (core_column_cell(x, y, z)) {
                system_chat("The core cannot be torn down.", p.id);
                return;
            }
            if (p.tool == kToolSpade && !within(kMaxDigDistance)) return;
            std::vector<Cell> removed;
            if (value == kDestroyBlock) {
                if (map.destroy_point(x, y, z, &removed)) p.blocks = std::min(50, p.blocks + 1);
            } else {
                map.destroy_point(x, y, z, &removed);
                map.destroy_point(x, y, z + 1, &removed);
                map.destroy_point(x, y, z - 1, &removed);
            }
            touched(removed, false);
        }
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::BlockAction));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_u8(static_cast<uint8_t>(value));
        bw.write_i32(x);
        bw.write_i32(y);
        bw.write_i32(z);
        broadcast(bw, true);
    }

    void on_block_line(Player& p, ByteReader& br) {
        if (!br.can_read(25) || !p.line_started) return;
        br.read_u8();
        const proto::IVec3 a = br.read_ivec();
        const proto::IVec3 b = br.read_ivec();
        if (!VoxelMap::valid(a.x, a.y, a.z) || !VoxelMap::valid(b.x, b.y, b.z)) return;
        auto within = [](const Vec3& v, const proto::IVec3& c) {
            return std::fabs(v.x - c.x) < kMaxBlockDistance && std::fabs(v.y - c.y) < kMaxBlockDistance &&
                   std::fabs(v.z - c.z) < kMaxBlockDistance;
        };
        if (!within(p.body.pos, b) || !within(p.line_start, a)) return;
        if (!map.has_neighbors(a.x, a.y, a.z) || !map.has_neighbors(b.x, b.y, b.z)) return;
        const std::vector<Cell> pts = cube_line(a.x, a.y, a.z, b.x, b.y, b.z);
        if (pts.empty() || static_cast<int>(pts.size()) > p.blocks + kBuildTolerance) return;
        std::vector<Cell> built;
        for (const Cell& c : pts) {
            if (map.solid(c.x, c.y, c.z)) continue;
            if (horde && !horde->can_build(c.x, c.y, c.z)) continue;   // the veto skips a cell
            if (!map.build_point(c.x, c.y, c.z, p.color | 0x7F000000u)) break;
            built.push_back(c);
        }
        touched(built, true);
        p.blocks -= static_cast<int>(pts.size());
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::BlockLine));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_ivec(a.x, a.y, a.z);
        bw.write_ivec(b.x, b.y, b.z);
        broadcast(bw, true);
    }

    void on_chat(Player& p, ByteReader& br) {
        if (!p.named || !br.can_read(2)) return;
        br.read_u8();
        const int type = br.read_u8();
        std::string text;
        while (br.can_read(1)) {
            const char ch = static_cast<char>(br.read_u8());
            if (ch == 0) break;
            text.push_back(ch == '\r' || ch == '\n' ? ' ' : ch);
        }
        if (text.size() > 108) text.resize(108);
        if (!text.empty() && text[0] == '/') {
            if (text == "/kill" && p.alive) kill(p, nullptr, kFallKill);
            else if (text == "/restart" && aim_mode()) {
                reset_run();
                if (p.alive) spawn(p);
            } else if (text == "/clearwave" && horde_mode()) clear_wave();
            else if (horde_mode()) system_chat("Commands here: /clearwave, /kill", p.id);
            else system_chat(aim_mode() ? "Commands here: /restart, /kill"
                                        : "Commands here: /kill", p.id);
            return;
        }
        const bool all = type == static_cast<int>(proto::ChatType::All);
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::ChatMessage));
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_u8(all ? 0 : 1);
        bw.write_cstring(text);
        broadcast(bw, false, -1, all ? -2 : p.team);
    }

    // --- lamps (Flashlight, extension 0x32) --------------------------------
    std::vector<bool> lit_lamps() const {
        std::vector<bool> lit(kSlots, false);
        for (const auto& pp : players)
            if (pp && pp->lamp) lit[static_cast<size_t>(pp->id)] = true;
        return lit;
    }
    // To everybody who claimed the extension, the switcher too: that echo is
    // what lights it. Saved for a client still loading, which applies it once
    // its world exists.
    void send_lamp(const Player& owner) {
        ByteWriter bw;
        proto::write_flashlight_light(bw, static_cast<uint8_t>(owner.id), owner.lamp);
        for (auto& pp : players) {
            if (!pp || !pp->ext_flashlight) continue;
            if (pp->loading) pp->saved.push_back(bw.data);
            else send(*pp, bw);
        }
    }
    void on_flashlight(Player& p, ByteReader& br) {
        proto::FlashlightMsg m;
        if (!p.ext_flashlight || !proto::read_flashlight(br, m)) return;
        if (m.sub != proto::FlashlightSub::Light) return;   // the rest are ours to send
        // The dead and the spectators carry no lamp; Kill Action puts one out.
        if (!p.alive || p.spectator()) return;
        if (p.lamp == m.on) return;
        p.lamp = m.on;
        send_lamp(p);
    }

    // Teamplay ping (sub 1), client to server: relayed to every teammate that
    // claimed the extension, the sender filled in by us.
    void on_teamplay(Player& p, ByteReader& br) {
        if (!br.can_read(1) || br.read_u8() != 1) return;
        if (!p.alive || !p.playing()) return;
        if (!br.can_read(21)) return;
        br.read_u8();                 // sender: ours to fill
        const Vec3 at = br.read_vec();
        br.read_f32();
        br.read_u8();
        br.read_u8(); br.read_u8(); br.read_u8();
        br.read_u8();
        std::string reason;
        while (br.can_read(1) && reason.size() < proto::kTeamplayReasonMax)
            reason.push_back(static_cast<char>(br.read_u8()));
        if (!finite3(at) || at.x < 0 || at.x > 512 || at.y < 0 || at.y > 512 || at.z < 0 || at.z > 64) return;
        if (world_time - p.last_ping < kPingInterval) return;
        p.last_ping = world_time;
        ByteWriter bw;
        bw.write_u8(static_cast<uint8_t>(PacketId::Teamplay));
        bw.write_u8(1);
        bw.write_u8(static_cast<uint8_t>(p.id));
        bw.write_vec(at);
        bw.write_f32(kPingDuration);
        bw.write_u8(0);
        write_color(bw, p.team == 0 ? 0x3070FF : 0x30E040);
        bw.write_u8(0);
        for (char ch : reason) bw.write_u8(static_cast<uint8_t>(ch));
        for (auto& o : players) {
            if (!o || o->loading || !o->ext_teamplay || o->team != p.team) continue;
            send(*o, bw);
        }
    }

    // --- the loop ---------------------------------------------------------
    void pump(int wait_ms) {
        ENetEvent ev;
        auto take = [&]() {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: on_connect(ev.peer, ev.data); break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    if (ev.peer->data) on_disconnect(*static_cast<Player*>(ev.peer->data));
                    ev.peer->data = nullptr;
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    if (ev.peer->data)
                        on_packet(*static_cast<Player*>(ev.peer->data), ev.packet->data, ev.packet->dataLength);
                    enet_packet_destroy(ev.packet);
                    break;
                default: break;
            }
        };
        // Read until the socket is empty: ENet reads at most 256 datagrams a
        // service, and a full read means there may be more.
        uint32_t read_before = host->totalReceivedPackets;
        int r = enet_host_service(host, &ev, static_cast<enet_uint32>(std::max(wait_ms, 0)));
        for (int round = 1;; ++round) {
            while (r > 0) {
                take();
                r = enet_host_check_events(host, &ev);
            }
            if (r < 0 || host->totalReceivedPackets - read_before < 256 || round >= 16) break;
            read_before = host->totalReceivedPackets;
            r = enet_host_service(host, &ev, 0);
        }
        serve_maps();
        chunk_budget = 24;
        for (auto& pp : players)
            if (pp && pp->loading) continue_map(*pp);
        const double t = now_sec();
        int ticks = 0;
        while (t - world_time > kTick && ticks < 30) {
            world_time += kTick;
            world_tick();
            ++ticks;
        }
        if (t - world_time > 1.0) world_time = t;   // a stall: do not try to catch up
        const double period = 1.0 / std::max(1, cfg.update_rate);
        if (t >= next_network) {
            // Behind by more than a period (a stall): start again from now.
            next_network = t - next_network > period ? t + period : next_network + period;
            world_update();
        }
        enet_host_flush(host);
    }
};

// ---------------------------------------------------------------------------

Server::Server(const HostConfig& cfg) : impl_(std::make_unique<Impl>()) { impl_->cfg = cfg; }

Server::~Server() {
    if (impl_->host) {
        for (auto& p : impl_->players)
            if (p && p->peer) enet_peer_disconnect_now(p->peer, 5);
        enet_host_destroy(impl_->host);
    }
    if (impl_->map_job.joinable()) impl_->map_job.join();
    if (impl_->enet_up) enet_deinitialize();
}

bool Server::start(std::string& err) {
    Impl& s = *impl_;
    // WSAStartup on Windows, nothing elsewhere. It counts, so the server's own
    // call and the game's do not undo each other.
    if (enet_initialize() != 0) { err = "cannot start networking"; return false; }
    s.enet_up = true;
    std::string map_err;
    if (!s.load_map(map_err)) { err = map_err; return false; }
    if (s.aim_mode()) {
        s.aim.scenario = s.cfg.scenario;
        s.add_aim_bots();
    } else if (s.horde_mode()) {
        // The corners of the map are the spawns, the middle the core; the map
        // is flat open land everywhere outside the course's corridor. On a
        // .vxl of coastline the cells the settings ask for can be at sea, so
        // each is walked in to the nearest land.
        s.horde_cfg.core_x = s.cfg.core_x;
        s.horde_cfg.core_y = s.cfg.core_y;
        s.nearest_land(s.horde_cfg.core_x, s.horde_cfg.core_y);
        for (int k = 0; k < horde::Config::kSpawnSites; ++k) {
            int x = s.horde_cfg.spawn_x[k], y = s.horde_cfg.spawn_y[k];
            s.nearest_land(x, y);
            s.horde_cfg.spawn_x[k] = x;
            s.horde_cfg.spawn_y[k] = y;
        }
        s.build_core_column();
        s.map.to_solid_map(s.phys_map);
        s.horde = std::make_unique<horde::Horde>(s.phys_map, s.horde_cfg);
    } else {
        s.add_targets();
        s.bot_count = training::kLaneCount;
    }
    s.map_cache = std::make_shared<const std::vector<uint8_t>>(s.map.save_compressed());
    s.cache_rev = s.map_rev;
    ENetAddress addr;
    enet_address_set_host(&addr, "127.0.0.1");
    addr.port = s.cfg.port;
    s.host = enet_host_create(&addr, s.cfg.max_players + 2, 1, 0, 0);
    if (!s.host) {
        err = "cannot listen on 127.0.0.1:" + std::to_string(s.cfg.port) + " (is something already using it?)";
        return false;
    }
    // The range coder on the way in only. Clients compress what they send and
    // flag it, so it has to be decoded; what the server sends goes plain,
    // which every ENet peer reads, and over loopback the coder saves nothing.
    {
        ENetCompressor inbound{};
        inbound.context = enet_range_coder_create();
        inbound.decompress = enet_range_coder_decompress;
        inbound.destroy = enet_range_coder_destroy;
        if (inbound.context) enet_host_compress(s.host, &inbound);
        else enet_host_compress_with_range_coder(s.host);
    }
    s.world_time = now_sec();
    s.next_network = s.world_time;
    if (s.horde) {
        s.horde->restart(s.world_time);
        s.system_chat("Build while it is quiet: the first wave comes in twenty seconds.");
    }
    s.log("training course on 127.0.0.1:%u, %d world updates a second",
          static_cast<unsigned>(s.cfg.port), s.cfg.update_rate);
    return true;
}

void Server::pump(int wait_ms) { impl_->pump(wait_ms); }

void Server::run(const std::atomic<bool>& stop) {
    // A fixed cycle: take everything that has arrived, run the world, send
    // once. On Windows a sleep lasts a multiple of the system timer, 15.6ms
    // unless somebody asks for finer, which would halve the world updates:
    // this asks for 1ms for as long as the server runs.
#ifdef _WIN32
    timeBeginPeriod(1);
    struct EndPeriod { ~EndPeriod() { timeEndPeriod(1); } } end_period;
#endif
    const double cycle = 1.0 / std::max(120, impl_->cfg.update_rate);
    auto next = std::chrono::steady_clock::now();
    const auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(cycle));
    while (!stop.load(std::memory_order_relaxed)) {
        impl_->pump(0);
        next += step;
        const auto now = std::chrono::steady_clock::now();
        if (next < now) next = now;   // behind: do not try to catch up
        else std::this_thread::sleep_until(next);
    }
}

int Server::player_count() const {
    int n = 0;
    for (auto& p : impl_->players)
        if (p && p->named) ++n;
    return n;
}
int Server::player_hp(int id) const {
    if (id < 0 || id >= kSlots || !impl_->players[id] || !impl_->players[id]->alive) return -1;
    return impl_->players[id]->hp;
}
bool Server::player_alive(int id) const { return player_hp(id) > 0; }
int Server::player_team(int id) const {
    if (id < 0 || id >= kSlots || !impl_->players[id]) return -2;
    return impl_->players[id]->team;
}
bool Server::player_position(int id, float& x, float& y, float& z) const {
    if (id < 0 || id >= kSlots || !impl_->players[id] || !impl_->players[id]->alive) return false;
    const Vec3& p = impl_->players[id]->body.pos;
    x = p.x, y = p.y, z = p.z;
    return true;
}
bool Server::map_solid(int x, int y, int z) const {
    return VoxelMap::valid(x, y, z) && impl_->map.solid(x, y, z);
}

}  // namespace host

