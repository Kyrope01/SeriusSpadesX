#pragma once
// The training course's server: a small Ace of Spades 0.75 server, run on a
// thread of the game, that hosts the course (TrainingCourse.h) on 127.0.0.1.
// The game's own client connects to it like to any other server, so the
// tutorial is the real game: the same packets, guns, damage, block rules and
// floating-block check as pyspades, which every client repeats on its own and
// the server has to agree with.
//
// It plays nothing but the course. The range's targets are its own players,
// with no connection behind them: they walk a lane and fall over.
//
// Where pyspades makes a choice because its players are on the internet, this
// one makes the other choice, because they are not: world updates go out at
// `update_rate` a second (pyspades sends 10), a position a client reports is
// taken as it is, and the map goes out as fast as the link takes it.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TrainingModes.h"

namespace host {

struct HostConfig {
    uint16_t port = 32887;
    // World updates a second.
    int update_rate = 60;
    // Midnight, through the Daytime and Weather extension: a client that
    // claims it draws a night, and only lamps and flashes light it.
    bool night = false;
    float respawn_sec = 5.0f;
    int max_players = 32;
    // Where the server's log lines go; nowhere if empty.
    std::function<void(const std::string&)> log;
    // The tutorial, or an aim scenario in the arena.
    training::Mode mode = training::Mode::Tutorial;
    training::Scenario scenario = training::Scenario::Gridshot;
    // Aim: outline the targets (a Teamplay mark), through walls too.
    bool highlight_targets = false;
    // Horde: a .vxl map to play instead of the generated course, and the cell
    // the core stands on (moved to the nearest land when that cell is at sea).
    std::string map_file;
    // The same map as bytes, when the game carries it - the mode's own
    // course read out of the paks - and its settings .txt beside it. Set,
    // they are played whatever map_file says; a map_file of a world that
    // has neither still falls back to the generated course.
    std::string map_data, map_settings;
    // The resource the map_data bytes were read from ("" for the shipped
    // course or the generated one), so the log can name it.
    std::string map_name;
    // The .vxl resources the horde could play, for /va to advance through.
    std::vector<std::string> map_roster;
    int core_x = 256, core_y = 256;
    // Aim: the run's status for the HUD, and its restart requests. Shared with
    // the game, which runs this server on a thread of its own.
    std::shared_ptr<training::AimShared> aim;
    // Horde: /va arms the next map of the roster here; the menu thread reads
    // and clears it when the mode starts again.
    std::shared_ptr<training::HordeShared> horde;
};

class Server {
public:
    explicit Server(const HostConfig& cfg);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Build the course and bind the port. False with a reason.
    bool start(std::string& err);
    // Run until `stop` goes true. Call start() first.
    void run(const std::atomic<bool>& stop);
    // One pass of the loop, waiting at most `wait_ms` for packets. What run()
    // calls; a test can drive it by hand.
    void pump(int wait_ms);

    // For tests: what the server believes.
    int player_count() const;
    int player_hp(int id) const;          // -1 if no such player or dead
    bool player_alive(int id) const;
    int player_team(int id) const;        // -2 if no such player
    // Where a living player is; false if there is no such living player.
    bool player_position(int id, float& x, float& y, float& z) const;
    // Whether the map's block here is solid, as the server has it.
    bool map_solid(int x, int y, int z) const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace host
