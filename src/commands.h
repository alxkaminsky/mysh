#ifndef __COMMANDS_H__
#define __COMMANDS_H__

#include <unistd.h>

void pipe_op(char *left, char *right);
void bg_op(char *command);

/* Type for network command handling functions
 * Input: Array of tokens
 * Return: >=0 on success and -1 on error
 */
typedef int (*cmd_ptr)(char **);

int start_server(char **tokens);
int close_server(char **tokens); // Note: Updated signature to accept char**
int send_message(char **args);
int start_client(char **args);

/* Return: the address of the function handling the command or null */
cmd_ptr check_command(const char *cmd);

/* COMMANDS and COMMANDS_FN are parallel arrays */
static const char * const COMMANDS[] = {"start-server", "close-server", "send", "start-client"};
static const cmd_ptr COMMANDS_FN[] = {start_server, close_server, send_message, start_client};
static const int COMMANDS_COUNT = sizeof(COMMANDS) / sizeof(char *);

#endif