#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "io_helpers.h"
#include "commands.h"
#include "variables.h"

extern JobNode *bg_list;
extern ClientNode *clients;
extern int next_job_id;
extern int active_jobs;
extern int server_socket;
extern Node *variables;

volatile sig_atomic_t client_interrupted = 0;

void handle_client_sigint(int sig) {
    (void)sig;
    client_interrupted = 1;
}

void pipe_op(char *left, char *right) {
    if (!left || !right || strlen(left) == 0 || strlen(right) == 0) {
        display_error("ERROR: Syntax error, not enough pipe arguments", "");
        return;
    }

    int fd[2];
    if (pipe(fd) == -1) {
        display_error("ERROR: pipe failed", "");
        return;
    }

    pid_t left_pid = fork();
    if (left_pid == 0) {
        dup2(fd[1], STDOUT_FILENO);
        close(fd[0]);
        close(fd[1]);

        write(STDOUT_FILENO, right, strlen(right));
        write(STDOUT_FILENO, "\n", 1);

        char *args[] = {"./mysh", "-c", left, NULL};
        execvp("./mysh", args);
        _exit(1);
    }

    pid_t right_pid = fork();
    if (right_pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);
        close(fd[1]);

        char *args[] = {"./mysh", "-p", NULL};
        execvp("./mysh", args);
        _exit(1);
    }

    close(fd[0]);
    close(fd[1]);
    waitpid(left_pid, NULL, 0);
    waitpid(right_pid, NULL, 0);
}

void bg_op(char *command) {
    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    pid_t pid = fork();
    if (pid < 0) {
        display_error("ERROR: fork failed", "");
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        return;
    }

    if (pid > 0) {
        add_job(&bg_list, pid, next_job_id, command);

        char buffa[MAX_STR_LEN];
        sprintf(buffa, "[%d] %d\n", next_job_id, (int)pid);
        display_message(buffa);

        next_job_id++;
        active_jobs++;
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
    } else {
        sigprocmask(SIG_SETMASK, &oldmask, NULL);
        signal(SIGINT, SIG_DFL);

        prctl(PR_SET_NAME, command, 0, 0, 0);

        int fd[2];
        if (pipe(fd) == -1) _exit(1);

        write(fd[1], command, strlen(command));
        write(fd[1], "\n", 1);
        close(fd[1]);

        dup2(fd[0], STDIN_FILENO);
        close(fd[0]);

        char *args[] = {"./mysh", "-c", NULL};
        execvp("./mysh", args);

        _exit(1);
    }
}

cmd_ptr check_command(const char *cmd) {
    for (int i = 0; i < COMMANDS_COUNT; i++) {
        if (strcmp(cmd, COMMANDS[i]) == 0) {
            return COMMANDS_FN[i];
        }
    }
    return NULL;
}

int close_server(char **tokens) {
    (void)tokens;
    if (server_socket == -1) return -1;
    free_clients();
    clients = NULL;
    close(server_socket);
    server_socket = -1;
    return 0;
}

int start_server(char **tokens) {
    if (server_socket != -1) {
        display_error("ERROR: Server already running", "");
        return -1;
    }

    if (!tokens[1]) {
        display_error("ERROR: No port provided", "");
        return -1;
    }

    char *ptr = NULL;
    int port = strtol(tokens[1], &ptr, 10);
    if (ptr == tokens[1] || *ptr != '\0') {
        display_error("ERROR: Invalid port: ", tokens[1]);
        return -1;
    }

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        display_error("ERROR: Failed to create socket", "");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        display_error("ERROR: Failed to start server", "");
        close(listen_sock);
        return -1;
    }

    listen(listen_sock, 10);
    server_socket = listen_sock;
    return 0;
}

int send_message(char **args) {
    if (args[1] == NULL || args[2] == NULL || args[3] == NULL) {
        display_error("ERROR: Usage: send <port> <hostname> <message>", "");
        return 1;
    }

    int total_len = 0;
    for (int i = 3; args[i] != NULL; i++) {
        total_len += strlen(args[i]);
        if (args[i+1] != NULL) total_len += 1;
    }

    // Now correctly catches massive strings passed by the main parser
    if (total_len > MAX_STR_LEN) {
        display_error("ERROR: Message exceeds 128 characters", "");
        return 1;
    }

    char message[MAX_STR_LEN + 1];
    message[0] = '\0';

    for (int i = 3; args[i] != NULL; i++) {
        strcat(message, args[i]);
        if (args[i+1] != NULL) {
            strcat(message, " ");
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 1;

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(atoi(args[1]));

    if (inet_pton(AF_INET, args[2], &serv_addr.sin_addr) <= 0) {
        display_error("ERROR: Invalid hostname", "");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        display_error("ERROR: Connection failed", "");
        close(sock);
        return 1;
    }

    write(sock, message, strlen(message));
    close(sock);
    return 0;
}

int start_client(char **args) {
    if (args[1] == NULL || args[2] == NULL) {
        display_error("ERROR: Usage: start-client <port> <hostname>", "");
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        display_error("ERROR: Failed to create socket", "");
        return 1;
    }

    struct sockaddr_in serv_addr;
    // 1. Zero out the struct to prevent garbage memory
    memset(&serv_addr, 0, sizeof(serv_addr));

    // 2. Safely parse the port
    int port = atoi(args[1]);
    if (port <= 0 || port > 65535) {
        display_error("ERROR: Invalid port", "");
        close(sock);
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // 3. Check for inet_pton failure (Prevents the connect hang)
    if (inet_pton(AF_INET, args[2], &serv_addr.sin_addr) <= 0) {
        display_error("ERROR: Invalid IP address", "");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        display_error("ERROR: Connection failed", "");
        close(sock);
        return 1;
    }

    struct sigaction sa_client, sa_old;
    sa_client.sa_handler = handle_client_sigint;
    sigemptyset(&sa_client.sa_mask);
    sa_client.sa_flags = 0;
    sigaction(SIGINT, &sa_client, &sa_old);

    client_interrupted = 0;
    fd_set client_fds;

    while (!client_interrupted) {
        FD_ZERO(&client_fds);
        FD_SET(STDIN_FILENO, &client_fds);
        FD_SET(sock, &client_fds);

        int max_fd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;

        if (select(max_fd + 1, &client_fds, NULL, NULL, NULL) < 0) {
            if (client_interrupted) break;
            continue;
        }

        if (FD_ISSET(sock, &client_fds)) {
            char server_buf[MAX_STR_LEN + 32];
            int r = read(sock, server_buf, sizeof(server_buf) - 1);
            if (r <= 0) break;
            server_buf[r] = '\0';
            write(STDOUT_FILENO, server_buf, strlen(server_buf));
        }

        if (FD_ISSET(STDIN_FILENO, &client_fds)) {
            char client_buf[1024];
            int r = read(STDIN_FILENO, client_buf, sizeof(client_buf) - 1);
            if (r <= 0) break;

            client_buf[r] = '\0';
            char *nl = strchr(client_buf, '\n');
            if (nl) *nl = '\0';

            if (client_buf[0] == '\0' || strcmp(client_buf, "exit") == 0) {
                break;
            }

            char *expanded_msg = client_buf;
            // Bypass truncation here to let the client check the total length
            expand_variables(&expanded_msg, variables, 1);

            // Now properly catches the error if the expanded message is too massive
            if (strlen(expanded_msg) > MAX_STR_LEN) {
                display_error("ERROR: Message exceeds 128 characters", "");
                if (expanded_msg != client_buf) free(expanded_msg);
                continue;
            }

            write(sock, expanded_msg, strlen(expanded_msg));
            if (expanded_msg != client_buf) free(expanded_msg);
        }
    }

    if (client_interrupted) {
        write(STDOUT_FILENO, "\n", 1);
    }

    close(sock);
    sigaction(SIGINT, &sa_old, NULL);

    return 0;
}