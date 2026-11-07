# LoadBalancing

How a Photon Realtime client moves between servers, the codes it uses, and the server behaviour it depends on. A client opens a new ENet connection for each server it's sent to.

## Flow

**1. Name server.** Connect, Init, InitEncryption, then:

- `GetRegions` (220) with the AppId (224). The response lists region codes (210, string[]) and their master addresses (230, string[]); the client then pings each region. A client with a fixed region skips this step.
- `Authenticate` (230) with the AppVersion (220), AppId (224), region (210) and UserId (225). The response carries the UserId (225), the master address (230) and a token (221).

**2. Master.** Connect, Init, InitEncryption, then authenticate with the token. With AuthMode AuthOnce the token rides in the Init instead and the client waits for an Authenticate response without sending the operation. Then:

- `JoinLobby` (229), optionally naming a lobby (213) and lobby type (212). The server answers, then sends a `GameList` event (230) with every listed room (222: room name → properties). After that, `GameListUpdate` events (229) carry only what changed; a room that disappeared is sent with `Removed` (251) set to true. The client keeps its own copy and drops a room only when told to.
- `GetGameList` (217) returns the same list on request.
- `CreateGame` (227), `JoinGame` (226) or `JoinRandomGame` (225). The response carries the room name (255), the game server address (230) and a new token (221). The master assigns a name when the client didn't give one.
- `ServerSettings` (218) needs only an Ok.

**3. Game server.** Connect, Init, InitEncryption, authenticate with the token, then repeat the create or join with its full parameters. When the client leaves the room it reconnects to the master with the last token it got, so the game server's auth response must hand back a token that leads to the master.

## Master behaviour

- Rooms are listed only in the lobby they were created in. Joining by name finds a room in any lobby.
- A create without a lobby, and a `JoinRandomGame` without one, use the lobby the client is currently in.
- `JoinRandomGame` picks an open, visible room that isn't full and whose properties equal every expected property sent (248), MaxPlayers included when the client asks for it.
- A room's lobby listing holds MaxPlayers (255 and 243), IsOpen (253), IsVisible (254), the live player count (252), and the custom properties the creator named in key 250.

## Game server behaviour

**Joining.** The response carries the joiner's actor number (254), the actor list (252, int[]), the room properties (248), the room option flags (191), and every player's properties (249), keyed by actor number as ints. Then a `Join` event (255) goes to everyone **including the joiner**; that event is what completes the join on the client's side. The joiner then gets the room's cached events. The first actor becomes the master client.

**Leaving.** `Leave` (254) gets a response before anything else; the client returns to the master once it has it. Everyone left gets a `Leave` event (254) with the actor number and the new actor list. If the master client left, the lowest remaining actor number takes over and the event carries it in 203, and only then: the client reports any 203 as a master switch. An empty room is deleted.

**Raising events.** `RaiseEvent` (253) carries the event code (244), data (245) and a target. Recipients are, in order of priority: explicit target actors (252); an interest group (240, where 0 means no group); or the receiver group (246: 0 others, 1 all, 2 master client). Each recipient gets the event with the sender's actor number (254) and the data (245). The caching option (247) is applied instead of sending when it is a removal:

| 247 | Effect |
|-----|--------|
| 4 | Add to the room cache, owned by the sender |
| 5 | Add to the room cache globally; it outlives the sender |
| 6 | Remove matching cached events: code 0 matches any code, target actors (252) select owners, and a data hashtable matches events whose data holds all of its entries |
| 7 | Remove cached events of actors who have left |

**Properties.** `SetProperties` (252) carries the properties (251), a target actor (254, absent or 0 for the room), optional expected values (231) for a check-and-swap, and whether to broadcast (250). With expected values, every one must still be current, a null meaning "not set", or the update fails with -2. With broadcast set, a `PropertiesChanged` event (253) goes out with the properties (251), the sender (254) and the target actor (253). The sender gets it too when it used check-and-swap or the room has `BroadcastPropsChangeToAll`: in those cases the client applies its own change only when the event comes back. `GetProperties` (251) returns the room (248) and player (249) properties.

**Interest groups.** `ChangeGroups` (248) takes byte arrays of groups to remove (239) and add (238). An absent array changes nothing; an empty one means every group, which for add means every group in use in the room.

**Anything else** gets `InvalidOperation` (-2) with a debug message. Clients wait for a response.

## Codes

**Room option flags** (191). Older clients send separate booleans instead: 241 CleanupCacheOnLeave (default true), 237 SuppressRoomEvents, 239 PublishUserId.

| Bit | Flag |
|-----|------|
| 0x02 | DeleteCacheOnLeave |
| 0x04 | SuppressRoomEvents |
| 0x08 | PublishUserId: add each player's UserId to their properties (key 253) |
| 0x10 | DeleteNullProps: setting a property to null removes it |
| 0x20 | BroadcastPropsChangeToAll |
| 0x40 | SuppressPlayerInfo: leave player properties out of join responses and events |

**Room property keys** (byte keys in the room's properties hashtable)

| Key | Property |
|-----|----------|
| 255 | MaxPlayers (byte) |
| 243 | MaxPlayers (int, newer clients, for rooms over 255) |
| 254 | IsVisible |
| 253 | IsOpen |
| 252 | PlayerCount |
| 251 | Removed (room lists only) |
| 250 | Custom property names to show in the lobby (string[]) |
| 248 | MasterClientId |

**Return codes**

| Code | Meaning |
|------|---------|
| 0 | Ok |
| -2 | InvalidOperation |
| -3 | NotAllowedInState |
| 32766 | GameIdAlreadyExists |
| 32765 | GameFull |
| 32764 | GameClosed |
| 32760 | NoRandomMatchFound |
| 32758 | GameDoesNotExist |

A join of a room that doesn't exist creates it when JoinMode (215) is 1, CreateIfNotExists.
