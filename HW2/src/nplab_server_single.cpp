// nplab_server_single.cpp - single-process concurrent 1A2B server.
//
// One process, no fork, no threads ("A Single Process Concurrent Server",
// Server Algorithms slides): select() waits on the master socket and every
// client socket at once. The 8-second turn deadlines of all running duels
// are handled with select()'s timeout: before each call we compute how long
// until the earliest deadline, so select() wakes up in time.
//
// All players and rooms live in this process's memory, so a duel is simply
// a Duel object whose output goes straight to both players' sessions.

#include "nplab_common.h"

#include <csignal>
#include <map>
#include <memory>
#include <sys/select.h>

class SingleSession;

struct Room {
    int id = 0, gameId = 0, rounds = 0;
    SingleSession *host = nullptr, *guest = nullptr;
    std::unique_ptr<Duel> duel;  // null while waiting for the second player
};

static std::map<int, Room *> g_rooms;  // room ID -> room
static int g_lastGameId = 0;

static void endRoomIfOver(Room *rm);

class SingleSession : public Session {
public:
    explicit SingleSession(int fd) : fd(fd) {}

    int fd;
    LineReader reader;
    Room *room = nullptr;

    void enterMode() {
        room = nullptr;
        if (!closing) state = State::MODE;
    }

protected:
    int newGameId() override { return ++g_lastGameId; }

    void createRoom(int id, int r) override {
        if (g_rooms.count(id)) {
            out += "Room already exists.\n";
            return;
        }
        room = new Room;
        room->id = id;
        room->rounds = r;
        room->gameId = newGameId();
        room->host = this;
        g_rooms[id] = room;
        roomCreated(room->gameId, id, r);
    }

    void joinRoom(int id) override {
        auto it = g_rooms.find(id);
        if (it == g_rooms.end()) {
            out += "Room not found.\n";
            return;
        }
        Room *rm = it->second;
        if (rm->guest) {
            out += "Room is full.\n";
            return;
        }
        if (rm->host->name == name) {
            out += "Player name already used in this room.\n";
            return;
        }
        rm->guest = this;
        room = rm;
        roomJoined(rm->gameId, id, rm->rounds);
        rm->host->state = State::DUEL;
        rm->duel.reset(new Duel(rm->gameId, id, rm->rounds, rm->host->name, name,
                                [rm](int i, const std::string &t) {
                                    (i == 0 ? rm->host : rm->guest)->out += t;
                                }));
        rm->duel->start();
    }

    void duelGuess(const std::string &g) override {
        Room *rm = room;
        rm->duel->guess(index(), g);
        endRoomIfOver(rm);
    }

    void duelStatus() override { out += room->duel->statusText(index()); }

    void duelLeave() override {
        Room *rm = room;
        rm->duel->leave(index(), true);
        goodbye();
        endRoomIfOver(rm);
    }

    void closeWaitingRoom() override {
        g_rooms.erase(room->id);
        delete room;
        room = nullptr;
        roomClosedLog();
    }

private:
    int index() const { return room->host == this ? 0 : 1; }
};

// After a duel ends: both players back to mode selection, room removed.
static void endRoomIfOver(Room *rm) {
    if (!rm->duel || !rm->duel->over()) return;
    rm->host->enterMode();
    rm->guest->enterMode();
    g_rooms.erase(rm->id);
    gameLog(rm->gameId, "Room " + std::to_string(rm->id) + " closed.");
    delete rm;
}

static void usage(const char *prog) {
    printf("Usage: %s <port number 1024-65535>\n", prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc != 2) usage(argv[0]);
    int port = parsePort(argv[1]);
    if (port < 0) usage(argv[0]);
    signal(SIGPIPE, SIG_IGN);  // a vanished client must not kill the server

    // ---- master socket
    int msock = socket(AF_INET, SOCK_STREAM, 0);
    if (msock < 0) { perror("socket"); return 1; }
    int on = 1;
    setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);
    if (bind(msock, (struct sockaddr *)&sin, sizeof(sin)) < 0) { perror("bind"); return 1; }
    if (listen(msock, 32) < 0) { perror("listen"); return 1; }

    logLine("Server listening on port " + std::to_string(port) + ".");
    logLine("Waiting for a new client...");

    std::map<int, SingleSession *> sessions;  // client fd -> session

    for (;;) {
        // ---- wait for input on any socket, or the earliest turn deadline
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(msock, &rfds);
        int maxfd = msock;
        for (auto &kv : sessions) {
            FD_SET(kv.first, &rfds);
            maxfd = std::max(maxfd, kv.first);
        }

        struct timeval tv, *ptv = nullptr;
        bool haveDeadline = false;
        Clock::time_point earliest;
        for (auto &kv : g_rooms)
            if (kv.second->duel && (!haveDeadline || kv.second->duel->deadline() < earliest)) {
                earliest = kv.second->duel->deadline();
                haveDeadline = true;
            }
        if (haveDeadline) {
            long ms = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - Clock::now()).count();
            if (ms < 0) ms = 0;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000 + 1000;  // +1ms: wake just after
            ptv = &tv;
        }

        if (select(maxfd + 1, &rfds, nullptr, nullptr, ptv) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            continue;
        }

        // ---- expired turns (each room has its own timer)
        Clock::time_point now = Clock::now();
        std::vector<Room *> expired;
        for (auto &kv : g_rooms)
            if (kv.second->duel && kv.second->duel->deadline() <= now) expired.push_back(kv.second);
        for (Room *rm : expired) {
            rm->duel->timeout();
            endRoomIfOver(rm);
        }

        // ---- new connection
        if (FD_ISSET(msock, &rfds)) {
            struct sockaddr_in fsin;
            socklen_t alen = sizeof(fsin);
            int fd = accept(msock, (struct sockaddr *)&fsin, &alen);
            if (fd >= 0) {
                logLine("Client connected.");
                sessions[fd] = new SingleSession(fd);
            }
        }

        // Send everything queued so far. Done after every command so each
        // reply is followed by its own prompt, even when several lines
        // arrived in one read (e.g. input piped into the client).
        auto flushAll = [&]() {
            for (auto &kv : sessions) {
                std::string o = kv.second->takeOutput();
                if (!o.empty()) sendAll(kv.first, o);
            }
        };
        flushAll();  // timeouts, new connection's name prompt

        // ---- client input: process every complete line, then EOF
        for (auto &kv : sessions) {
            if (!FD_ISSET(kv.first, &rfds)) continue;
            SingleSession *s = kv.second;
            s->reader.fill(kv.first);
            std::string line;
            while (!s->closing && s->reader.next(line)) {
                s->handleLine(line);
                flushAll();
            }
            if (s->reader.eof() && !s->closing) s->handleEOF();
        }

        // ---- send what is left; drop finished sessions
        for (auto it = sessions.begin(); it != sessions.end();) {
            SingleSession *s = it->second;
            std::string o = s->takeOutput();
            if (!o.empty()) sendAll(it->first, o);
            if (s->closing) {
                close(it->first);
                delete s;
                it = sessions.erase(it);
            } else {
                ++it;
            }
        }
    }
}
