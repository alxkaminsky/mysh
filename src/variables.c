#include <stdio.h>
#include <string.h>
#include "variables.h"
#include "io_helpers.h"

#include <stdlib.h>

Node* create(char *key, char* value) {
    Node *new = malloc(sizeof(Node));
    if (new==NULL) {
        display_error("ERROR: memory limit reached", "");
        return NULL;
    }

    new->name = key;
    new->value = value;
    new->next = NULL;

    return new;
}

char* find(char *key, Node *head_ptr) {
    while (head_ptr != NULL) {
        if (strcmp(head_ptr->name, key) == 0) {
            return head_ptr->value;
        }
        head_ptr = head_ptr->next;
    }
    return ""; //not found
}

void define_var(char *key, char *value, Node **head_ptr_ptr) {
    if (*head_ptr_ptr == NULL) {
        *head_ptr_ptr = create(key, value);
        return;
    }
    Node *curr = *head_ptr_ptr;
    Node *prev = NULL;

    while(curr != NULL) {
        //check if user re-defined
        if (strcmp(curr->name, key) == 0) {
            free(curr->value);
            free(key);
            curr->value = value;
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    prev->next = create(key, value);
}

int free_all(Node *head_ptr) {
    int freed = 0;
    Node *curr = head_ptr;

    while (curr != NULL) {
        curr = head_ptr->next;
        free(head_ptr->name);
        free(head_ptr->value);
        free(head_ptr);
        head_ptr = curr;
        freed += 1;
    }
    return freed;
}