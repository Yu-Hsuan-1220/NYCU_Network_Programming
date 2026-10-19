// nplab_server_multi.cpp - multi-process concurrent 1A2B server.
//
//                         parent
//          accepts connections, owns the room table and Game IDs,
//          reaps finished children (SIGCHLD + waitpid)
//               |  pipe pair          |  pipe pair
//          child (Alice)  ===TCP===  child (Bob)
//          room host: answer,        relays Bob's commands to the host,
//          turns, deadline, score    shows the host's events to Bob
//               |                          |
//            client                     client
//
// * Every accepted client gets its own child process for its whole session.
// * parent <-> child: two pipes per child (a pipe is one-way). The child
//   sends one request line and blocks for the one-line reply; the parent
//   never starts a conversation. Requests:
//     GAMEID                          -> OK <gameId>
//     CREATE <room> <rounds> <port> <name>
//                                     -> OK <gameId> | EXISTS
//     JOIN <room> <name>              -> OK <port> <gameId> <rounds> | NOTFOUND
//                                        | FULL | SAMENAME   (room reserved)
//     JOINED <room>                   -> OK   (reserved -> playing)
//     JOINFAIL <room>                 -> OK   (reserved -> waiting)
//     CLOSE <room>                    -> OK   (room removed)
// * child <-> child: TCP on 127.0.0.1. The host child listens on a port in
//   [IPC_PORT_MIN, IPC_PORT_MAX]; the joining child asks the parent for that
//   port and connects. Handshake: HELLO <name> / WELCOME / READY.
//   Guest -> host: GUESS <n> | STATUS | QUIT.
//   Host -> guest: OUT <escaped text> (show to the player), DONE (reply to
//   the guest's request complete), END (game over; also completes a request).
//   The parent never relays game data.

#include "nplab_common.h"

#include <csignal>
#include <map>
#include <memory>
#include <sys/select.h>
#include <sys/wait.h>

#ifndef IPC_PORT_MIN
#define IPC_PORT_MIN 20000
#endif
#ifndef IPC_PORT_MAX
#define IPC_PORT_MAX 20099
#endif

// ======================================================= child: parent link

static int g_reqFd = -1;   // child -> parent (write end)
static int g_respFd = -1;  // parent -> child (read end)
static LineReader g_respReader;

// Synchronous request to the parent over the pipes.
static std::vector<std::string> askParent(const std::string &req) {
    std::string line;
    if (!writeAll(g_reqFd, req + "\n") || !g_respReader.readLine(g_respFd, line)) exit(1);
    return splitWords(line);
}

static void setRecvTimeout(int fd, int sec) {
    struct timeval tv = {sec, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Listening socket for child-to-child traffic: 127.0.0.1 only, first free
// port in the TA's range. Returns the socket and sets `port`, or -1.
static int openInternalListener(int &port) {
    for (int p = IPC_PORT_MIN; p <= IPC_PORT_MAX; ++p) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return -1;
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sin.sin_port = htons(p);
        // bind or listen fails with EADDRINUSE if another room holds p
        if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) == 0 && listen(s, 4) == 0) {
            port = p;
            return s;
        }
        close(s);
    }
    return -1;
}

static int connectLocal(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(port);
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

// ======================================================= child: session

class MultiSession : public Session {
public:
    explicit MultiSession(int cfd) : cfd_(cfd) {}

    // The child's whole life: serve one client until it leaves.
    void run() {
        for (;;) {
            progress();
            flush();
            if (closing && !pending_) return;

            fd_set rfds;
            FD_ZERO(&rfds);
            int maxfd = -1;
            auto add = [&](int fd) { FD_SET(fd, &rfds); maxfd = std::max(maxfd, fd); };
            if (!pending_ && !creader_.eof()) add(cfd_);
            if (role_ == HOST && state == State::WAITING) add(lfd_);
            if (pfd_ >= 0) add(pfd_);

            struct timeval tv, *ptv = nullptr;
            if (duel_) {  // host: wake up at the turn deadline
                long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              duel_->deadline() - Clock::now()).count();
                if (ms < 0) ms = 0;
                tv.tv_sec = ms / 1000;
                tv.tv_usec = (ms % 1000) * 1000 + 1000;
                ptv = &tv;
            }
            if (maxfd < 0 && !ptv) return;  // nothing left to wait for

            if (select(maxfd + 1, &rfds, nullptr, nullptr, ptv) < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (duel_ && Clock::now() >= duel_->deadline()) {
                duel_->timeout();
                afterHostDuel(false);
            }
            if (maxfd >= 0 && FD_ISSET(cfd_, &rfds) && !pending_) creader_.fill(cfd_);
            if (role_ == HOST && lfd_ >= 0 && state == State::WAITING && FD_ISSET(lfd_, &rfds))
                acceptGuest();
            if (pfd_ >= 0 && FD_ISSET(pfd_, &rfds)) preader_.fill(pfd_);
        }
    }

protected:
    int newGameId() override { return atoi(askParent("GAMEID")[1].c_str()); }

    void createRoom(int room, int r) override {
        int port;
        lfd_ = openInternalListener(port);
        if (lfd_ < 0) {  // every port in the range is taken
            out += NOT_AVAILABLE;
            return;
        }
        std::vector<std::string> rep = askParent("CREATE " + std::to_string(room) + " " +
                                                 std::to_string(r) + " " + std::to_string(port) +
                                                 " " + name);
        if (rep[0] != "OK") {
            close(lfd_);
            lfd_ = -1;
            out += "Room already exists.\n";
            return;
        }
        role_ = HOST;
        roomCreated(atoi(rep[1].c_str()), room, r);
    }

    void joinRoom(int room) override {
        std::string rs = std::to_string(room);
        std::vector<std::string> rep = askParent("JOIN " + rs + " " + name);
        if (rep[0] == "NOTFOUND") { out += "Room not found.\n"; return; }
        if (rep[0] == "FULL") { out += "Room is full.\n"; return; }
        if (rep[0] == "SAMENAME") { out += "Player name already used in this room.\n"; return; }

        // reserved for us: connect to the host child and shake hands
        int s = connectLocal(atoi(rep[1].c_str()));
        std::string line;
        LineReader rd;
        if (s >= 0) {
            setRecvTimeout(s, 3);
            if (!sendAll(s, "HELLO " + name + "\n") || !rd.readLine(s, line) || line != "WELCOME") {
                close(s);
                s = -1;
            }
        }
        if (s < 0) {  // the host left in the meantime
            askParent("JOINFAIL " + rs);
            out += "Room not found.\n";
            return;
        }
        roomJoined(atoi(rep[2].c_str()), room, atoi(rep[3].c_str()));
        askParent("JOINED " + rs);  // tell the parent: reserved -> playing
        sendAll(s, "READY\n");      // host starts the game and answers with DONE
        role_ = GUEST;
        pfd_ = s;
        preader_ = rd;
        pending_ = true;
    }

    void duelGuess(const std::string &g) override {
        if (role_ == HOST) {
            duel_->guess(0, g);
            afterHostDuel(false);
        } else {
            request("GUESS " + g);
        }
    }

    void duelStatus() override {
        if (role_ == HOST) out += duel_->statusText(0);
        else request("STATUS");
    }

    void duelLeave() override {
        if (role_ == HOST) {
            duel_->leave(0, true);
            goodbye();
            afterHostDuel(false);
        } else {
            quitting_ = true;  // goodbye after the host's summary arrives
            request("QUIT");
        }
    }

    void closeWaitingRoom() override {
        close(lfd_);  // release the internal port
        lfd_ = -1;
        askParent("CLOSE " + std::to_string(roomId));
        role_ = NONE;
        roomClosedLog();
    }

    bool busy() const override { return pending_; }

private:
    enum Role { NONE, HOST, GUEST };

    // Handle everything already buffered: peer messages, then client lines
    // (one at a time; a guest stops while a request to the host is pending).
    // Output is flushed after every step so each reply gets its own prompt.
    void progress() {
        std::string line;
        auto peer = [&]() {
            while (pfd_ >= 0 && preader_.next(line)) {
                peerLine(line);
                flush();
            }
            if (pfd_ >= 0 && preader_.eof()) peerClosed();
        };
        peer();
        while (!pending_ && !closing && creader_.next(line)) {
            handleLine(line);
            flush();
            peer();
        }
        if (!pending_ && !closing && creader_.eof()) handleEOF();
    }

    void flush() {
        std::string o = takeOutput();
        if (!o.empty()) sendAll(cfd_, o);
    }

    void request(const std::string &msg) {
        pending_ = true;
        if (!sendAll(pfd_, msg + "\n")) peerClosed();
    }

    void sendPeer(const std::string &msg) {
        if (pfd_ >= 0) sendAll(pfd_, msg + "\n");
    }

    // ---- host side
    void acceptGuest() {
        int s = accept(lfd_, nullptr, nullptr);
        if (s < 0) return;
        setRecvTimeout(s, 3);
        LineReader rd;
        std::string line;
        if (!rd.readLine(s, line) || line.compare(0, 6, "HELLO ") != 0) { close(s); return; }
        std::string guestName = line.substr(6);
        if (!sendAll(s, "WELCOME\n") || !rd.readLine(s, line) || line != "READY") { close(s); return; }

        pfd_ = s;
        preader_ = rd;
        state = State::DUEL;
        duel_.reset(new Duel(gameId, roomId, rounds, name, guestName,
                             [this](int i, const std::string &t) {
                                 if (i == 0) out += t;
                                 else remote_ += t;
                             }));
        duel_->start();
        afterHostDuel(true);  // the guest's join request is complete
    }

    // Send the guest its queued text; finish the room when the duel is over.
    void afterHostDuel(bool reply) {
        if (!remote_.empty()) {
            sendPeer("OUT " + escapeText(remote_));
            remote_.clear();
        }
        if (!duel_->over()) {
            if (reply) sendPeer("DONE");
            return;
        }
        sendPeer("END");
        if (pfd_ >= 0) {  // let the guest read everything before closing
            shutdown(pfd_, SHUT_WR);
            setRecvTimeout(pfd_, 2);
            char buf[512];
            while (recv(pfd_, buf, sizeof(buf), 0) > 0) {}
            close(pfd_);
            pfd_ = -1;
        }
        close(lfd_);
        lfd_ = -1;
        askParent("CLOSE " + std::to_string(roomId));
        roomClosedLog();
        duel_.reset();
        role_ = NONE;
        if (!closing) state = State::MODE;
    }

    // ---- messages from the other child
    void peerLine(const std::string &line) {
        if (role_ == HOST) {
            if (line.compare(0, 6, "GUESS ") == 0) duel_->guess(1, line.substr(6));
            else if (line == "STATUS") remote_ += duel_->statusText(1);
            else if (line == "QUIT") duel_->leave(1, true);
            else return;
            afterHostDuel(true);
        } else {
            if (line.compare(0, 4, "OUT ") == 0) {
                out += unescapeText(line.substr(4));
            } else if (line == "DONE") {
                pending_ = false;
                if (quitting_) goodbye();
            } else if (line == "END") {
                leaveDuelAsGuest();
            }
        }
    }

    void peerClosed() {
        if (role_ == HOST) {  // the guest's process vanished
            duel_->leave(1, false);
            afterHostDuel(false);
        } else {              // the host's process vanished
            if (!quitting_) out += std::string(RETURNED) + MODE_COMMANDS;
            leaveDuelAsGuest();
        }
    }

    void leaveDuelAsGuest() {
        close(pfd_);
        pfd_ = -1;
        role_ = NONE;
        pending_ = false;
        if (quitting_) goodbye();
        else state = State::MODE;
    }

    int cfd_;
    LineReader creader_;
    Role role_ = NONE;
    int lfd_ = -1;            // host: internal listening socket
    int pfd_ = -1;            // socket to the other child
    LineReader preader_;
    std::unique_ptr<Duel> duel_;  // host only
    std::string remote_;      // host: text queued for the guest
    bool pending_ = false;    // guest: waiting for the host's reply
    bool quitting_ = false;
};

// ================================================================== parent

struct ChildRec {
    pid_t pid;
    int reqFd;   // read requests from the child
    int respFd;  // write replies to the child
    LineReader reader;
};

struct RoomRec {
    enum St { WAITING, RESERVED, PLAYING };
    int gameId, rounds, port;
    std::string host;
    pid_t hostPid;
    St st;
    pid_t reservedBy;
};

static std::map<int, RoomRec> g_rooms;
static int g_lastGameId = 0;

static std::string handleRequest(pid_t pid, const std::string &line) {
    std::vector<std::string> w = splitWords(line);
    if (w.empty()) return "ERR";
    if (w[0] == "GAMEID") return "OK " + std::to_string(++g_lastGameId);

    int room = w.size() > 1 ? atoi(w[1].c_str()) : 0;
    auto it = g_rooms.find(room);
    if (w[0] == "CREATE" && w.size() == 5) {
        if (it != g_rooms.end()) return "EXISTS";
        RoomRec r{++g_lastGameId, atoi(w[2].c_str()), atoi(w[3].c_str()), w[4], pid,
                  RoomRec::WAITING, 0};
        g_rooms[room] = r;
        return "OK " + std::to_string(r.gameId);
    }
    if (w[0] == "JOIN" && w.size() == 3) {
        if (it == g_rooms.end()) return "NOTFOUND";
        RoomRec &r = it->second;
        if (r.st != RoomRec::WAITING) return "FULL";
        if (r.host == w[2]) return "SAMENAME";
        r.st = RoomRec::RESERVED;  // nobody else can join while we connect
        r.reservedBy = pid;
        return "OK " + std::to_string(r.port) + " " + std::to_string(r.gameId) + " " +
               std::to_string(r.rounds);
    }
    if (w[0] == "JOINED") {
        if (it != g_rooms.end()) it->second.st = RoomRec::PLAYING;
        return "OK";
    }
    if (w[0] == "JOINFAIL") {
        if (it != g_rooms.end() && it->second.st == RoomRec::RESERVED && it->second.reservedBy == pid)
            it->second.st = RoomRec::WAITING;
        return "OK";
    }
    if (w[0] == "CLOSE") {
        if (it != g_rooms.end() && it->second.hostPid == pid) g_rooms.erase(it);
        return "OK";
    }
    return "ERR";
}

// A child ended: drop rooms it still owned, undo its reservations.
static void forgetChild(pid_t pid) {
    for (auto it = g_rooms.begin(); it != g_rooms.end();) {
        if (it->second.hostPid == pid) {
            it = g_rooms.erase(it);
            continue;
        }
        if (it->second.st == RoomRec::RESERVED && it->second.reservedBy == pid)
            it->second.st = RoomRec::WAITING;
        ++it;
    }
}

// Reap finished children so they do not stay as zombies.
static void reaper(int) {
    int saved = errno;
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    errno = saved;
}

static void usage(const char *prog) {
    printf("Usage: %s <port number 1024-65535>\n", prog);
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc != 2) usage(argv[0]);
    int port = parsePort(argv[1]);
    if (port < 0) usage(argv[0]);

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reaper;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

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

    std::vector<ChildRec *> children;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(msock, &rfds);
        int maxfd = msock;
        for (ChildRec *c : children) {
            FD_SET(c->reqFd, &rfds);
            maxfd = std::max(maxfd, c->reqFd);
        }
        if (select(maxfd + 1, &rfds, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;  // SIGCHLD arrived
            perror("select");
            continue;
        }

        // ---- new client: fork a dedicated child
        if (FD_ISSET(msock, &rfds)) {
            int cfd = accept(msock, nullptr, nullptr);
            int c2p[2], p2c[2];
            if (cfd >= 0 && pipe(c2p) == 0) {
                if (pipe(p2c) != 0) {
                    close(c2p[0]);
                    close(c2p[1]);
                    close(cfd);
                } else {
                    logLine("Client connected.");
                    pid_t pid = fork();
                    if (pid == 0) {  // ---- child
                        // keep only what this child needs
                        close(msock);
                        for (ChildRec *c : children) {
                            close(c->reqFd);
                            close(c->respFd);
                        }
                        close(c2p[0]);
                        close(p2c[1]);
                        signal(SIGCHLD, SIG_DFL);
                        reseedRng();
                        g_reqFd = c2p[1];
                        g_respFd = p2c[0];
                        MultiSession session(cfd);
                        session.run();
                        close(cfd);
                        exit(0);
                    }
                    // ---- parent: the child serves the client now
                    close(cfd);
                    close(c2p[1]);
                    close(p2c[0]);
                    if (pid < 0) {
                        close(c2p[0]);
                        close(p2c[1]);
                    } else {
                        children.push_back(new ChildRec{pid, c2p[0], p2c[1], LineReader()});
                    }
                }
            } else if (cfd >= 0) {
                close(cfd);
            }
        }

        // ---- requests from children; EOF means the child exited
        for (auto it = children.begin(); it != children.end();) {
            ChildRec *c = *it;
            if (!FD_ISSET(c->reqFd, &rfds)) { ++it; continue; }
            c->reader.fill(c->reqFd);
            std::string line;
            while (c->reader.next(line)) writeAll(c->respFd, handleRequest(c->pid, line) + "\n");
            if (c->reader.eof()) {
                forgetChild(c->pid);
                close(c->reqFd);
                close(c->respFd);
                delete c;
                it = children.erase(it);
            } else {
                ++it;
            }
        }
    }
}
