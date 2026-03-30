#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>

#include "client_registry.h"

struct fd_node {
	int fd;
	struct fd_node *next;
};

struct client_registry{
	pthread_mutex_t lock;
	sem_t empty;
	int n_clients;
	struct fd_node *head;
};

CLIENT_REGISTRY *client_registry = NULL;

static void fd_list_insert(struct client_registry *cr, int fd) {
	struct fd_node *node = malloc(sizeof *node);
	if (!node) abort();

	node->fd = fd;
	node->next = cr->head;
	cr->head = node;
}

static int fd_list_remove(struct client_registry *cr, int fd) {
	struct fd_node **pp = &cr->head;

	while (*pp) {
		if ((*pp)->fd == fd) {
			struct fd_node *victim = *pp;
			*pp = victim->next;
			free(victim);
			return 0;
		}
		pp = &(*pp)->next;
	}
	return -1;
}

CLIENT_REGISTRY *creg_init() {
	CLIENT_REGISTRY *cr = calloc(1, sizeof *cr);
	if (!cr) return NULL;

	if (pthread_mutex_init(&cr->lock, NULL) != 0) {
		free(cr);
		return NULL;
	}

	if (sem_init(&cr->empty, 0, 0) != 0) {
		pthread_mutex_destroy(&cr->lock);
		free(cr);
		return NULL;
	}

	client_registry = cr;
	return cr;
}

void creg_fini(CLIENT_REGISTRY *cr) {
	if (!cr) return;

	struct fd_node *n = cr->head;
	while (n) {
		struct fd_node *next = n->next;
		free(n);
		n = next;
	}

	pthread_mutex_destroy(&cr->lock);
	sem_destroy(&cr->empty);
	free(cr);
	client_registry = NULL;
}

void creg_register(CLIENT_REGISTRY *cr, int fd) {
	pthread_mutex_lock(&cr->lock);
	fd_list_insert(cr, fd);

	if (cr->n_clients == 0) {
		sem_trywait(&cr->empty);
	}
	cr->n_clients++;
	pthread_mutex_unlock(&cr->lock);
}

void creg_unregister(CLIENT_REGISTRY *cr, int fd) {
	pthread_mutex_lock(&cr->lock);

	if (fd_list_remove(cr, fd) == 0) {
		cr->n_clients--;
		if (cr->n_clients == 0) sem_post(&cr->empty);
	}
	pthread_mutex_unlock(&cr->lock);
}

void creg_wait_for_empty(CLIENT_REGISTRY *cr) {
	pthread_mutex_lock(&cr->lock);
	int empty = (cr->n_clients == 0);
	pthread_mutex_unlock(&cr->lock);

	if (empty) return;

	while (sem_wait(&cr->empty) == -1 && errno == EINTR) continue;
}

void creg_shutdown_all(CLIENT_REGISTRY *cr) {
	pthread_mutex_lock(&cr->lock);

	for (struct fd_node *n = cr->head; n; n = n->next)
		shutdown(n->fd, SHUT_RD);

	pthread_mutex_unlock(&cr->lock);

}