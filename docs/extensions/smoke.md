# Smoke

Smoke the server puts in the world: a wide cloud, a rising column, or a
heavy vapour that pools at the floor, in a
colour and an opacity, of a size, for a time.

| ------------: | ------------- |
| Extension ID: | `0x3F`        |
| Packet ID:    | `0x7F`        |
| Version:      | 2             |
| Type:         | `HAS_PACKETS` |

Written in the style of [aosprotocol](https://github.com/zerospades/aosprotocol)'s
extensions, for ZeroSpades X; not registered there (yet). `0x3F` is clear of
every id registered or proposed in aosprotocol as of 13 November 2026.

### Sub Packets:

| Sub ID | Name        | Direction        | Size |
|--------|-------------|------------------|------|
| 0      | Smoke       | Server -> Client | 25   |
| 1      | Clear Smoke | Server -> Client | 3    |
| 2      | Path        | Server -> Client | 11 + 3 n |

## What smoke is

Smoke is only drawn. It hides what is behind it on the clients that draw it,
and does nothing else: no damage, no block, nothing the server simulates. A
client without the extension sees nothing, so a server should use it where
that is fine (a marker, a cloud where something happened, a trail), or know
that only some of its players are hidden by it.

A client draws smoke whatever its particle setting, since turning particles
down must not see through it.

Smoke is an event in time, not part of the map: [Map Start](https://github.com/zerospades/aosprotocol/blob/main/protocol075.md#map-start-075)
clears it all. A player who joins while a smoke is still coming does not see
it unless the server sends it to them again (with what is left of its
duration).

## Sub ID 0: Smoke

| Field Name    | Field Type | Example        | Notes |
|---------------|------------|----------------|-------|
| Packet ID     | UByte      | `0x7F`         | Always `0x7F`. |
| Sub Packet ID | UByte      | `0`            | Always `0` for this sub-packet. |
| Smoke ID      | UByte      | `3`            | `0`-`254`: the server's name for it, for Clear Smoke; a new Smoke with an id in use replaces the old one. `255`: none, it cannot be cleared. |
| Style         | UByte      | `0`            | See [Style](#style). |
| X             | LE Float   | `256.5`        | Where it comes from, in world coordinates. |
| Y             | LE Float   | `256.5`        | |
| Z             | LE Float   | `60.0`         | As everywhere, Z grows downwards: the ground is a larger Z. |
| Radius        | UByte      | `5`            | How far it spreads, in blocks, `1`-`255`. A grenade's cloud is about `5`. |
| Red           | UByte      | `210`          | The colour. |
| Green         | UByte      | `210`          | |
| Blue          | UByte      | `210`          | |
| Opacity       | UByte      | `250`          | `0` clear to `255` opaque, for each puff; puffs over one another are thicker. |
| Duration      | LE UShort  | `100`          | How long it keeps coming, in tenths of a second (`100` = 10 s, up to 6553.5 s). |
| Fade          | UByte      | `50`           | How long it takes to go once it stops, in tenths of a second (`50` = 5 s). |
| Flags         | UByte      | `1`            | Bit 0: hiss, a sound where it comes from, for as long as it keeps coming. Other bits: `0`, ignored. |

### Style

| Value | Name   | Look |
|-------|--------|------|
| 0     | Cloud  | A wide, low cloud, spreading out over its radius: a smoke screen. |
| 1     | Column | A thin plume rising straight up from where it is: a marker. Radius sets how wide. |
| 2     | Ground | A heavy vapour hardly a block deep, hugging the floor and renewed for its whole stay like it is freshly poured: a trail. Nothing climbs out of it. |

A client draws a style it does not know as `0`.

## Sub ID 1: Clear Smoke

| Field Name    | Field Type | Example | Notes |
|---------------|------------|---------|-------|
| Packet ID     | UByte      | `0x7F`  | Always `0x7F`. |
| Sub Packet ID | UByte      | `1`     | Always `1` for this sub-packet. |
| Smoke ID      | UByte      | `3`     | The smoke to stop, or `255` for every one. |

The smoke stops coming; what is already out fades over its Fade. An id with
no smoke is ignored.

## Sub ID 2: Path

Version 2 of the extension. A client that claimed version 1 draws smoke
and clears only; a version-1 client receiving a Path ignores it as an
unknown sub packet, so a server may send both kinds and let each client
take what it understands - or, as ZeroSpades X's server does, sort its
players by the version they claimed and send Path to those with `2`, the
three-sub-packet Smoke, and the beds of Ground smoke to the rest.

| Field Name    | Field Type    | Example  | Notes |
|---------------|---------------|----------|-------|
| Packet ID     | UByte         | `0x7F`   | Always `0x7F`. |
| Sub Packet ID | UByte         | `2`      | Always `2` for this sub-packet. |
| Path ID       | UByte         | `4`      | The server's name for this path. A Path with an id in use replaces the old one; `Count` `0` takes it away. |
| Red           | UByte         | `255`    | The colour the whole path is drawn in. |
| Green         | UByte         | `160`    | |
| Blue          | UByte         | `0`      | |
| Count         | UByte         | `11`     | How many points the path has, `0`-`255`. `0`: forget this path. |
| First X       | LE Short      | `-3`     | The first point, in world cells (block coordinates, not centred). |
| First Y       | LE Short      | `128`    | |
| First Z       | UByte         | `32`     | As everywhere, Z grows downwards. The surface the path runs on. |
| Delta X 1     | Byte          | `96`     | Each later point, as the difference from the one before it, `-128` to `127`. |
| Delta Y 1     | Byte          | `0`      | |
| Delta Z 1     | Byte          | `1`      | |

Point 0 is `First X/Y/Z` (5 bytes); each of points `1` to `Count - 1` is
`Delta X, Delta Y, Delta Z` (3 bytes), so the packet is `11 + 3 Count`
bytes long, and a route of ten turns is about `70` bytes where its bed upon
bed of Ground smoke was ten packets of their own.

### What a path is

A path is a line the server names, drawn by the client on its own. It is
how a long trail outlives the smoke it was once drawn with: the beds of
sub-packet 0 are each a bounded event in time and place, a trail of them
runs out of budget on the client before it runs out of length, while a
path is one small packet however long the way it describes - the client
keeps the line and draws it until the server says otherwise.

The points are waypoints, not every tread: the server sends a point at
each turn, at each rise or fall that leaves the level of the last point
by more than it wants the line to swing, and wherever a straight run must
be broken to stay within a byte of delta; the client joins them up. The
line is drawn on the floor along the way it was sent, a marker of how
somewhere is to be reached rather than a thing hiding in the world: only
drawn, like the smoke, nothing the server simulates follows from it.

A path lasts until another Path with its id replaces it, a Path with its
id and `Count` `0` takes it away, or the map changes; a player who joins
mid-game sees no path until the server sends it again, which it should do
every few seconds, since the packets are small enough to repeat.

## Examples

A smoke screen, 10 s, grey, with the hiss:

    7F 00  FF  00  <x> <y> <z>  05  D2 D2 D2 FA  64 00  32  01

A green marker column over a supply drop, two minutes, cleared when taken:

    7F 00  03  01  <x> <y> <z>  02  30 E0 40 E6  B0 04  1E  00
    ...
    7F 01  03

An orange way in, five waypoints, path 2:

    7F 02  02  FF A0 00  05  88 00... (first X/Y/Z) <15 deltas>

## Later

A thrown smoke grenade, a player's own, belongs with the weapons a server
gives (aosprotocol's Custom Weapon draft, #22): the server would know the
player threw smoke and send Smoke where it lands. Nothing here changes what
a grenade does.

See [Extensions](https://github.com/zerospades/aosprotocol/blob/main/extensions/extension.md)
for how the extension is negotiated.
