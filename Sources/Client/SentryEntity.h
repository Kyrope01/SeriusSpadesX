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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ILocalEntity.h"
#include <Core/Math.h>
#include <Core/RefCountedObject.h>

namespace spades {

	namespace client {
		class Client;
		class IAudioDevice;
		class IAudioChunk;
		class IModel;
		class Player;

		/** One sentry gun as the Smoke extension's third version says
		 * it: the wire's own name for it, its tier, the two pillar
		 * blocks it stands on (the lower one named; the upper is
		 * z - 1), whether it is on a walker, and the server's aim
		 * quantized for travel. Parsed by Smoke.cpp's Turrets case,
		 * applied whole by Client::ApplyTurrets. */
		struct ServerTurret {
			std::uint8_t id = 0;       // 1..255, never 0
			std::uint8_t level = 1;    // 1..3
			int x = 0, y = 0;          // the pillar's own column
			int z = 0;                 // the pillar's LOWER block
			bool locked = false;       // 1 while the gun is on a walker
			std::uint8_t yaw_pack = 0;    // 128 steps round, 0 = +Y
			std::uint8_t pitch_pack = 0;  // 127 steps, -80 climb..+80 descent
		};

		/** The animated sentry gun. A sentry server-side is a pillar of
		 * two real blocks: walked on, shot at, dug out like any wall,
		 * its damage the server's own. This entity is the eye's half:
		 * the model over the blocks (which it hides from the render,
		 * never from the game), turning as the server says its gun
		 * turns, firing its show the moment the server says a shot
		 * lands. Purely cosmetic; the fork's client only.
		 *
		 * Assets, flat under Turrets/ (the fork's own layout): one
		 * shared base (TurretBaseMG1.kv6), a head per tier
		 * (TurretHeadMG1..3.kv6), the level-3 minigun's barrels as a
		 * separate spinning model (TurretMG3Barrels.kv6), one casing
		 * (Casing.kv6) and the fire samples by tier
		 * (FireTurretMG<tier><1..4>.opus) with the level-3 barrel
		 * spin tick (SpinTick.opus). Every registration is guarded: a
		 * missing file leaves the gun standing all the less for it,
		 * never the game down.
		 *
		 * It also lives a whole second life, local only: /turretlocal
		 * stands a cosmetic gun on the block the eye names, one that
		 * scans and fires all by itself - a toy for screenshots and
		 * calibration, no server traffic, no damage, invisible to
		 * everyone else. */
		class SentryEntity : public ILocalEntity {
			Client &client;
			IAudioDevice *audioDevice; // may be null when muted

			int level; // tier 1-3 (clamped)

			// four fire samples (like the SMG): a random one per shot.
			// Missing files stay silent.
			static constexpr int FireSampleCount = 4;
			Handle<IAudioChunk> fireChunks[FireSampleCount];

			// the shared tripod and the tier's own head; both may be
			// null when the assets are absent (invisible sentry)
			Handle<IModel> baseModel;  // Models/Turrets/TurretBaseMG1.kv6
			Handle<IModel> headModel;  // Models/Turrets/TurretHeadMG<1..3>.kv6

			// level 3 only: the minigun's spinning barrel assembly,
			// a SEPARATE model rolled around the gun's forward axis
			Handle<IModel> barrelsModel; // Models/Turrets/TurretMG3Barrels.kv6

			// ejected shell casing: the fork's own model + vanilla SMG
			// drop/water sounds; a missing model = no casing
			Handle<IModel> casingModel; // Models/Turrets/Casing.kv6
			Handle<IAudioChunk> casingDropSound1;
			Handle<IAudioChunk> casingDropSound2;
			Handle<IAudioChunk> casingWaterSound;
			// level-3 barrel spin tick (may stay null = silent spin)
			Handle<IAudioChunk> spinTickSound;

			Vector3 position; // where the model's feet stand
			int myTeamId;      // team filter for the LOCAL mode's scans

			// ---- server mode ----
			// A gun the Smoke wire named: it lives exactly as long as
			// the wire says it does, never picks a target of its own,
			// and dies with its blocks. wireId is the wire's own name,
			// the bookkeeping key; the pillar's lower block is the
			// seat the model hides.
			bool serverMode = false;
			std::uint8_t wireId = 0;
			IntVector3 serverBlock; // lower pillar block (the anchor)
			// an upgrade re-lists the same id at a new level: the old
			// entity is taken down WITHOUT lifting the blocks' hiding
			// (the new one hides them again), so the pillar never
			// flickers into view between the two
			bool keepHiddenOnDeath = false;

			// the server's aim, handed by ApplyServerTurrets; the
			// entity lerps the head toward it while fresh and stands
			// down once it stops coming
			bool serverAimPending = false;
			float serverAimYaw = 0.f;
			float serverAimPitch = 0.f;
			float serverAimTime = -1000.f; // entity age of the last aim
			// seconds without aim data = considered idle again
			static constexpr float ServerAimTimeout = 0.75f;

			// all the server's live guns by wire id; entries remove
			// themselves in the destructor
			static std::vector<SentryEntity *> s_serverTurrets;

			float currentYaw = 0.f;   // smooth head yaw (radians, 0 = +Y)
			float currentPitch = 0.f; // gun pitch (positive = below horizon)
			bool hasTarget = false;
			float fireTimer = 0.f;  // seconds until the next shot
			float flashTimer = 0.f; // muzzle flash decay
			float age = 0.f;

			// level 3 only: the minigun's barrel roll. Each shot kicks
			// the spin up (in FireVisual, clamped at BarrelSpinMax);
			// Update decelerates it continuously once the firing
			// stops, so the barrels wind up while firing and wind down
			// after, like the first-person minigun skin
			float barrelsRoll = 0.f;    // current roll angle, radians
			float barrelsSpinVel = 0.f; // current roll speed, rad/s
			float spinTickTimer = 0.f;  // countdown to the next spin tick

			// cg_sentryDebug log throttle, per instance
			float nextDebugLog = 0.f;

			// ---- tunables (see SentryEntity.cpp) -----------------
			static constexpr float TurnSpeed = 4.0f; // head, radians/s
			static constexpr float AimConeDeg = 10.f; // local mode's fire cone
			static constexpr float MaxAimPitchDeg = 80.f; // gun pitch clamp
			static constexpr float BulletVel = 500.f; // the tracer's speed
			static constexpr float FlashTime = 0.08f;  // muzzle light lifetime
			static constexpr int LOSRaySteps = 256; // local scan's ray budget
			static constexpr float MaxAge = 3600.f;  // local mode only
			static constexpr float BarrelSpinMax = 25.f;   // rad/s
			static constexpr float BarrelSpinAccel = 6.f;  // kick per shot
			static constexpr float BarrelSpinDown = 8.f;   // rad/s^2
			static constexpr float BarrelRollWrapTurns = 100.f;
			static constexpr float SpinTickVolume = 0.08f;
			static constexpr float SpinTickMinSpeed = 1.f;
			static constexpr float SpinTicksPerRevolution = 2.f;

			public:
			/** serverBlock: the LOWER pillar block (negative x = none:
			 * a plain local deploy, e.g. /turretlocal). */
			SentryEntity(Client &client, IAudioDevice *audioDevice,
			             Vector3 deployPos, int level = 1,
			             IntVector3 serverBlock = IntVector3(-1, 0, 0),
			             std::uint8_t wireId = 0);
			~SentryEntity();

			/** The whole set of the server's guns, as the Smoke wire
			 * says it: replaces everything - what the list does not
			 * name is not standing. Spawns the models, steers the
			 * aims, takes down the guns that died. Gated by
			 * cg_sentryServer. */
			static void ApplyServerTurrets(Client &client,
			                                std::vector<ServerTurret> list);
			/** One real server-side shot: the show of it (tracer,
			 * flash, sound) along the server's last aim, at the exact
			 * moment the damage lands. */
			static void ServerTurretShot(Client &client, std::uint8_t id);
			/** The local toy: /turretlocal [1-3] (and its /sentrylocal
			 * alias) stands a cosmetic gun on the block the eye
			 * names. @return 1 when the text was one of ours. */
			static bool TryLocalCommand(Client &client, const std::string &text);

			/** @return false when the entity should be removed. */
			bool Update(float dt) override;
			void Render3D() override;

			private:
			/** A gun's blocks hidden from the render (they stay solid
			 * to the game); the model anchored on the pillar's ground
			 * face. @return false when a gun already stands there. */
			static bool SpawnServerTurret(Client &client, IAudioDevice *audio,
			                              std::uint8_t wireId, int level,
			                              IntVector3 lowerBlock);
			/** Find the live gun the wire knows by this id, null when
			 * none. */
			static SentryEntity *FindById(std::uint8_t wireId);
			/** Take an entity down through the world's entity list:
			 * marks it dead and lets the next Client update sweep
			 * it. (The entities list is the client's own; we only
			 * mark, never erase here.) */
			void MarkDead();
			/** Take the entity down now, as an upgrade does: without
			 * lifting the blocks' hiding. */
			void RetireForUpgrade();
			/** The visual of one shot (tracer, flash, sound, casing,
			 * barrel kick) along the given direction; shared by the
			 * local mode's Fire and the server's shots. */
			void FireVisual(Vector3 aimDir);
			/** Steer this gun toward the server's current aim. */
			void SetServerAim(float yaw, float pitch);
			/** The level-3 minigun's barrel spin, one frame: wind-down,
			 * roll wrap and the spin tick. */
			void UpdateBarrels(float dt);
			/** Fire along the server's last named aim; when none ever
			 * came (a shot ahead of its state), fire the way the head
			 * currently faces. */
			void ServerFire();
			/** The local mode's own targeting: nearest enemy in range
			 * and visible from the head. */
			Player *FindTarget();
			/** Per-tier fire interval from the one console-tunable
			 * base (cg_sentryFireInterval). */
			float FireIntervalSeconds() const;
			/** True when no solid block stands between the two points. */
			bool HasLineOfSight(Vector3 from, Vector3 to) const;
			Vector3 HeadOrigin() const;
			Vector3 HeadForward() const; // horizontal, yaw only
			Vector3 AimForward() const;  // yaw and pitch
			/** The muzzle of the tier's own gun, the one point every
			 * shot's show anchors to (tracer, smoke, puff, flash):
			 * the head pivot plus the frozen reach, the level-3
			 * world trim, and the seat triple the calibration
			 * rounds locked in as literals (X leans to the
			 * barrel's right, Y stretches the reach, Z raises). */
			Vector3 MuzzlePoint() const;
			void Fire(Player &target);   // the local mode's shot
		};
	} // namespace client
} // namespace spades
