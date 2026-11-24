/*
 Copyright (c) 2026 Detank2002, ZeroSpades developers.

 This file is part of ZeroSpades, a fork of OpenSpades.

 ZeroSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 ZeroSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with ZeroSpades.  If not, see <http://www.gnu.org/licenses/>.

 */

#include "TrainingHost.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>

#include <Core/Debug.h>
#include <Core/FileManager.h>
#include <Core/Settings.h>
#include <Training/Server.h>

DEFINE_SPADES_SETTING(cg_localPort, "32890");
// Horde: the .vxl it plays - or none, which plays the mode's own shipped
// Termina Fields, the generated training course standing in for an install
// whose paks lost it - and the cell the core stands on (moved to the
// nearest land when that is at sea; the map's own settings .txt names
// another cell, and its word outranks these).
DEFINE_SPADES_SETTING(cg_towerDefenseMap, "");
DEFINE_SPADES_SETTING(cg_towerDefenseCoreX, "256");
DEFINE_SPADES_SETTING(cg_towerDefenseCoreY, "256");

namespace spades {
	namespace gui {

		struct TrainingHost::Impl {
			std::mutex mutex;
			State state = State::Stopped;
			std::string error;
			std::atomic<bool> stop{false};
			std::thread thread;
			std::shared_ptr<training::AimShared> aim = std::make_shared<training::AimShared>();
			std::shared_ptr<training::TowerDefenseShared> tower_defense =
			    std::make_shared<training::TowerDefenseShared>();

			void Join() {
				if (thread.joinable()) {
					stop = true;
					thread.join();
				}
			}
		};

		TrainingHost& TrainingHost::Get() {
			static TrainingHost instance;
			return instance;
		}

		TrainingHost::TrainingHost() : impl(new Impl()) {}

		TrainingHost::~TrainingHost() { impl->Join(); }

		TrainingHost::State TrainingHost::GetState() {
			std::lock_guard<std::mutex> lock(impl->mutex);
			return impl->state;
		}

		std::string TrainingHost::GetError() {
			std::lock_guard<std::mutex> lock(impl->mutex);
			return impl->error;
		}

		bool TrainingHost::IsActive() {
			std::lock_guard<std::mutex> lock(impl->mutex);
			return impl->state == State::Ready;
		}

		void TrainingHost::Stop() {
			impl->Join();
			std::lock_guard<std::mutex> lock(impl->mutex);
			impl->state = State::Stopped;
		}

		training::AimStatus TrainingHost::GetAimStatus() {
			std::lock_guard<std::mutex> lock(impl->aim->mutex);
			return impl->aim->status;
		}

		void TrainingHost::RestartAim() {
			std::lock_guard<std::mutex> lock(impl->aim->mutex);
			impl->aim->restart = true;
		}

		/** The name in lower case, for suffix and course checks. */
		static std::string lowered_name(std::string s) {
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return s;
		}

		std::vector<std::string> TrainingHost::MapRoster() {
			std::vector<std::string> maps;
			std::vector<std::string> files;
			try {
				files = spades::FileManager::EnumFiles("Maps");
			} catch (const std::exception&) {
				return maps; // no Maps folder: the course is the whole menu
			}
			for (const std::string& file : files) {
				const std::string whole = lowered_name(file);
				if (whole.size() < 5 ||
				    whole.compare(whole.size() - 4, 4, ".vxl") != 0)
					continue;
				if (whole == "termina_fieldstd.vxl" || whole == "title.vxl")
					continue; // the course is the menu head; Title, its backdrop
				maps.push_back("Maps/" + file);
			}
			std::sort(maps.begin(), maps.end());
			return maps;
		}

		std::string TrainingHost::GetTowerDefenseMap() { return
		    (std::string)cg_towerDefenseMap; }

		void TrainingHost::SetTowerDefenseMap(const std::string& map)
		{ cg_towerDefenseMap = map; }

		std::string TrainingHost::Start(const Launch& what) {
			Stop();
			launch = what;
			// /va asked for the map after this one while the last run stood:
			// answered here, where the next run takes its map from.
			if (what.mode == training::Mode::TowerDefense) {
				bool advance = false;
				{
					std::lock_guard<std::mutex> lock(impl->tower_defense->mutex);
					advance = impl->tower_defense->map_advance;
					impl->tower_defense->map_advance = false;
				}
				if (advance) {
					const std::vector<std::string> roster = MapRoster();
					const int n = static_cast<int>(roster.size());
					if (n > 0) {
						const std::string current = (std::string)cg_towerDefenseMap;
						int at = -1;
						for (int i = 0; i < n; i++)
							if (roster[i] == current)
								at = i;
						const std::string next = roster[(at + 1) % n];
						SPLog("[Training] map advance: the next map is %s", next.c_str());
						SetTowerDefenseMap(next);
					}
				}
			}
			{
				// A new run: nothing of the last one shows.
				std::lock_guard<std::mutex> lock(impl->aim->mutex);
				impl->aim->status = training::AimStatus{};
				impl->aim->status.scenario = what.scenario;
				impl->aim->status.weapon = what.weapon;
				impl->aim->restart = false;
			}
			{
				std::lock_guard<std::mutex> lock(impl->mutex);
				impl->state = State::Starting;
				impl->error.clear();
			}
			port = (int)cg_localPort;
			SPLog("Starting the training course on 127.0.0.1:%d", port);

			// Before the thread, not in it: a Stop() in between must not be lost.
			impl->stop = false;
			Impl* self = impl.get();
			const int listenPort = port;
			const Launch run = what;
			const std::string towerDefenseMap = (std::string)cg_towerDefenseMap;
			const std::vector<std::string> towerDefenseRoster = MapRoster();
			const int towerDefenseCoreX = (int)cg_towerDefenseCoreX,
			    towerDefenseCoreY = (int)cg_towerDefenseCoreY;
			impl->thread = std::thread([self, listenPort, run, towerDefenseMap,
			                            towerDefenseRoster, towerDefenseCoreX,
			                            towerDefenseCoreY]() {
				host::HostConfig cfg;
				cfg.port = static_cast<uint16_t>(listenPort);
				cfg.mode = run.mode;
				cfg.scenario = run.scenario;
				cfg.highlight_targets = run.highlight;
				cfg.map_file = towerDefenseMap;
				cfg.map_roster = towerDefenseRoster;
				cfg.tower_defense = self->tower_defense;
				cfg.core_x = towerDefenseCoreX;
				cfg.core_y = towerDefenseCoreY;
				// A map the Maps folder carries is played as a resource, read
				// the same way every other asset is read - its settings .txt
				// beside it too, and the server logs it by name. The setting
				// only names a resource; a path the game cannot see falls back
				// to map_file above, and a world with neither of them still
				// stands the old road: the course, shipped in the paks, and
				// behind it the generated one.
				if (run.mode == training::Mode::TowerDefense &&
				    !towerDefenseMap.empty() &&
				    spades::FileManager::FileExists(towerDefenseMap.c_str())) {
					cfg.map_data =
					    spades::FileManager::ReadAllBytes(towerDefenseMap.c_str());
					cfg.map_name = towerDefenseMap;
					std::string side = towerDefenseMap;
					const size_t slash = side.rfind('/');
					const size_t dot = side.rfind('.');
					if (dot != std::string::npos &&
					    (slash == std::string::npos || dot > slash))
						side = side.substr(0, dot) + ".txt";
					if (spades::FileManager::FileExists(side.c_str()))
						cfg.map_settings =
						    spades::FileManager::ReadAllBytes(side.c_str());
					SPLog("[Training] the named map %s rides the game files",
					      towerDefenseMap.c_str());
				} else if (run.mode == training::Mode::TowerDefense &&
				    towerDefenseMap.empty() &&
				    spades::FileManager::FileExists("Maps/Termina_FieldsTD.vxl")) {
					cfg.map_data = spades::FileManager::ReadAllBytes("Maps/Termina_FieldsTD.vxl");
					if (spades::FileManager::FileExists("Maps/Termina_FieldsTD.txt"))
						cfg.map_settings =
						    spades::FileManager::ReadAllBytes("Maps/Termina_FieldsTD.txt");
					SPLog("[Training] the tower defense plays the shipped Termina Fields");
				} else if (run.mode == training::Mode::TowerDefense &&
				    towerDefenseMap.empty()) {
					// The paks carry the course with the game (Resources/mkpak
					// packs Maps into pak005); an install whose paks are stale
					// says so here instead of quietly standing the generated
					// course in for Termina Fields.
					SPLog("[Training] no shipped Termina Fields in the game's"
					      " files: rebuild the paks (Resources/mkpak) or the"
					      " generated course stands in");
				}
				cfg.aim = self->aim;
				cfg.log = [](const std::string& line) { SPLog("[Training] %s", line.c_str()); };
				try {
					host::Server server(cfg);
					std::string err;
					if (!server.start(err)) {
						std::lock_guard<std::mutex> lock(self->mutex);
						self->state = State::Failed;
						self->error = err;
						return;
					}
					{
						std::lock_guard<std::mutex> lock(self->mutex);
						if (self->state == State::Starting)
							self->state = State::Ready;
					}
					server.run(self->stop);
				} catch (const std::exception& ex) {
					std::lock_guard<std::mutex> lock(self->mutex);
					self->state = State::Failed;
					self->error = ex.what();
					return;
				}
				std::lock_guard<std::mutex> lock(self->mutex);
				if (self->state == State::Ready)
					self->state = State::Stopped;
			});
			return "";
		}
	} // namespace gui
} // namespace spades
