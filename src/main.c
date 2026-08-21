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
    printf("  upload   <file>              Upload a file to RevFS\n");
    printf("  download <file> <output>     Download a file from RevFS\n");
    printf("  history  <file>              Show version history\n");
    printf("  restore  <file> <version>    Restore a specific version\n");
    printf("  list                         List all stored files\n");
    printf("  server   [port]              Start TCP server (default: 9000)\n");
    printf("  stats                        Show storage statistics\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help                       Show this help message\n");
    printf("  --version                    Show version information\n");
}

static void print_version(void)
{
    printf("%s version %s\n", REVFS_NAME, REVFS_VERSION);
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
            fprintf(stderr, "Usage: %s upload <file>\n", argv[0]);
            return EXIT_FAILURE;
        }
        int version = revfs_upload(argv[2]);
        return (version > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "download") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s download <file> <output> [--version N]\n",
                    argv[0]);
            return EXIT_FAILURE;
        }
        const char *filename    = argv[2];
        const char *output_path = argv[3];
        int version = -1;  /* default: latest */

        /* Check for optional --version flag */
        if (argc >= 6 && strcmp(argv[4], "--version") == 0) {
            version = atoi(argv[5]);
            if (version < 1) {
                fprintf(stderr, "Invalid version: %s\n", argv[5]);
                return EXIT_FAILURE;
            }
        }

        int rc = revfs_download(filename, version, output_path);
        return (rc == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(cmd, "history") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s history <file>\n", argv[0]);
            return EXIT_FAILURE;
        }
        int rc = revfs_history(argv[2]);
        return (rc > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
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
        int rc = revfs_list_files();
        return (rc >= 0) ? EXIT_SUCCESS : EXIT_FAILURE;
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
