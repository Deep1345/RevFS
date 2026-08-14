#ifndef REVFS_H
#define REVFS_H

#define REVFS_VERSION "0.1.0"
#define REVFS_NAME    "RevFS"

/* Default chunk size: 4 MB */
#define REVFS_CHUNK_SIZE (4 * 1024 * 1024)

/* Default server port */
#define REVFS_DEFAULT_PORT 9000

/* Default data directory */
#define REVFS_DATA_DIR "data"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#endif /* REVFS_H */
