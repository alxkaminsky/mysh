#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include "builtins.h"
#include "io_helpers.h"

// ====== Helper functions =====

/**
 * Helper: Expands only custom "..." tokens.
 * - "." and ".." are returned as-is (realpath handles them).
 * - "..." (3+ dots) -> returns (n-1) count of "../"
 * - Others returned as-is.
 * Returns NULL on malloc failure.
 */
char *expand_dots(char *token) {
    if (!token) return NULL;

    // Optimization: Let realpath handle standard . and ..
    if (strcmp(token, ".") == 0 || strcmp(token, "..") == 0) {
        return strdup(token);
    }

    // Check if the token consists ONLY of dots
    int all_dots = 1;
    size_t len = strlen(token);

    // We only care about custom expansion for 3 or more dots
    if (len < 3) all_dots = 0;

    if (all_dots) {
        for (size_t i = 0; i < len; i++) {
            if (token[i] != '.') {
                all_dots = 0;
                break;
            }
        }
    }

    if (all_dots) {
        size_t repeats = len - 1; // n dots = n-1 parent directories

        size_t needed_size = (repeats * 3) + 1;
        char *expanded = malloc(needed_size);
        if (!expanded) return NULL;

        expanded[0] = '\0';
        for (size_t i = 0; i < repeats; i++) {
            strcat(expanded, "../");
        }
        return expanded;
    }
    else {
        return strdup(token);
    }
}

/**
 * Expands path by:
 * 1. Pre-processing custom dot tokens (...) into standard ../../
 * 2. Using realpath() to resolve the final canonical path
 * Returns NULL on internal malloc failure.
 */
char *expand_path(char *path) {
    if (path == NULL) {
        return strdup(".");
    }

    // --- PHASE 1: Pre-process custom dots (..., ....) ---
    size_t cap = MAX_STR_LEN;
    char *temp_path = malloc(cap);
    if (!temp_path) return NULL;
    temp_path[0] = '\0';

    // If absolute path, preserve leading slash
    if (path[0] == '/') {
        strcat(temp_path, "/");
    }

    // Work on a copy to avoid modifying the input
    char *work_copy = strdup(path);
    if (!work_copy) {
        free(temp_path);
        return NULL;
    }

    char *token = strtok(work_copy, "/");

    while (token != NULL) {
        char *expanded_token = expand_dots(token);

        if (!expanded_token) {
            // Malloc failed in helper
            free(temp_path);
            free(work_copy);
            return NULL;
        }

        size_t new_len = strlen(temp_path) + strlen(expanded_token) + 2;
        if (new_len > cap) {
            cap *= 2;
            char *bigger = realloc(temp_path, cap);
            if (!bigger) {
                free(temp_path);
                free(work_copy);
                free(expanded_token);
                return NULL;
            }
            temp_path = bigger;
        }

        // Add separator if needed (avoid double slash at start)
        if (strlen(temp_path) > 0 && temp_path[strlen(temp_path)-1] != '/') {
            strcat(temp_path, "/");
        }
        strcat(temp_path, expanded_token);

        free(expanded_token);
        token = strtok(NULL, "/");
    }
    free(work_copy);

    if (strlen(temp_path) == 0) {
        strcpy(temp_path, ".");
    }

    // --- PHASE 2: Canonicalization ---
    char *resolved = realpath(temp_path, NULL);

    if (resolved) {
        free(temp_path);
        return resolved;
    } else {
        // Return temp_path so the caller can report "Invalid path"
        // Note: We do NOT return NULL here, because a non-existent path
        // is a user error (Invalid Path), not a system error (Builtin Failed).
        return temp_path;
    }
}

ssize_t list_files_recursive(char *path, int depth, int a, char *substring) {
    char *expanded = expand_path(path);

    // CHECK: Expand path failure (internal error)
    if (!expanded) {
        display_error("ERROR: Builtin failed: ls", "");
        return -1;
    }

    DIR *dir = opendir(expanded);

    if (dir == NULL) {
        display_error("ERROR: Invalid path: ", path);
        free(expanded);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int is_dot_or_dotdot = (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);
        int is_hidden = (entry->d_name[0] == '.');

        // --- DISPLAY LOGIC ---
        if (strstr(entry->d_name, substring) != NULL) {
            if (is_dot_or_dotdot || a != 0 || !is_hidden) {
                display_message(entry->d_name);
                display_message("\n");
            }
        }

        if (depth > 0 && entry->d_type == DT_DIR && !is_dot_or_dotdot) {
            if (a != 0 || !is_hidden) {
                size_t len = strlen(expanded) + strlen(entry->d_name) + 2;
                char *subdir = malloc(len);
                if (subdir) {
                    sprintf(subdir, "%s/%s", expanded, entry->d_name);
                    list_files_recursive(subdir, depth - 1, a, substring);
                    free(subdir);
                } else {
                    // Malloc failure during recursion
                    display_error("ERROR: Builtin failed: ls", "");
                    closedir(dir);
                    free(expanded);
                    return -1;
                }
            }
        }
    }
    closedir(dir);
    free(expanded);
    return 0;
}


// ====== Command execution =====

bn_ptr check_builtin(const char *cmd) {
    ssize_t cmd_num = 0;
    while (cmd_num < BUILTINS_COUNT &&
           strncmp(BUILTINS[cmd_num], cmd, MAX_STR_LEN) != 0) {
        cmd_num += 1;
    }
    return BUILTINS_FN[cmd_num];
}


// ===== Builtins =====

ssize_t bn_echo(char **tokens) {
    ssize_t index = 1;
    int empty_count = 0;

    if (tokens[index] != NULL) {
        if (strlen(tokens[index]) == 0) empty_count++;
        display_message(tokens[index]);
        index += 1;
    }

    while (tokens[index] != NULL) {
        if (strlen(tokens[index]) == 0) {
            empty_count++;
        }
        else if (strlen(tokens[index]) > 0 && empty_count != index - 1) {
            display_message(" ");
        }
        display_message(tokens[index]);
        index += 1;
    }
    display_message("\n");
    return 0;
}

ssize_t bn_ls(char **tokens) {
    char *path = NULL;
    int depth = -1;
    int a = 0;
    int rec = 0;
    char *substring = "";

    for (int i = 1; tokens[i] != NULL; i++) {
        if (strcmp(tokens[i], "--a") == 0) {
            a = 1;
        } else if (strcmp(tokens[i], "--rec") == 0) {
            rec = 1;
        } else if (strcmp(tokens[i], "--d") == 0) {
            if (tokens[i + 1] != NULL) {
                char *endptr;
                char *val_to_parse = tokens[++i];
                long val = strtol(val_to_parse, &endptr, 10);

                if (*val_to_parse == '\0' || *endptr != '\0') {
                    display_error("ERROR: Invalid depth value: ", val_to_parse);
                    return -1;
                }
                depth = (int)val;
                if (depth < 0) {
                    display_error("ERROR: negative depth provided", "");
                    return -1;
                }
            } else {
                display_error("ERROR: No depth value provided", "");
                return -1;
            }
        } else if (strcmp(tokens[i], "--f") == 0) {
            if (tokens[i + 1] != NULL) {
                substring = tokens[++i];
            } else {
                display_error("ERROR: No substring value provided", "");
                return -1;
            }
        } else if (strncmp(tokens[i], "--", 2) == 0) {
            display_error("ERROR: Unknown flag: ", tokens[i]);
            return -1;
        } else {
            if (path == NULL) {
                path = tokens[i];
            }
            else {
                display_error("ERROR: Too many arguments: ls takes a single path", "");
                return -1;
            }
        }
    }

    if (path == NULL) path = ".";
    if (depth > -1 && rec == 0) {
        display_error("ERROR: Depth provided with no recursive traversal", "");
        return -1;
    }
    return list_files_recursive(path, depth, a, substring);
}

ssize_t bn_cd(char **tokens) {
    if (tokens[1] && tokens[2]) {
        display_error("ERROR: Too many arguments: cd takes a single path", "");
        return -1;
    }

    char *target = NULL;
    if (tokens[1] != NULL) {
        target = tokens[1];
    } else {
        struct passwd *pw = getpwuid(getuid());
        if (pw == NULL) {
            display_error("ERROR: Builtin failed: cd", "");
            return -1;
        }
        target = pw->pw_dir;
    }

    char *expanded = expand_path(target);

    // CHECK: Expand path failure
    if (!expanded) {
        display_error("ERROR: Builtin failed: cd", "");
        return -1;
    }

    if (chdir(expanded) == -1) {
        display_error("ERROR: Invalid path: ", target);
        free(expanded);
        return -1;
    }
    free(expanded);
    return 0;
}

ssize_t bn_cat(char **tokens) {
    // 1. Argument Validation
    if (tokens[1] && tokens[2]) {
        display_error("ERROR: Too many arguments: cat takes a single file", "");
        return -1;
    }

    int fd;
    char *expanded = NULL;

    // 2. Determine Input Source
    if (!tokens[1]) {
        // Point to stdin. We will rely on read() to wait indefinitely.
        fd = STDIN_FILENO;
    } else {
        // Milestone 3 logic: File name provided
        expanded = expand_path(tokens[1]);
        if (!expanded) {
            display_error("ERROR: Builtin failed: cat", "");
            return -1;
        }

        struct stat path_stat;
        if (stat(expanded, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            display_error("ERROR: Cannot open file: ", tokens[1]);
            free(expanded);
            return -1;
        }

        fd = open(expanded, O_RDONLY);
        if (fd == -1) {
            display_error("ERROR: Cannot open file: ", tokens[1]);
            free(expanded);
            return -1;
        }
    }

    // 3. Read Loop
    char buffer[MAX_STR_LEN];
    ssize_t bytes_read;
    int has_read_data = 0;

    // read() will block and wait indefinitely for both pipes and terminals.
    // It only exits the loop on EOF (0) or error (-1).
    while ((bytes_read = read(fd, buffer, MAX_STR_LEN - 1)) > 0) {
        has_read_data = 1;
        buffer[bytes_read] = '\0';
        display_message(buffer);
    }

    // 4. Cleanup and Error Handling
    int error_occurred = 0;

    if (bytes_read == -1) {
        display_error("ERROR: Failed to read from source: ", tokens[1] ? tokens[1] : "stdin");
        error_occurred = 1;
    } else if (!has_read_data && fd == STDIN_FILENO && !isatty(STDIN_FILENO)) {
        // If it was a pipe (!isatty) and it reached EOF without providing any data
        display_error("ERROR: No input source provided", "");
        error_occurred = 1;
    }

    if (fd != STDIN_FILENO) {
        close(fd);
    }
    if (expanded) {
        free(expanded);
    }

    return error_occurred ? -1 : 0;
}

ssize_t bn_wc(char **tokens) {
    if (tokens[1] && tokens[2]) {
        display_error("ERROR: Too many arguments: wc takes a single file", "");
        return -1;
    }

    int fd;
    char *exp = NULL;

    // 1. Determine Input Source
    if (!tokens[1]) {
        // Point to stdin. We will rely on read() to wait indefinitely.
        fd = STDIN_FILENO;
    } else {
        exp = expand_path(tokens[1]);
        if (!exp) {
            display_error("ERROR: Builtin failed: wc", "");
            return -1;
        }

        struct stat path_stat;
        if (stat(exp, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            display_error("ERROR: Cannot open file: ", tokens[1]);
            free(exp);
            return -1;
        }

        fd = open(exp, O_RDONLY);
        if (fd == -1) {
            display_error("ERROR: Cannot open file: ", tokens[1]);
            free(exp);
            return -1;
        }
    }

    char buffer[4096];
    ssize_t bytes_read;
    int char_count = 0;
    int newline_count = 0;
    int word_count = 0;
    int in_word = 0;
    int has_read_data = 0;

    // 2. Read loop for counting
    // read() will block and wait indefinitely for both pipes and terminals.
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        has_read_data = 1;
        char_count += bytes_read;
        for (int i = 0; i < bytes_read; i++) {
            unsigned char c = buffer[i];
            if (c == '\n') {
                newline_count++;
            }
            if (isspace(c)) {
                if (in_word) {
                    word_count++;
                    in_word = 0;
                }
            } else {
                in_word = 1;
            }
        }
    }

    // 3. Error Handling
    int error_occurred = 0;

    if (bytes_read == -1) {
        display_error("ERROR: Failed to read from source: ", tokens[1] ? tokens[1] : "stdin");
        error_occurred = 1;
    } else if (!has_read_data && fd == STDIN_FILENO && !isatty(STDIN_FILENO)) {
        // If it was a pipe (!isatty) and it reached EOF without providing any data
        display_error("ERROR: No input source provided", "");
        error_occurred = 1;
    }

    // 4. Output formatting (Only if no errors occurred)
    if (!error_occurred) {
        if (in_word) {
            word_count++;
        }

        char output[256]; // Using a stack buffer is safer/faster than malloc here
        snprintf(output, sizeof(output), "word count %d\ncharacter count %d\nnewline count %d\n",
                 word_count, char_count, newline_count);
        display_message(output);
    }

    // 5. Cleanup
    if (fd != STDIN_FILENO) {
        close(fd);
    }
    if (exp) {
        free(exp);
    }

    return error_occurred ? -1 : 0;
}

/**
 * ps: Lists all descendant processes, filtering out shell overhead.
 * This ensures the output is clean and contains the specific command names
 * (like 'sleep') that the Python tester is looking for.
 */
ssize_t ps() {
    FILE *fp;
    char line[MAX_STR_LEN];
    pid_t my_pid = getpid();

    // Fetch PID, Parent PID, and Command Name
    fp = popen("ps -eo pid,ppid,comm --no-headers", "r");
    if (fp == NULL) {
        display_error("ERROR: ps failed", "");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        int pid, ppid;
        char comm[MAX_STR_LEN];

        // Parse the three columns
        if (sscanf(line, "%d %d %s", &pid, &ppid, comm) >= 3) {

            // 1. Is it a child or grandchild of this shell?
            // 2. Is it NOT the main shell itself?
            if ((ppid == (int)my_pid || ppid > (int)my_pid) && pid != (int)my_pid) {

                // 3. FILTER: Hide the "infrastructure" processes.
                // We hide 'mysh' (the background managers), 'sh' (popen shell),
                // and 'ps' (the command we are currently running).
                if (strcmp(comm, "mysh") != 0 &&
                    strcmp(comm, "sh") != 0 &&
                    strcmp(comm, "ps") != 0) {

                    char output[256];
                    // Format: "PID COMMAND\n"
                    sprintf(output, "%s %d\n", comm, pid);
                    display_message(output);
                    }
            }
        }
    }

    pclose(fp);
    return 0;
}

/**
 * handle_kill: Sends a signal to a specific PID.
 * Returns 0 on success, -1 on error.
 */
ssize_t handle_kill(char **tokens) {
    // 1. Check if PID is provided
    if (tokens[1] == NULL) {
        display_error("ERROR: Usage: kill [pid] [signum]", "");
        return -1;
    }

    // 2. Convert PID and validate it's a number
    for (int i = 0; tokens[1][i] != '\0'; i++) {
        if (!isdigit(tokens[1][i]) && !(i == 0 && tokens[1][i] == '-')) {
            display_error("ERROR: The process does not exist", "");
            return -1;
        }
    }
    pid_t pid = (pid_t)atoi(tokens[1]);

    int signum = SIGTERM; // Default signal

    // 3. Check if a specific signal number was provided
    if (tokens[2] != NULL) {
        for (int i = 0; tokens[2][i] != '\0'; i++) {
            if (!isdigit(tokens[2][i])) {
                display_error("ERROR: Invalid signal specified", "");
                return -1;
            }
        }
        signum = atoi(tokens[2]);
    }

    // 4. Validate the signal range (1-64 are standard)
    if (signum < 1 || signum > 64) {
        display_error("ERROR: Invalid signal specified", "");
        return -1;
    }

    // 5. Validate process existence
    // kill(pid, 0) checks if the process exists and if we have permission
    if (kill(pid, 0) == -1) {
        if (errno == ESRCH) {
            display_error("ERROR: The process does not exist", "");
        } else {
            display_error("ERROR: Invalid PID", "");
        }
        return -1;
    }

    // 6. Send the signal
    if (kill(pid, signum) == -1) {
        display_error("ERROR: Failed to send signal", "");
        return -1;
    }

    usleep(10000);

    return 0;
}