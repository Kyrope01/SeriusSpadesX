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

#pragma once

#include <cstdint>
#include <vector>

#include "IModel.h"
#include <Core/Math.h>

namespace spades {

	namespace client {
		class IRenderer;

		/**
		 * The ways in, one line a front, drawn by this client on its own -
		 * the Smoke extension's Path sub packet (version 2), one small
		 * packet of waypoints however long the way, against a budget of
		 * smoke beds that ran out long before the way did.
		 *
		 * A path is kept, not played: it is drawn until the server
		 * replaces it, takes it away, or the map changes. It is only a
		 * marker - nothing the client simulates follows from it.
		 *
		 * Every way is laid out as crumbs, small coloured knots along the
		 * line at the spacing the settings say; the spacing stretches with
		 * distance, since a crumb the eye can no longer tell from its
		 * neighbour is one not worth drawing, and a headset that must
		 * draw the world twice can thin its ways to a hint or turn them
		 * off whole.
		 */
		class TrailLayer {
			/** One front's way: its colour and its crumbs, drawn places. */
			struct Path {
				Vector3 color;
				std::vector<Vector3> crumbs;
			};

			std::vector<Path> paths; // indexed by the front id the server sent

			// The crumb the crumbs are: a knot of voxels in the resource
			// pack, coloured (0, 0, 0) so the renderer paints it in the
			// front's own colour. Registered on first draw; a pack without
			// it draws no way in, and the layer otherwise works.
			Handle<IModel> model;
			bool triedModel = false;

		public:
			/** How many crumbs every way in holds at most, however many
			 * fronts and lengths: each crumb is a model drawn, and the
			 * frame has others to carry. */
			static constexpr int kMaxCrumbs = 1280;

			/** Replaces the front's line, or lays one where none was. */
			void SetPath(uint8_t front, const Vector3& color, std::vector<Vector3> crumbs);
			/** Takes the front's line away. */
			void Forget(uint8_t front);
			/** Takes every line away: the map has changed, and its ways
			 * mean nothing in the next one. */
			void ForgetAll();

			/** Draws the ways that are set, thinned by distance. */
			void Draw(IRenderer& renderer, const Vector3& viewOrigin);
		};
	} // namespace client
} // namespace spades
