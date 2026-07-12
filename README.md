# trp-todo-c

`trp-todo-c` is a small C todo app with both a terminal interface and a tiny HTTP view of the current task list.

## Layout

- `main.c` - the application entry point, CLI, SQLite storage, and HTTP server
- `styles.css` - the stylesheet served by the built-in web server
- `tasks.db` - the local SQLite database file created at runtime
- `Makefile` - build, run, and clean targets

## Build

```sh
make
```

## Run

```sh
./todo
```

You can also pass a port number:

```sh
./todo 9090
```

## Commands

- `/add <description>` add a task
- `/delete <id>` delete a task
- `/check <id>` toggle completion
- `/list` or `/show` list tasks
- `/help` show the command list
- `/quit` or `/exit` close the app