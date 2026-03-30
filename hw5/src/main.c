#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "client_registry.h"
#include "maze.h"
#include "player.h"
#include "debug.h"
#include "server.h"

static void terminate(int status);

static char **template_mem = NULL;

static char *default_maze[] = {
  "******************************",
  "***** %%%%%%%%% &&&&&&&&&&& **",
  "***** %%%%%%%%%        $$$$  *",
  "*           $$$$$$ $$$$$$$$$ *",
  "*##########                  *",
  "*########## @@@@@@@@@@@@@@@@@*",
  "*           @@@@@@@@@@@@@@@@@*",
  "******************************",
  NULL
};

static volatile sig_atomic_t hup_seen = 0;
static int listenfd = -1;

static void hup_handler(int sig) {
    (void)sig;
    hup_seen = 1;

    if (listenfd >= 0) {
        close(listenfd);
        listenfd = -1;
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -p <port> [-t <template_file>]\n", prog);
    exit(EXIT_FAILURE);
}

static int open_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0 || listen(fd, 128) < 0) {
        perror("bind/listen");
        exit(EXIT_FAILURE);
    }

    return fd;
}

static char **load_template(const char *path) {
    FILE *fp = fopen(path, "r");

    if (!fp) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    size_t n = 0, cap = 16;
    char **lines = calloc(cap+1, sizeof(char*));
    char *buf = NULL;
    size_t len = 0;

    while (getline(&buf, &len, fp) != -1) {
        size_t L = strlen(buf);

        if (L && (buf[L-1] == '\n' || buf[L-1] == '\r')) buf[--L] = '\0';
        if (n == cap) {
            cap <<= 1;
            lines = realloc(lines, (cap+1)*sizeof(char*));
        }
        lines[n++] = strdup(buf);
    }

    free(buf);
    fclose(fp);
    lines[n] = NULL;
    return lines;
}

int main(int argc, char* argv[]){

    signal(SIGPIPE, SIG_IGN);
    // Option processing should be performed here.
    // Option '-p <port>' is required in order to specify the port number
    // on which the server should listen.

    uint16_t port = 0;
    char *template_file = NULL;

    int opt;

    while ((opt = getopt(argc, argv, "p:t:")) != -1) {
        switch (opt) {
        case 'p': {
                long v = strtol(optarg, NULL, 10);
                if (v <= 0 || v > 65535) usage(argv[0]);
                port = (uint16_t)v;
                break;
            }
        case 't':
            template_file = optarg;
            break;
        default:
            usage(argv[0]);
        }
    }

    if (port == 0) usage(argv[0]);

    // Perform required initializations of the client_registry,
    // maze, and player modules.
    client_registry = creg_init();

    if (client_registry == NULL) {
        fprintf(stderr, "creg_init() failed - cannot start server\n");
        exit(EXIT_FAILURE);
    }
    template_mem = template_file ? load_template(template_file) : default_maze;
    maze_init(template_mem);
    player_init();

    #ifdef DEBUG
        debug_show_maze = 1;  // Show the maze after each packet.
    #else
        debug_show_maze = 0;
    #endif
    struct sigaction sa = {0};
    sa.sa_handler = hup_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }

    listenfd = open_listener(port);

    while (!hup_seen) {
        int *connfdp = malloc(sizeof(int));
        if (!connfdp) {
            perror("malloc");
            break;
        }
        *connfdp = accept(listenfd, NULL, NULL);

        if (*connfdp < 0) {
            free(connfdp);
            if (errno == EINTR) continue;
            if (errno == EBADF) break;
            perror("accept");
            break;
        }
        pthread_t tid;

        if (pthread_create(&tid, NULL, mzw_client_service, connfdp) == 0) {
            pthread_detach(tid);
        } else {
            perror("pthread_create");
            close(*connfdp);
            free(connfdp);
        }
    }

    terminate(EXIT_SUCCESS);
    return 0;
}

/*
 * Function called to cleanly shut down the server.
 */
void terminate(int status) {
    // Shutdown all client connections.
    // This will trigger the eventual termination of service threads.
    creg_shutdown_all(client_registry);

    debug("Waiting for service threads to terminate...");
    creg_wait_for_empty(client_registry);
    debug("All service threads terminated.");

    // Finalize modules.
    creg_fini(client_registry);
    player_fini();
    maze_fini();

    if (template_mem && template_mem != default_maze) {
        for (char **p = template_mem; *p; p++) free(*p);
        free(template_mem);
    }

    debug("MazeWar server terminating");
    exit(status);
}
