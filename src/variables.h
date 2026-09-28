#ifndef __VARIABLES_H__
#define __VARIABLES_H__

typedef struct Node {
    char *name;
    char *value;
    struct Node *next;
}Node;
//helper function to create a new node with a specified key value
//pair. Implemented to follow the SRP.
Node* create(char *key, char* value);

//Find the node that corresponds to key. If key does not exist,
//return -1. Else, return 0.
char* find(char *key, Node *head_ptr);

int free_all(Node *head_ptr);

void define_var(char *key, char *value,  Node **head_ptr_ptr);

#endif