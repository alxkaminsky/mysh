#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include "commands.h"
#include "builtins.h"
#include "io_helpers.h"
#include "variables.h"

// --- Globals ---
fd_set read_fds;
sigjmp_buf jump_buffer;

char* input_buf = NULL;
char* padded_line = NULL;
char *token_arr[MAX_STR_LEN] = {NULL};
Node *variables = NULL;
ClientNode *clients = NULL;

int limit = 1;
int should_prompt = 1;
int server_socket = -1;
int connections = 0;
int prompt_for_input = 1;

// --- Background Job Globals ---
JobNode *bg_list = NULL;
int next_job_id = 1;
int active_jobs = 0;

// --- Helpers ---
void free_dynamic_tokens(char **tokens, char *buf_start, char *buf_end) {
    for (size_t i = 0; i < MAX_STR_LEN; i++) {
        if (tokens[i] && (tokens[i] < buf_start || tokens[i] > buf_end)) {
            free(tokens[i]);
            tokens[i] = NULL;
        }
    }
}

// --- Signal Handlers ---
void handle_sigchld(int sig) {
    (void)sig;
}

void handle_sigint(int sig) {
    (void)sig;
    write(STDOUT_FILENO, "\n", 1);

    if (padded_line) {
        free_dynamic_tokens(token_arr, padded_line, padded_line + (MAX_STR_LEN * 2));
        free(padded_line);
        padded_line = NULL;
    }

    if (input_buf) {
        free(input_buf);
        input_buf = NULL;
    }

    if (!should_prompt) {
        should_prompt = 1;
    }

    siglongjmp(jump_buffer, 1);
}

void execute_external_command(char **tokens) {
    extern int prompt_for_input;

    // skip the fork and replace the shell process directly.
    // This ensures the external command inherits the exact Job PID.
    if (!prompt_for_input) {
        signal(SIGINT, SIG_DFL);
        if (execvp(tokens[0], tokens) == -1) {
            display_error("ERROR: Unknown command: ", tokens[0]);
            _exit(1);
        }
    }

    // Normal interactive execution: fork and wait
    pid_t pid = fork();
    if (pid == -1) {
        display_error("ERROR: Fork failed", "");
    } else if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        if (execvp(tokens[0], tokens) == -1) {
            display_error("ERROR: Unknown command: ", tokens[0]);
            _exit(1);
        }
    } else {
        int status;
        waitpid(pid, &status, 0);
    }
}

int handle_assignment(char *token, Node **var_ptr, char *sep) {
    if (sep == token) {
        display_error("ERROR: No variable name: ", token);
        return -1;
    }
    *sep = '\0';
    char *val = sep + 1;
    char* key = malloc(MAX_STR_LEN + 1);
    key[MAX_STR_LEN] = '\0';
    strncpy(key, token, MAX_STR_LEN);

    char *original_val = val;
    expand_variables(&val, *var_ptr, 0);


    if (val == original_val) {
        val = strdup(original_val);
    }

    if (strlen(val) > MAX_STR_LEN) {
        val[MAX_STR_LEN] = '\0';
    }

    define_var(key, val, var_ptr);

    // Do NOT free(val) here! define_var now owns this heap string safely.
    return 0;
}

// --- Main ---
int main(int argc, char* argv[]) {
    char *prompt = "mysh$ ";
    char *cmd_from_args = NULL;
    int c_flag_executed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            prompt_for_input = 0;
        } else if (strcmp(argv[i], "-p") == 0) {
            limit = 0;
            prompt_for_input = 0;
        } else {
            if (cmd_from_args == NULL) cmd_from_args = argv[i];
        }
    }

    struct sigaction sa_int;
    sa_int.sa_handler = handle_sigint;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_chld;
    sa_chld.sa_handler = handle_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    signal(SIGPIPE, SIG_IGN);

    sigsetjmp(jump_buffer, 1);

    while (1) {
        if (input_buf) { free(input_buf); input_buf = NULL; }
        if (padded_line) { free(padded_line); padded_line = NULL; }

        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            int job_id;
            char *cmd = remove_job(&bg_list, pid, &job_id);
            if (cmd) {
                char buffer[MAX_STR_LEN + 64];
                sprintf(buffer, "[%d]+ Done %s\n", job_id, cmd);
                display_message(buffer);
                free(cmd);
                active_jobs--;
                if (active_jobs == 0) next_job_id = 1;
            }
        }

        if (prompt_for_input && cmd_from_args == NULL && should_prompt) {
            write(STDOUT_FILENO, prompt, strlen(prompt));
            should_prompt = 0;
        }

        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        int max_fd = STDIN_FILENO;

        if (server_socket != -1) {
            FD_SET(server_socket, &read_fds);
            if (server_socket > max_fd) max_fd = server_socket;
        }

        ClientNode *curr = clients;
        while (curr) {
            FD_SET(curr->fd, &read_fds);
            if (curr->fd > max_fd) max_fd = curr->fd;
            curr = curr->next;
        }

        if (cmd_from_args == NULL) {
            select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        }

        curr = clients;
        while (curr) {
            ClientNode *next_node = curr->next;
            if (FD_ISSET(curr->fd, &read_fds)) {
                char buffer[MAX_STR_LEN + 32];
                int prefix_len = sprintf(buffer, "client%d: ", curr->id);
                int r = read(curr->fd, buffer + prefix_len, MAX_STR_LEN);

                if (r > 0) {
                    buffer[prefix_len + r] = '\0';
                    char *payload = buffer + prefix_len;

                    if (strcmp(payload, "\\connected") == 0 || strcmp(payload, "\\connected\n") == 0) {
                        int active_count = 0;
                        ClientNode *temp = clients;
                        while(temp) { active_count++; temp = temp->next; }
                        char conn_msg[64];
                        sprintf(conn_msg, "Connected clients: %d\n", active_count);
                        write(curr->fd, conn_msg, strlen(conn_msg));
                    } else {
                        strcat(buffer, "\n");
                        write(STDOUT_FILENO, buffer, strlen(buffer));
                        ClientNode *bcast_target = clients;
                        while (bcast_target) {
                            write(bcast_target->fd, buffer, strlen(buffer));
                            bcast_target = bcast_target->next;
                        }
                    }
                } else {
                    remove_client(curr->id);
                }
            }
            curr = next_node;
        }

        if (server_socket != -1 && FD_ISSET(server_socket, &read_fds)) {
            int fd = accept(server_socket, NULL, NULL);
            if (fd > 0) {
                connections++;
                add_client(fd, connections);
            }
        }

        if (cmd_from_args != NULL) {
            if (c_flag_executed > 0) return 0;
            input_buf = strdup(cmd_from_args);
            c_flag_executed++;
        } else if (FD_ISSET(STDIN_FILENO, &read_fds)){
            int r = get_input(&input_buf);
            if (r < 0) {
                should_prompt = 1;
                continue;
            }
            if (r == 0) {
                goto cleanup_and_exit;
            }
        }
        else {
            continue;
        }

        char* curr_line = input_buf;
        while (curr_line != NULL) {
            char* next_line = strchr(curr_line, '\n');
            if (next_line != NULL) *next_line = '\0';

            padded_line = malloc(MAX_STR_LEN * 2);
            if (!padded_line) break;

            int p_idx = 0;
            for (int j = 0; curr_line[j] != '\0' && p_idx < (MAX_STR_LEN * 2) - 3; j++) {
                if (curr_line[j] == '|' || curr_line[j] == '&') {
                    padded_line[p_idx++] = ' ';
                    padded_line[p_idx++] = curr_line[j];
                    padded_line[p_idx++] = ' ';
                } else {
                    padded_line[p_idx++] = curr_line[j];
                }
            }
            padded_line[p_idx] = '\0';

            // from previous loops don't trick free_dynamic_tokens into a double-free
            memset(token_arr, 0, sizeof(token_arr));
            size_t token_count = tokenize_input(padded_line, token_arr);

            if (token_count > 0) {
                char *assign_sep = strchr(token_arr[0], '=');
                char *dollar_sep = strchr(token_arr[0], '$');
                int is_main_assignment = 0;

                if (assign_sep && (dollar_sep == NULL || dollar_sep > assign_sep) && token_arr[0][0] != '$') {
                    is_main_assignment = 1;
                }

                int is_send = 0;

                if (!is_main_assignment) {
                    expand_variables(&token_arr[0], variables, 0);
                    if (token_arr[0] != NULL && strcmp(token_arr[0], "send") == 0) {
                        is_send = 1;
                    }
                }

                for (size_t i = 1; i < token_count; i++) {
                    expand_variables(&token_arr[i], variables, is_send);
                }

                char *exec_tokens[MAX_STR_LEN] = {NULL};
                size_t exec_count = 0;
                for (size_t i = 0; i < token_count; i++) {
                    if (token_arr[i] && strlen(token_arr[i]) > 0) exec_tokens[exec_count++] = token_arr[i];
                }
                exec_tokens[exec_count] = NULL;

                if (exec_count > 0 || is_main_assignment) {
                    char cmd_str[MAX_STR_LEN * 2] = "";
                    for (size_t i = 0; i < exec_count; i++) {
                        strncat(cmd_str, exec_tokens[i], sizeof(cmd_str) - strlen(cmd_str) - 1);
                        if (i < exec_count - 1) strncat(cmd_str, " ", sizeof(cmd_str) - strlen(cmd_str) - 1);
                    }

                    char* and_ptr = strchr(cmd_str, '&');
                    char* pipe_ptr = strchr(cmd_str, '|');

                    if (and_ptr) {
                        *and_ptr = '\0';
                        bg_op(cmd_str);
                    } else if (pipe_ptr) {
                        char *left = cmd_str; char *right = pipe_ptr + 1;
                        *pipe_ptr = '\0';
                        pipe_op(left, right);
                    } else {
                        if (is_main_assignment) {
                            handle_assignment(token_arr[0], &variables, assign_sep);
                        } else if (strcmp(exec_tokens[0], "exit") == 0) {
                            goto cleanup_and_exit;
                        } else {
                            bn_ptr builtin_fn = check_builtin(exec_tokens[0]);
                            cmd_ptr cmd_fn = check_command(exec_tokens[0]);
                            if (builtin_fn) builtin_fn(exec_tokens);
                            else if (cmd_fn) cmd_fn(exec_tokens);
                            else execute_external_command(exec_tokens);
                        }
                    }
                }
            }
            free_dynamic_tokens(token_arr, padded_line, padded_line + (MAX_STR_LEN * 2));
            free(padded_line);
            padded_line = NULL;
            if (next_line) curr_line = next_line + 1; else curr_line = NULL;
        }
        should_prompt = 1;
    }

cleanup_and_exit:
    free_all(variables);
    free_jobs(bg_list);
    if (server_socket != -1) close_server(NULL); else free_clients();
    if (padded_line) free(padded_line);
    if (input_buf) free(input_buf);

    return 0;
}