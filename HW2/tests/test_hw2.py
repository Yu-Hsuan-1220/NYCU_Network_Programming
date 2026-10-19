#!/usr/bin/env python3
"""End-to-end tests for HW2 (nplab_server_single / nplab_server_multi / nplab_client).

Usage:  python3 test_hw2.py <dir with the executables> [single|multi ...]
Every scenario runs against each server; client output and the server log are
compared with the spec text exactly. Not part of the submission.
"""
import os
import random
import re
import select
import socket
import subprocess
import sys
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
KINDS = sys.argv[2:] or ["single", "multi"]
MODE_CMDS = "Commands: solo [rounds], duo, help, quit\n"
ROOM_CMDS = "Commands: create <room ID> [rounds], join <room ID>, back, help, quit\n"
RETURNED = "Returned to mode selection.\n"

passed = failed = 0
failures = []


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        failures.append(name)
        print(f"  FAIL  {name}\n{detail}")


def eq(name, got, exp):
    check(name, got == exp, f"--- expected\n{exp!r}\n--- got\n{got!r}")


def score(ans, g):
    a = sum(x == y for x, y in zip(ans, g))
    return f"{a}A{sum(x in ans for x in g) - a}B"


def wrong(ans):
    while True:
        g = "".join(random.sample("0123456789", 4))
        if g != ans:
            return g


def counts(g, c):
    return f"Guess count: {g}, Correct count: {c}"


# --------------------------------------------------------------------------
class Server:
    def __init__(self, kind, port, binary_dir=BIN):
        self.kind, self.port = kind, port
        self.p = subprocess.Popen([os.path.join(binary_dir, f"nplab_server_{kind}"), str(port)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.fd = self.p.stdout.fileno()
        self.buf = b""
        self.lines = []
        self.answer_pos = {}
        self.cursor = 0
        self.wait_line("Waiting for a new client...")

    def _read_line(self, timeout):
        while b"\n" not in self.buf:
            r, _, _ = select.select([self.fd], [], [], timeout)
            chunk = os.read(self.fd, 65536) if r else b""
            if not chunk:
                raise TimeoutError("server log stalled; tail:\n" + "\n".join(self.lines[-15:]))
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        self.lines.append(raw.decode())
        return self.lines[-1]

    def wait_line(self, pattern, timeout=4):
        """Next line (after the previous match) fully matching `pattern`."""
        while True:
            for i in range(self.cursor, len(self.lines)):
                m = re.fullmatch(pattern, self.lines[i])
                if m:
                    self.cursor = i + 1
                    return m
            self._read_line(timeout)

    def drain(self, wait=0.3):
        end = time.time() + wait
        while time.time() < end:
            try:
                self._read_line(max(0.01, end - time.time()))
            except TimeoutError:
                break

    def answer(self, gid, timeout=4):
        """Next 'Answer for this round' of game gid (in order)."""
        pat = re.compile(rf"\[Game {gid}\] Answer for this round: (\d{{4}})\.")
        pos = self.answer_pos.get(gid, 0)
        end = time.time() + timeout
        while True:
            for i in range(pos, len(self.lines)):
                m = pat.fullmatch(self.lines[i])
                if m:
                    self.answer_pos[gid] = i + 1
                    return m.group(1)
            pos = len(self.lines)
            self._read_line(max(0.05, end - time.time()))

    def game_log(self, gid):
        p = f"[Game {gid}] "
        return [l[len(p):] for l in self.lines if l.startswith(p)]

    def plain_log(self):
        return [l for l in self.lines if not l.startswith("[Game ")]

    def gid_of_room(self, room):
        m = self.wait_line(rf"\[Game (\d+)\] Room {room} created by \w+\. Rounds: \d\.")
        return int(m.group(1))

    def stop(self):
        self.p.kill()
        self.p.wait()


class P:
    """A player talking to the server over a raw socket."""

    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.settimeout(0.05)
        self.buf = ""
        self.name = None
        self.closed = False

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def _recv(self):
        try:
            d = self.s.recv(65536)
            if not d:
                self.closed = True
            self.buf += d.decode()
        except socket.timeout:
            pass

    def read_until(self, token, timeout=4):
        end = time.time() + timeout
        while token not in self.buf:
            if time.time() > end or self.closed:
                raise TimeoutError(f"{self.name}: waiting for {token!r}, have {self.buf!r}")
            self._recv()
        i = self.buf.index(token) + len(token)
        out, self.buf = self.buf[:i], self.buf[i:]
        return out

    @property
    def prompt(self):
        return f"nplab({self.name})> "

    def cmd(self, line, timeout=4):
        """Send a command; return the reply without the trailing prompt."""
        self.send(line)
        return self.read_until(self.prompt, timeout)[: -len(self.prompt)]

    def event(self, timeout=4):
        """An asynchronous batch: starts with '\\n', ends with the prompt."""
        return self.read_until(self.prompt, timeout)[: -len(self.prompt)]

    def login(self, name):
        self.read_until("Enter your name: ")
        self.name = name
        return self.cmd(name)

    def until_closed(self, timeout=4):
        end = time.time() + timeout
        while not self.closed and time.time() < end:
            self._recv()
        out, self.buf = self.buf, ""
        return out

    def silent(self, wait=0.3):
        end = time.time() + wait
        while time.time() < end:
            self._recv()
        return self.buf == ""

    def close(self):
        self.s.close()


def run_client(port, data, host="127.0.0.1", timeout=6):
    r = subprocess.run([os.path.join(BIN, "nplab_client"), host, str(port)], input=data.encode(),
                       capture_output=True, timeout=timeout)
    return r.returncode, r.stdout.decode()


# ============================================================ test cases
def t_usage(kind, _srv, _port):
    for args in ([], ["8888abc"], ["1023"], ["65536"], ["1453", "x"], ["-1"], [""]):
        r = subprocess.run([f"./nplab_server_{kind}"] + args, cwd=BIN, capture_output=True, text=True, timeout=3)
        eq(f"server usage {args}", (r.stdout, r.returncode != 0),
           (f"Usage: ./nplab_server_{kind} <port number 1024-65535>\n", True))
    if kind == KINDS[0]:
        for args in ([], ["127.0.0.1"], ["127.0.0.1", "8888abc"], ["127.0.0.1", "80"], ["a", "1453", "x"]):
            r = subprocess.run(["./nplab_client"] + args, cwd=BIN, capture_output=True, text=True, timeout=3)
            eq(f"client usage {args}", (r.stdout, r.returncode != 0),
               ("Usage: ./nplab_client <server IP> <port number 1024-65535>\n", True))


def t_login_and_commands(kind, srv, port):
    a = P(port)
    srv.wait_line("Client connected.")
    a.read_until("Enter your name: ")
    got = ""
    for bad in ["", "Alice Bob", "Alice@home", "1Alice", "_x", "A" * 21]:
        a.send(bad)
        got += a.read_until("Enter your name: ")
    eq("invalid names", got, "Invalid player name.\nEnter your name: " * 6)
    a.name = "Alice"
    eq("welcome", a.cmd("Alice"), "Welcome, Alice!\n" + MODE_CMDS)
    srv.wait_line("Player name: Alice")

    # examples from spec 3.4, at mode selection
    cases = [("foo bar", "Unknown command: foo\n"), ("GUESS 1234", "Unknown command: GUESS\n"),
             ("guess 1123", "Usage: guess <4 distinct digits>\n"), ("solo 0", "Usage: solo [rounds]\n"),
             ("create 0123", "Usage: create <room ID> [rounds]\n"),
             ("create 1234 10", "Usage: create <room ID> [rounds]\n"), ("join 10000", "Usage: join <room ID>\n"),
             ("status abc", "Usage: status\n"), ("quit abc", "Usage: quit\n"), ("help abc", "Usage: help\n"),
             ("duo x", "Usage: duo\n"), ("back x", "Usage: back\n"), ("solo 10", "Usage: solo [rounds]\n"),
             ("solo 05", "Usage: solo [rounds]\n"), ("solo 3 3", "Usage: solo [rounds]\n"),
             ("join", "Usage: join <room ID>\n"), ("create", "Usage: create <room ID> [rounds]\n"),
             ("guess", "Usage: guess <4 distinct digits>\n"), ("guess 1234 5678", "Usage: guess <4 distinct digits>\n"),
             # format OK but not available at mode selection
             ("guess 1234", "Command not available now.\n"), ("status", "Command not available now.\n"),
             ("create 12", "Command not available now.\n"), ("join 12", "Command not available now.\n"),
             ("back", "Command not available now.\n"),
             ("   ", ""), ("", "")]
    for line, exp in cases:
        eq(f"mode: {line!r}", a.cmd(line), exp)

    eq("help (mode)", a.cmd("help"),
       "Available commands:\n  solo [rounds]\n  duo\n  help\n  quit\nRounds: 1-9, default 3.\n")
    eq("duo", a.cmd("duo"), "Two-player mode.\n" + ROOM_CMDS)
    eq("help (room selection)", a.cmd("help"),
       "Available commands:\n  create <room ID> [rounds]\n  join <room ID>\n  back\n  help\n  quit\n"
       "Room ID: 1-9999. Rounds: 1-9, default 3.\n")
    for line in ["duo", "solo", "status", "guess 1234"]:
        eq(f"room selection: {line}", a.cmd(line), "Command not available now.\n")
    eq("join missing room", a.cmd("join 4321"), "Room not found.\n")
    eq("back", a.cmd("back"), RETURNED + MODE_CMDS)
    a.send("quit")
    eq("quit at mode selection", a.until_closed(), "Goodbye, Alice!\n")
    srv.wait_line("Alice left.")


def t_solo(kind, srv, port):
    c = P(port)
    srv.wait_line("Client connected.")
    c.login("Carol")
    eq("solo start", c.cmd("solo"), "Single-player game started. Rounds: 3.\nRound 1/3.\n")
    gid = int(srv.wait_line(r"\[Game (\d+)\] Single-player game started: Carol\. Rounds: 3\.").group(1))
    a1 = srv.answer(gid)
    w = wrong(a1)
    eq("solo wrong guess", c.cmd(f"guess {w}"), score(a1, w) + "\n")
    eq("solo status", c.cmd("status"), "Round: 1/3\n" + counts(1, 0) + "\n")
    eq("solo help", c.cmd("help"), "Available commands:\n  guess <4 distinct digits>\n  status\n  help\n  quit\n")
    for line in ["solo", "duo", "back", "create 1", "join 1"]:
        eq(f"solo: {line} not available", c.cmd(line), "Command not available now.\n")
    eq("solo invalid guess not counted", c.cmd("guess 1123"), "Usage: guess <4 distinct digits>\n")
    eq("solo round 1 solved", c.cmd(f"guess {a1}"), "4A0B\nRound 2/3.\n")
    a2 = srv.answer(gid)
    eq("solo round 2 solved", c.cmd(f"guess {a2}"), "4A0B\nRound 3/3.\n")
    a3 = srv.answer(gid)
    eq("solo finished", c.cmd(f"guess {a3}"),
       "4A0B\n" + counts(4, 3) + "\n" + RETURNED + MODE_CMDS)
    srv.drain()
    eq("solo server log", srv.game_log(gid), [
        "Single-player game started: Carol. Rounds: 3.", "Round 1/3.", f"Answer for this round: {a1}.",
        f"Carol guessed {w}: {score(a1, w)}.", f"Carol guessed {a1}: 4A0B.", "Round 2/3.",
        f"Answer for this round: {a2}.", f"Carol guessed {a2}: 4A0B.", "Round 3/3.",
        f"Answer for this round: {a3}.", f"Carol guessed {a3}: 4A0B.", "Game over.",
        "Carol final status: Guess count: 4, Correct count: 3.", "Carol returned to mode selection."])

    # a new game resets the counts; solo 5; quit in the middle
    eq("solo 5", c.cmd("solo 5"), "Single-player game started. Rounds: 5.\nRound 1/5.\n")
    gid2 = int(srv.wait_line(r"\[Game (\d+)\] Single-player game started: Carol\. Rounds: 5\.").group(1))
    check("game id increments", gid2 == gid + 1, f"{gid} -> {gid2}")
    b1 = srv.answer(gid2)
    c.cmd(f"guess {wrong(b1)}")
    c.cmd(f"guess {b1}")
    eq("solo status resets per game", c.cmd("status"), "Round: 2/5\n" + counts(2, 1) + "\n")
    c.send("quit")
    eq("solo quit", c.until_closed(), counts(2, 1) + "\nGoodbye, Carol!\n")
    srv.wait_line(rf"\[Game {gid2}\] Carol left\.")
    log = srv.game_log(gid2)
    eq("solo quit log tail", log[-2:], ["Carol final status: Guess count: 2, Correct count: 1.", "Carol left."])


def t_duel_spec(kind, srv, port):
    """The two-player scenario of spec 2.3 / 2.4 (incl. one 8-second timeout)."""
    a, b = P(port), P(port)
    a.login("Alice")
    b.login("Bob")
    a.cmd("duo")
    eq("create", a.cmd("create 1234"), "Room 1234 created. Rounds: 3.\nWaiting for another player...\n")
    gid = srv.gid_of_room(1234)
    eq("waiting status", a.cmd("status"),
       "Room: 1234\nRounds: 3\nStatus: Waiting for another player.\n" + counts(0, 0) + "\n")
    eq("waiting help", a.cmd("help"), "Available commands:\n  status\n  help\n  quit\n")
    eq("waiting guess", a.cmd("guess 1234"), "Waiting for another player. You cannot guess yet.\n")
    eq("waiting bad guess", a.cmd("guess 11"), "Usage: guess <4 distinct digits>\n")
    for line in ["solo", "duo", "back", "create 5", "join 5"]:
        eq(f"waiting: {line} not available", a.cmd(line), "Command not available now.\n")

    b.cmd("duo")
    start = "Room 1234: Alice vs Bob. Rounds: 3.\nRound 1/3.\nTurn 1.\n"
    eq("join", b.cmd("join 1234"), "Joined room 1234.\n" + start)
    eq("host gets start event (new line first)", a.event(), "\n" + start)
    a1 = srv.answer(gid)

    # round 1, turn 1: both wrong
    w1, w2 = wrong(a1), wrong(a1)
    eq("first submit waits", a.cmd(f"guess {w1}"), "Waiting for the other's answer...\n")
    eq("duel status before reveal", a.cmd("status"),
       "Room: 1234\nRound: 1/3, Turn: 1\nScore: Alice 0, Bob 0\nAlice: " + counts(0, 0) +
       "\nBob: " + counts(0, 0) + "\nWaiting for: Bob\n")
    eq("duplicate submit", a.cmd(f"guess {w2}"), "You have already guessed this turn.\n")
    eq("duel help", a.cmd("help"), "Available commands:\n  guess <4 distinct digits>\n  status\n  help\n  quit\n")
    r1 = f"Alice guessed {w1}: {score(a1, w1)}.\nBob guessed {w2}: {score(a1, w2)}.\nTurn 2.\n"
    eq("second submit reveals", b.cmd(f"guess {w2}"), r1)
    eq("reveal reaches first player", a.event(), "\n" + r1)

    # turn 2: Bob solves round 1
    w3 = wrong(a1)
    a.cmd(f"guess {w3}")
    r2 = (f"Alice guessed {w3}: {score(a1, w3)}.\nBob guessed {a1}: 4A0B.\nRound winner: Bob.\n"
          "Score: Alice 0, Bob 1.\nRound 2/3.\nTurn 1.\n")
    eq("round 1 won by Bob", b.cmd(f"guess {a1}"), r2)
    eq("round 1 event", a.event(), "\n" + r2)
    a2 = srv.answer(gid)
    eq("status both pending", b.cmd("status"),
       "Room: 1234\nRound: 2/3, Turn: 1\nScore: Alice 0, Bob 1\nAlice: " + counts(2, 0) +
       "\nBob: " + counts(2, 1) + "\nWaiting for: Alice, Bob\n")

    # round 2: Alice solves, Bob times out after 8 s
    t0 = time.time()
    a.cmd(f"guess {a2}")
    r3 = (f"Alice guessed {a2}: 4A0B.\nBob timed out.\nRound winner: Alice.\nScore: Alice 1, Bob 1.\n"
          "Round 3/3.\nTurn 1.\n")
    eq("timeout reveal (Alice)", a.event(timeout=12), "\n" + r3)
    took = time.time() - t0
    eq("timeout reveal (Bob)", b.event(), "\n" + r3)
    check(f"deadline ~8s (took {took:.1f}s)", 6.5 < took < 9.5)
    a3 = srv.answer(gid)

    # round 3: Bob first, so Alice sees results immediately
    eq("Bob submits first", b.cmd(f"guess {a3}"), "Waiting for the other's answer...\n")
    w4 = wrong(a3)
    r4 = (f"Alice guessed {w4}: {score(a3, w4)}.\nBob guessed {a3}: 4A0B.\nRound winner: Bob.\n"
          "Score: Alice 1, Bob 2.\nGame over.\nWinner: Bob.\n")
    eq("game over (Alice)", a.cmd(f"guess {w4}"), r4 + counts(4, 1) + "\n" + RETURNED + MODE_CMDS)
    eq("game over (Bob)", b.event(), "\n" + r4 + counts(3, 2) + "\n" + RETURNED + MODE_CMDS)
    srv.wait_line(rf"\[Game {gid}\] Room 1234 closed\.")
    eq("duel server log (spec 2.4)", srv.game_log(gid), [
        "Room 1234 created by Alice. Rounds: 3.", "Waiting for another player.", "Bob joined room 1234.",
        "Game started: Alice vs Bob.", "Round 1/3.", f"Answer for this round: {a1}.", "Turn 1.",
        f"Alice guessed {w1}: {score(a1, w1)}.", f"Bob guessed {w2}: {score(a1, w2)}.", "Turn 2.",
        f"Alice guessed {w3}: {score(a1, w3)}.", f"Bob guessed {a1}: 4A0B.", "Round winner: Bob.",
        "Score: Alice 0, Bob 1.", "Round 2/3.", f"Answer for this round: {a2}.", "Turn 1.",
        f"Alice guessed {a2}: 4A0B.", "Bob timed out.", "Round winner: Alice.", "Score: Alice 1, Bob 1.",
        "Round 3/3.", f"Answer for this round: {a3}.", "Turn 1.", f"Alice guessed {w4}: {score(a3, w4)}.",
        f"Bob guessed {a3}: 4A0B.", "Round winner: Bob.", "Score: Alice 1, Bob 2.", "Game over.",
        "Winner: Bob.", "Alice final status: Guess count: 4, Correct count: 1.",
        "Bob final status: Guess count: 3, Correct count: 2.", "Alice returned to mode selection.",
        "Bob returned to mode selection.", "Room 1234 closed."])

    # both keep their connection and can play again; room ID reusable
    eq("back at mode selection", a.cmd("help").splitlines()[1], "  solo [rounds]")
    a.cmd("duo")
    eq("room id reusable", a.cmd("create 1234 1"), "Room 1234 created. Rounds: 1.\nWaiting for another player...\n")
    gid2 = srv.gid_of_room(1234)
    a.send("quit")
    eq("waiting host quit", a.until_closed(), "Goodbye, Alice!\n")
    srv.wait_line(rf"\[Game {gid2}\] Room 1234 closed\.")
    eq("waiting quit log", srv.game_log(gid2)[-2:], ["Alice left.", "Room 1234 closed."])
    b.send("quit")
    b.until_closed()
    srv.wait_line("Bob left.")


def t_room_errors(kind, srv, port):
    a, b, c, d = P(port), P(port), P(port), P(port)
    a.login("Ann")
    b.login("Ann")
    c.login("Cid")
    d.login("Dee")
    for p in (a, b, c, d):
        p.cmd("duo")
    a.cmd("create 77 2")
    gid = srv.gid_of_room(77)
    eq("room exists", c.cmd("create 77"), "Room already exists.\n")
    eq("same name", b.cmd("join 77"), "Player name already used in this room.\n")
    eq("join ok", c.cmd("join 77"), "Joined room 77.\nRoom 77: Ann vs Cid. Rounds: 2.\nRound 1/2.\nTurn 1.\n")
    a.event()
    eq("room full", d.cmd("join 77"), "Room is full.\n")
    eq("full before same name", b.cmd("join 77"), "Room is full.\n")
    eq("not found", d.cmd("join 78"), "Room not found.\n")
    eq("still in room selection", d.cmd("back"), RETURNED + MODE_CMDS)
    ans = srv.answer(gid)
    # Ann quits during the game after one revealed turn
    a.cmd(f"guess {wrong(ans)}")
    c.cmd(f"guess {wrong(ans)}")
    a.event()
    a.send("quit")
    settle = ("Game over.\nWinner: Cid.\nScore: Ann 0, Cid 0.\nAnn: " + counts(1, 0) +
              "\nCid: " + counts(1, 0) + "\n")
    eq("quitter gets summary + goodbye", a.until_closed(), settle + "Goodbye, Ann!\n")
    eq("other player wins, returns", c.event(), "\n" + settle + RETURNED + MODE_CMDS)
    srv.wait_line(rf"\[Game {gid}\] Room 77 closed\.")
    log = srv.game_log(gid)
    eq("leave log", log[log.index("Ann left."):], [
        "Ann left.", "Game over.", "Winner: Cid.", "Score: Ann 0, Cid 0.",
        "Ann final status: Guess count: 1, Correct count: 0.",
        "Cid final status: Guess count: 1, Correct count: 0.", "Cid returned to mode selection.",
        "Room 77 closed."])
    for p in (b, c, d):
        p.close()
    srv.drain(0.5)


def t_unannounced_guess_dropped(kind, srv, port):
    """Pending guess is not counted when the opponent's client is killed."""
    a, b = P(port), P(port)
    a.login("Eve")
    b.login("Fay")
    a.cmd("duo")
    b.cmd("duo")
    a.cmd("create 9999")
    gid = srv.gid_of_room(9999)
    b.cmd("join 9999")
    a.event()
    ans = srv.answer(gid)
    a.cmd(f"guess {ans}")          # pending, never revealed
    b.close()                      # Fay's client dies
    settle = ("Game over.\nWinner: Eve.\nScore: Eve 0, Fay 0.\nEve: " + counts(0, 0) +
              "\nFay: " + counts(0, 0) + "\n")
    eq("killed opponent -> win, pending guess dropped", a.event(), "\n" + settle + RETURNED + MODE_CMDS)
    srv.wait_line(rf"\[Game {gid}\] Room 9999 closed\.")
    eq("killed client log", srv.game_log(gid)[-8:], [
        "Fay left.", "Game over.", "Winner: Eve.", "Score: Eve 0, Fay 0.",
        "Eve final status: Guess count: 0, Correct count: 0.",
        "Fay final status: Guess count: 0, Correct count: 0.", "Eve returned to mode selection.",
        "Room 9999 closed."])
    eq("survivor can play on", a.cmd("solo 1"), "Single-player game started. Rounds: 1.\nRound 1/1.\n")
    a.close()
    srv.drain(0.5)


def t_host_killed(kind, srv, port):
    a, b = P(port), P(port)
    a.login("Gus")
    b.login("Hal")
    a.cmd("duo")
    b.cmd("duo")
    a.cmd("create 5")
    gid = srv.gid_of_room(5)
    b.cmd("join 5")
    a.event()
    srv.answer(gid)
    a.close()  # the room host's client dies
    settle = ("Game over.\nWinner: Hal.\nScore: Gus 0, Hal 0.\nGus: " + counts(0, 0) +
              "\nHal: " + counts(0, 0) + "\n")
    eq("host killed -> joiner wins", b.event(), "\n" + settle + RETURNED + MODE_CMDS)
    srv.wait_line(rf"\[Game {gid}\] Room 5 closed\.")
    b.cmd("duo")
    eq("room freed after host left", b.cmd("create 5"), "Room 5 created. Rounds: 3.\nWaiting for another player...\n")
    b.close()
    srv.wait_line(r"\[Game \d+\] Room 5 closed\.")


def t_draw_and_tiebreak(kind, srv, port):
    # 1 round, both solve in the same turn with one guess each -> Draw
    a, b = P(port), P(port)
    a.login("Ivy")
    b.login("Jon")
    a.cmd("duo")
    b.cmd("duo")
    a.cmd("create 31 1")
    gid = srv.gid_of_room(31)
    b.cmd("join 31")
    a.event()
    ans = srv.answer(gid)
    a.cmd(f"guess {ans}")
    end = (f"Ivy guessed {ans}: 4A0B.\nJon guessed {ans}: 4A0B.\nRound winner: Ivy, Jon.\n"
           "Score: Ivy 1, Jon 1.\nGame over.\nWinner: Draw.\n")
    eq("draw", b.cmd(f"guess {ans}"), end + counts(1, 1) + "\n" + RETURNED + MODE_CMDS)
    a.event()
    srv.wait_line(rf"\[Game {gid}\] Room 31 closed\.")

    # same score, Ivy has more guesses -> Ivy wins; both time out once
    a.cmd("duo")
    b.cmd("duo")
    a.cmd("create 32 1")
    gid = srv.gid_of_room(32)
    b.cmd("join 32")
    a.event()
    ans = srv.answer(gid)
    both_out = "Ivy timed out.\nJon timed out.\nTurn 2.\n"
    eq("both time out -> next turn (Ivy)", a.event(timeout=12), "\n" + both_out)
    eq("both time out -> next turn (Jon)", b.event(), "\n" + both_out)
    w = wrong(ans)
    a.cmd(f"guess {w}")
    eq("Jon times out in turn 2", a.event(timeout=12), f"\nIvy guessed {w}: {score(ans, w)}.\nJon timed out.\nTurn 3.\n")
    b.event()
    a.cmd(f"guess {ans}")
    end = (f"Ivy guessed {ans}: 4A0B.\nJon guessed {ans}: 4A0B.\nRound winner: Ivy, Jon.\n"
           "Score: Ivy 1, Jon 1.\nGame over.\nWinner: Ivy.\n")
    eq("tie broken by guess count", b.cmd(f"guess {ans}"), end + counts(1, 1) + "\n" + RETURNED + MODE_CMDS)
    eq("winner's own stats", a.event(), "\n" + end + counts(2, 1) + "\n" + RETURNED + MODE_CMDS)
    srv.wait_line(rf"\[Game {gid}\] Room 32 closed\.")
    a.close()
    b.close()
    srv.drain(0.5)


def t_concurrency(kind, srv, port):
    """8 players, 4 duels at once; a silent client must not block anyone."""
    idle = P(port)  # connected but never logs in
    ps = [P(port) for _ in range(8)]
    for i, p in enumerate(ps):
        p.login(f"U{i}")
        p.cmd("duo")
    gids = {}
    for k in range(4):
        ps[2 * k].cmd(f"create {100 + k} 2")
        gids[k] = srv.gid_of_room(100 + k)
    for k in range(4):
        ps[2 * k + 1].cmd(f"join {100 + k}")
        ps[2 * k].event()
    check("4 rooms got distinct game ids", len(set(gids.values())) == 4, str(gids))
    answers = {k: srv.answer(gids[k]) for k in range(4)}
    t0 = time.time()
    for rnd in (1, 2):
        for k in range(4):  # interleave the four games
            ps[2 * k].cmd(f"guess {answers[k]}")
        for k in range(4):
            ps[2 * k + 1].cmd(f"guess {wrong(answers[k])}")
            ps[2 * k].event()
        if rnd == 1:
            answers = {k: srv.answer(gids[k]) for k in range(4)}
    check(f"4 parallel duels finished quickly ({time.time() - t0:.1f}s)", time.time() - t0 < 5)
    for k in range(4):
        srv.wait_line(rf"\[Game {gids[k]}\] Room {100 + k} closed\.")
        log = srv.game_log(gids[k])
        check(f"game {gids[k]} log complete", log[-3:-1] == [f"U{2*k} returned to mode selection.",
                                                            f"U{2*k+1} returned to mode selection."], str(log[-5:]))
    idle.close()
    srv.wait_line("Client disconnected before login.")
    for p in ps:
        p.close()
    srv.drain(0.5)


def t_stream_and_crlf(kind, srv, port):
    s = socket.create_connection(("127.0.0.1", port))
    s.sendall(b"Ka")
    time.sleep(0.2)
    s.sendall(b"te\r\nsolo 1\r\nhe")
    time.sleep(0.2)
    s.sendall(b"lp\nquit\n")
    s.settimeout(3)
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    exp = ("Enter your name: Welcome, Kate!\n" + MODE_CMDS + "nplab(Kate)> "
           "Single-player game started. Rounds: 1.\nRound 1/1.\nnplab(Kate)> "
           "Available commands:\n  guess <4 distinct digits>\n  status\n  help\n  quit\nnplab(Kate)> "
           + counts(0, 0) + "\nGoodbye, Kate!\n")
    eq("split / merged / CRLF input", data.decode(), exp)
    srv.wait_line(r"\[Game \d+\] Kate left\.")


def t_real_client(kind, srv, port):
    code, out = run_client(port, "Liz\nsolo 2\nstatus\nquit\n")
    eq("nplab_client scripted session", out,
       "Enter your name: Welcome, Liz!\n" + MODE_CMDS + "nplab(Liz)> Single-player game started. Rounds: 2.\n"
       "Round 1/2.\nnplab(Liz)> Round: 1/2\n" + counts(0, 0) + "\nnplab(Liz)> " + counts(0, 0) +
       "\nGoodbye, Liz!\n")
    srv.wait_line(r"\[Game \d+\] Liz left\.")
    code, out = run_client(port, "Max\nduo\n")
    eq("stdin EOF when logged in = quit", out,
       "Enter your name: Welcome, Max!\n" + MODE_CMDS + "nplab(Max)> Two-player mode.\n" + ROOM_CMDS +
       "nplab(Max)> Goodbye, Max!\n")
    srv.wait_line("Max left.")
    code, out = run_client(port, "Ned\nsolo\n")
    check("stdin EOF in solo = quit (summary + goodbye)", out.endswith(counts(0, 0) + "\nGoodbye, Ned!\n"), out)
    srv.wait_line(r"\[Game \d+\] Ned left\.")
    code, out = run_client(port, "")
    eq("stdin EOF before login ends client", (out, code), ("Enter your name: ", 0))
    srv.wait_line("Client disconnected before login.")
    code, out = run_client(port, "Oli\nquit\n", host="localhost")
    check("hostname works", out.endswith("Goodbye, Oli!\n"), out)
    srv.wait_line("Oli left.")


# ---------------------------------------------------------- architecture
def children_of(pid):
    kids = []
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open(f"/proc/{d}/stat") as f:
                    st = f.read().rsplit(")", 1)[1].split()
                if int(st[1]) == pid:
                    kids.append((int(d), st[0]))
            except OSError:
                pass
    return kids


def listening_ports(pid):
    """Ports a process listens on (via /proc/<pid>/fd + /proc/net/tcp)."""
    inodes = set()
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            l = os.readlink(f"/proc/{pid}/fd/{fd}")
            if l.startswith("socket:["):
                inodes.add(l[8:-1])
        except OSError:
            pass
    res = []
    with open("/proc/net/tcp") as f:
        for line in f.readlines()[1:]:
            p = line.split()
            if p[3] == "0A" and p[9] in inodes:
                ip, port = p[1].split(":")
                res.append((socket.inet_ntoa(bytes.fromhex(ip)[::-1]), int(port, 16)))
    return res


def fd_kinds(pid):
    kinds = []
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            kinds.append(os.readlink(f"/proc/{pid}/fd/{fd}").split(":")[0])
        except OSError:
            pass
    return kinds


def t_architecture(kind, srv, port, ipc_range=(20000, 20099)):
    pid = srv.p.pid
    a, b = P(port), P(port)
    a.login("Pam")
    b.login("Quin")
    time.sleep(0.2)
    kids = children_of(pid)
    if kind == "single":
        check("single: no child processes", kids == [], str(kids))
        check("single: one thread", len(os.listdir(f"/proc/{pid}/task")) == 1)
        a.close(); b.close()
        srv.drain(0.5)
        return
    check("multi: one child per client", len(kids) == 2, str(kids))
    check("multi: parent talks to children through pipes", fd_kinds(pid).count("pipe") >= 4, str(fd_kinds(pid)))
    a.cmd("duo")
    a.cmd("create 4242")
    gid = srv.gid_of_room(4242)
    time.sleep(0.2)
    host_child = [k for k, _ in children_of(pid) if listening_ports(k)]
    lp = listening_ports(host_child[0]) if host_child else []
    check(f"multi: room host child listens on 127.0.0.1 in {ipc_range}",
          len(lp) == 1 and lp[0][0] == "127.0.0.1" and ipc_range[0] <= lp[0][1] <= ipc_range[1], str(lp))
    check("multi: parent has no internal listener", listening_ports(pid) == [("0.0.0.0", port)],
          str(listening_ports(pid)))
    b.cmd("duo")
    b.cmd("join 4242")
    a.event()
    time.sleep(0.2)
    # the two children are connected directly over TCP on 127.0.0.1
    with open("/proc/net/tcp") as f:
        est = [l.split() for l in f.readlines()[1:] if l.split()[3] == "01"]
    iport = lp[0][1] if lp else -1
    direct = [e for e in est if e[1].endswith(f":{iport:04X}") and e[1].startswith("0100007F")]
    check("multi: children connected by TCP", len(direct) == 1, str(direct))
    check("multi: both children alive during duel", len(children_of(pid)) == 2)
    a.send("quit")
    a.until_closed()
    b.event()
    srv.wait_line(rf"\[Game {gid}\] Room 4242 closed\.")
    time.sleep(0.5)
    kids = children_of(pid)
    check("multi: finished child reaped (no zombie)", len(kids) == 1 and kids[0][1] != "Z", str(kids))
    check("multi: internal port released", all(listening_ports(k) == [] for k, _ in kids))
    b.close()
    time.sleep(0.5)
    check("multi: all children reaped", children_of(pid) == [], str(children_of(pid)))


def t_cppflags():
    """make CPPFLAGS=... must move the internal port range."""
    import shutil, tempfile
    d = tempfile.mkdtemp()
    for f in os.listdir(os.path.join(BIN)):
        if f.endswith((".cpp", ".h")) or f == "Makefile":
            shutil.copy(os.path.join(BIN, f), d)
    r = subprocess.run(["make", 'CPPFLAGS=-DIPC_PORT_MIN=20300 -DIPC_PORT_MAX=20399'], cwd=d,
                       capture_output=True, text=True)
    check("make with CPPFLAGS", r.returncode == 0 and "-DIPC_PORT_MIN=20300" in r.stdout, r.stdout + r.stderr)
    port = random.randint(30000, 60000)
    srv = Server("multi", port, d)
    try:
        t_architecture("multi", srv, port, (20300, 20399))
    finally:
        srv.stop()
    r = subprocess.run(["make", "clean"], cwd=d, capture_output=True, text=True)
    left = sorted(os.listdir(d))
    check("make clean keeps only sources", left == sorted(
        ["Makefile", "nplab_client.cpp", "nplab_common.h", "nplab_server_multi.cpp", "nplab_server_single.cpp"]),
        str(left))
    shutil.rmtree(d)


# ===================================================================== main
TESTS = [t_login_and_commands, t_solo, t_duel_spec, t_room_errors, t_unannounced_guess_dropped,
         t_host_killed, t_draw_and_tiebreak, t_concurrency, t_stream_and_crlf, t_real_client, t_architecture]


def main():
    for kind in KINDS:
        print(f"\n######## {kind}")
        port = random.randint(30000, 60000)
        print("[usage]")
        t_usage(kind, None, port)
        srv = Server(kind, port)
        eq("startup log", srv.lines, [f"Server listening on port {port}.", "Waiting for a new client..."])
        try:
            for t in TESTS:
                print(f"[{t.__name__}]")
                try:
                    t(kind, srv, port)
                except Exception as e:
                    check(t.__name__, False, f"    {type(e).__name__}: {e}")
                    srv.stop()
                    port = random.randint(30000, 60000)
                    srv = Server(kind, port)
            check("'Waiting for a new client...' printed once",
                  srv.lines.count("Waiting for a new client...") == 1)
            check("server still running", srv.p.poll() is None)
        finally:
            srv.stop()
    if "multi" in KINDS:
        print("\n[t_cppflags]")
        t_cppflags()
    print(f"\n{passed} passed, {failed} failed")
    for f in failures:
        print("  failed:", f)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
