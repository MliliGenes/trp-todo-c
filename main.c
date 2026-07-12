/*
 * main.c — Minimalist C Task Manager (Phase 1: CLI + SQLite persistence)
 *
 * Compile:
 *   gcc main.c -o todo -lsqlite3 -lreadline
 *
 * Run:
 *   ./todo
 *
 * Commands:
 *   /add <description>   add a new task
 *   /delete <id>          delete a task by id
 *   /check <id>            toggle a task's completed status
 *   /list                     show all tasks
 *   /help                   show this help
 *   /quit, /exit          exit the app
 *
 * Design:
 *   The linked list is the in-memory source of truth for the running
 *   session (fast reads, no round-trip to disk to print/toggle). SQLite
 *   is the persistence layer: every mutation goes list-then-db (or
 *   db-then-list, see comments) so the two never drift apart while the
 *   app is running. On startup, the list is rebuilt from whatever is in
 *   the database, so state survives restarts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <readline/readline.h>
#include <readline/history.h>

#define DB_PATH "tasks.db"

/* ---------- Data structures ---------- */

typedef struct Task {
    int id;
    char *description;
    int is_completed;
    struct Task *next;
    struct Task *prev;
} Task;

typedef struct TaskList {
    Task *head;
    Task *tail;
    int count;
} TaskList;

/* ---------- Linked list layer ---------- */

TaskList *list_create(void);
Task *list_add(TaskList *list, int id, const char *desc, int is_completed);
Task *list_find(TaskList *list, int id);
int list_delete(TaskList *list, int id);
void list_print(const TaskList *list);
void list_free(TaskList *list);

/* ---------- SQLite layer ---------- */

sqlite3 *db_init(const char *path);
void db_close(sqlite3 *db);
int db_insert_task(sqlite3 *db, const char *desc);
int db_toggle_task(sqlite3 *db, int id, int new_status);
int db_delete_task(sqlite3 *db, int id);
TaskList *db_load_all(sqlite3 *db);

/* ---------- Parsing helpers ---------- */

static char *skip_spaces(char *s);
static void parse_line(char *line, char **cmd, char **arg);
static int parse_id(const char *arg, int *out);

/* ---------- Command handlers ---------- */

static void cmd_add(sqlite3 *db, TaskList *list, const char *desc);
static void cmd_delete(sqlite3 *db, TaskList *list, const char *arg);
static void cmd_check(sqlite3 *db, TaskList *list, const char *arg);
static void print_help(void);

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

Task *list_add(TaskList *list, int id, const char *desc, int is_completed) {
    Task *t = malloc(sizeof(Task));
    if (!t)
        return NULL;

    t->id = id;
    t->description = strdup(desc);
    t->is_completed = is_completed;
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
        printf("  [%s] #%-3d %s\n", t->is_completed ? "x" : " ", t->id, t->description);
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

int db_insert_task(sqlite3 *db, const char *desc) {
    const char *sql = "INSERT INTO tasks (description) VALUES (?);";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, desc, -1, SQLITE_TRANSIENT);

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
    const char *sql = "SELECT id, description, is_completed FROM tasks ORDER BY id;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Load failed: %s\n", sqlite3_errmsg(db));
        return list;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *desc = sqlite3_column_text(stmt, 1);
        int done = sqlite3_column_int(stmt, 2);
        list_add(list, id, (const char *)desc, done);
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

/* ================= Command handlers ================= */

static void cmd_add(sqlite3 *db, TaskList *list, const char *desc) {
    if (*desc == '\0') {
        printf("Usage: /add <description>\n");
        return;
    }

    /* DB first: we need the autoincrement id before we can create the
     * in-memory node, since the node's id must match the DB row. */
    int id = db_insert_task(db, desc);
    if (id < 0) {
        printf("Failed to add task to database.\n");
        return;
    }

    list_add(list, id, desc, 0);
    printf("Added task #%d: %s\n", id, desc);
}

static void cmd_delete(sqlite3 *db, TaskList *list, const char *arg) {
    int id;

    if (parse_id(arg, &id) != 0) {
        printf("Usage: /delete <id>\n");
        return;
    }

    if (!list_find(list, id)) {
        printf("No task with id %d.\n", id);
        return;
    }

    if (db_delete_task(db, id) != 0) {
        printf("Failed to delete task in database.\n");
        return;
    }

    list_delete(list, id);
    printf("Deleted task #%d.\n", id);
}

static void cmd_check(sqlite3 *db, TaskList *list, const char *arg) {
    int id;

    if (parse_id(arg, &id) != 0) {
        printf("Usage: /check <id>\n");
        return;
    }

    Task *t = list_find(list, id);
    if (!t) {
        printf("No task with id %d.\n", id);
        return;
    }

    int new_status = !t->is_completed;

    if (db_toggle_task(db, id, new_status) != 0) {
        printf("Failed to update task in database.\n");
        return;
    }

    t->is_completed = new_status;
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

/* ================= Main loop ================= */

int main(void) {
    sqlite3 *db = db_init(DB_PATH);
    if (!db) {
        fprintf(stderr, "Could not initialize database. Exiting.\n");
        return 1;
    }

    TaskList *list = db_load_all(db);
    printf("Loaded %d task(s) from %s.\n", list->count, DB_PATH);
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
            list_print(list);
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

    list_free(list);
    db_close(db);
    printf("Goodbye.\n");
    return 0;
}
