/*
 Copyright (c) 2026 ZeroSpades developers.

 This file is part of ZeroSpades, a fork of OpenSpades.

 ZeroSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published
 by the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 ZeroSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with ZeroSpades.	 If not, see <http://www.gnu.org/licenses/>.

 */

// The ways in, drawn: the Smoke extension's second version sends a front's
// line of march as a handful of waypoints, and this end of the wire opens
// it into crumbs along the ground - the marker a defender watches for,
// without a bed of vapour per tread or a budget that runs out.

#include "Trail.h"

#include "GameConstants.h"
#include "IRenderer.h"
#include <Core/Settings.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace spades {

	namespace client {

	// The whole way: the master switch, for a headset that must draw every
	// world twice and would rather its frame went to the dead it can see.
	DEFINE_SPADES_SETTING(cg_towerDefenseTrails, "1");
	// Blocks between crumbs at the eye; the farther out the line runs, the
	// wider the crumbs are spaced (see Draw), so the setting sets the busy
	// ground's density alone.
	DEFINE_SPADES_SETTING(cg_towerDefenseTrailSpacing, "2.0");
	// A crumb's whole size, in blocks: a knot small enough to read as a
	// dotted line from the stand, big enough to find again.
	DEFINE_SPADES_SETTING(cg_towerDefenseTrailSize, "0.4");
	// How solid a crumb stands; they do not shade or cast, so this is all
	// there is to their look.
	DEFINE_SPADES_SETTING(cg_towerDefenseTrailOpacity, "0.85");
	// 0: the ways are drawn where the world hides them not at all; 1: the
	// crumbs also ghost through the terrain, a way in underground read
	// from above it. Doubles the crumbs drawn; off by default.
	DEFINE_SPADES_SETTING(cg_towerDefenseTrailThrough, "0");
	// The pace the ways walk, in lengths a second: a crumb reaches its
	// neighbour's place once a period, and the line reads as what it
	// stands for - the wave, coming in. 0 and the ways stand still.
	DEFINE_SPADES_SETTING(cg_towerDefenseTrailFlow, "1.0");

	void TrailLayer::SetPath(uint8_t front, const Vector3& color, std::vector<Vector3> crumbs) {
		if (paths.size() <= front)
			paths.resize(front + 1);
		Path& p = paths[front];
		p.color = color;
		p.crumbs = std::move(crumbs);
	}

	void TrailLayer::Forget(uint8_t front) {
		if (paths.size() > front)
			paths[front].crumbs.clear();
	}

	void TrailLayer::ForgetAll() {
		for (Path& p : paths)
			p.crumbs.clear();
	}

	void TrailLayer::Draw(IRenderer& renderer, const Vector3& viewOrigin) {
		if ((int)cg_towerDefenseTrails == 0)
			return;

		// The one thing a crumb is: looked for once, and its absence - a
		// pack without the knot - leaves every other way of this marker
		// working, merely undrawn, like a skin nobody installed.
		if (!triedModel) {
			triedModel = true;
			try {
				model = renderer.RegisterModel("Models/Effects/Trail/Breadcrumb.kv6");
			} catch (...) {
				// model file missing: no way in is drawn
			}
		}
		if (!model)
			return;

		// What a look is worth telling: a spacing set low (or to zero, the
		// setting taken at its floor) turns a hint to a ribbon.
		float spacing = static_cast<float>(cg_towerDefenseTrailSpacing);
		if (!(spacing >= 0.5F))
			spacing = 0.5F;
		const float size =
		    std::max(0.05F, static_cast<float>(cg_towerDefenseTrailSize));
		const float opacity =
		  std::max(0.05F, std::min(1.0F,
		             static_cast<float>(cg_towerDefenseTrailOpacity)));
		const bool through = (int)cg_towerDefenseTrailThrough != 0;
		const float flow = std::max(0.0F,
		    static_cast<float>(cg_towerDefenseTrailFlow));

		// The clock the ways walk by is this side's own: no packet and no
		// server keeps it, for the walk is a look alone - the line means
		// the same whether drawn, thinned or not drawn at all.
		static const std::chrono::steady_clock::time_point walkStart =
		  std::chrono::steady_clock::now();
		const float now =
		  std::chrono::duration<float>(std::chrono::steady_clock::now() - walkStart)
		    .count();

		ModelRenderParam param;
		param.ghost = true;            // no shading, no shadow: a marker, flat
		param.castShadow = false;
		param.drawOutline = false;
		param.opacity = opacity;
		param.xray = through;
		// The knot itself is voxels of (0, 0, 0): the renderer paints them
		// with the front's own colour, the whole way one colour held.

		int budget = kMaxCrumbs;
		for (size_t front = 0; front < paths.size(); front++) {
			const Path& p = paths[front];
			if (p.crumbs.empty())
				continue;
			param.customColor = p.color;
			param.xrayColor = p.color;

			// The breadth of a way, its own. Two colours laid over one
			// ground fight for the same faces - two ways of one breadth
			// share their planes, and the renderer parts them only by
			// the flip of a coin - so each front stands a touch broader
			// or a touch slighter than its neighbour, front by front,
			// every way one breadth along itself, and the planes that
			// overlapped fall out of step, the flicker gone with them.
			// (The number taken off before it is made a float: unsigned
			// has no negatives, and nought less six went once round the
			// whole world, a girth of billions that the renderer drew
			// as a cube past the sky, every even front's way unseen.)
			const float girth =
			  1.0F + (static_cast<float>((front * 7U) % 13U) - 6.0F) * (1.0F / 60.0F);
			const float scale = size / 3.0F * girth;

			// One way's own beat. A line reads as a line only walking
			// whole, so one front's crumbs lock step with one another;
			// but two ways sharing a ground - a crossing, a stair, a run
			// laid over a run - walk their own rounds out of step with
			// each other, their own drum and their own downbeat, and
			// the eye parts them the way it parts two voices in a song.
			// Stood back in step, they would read as one broad way.
			float u = 0.0F;
			if (flow > 0.0F) {
				const float drum =
				  1.0F + 0.13F * std::sin(2.399F * static_cast<float>(front));
				u = std::fmod(now * flow * drum + 0.618F * static_cast<float>(front),
				              1.0F);
			}

			// The line walked, a crumb a step and a step stretching with
			// distance: the crumbs near the eye tell the way to the
			// doorstep, the crumbs far out hint it, the crumbs past the
			// fog are not drawn at all.
			const std::vector<Vector3>& crumbs = p.crumbs;
			const size_t last = crumbs.size() - 1;
			size_t i = 0;
			bool drewAny = false;
			Vector3 first(0.0F, 0.0F, 0.0F);
			Vector3 drawn(0.0F, 0.0F, 0.0F);
			while (i < crumbs.size() && budget > 0) {
				const Vector3& c = crumbs[i];
				const float distSq = (viewOrigin - c).GetSquaredLength();
				if (distSq < FOG_DISTANCE_SQ) {
					const float dist = std::sqrt(distSq);
					int stride =
					  static_cast<int>(spacing * (1.0F + dist / 128.0F) + 0.5F);
					if (stride < 1)
						stride = 1;

					// Where this crumb stands while the way walks: its
					// place slid along the line by this frame's share of
					// a period, carried between crumbs by the way's own
					// floor - the march rides the ground the line rides,
					// keeps its turns, and the two ends of the way
					// stand still however long the middle walks.
					const float slide = u * static_cast<float>(stride);
					const size_t m = std::min(i + static_cast<size_t>(slide), last);
					const size_t n = std::min(m + 1, last);
					const float f = slide - static_cast<float>(static_cast<size_t>(slide));
					const Vector3 pos = crumbs[m] + (crumbs[n] - crumbs[m]) * f;

					param.matrix = Matrix4::Translate(pos) * Matrix4::Scale(scale);
					renderer.RenderModel(*model, param);
					--budget;
					if (!drewAny)
						first = pos;
					drawn = pos;
					drewAny = true;

					// The spacing, stretched by how far out this crumb
					// fell: twice the setting at the fog, the setting at
					// the eye. Each dot slides over exactly the gap it
					// keeps, so when a period turns and every dot steps
					// back to its own place, each had reached its
					// neighbour's - the same line by another dot, a
					// wrap the eye cannot catch.
					i += static_cast<size_t>(stride);
				} else {
					// Out past the fog: skip ahead in spans of the fog
					// itself rather than a crumb at a time it will not draw.
					i += 32;
				}
			}

			// The door the way came in by: once the first walking dot
			// has left it, an anchored crumb stands on it - a defender
			// reads the way by where it starts, and the start does not
			// wander with the walk.
			if (drewAny && budget > 0 && crumbs.size() > 1 &&
			    (crumbs[0] - first).GetSquaredLength() > 0.25F) {
				param.matrix = Matrix4::Translate(crumbs[0]) * Matrix4::Scale(scale);
				renderer.RenderModel(*model, param);
				--budget;
			}
			// The wave's own head, the far end of its way: whatever the
			// stride skipped or the slide carried, the front itself
			// stands marked - the dot to watch, the rest the road to it.
			if (drewAny && budget > 0 &&
			    (crumbs[last] - drawn).GetSquaredLength() > 0.25F) {
				param.matrix = Matrix4::Translate(crumbs[last]) * Matrix4::Scale(scale);
				renderer.RenderModel(*model, param);
				--budget;
			}
		}
	}
	} // namespace client
} // namespace spades
