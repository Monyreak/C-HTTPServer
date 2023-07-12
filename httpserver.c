#include "asgn4_helper_funcs.h"
#include "connection.h"
#include "debug.h"
#include "queue.h"
#include "request.h"
#include "response.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <sys/stat.h>
#include <sys/file.h>

#define DEFAULT_THREAD 4;
#define RID            "Request-Id"
#define TEMP_FILENAME  ".temp_file"
queue_t *workq = NULL;

void handle_connection(void);
void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unspported(conn_t *);
void acquire_exclusive(int fd);
void acquire_shared(int fd);
int acquire_templock(void);
void release(int fd);

char *get_rid(conn_t *conn) {
    char *id = conn_get_header(conn, RID);
    if (id == NULL) {
        id = "0";
    }
    return id;
}

void audit_log(char *name, char *uri, char *id, int code) {
    fprintf(stderr, "%s,/%s,%d,%s\n", name, uri, code, id);
}

void usage(FILE *stream, char *exec) {
    fprintf(stream, "usage: %s [-t threads] <port> \n", exec);
}

void acquire_exclusive(int fd) {
    int rc = flock(fd, LOCK_EX);
    assert(rc == 0);
    debug("acquire_exclusive on %d", fd);
}

void acquire_shared(int fd) {
    int rc = flock(fd, LOCK_SH);
    assert(rc == 0);
    debug("acquire_share on %d", fd);
}

int acquire_templock(void) {
    int fd = open(TEMP_FILENAME, O_WRONLY);
    debug("opened %d", fd);
    acquire_exclusive(fd);
    return fd;
}

void release(int fd) {
    debug("release");
    flock(fd, LOCK_UN);
}

void init(int threads) {
    workq = queue_new(threads);
    creat(TEMP_FILENAME, 0600);
    assert(workq != NULL);
}

int main(int argc, char **argv) {
    int opt = 0;
    int threads = DEFAULT_THREAD;
    pthread_t *threadids;

    if (argc < 2) {
        warnx("wrong arguments: %s port_num", argv[0]);
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    while ((opt = getopt(argc, argv, "t:h")) != -1) {
        switch (opt) {
        case 't':
            threads = strtol(optarg, NULL, 10);
            if (threads <= 0) {
                errx(EXIT_FAILURE, "bad number of threads");
            }
            break;
        case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
        default: usage(stderr, argv[0]); return EXIT_FAILURE;
        }
    }
    if (optind >= argc) {
        warnx("wrong arguments: %s port_num", argv[0]);
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr = NULL;
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        warnx("invalud port number: %s", argv[1]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    Listener_Socket sock;

    if (listener_init(&sock, port) < 0) {
        warnx("Cannot open listener sock: %s", argv[0]);
        return EXIT_FAILURE;
    }

    threadids = malloc(sizeof(pthread_t) * threads);
    init(threads);

    for (int i = 0; i < threads; ++i) {
        int rc = pthread_create(threadids + i, NULL, (void *(*) (void *) ) handle_connection, NULL);

        if (rc != 0) {
            warnx("Cannot create %d pthreads", threads);
            return EXIT_FAILURE;
        }
    }
    while (1) {
        uintptr_t connfd = listener_accept(&sock);
        debug("accepted %lu\n", connfd);
        queue_push(workq, (void *) connfd);
    }

    queue_delete(&workq);
    free(threadids);

    return EXIT_SUCCESS;
}

void handle_connection(void) {
    while (true) {
        uintptr_t connfd = 0;
        conn_t *conn = NULL;

        queue_pop(workq, (void **) &connfd);
        debug("popped off %lu", connfd);

        conn = conn_new(connfd);

        const Response_t *res = conn_parse(conn);

        if (res != NULL) {
            conn_send_response(conn, res);
        } else {
            const Request_t *req = conn_get_request(conn);
            if (req == &REQUEST_GET) {
                handle_get(conn);
            } else if (req == &REQUEST_PUT) {
                handle_put(conn);
            } else {
                handle_unspported(conn);
            }
        }
        conn_delete(&conn);
        close(connfd);
    }
}

void handle_get(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    debug("GET %s", uri);

    int lock = acquire_templock();

    int fd = open(uri, O_RDONLY);
    struct stat st;
    off_t size = 0;

    int status_code = 0; // Initialize with success status
    int fail = 0;

    char *id = get_rid(conn);

    if (fd < 0) {
        fail = 1;

        switch (errno) {
        case EACCES:
            conn_send_response(conn, &RESPONSE_FORBIDDEN); // Send 403 forbidden
            status_code = 403;
            goto out;
        case ENOENT:
            conn_send_response(conn, &RESPONSE_NOT_FOUND); // Send 404 not found
            status_code = 404;
            goto out;
        case EISDIR:
            conn_send_response(conn, &RESPONSE_FORBIDDEN); // Send 403 forbidden
            status_code = 403;
            goto out;
        default:
            conn_send_response(
                conn, &RESPONSE_INTERNAL_SERVER_ERROR); // Send 500 internal server error
            status_code = 500;
            goto out;
        }
    } else {
        acquire_shared(fd);
        release(lock);
        close(lock);

        if (fstat(fd, &st) == 0) {
            size = st.st_size;

            const Response_t *res = conn_send_file(conn, fd, size);
            if (res == NULL) {
                // Send 200 OK
                // conn_send_response(conn, &RESPONSE_OK);
                status_code = 200;
                goto out;
            } else {
                conn_send_response(
                    conn, &RESPONSE_INTERNAL_SERVER_ERROR); // Send 500 internal server error
                status_code = 500;
                goto out;
            }
        } else {
            conn_send_response(
                conn, &RESPONSE_INTERNAL_SERVER_ERROR); // Send 500 internal server error
            status_code = 500;
            goto out;
        }
    }
out:
    audit_log("GET", uri, id, status_code); // Log the final status

    if (fail == 0) {
        release(fd);
        close(fd);
    } else {
        release(lock);
        close(lock);
    }
    return;
}

void handle_put(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    debug("PUT %s", uri);

    int lock = acquire_templock();
    bool exists = access(uri, F_OK) == 0;

    int fd = open(uri, O_CREAT | O_WRONLY, 0600);

    int status_code = 200; // Initialize with success status

    char *id = get_rid(conn);

    int fail = 0;

    if (fd < 0) {
        fail = 1;

        switch (errno) {
        case EACCES:
            conn_send_response(conn, &RESPONSE_FORBIDDEN); // Send 403 forbidden
            status_code = 403;
            goto out;
        case ENOENT:
            conn_send_response(conn, &RESPONSE_FORBIDDEN); // Send 403 forbidden
            status_code = 403;
            goto out;
        case EISDIR:
            conn_send_response(conn, &RESPONSE_FORBIDDEN); // Send 403 forbidden
            status_code = 403;
            goto out;
        default:
            conn_send_response(
                conn, &RESPONSE_INTERNAL_SERVER_ERROR); // Send 500 internal server error
            status_code = 500;
            goto out;
        }

    } else {
        acquire_exclusive(fd);
        release(lock);
        close(lock);

        int rc = ftruncate(fd, 0);
        assert(rc == 0);

        const Response_t *res = conn_recv_file(conn, fd);

        if ((res == NULL) && (exists)) {
            conn_send_response(conn, &RESPONSE_OK);
            status_code = 200;
        } else if ((res == NULL) && (!exists)) {
            conn_send_response(conn, &RESPONSE_CREATED);
            status_code = 201;
        } else {
            conn_send_response(
                conn, &RESPONSE_INTERNAL_SERVER_ERROR); // Send 500 internal server error
            status_code = 500;
        }
    }

out:
    audit_log("PUT", uri, id, status_code); // Log the final status

    if (fail == 0) {
        release(fd);
        close(fd);
    } else {
        release(lock);
        close(lock);
    }
    return;
}

void handle_unspported(conn_t *conn) {
    debug("handling unsupported request");
    conn_send_response(conn, &RESPONSE_NOT_IMPLEMENTED);
}
