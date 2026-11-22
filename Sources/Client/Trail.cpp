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
#include <cmath>

namespace spades {

	namespace client {

	// The whole way: the master switch, for a headset that must draw every
	// world twice and would rather its frame went to the dead it can see.
	DEFINE_SPADES_SETTING(cg_hordeTrails, "1");
	// Blocks between crumbs at the eye; the farther out the line runs, the
	// wider the crumbs are spaced (see Draw), so the setting sets the busy
	// ground's density alone.
	DEFINE_SPADES_SETTING(cg_hordeTrailSpacing, "2.0");
	// A crumb's whole size, in blocks: a knot small enough to read as a
	// dotted line from the stand, big enough to find again.
	DEFINE_SPADES_SETTING(cg_hordeTrailSize, "0.4");
	// How solid a crumb stands; they do not shade or cast, so this is all
	// there is to their look.
	DEFINE_SPADES_SETTING(cg_hordeTrailOpacity, "0.85");
	// 0: the ways are drawn where the world hides them not at all; 1: the
	// crumbs also ghost through the terrain, a way in underground read
	// from above it. Doubles the crumbs drawn; off by default.
	DEFINE_SPADES_SETTING(cg_hordeTrailThrough, "0");

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
		if ((int)cg_hordeTrails == 0)
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
		float spacing = static_cast<float>(cg_hordeTrailSpacing);
		if (!(spacing >= 0.5F))
			spacing = 0.5F;
		const float size = std::max(0.05F, static_cast<float>(cg_hordeTrailSize));
		const float opacity =
		  std::max(0.05F, std::min(1.0F, static_cast<float>(cg_hordeTrailOpacity)));
		const bool through = (int)cg_hordeTrailThrough != 0;

		ModelRenderParam param;
		param.ghost = true;            // no shading, no shadow: a marker, flat
		param.castShadow = false;
		param.drawOutline = false;
		param.opacity = opacity;
		param.xray = through;
		// The knot itself is voxels of (0, 0, 0): the renderer paints them
		// with the front's own colour, the whole way one colour held.
		const float scale = size / 3.0F;

		int budget = kMaxCrumbs;
		for (const Path& p : paths) {
			if (p.crumbs.empty())
				continue;
			param.customColor = p.color;
			param.xrayColor = p.color;

			// The line walked, a crumb a step and a step stretching with
			// distance: the crumbs near the eye tell the way to the
			// doorstep, the crumbs far out hint it, the crumbs past the
			// fog are not drawn at all.
			const std::vector<Vector3>& crumbs = p.crumbs;
			size_t i = 0;
			while (i < crumbs.size() && budget > 0) {
				const Vector3& c = crumbs[i];
				const float distSq = (viewOrigin - c).GetSquaredLength();
				if (distSq < FOG_DISTANCE_SQ) {
					param.matrix = Matrix4::Translate(c) * Matrix4::Scale(scale);
					renderer.RenderModel(*model, param);
					--budget;

					// The spacing, stretched by how far out this crumb
					// fell: twice the setting at the fog, the setting at
					// the eye.
					const float dist = std::sqrt(distSq);
					int stride =
					  static_cast<int>(spacing * (1.0F + dist / 128.0F) + 0.5F);
					if (stride < 1)
						stride = 1;
					i += static_cast<size_t>(stride);
				} else {
					// Out past the fog: skip ahead in spans of the fog
					// itself rather than a crumb at a time it will not draw.
					i += 32;
				}
			}
		}
	}
	} // namespace client
} // namespace spades
