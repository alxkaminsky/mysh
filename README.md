# myshell

A Unix shell written in C. It supports shell variables, multi-stage pipelines, background jobs, a set of builtins, and a built-in TCP chat server and client, all running on a single `select()`-based event loop.

Built for systems programming practise.

> **Platform:** Linux only. Developed and tested on Ubuntu. It relies on Linux-specific APIs (`prctl`, GNU `ps` options), so it will not build or run correctly on macOS or Windows.

```
mysh$ name=world
mysh$ echo hello $name
hello world
mysh$ cat notes.txt | wc
word count 7
character count 40
newline count 3
mysh$ sleep 1 &
[1] 2027
mysh$ ps
sleep 2027
mysh$ echo done waiting
done waiting
[1]+ Done sleep 1
```

## Features

**Builtins**

| Command | Description |
|---|---|
| `echo [args...]` | Print arguments separated by spaces |
| `cat [file]` | Print a file, or stdin if no file is given |
| `wc [file]` | Count words, characters, and newlines in a file or stdin |
| `cd [path]` | Change directory (no argument goes to your home directory) |
| `ls [path] [--a] [--rec --d N] [--f substr]` | List a directory. `--a` shows hidden files, `--rec --d N` recurses to depth N, `--f` filters names by substring |
| `ps` | List processes started from this shell |
| `kill pid [signum]` | Send a signal (default `SIGTERM`) |
| `exit` | Quit the shell |

In any path, `...` means two directories up, `....` three, and so on.

**Variables.** `name=value` defines a variable and `$name` expands it anywhere in a command, including inside other assignments (`greeting=hi$name`).

**Pipelines.** Chain any number of commands with `|`, mixing builtins and external programs (`cat file | cat | wc`).

**Background jobs.** End a command with `&` to run it in the background. The shell prints the job number and pid, and reports `[n]+ Done command` once the job finishes.

**External programs.** Anything that isn't a builtin is looked up on `PATH` and run with `fork` and `execvp`.

**Chat server and client.**

| Command | Description |
|---|---|
| `start-server PORT` | Accept TCP clients in the background while you keep using the shell. Incoming messages are printed and broadcast to every client |
| `close-server` | Disconnect all clients and stop the server |
| `send PORT HOST MESSAGE` | Send a single message to a server |
| `start-client PORT HOST` | Open an interactive chat session. Sending `\connected` asks the server how many clients are connected; an empty line, `exit`, or Ctrl-C leaves |

**Signals.** Ctrl-C cancels the current command and returns to a fresh prompt without exiting the shell. Ctrl-D exits.

## Building and running

Requires Linux and `gcc`.

```bash
cd src
make
./mysh
```

The Makefile builds with `-Wall -Wextra -Werror` and with AddressSanitizer, LeakSanitizer, and UndefinedBehaviorSanitizer enabled, so memory errors are reported immediately during development.

Run `./mysh` from inside `src/`. Pipelines and background jobs work by relaunching `./mysh` (see below), so they expect the binary in the current directory.

## How it works

**Event loop.** The main loop blocks in `select()` on stdin, the server's listening socket, and every connected client at the same time. That lets the shell accept connections and relay chat messages while it waits for your next command. Finished background jobs are reaped with `waitpid(WNOHANG)` on each pass through the loop.

**Pipelines and background jobs.** Instead of a separate executor, the shell runs copies of itself as helper processes:

- For `left | right`, it forks two children connected by a pipe. The left child runs `mysh -c left` with its stdout on the pipe. The right child runs `mysh -p`, which reads the command `right` from the first line of the pipe and passes the rest of the data to that command as stdin. Because `right` is parsed by a full shell, a pipeline of any length breaks down into repeated two-stage splits.
- For `command &`, it forks a child running `mysh -c`, records the pid as a job, and returns to the prompt immediately. `SIGCHLD` is blocked around the fork so a job that exits instantly can't be reaped before it's recorded.
- Helper shells `exec` external programs directly instead of forking again, so a background job's pid is the pid of the actual program, which is what `ps` and `kill` operate on.

**Signals.** The `SIGINT` handler jumps back to the main loop with `siglongjmp`, discarding the command in progress. Child processes restore the default `SIGINT` behaviour so Ctrl-C stops a foreground program. `SIGPIPE` is ignored so a client disconnecting mid-broadcast can't crash the server.

**Variables** are stored in a linked list of name/value pairs, and expansion builds a new string for each token that contains `$`.

## Limitations

- No quoting or escaping; arguments are split on whitespace.
- Input lines and variable values are limited to 128 characters.
- Linux only (see above).

## Layout

```
src/mysh.c         main loop, input parsing, command dispatch, signal handling, chat server loop
src/builtins.c     echo, ls, cd, cat, wc, ps, kill
src/commands.c     pipes, background jobs, network commands
src/io_helpers.c   input, tokenizing, variable expansion, job and client lists
src/variables.c    variable storage
```
