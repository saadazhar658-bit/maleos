#ifndef MALEOS_KERNEL_LIST_H
#define MALEOS_KERNEL_LIST_H

#include <stdbool.h>
#include <stddef.h>

/* Intrusive circular doubly linked list. A head and its nodes share one type. */
struct list_node {
    struct list_node *next, *prev;
};

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

static inline void list_init(struct list_node *n)
{
    n->next = n->prev = n;
}

static inline bool list_empty(const struct list_node *head)
{
    return head->next == head;
}

static inline void list_add_tail(struct list_node *head, struct list_node *n)
{
    n->prev = head->prev;
    n->next = head;
    head->prev->next = n;
    head->prev = n;
}

static inline void list_del(struct list_node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = n->prev = n;
}

#endif
