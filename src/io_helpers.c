#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "io_helpers.h"
#include "variables.h"

extern int limit;
extern ClientNode *clients;

// ===== Output helpers =====

void display_message(char *str) {
    write(STDOUT_FILENO, str, strnlen(str, MAX_STR_LEN));
}

void display_error(char *pre_str, char *str) {
    write(STDERR_FILENO, pre_str, strnlen(pre_str, MAX_STR_LEN));
    write(STDERR_FILENO, str, strnlen(str, MAX_STR_LEN));
    write(STDERR_FILENO, "\n", 1);
}

// ===== Input tokenizing =====

int get_input(char **in_ptr) {
    *in_ptr = malloc(MAX_STR_LEN + 1);
    if (!*in_ptr) return -1;

    if (limit == 0) {
        size_t capacity = MAX_STR_LEN + 1;
        size_t len = 0;
        char c;
        ssize_t bytes_read;

        while ((bytes_read = read(STDIN_FILENO, &c, 1)) > 0) {
            if (len >= capacity - 1) {
                capacity *= 2;
                char *new_ptr = realloc(*in_ptr, capacity);
                if (!new_ptr) {
                    free(*in_ptr);
                    *in_ptr = NULL;
                    return -1;
                }
                *in_ptr = new_ptr;
            }

            (*in_ptr)[len++] = c;

            // Break AFTER adding the newline so 'len' counts it
            if (c == '\n') break;
        }

        // Now, a pure Ctrl^D will have len == 0.
        // Just pressing Enter will have len == 1.
        if (len == 0 && bytes_read == 0) {
            free(*in_ptr);
            *in_ptr = NULL;
            return 0;
        } else if (len == 0 && bytes_read < 0) {
            free(*in_ptr);
            *in_ptr = NULL;
            return -1;
        }

        (*in_ptr)[len] = '\0';
        return len;
    }

    int retval = read(STDIN_FILENO, *in_ptr, MAX_STR_LEN + 1);
    int read_len = retval;

    if (retval == -1) {
        read_len = 0;
    }

    if (read_len > MAX_STR_LEN) {
        read_len = 0;
        retval = -1;
        write(STDERR_FILENO, "ERROR: input line too long\n", 27);

        char junk = (*in_ptr)[MAX_STR_LEN];
        while (junk != '\n' && read(STDIN_FILENO, &junk, 1) > 0);
    }

    (*in_ptr)[read_len] = '\0';
    return retval;
}

size_t tokenize_input(char *in_ptr, char **tokens) {
    char *curr_ptr = strtok(in_ptr, DELIMITERS);
    size_t token_count = 0;

    while (curr_ptr != NULL) {
        token_count++;
        tokens[token_count - 1] = curr_ptr;
        curr_ptr = strtok(NULL, DELIMITERS);
    }

    tokens[token_count] = NULL;
    return token_count;
}

// NEW SIGNATURE: Added bypass_truncation flag
void expand_variables(char **token_ptr_ptr, Node *head, int bypass_truncation) {
    char *curr = *token_ptr_ptr;

    if (strchr(curr, '$') == NULL) return;

    // Allocate exact bounds for typical case, or dynamic capacity if bypassing
    size_t capacity = bypass_truncation ? (MAX_STR_LEN * 2) : (MAX_STR_LEN + 1);
    char *expanded = malloc(capacity);
    if (!expanded) {
        display_error("ERROR: memory limit reached", "");
        return;
    }
    expanded[0] = '\0';
    size_t current_len = 0;

    char *dollar;
    while ((dollar = strchr(curr, '$')) != NULL) {
        *dollar = '\0';
        size_t chunk_len = strlen(curr);

        // Natively truncate the chunk if we aren't bypassing
        if (!bypass_truncation && current_len + chunk_len > MAX_STR_LEN) {
            chunk_len = MAX_STR_LEN - current_len;
        }

        while (bypass_truncation && current_len + chunk_len + 1 > capacity) {
            capacity *= 2;
            expanded = realloc(expanded, capacity);
        }
        strncat(expanded, curr, chunk_len);
        current_len += chunk_len;

        // Stop processing immediately if typical limit is hit
        if (!bypass_truncation && current_len >= MAX_STR_LEN) break;

        char *var_start = dollar + 1;
        char *next_dollar = strchr(var_start, '$');
        char var_name[MAX_STR_LEN] = {0};

        if (next_dollar) {
            strncpy(var_name, var_start, next_dollar - var_start);
            curr = next_dollar;
        } else {
            strcpy(var_name, var_start);
            curr = var_start + strlen(var_start);
        }

        char *val = find(var_name, head);
        if (val) {
            size_t val_len = strlen(val);

            // Natively truncate the variable insertion if we aren't bypassing
            if (!bypass_truncation && current_len + val_len > MAX_STR_LEN) {
                val_len = MAX_STR_LEN - current_len;
            }

            while (bypass_truncation && current_len + val_len + 1 > capacity) {
                capacity *= 2;
                expanded = realloc(expanded, capacity);
            }
            strncat(expanded, val, val_len);
            current_len += val_len;

            // Stop processing immediately if typical limit is hit
            if (!bypass_truncation && current_len >= MAX_STR_LEN) break;
        }
    }

    // Append whatever is left of the original string
    if (!bypass_truncation && current_len < MAX_STR_LEN) {
        size_t remaining_len = strlen(curr);
        if (current_len + remaining_len > MAX_STR_LEN) {
            remaining_len = MAX_STR_LEN - current_len;
        }
        strncat(expanded, curr, remaining_len);
    } else if (bypass_truncation) {
        size_t remaining_len = strlen(curr);
        while (current_len + remaining_len + 1 > capacity) {
            capacity *= 2;
            expanded = realloc(expanded, capacity);
        }
        strcat(expanded, curr);
    }

    *token_ptr_ptr = expanded;
}

void add_job(JobNode **head, pid_t pid, int job_id, char *cmd) {
    JobNode *new_node = malloc(sizeof(JobNode));
    new_node->pid = pid;
    new_node->job_id = job_id;
    new_node->command = strdup(cmd);
    new_node->next = *head;
    *head = new_node;
}

char* remove_job(JobNode **head, pid_t pid, int *out_job_id) {
    JobNode *curr = *head, *prev = NULL;
    while (curr) {
        if (curr->pid == pid) {
            if (prev) prev->next = curr->next;
            else *head = curr->next;

            char *cmd = curr->command;
            if (out_job_id) *out_job_id = curr->job_id;
            free(curr);
            return cmd;
        }
        prev = curr;
        curr = curr->next;
    }
    return NULL;
}

void free_jobs(JobNode *head) {
    JobNode *curr = head;
    while (curr != NULL) {
        JobNode *temp = curr;
        curr = curr->next;
        if (temp->command) free(temp->command);
        free(temp);
    }
}

void add_client(int fd, int connections) {
    ClientNode *client = malloc(sizeof(ClientNode));
    client->fd = fd;
    client->id = connections;
    client->next = NULL;

    if (!clients) {
        clients = client;
        return;
    }
    ClientNode *curr = clients;
    while (curr->next != NULL) {
        curr = curr->next;
    }
    curr->next = client;
}

void remove_client(int id) {
    ClientNode *curr = clients;
    ClientNode *prev = NULL;

    while (curr) {
        if (curr->id == id) {
            if (prev == NULL) {
                clients = curr->next;
            } else {
                prev->next = curr->next;
            }
            close(curr->fd);
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

void free_clients() {
    ClientNode *curr = clients;
    while (curr) {
        ClientNode *tmp = curr;
        close(curr->fd);
        curr = curr -> next;
        free(tmp);
    }
}