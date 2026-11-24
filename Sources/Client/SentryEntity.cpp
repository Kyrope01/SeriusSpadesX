/*
 Copyright (c) 2013 yvt

 This file is part of OpenSpades.

 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.
 */

// The sentry gun: the Seriuspades horde's own third word on the Smoke
// extension, made a thing the eye can see. The server owns the gun -
// its blocks, its aim, its damage, its death - and says it to the wire;
// this file only draws what the wire says.

#include "SentryEntity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Client.h"
#include "GameConstants.h"
#include "GameMap.h"
#include "IAudioChunk.h"
#include "IAudioDevice.h"
#include "IImage.h"
#include "IModel.h"
#include "IRenderer.h"
#include "ParticleSpriteEntity.h"
#include "Player.h"
#include "Tracer.h"
#include "World.h"
#include <Core/Debug.h>
#include <Core/Math.h>
#include <Core/Settings.h>
#include <Core/TMPUtils.h>

// ============================ SENTRY CONFIG ============================
// Live-tunable in the in-game console: change the value, test, then
// freeze the winning values here. Each item is DEFINE'd ONLY in this
// file: a duplicate DEFINE anywhere else in the project is a linker
// error.
//
// --- Primary stats (the LOCAL mode's toy gun scales from these; the
//     the stats of the live server sit in towerdefense.toml) ---
DEFINE_SPADES_SETTING(cg_sentryRange, "48");          // local scan range, blocks
DEFINE_SPADES_SETTING(cg_sentryFireInterval, "0.50"); // local fire interval base, s
DEFINE_SPADES_SETTING(cg_sentryAimPitch, "1");        // local mode: 0 = level gun
DEFINE_SPADES_SETTING(cg_sentryAimBody, "0.5");       // local aim below the eye, blocks
DEFINE_SPADES_SETTING(cg_sentryVolume, "1");          // shot volume multiplier
DEFINE_SPADES_SETTING(cg_sentryDebug, "0");           // 1 = log scan verdicts
DEFINE_SPADES_SETTING(cg_sentryServer, "1");          // 1 = draw the server's guns
//
// --- Placement / visuals ---
DEFINE_SPADES_SETTING(cg_sentryScale, "0.1");  // model render scale
DEFINE_SPADES_SETTING(cg_sentryHeadZ, "-0.1");  // manual head seat, world blocks
// the per-tier muzzle seats now stand frozen in the code
// itself: see SentryMuzzleDistance, SentryMuzzleSeat and
// MuzzlePoint further down. The numbered cg_sentry muzzle
// knobs of the November calibration rounds are RETIRED; the
// values that landed right on 22 Nov are now literals.
DEFINE_SPADES_SETTING(cg_sentryFlash, "1");       // muzzle flash dynamic light
DEFINE_SPADES_SETTING(cg_sentryYawFix, "0");   // constant orientation fixes,
DEFINE_SPADES_SETTING(cg_sentryPitchFix, "0"); // in the MODEL's local frame,
DEFINE_SPADES_SETTING(cg_sentryRollFix, "0");  // degrees, both models
//
// --- Level-3 minigun barrels ---
DEFINE_SPADES_SETTING(cg_sentryBarrelPivotX, "-0.5"); // kv6 pivot to true axis, x
DEFINE_SPADES_SETTING(cg_sentryBarrelPivotZ, "-0.5"); // ... z (y cancels: spin is Y)
// the level-3 body and barrel seats stand frozen as literals
// in Render3D; their numbered cg_sentry knobs are retired.
//
// --- casings & muzzle puff ---
DEFINE_SPADES_SETTING(cg_sentryCasingFwd, "-0.5"); // casing throw, levels 1-2
// the per-tier casing scales and the level-3 ejection seat
// stand frozen as literals in FireVisual; their cg_sentry
// knobs are retired.
DEFINE_SPADES_SETTING(cg_sentryFlashRadius, "14"); // muzzle light radius
DEFINE_SPADES_SETTING(cg_sentryPuffRadius, "0.3"); // levels 1-2 muzzle puff
DEFINE_SPADES_SETTING(cg_sentryPuffGrowth, "6");   // ... growth, blocks/s
DEFINE_SPADES_SETTING(cg_sentryPuffLife, "0.15");  // ... lifetime, s
// ========================================================================

SPADES_SETTING(cg_particles); // DEFINE'd in Client_LocalEnts.cpp

namespace spades {

	namespace client {

		// Per-level muzzle distance from the head pivot: each tier has
		// its own gun length, so the tracer, smoke, flash puff and
		// muzzle light all start at the right spot. The numbered
		// cg_sentry knobs are retired; the distances froze 22 Nov.
		static float SentryMuzzleDistance(int level) {
			switch (level) {
				case 1:  return 0.78f; // level-1 (frozen 14 Nov)
				case 2:  return 2.f;    // level-2 (frozen 22 Nov)
				default: return 3.f;   // level-3 (frozen 22 Nov)
			}
		}

		// The seat of the tier's muzzle, the triple every shot's
		// show anchors to: X leans to the barrel's right, Y
		// stretches the reach past the distance, Z raises the
		// muzzle in the world's own down-pointing z. The values
		// the 22 Nov calibration landed on now stand frozen
		// here; the cg_sentryN* knobs are retired.
		static Vector3 SentryMuzzleSeat(int level) {
			switch (level) {
				case 1:
					return MakeVector3(0.f, 0.3f, 0.f);    // frozen 22 Nov
				case 2:
					return MakeVector3(0.f, 0.1f, 0.f);    // frozen 22 Nov
				default:
					return MakeVector3(0.f, -1.1f, -0.05f); // frozen 22 Nov
			}
		}

		// ---- the server's quantized aim, read back ---------------------
		// The Smoke wire packs a gun's bearing as 128 steps round the
		// circle (0 = +Y, positive toward +X, radians
		// q * 2pi / 128 - pi) and its climb as 127 steps over the -80
		// of full climb to the +80 of full descent (map z is down, so a
		// positive pitch looks below the horizon). Keep in sync with
		// the server's turret_yaw_pack / turret_pitch_pack.
		static float DecodeAimYaw(std::uint8_t q) {
			return (float)q * ((float)kTwoPi / 128.f) - (float)kPi;
		}
		static float DecodeAimPitch(std::uint8_t q) {
			return (((float)q * (160.f / 127.f) - 80.f) * ((float)kPi / 180.f));
		}

		// ---- Level-3 minigun bullet smoke trail ------------------------
		// Races from the muzzle along the firing direction, dropping a
		// line of short-lived puffs, each carrying the distance fade of
		// its spawn point. Stationary growing billboards approximating
		// the first-person minigun skin's trail.
		class SentrySmokeTrail : public ILocalEntity {
			Client &client;
			Vector3 origin; // muzzle at the shot (frozen)
			Vector3 dir;    // firing direction (frozen)
			Handle<IImage> image; // Textures/WaterExpl.png (base pak)
			float distance = 0.f; // flown distance, world units
			float nextPuff = 0.f; // distance of the next puff to drop

			static constexpr float Speed = 220.f;
			static constexpr float MaxDistance = 2.f;
			static constexpr float PuffSpacing = 0.05f;
			static constexpr float PuffLife = 0.1f;
			static constexpr float PuffRadius = 0.1f;
			static constexpr float PuffGrowth = 0.5f;
			static constexpr float PuffSpin = 1.6f;

			public:
			SentrySmokeTrail(Client &cli, Vector3 muzzle, Vector3 fireDir)
			    : client(cli), origin(muzzle), dir(fireDir) {
				// RegisterImage returns null silently when the file is
				// missing: a missing sprite just disables the puffs
				try {
					image = client.GetRenderer().RegisterImage("Textures/WaterExpl.png");
				} catch (...) {
				}
			}

			bool Update(float dt) override {
				distance += Speed * dt;
				while (nextPuff <= distance && nextPuff <= MaxDistance) {
					if (image) {
						float fade = 1.f - nextPuff / MaxDistance;
						auto puff = stmp::make_unique<ParticleSpriteEntity>(
							client, image, MakeVector4(1.f, 1.f, 1.f, fade));
						// the TRAIL moves; the puffs stay where dropped
						puff->SetTrajectory(origin + dir * nextPuff,
						                    MakeVector3(0.f, 0.f, 0.f));
						puff->SetRotation(0.f, PuffSpin);
						puff->SetRadius(PuffRadius * fade, PuffGrowth * fade / PuffLife);
						puff->SetLifeTime(PuffLife, 0.02f, 0.03f);
						client.AddLocalEntity(std::move(puff));
					}
					nextPuff += PuffSpacing;
				}
				return distance < MaxDistance;
			}
			void Render3D() override {}
		};

		// ---- Ejected shell casing (sentry-local) ------------------------
		// The same flight, bounce and sound behavior as the vanilla
		// GunCasing local entity, plus a render size multiplier. Local
		// to this file so the sentry feature never touches the vanilla
		// GunCasing; scale 1 renders exactly the vanilla size.
		class SentryCasing : public ILocalEntity {
			Client &client;
			IRenderer &renderer;
			IModel *model;
			Matrix4 matrix;
			Vector3 velocity;
			int rotDir;
			float time;
			float scale; // render size multiplier (1 = vanilla)

			IAudioChunk *dropSound;
			IAudioChunk *waterSound;

			public:
			SentryCasing(Client &cli, IModel *model, IAudioChunk *dropSound,
			             IAudioChunk *waterSound, Vector3 pos, Vector3 dir,
			             Vector3 flyDir, float scale)
			    : client(cli), renderer(cli.GetRenderer()), model(model),
			      dropSound(dropSound), waterSound(waterSound), scale(scale) {
				if (dropSound)
					dropSound->AddRef();
				if (waterSound)
					waterSound->AddRef();

				Vector3 up = MakeVector3(0, 0, 1);
				Vector3 right = Vector3::Cross(dir, up).Normalize();
				up = Vector3::Cross(right, dir);

				matrix = Matrix4::FromAxis(right, dir, up, pos);
				velocity = flyDir * 10.0F;
				rotDir = SampleRandom() & 3; // random initial dir
				time = 1.0F;
			}
			~SentryCasing() {
				if (dropSound)
					dropSound->Release();
				if (waterSound)
					waterSound->Release();
			}
			bool Update(float dt) override {
				time -= 1.0F / 5.0F * dt;

				const auto &viewOrigin = client.GetLastSceneDef().viewOrigin;
				float distSqr = (viewOrigin - matrix.GetOrigin()).GetSquaredLength2D();
				if (distSqr > FOG_DISTANCE_SQ)
					return false;

				if (time <= 0.0F)
					return false;

				Matrix4 lastMat = matrix;

				matrix = matrix * Matrix4::Rotate(MakeVector3(1, 0, 0),
				                                  ((rotDir & 1) ? 1.0F : -1.0F) * dt * 5.0F);
				matrix = matrix * Matrix4::Rotate(MakeVector3(0, 1, 0),
				                                  ((rotDir & 2) ? 1.0F : -1.0F) * dt * 5.0F);
				matrix = Matrix4::Translate(velocity * dt) * matrix;
				velocity.z += dt * 32.0F;

				const Handle<GameMap> &map = client.GetWorld()->GetMap();
				SPAssert(map);

				// Collision
				IntVector3 lp = matrix.GetOrigin().Floor();

				if (lp.z >= 63) { // dropped into water
					if (waterSound) {
						if (!client.IsMuted() && distSqr < CASING_BOUNCE_DIST_SQ) {
							IAudioDevice &dev = client.GetAudioDevice();
							AudioParam param;
							param.referenceDistance = 0.6F;
							param.pitch = 0.9F + SampleRandomFloat() * 0.2F;
							dev.Play(waterSound, lastMat.GetOrigin(), param);
						}
						waterSound = NULL;
					}

					if (cg_particles && distSqr < CASING_BOUNCE_DIST_SQ) {
						Handle<IImage> img = renderer.RegisterWhiteImage();

						Vector4 col = {1, 1, 1, 0.6F};
						Vector3 pt = matrix.GetOrigin();
						pt.z = 62.99F;

						int splats = SampleRandomInt(1, 3);
						for (int i = 0; i < splats; i++) {
							auto ent = stmp::make_unique<ParticleSpriteEntity>(client, img, col);
							ent->SetTrajectory(
								pt,
								MakeVector3(SampleRandomFloat() - SampleRandomFloat(),
								            SampleRandomFloat() - SampleRandomFloat(),
								            -SampleRandomFloat()) *
								    2.0F,
								1.0F, 0.4F);
							ent->SetRotation(SampleRandomFloat() * (float)kTwoPi);
							ent->SetRadius(0.1F +
							               SampleRandomFloat() * SampleRandomFloat() * 0.1F);
							ent->SetLifeTime(2.0F, 0.0F, 1.0F);
							client.AddLocalEntity(std::move(ent));
						}
					}

					return false;
				}

				if (map->ClipWorld(lp.x, lp.y, lp.z)) { // hit a wall
					IntVector3 lp2 = lastMat.GetOrigin().Floor();
					if (lp.z != lp2.z &&
					    ((lp.x == lp2.x && lp.y == lp2.y) ||
					     !map->ClipWorld(lp.x, lp.y, lp2.z))) {
						velocity.z = -velocity.z;
						if (lp2.z < lp.z) { // ground hit
							if (dropSound) {
								if (!client.IsMuted() && distSqr < CASING_BOUNCE_DIST_SQ) {
									IAudioDevice &dev = client.GetAudioDevice();
									AudioParam param;
									param.referenceDistance = 0.6F;
									dev.Play(dropSound, lastMat.GetOrigin(), param);
								}
								dropSound = NULL;
							}
						}
					} else if (lp.x != lp2.x &&
					           ((lp.y == lp2.y && lp.z == lp2.z) ||
					            !map->ClipWorld(lp2.x, lp.y, lp.z))) {
						velocity.x = -velocity.x;
					} else if (lp.y != lp2.y &&
					           ((lp.x == lp2.x && lp.z == lp2.z) ||
					            !map->ClipWorld(lp.x, lp2.y, lp.z))) {
						velocity.y = -velocity.y;
					} else {
						return false;
					}

					matrix = lastMat;
					// friction eats some velocity
					velocity *= 0.46F;
					rotDir = (rotDir + 1) % 4;
					time -= 0.36F;
				}

				return true;
			}

			void Render3D() override {
				ModelRenderParam param;
				param.matrix = matrix * Matrix4::Scale(0.0125F * scale);
				renderer.RenderModel(*model, param);
			}
		};

		std::vector<SentryEntity *> SentryEntity::s_serverTurrets;

		// lift the client-side render hiding of a server turret's two
		// pillar blocks; defined past ServerTurretShot below. Never
		// called from a destructor on purpose: during world teardown
		// the map may already be gone; every death path lifts the
		// hiding where the death is decided, before the entity is
		// swept
		static void UnhidePillarBlocksFor(Client &cli, IntVector3 serverBlock);

		// ---- the wire's whole truth -----------------------------------
		// The Smoke wire says a Turrets packet that replaces everything:
		// what the list does not name is not standing. So apply it as
		// the three moves it is - spawn what is new, steer what stands,
		// take down what died - and the client's world always mirrors
		// the server's.

		void SentryEntity::SetServerAim(float yaw, float pitch) {
			serverAimYaw = yaw;
			serverAimPitch = pitch;
			serverAimPending = true;
		}

		SentryEntity *SentryEntity::FindById(std::uint8_t wireId) {
			for (SentryEntity *t : s_serverTurrets)
				if (t->serverMode && t->wireId == wireId)
					return t;
			return nullptr;
		}

		void SentryEntity::MarkDead() {
			// the one mark the next Client update pass reads: away with
			// the entity when its turn comes (the entities list is the
			// client's own; we never erase from it here)
			serverMode = false;
		}

		void SentryEntity::RetireForUpgrade() {
			// the registry forgets the gun now, so the gun that replaces
			// it may bear the same wire id in the same breath; the entity
			// itself is swept away by the client's next update pass.
			// The blocks stay hidden: the one who asked for this
			// retire hides the same pillar again (an upgrade's swap),
			// and the sender that means a death lifts the hiding itself
			// before calling here - so the pillar never flickers into
			// view between the two models either way.
			auto it = std::find(s_serverTurrets.begin(), s_serverTurrets.end(), this);
			if (it != s_serverTurrets.end())
				s_serverTurrets.erase(it);
			keepHiddenOnDeath = true;
			MarkDead(); // no longer findable, no more wire steering
		}

		bool SentryEntity::SpawnServerTurret(Client &cli, IAudioDevice *audio,
		                                      std::uint8_t wireId, int level,
		                                      IntVector3 lowerBlock) {
			// dedup on the wire's own name: an id in use is an error of
			// ours, not a second gun
			if (FindById(wireId) != nullptr)
				return false;

			// Hide both pillar blocks from the render; the map keeps
			// them solid. The GL map chunk reads IsHidden when it builds
			// its mesh; the game's own paths (IsSolid, the raycasts)
			// never do, so the model is all that is drawn while the world
			// still collides with the pillar it stands for - the player
			// can dig the turret by aiming at the model itself.
			// NOTE: World::GetMap returns a Handle<GameMap>, not a raw
			// pointer: "auto map", never "auto *map"
			auto map = cli.GetWorld()->GetMap();
			map->SetHidden(lowerBlock.x, lowerBlock.y, lowerBlock.z, true);
			map->SetHidden(lowerBlock.x, lowerBlock.y, lowerBlock.z - 1, true);

			// The model REPLACES the hidden pillar on the screen: anchor
			// its base at the ground face under the pillar, i.e.
			// lowerBlock.z + 1. (Map z points down; anchored on the
			// pillar top instead, the model floats two blocks above the
			// ground once the blocks are invisible.)
			Vector3 deployPos((float)lowerBlock.x + 0.5f, (float)lowerBlock.y + 0.5f,
			                  (float)lowerBlock.z + 1.0f);
			if ((float)cg_sentryDebug != 0.f)
				SPLog("server turret %d tier %d spawned @ (%d, %d, %d)",
				      (int)wireId, level, lowerBlock.x, lowerBlock.y, lowerBlock.z);
			cli.AddLocalEntity(stmp::make_unique<SentryEntity>(
				cli, audio, deployPos, level, lowerBlock, wireId));
			return true;
		}

		void SentryEntity::ApplyServerTurrets(Client &cli, std::vector<ServerTurret> list) {
			// Nothing can be steered, spawned or taken down before the
			// world stands; the wire's whole state comes again every
			// five seconds, so this breath is simply missed.
			if (!cli.GetWorld())
				return;

			if (!(float)cg_sentryServer) {
				// the player asked for the bare pillars: every model
				// comes down and every block is uncovered, for good
				std::vector<SentryEntity *> live(s_serverTurrets);
				for (SentryEntity *t : live) {
					UnhidePillarBlocksFor(cli, t->serverBlock);
					t->RetireForUpgrade();
				}
				return;
			}

			std::vector<std::uint8_t> seen;
			seen.reserve(list.size());

			for (const ServerTurret &t : list) {
				if (t.id == 0 || t.level < 1 || t.level > 3)
					continue; // a malformed entry: not ours to draw
				seen.push_back(t.id);

				// the anchor: one pillar block within the map's bounds,
				// its upper partner inside them too
				if (t.x < 0 || t.x >= GameMap::DefaultWidth || t.y < 0 ||
				    t.y >= GameMap::DefaultHeight || t.z < 1 ||
				    t.z >= GameMap::DefaultDepth - 2)
					continue;

				SentryEntity *existing = FindById(t.id);
				if (existing) {
					if (existing->level != t.level) {
						// an upgrade: the same id, a head the wire says
						// is higher than the one that stands. The old
						// model is taken down without uncovering its
						// blocks and the new one seated over them.
						existing->RetireForUpgrade();
						existing = nullptr;
					} else {
						// the same gun: its aim is ours to follow. A
						// locked gun's pack is the server's own aim; an
						// idle gun gives no data, and the entity stands
						// down on its own once what it holds goes stale.
						if (t.locked)
							existing->SetServerAim(DecodeAimYaw(t.yaw_pack),
							                       DecodeAimPitch(t.pitch_pack));
						continue;
					}
				}

				SpawnServerTurret(cli, &cli.GetAudioDevice(), t.id, t.level,
				                  IntVector3(t.x, t.y, t.z));
			}

			// What the list does not name is not standing: the wire's
			// whole truth. Take the unnamed down and lift their blocks'
			// hiding - the server destroyed them, so a block rebuilt
			// there later renders normally.
			std::vector<SentryEntity *> live(s_serverTurrets);
			for (SentryEntity *t : live) {
				if (!t->serverMode)
					continue; // already retired this very packet (an upgrade)
				bool named = false;
				for (std::uint8_t s : seen)
					if (s == t->wireId) {
						named = true;
						break;
					}
				if (named)
					continue;
				UnhidePillarBlocksFor(cli, t->serverBlock);
				t->RetireForUpgrade(); // registry out, entity swept later
			}
		}

		void SentryEntity::ServerTurretShot(Client &cli, std::uint8_t id) {
			SentryEntity *t = FindById(id);
			if (t)
				t->ServerFire();
			// a shot for a gun never named: the state was missed
			// (a packet dropped); the five-second whole of the state
			// buries it. Nothing to do but wait for the whole.
		}

		// the Client's own two halves of the wire, as Smoke.cpp reads
		// them; the thinking lives in the statics above
		void Client::ApplyTurrets(std::vector<ServerTurret> guns) {
			SentryEntity::ApplyServerTurrets(*this, std::move(guns));
		}

		void Client::ServerTurretFired(std::uint8_t id) {
			SentryEntity::ServerTurretShot(*this, id);
		}

		// lift the client-side render hiding of a server turret's two
		// pillar blocks; called when the turret dies by its own blocks.
		// NOT called from the destructor on purpose: during world
		// teardown the map renderer may already be gone, and the flags
		// die with the map
		static void UnhidePillarBlocksFor(Client &cli, IntVector3 serverBlock) {
			auto *world = cli.GetWorld();
			if (!world)
				return;
			auto map = world->GetMap();
			map->SetHidden(serverBlock.x, serverBlock.y, serverBlock.z, false);
			map->SetHidden(serverBlock.x, serverBlock.y, serverBlock.z - 1, false);
		}

		// ---- the local toy: /turretlocal --------------------------------
		// A cosmetic gun for screenshots and calibration scans all by
		// itself. No server traffic, no damage, invisible to the other
		// players; /turret is the SERVER's word and its own thing.
		bool SentryEntity::TryLocalCommand(Client &cli, const std::string &text) {
			bool ours = false;
			int level = 1;
			std::string word = text;
			if (word.rfind("/turretlocal", 0) == 0 || word.rfind("/sentrylocal", 0) == 0) {
				ours = true;
				if (word.size() > 12 && word[12] >= '1' && word[12] <= '3')
					level = word[12] - '0';
			}
			if (!ours)
				return false;

			auto *world = cli.GetWorld();
			if (!world) {
				cli.ShowAlert("No world to stand a turret in.", Client::AlertType::Notice);
				return true;
			}
			auto lp = world->GetLocalPlayer();
			if (!lp) {
				cli.ShowAlert("Spawn in before placing local turrets.",
				              Client::AlertType::Notice);
				return true;
			}
			World::WeaponRayCastResult res =
			  world->WeaponRayCast(lp->GetEye(), lp->GetFront(), lp->GetId());
			if (!res.hit) {
				cli.ShowAlert("Aim at a block to place the turret on it.",
				              Client::AlertType::Notice);
				return true;
			}
			// the face the eye names: the gun's feet on the cell's own
			// face at the hit (a floor hit stands the gun on the floor;
			// a wall hit seats it on the wall, the toy it is)
			IntVector3 cell = res.blockPos;
			Vector3 deployPos((float)cell.x + 0.5f, (float)cell.y + 0.5f,
			                  (float)cell.z);
			cli.AddLocalEntity(stmp::make_unique<SentryEntity>(
				cli, &cli.GetAudioDevice(), deployPos, level));
			cli.ShowAlert("Local turret placed: cosmetics only, fire away.",
			              Client::AlertType::Notice);
			return true;
		}

		// ---- the entity ------------------------------------------------

		SentryEntity::SentryEntity(Client &cli, IAudioDevice *audio, Vector3 deployPos,
		                           int deployLevel, IntVector3 serverBlockPos,
		                           std::uint8_t id)
		    : client(cli), audioDevice(audio), level(Clamp(deployLevel, 1, 3)),
		      position(deployPos), serverBlock(serverBlockPos), wireId(id) {
			SPADES_MARK_FUNCTION();

			// initial team filter for the LOCAL mode's scans (a server
			// gun never scans: the server owns its targeting)
			myTeamId = 0;
			if (client.GetWorld()) {
				auto lp = client.GetWorld()->GetLocalPlayer();
				if (lp)
					myTeamId = lp->GetTeamId();
			}

			// shot samples: four random variations a tier, flat under
			// Turrets (FireTurretMG<tier><1..4>.opus). RegisterSound
			// throws when the file is missing, so each registration is
			// guarded: a missing sample stays silent instead of
			// crashing. NO fallback to another tier's assets.
			if (audioDevice && !client.IsMuted()) {
				for (int i = 0; i < FireSampleCount; i++) {
					char path[96];
					std::snprintf(path, sizeof(path),
					              "Sounds/Turrets/FireTurretMG%d%d.opus", level, i + 1);
					try {
						fireChunks[i] = audioDevice->RegisterSound(path);
					} catch (...) {
					}
				}
				if (level == 3) {
					try {
						spinTickSound =
						  audioDevice->RegisterSound("Sounds/Turrets/SpinTick.opus");
					} catch (...) {
					}
				}
			}

			// models: RegisterModel throws when the file is missing, so
			// each registration is guarded: a missing model leaves the
			// gun standing there all the less for it. The base is the
			// shared tripod; the head is the tier's own.
			auto *r = &client.GetRenderer();
			char path[96];
			std::snprintf(path, sizeof(path), "Models/Turrets/TurretHeadMG%d.kv6", level);
			try {
				headModel = r->RegisterModel(path);
			} catch (...) {
			}
			try {
				baseModel = r->RegisterModel("Models/Turrets/TurretBaseMG.kv6");
			} catch (...) {
			}

			// level 3 only: the minigun's spinning barrels, a SEPARATE
			// model; the level-3 head is the body WITHOUT the barrels
			if (level == 3) {
				try {
					barrelsModel =
					  r->RegisterModel("Models/Turrets/TurretMG3Barrels.kv6");
				} catch (...) {
				}
			}

			// ejected shell casing, the fork's own flat asset; the
			// drop/water sounds are the vanilla SMG's (base pak)
			try {
				casingModel = r->RegisterModel("Models/Turrets/Casing.kv6");
			} catch (...) {
			}
			if (audioDevice && !client.IsMuted()) {
				try {
					casingDropSound1 =
					  audioDevice->RegisterSound("Sounds/Weapons/SMG/ShellDrop1.opus");
					casingDropSound2 =
					  audioDevice->RegisterSound("Sounds/Weapons/SMG/ShellDrop2.opus");
					casingWaterSound =
					  audioDevice->RegisterSound("Sounds/Weapons/SMG/ShellWater.opus");
				} catch (...) {
				}
			}

			// initial head orientation: face the deploying player
			if (client.GetWorld()) {
				auto lp = client.GetWorld()->GetLocalPlayer();
				if (lp) {
					Vector3 d = lp->GetEye() - HeadOrigin();
					d.z = 0.f;
					if (d.GetSquaredLength() > 0.001f)
						currentYaw = atan2f(d.x, d.y);
				}
			}

			// server wire bookkeeping
			serverMode = serverBlockPos.x >= 0;
			if (serverMode)
				s_serverTurrets.push_back(this);
		}

		SentryEntity::~SentryEntity() {
			SPADES_MARK_FUNCTION();
			// the registry only: it removes every live gun's own entry.
			// The blocks' hiding is lifted where the death is decided
			// (ApplyServerTurrets, the Update poll), never here - the
			// entity's Update has already run by the time the sweep
			// reaches the destructor, and during world teardown the
			// map may already be gone; every SetHidden call site
			// keeps its own bookkeeping honest instead.
			auto it = std::find(s_serverTurrets.begin(), s_serverTurrets.end(), this);
			if (it != s_serverTurrets.end())
				s_serverTurrets.erase(it);
		}

		float SentryEntity::FireIntervalSeconds() const {
			float base = (float)cg_sentryFireInterval;
			switch (level) {
				case 2: return base * (2.f / 3.f);
				case 3: return base * 0.5f;
				default: return base;
			}
		}

		Vector3 SentryEntity::HeadOrigin() const {
			// The head rests on TOP of the base: the auto height is the
			// base model's canvas box times the render scale, and
			// cg_sentryHeadZ is the one-time manual seat that puts it
			// on the actual mount (world blocks, negative = lower). Map
			// z is down, so the height is SUBTRACTED to move UP. The
			// fallback (base model missing) keeps the head near the
			// ground.
			float headZ = 0.5f;
			if (baseModel) {
				AABB3 bb = baseModel->GetBoundingBox();
				headZ = (bb.max.z - bb.min.z) * (float)cg_sentryScale;
			}
			headZ += (float)cg_sentryHeadZ;
			return position - MakeVector3(0.f, 0.f, headZ);
		}

		Vector3 SentryEntity::HeadForward() const {
			// yaw 0 = +Y (the model's forward axis)
			return MakeVector3(sinf(currentYaw), cosf(currentYaw), 0.f);
		}

		Vector3 SentryEntity::AimForward() const {
			// map z is down: a POSITIVE pitch looks below the horizon
			return HeadForward() * cosf(currentPitch) +
			       MakeVector3(0.f, 0.f, 1.f) * sinf(currentPitch);
		}

		Vector3 SentryEntity::MuzzlePoint() const {
			// One seat, read everywhere a shot shows: the tracer, the
			// smoke, the puff and the flash all start from THIS
			// point, so the gun reads as one piece however it turns.
			// The frozen reach runs along the aim as ever; the
			// seat rides the gun's OWN frame (right = the render's
			// own right axis, so a lean dialed in facing one way
			// holds facing every way), and the Z of the seat is the
			// world-vertical trim the frozen seats already speak -
			// positive raises, whatever the pitch.
			Vector3 seat = SentryMuzzleSeat(level);
			Vector3 right = Vector3::Cross(HeadForward(),
			                               MakeVector3(0.f, 0.f, 1.f)).Normalize();
			Vector3 muzzle = HeadOrigin() +
			                 AimForward() * (SentryMuzzleDistance(level) + seat.y) +
			                 right * seat.x;
			muzzle.z -= seat.z; // z is down: a positive seat raises
			if (level == 3)
				muzzle.z -= 0.43f; // z is down: raise; frozen 14 Nov
			return muzzle;
		}

		Player *SentryEntity::FindTarget() {
			World *world = client.GetWorld();
			if (!world)
				return nullptr;

			// keep the local toy's team filter in step with the local
			// player's CURRENT team; the last known value stays when
			// they are gone
			auto lp = world->GetLocalPlayer();
			if (lp)
				myTeamId = lp->GetTeamId();

			float range = (float)cg_sentryRange;
			float best = range * range;
			Player *bestPlayer = nullptr;
			Vector3 origin = HeadOrigin();

			// "any bit of head visible = targetable": the eye is tried
			// first (the cheap common case), then four more points
			// around it. A fully hidden head remains ignored; the extra
			// rays are paid only when the eye ray was blocked.
			auto headVisible = [&](Player &p) -> bool {
				Vector3 eye = p.GetEye();
				if (HasLineOfSight(origin, eye))
					return true;
				Vector3 toEye = eye - origin;
				if (toEye.GetSquaredLength() < 0.0001f)
					return true;
				Vector3 up = MakeVector3(0.f, 0.f, -1.f);
				Vector3 lateral =
				  Vector3::Cross(up, toEye.Normalize()).Normalize();
				return HasLineOfSight(origin, eye + up * 0.35f) ||
				       HasLineOfSight(origin, eye - up * 0.35f) ||
				       HasLineOfSight(origin, eye + lateral * 0.25f) ||
				       HasLineOfSight(origin, eye - lateral * 0.25f);
			};

			size_t n = world->GetNumPlayerSlots();
			for (size_t i = 0; i < n; i++) {
				stmp::optional<Player &> maybe = world->GetPlayer((unsigned int)i);
				if (!maybe)
					continue;
				Player &p = *maybe;

				if ((float)cg_sentryDebug != 0.f) {
					float now = world->GetTime();
					if (now >= nextDebugLog) {
						nextDebugLog = now + 1.f;
						SPLog("sentry scan @ (%.1f, %.1f, %.1f): '%s' team=%d alive=%d",
						      origin.x, origin.y, origin.z, p.GetName().c_str(),
						      (int)p.GetTeamId(),
						      (int)(p.IsAlive() && !p.IsSpectator()));
					}
				}

				if (!p.IsAlive() || p.IsSpectator())
					continue;
				if (p.GetTeamId() == myTeamId)
					continue;

				float d = (p.GetEye() - origin).GetSquaredLength();
				if (d < best) {
					if (!headVisible(p))
						continue;
					best = d;
					bestPlayer = &p;
				}
			}
			return bestPlayer;
		}

		bool SentryEntity::HasLineOfSight(Vector3 from, Vector3 to) const {
			// map-only raycast (the engine's own CastRay2): solid blocks
			// block the sight; players never block it here
			Vector3 dir = to - from;
			float len = dir.GetLength();
			if (len < 0.001f)
				return true;

			dir = dir * (1.f / len);

			if (!client.GetWorld())
				return true;
			auto map = client.GetWorld()->GetMap();
			if (!map)
				return true;

			GameMap::RayCastResult res = map->CastRay2(from, dir, LOSRaySteps);
			return !res.hit;
		}

		void SentryEntity::Fire(Player &target) {
			SPADES_MARK_FUNCTION();

			Vector3 muzzle = MuzzlePoint();
			Vector3 aimPoint =
			  target.GetEye() + MakeVector3(0.f, 0.f, (float)cg_sentryAimBody);
			Vector3 aim = aimPoint - muzzle;
			if (aim.GetSquaredLength() < 0.0001f)
				return; // the target stands inside the gun: nothing to draw
			FireVisual(aim.Normalize());
		}

		void SentryEntity::FireVisual(Vector3 aimDir) {
			Vector3 muzzle = MuzzlePoint();

			// the endpoint: the first thing on the sight line (a wall, a
			// player - the local player never blocks their own gun); no
			// hit = the tracer flies out to the range limit
			Vector3 hitPos = muzzle + aimDir * (float)cg_sentryRange;
			if (client.GetWorld()) {
				stmp::optional<int> excludePlayer{};
				auto lp = client.GetWorld()->GetLocalPlayer();
				if (lp)
					excludePlayer = lp->GetId();

				World::WeaponRayCastResult res =
				  client.GetWorld()->WeaponRayCast(muzzle, aimDir, excludePlayer);
				if (res.hit)
					hitPos = res.hitPos;
			}
			if ((hitPos - muzzle).GetSquaredLength() < 0.0001f)
				hitPos = muzzle + aimDir * 0.5f; // never a degenerate shot

			// the engine's own tracer, the very entity the rifles use
			client.AddLocalEntity(stmp::make_unique<Tracer>(client, muzzle, hitPos,
			                                                BulletVel, false));

			// the shot's sound: the engine's spatialized play at the
			// muzzle, a random pick of the four samples like the SMG.
			// CAVEAT: a missing sample leaves the chunk null, and the
			// 3D play dereferences it with no null check: the guard is
			// the silence, not the crash.
			if (audioDevice) {
				AudioParam param;
				param.referenceDistance = 5.f;
				param.volume = (float)cg_sentryVolume;
				Handle<IAudioChunk> &chunk =
				  fireChunks[SampleRandomInt(0, FireSampleCount - 1)];
				if (chunk) {
					audioDevice->Play(chunk.GetPointerOrNull(), muzzle, param);
				}
			}

			// the ejected casing: the vanilla GunCasing's flight, from
			// the head pivot. Levels 1-2 throw left and behind; the
			// minigun keeps the vanilla side: right, forward.
			if (casingModel) {
				Vector3 up = MakeVector3(0.f, 0.f, -1.f);
				Vector3 left = Vector3::Cross(up, aimDir).Normalize();
				Vector3 flyDir;
				float casingScale = 1.f;
				Vector3 casingOrigin = HeadOrigin();
				if (level == 3) {
					flyDir = aimDir * 0.5f - left + up * 0.2f;
					casingScale = 2.f;  // frozen 22 Nov
					casingOrigin += AimForward() * -0.5f; // frozen 14 Nov
				} else {
					flyDir = aimDir * (float)cg_sentryCasingFwd + left + up * 0.2f;
					if (level == 2)
						casingScale = 1.5f; // frozen 22 Nov
				}
				client.AddLocalEntity(stmp::make_unique<SentryCasing>(
					client, casingModel.GetPointerOrNull(),
					SampleRandomBool() ? casingDropSound1.GetPointerOrNull()
					                   : casingDropSound2.GetPointerOrNull(),
					casingWaterSound.GetPointerOrNull(), casingOrigin, aimDir,
					flyDir, casingScale));
			}

			flashTimer = FlashTime;

			if (level == 3) {
				// each shot kicks the spin (wind-down is Update's), and
				// the minigun's trail races up from the muzzle
				barrelsSpinVel =
				  Clamp(barrelsSpinVel + BarrelSpinAccel, 0.f, BarrelSpinMax);
				client.AddLocalEntity(
				  stmp::make_unique<SentrySmokeTrail>(client, muzzle, aimDir));
			} else {
				// levels 1-2: a single flash puff at the muzzle
				try {
					Handle<IImage> img =
					  client.GetRenderer().RegisterImage("Textures/WaterExpl.png");
					if (img) {
						auto puff = stmp::make_unique<ParticleSpriteEntity>(
							client, img, MakeVector4(1.f, 1.f, 1.f, 1.f));
						puff->SetTrajectory(muzzle, MakeVector3(0.f, 0.f, 0.f));
						// a random side, so successive shots never show
						// the same face of the sprite
						puff->SetRotation(SampleRandomFloat() * (float)kTwoPi, 1.6f);
						puff->SetRadius((float)cg_sentryPuffRadius,
						                (float)cg_sentryPuffGrowth);
						float puffLife = (float)cg_sentryPuffLife;
						puff->SetLifeTime(puffLife, 0.02f, puffLife * 0.5f);
						client.AddLocalEntity(std::move(puff));
					}
				} catch (...) {
				}
			}
		}

		void SentryEntity::ServerFire() {
			SPADES_MARK_FUNCTION();
			// the real shot's show along the LAST aim the wire named -
			// not the lerped head pose, which may still be catching up -
			// so the tracer goes where the damage went
			static constexpr float kNoAim = -1000.f;
			if (serverAimPending || serverAimTime > kNoAim + 0.001f) {
				// pending counts too: a shot often lands in the same
				// frame as the state that aims it, before Update
				// stamps the time
				Vector3 fwd = MakeVector3(sinf(serverAimYaw) * cosf(serverAimPitch),
				                          cosf(serverAimYaw) * cosf(serverAimPitch),
				                          sinf(serverAimPitch));
				FireVisual(fwd);
			} else {
				// a shot ahead of any state: fire the way the head
				// faces, the best the client has
				FireVisual(AimForward());
			}
		}

		// The survival poll's grace: the pillar blocks reach the map
		// through the world's deferred block queue (applied at most
		// 1/60 s later); a poll before they land would read the fresh
		// gun as already destroyed
		static constexpr float ServerSpawnGrace = 0.5f;

		bool SentryEntity::Update(float dt) {
			SPADES_MARK_FUNCTION();

			age += dt;

			// taken down by the wire (a death, an upgrade): away
			if (!serverMode && wireId != 0)
				return false;
			// the local toy has an age limit; a wire gun's lifetime is
			// the server's, relayed whole
			if (!serverMode && age >= MaxAge)
				return false;

			// a wire gun dies with its blocks: any of the two pillar
			// blocks gone = the gun is destroyed. Lift their hiding so
			// whatever is rebuilt there renders normally. The grace
			// rides out the blocks' deferred arrival.
			if (serverMode && age >= ServerSpawnGrace) {
				auto *world = client.GetWorld();
				if (!world || !world->GetMap()->IsSolidWrapped(serverBlock.x,
				                                               serverBlock.y,
				                                               serverBlock.z) ||
				    !world->GetMap()->IsSolidWrapped(serverBlock.x, serverBlock.y,
				                                               serverBlock.z - 1)) {
					if ((float)cg_sentryDebug != 0.f)
						SPLog("server turret %d @ (%d, %d, %d): pillar blocks gone",
						      (int)wireId, serverBlock.x, serverBlock.y, serverBlock.z);
					if (keepHiddenOnDeath)
						return false; // an upgrade's swap: the blocks stay hidden
					UnhidePillarBlocksFor(client, serverBlock);
				MarkDead(); // die with our blocks lifted
					return false;
				}
			}

			// the server gun's one beat: follow the wire's aim while it
			// keeps coming, stand down when it stops. It never picks a
			// target of its own: the server owns the targeting, and a
			// client that guessed would aim its show the wrong way.
			if (serverMode) {
				flashTimer -= dt; // the shots come by the wire, the
				                  // muzzle light still fades here
				if (serverAimPending) {
					serverAimPending = false;
					serverAimTime = age;
				}
				bool aimFresh = age - serverAimTime < ServerAimTimeout;
				hasTarget = aimFresh;

				float pitchLimit = MaxAimPitchDeg * (float)kPi / 180.f;
				float desiredPitch = 0.f;
				float desiredYaw = currentYaw;
				if (aimFresh) {
					desiredPitch = Clamp(serverAimPitch, -pitchLimit, pitchLimit);
					desiredYaw = serverAimYaw;
				}
				currentPitch +=
				  Clamp(desiredPitch - currentPitch, -TurnSpeed * dt, TurnSpeed * dt);
				float remain = desiredYaw - currentYaw;
				while (remain > (float)kPi)
					remain -= (float)kTwoPi;
				while (remain < -(float)kPi)
					remain += (float)kTwoPi;
				currentYaw += Clamp(remain, -TurnSpeed * dt, TurnSpeed * dt);

				UpdateBarrels(dt);
				return true;
			}

			// ---- the local toy's own life ----
			fireTimer -= dt;
			flashTimer -= dt;
			UpdateBarrels(dt);

			Player *target = FindTarget();
			hasTarget = target != nullptr;

			float desiredPitch = 0.f;
			if (target && (float)cg_sentryAimPitch != 0.f) {
				Vector3 dv = target->GetEye() - HeadOrigin();
				float hDist = sqrtf(dv.x * dv.x + dv.y * dv.y);
				if (hDist > 0.001f)
					desiredPitch = atan2f(dv.z, hDist);
			}
			float pitchLimit = MaxAimPitchDeg * (float)kPi / 180.f;
			desiredPitch = Clamp(desiredPitch, -pitchLimit, pitchLimit);
			currentPitch +=
			  Clamp(desiredPitch - currentPitch, -TurnSpeed * dt, TurnSpeed * dt);

			if (target) {
				Vector3 d = target->GetEye() - HeadOrigin();
				d.z = 0.f;
				if (d.GetSquaredLength() > 0.001f) {
					float desiredYaw = atan2f(d.x, d.y);
					float remain = desiredYaw - currentYaw;
					while (remain > (float)kPi)
						remain -= (float)kTwoPi;
					while (remain < -(float)kPi)
						remain += (float)kTwoPi;

					currentYaw += Clamp(remain, -TurnSpeed * dt, TurnSpeed * dt);

					// inside the aim cone: shoot
					float aimCone = AimConeDeg * (float)kPi / 180.f;
					if (fabsf(remain) < aimCone &&
					    fabsf(desiredPitch - currentPitch) < aimCone &&
					    fireTimer <= 0.f) {
						fireTimer = FireIntervalSeconds();
						Fire(*target);
					}
				}
			}

			return true;
		}

		/** The level-3 minigun's barrels, one frame: the continuous
		 * wind-down once the shots stop kicking the spin (in
		 * FireVisual), the roll angle wrapped so hours of spinning
		 * never lose float precision, and the spin tick of the
		 * first-person minigun skin, two per revolution, only while
		 * the barrels actually spin. The 3D play dereferences a null
		 * chunk, hence the guards. */
		void SentryEntity::UpdateBarrels(float dt) {
			if (level != 3)
				return; // the other tiers have no barrels to spin

			if (barrelsSpinVel > 0.f) {
				barrelsSpinVel -= BarrelSpinDown * dt;
				if (barrelsSpinVel < 0.f)
					barrelsSpinVel = 0.f;
			}
			barrelsRoll += barrelsSpinVel * dt;
			float wrap = (float)kTwoPi * BarrelRollWrapTurns;
			if (barrelsRoll > wrap)
				barrelsRoll -= wrap;

			if (barrelsSpinVel > SpinTickMinSpeed) {
				spinTickTimer -= dt;
				if (spinTickTimer <= 0.f) {
					if (audioDevice && spinTickSound && !client.IsMuted()) {
						AudioParam param;
						param.referenceDistance = 1.f; // a close mechanical click
						param.volume = (float)cg_sentryVolume * SpinTickVolume;
						audioDevice->Play(spinTickSound.GetPointerOrNull(),
						                  HeadOrigin(), param);
					}
					spinTickTimer += (float)kTwoPi /
					                 (barrelsSpinVel * SpinTicksPerRevolution);
				}
			} else {
				// spin too slow: re-arm the next tick so the first tick
				// of a new burst is instant, like the skin
				spinTickTimer = 0.f;
			}
		}

		void SentryEntity::Render3D() {
			SPADES_MARK_FUNCTION();
			auto *r = &client.GetRenderer();

			float d2r = (float)kPi / 180.f;
			float scale = (float)cg_sentryScale;

			// map space and kv6 model space share the z-down axis (the
			// loader keeps the file's axis), so +Z maps upright models
			Vector3 up = MakeVector3(0.f, 0.f, 1.f);

			// constant orientation corrections, both models
			Matrix4 fix =
			  Matrix4::Rotate(MakeVector3(0.f, 0.f, 1.f), (float)cg_sentryYawFix * d2r) *
			  Matrix4::Rotate(MakeVector3(1.f, 0.f, 0.f), (float)cg_sentryPitchFix * d2r) *
			  Matrix4::Rotate(MakeVector3(0.f, 1.f, 0.f), (float)cg_sentryRollFix * d2r);

			// --- base: the tripod, fixed at the deployment point ---
			if (baseModel) {
				AABB3 bb = baseModel->GetBoundingBox();
				// bb.max.z is the BOTTOM edge of the canvas box (z-down):
				// shifting by -max.z * scale drops that edge onto the
				// deployment point, the feet with a tight canvas
				Vector3 basePos = position - MakeVector3(0.f, 0.f, bb.max.z) * scale;
				Matrix4 mat =
				  Matrix4::FromAxis(MakeVector3(1.f, 0.f, 0.f),
				                    MakeVector3(0.f, 1.f, 0.f), up, basePos) *
				  fix * Matrix4::Scale(scale);
				ModelRenderParam p;
				p.matrix = mat;
				r->RenderModel(*baseModel, p);
			}

			// --- head (and the minigun's barrels): yaw- and
			//     pitch-driven, resting on top of the base ---
			if (headModel || barrelsModel) {
				Vector3 fwd = AimForward();
				Vector3 right =
				  Vector3::Cross(HeadForward(), up).Normalize();
				Vector3 upv = Vector3::Cross(right, fwd).Normalize();

				// the level-3 minigun's body and barrels ride their own
				// scale multipliers and seat offsets (MODEL units, Z
				// down: a negative Z moves UP)
				float headScale = scale;
				float barrelScale = scale;
				Vector3 bodyOff = MakeVector3(0.f, 0.f, 0.f);
				Vector3 barrelOff = MakeVector3(0.f, 0.f, 0.f);
				if (level == 3) {
					headScale *= 0.75f;
					bodyOff.x = -0.01f;
					bodyOff.y = 0.25f;
					bodyOff.z = -0.75f;
					barrelScale *= 0.65f;
					barrelOff.x = 0.f;
					barrelOff.y = -0.07f;
					barrelOff.z = 0.7f; // frozen 22 Nov
				}

				if (headModel) {
					Matrix4 mat = Matrix4::FromAxis(right, fwd, upv, HeadOrigin()) *
					              fix * Matrix4::Translate(bodyOff) *
					              Matrix4::Scale(headScale);
					ModelRenderParam p;
					p.matrix = mat;
					r->RenderModel(*headModel, p);
				}

				// the barrels: mounted on the body, rolled around the
				// TRUE barrel axis by the live spin angle (T(offset) *
				// R * T(-offset), the offset from the kv6 pivot to the
				// axis; y cancels on a Y-axis spin)
				if (barrelsModel) {
					Vector3 pivot = MakeVector3((float)cg_sentryBarrelPivotX, 0.f,
					                             (float)cg_sentryBarrelPivotZ);
					Matrix4 barrelMat =
					  Matrix4::FromAxis(right, fwd, upv, HeadOrigin()) * fix *
					  Matrix4::Translate(bodyOff) *
					  Matrix4::Translate(barrelOff) *
					  Matrix4::Scale(barrelScale) * Matrix4::Translate(pivot) *
					  Matrix4::Rotate(MakeVector3(0.f, 1.f, 0.f), barrelsRoll) *
					  Matrix4::Translate(-pivot);
					ModelRenderParam p;
					p.matrix = barrelMat;
					r->RenderModel(*barrelsModel, p);
				}
			}

			// --- muzzle flash: a short warm light after each shot ---
			if (flashTimer > 0.f && (float)cg_sentryFlash != 0.f) {
				DynamicLightParam light;
				light.type = DynamicLightTypePoint;
				light.origin = MuzzlePoint();
				light.radius = (float)cg_sentryFlashRadius * (flashTimer / FlashTime);
				light.color = MakeVector3(1.0f, 0.8f, 0.5f);
				r->AddLight(light);
			}
		}
	} // namespace client
} // namespace spades
