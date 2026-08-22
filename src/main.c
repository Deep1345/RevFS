/*
 * RevFS — Versioned Distributed File Storage System
 *
 * Entry point and CLI argument parsing.
 */

#include "revfs.h"

static void print_usage(const char *program)
{
    printf("Usage: %s <command> [options]\n", program);
    printf("\n");
    printf("Commands:\n");
    printf("  upload   <file> [--host H] [--port P]             Upload a file (local or remote)\n");
    printf("  download <file> <output> [--version N] [--host H] [--port P]\n");
    printf("                                                    Download a file (local or remote)\n");
    printf("  history  <file> [--host H] [--port P]             Show version history\n");
    printf("  restore  <file> <version>                         Restore a specific version\n");
    printf("  list     [--host H] [--port P]                    List all stored files\n");
    printf("  ping     [msg] [--host H] [--port P]              Ping a RevFS server\n");
    printf("  server   [port]                                   Start TCP server (default: 9000)\n");
    printf("  stats                                             Show storage statistics\n");
    printf("\n");
    printf("Options:\n");
    printf("  --host <host>                Remote RevFS server host (default: 127.0.0.1)\n");
    printf("  --port <port>                Remote RevFS server port (default: 9000)\n");
    printf("  --version <N>                Specific file version number\n");
    printf("  --help                       Show this help message\n");
    printf("  --version                    Show version information\n");
}

static void print_version(void)
{
    printf("%s version %s\n", REVFS_NAME, REVFS_VERSION);
}

/* Helper to extract --host and --port from CLI argument list */
static void parse_network_flags(int argc, char *argv[], const char **host_out, int *port_out)
{
    *host_out = NULL;
    *port_out = REVFS_DEFAULT_PORT;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            *host_out = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            *port_out = atoi(argv[i + 1]);
            i++;
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "upload") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s upload <file> [--host H] [--port P]\n", argv[0]);
            return EXIT_FAILURE;
        }
        const char *filepath = argv[2];
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int version = revfs_client_upload(host, port, filepath);
            return (version > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int version = revfs_upload(filepath);
            return (version > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "download") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s download <file> <output> [--version N] [--host H] [--port P]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename    = argv[2];
        const char *output_path = argv[3];
        int version = -1;  /* default: latest */
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;

        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) {
                version = atoi(argv[i + 1]);
                if (version < 1) {
                    fprintf(stderr, "Invalid version: %s\n", argv[i + 1]);
                    return EXIT_FAILURE;
                }
                i++;
            } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                host = argv[i + 1];
                i++;
            } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = atoi(argv[i + 1]);
                i++;
            }
        }

        if (host) {
            int rc = revfs_client_download(host, port, filename, version, output_path);
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_download(filename, version, output_path);
            return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "history") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s history <file> [--host H] [--port P]\n", argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename = argv[2];
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int rc = revfs_client_history(host, port, filename);
            return (rc > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_history(filename);
            return (rc > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "restore") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s restore <file> <version>\n", argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename = argv[2];
        int version = atoi(argv[3]);
        if (version < 1) {
            fprintf(stderr, "Invalid version: %s\n", argv[3]);
            return EXIT_FAILURE;
        }
        int new_ver = revfs_restore(filename, version);
        return (new_ver > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "list") == 0) {
        const char *host = NULL;
        int port = REVFS_DEFAULT_PORT;
        parse_network_flags(argc, argv, &host, &port);

        if (host) {
            int rc = revfs_client_list(host, port);
            return (rc >= 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        } else {
            int rc = revfs_list_files();
            return (rc >= 0) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    if (strcmp(cmd, "ping") == 0) {
        const char *host = "127.0.0.1";
        int port = REVFS_DEFAULT_PORT;
        const char *msg = (argc >= 3 && argv[2][0] != '-') ? argv[2] : NULL;

        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
                host = argv[i + 1];
                i++;
            } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
                port = atoi(argv[i + 1]);
                i++;
            }
        }

        int sock = revfs_client_connect(host, port);
        if (sock < 0) {
            return EXIT_FAILURE;
        }
        char resp[256];
        if (revfs_client_ping(sock, msg, resp, sizeof(resp)) < 0) {
            fprintf(stderr, "revfs: ping failed\n");
            revfs_client_disconnect(sock);
            return EXIT_FAILURE;
        }
        printf("%s\n", resp);
        revfs_client_disconnect(sock);
        return EXIT_SUCCESS;
    }

    if (strcmp(cmd, "server") == 0 || strcmp(cmd, "serve") == 0) {
        int port = REVFS_DEFAULT_PORT;
        if (argc >= 3) {
            port = atoi(argv[2]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[2]);
                return EXIT_FAILURE;
            }
        }
        int rc = revfs_server_start(port);
        return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "stats") == 0) {
        fprintf(stderr, "stats: not yet implemented\n");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    fprintf(stderr, "Run '%s --help' for usage.\n", argv[0]);
    return EXIT_FAILURE;
}
