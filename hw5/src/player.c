#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "player.h"
#include "client_registry.h"
#include "maze.h"
#include "protocol.h"
#include "server.h"
#include "debug.h"

struct player {
	pthread_mutex_t mtx;
	int refcnt;
	unsigned char avatar;
	char *name;
	int clientfd;
	pthread_t servant;

	int row, col;
	DIRECTION gaze;
	int score;
	int hits_pending;
	int view_valid;
	char prev_view[VIEW_DEPTH][VIEW_WIDTH];
	int prev_depth;
};

static const int drow[NUM_DIRECTIONS] = { -1,  0,  1,  0 };
static const int dcol[NUM_DIRECTIONS] = {  0, -1,  0,  1 };

static PLAYER *pmap[256] = { NULL };
static pthread_mutex_t pmap_mtx = PTHREAD_MUTEX_INITIALIZER;

static void sigusr1_handler(int sig);

static __thread PLAYER *tls_self = NULL;

static inline int encode_object(char ch, MZW_PACKET *pkt, char *payload_buf) {
	if (IS_EMPTY(ch)) return 0;

	if (IS_AVATAR(ch)) {
		pkt->param1 = MZW_PLAYER;
		pkt->size = 1;
		*payload_buf = ch;
	} else if (IS_WALL(ch)) {
		pkt->param1 = MZW_WALL;
		pkt->size = 1;
		*payload_buf = ch;
	} else {
		pkt->param1 = MZW_DOOR;
		pkt->size = 0;
	}
	return 1;
}

static int snapshot_players(PLAYER *out[256]) {
	int n = 0;
	pthread_mutex_lock(&pmap_mtx);
	for (int i=0; i<256; i++) {
		if (pmap[i]) {
			out[n] = player_ref(pmap[i], "snapshot");
			n++;
		}
	}
	pthread_mutex_unlock(&pmap_mtx);
	return n;
}

static void release_snapshot(PLAYER *arr[], int n) {
	for (int i = 0; i<n; i++) player_unref(arr[i], "snapshot");
}

static void init_recursive_mutex(pthread_mutex_t *m) {

	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(m, &a);
	pthread_mutexattr_destroy(&a);
}

PLAYER *player_ref(PLAYER *p, char *why) {

	(void)why;
	pthread_mutex_lock(&p->mtx);
	p->refcnt++;
	pthread_mutex_unlock(&p->mtx);
	return p;
}

void player_unref(PLAYER *p, char *why) {

	(void)why;
	pthread_mutex_lock(&p->mtx);
	if (--p->refcnt == 0) {
		pthread_mutex_unlock(&p->mtx);
		pthread_mutex_destroy(&p->mtx);
		free(p->name);
		free(p);
		return;
	}
	pthread_mutex_unlock(&p->mtx);
}

void player_init(void) {
	struct sigaction sa = { .sa_handler = sigusr1_handler };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	debug("Player module initialized");
}

void player_fini(void) {
	pthread_mutex_lock(&pmap_mtx);
	for (int i=0; i<256; i++) {
		if (pmap[i]) player_unref(pmap[i], "module fini");
	}

	pthread_mutex_unlock(&pmap_mtx);
}

static void sigusr1_handler(int sig) {
	(void)sig;
	if (tls_self) tls_self->hits_pending = 1;
}

PLAYER* player_login(int fd, OBJECT avatar, char *name) {

	pthread_mutex_lock(&pmap_mtx);

	if (!IS_AVATAR(avatar)) avatar = 'A';
	if (pmap[avatar]) {
		for (int i = 'A'; i<='Z'; i++) {
			if (!pmap[i]) {
				avatar = (unsigned char)i;
				break;
			}
		}
		if (pmap[avatar]) {
			pthread_mutex_unlock(&pmap_mtx);
			return NULL;
		}
	}

	PLAYER *p = calloc(1, sizeof *p);
	init_recursive_mutex(&p->mtx);
	p->refcnt = 1;
	p->avatar = avatar;
	p->name = strdup(name);
	p->clientfd = fd;
	p->servant = pthread_self();
	p->gaze = NORTH;
	p->row = p->col = -1;

	pmap[avatar] = p;
	pthread_mutex_unlock(&pmap_mtx);

	tls_self = p;
	debug("Player %c logged in (fd=%d)", avatar, fd);
	return p;
}

void player_logout(PLAYER *p) {

	pthread_mutex_lock(&pmap_mtx);
	if (pmap[p->avatar] == p) pmap[p->avatar] = NULL;
	pthread_mutex_unlock(&pmap_mtx);

	maze_remove_player(p->avatar, p->row, p->col);

	MZW_PACKET pkt = {.type = MZW_SCORE_PKT, .param1 = p->avatar, .param2 = -1, .size = 0};

	for (int i=0; i<256; i++) {
		if (pmap[i]) player_send_packet(pmap[i], &pkt, NULL);
	}

	player_unref(p, "logout");
	tls_self = NULL;
}

void player_reset(PLAYER *p) {

	maze_remove_player(p->avatar, p->row, p->col);
	if (maze_set_player_random(p->avatar, &p->row, &p->col) != 0) {
		shutdown(p->clientfd, SHUT_RDWR);
		return;
	}

	PLAYER *list[256];
	int n = snapshot_players(list);

	for (int i=0; i<n; i++) {
		player_invalidate_view(list[i]);
		player_update_view(list[i]);
	}

	for (int i=0; i<n; i++) {
		PLAYER *src = list[i];
		MZW_PACKET s = { .type = MZW_SCORE_PKT,
						 .param1 = src->avatar,
						 .param2 = src->score,
						 .size = 0};
		for (int j=0; j<n; j++) {
			player_send_packet(list[j], &s, NULL);
		}
	}

	release_snapshot(list, n);
}

PLAYER* player_get(unsigned char avatar) {
	pthread_mutex_lock(&pmap_mtx);
	PLAYER *p = pmap[avatar];

	if (p) player_ref(p, "player_get");
	pthread_mutex_unlock(&pmap_mtx);
	return p;
}

int player_send_packet(PLAYER *p, MZW_PACKET *pkt, void *data) {
	pthread_mutex_lock(&p->mtx);
	int rc = proto_send_packet(p->clientfd, pkt, data);
	pthread_mutex_unlock(&p->mtx);
	return rc;
}

int player_get_location(PLAYER *p, int *rp, int *cp, int *dp) {
	pthread_mutex_lock(&p->mtx);
	if (p->row<0) {
		pthread_mutex_unlock(&p->mtx);
		return -1;
	}
	if (rp) *rp = p->row;
	if (cp) *cp = p->col;
	if (dp) *dp = p->gaze;
	pthread_mutex_unlock(&p->mtx);
	return 0;
}

int player_move(PLAYER *p, int dir) {
	int r,c,d;

	if (player_get_location(p, &r, &c, &d) != 0) return -1;

	int target_dir = (dir>0) ? d : REVERSE(d);
	if (maze_move(r, c, target_dir) == 0) {
		if (debug_show_maze) show_maze();

		player_invalidate_view(p);
		pthread_mutex_lock(&p->mtx);
		p->row += drow[target_dir];
		p->col += dcol[target_dir];
		pthread_mutex_unlock(&p->mtx);

		PLAYER *list[256];
		int n = snapshot_players(list);
		for (int i=0; i<n; i++){
			player_update_view(list[i]);
		}
		release_snapshot(list, n);
		return 0;
	}
	return -1;
}

void player_rotate(PLAYER *p, int sign) {
	pthread_mutex_lock(&p->mtx);
	p->gaze = (sign>0) ? TURN_LEFT(p->gaze) : TURN_RIGHT(p->gaze);
	p->view_valid = 0;
	pthread_mutex_unlock(&p->mtx);
	player_update_view(p);
}

void player_fire_laser(PLAYER *p) {
	int r,c,d;
	if (player_get_location(p, &r, &c, &d) != 0) return;
	OBJECT tgt = maze_find_target(r,c,d);
	if (!IS_AVATAR(tgt)) return;

	PLAYER *victim = player_get(tgt);
	if (!victim) return;

	pthread_mutex_lock(&p->mtx);
	p->score++;
	pthread_mutex_unlock(&p->mtx);

	PLAYER *list[256];
	int n = snapshot_players(list);

	MZW_PACKET sc = { .type = MZW_SCORE_PKT,
					 .param1 = p->avatar,
					 .param2 = p->score };
	MZW_PACKET rm = { .type = MZW_SCORE_PKT,
					  .param1 = victim->avatar,
					  .param2 = -1};

	for (int i=0; i<n; i++) {
		player_send_packet(list[i], &sc, NULL);
		player_send_packet(list[i], &rm, NULL);
	}
	release_snapshot(list, n);

	pthread_mutex_lock(&victim->mtx);
	victim->hits_pending = 1;
	pthread_kill(victim->servant, SIGUSR1);
	pthread_mutex_unlock(&victim->mtx);

	player_unref(victim, "laser fire");
}

void player_check_for_laser_hit(PLAYER *p) {
	if (!p->hits_pending) return;

	pthread_mutex_lock(&p->mtx);
	p->hits_pending = 0;
	pthread_mutex_unlock(&p->mtx);

	maze_remove_player(p->avatar, p->row, p->col);
	pthread_mutex_lock(&p->mtx);
	p->row = p->col = -1;
	pthread_mutex_unlock(&p->mtx);

	PLAYER *list[256];
	int n = snapshot_players(list);

	for (int i=0; i<n; i++) {
		player_invalidate_view(list[i]);
		player_update_view(list[i]);
	}
	release_snapshot(list, n);

	MZW_PACKET a = {.type = MZW_ALERT_PKT};
	player_send_packet(p, &a, NULL);

	sleep(3);
	player_reset(p);
}

void player_invalidate_view(PLAYER *p) {
	pthread_mutex_lock(&p->mtx);
	p->view_valid = 0;
	pthread_mutex_unlock(&p->mtx);
}

void player_update_view(PLAYER *p) {

	char new_view[VIEW_DEPTH][VIEW_WIDTH];

	pthread_mutex_lock(&p->mtx);

	int depth = maze_get_view((VIEW *)&new_view, p->row, p->col, p->gaze, VIEW_DEPTH);

	if (!p->view_valid) {
		MZW_PACKET clr = { .type = MZW_CLEAR_PKT, .size = 0 };
		player_send_packet(p, &clr, NULL);

		for (int d=0; d<depth; d++) {
			for (int w=0; w<VIEW_WIDTH; w++) {
				MZW_PACKET sh = { .type = MZW_SHOW_PKT,
								  .param1 = new_view[d][w],
								  .param2 = d,
								  .param3 = w,
								  .size = 0 };
				player_send_packet(p, &sh, NULL);
			}
		}
	} else {
		int old_depth = p->prev_depth;

		for (int d=0; d<depth; d++) {
			for (int w=0; w<VIEW_WIDTH; w++) {
				if (new_view[d][w] == p->prev_view[d][w]) continue;

				MZW_PACKET sh = { .type = MZW_SHOW_PKT,
								  .param1 = new_view[d][w],
								  .param2 = d,
								  .param3 = w,
								  .size = 0 };
				player_send_packet(p, &sh, NULL);
			}
		}

		for (int d=depth; d<old_depth; d++) {
			for (int w=0; w<VIEW_WIDTH; w++) {
				if (p->prev_view[d][w] == EMPTY) continue;

				MZW_PACKET sh = { .type = MZW_SHOW_PKT,
								  .param1 = EMPTY,
								  .param2 = d,
								  .param3 = w,
								  .size = 0 };
				player_send_packet(p, &sh, NULL);
			}
		}
	}

	memcpy(p->prev_view, new_view, sizeof(new_view));
	p->prev_depth = depth;
	p->view_valid = 1;
	pthread_mutex_unlock(&p->mtx);
}

void player_send_chat(PLAYER *snd, char *msg, size_t len) {
	char text[256];
	int n = snprintf(text, sizeof text, "%s[%c] %.*s", snd->name, snd->avatar, (int)len, msg);

	if (n < 0) return;
	if (n >= (int)sizeof text) n = sizeof text-1;

	MZW_PACKET pkt = {.type = MZW_CHAT_PKT, .size = n};

	PLAYER *list[256];
	int cnt = snapshot_players(list);
	for (int i=0; i<cnt; i++) {
		player_send_packet(list[i], &pkt, text);
	}
	release_snapshot(list, cnt);
}