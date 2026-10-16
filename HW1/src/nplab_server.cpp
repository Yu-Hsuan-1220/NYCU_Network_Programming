// nplab_server.cpp - Iterative TCP server for the 1A2B guessing game.
//
// Architecture (see "Server Algorithms" slides, Iterative TCP Server):
//   master socket: socket -> setsockopt(SO_REUSEADDR) -> bind -> listen
//   loop forever:  accept -> serve ONE player on the slave socket -> close
// Clients that connect while a game is in progress wait in the listen()
// backlog queue until the current player leaves.
//
// Protocol: plain text lines terminated by '\n'.
//   The server sends every piece of text the player sees, including the
//   prompts ("Enter your name: ", "nplab(<name>)> "); the client just relays.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static const int QLEN = 16;           // listen() backlog: waiting connections
static const int ROUNDS_TO_WIN = 3;   // game ends after three 4A0B
static const size_t MAX_LINE = 4096;  // guard against endless lines

// ---------------------------------------------------------------- helpers

static void usage(const char *prog) {
    printf("Usage: %s <port number 1024-65535>\n", prog);
    exit(1);
}

// Parse a port: digits only, value in [1024, 65535]. Returns -1 on error.
static int parsePort(const char *s) {
    if (*s == '\0' || strlen(s) > 5) return -1;
    for (const char *p = s; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    int port = atoi(s);
    return (port >= 1024 && port <= 65535) ? port : -1;
}

// send() may write fewer bytes than asked, so loop until everything is sent.
// MSG_NOSIGNAL: if the peer is gone we get EPIPE instead of a SIGPIPE kill.
static bool sendAll(int fd, const std::string &msg) {
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += n;
    }
    return true;
}

// TCP is a byte stream with no message boundaries (Client slides, "Some
// Important Facts"): one recv() may return half a line or several lines.
// LineReader buffers the bytes and hands out one complete '\n' line at a time.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    // Returns false when the peer closed the connection or an error occurred.
    bool readLine(std::string &line) {
        for (;;) {
            size_t pos = buf_.find('\n');
            if (pos != std::string::npos || buf_.size() >= MAX_LINE) {
                if (pos == std::string::npos) pos = buf_.size();
                line = buf_.substr(0, pos);
                buf_.erase(0, std::min(pos + 1, buf_.size()));
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char tmp[1024];
            ssize_t n = recv(fd_, tmp, sizeof(tmp), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;  // 0 = orderly close, <0 = error/reset
            buf_.append(tmp, n);
        }
    }

private:
    int fd_;
    std::string buf_;
};

// ------------------------------------------------------------- game logic

// Name rule: ^[A-Za-z][A-Za-z0-9_]{0,19}$
static bool validName(const std::string &name) {
    if (name.empty() || name.size() > 20) return false;
    if (!isalpha((unsigned char)name[0]) || !isascii((unsigned char)name[0]))
        return false;
    for (unsigned char c : name)
        if (!isascii(c) || !(isalnum(c) || c == '_')) return false;
    return true;
}

// A valid number: exactly 4 decimal digits, all different (leading 0 allowed).
static bool validNumber(const std::string &s) {
    if (s.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        for (size_t j = 0; j < i; ++j)
            if (s[i] == s[j]) return false;
    }
    return true;
}

static std::string newAnswer(std::mt19937 &rng) {
    std::string digits = "0123456789";
    std::shuffle(digits.begin(), digits.end(), rng);
    return digits.substr(0, 4);
}

// A = right digit, right place; B = right digit, wrong place.
static std::string score(const std::string &answer, const std::string &guess) {
    int a = 0, b = 0;
    for (int i = 0; i < 4; ++i) {
        if (guess[i] == answer[i]) ++a;
        else if (answer.find(guess[i]) != std::string::npos) ++b;
    }
    return std::to_string(a) + "A" + std::to_string(b) + "B";
}

static std::string statusLine(int guesses, int correct) {
    return "Guess count: " + std::to_string(guesses) +
           ", Correct count: " + std::to_string(correct) + "\n";
}

// Serve one connected player until quit / three wins / disconnect.
static void servePlayer(int fd, std::mt19937 &rng) {
    LineReader reader(fd);
    std::string line;

    // ---- 1. ask for a valid player name
    std::string name;
    for (;;) {
        if (!sendAll(fd, "Enter your name: ")) return;
        if (!reader.readLine(line)) return;  // left before giving a name
        if (validName(line)) { name = line; break; }
        if (!sendAll(fd, "Invalid player name.\n")) return;
    }
    printf("Player name: %s\n", name.c_str());

    // ---- 2. play
    std::string answer = newAnswer(rng);
    printf("Answer for this round: %s.\n", answer.c_str());

    int guesses = 0, correct = 0;
    std::string prompt = "nplab(" + name + ")> ";
    bool ok = sendAll(fd, "Welcome, " + name + "!\n" + prompt);

    while (ok && reader.readLine(line)) {
        std::istringstream iss(line);
        std::vector<std::string> args;
        for (std::string w; iss >> w;) args.push_back(w);

        std::string reply;
        if (args.empty()) {
            // blank line: just show the prompt again
        } else if (args[0] == "guess") {
            if (args.size() != 2 || !validNumber(args[1])) {
                reply = "Usage: guess <4-digit number>\n";
            } else {
                ++guesses;
                printf("%s guessed %s.\n", name.c_str(), args[1].c_str());
                std::string result = score(answer, args[1]);
                reply = result + "\n";
                if (result == "4A0B") {
                    ++correct;
                    if (correct >= ROUNDS_TO_WIN) {
                        sendAll(fd, reply + statusLine(guesses, correct) +
                                        "Goodbye, " + name + "!\n");
                        break;
                    }
                    answer = newAnswer(rng);
                    printf("Answer for this round: %s.\n", answer.c_str());
                }
            }
        } else if (args[0] == "status") {
            reply = statusLine(guesses, correct);
        } else if (args[0] == "quit") {
            sendAll(fd, statusLine(guesses, correct) + "Goodbye, " + name + "!\n");
            break;
        } else {
            reply = "Unknown command: " + args[0] + "\n";
        }
        ok = sendAll(fd, reply + prompt);
    }
    // quit, three wins, or the client disconnected
    printf("%s left.\n", name.c_str());
}

// -------------------------------------------------------------------- main

int main(int argc, char *argv[]) {
    if (argc != 2) usage(argv[0]);
    int port = parsePort(argv[1]);
    if (port < 0) usage(argv[0]);

    setvbuf(stdout, nullptr, _IOLBF, 0);  // every line appears immediately
    signal(SIGPIPE, SIG_IGN);             // a dead client must not kill us

    // ---- master socket: socket -> SO_REUSEADDR -> bind -> listen
    int msock = socket(AF_INET, SOCK_STREAM, 0);
    if (msock < 0) { perror("socket"); return 1; }

    // Without SO_REUSEADDR, restarting the server soon after it stops fails
    // with "Address already in use" while old connections sit in TIME_WAIT.
    int on = 1;
    if (setsockopt(msock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt"); return 1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);  // accept on every interface
    sin.sin_port = htons(port);               // host -> network byte order
    if (bind(msock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(msock, QLEN) < 0) { perror("listen"); return 1; }

    printf("Server listening on port %d.\n", port);

    std::mt19937 rng(std::random_device{}());

    // ---- iterative server: one client per iteration
    for (;;) {
        printf("Waiting for a new client...\n");
        struct sockaddr_in fsin;
        socklen_t alen = sizeof(fsin);
        int ssock = accept(msock, (struct sockaddr *)&fsin, &alen);
        if (ssock < 0) {
            if (errno != EINTR) perror("accept");
            continue;
        }
        printf("Client connected.\n");
        servePlayer(ssock, rng);
        close(ssock);
    }
}
