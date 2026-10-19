# HW2 — Multiplayer 1A2B: single-process and multi-process servers

Extends HW1 into a game server that hosts many players and games at once: single-player
games and two-player matches in rooms. The same game is implemented twice, once in a
single-process `select()` server and once in a multi-process server, and one client
works with both. The full assignment is in [spec/NP_Lab2_Spec.pdf](spec/NP_Lab2_Spec.pdf).

## Layout

```
HW2/
├── spec/NP_Lab2_Spec.pdf
├── src/
│   ├── Makefile
│   ├── nplab_common.h            rules, messages, Solo / Duel / Session (shared)
│   ├── nplab_server_single.cpp   one process, select()
│   ├── nplab_server_multi.cpp    parent + one child per client, pipes + TCP
│   └── nplab_client.cpp          client for both servers
└── tests/test_hw2.py             end-to-end tests (not part of the submission)
```

## Build and run

```bash
cd src
make                                                   # internal ports 20000-20099
make CPPFLAGS="-DIPC_PORT_MIN=20100 -DIPC_PORT_MAX=20199"   # another TA's range
./nplab_server_single 1453     # or ./nplab_server_multi 1453
./nplab_client 127.0.0.1 1453
make clean                     # removes executables and object files only
```

Modes and commands:

| State | Commands |
|---|---|
| Mode selection | `solo [rounds]`, `duo`, `help`, `quit` |
| Two-player room selection | `create <room ID> [rounds]`, `join <room ID>`, `back`, `help`, `quit` |
| Waiting for another player | `status`, `help`, `quit` |
| In a game | `guess <4 distinct digits>`, `status`, `help`, `quit` |

## Design

### Shared game logic (`nplab_common.h`)

Every player-visible message and server log line lives in one header, so the two servers
cannot drift apart.

- **`Session`**: one player's state machine (login → mode selection → room selection →
  waiting / solo / duel). It parses commands and checks them in the spec's order: unknown
  command → argument format (`Usage: ...`) → current state (`Command not available now.`)
  → room conditions. Each server subclasses it and implements only what depends on the
  architecture: game IDs, rooms, and how duel actions reach the game.
- **`Duel`**: one two-player game. It handles rounds and turns, hides guesses until both
  players submit or the 8-second deadline passes, scores, picks the winner (score, then
  guess count, then draw), and handles a player leaving. It only produces text through a
  callback, so it works the same whether its players are in the same process or not.
- **Prompt handling**: the server sends all text, including prompts. An event that arrives
  while a prompt is on screen starts on a new line and is followed by a fresh prompt.
  Output is flushed after every command, so piped input still gets one prompt per reply.

### `nplab_server_single`

No `fork`, no threads. A single `select()` loop watches the listening socket and every
client socket. **Turn deadlines use `select()`'s timeout**: before each call the server
computes the time left until the earliest deadline among all running duels. That gives
each room its own timer inside one process. Rooms and duels are ordinary objects in
memory.

### `nplab_server_multi`

```
                     parent  (accept, room table, Game IDs, reap children)
            two pipes |                      | two pipes
     client ── child (room host) ══ TCP ══ child (guest) ── client
                127.0.0.1:[IPC_PORT_MIN, IPC_PORT_MAX]
```

- Every client is served by its own child process for the whole session. The parent reaps
  finished children with `SIGCHLD` + `waitpid(WNOHANG)`.
- **Parent ↔ child: pipes only**, two per child because a pipe is one-way. The child sends
  a one-line request and blocks for the reply; the parent never starts a conversation:
  `GAMEID`, `CREATE`, `JOIN` (reserves the room), `JOINED` (reserved → playing),
  `JOINFAIL`, `CLOSE`.
- **Child ↔ child: TCP on 127.0.0.1.** The host child binds the first free port in the
  compile-time range and registers it with the parent. The joining child asks the parent
  for the port and connects (`HELLO` / `WELCOME` / `READY`). The host child owns the
  `Duel` (answer, turns, deadline, score). The guest child forwards `GUESS` / `STATUS` /
  `QUIT` and shows the host's `OUT` text; `DONE` and `END` complete its requests. The
  parent never relays game data.
- After a game the internal socket is closed and the room unregistered. Both children keep
  serving their players.
- After `fork()` each child closes the listening socket and every other child's pipes, and
  reseeds its random generator so rooms get different answers.

### Client

The client is a single-thread concurrent client: `select()` on stdin and the socket, so
game events show up while the player is typing. On stdin EOF it half-closes the socket
(`shutdown(SHUT_WR)`). The server treats this as `quit` for a logged-in player, sending
the summary and `Goodbye`, and simply closes the connection otherwise.

## Tests

[tests/test_hw2.py](tests/test_hw2.py) runs every scenario against **both** servers and
compares client output and server logs with the spec exactly. After building:

```bash
python3 tests/test_hw2.py src            # or: ... src single / ... src multi
```

Coverage:

- usage messages
- invalid names
- every error and `Usage` example in the spec, and `Command not available now.` in each state
- help text for every state
- a full single-player game, `solo 5`, and quitting mid-game
- the complete two-player scenario from spec 2.3/2.4, including a real 8-second timeout,
  status formats, duplicate submissions, and the server log line by line
- room errors (exists / not found / full / same name, with full checked before same name)
- quitting, or a killed client, during a duel (an unrevealed guess is not counted)
- a killed room host
- a draw, the guess-count tie-break, and both players timing out
- 8 players in 4 simultaneous duels while an idle client stays connected
- split, merged and CRLF input
- the real `nplab_client` with piped input and stdin EOF, before and after login
- process architecture:
  - single: no children and one thread
  - multi: one child per client, parent-child pipes, a host listener on 127.0.0.1 within
    the range, a direct child-child TCP connection, children reaped, ports released
- building with `CPPFLAGS`, and `make clean`

All 291 checks pass on linux2.cs.nycu.edu.tw (Debian 12, g++ 12.2), on 3 consecutive runs.
