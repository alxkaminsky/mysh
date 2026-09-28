#ifndef __IO_HELPERS_H__
#define __IO_HELPERS_H__

#include <sys/types.h>
#include "variables.h"


#define MAX_STR_LEN 128
#define DELIMITERS " \t\n"     // Assumption: all input tokens are whitespace delimited


typedef struct JobNode {
    pid_t pid;
    int job_id;
    char *command;
    struct JobNode *next;
} JobNode;

typedef struct ClientNode {
    int fd;
    int id;
    struct ClientNode *next;
} ClientNode;

void add_job(JobNode **head, pid_t pid, int job_id, char *cmd);
char* remove_job(JobNode **head, pid_t pid, int *out_job_id);
void free_jobs(JobNode *head);

void add_client(int fd, int connections);
void remove_client(int id);
void free_clients();

/* Prereq: pre_str, str are NULL terminated string
 */
void display_message(char *str);
void display_error(char *pre_str, char *str);


/* Prereq: in_ptr points to a character buffer of size > MAX_STR_LEN
 * Return: number of bytes read
 */
int get_input(char **in_ptr);
void argv_to_chunk(int argc, char **argv, char **chunk_ptr);
/* Prereq: in_ptr is a string, tokens is of size >= len(in_ptr)
 * Warning: in_ptr is modified
 * Return: number of tokens.
 */
size_t tokenize_input(char *in_ptr, char **tokens);

void expand_variables(char **token_ptr_ptr, Node *head, int bypass_truncation);

#endif
