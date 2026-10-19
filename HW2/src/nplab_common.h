// nplab_common.h - code shared by nplab_server_single and nplab_server_multi.
//
//   * socket / text helpers (port parsing, sendAll, LineReader)
//   * the 1A2B rules, the player-visible messages and the server log format
//   * Solo  : one single-player game
//   * Duel  : one two-player game (rounds, turns, 8-second deadline, scoring)
//   * Session: one logged-in player's state machine and command parser.
//              The two servers subclass it and only implement the parts that
//              depend on the process architecture (game IDs, rooms, duels).
//
// Keeping every message in this file guarantees both servers print exactly
// the same text.

#ifndef NPLAB_COMMON_H
#define NPLAB_COMMON_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

typedef std::chrono::steady_clock Clock;

static const int TURN_SECONDS = 8;     // two-player turn deadline
static const int DEFAULT_ROUNDS = 3;
static const size_t MAX_LINE = 4096;   // guard against endless lines

// ===================================================================== I/O

// Port: decimal digits only, value 1024..65535. Returns -1 when invalid.
inline int parsePort(const char *s) {
    if (*s == '\0') return -1;
    long v = 0;
    for (const char *p = s; *p; ++p) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
        if (v > 65535) return -1;
    }
    return v >= 1024 ? (int)v : -1;
}

// send() may write only part of the buffer, so loop. MSG_NOSIGNAL: a closed
// peer gives EPIPE instead of a SIGPIPE that would kill the server.
inline bool sendAll(int fd, const std::string &s) {
    size_t sent = 0;
    while (sent < s.size()) {
        ssize_t n = send(fd, s.data() + sent, s.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += n;
    }
    return true;
}

// Same for pipes (send() only works on sockets).
inline bool writeAll(int fd, const std::string &s) {
    size_t done = 0;
    while (done < s.size()) {
        ssize_t n = write(fd, s.data() + done, s.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += n;
    }
    return true;
}

// TCP (and pipes) are byte streams without message boundaries: one read may
// return half a line or several lines. LineReader buffers the bytes and
// hands out complete '\n'-terminated lines.
class LineReader {
public:
    // One read() from fd. Returns bytes read; 0 / -1 mark end of stream.
    ssize_t fill(int fd) {
        char tmp[4096];
        ssize_t n;
        do {
            n = read(fd, tmp, sizeof(tmp));
        } while (n < 0 && errno == EINTR);
        if (n > 0) buf_.append(tmp, n);
        else eof_ = true;
        return n;
    }

    // Next complete line (without "\r\n"). After end of stream, a trailing
    // line without '\n' is returned too.
    bool next(std::string &line) {
        size_t pos = buf_.find('\n');
        if (pos == std::string::npos) {
            if (buf_.empty() || (!eof_ && buf_.size() < MAX_LINE)) return false;
            pos = buf_.size();
        }
        line = buf_.substr(0, pos);
        buf_.erase(0, std::min(pos + 1, buf_.size()));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

    // Blocking: read until a full line arrives. False on EOF / error / timeout.
    bool readLine(int fd, std::string &line) {
        for (;;) {
            if (next(line)) return true;
            if (eof_) return false;
            fill(fd);
        }
    }

    bool eof() const { return eof_; }
    bool hasLine() const { return buf_.find('\n') != std::string::npos || (eof_ && !buf_.empty()); }

private:
    std::string buf_;
    bool eof_ = false;
};

// Server log: one complete line per write() so lines printed by different
// processes (multi version) never mix within a line.
inline void logLine(const std::string &s) {
    writeAll(STDOUT_FILENO, s + "\n");
}
inline void gameLog(int gameId, const std::string &s) {
    logLine("[Game " + std::to_string(gameId) + "] " + s);
}

// Multi-line text sent between processes as one line: escape '\\' and '\n'.
inline std::string escapeText(const std::string &s) {
    std::string r;
    for (char c : s) {
        if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    return r;
}
inline std::string unescapeText(const std::string &s) {
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            r += (s[i] == 'n') ? '\n' : s[i];
        } else {
            r += s[i];
        }
    }
    return r;
}

inline std::vector<std::string> splitWords(const std::string &line) {
    std::istringstream iss(line);
    std::vector<std::string> w;
    for (std::string t; iss >> t;) w.push_back(t);
    return w;
}

// ============================================================== validation

// ^[A-Za-z][A-Za-z0-9_]{0,19}$
inline bool validName(const std::string &s) {
    if (s.empty() || s.size() > 20) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = s[i];
        if (c >= 128) return false;
        if (i == 0 ? !isalpha(c) : !(isalnum(c) || c == '_')) return false;
    }
    return true;
}

// Four distinct decimal digits; a leading 0 is allowed.
inline bool validGuess(const std::string &s) {
    if (s.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        for (size_t j = 0; j < i; ++j)
            if (s[i] == s[j]) return false;
    }
    return true;
}

// Room ID: decimal 1..9999 without leading zeros. Returns -1 when invalid.
inline int parseRoom(const std::string &s) {
    if (s.empty() || s.size() > 4 || s[0] < '1' || s[0] > '9') return -1;
    for (char c : s)
        if (c < '0' || c > '9') return -1;
    return atoi(s.c_str());
}

// Rounds: a single digit 1..9. Returns -1 when invalid.
inline int parseRounds(const std::string &s) {
    return (s.size() == 1 && s[0] >= '1' && s[0] <= '9') ? s[0] - '0' : -1;
}

// ================================================================ 1A2B core

inline std::mt19937 &rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
// Each forked child must call this, otherwise all children would share the
// parent's generator state and produce the same answers.
inline void reseedRng() { rng().seed(std::random_device{}() ^ (unsigned)getpid()); }

inline std::string newAnswer() {
    std::string d = "0123456789";
    std::shuffle(d.begin(), d.end(), rng());
    return d.substr(0, 4);
}

// A = right digit in the right place, B = right digit in the wrong place.
inline std::string score(const std::string &answer, const std::string &guess) {
    int a = 0, b = 0;
    for (int i = 0; i < 4; ++i) {
        if (guess[i] == answer[i]) ++a;
        else if (answer.find(guess[i]) != std::string::npos) ++b;
    }
    return std::to_string(a) + "A" + std::to_string(b) + "B";
}

// ============================================================== messages

static const char *const MODE_COMMANDS = "Commands: solo [rounds], duo, help, quit\n";
static const char *const ROOM_COMMANDS =
    "Commands: create <room ID> [rounds], join <room ID>, back, help, quit\n";
static const char *const RETURNED = "Returned to mode selection.\n";
static const char *const NOT_AVAILABLE = "Command not available now.\n";

inline std::string countsText(int guesses, int correct) {
    return "Guess count: " + std::to_string(guesses) +
           ", Correct count: " + std::to_string(correct);
}

// ==================================================================== Solo

struct Solo {
    int rounds = 0, round = 0, guesses = 0, correct = 0;
    std::string answer;
};

// ==================================================================== Duel
//
// One two-player game. Player 0 is the room creator (Player 1 in the spec),
// player 1 the one who joined. The Duel only produces text: out(i, text)
// queues text for player i, server log lines go to stdout. The owner decides
// how text reaches the players (direct socket in the single server, local
// socket or child-to-child TCP in the multi server).

class Duel {
public:
    struct Player {
        std::string name;
        int score = 0, guesses = 0, correct = 0;
        bool submitted = false;
        std::string guess;
    };

    Duel(int gameId, int roomId, int rounds, const std::string &p1,
         const std::string &p2, std::function<void(int, const std::string &)> out)
        : gameId_(gameId), roomId_(roomId), rounds_(rounds), out_(out) {
        p_[0].name = p1;
        p_[1].name = p2;
    }

    bool over() const { return over_; }
    Clock::time_point deadline() const { return deadline_; }

    void start() {
        log("Game started: " + p_[0].name + " vs " + p_[1].name + ".");
        both("Room " + std::to_string(roomId_) + ": " + p_[0].name + " vs " +
             p_[1].name + ". Rounds: " + std::to_string(rounds_) + ".\n");
        newRound();
    }

    // A valid "guess" from player i. Guesses stay hidden until both players
    // submitted or the deadline passed.
    void guess(int i, const std::string &g) {
        if (over_) return;
        if (p_[i].submitted) {
            out_(i, "You have already guessed this turn.\n");
            return;
        }
        p_[i].submitted = true;
        p_[i].guess = g;
        if (p_[1 - i].submitted) reveal();
        else out_(i, "Waiting for the other's answer...\n");
    }

    // Called by the owner's event loop once the deadline has passed.
    void timeout() {
        if (!over_) reveal();
    }

    std::string statusText(int i) const {
        (void)i;  // both players see the same information
        std::string s = "Room: " + std::to_string(roomId_) + "\n";
        s += "Round: " + std::to_string(round_) + "/" + std::to_string(rounds_) +
             ", Turn: " + std::to_string(turn_) + "\n";
        s += "Score: " + scoreText() + "\n";
        for (int k = 0; k < 2; ++k)
            s += p_[k].name + ": " + countsText(p_[k].guesses, p_[k].correct) + "\n";
        std::string waiting;
        for (int k = 0; k < 2; ++k)
            if (!p_[k].submitted) waiting += (waiting.empty() ? "" : ", ") + p_[k].name;
        s += "Waiting for: " + waiting + "\n";
        return s;
    }

    // Player i left (quit or connection closed): the other player wins.
    // Only results already revealed count. withText: player i still wants
    // the summary (used quit / stdin EOF); false when it is unreachable.
    void leave(int i, bool withText) {
        if (over_) return;
        over_ = true;
        int o = 1 - i;
        log(p_[i].name + " left.");
        log("Game over.");
        log("Winner: " + p_[o].name + ".");
        log("Score: " + scoreText() + ".");
        std::string summary = "Game over.\nWinner: " + p_[o].name + ".\nScore: " + scoreText() + ".\n";
        for (int k = 0; k < 2; ++k)
            summary += p_[k].name + ": " + countsText(p_[k].guesses, p_[k].correct) + "\n";
        out_(o, summary + RETURNED + MODE_COMMANDS);
        if (withText) out_(i, summary);
        for (int k = 0; k < 2; ++k) logFinal(k);
        log(p_[o].name + " returned to mode selection.");
    }

private:
    void log(const std::string &s) const { gameLog(gameId_, s); }
    void both(const std::string &s) { out_(0, s); out_(1, s); }
    std::string scoreText() const {
        return p_[0].name + " " + std::to_string(p_[0].score) + ", " + p_[1].name + " " +
               std::to_string(p_[1].score);
    }
    void logFinal(int k) const {
        log(p_[k].name + " final status: " + countsText(p_[k].guesses, p_[k].correct) + ".");
    }

    void newRound() {
        ++round_;
        turn_ = 0;
        answer_ = newAnswer();
        std::string r = std::to_string(round_) + "/" + std::to_string(rounds_);
        both("Round " + r + ".\n");
        log("Round " + r + ".");
        log("Answer for this round: " + answer_ + ".");
        newTurn();
    }

    void newTurn() {
        ++turn_;
        p_[0].submitted = p_[1].submitted = false;
        deadline_ = Clock::now() + std::chrono::seconds(TURN_SECONDS);
        both("Turn " + std::to_string(turn_) + ".\n");
        log("Turn " + std::to_string(turn_) + ".");
    }

    // Publish both results (Player 1 first), then score / next turn / end.
    void reveal() {
        std::string text;
        bool win[2] = {false, false};
        for (int k = 0; k < 2; ++k) {
            std::string line;
            if (p_[k].submitted) {
                std::string res = score(answer_, p_[k].guess);
                ++p_[k].guesses;
                if (res == "4A0B") {
                    ++p_[k].correct;
                    win[k] = true;
                }
                line = p_[k].name + " guessed " + p_[k].guess + ": " + res + ".";
            } else {
                line = p_[k].name + " timed out.";
            }
            text += line + "\n";
            log(line);
        }
        if (!win[0] && !win[1]) {  // nobody solved it: same answer, next turn
            both(text);
            newTurn();
            return;
        }
        std::string winners;
        for (int k = 0; k < 2; ++k)
            if (win[k]) {
                ++p_[k].score;
                winners += (winners.empty() ? "" : ", ") + p_[k].name;
            }
        text += "Round winner: " + winners + ".\nScore: " + scoreText() + ".\n";
        log("Round winner: " + winners + ".");
        log("Score: " + scoreText() + ".");
        both(text);
        if (round_ < rounds_) newRound();
        else finish();
    }

    // Normal end after the last round.
    void finish() {
        over_ = true;
        std::string winner;
        if (p_[0].score != p_[1].score) winner = p_[p_[0].score > p_[1].score ? 0 : 1].name;
        else if (p_[0].guesses != p_[1].guesses) winner = p_[p_[0].guesses > p_[1].guesses ? 0 : 1].name;
        else winner = "Draw";
        log("Game over.");
        log("Winner: " + winner + ".");
        for (int k = 0; k < 2; ++k)
            out_(k, "Game over.\nWinner: " + winner + ".\n" +
                        countsText(p_[k].guesses, p_[k].correct) + "\n" + RETURNED + MODE_COMMANDS);
        for (int k = 0; k < 2; ++k) logFinal(k);
        for (int k = 0; k < 2; ++k) log(p_[k].name + " returned to mode selection.");
    }

    int gameId_, roomId_, rounds_;
    int round_ = 0, turn_ = 0;
    std::string answer_;
    Player p_[2];
    bool over_ = false;
    Clock::time_point deadline_;
    std::function<void(int, const std::string &)> out_;
};

// ================================================================= Session
//
// One connected player. handleLine() parses a command and validates it in
// the order the spec requires: unknown command -> argument format (Usage) ->
// current state (Command not available now.) -> room conditions.
// Text for the player accumulates in `out`; takeOutput() turns it into the
// bytes to send, adding the prompt.

enum class State { LOGIN, MODE, ROOMSEL, WAITING, SOLO, DUEL };

class Session {
public:
    virtual ~Session() {}

    State state = State::LOGIN;
    std::string name;
    std::string out;          // queued text for the client
    bool closing = false;     // flush `out`, then close the connection

    void handleLine(const std::string &line) {
        if (closing) return;
        needPrompt_ = true;
        promptShown_ = false;  // the user pressed Enter: cursor is at column 0

        if (state == State::LOGIN) {
            if (validName(line)) {
                name = line;
                state = State::MODE;
                logLine("Player name: " + name);
                out += "Welcome, " + name + "!\n" + MODE_COMMANDS;
            } else {
                out += "Invalid player name.\n";
            }
            return;
        }

        std::vector<std::string> w = splitWords(line);
        if (w.empty()) return;  // blank line: ignored
        const std::string &cmd = w[0];
        size_t argc = w.size() - 1;

        if (cmd == "solo") {
            int rounds = DEFAULT_ROUNDS;
            if (argc > 1 || (argc == 1 && (rounds = parseRounds(w[1])) < 0))
                return usage("solo [rounds]");
            if (state != State::MODE) return notAvailable();
            startSolo(rounds);
        } else if (cmd == "duo") {
            if (argc != 0) return usage("duo");
            if (state != State::MODE) return notAvailable();
            state = State::ROOMSEL;
            out += std::string("Two-player mode.\n") + ROOM_COMMANDS;
        } else if (cmd == "back") {
            if (argc != 0) return usage("back");
            if (state != State::ROOMSEL) return notAvailable();
            state = State::MODE;
            out += std::string(RETURNED) + MODE_COMMANDS;
        } else if (cmd == "help") {
            if (argc != 0) return usage("help");
            out += helpText();
        } else if (cmd == "quit") {
            if (argc != 0) return usage("quit");
            quit();
        } else if (cmd == "status") {
            if (argc != 0) return usage("status");
            if (state == State::SOLO) {
                out += "Round: " + std::to_string(solo_.round) + "/" + std::to_string(solo_.rounds) +
                       "\n" + countsText(solo_.guesses, solo_.correct) + "\n";
            } else if (state == State::WAITING) {
                out += "Room: " + std::to_string(roomId) + "\nRounds: " + std::to_string(rounds) +
                       "\nStatus: Waiting for another player.\n" + countsText(0, 0) + "\n";
            } else if (state == State::DUEL) {
                duelStatus();
            } else {
                notAvailable();
            }
        } else if (cmd == "create") {
            int room = -1, r = DEFAULT_ROUNDS;
            if (argc < 1 || argc > 2 || (room = parseRoom(w[1])) < 0 ||
                (argc == 2 && (r = parseRounds(w[2])) < 0))
                return usage("create <room ID> [rounds]");
            if (state != State::ROOMSEL) return notAvailable();
            createRoom(room, r);
        } else if (cmd == "join") {
            int room = -1;
            if (argc != 1 || (room = parseRoom(w[1])) < 0) return usage("join <room ID>");
            if (state != State::ROOMSEL) return notAvailable();
            joinRoom(room);
        } else if (cmd == "guess") {
            if (argc != 1 || !validGuess(w[1])) return usage("guess <4 distinct digits>");
            if (state == State::SOLO) soloGuess(w[1]);
            else if (state == State::WAITING) out += "Waiting for another player. You cannot guess yet.\n";
            else if (state == State::DUEL) duelGuess(w[1]);
            else notAvailable();
        } else {
            out += "Unknown command: " + cmd + "\n";
        }
    }

    // The client closed the connection, or its stdin hit EOF (it then
    // half-closes the socket). Logged in: same as quit.
    void handleEOF() {
        if (closing) return;
        promptShown_ = false;  // output exactly as if "quit" had been typed
        if (state == State::LOGIN) {
            logLine("Client disconnected before login.");
            closing = true;
            return;
        }
        quit();
    }

    // Bytes to send to the client now ("" if nothing). An event that arrives
    // while the prompt is on screen starts on a new line; every batch ends
    // with a fresh prompt unless the connection is closing.
    std::string takeOutput() {
        if (busy() || (out.empty() && !needPrompt_)) return "";
        std::string s = (promptShown_ && !out.empty()) ? "\n" : "";
        s += out;
        out.clear();
        if (!closing) {
            s += prompt();
            promptShown_ = true;
        }
        needPrompt_ = false;
        return s;
    }

protected:
    int gameId = 0, roomId = 0, rounds = 0;

    // ---- architecture-specific parts
    virtual int newGameId() = 0;
    virtual void createRoom(int room, int rounds) = 0;
    virtual void joinRoom(int room) = 0;
    virtual void duelGuess(const std::string &g) = 0;
    virtual void duelStatus() = 0;
    virtual void duelLeave() = 0;      // quit during a duel (must end with goodbye())
    virtual void closeWaitingRoom() = 0;
    virtual bool busy() const { return false; }  // waiting for another process

    // ---- shared helpers for subclasses
    void goodbye() {
        out += "Goodbye, " + name + "!\n";
        closing = true;
    }

    void roomCreated(int gid, int room, int r) {
        gameId = gid;
        roomId = room;
        rounds = r;
        state = State::WAITING;
        out += "Room " + std::to_string(room) + " created. Rounds: " + std::to_string(r) +
               ".\nWaiting for another player...\n";
        gameLog(gid, "Room " + std::to_string(room) + " created by " + name + ". Rounds: " +
                         std::to_string(r) + ".");
        gameLog(gid, "Waiting for another player.");
    }

    void roomJoined(int gid, int room, int r) {
        gameId = gid;
        roomId = room;
        rounds = r;
        state = State::DUEL;
        out += "Joined room " + std::to_string(room) + ".\n";
        gameLog(gid, name + " joined room " + std::to_string(room) + ".");
    }

    void roomClosedLog() { gameLog(gameId, "Room " + std::to_string(roomId) + " closed."); }

    std::string prompt() const {
        return state == State::LOGIN ? "Enter your name: " : "nplab(" + name + ")> ";
    }

    bool needPrompt_ = true;  // show a prompt at the next flush (first: name)
    bool promptShown_ = false;

private:
    void usage(const std::string &u) { out += "Usage: " + u + "\n"; }
    void notAvailable() { out += NOT_AVAILABLE; }

    std::string helpText() const {
        std::string h = "Available commands:\n";
        switch (state) {
        case State::MODE:
            return h + "  solo [rounds]\n  duo\n  help\n  quit\nRounds: 1-9, default 3.\n";
        case State::ROOMSEL:
            return h + "  create <room ID> [rounds]\n  join <room ID>\n  back\n  help\n  quit\n"
                       "Room ID: 1-9999. Rounds: 1-9, default 3.\n";
        case State::WAITING:
            return h + "  status\n  help\n  quit\n";
        default:  // SOLO, DUEL
            return h + "  guess <4 distinct digits>\n  status\n  help\n  quit\n";
        }
    }

    void quit() {
        switch (state) {
        case State::MODE:
        case State::ROOMSEL:
            logLine(name + " left.");
            goodbye();
            break;
        case State::WAITING:
            gameLog(gameId, name + " left.");
            closeWaitingRoom();
            goodbye();
            break;
        case State::SOLO:
            gameLog(gameId, name + " final status: " + countsText(solo_.guesses, solo_.correct) + ".");
            gameLog(gameId, name + " left.");
            out += countsText(solo_.guesses, solo_.correct) + "\n";
            goodbye();
            break;
        case State::DUEL:
            duelLeave();
            break;
        default:
            break;
        }
    }

    // ---- single-player game (identical in both servers)
    void startSolo(int r) {
        gameId = newGameId();
        solo_ = Solo();
        solo_.rounds = r;
        state = State::SOLO;
        out += "Single-player game started. Rounds: " + std::to_string(r) + ".\n";
        gameLog(gameId, "Single-player game started: " + name + ". Rounds: " + std::to_string(r) + ".");
        soloRound();
    }

    void soloRound() {
        ++solo_.round;
        solo_.answer = newAnswer();
        std::string r = std::to_string(solo_.round) + "/" + std::to_string(solo_.rounds);
        out += "Round " + r + ".\n";
        gameLog(gameId, "Round " + r + ".");
        gameLog(gameId, "Answer for this round: " + solo_.answer + ".");
    }

    void soloGuess(const std::string &g) {
        std::string res = score(solo_.answer, g);
        ++solo_.guesses;
        gameLog(gameId, name + " guessed " + g + ": " + res + ".");
        out += res + "\n";
        if (res != "4A0B") return;
        ++solo_.correct;
        if (solo_.round < solo_.rounds) return soloRound();
        // last round solved
        gameLog(gameId, "Game over.");
        gameLog(gameId, name + " final status: " + countsText(solo_.guesses, solo_.correct) + ".");
        gameLog(gameId, name + " returned to mode selection.");
        out += countsText(solo_.guesses, solo_.correct) + "\n" + RETURNED + MODE_COMMANDS;
        state = State::MODE;
    }

    Solo solo_;
};

#endif
