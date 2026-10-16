# HW1 — 1A2B over TCP

A client-server version of the 1A2B number guessing game, built on TCP sockets.
The full assignment is in [spec/NP_Project 1_Spec.pdf](spec/NP_Project%201_Spec.pdf).

## Layout

```
HW1/
├── spec/NP_Project 1_Spec.pdf   assignment spec
├── src/
│   ├── Makefile
│   ├── nplab_server.cpp         iterative TCP server
│   └── nplab_client.cpp         TCP client
└── tests/test_nplab.py          end-to-end tests (not part of the submission)
```

## Build and run

Requires Linux and g++ (C++17). From `src/`:

```bash
make
./nplab_server 1453               # port must be 1024–65535
./nplab_client 127.0.0.1 1453     # a hostname such as localhost also works
```

```
Enter your name: Alice
Welcome, Alice!
nplab(Alice)> guess 6458
1A2B
nplab(Alice)> status
Guess count: 1, Correct count: 0
nplab(Alice)> quit
Guess count: 1, Correct count: 0
Goodbye, Alice!
```

Commands: `guess <4-digit number>`, `status`, `quit`. The game ends after three `4A0B` results or on `quit`.

## Design

### Server: iterative TCP server

```
socket → setsockopt(SO_REUSEADDR) → bind(INADDR_ANY) → listen
loop:  accept → serve one player → close → back to accept
```

- **One player at a time.** While a game is in progress, new connections wait in the `listen()` backlog queue and are `accept()`ed after the current player leaves.
- **Stateful.** For the length of a connection the server keeps the player's name, the current answer, the guess count and the correct count. All of it is discarded when the connection closes.
- **No message boundaries in TCP.** `LineReader` buffers whatever `recv()` returns and hands out one line only when it reaches `\n`. Lines split across reads, several lines in one read, and `\r\n` endings are all handled.
- **Survives abrupt client disconnects.** `SIGPIPE` is ignored and writes use `send(..., MSG_NOSIGNAL)`. When `recv()` returns 0 or -1 the server prints `<name> left.` and waits for the next client.
- **Output appears immediately.** `setvbuf(stdout, nullptr, _IOLBF, 0)` keeps stdout line-buffered even when it goes to a pipe or a file.
- **`SO_REUSEADDR`** lets the server restart right away instead of waiting for TIME_WAIT to expire.

### Client: TCP client with `select()`

```
getaddrinfo → socket → connect → select loop → close
```

- The server sends all text the player sees, including the prompts `Enter your name: ` and `nplab(<name>)> `. The client only relays: keyboard → server, server → screen.
- `select()` waits on stdin and the socket together, so the client exits as soon as the server closes the connection.
- I/O uses the `read()` / `write()` system calls rather than stdio, so no data can sit in a user-space buffer where `select()` cannot see it.
- When stdin reaches EOF (Ctrl+D, or input from a file or pipe), the client half-closes with `shutdown(SHUT_WR)` and still receives the server's final reply.

### Protocol

Plain text, one line per `\n`. The client sends each line the user types; the server replies with the result followed by the next prompt.

### Behavior not defined by the spec

- An empty line just shows the prompt again.
- An unknown command reports only its first word: `foo bar` → `Unknown command: foo`.
- Extra words after `status` or `quit` are ignored.
- If a client disconnects before entering a name, the server prints no `left.` line and goes back to waiting.

## Tests

[tests/test_nplab.py](tests/test_nplab.py) starts a real server and real clients and compares their output character by character. After building:

```bash
python3 tests/test_nplab.py src
```

Coverage:

- usage messages for bad arguments
- the full example session from the spec
- invalid player names and commands
- the game ending after three correct guesses
- the server surviving a killed client, a reset connection, and stdin EOF, then serving the next client
- a second player waiting in the queue
- TCP data split mid-line or merged across lines
- connecting by hostname
- A/B scoring
- immediate restart thanks to `SO_REUSEADDR`

All 30 checks pass on linux2.cs.nycu.edu.tw (Debian 12, g++ 12.2).
