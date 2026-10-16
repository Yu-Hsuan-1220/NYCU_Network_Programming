// nplab_client.cpp - TCP client for the 1A2B guessing game.
//
// Follows the TCP Client Algorithm (Client slides):
//   resolve host/port (getaddrinfo) -> socket -> connect -> read/write -> close
// The client does not bind(): TCP picks a free local port automatically.
//
// The server sends all text the player sees (prompts included), so the
// client is a relay: keyboard -> server, server -> screen. It uses select()
// (I/O multiplexing, Server slides) to wait on stdin and the socket at once,
// so it notices immediately when the server closes the connection.

#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void usage(const char *prog) {
    printf("Usage: %s <server IP> <port number 1024-65535>\n", prog);
    exit(1);
}

static bool validPort(const char *s) {
    if (*s == '\0' || strlen(s) > 5) return false;
    for (const char *p = s; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    int port = atoi(s);
    return port >= 1024 && port <= 65535;
}

// write() may write fewer bytes than asked, so loop until done.
static bool writeAll(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buf += n;
        len -= n;
    }
    return true;
}

// Resolve host (IP or hostname) and port, then connect. Returns socket or -1.
static int connectTCP(const char *host, const char *port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        // IPv4
    hints.ai_socktype = SOCK_STREAM;  // TCP

    int err = getaddrinfo(host, port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(err));
        return -1;
    }

    int s = -1;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        if (connect(s, p->ai_addr, p->ai_addrlen) == 0) break;  // success
        close(s);
        s = -1;
    }
    freeaddrinfo(res);

    if (s < 0) fprintf(stderr, "Cannot connect to %s:%s\n", host, port);
    return s;
}

int main(int argc, char *argv[]) {
    if (argc != 3 || !validPort(argv[2])) usage(argv[0]);

    signal(SIGPIPE, SIG_IGN);  // server gone -> write() returns EPIPE instead

    int sock = connectTCP(argv[1], argv[2]);
    if (sock < 0) return 1;

    bool stdinOpen = true;
    char buf[4096];

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        if (stdinOpen) FD_SET(STDIN_FILENO, &rfds);

        // nfds = highest fd + 1; NULL timeout = wait forever
        if (select(sock + 1, &rfds, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // Server -> screen. recv() == 0 means the server closed the
        // connection (after quit / three wins), so the client exits.
        if (FD_ISSET(sock, &rfds)) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            writeAll(STDOUT_FILENO, buf, n);
        }

        // Keyboard -> server. On EOF (Ctrl+D) we half-close the socket:
        // the server sees EOF and ends the session, and we keep reading
        // until it closes its side.
        if (stdinOpen && FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                stdinOpen = false;
                shutdown(sock, SHUT_WR);
            } else if (!writeAll(sock, buf, n)) {
                break;
            }
        }
    }

    close(sock);
    return 0;
}
