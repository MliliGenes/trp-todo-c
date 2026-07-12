/*
 * main.c — Minimalist C Task Manager (Phase 1 + Phase 2)
 *
 * Compile:
 *   gcc main.c -o todo -lsqlite3 -lreadline -pthread
 *
 * Run:
 *   ./todo [port]        (port defaults to 8080)
 *
 * Commands (CLI):
 *   /add <description>   add a new task
 *   /delete <id>          delete a task by id
 *   /check <id>            toggle a task's completed status
 *   /list, /show          show all tasks
 *   /help                   show this help
 *   /quit, /exit          exit the app
 *
 * While the CLI runs, a background HTTP thread serves the same task
 * list as a rendered HTML page at http://localhost:<port>/ — every
 * request re-renders straight from the live in-memory list, so a
 * browser refresh always reflects whatever you just did in the CLI.
 *
 * Design:
 *   The linked list is the in-memory source of truth for the running
 *   session (fast reads, no round-trip to disk to print/toggle). SQLite
 *   is the persistence layer: every mutation goes db-then-list (see
 *   comments) so the two never drift apart while the app is running.
 *   On startup, the list is rebuilt from whatever is in the database,
 *   so state survives restarts.
 *
 *   Now that a second thread (the HTTP server) reads the list while the
 *   main thread (the CLI) mutates it, every access to the list — reads
 *   included — goes through a single global mutex, g_list_mutex. The
 *   list has exactly one owner process and one instance, so a single
 *   global lock is simpler than threading a mutex pointer through every
 *   function signature, and is enough for this project's scale.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#include <readline/readline.h>
#include <readline/history.h>

#define DB_PATH "tasks.db"
#define DEFAULT_PORT 8080
#define SERVER_BACKLOG 16

/* ---------- Data structures ---------- */

typedef struct Task {
    int id;
    char *description;
    int is_completed;
    time_t created_at;
    struct Task *next;
    struct Task *prev;
} Task;

typedef struct TaskList {
    Task *head;
    Task *tail;
    int count;
} TaskList;

/* Guards every read and write of the TaskList once the HTTP server
 * thread is running alongside the CLI thread. See design note above. */
static pthread_mutex_t g_list_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Linked list layer ---------- */

TaskList *list_create(void);
Task *list_add(TaskList *list, int id, const char *desc, int is_completed, time_t created_at);
Task *list_find(TaskList *list, int id);
int list_delete(TaskList *list, int id);
void list_print(const TaskList *list);
void list_free(TaskList *list);

/* ---------- SQLite layer ---------- */

sqlite3 *db_init(const char *path);
void db_close(sqlite3 *db);
int db_insert_task(sqlite3 *db, const char *desc, time_t created_at);
int db_toggle_task(sqlite3 *db, int id, int new_status);
int db_delete_task(sqlite3 *db, int id);
TaskList *db_load_all(sqlite3 *db);

/* ---------- Parsing helpers ---------- */

static char *skip_spaces(char *s);
static void parse_line(char *line, char **cmd, char **arg);
static int parse_id(const char *arg, int *out);
static void format_timestamp(time_t t, char *buf, size_t len);

/* ---------- Command handlers ---------- */

static void cmd_add(sqlite3 *db, TaskList *list, const char *desc);
static void cmd_delete(sqlite3 *db, TaskList *list, const char *arg);
static void cmd_check(sqlite3 *db, TaskList *list, const char *arg);
static void print_help(void);

/* ---------- HTTP server layer ---------- */

static char *html_escape(const char *s);
static char *render_html(TaskList *list);
static const char *http_path_from_request(const char *req);
static int send_file_response(int client_fd, const char *content_type, const char *path);
static int send_all(int fd, const char *buf, size_t len);
static void handle_client(int client_fd, TaskList *list);
static void *server_thread_main(void *arg);
static int server_start(TaskList *list, int port);
static void server_stop(void);

/* ================= Linked list layer ================= */

TaskList *list_create(void) {
    TaskList *list = malloc(sizeof(TaskList));
    if (!list)
        return NULL;
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    return list;
}

Task *list_add(TaskList *list, int id, const char *desc, int is_completed, time_t created_at) {
    Task *t = malloc(sizeof(Task));
    if (!t)
        return NULL;

    t->id = id;
    t->description = strdup(desc);
    t->is_completed = is_completed;
    t->created_at = created_at;
    t->next = NULL;
    t->prev = list->tail;

    if (list->tail)
        list->tail->next = t;
    else
        list->head = t;

    list->tail = t;
    list->count++;
    return t;
}

Task *list_find(TaskList *list, int id) {
    for (Task *t = list->head; t; t = t->next) {
        if (t->id == id)
            return t;
    }
    return NULL;
}

int list_delete(TaskList *list, int id) {
    Task *t = list_find(list, id);
    if (!t)
        return -1;

    if (t->prev)
        t->prev->next = t->next;
    else
        list->head = t->next;

    if (t->next)
        t->next->prev = t->prev;
    else
        list->tail = t->prev;

    free(t->description);
    free(t);
    list->count--;
    return 0;
}

void list_print(const TaskList *list) {
    if (!list->head) {
        printf("No tasks yet. Use /add <description> to create one.\n");
        return;
    }
    for (Task *t = list->head; t; t = t->next) {
        char ts[20];
        format_timestamp(t->created_at, ts, sizeof(ts));
        printf("  [%s] #%-3d %-30s (%s)\n", t->is_completed ? "x" : " ", t->id, t->description, ts);
    }
}

void list_free(TaskList *list) {
    if (!list)
        return;
    Task *t = list->head;
    while (t) {
        Task *next = t->next;
        free(t->description);
        free(t);
        t = next;
    }
    free(list);
}

/* ================= SQLite layer ================= */

sqlite3 *db_init(const char *path) {
    sqlite3 *db;

    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  description TEXT NOT NULL,"
        "  is_completed INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))"
        ");";

    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "Failed to create table: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

void db_close(sqlite3 *db) {
    if (db)
        sqlite3_close(db);
}

int db_insert_task(sqlite3 *db, const char *desc, time_t created_at) {
    const char *sql = "INSERT INTO tasks (description, created_at) VALUES (?, ?);";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, desc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)created_at);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Insert failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    return (int)sqlite3_last_insert_rowid(db);
}

int db_toggle_task(sqlite3 *db, int id, int new_status) {
    const char *sql = "UPDATE tasks SET is_completed = ? WHERE id = ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, new_status);
    sqlite3_bind_int(stmt, 2, id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? 0 : -1;
}

int db_delete_task(sqlite3 *db, int id) {
    const char *sql = "DELETE FROM tasks WHERE id = ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE ? 0 : -1;
}

TaskList *db_load_all(sqlite3 *db) {
    TaskList *list = list_create();
    const char *sql = "SELECT id, description, is_completed, created_at FROM tasks ORDER BY id;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Load failed: %s\n", sqlite3_errmsg(db));
        return list;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *desc = sqlite3_column_text(stmt, 1);
        int done = sqlite3_column_int(stmt, 2);
        time_t created_at = (time_t)sqlite3_column_int64(stmt, 3);
        list_add(list, id, (const char *)desc, done, created_at);
    }

    sqlite3_finalize(stmt);
    return list;
}

/* ================= Parsing helpers ================= */

static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* Splits "line" in place into a command token and the remainder.
 * "/add buy milk"  ->  cmd="/add"   arg="buy milk"
 * "/delete 3"      ->  cmd="/delete" arg="3"
 */
static void parse_line(char *line, char **cmd, char **arg) {
    line = skip_spaces(line);
    *cmd = line;

    while (*line && *line != ' ' && *line != '\t')
        line++;

    if (*line) {
        *line = '\0';
        line++;
        line = skip_spaces(line);
    }

    *arg = line;
}

static int parse_id(const char *arg, int *out) {
    if (*arg == '\0')
        return -1;

    char *end;
    long val = strtol(arg, &end, 10);

    if (*end != '\0' || val <= 0)
        return -1;

    *out = (int)val;
    return 0;
}

/* "2026-07-12 14:32", local time. Shared by the CLI listing and the
 * HTML renderer so the two never format timestamps differently. */
static void format_timestamp(time_t t, char *buf, size_t len) {
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(buf, len, "%Y-%m-%d %H:%M", &tm_buf);
}

/* ================= Command handlers ================= */

static void cmd_add(sqlite3 *db, TaskList *list, const char *desc) {
    if (*desc == '\0') {
        printf("Usage: /add <description>\n");
        return;
    }

    /* One timestamp, used for both the DB row and the in-memory node,
     * so they can never disagree about when the task was created. */
    time_t now = time(NULL);

    /* DB first: we need the autoincrement id before we can create the
     * in-memory node, since the node's id must match the DB row. */
    int id = db_insert_task(db, desc, now);
    if (id < 0) {
        printf("Failed to add task to database.\n");
        return;
    }

    pthread_mutex_lock(&g_list_mutex);
    list_add(list, id, desc, 0, now);
    pthread_mutex_unlock(&g_list_mutex);

    printf("Added task #%d: %s\n", id, desc);
}

static void cmd_delete(sqlite3 *db, TaskList *list, const char *arg) {
    int id;

    if (parse_id(arg, &id) != 0) {
        printf("Usage: /delete <id>\n");
        return;
    }

    pthread_mutex_lock(&g_list_mutex);
    int exists = (list_find(list, id) != NULL);
    pthread_mutex_unlock(&g_list_mutex);

    if (!exists) {
        printf("No task with id %d.\n", id);
        return;
    }

    if (db_delete_task(db, id) != 0) {
        printf("Failed to delete task in database.\n");
        return;
    }

    pthread_mutex_lock(&g_list_mutex);
    list_delete(list, id);
    pthread_mutex_unlock(&g_list_mutex);

    printf("Deleted task #%d.\n", id);
}

static void cmd_check(sqlite3 *db, TaskList *list, const char *arg) {
    int id;

    if (parse_id(arg, &id) != 0) {
        printf("Usage: /check <id>\n");
        return;
    }

    pthread_mutex_lock(&g_list_mutex);
    Task *t = list_find(list, id);
    int new_status = t ? !t->is_completed : -1;
    pthread_mutex_unlock(&g_list_mutex);

    if (!t) {
        printf("No task with id %d.\n", id);
        return;
    }

    if (db_toggle_task(db, id, new_status) != 0) {
        printf("Failed to update task in database.\n");
        return;
    }

    pthread_mutex_lock(&g_list_mutex);
    t->is_completed = new_status;
    pthread_mutex_unlock(&g_list_mutex);

    printf("Task #%d marked as %s.\n", id, new_status ? "done" : "not done");
}

static void print_help(void) {
    printf(
        "Commands:\n"
        "  /add <description>   add a new task\n"
        "  /delete <id>          delete a task by id\n"
        "  /check <id>            toggle a task's completed status\n"
        "  /list, /show          show all tasks\n"
        "  /help                   show this help\n"
        "  /quit, /exit          exit the app\n"
    );
}

/* ================= HTTP server layer ================= */

/* Task descriptions are free-form user input and get dropped straight
 * into HTML, so they're escaped first — otherwise a description like
 * "<script>..." would break the page (or worse) instead of just
 * displaying as text. */
static char *html_escape(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len * 6 + 1); /* worst case: every char -> &quot; */
    if (!out)
        return NULL;

    char *p = out;
    for (const char *c = s; *c; c++) {
        switch (*c) {
            case '&': memcpy(p, "&amp;", 5); p += 5; break;
            case '<': memcpy(p, "&lt;", 4); p += 4; break;
            case '>': memcpy(p, "&gt;", 4); p += 4; break;
            case '"': memcpy(p, "&quot;", 6); p += 6; break;
            case '\'': memcpy(p, "&#39;", 5); p += 5; break;
            default: *p++ = *c;
        }
    }
    *p = '\0';
    return out;
}

/* Builds the full HTML page from the current list state. Caller owns
 * the returned buffer and must free() it. Uses open_memstream so the
 * buffer grows to fit however many tasks exist, no fixed-size guess. */
static char *render_html(TaskList *list) {
    char *buf = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&buf, &size);
    if (!f)
        return NULL;

    fprintf(f,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Tasks</title>"
        "<link rel=\"stylesheet\" href=\"/styles.css\">"
        "</head><body>"
        "<main class=\"app\"><h1>Tasks</h1><ul class=\"task-list\">");

    pthread_mutex_lock(&g_list_mutex);

    if (!list->head) {
        fprintf(f, "<li class=\"task-item empty\">No tasks yet.</li>");
    } else {
        for (Task *t = list->head; t; t = t->next) {
            char ts[20];
            format_timestamp(t->created_at, ts, sizeof(ts));

            char *esc = html_escape(t->description);
            fprintf(f,
                "<li class=\"task-item %s\"><span class=\"task-text\"><span class=\"id\">#%d</span><span class=\"desc\">%s</span></span>"
                "<span class=\"meta\">%s</span></li>",
                t->is_completed ? "done" : "", t->id, esc ? esc : "", ts);
            free(esc);
        }
    }

    pthread_mutex_unlock(&g_list_mutex);

    fprintf(f, "</ul></main></body></html>");
    fclose(f);
    return buf;
}

static const char *http_path_from_request(const char *req) {
    static char path[256];
    path[0] = '\0';

    if (sscanf(req, "GET %255s", path) != 1)
        return "/";

    return path;
}

static int send_file_response(int client_fd, const char *content_type, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, size);

    if (send_all(client_fd, header, (size_t)header_len) != 0) {
        fclose(f);
        return -1;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(client_fd, buf, n) != 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/* send() isn't guaranteed to write the whole buffer in one call. */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* HTTP/1.0-style: read whatever the client sent (we don't need to
 * parse it, every path gets the same page), render fresh from the
 * live list, respond, close. No keep-alive, no persistent connection. */
static void handle_client(int client_fd, TaskList *list) {
    char req[2048];
    recv(client_fd, req, sizeof(req) - 1, 0); /* discard; single-page server */

    req[sizeof(req) - 1] = '\0';
    const char *path = http_path_from_request(req);

    if (strcmp(path, "/styles.css") == 0) {
        if (send_file_response(client_fd, "text/css; charset=utf-8", "styles.css") != 0) {
            const char *err = "HTTP/1.0 404 Not Found\r\n\r\n";
            send_all(client_fd, err, strlen(err));
        }
        close(client_fd);
        return;
    }

    char *html = render_html(list);
    if (!html) {
        const char *err = "HTTP/1.0 500 Internal Server Error\r\n\r\n";
        send_all(client_fd, err, strlen(err));
        close(client_fd);
        return;
    }

    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        strlen(html));

    send_all(client_fd, header, (size_t)header_len);
    send_all(client_fd, html, strlen(html));

    free(html);
    close(client_fd);
}

typedef struct ServerContext {
    TaskList *list;
    int port;
    int server_fd;
} ServerContext;

static ServerContext g_server_ctx;
static pthread_t g_server_thread;
static _Atomic int g_server_running = 0;
static _Atomic int g_server_started = 0;

static void *server_thread_main(void *arg) {
    ServerContext *ctx = (ServerContext *)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[server] socket");
        return NULL;
    }

    ctx->server_fd = server_fd;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)ctx->port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[server] bind (port already in use?)");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, SERVER_BACKLOG) < 0) {
        perror("[server] listen");
        close(server_fd);
        ctx->server_fd = -1;
        return NULL;
    }

    printf("[server] listening on http://localhost:%d\n", ctx->port);

    while (g_server_running) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready = select(server_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (!g_server_running)
            break;

        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("[server] select");
            continue;
        }

        if (ready == 0)
            continue;

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            if (!g_server_running)
                break;
            perror("[server] accept");
            continue;
        }
        handle_client(client_fd, ctx->list);
    }

    close(server_fd);
    ctx->server_fd = -1;
    return NULL;
}

/* Spins up the HTTP server on a background thread so main can stop it
 * cleanly before freeing the shared task list and database. */
static int server_start(TaskList *list, int port) {
    g_server_ctx.list = list;
    g_server_ctx.port = port;
    g_server_ctx.server_fd = -1;
    g_server_running = 1;

    if (pthread_create(&g_server_thread, NULL, server_thread_main, &g_server_ctx) != 0) {
        fprintf(stderr, "Failed to start server thread.\n");
        g_server_running = 0;
        return -1;
    }
    g_server_started = 1;
    return 0;
}

static void server_stop(void) {
    if (!g_server_started)
        return;

    g_server_running = 0;
    pthread_join(g_server_thread, NULL);
    g_server_started = 0;
}

/* ================= Main loop ================= */

int main(int argc, char **argv) {
    /* A client closing its connection mid-response would otherwise
     * raise SIGPIPE and kill the whole process; ignore it so send()
     * just returns -1 instead, which handle_client already checks for. */
    signal(SIGPIPE, SIG_IGN);

    int port = DEFAULT_PORT;
    if (argc > 1) {
        char *end;
        long val = strtol(argv[1], &end, 10);
        if (*end != '\0' || val <= 0 || val > 65535) {
            fprintf(stderr, "Invalid port '%s'. Usage: %s [port]\n", argv[1], argv[0]);
            return 1;
        }
        port = (int)val;
    }

    sqlite3 *db = db_init(DB_PATH);
    if (!db) {
        fprintf(stderr, "Could not initialize database. Exiting.\n");
        return 1;
    }

    TaskList *list = db_load_all(db);
    printf("Loaded %d task(s) from %s.\n", list->count, DB_PATH);

    if (server_start(list, port) != 0)
        fprintf(stderr, "Continuing without the web server.\n");

    print_help();

    char *line;
    while ((line = readline("todo> ")) != NULL) {
        if (*line)
            add_history(line);

        char *cmd, *arg;
        parse_line(line, &cmd, &arg);

        if (*cmd == '\0') {
            /* empty input, ignore */
        } else if (strcmp(cmd, "/add") == 0) {
            cmd_add(db, list, arg);
        } else if (strcmp(cmd, "/delete") == 0) {
            cmd_delete(db, list, arg);
        } else if (strcmp(cmd, "/check") == 0) {
            cmd_check(db, list, arg);
        } else if (strcmp(cmd, "/list") == 0 || strcmp(cmd, "/show") == 0) {
            pthread_mutex_lock(&g_list_mutex);
            list_print(list);
            pthread_mutex_unlock(&g_list_mutex);
        } else if (strcmp(cmd, "/help") == 0) {
            print_help();
        } else if (strcmp(cmd, "/quit") == 0 || strcmp(cmd, "/exit") == 0) {
            free(line);
            break;
        } else {
            printf("Unknown command '%s'. Type /help for a list.\n", cmd);
        }

        free(line);
    }

    server_stop();
    list_free(list);
    db_close(db);
    printf("Goodbye.\n");
    return 0;
}
