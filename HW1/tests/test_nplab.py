#!/usr/bin/env python3
"""End-to-end tests for nplab_server / nplab_client.

Usage:  python3 test_nplab.py <dir containing the two executables>
Not part of the submission.
"""
import os
import random
import re
import socket
import subprocess
import sys
import time

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
SERVER = os.path.join(BIN, "nplab_server")
CLIENT = os.path.join(BIN, "nplab_client")
PORT = random.randint(20000, 60000)

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}\n{detail}")


def score(ans, g):
    a = sum(x == y for x, y in zip(ans, g))
    b = sum(x in ans for x in g) - a
    return f"{a}A{b}B"


def wrong_guess(ans):
    """A valid guess that is not the answer."""
    while True:
        g = "".join(random.sample("0123456789", 4))
        if g != ans:
            return g


class Server:
    def __init__(self, port):
        self.p = subprocess.Popen([SERVER, str(port)], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT)
        self.fd = self.p.stdout.fileno()
        self.buf = b""
        self.log = []
        self.expect(f"Server listening on port {port}.")
        self.expect("Waiting for a new client...")

    def readline(self, timeout=3):
        import select
        # raw os.read + own buffer: select() can't see data already sitting
        # in a Python file object's buffer
        while b"\n" not in self.buf:
            r, _, _ = select.select([self.fd], [], [], timeout)
            chunk = os.read(self.fd, 4096) if r else b""
            if not chunk:
                raise TimeoutError("server produced no output; log so far:\n" + "\n".join(self.log))
            self.buf += chunk
        raw, self.buf = self.buf.split(b"\n", 1)
        line = raw.decode()
        self.log.append(line)
        return line

    def expect(self, text):
        line = self.readline()
        if line != text:
            raise AssertionError(f"server: expected {text!r}, got {line!r}")
        return line

    def answer(self):
        m = re.fullmatch(r"Answer for this round: (\d{4})\.", self.readline())
        if not m:
            raise AssertionError(f"server: expected answer line, got {self.log[-1]!r}")
        return m.group(1)

    def stop(self):
        self.p.kill()
        self.p.wait()


class Client:
    """nplab_client driven through pipes."""

    def __init__(self, host="127.0.0.1", port=None):
        self.p = subprocess.Popen([CLIENT, host, str(port or PORT)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        os.set_blocking(self.p.stdout.fileno(), False)
        self.out = b""

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def read_until(self, text, timeout=3):
        end = time.time() + timeout
        while text.encode() not in self.out:
            if time.time() > end:
                raise TimeoutError(f"client: waiting for {text!r}, have {self.out.decode()!r}")
            try:
                chunk = self.p.stdout.read()
                if chunk:
                    self.out += chunk
            except BlockingIOError:
                pass
            time.sleep(0.01)
        return self.out.decode()

    def finish(self, timeout=3):
        try:
            rest, _ = self.p.communicate(timeout=timeout)
        except ValueError:  # stdin already closed
            self.p.wait(timeout)
            rest = b""
        self.out += rest or b""
        return self.out.decode()


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=3)
    return r.returncode, r.stdout + r.stderr


# ---------------------------------------------------------------------------
def test_usage():
    print("[usage]")
    u_srv = "Usage: ./nplab_server <port number 1024-65535>\n"
    u_cli = "Usage: ./nplab_client <server IP> <port number 1024-65535>\n"
    for args in ([], ["aaa"], ["80"], ["1023"], ["65536"], ["12a4"], ["1453", "x"], ["-1453"]):
        r = subprocess.run(["./nplab_server"] + args, cwd=BIN, capture_output=True, text=True, timeout=3)
        check(f"server {args}", r.stdout == u_srv and r.returncode != 0, repr(r.stdout))
    for args in ([], ["127.0.0.1"], ["127.0.0.1", "aaa"], ["127.0.0.1", "80"], ["a", "1453", "x"]):
        r = subprocess.run(["./nplab_client"] + args, cwd=BIN, capture_output=True, text=True, timeout=3)
        check(f"client {args}", r.stdout == u_cli and r.returncode != 0, repr(r.stdout))


def test_spec_scenario(srv):
    print("[spec scenario: names, commands, invalid input, quit]")
    c = Client()
    srv.expect("Client connected.")
    expected = ""
    p = "Enter your name: "
    for bad in ["", "Alice Bob", "Alice@home", "1Alice", "_Alice", "A" * 21, "Alicé"]:
        c.send(bad)
        expected += p + "Invalid player name.\n"
    c.send("Alice")
    expected += p + "Welcome, Alice!\n"
    srv.expect("Player name: Alice")
    ans = srv.answer()
    c.read_until("nplab(Alice)> ")
    pr = "nplab(Alice)> "
    expected += pr

    g = wrong_guess(ans)
    c.send(f"guess {g}")
    expected += score(ans, g) + "\n" + pr
    c.send("status")
    expected += "Guess count: 1, Correct count: 0\n" + pr

    for bad in ["foo", "guess", "guess 1234 5678", "guess 12a4", "guess 123",
                "guess 1123", "guess 12345", "Guess 1234", "STATUS"]:
        c.send(bad)
        if bad.split()[0] in ("guess",):
            expected += "Usage: guess <4-digit number>\n" + pr
        else:
            expected += f"Unknown command: {bad.split()[0]}\n" + pr

    c.send("status")
    expected += "Guess count: 1, Correct count: 0\n" + pr
    c.send(f"guess {ans}")
    expected += "4A0B\n" + pr
    c.read_until("4A0B")
    srv.expect(f"Alice guessed {g}.")
    srv.expect(f"Alice guessed {ans}.")
    ans2 = srv.answer()
    c.send(f"guess {ans2}")
    expected += "4A0B\n" + pr
    c.send("quit")
    expected += "Guess count: 3, Correct count: 2\nGoodbye, Alice!\n"
    out = c.finish()
    check("client output matches spec exactly", out == expected,
          f"--- expected\n{expected}\n--- got\n{out}")
    check("client exited after quit", c.p.returncode == 0)
    srv.expect(f"Alice guessed {ans2}.")
    srv.answer()
    srv.expect("Alice left.")
    srv.expect("Waiting for a new client...")
    check("server log for scenario", True)


def test_three_rounds(srv):
    print("[three 4A0B ends the game]")
    c = Client()
    srv.expect("Client connected.")
    c.send("Bob_2")
    srv.expect("Player name: Bob_2")
    for i in range(3):
        ans = srv.answer()
        c.send(f"guess {ans}")
        srv.expect(f"Bob_2 guessed {ans}.")
    srv.expect("Bob_2 left.")
    srv.expect("Waiting for a new client...")
    out = c.finish()
    check("ends with status + Goodbye",
          out.endswith("4A0B\nGuess count: 3, Correct count: 3\nGoodbye, Bob_2!\n"), out)
    check("client exited by itself", c.p.returncode == 0)


def test_abrupt_disconnect(srv):
    print("[client killed mid-game / before name]")
    c = Client()
    srv.expect("Client connected.")
    c.p.kill(); c.p.wait()
    srv.expect("Waiting for a new client...")
    check("disconnect before name -> server waits again", True)

    c = Client()
    srv.expect("Client connected.")
    c.send("Carol")
    srv.expect("Player name: Carol")
    srv.answer()
    c.read_until("nplab(Carol)> ")
    c.p.kill(); c.p.wait()
    srv.expect("Carol left.")
    srv.expect("Waiting for a new client...")
    check("killed client -> '<name> left.' and server survives", srv.p.poll() is None)

    # RST instead of FIN
    s = socket.create_connection(("127.0.0.1", PORT))
    srv.expect("Client connected.")
    s.sendall(b"Dave\n")
    srv.expect("Player name: Dave")
    srv.answer()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
    s.close()
    srv.expect("Dave left.")
    srv.expect("Waiting for a new client...")
    check("connection reset -> server survives", srv.p.poll() is None)


def test_stdin_eof(srv):
    print("[client stdin EOF (Ctrl+D)]")
    c = Client()
    srv.expect("Client connected.")
    c.send("Eve")
    srv.expect("Player name: Eve")
    srv.answer()
    c.p.stdin.close()
    srv.expect("Eve left.")
    srv.expect("Waiting for a new client...")
    c.finish()
    check("EOF ends the session cleanly", c.p.returncode == 0)


def test_queue(srv):
    print("[second player waits for the first]")
    c1 = Client()
    srv.expect("Client connected.")
    c1.send("First")
    srv.expect("Player name: First")
    srv.answer()
    c1.read_until("nplab(First)> ")

    c2 = Client()
    c2.send("Second")
    time.sleep(0.5)
    try:
        c2.p.stdout.read()
    except BlockingIOError:
        pass
    got = c2.out + (c2.p.stdout.read() or b"")
    check("second client gets nothing while first plays", got == b"", repr(got))
    c1.send("status")
    c1.read_until("Guess count: 0")
    c1.send("quit")
    c1.finish()
    srv.expect("First left.")
    srv.expect("Waiting for a new client...")
    srv.expect("Client connected.")
    srv.expect("Player name: Second")
    srv.answer()
    out = c2.read_until("nplab(Second)> ")
    check("second client served after first leaves",
          out == "Enter your name: Welcome, Second!\nnplab(Second)> ", repr(out))
    c2.send("quit")
    out = c2.finish()
    check("second client stats start at 0", "Guess count: 0, Correct count: 0\nGoodbye, Second!\n" in out)
    srv.expect("Second left.")
    srv.expect("Waiting for a new client...")


def test_tcp_stream(srv):
    print("[TCP byte stream: split and merged lines, CRLF]")
    s = socket.create_connection(("127.0.0.1", PORT))
    srv.expect("Client connected.")
    s.sendall(b"Fr")
    time.sleep(0.2)
    s.sendall(b"ank\r\n")
    srv.expect("Player name: Frank")
    ans = srv.answer()
    s.sendall(f"gue".encode())
    time.sleep(0.2)
    s.sendall(f"ss {ans}\nstatus\nquit\n".encode())
    data = b""
    s.settimeout(3)
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    exp = ("Enter your name: Welcome, Frank!\nnplab(Frank)> 4A0B\nnplab(Frank)> "
           "Guess count: 1, Correct count: 1\nnplab(Frank)> "
           "Guess count: 1, Correct count: 1\nGoodbye, Frank!\n")
    check("fragmented / merged input handled", data.decode() == exp, repr(data.decode()))
    srv.expect(f"Frank guessed {ans}.")
    srv.answer()
    srv.expect("Frank left.")
    srv.expect("Waiting for a new client...")


def test_hostname(srv):
    print("[client accepts a hostname]")
    c = Client(host="localhost")
    srv.expect("Client connected.")
    c.send("Grace")
    srv.expect("Player name: Grace")
    srv.answer()
    c.send("quit")
    out = c.finish()
    check("connect via 'localhost'", out.endswith("Goodbye, Grace!\n"), out)
    srv.expect("Grace left.")
    srv.expect("Waiting for a new client...")


def test_scoring(srv):
    print("[A/B scoring]")
    s = socket.create_connection(("127.0.0.1", PORT))
    srv.expect("Client connected.")
    s.sendall(b"Heidi\n")
    srv.expect("Player name: Heidi")
    ans = srv.answer()
    guesses = [wrong_guess(ans) for _ in range(20)]
    guesses.append(ans[1] + ans[0] + ans[3] + ans[2])  # 0A4B
    s.sendall("".join(f"guess {g}\n" for g in guesses).encode() + b"quit\n")
    s.settimeout(3)
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    results = re.findall(r"(\dA\dB)", data.decode())
    check("20 random guesses + 0A4B scored correctly",
          results == [score(ans, g) for g in guesses], f"{results}")
    for g in guesses:
        srv.expect(f"Heidi guessed {g}.")
    srv.expect("Heidi left.")
    srv.expect("Waiting for a new client...")


def test_reuseaddr():
    print("[SO_REUSEADDR: restart on same port right away]")
    srv = Server(PORT)
    c = Client()
    srv.expect("Client connected.")
    c.send("Ivan")
    srv.readline(); srv.readline()
    srv.stop()  # server closes first -> its side goes to TIME_WAIT
    c.finish()
    srv2 = Server(PORT)
    check("server rebinds immediately", srv2.p.poll() is None)
    srv2.stop()


def main():
    print(f"binaries: {BIN}  port: {PORT}")
    test_usage()
    srv = Server(PORT)
    check("server startup output", srv.log == [f"Server listening on port {PORT}.",
                                               "Waiting for a new client..."], srv.log)
    try:
        for t in (test_spec_scenario, test_three_rounds, test_abrupt_disconnect,
                  test_stdin_eof, test_queue, test_tcp_stream, test_hostname, test_scoring):
            try:
                t(srv)
            except Exception as e:  # keep going so we see every failure
                check(t.__name__, False, f"    {type(e).__name__}: {e}")
                srv.stop()
                srv = Server(PORT)
    finally:
        srv.stop()
    test_reuseaddr()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
