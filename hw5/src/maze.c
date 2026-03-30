#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "maze.h"
#include "debug.h"

static pthread_mutex_t maze_mtx = PTHREAD_MUTEX_INITIALIZER;

static int rows = 0;
static int cols = 0;
static OBJECT **cell = NULL;

static const int drow[NUM_DIRECTIONS] = {-1, 0, 1, 0};
static const int dcol[NUM_DIRECTIONS] = {0, -1, 0, 1};

static inline OBJECT safe_get(int r, int c) {
	if (r>=0 && r<rows && c>=0 && c<cols)
		return cell[r][c];
	return EMPTY;
}

void maze_init(char **template) {

	for (rows = 0; template[rows] != NULL; rows++) continue;
	cols = (int)strlen(template[0]);

	cell = malloc(rows * sizeof *cell);

	for (int r = 0; r < rows; r++) {
		cell[r] = malloc(cols);
		memcpy(cell[r], template[r], cols);
	}

	srand((unsigned)time(NULL));
	//pthread_mutex_init(&maze_mtx, NULL);
	debug("Maze initialised (%dx%d)", rows, cols);
}

void maze_fini(void) {
	if (!cell) return;

	for (int r = 0; r<rows; r++)
		free(cell[r]);
	free(cell);

	cell = NULL;
	rows = cols = 0;
	pthread_mutex_destroy(&maze_mtx);
}

int maze_get_rows(void) {
	return rows;
}

int maze_get_cols(void) {
	return cols;
}

int maze_set_player(OBJECT avatar, int r, int c) {
	pthread_mutex_lock(&maze_mtx);
	int rc = -1;

	if (r>=0 && r<rows && c>=0 && c<cols && IS_EMPTY(cell[r][c])) {
		cell[r][c] = avatar;
		rc = 0;
	}

	pthread_mutex_unlock(&maze_mtx);
	return rc;
}

int maze_set_player_random(OBJECT avatar, int *rowp, int *colp) {

	unsigned int seed = (unsigned)time(NULL) ^ (unsigned)pthread_self();

	for (int tries = 0; tries < 10000; tries++) {     // Setting the max number of tries for placing an avatar to 10000
		int r = rand_r(&seed) % rows;
		int c = rand_r(&seed) % cols;

		if (maze_set_player(avatar, r, c) == 0) {
			if (rowp) *rowp = r;
			if (colp) *colp = c;
			return 0;
		}
	}
	return -1;
}

void maze_remove_player(OBJECT avatar, int r, int c) {
	pthread_mutex_lock(&maze_mtx);

	if (r>=0 && r<rows && c>=0 && c<cols && cell[r][c] == avatar)
		cell[r][c] = EMPTY;
	pthread_mutex_unlock(&maze_mtx);
}

int maze_move(int r, int c, int dir) {

	if (dir<0 || dir>=NUM_DIRECTIONS) return -1;

	pthread_mutex_lock(&maze_mtx);

	if (r<0 || r>=rows || c<0 || c>=cols || !IS_AVATAR(cell[r][c])) {
		pthread_mutex_unlock(&maze_mtx);
		return -1;
	}

	int r2 = r + drow[dir];
	int c2 = c + dcol[dir];

	if (r2<0 || r2>=rows || c2<0 || c2>=cols || !IS_EMPTY(cell[r2][c2])) {
		pthread_mutex_unlock(&maze_mtx);
		return -1;
	}

	OBJECT av = cell[r][c];
	cell[r][c] = EMPTY;
	cell[r2][c2] = av;

	pthread_mutex_unlock(&maze_mtx);
	return 0;
}

OBJECT maze_find_target(int r, int c, DIRECTION dir) {

	pthread_mutex_lock(&maze_mtx);
	r += drow[dir];
	c += dcol[dir];

	while (r>=0 && r<rows && c>=0 && c<cols) {
		OBJECT obj = cell[r][c];
		if (!IS_EMPTY(obj)) {
			pthread_mutex_unlock(&maze_mtx);
			return IS_AVATAR(obj) ? obj : EMPTY;
		}
		r += drow[dir];
		c += dcol[dir];
	}

	pthread_mutex_unlock(&maze_mtx);
	return EMPTY;
}

int maze_get_view(VIEW *view, int r, int c, DIRECTION gaze, int depth) {
	if (depth > VIEW_DEPTH) depth = VIEW_DEPTH;

	pthread_mutex_lock(&maze_mtx);

	for (int d = 0; d<depth; d++) {

		r += drow[gaze];
        c += dcol[gaze];

        if (r < 0 || r >= rows || c < 0 || c >= cols) {
            depth = d;
            break;
        }

        (*view)[d][CORRIDOR] = cell[r][c];

        int ldir = TURN_LEFT(gaze);
        int rdir = TURN_RIGHT(gaze);
        (*view)[d][LEFT_WALL]  = safe_get(r + drow[ldir], c + dcol[ldir]);
        (*view)[d][RIGHT_WALL] = safe_get(r + drow[rdir], c + dcol[rdir]);

	}

	pthread_mutex_unlock(&maze_mtx);
	return depth;
}

void show_view(VIEW *v, int depth) {
	#ifdef DEBUG
		for (int d=0; d<depth; d++) {
			fprintf(stderr, "%02d %c%c%c\n", d, (*v)[d][LEFT_WALL], (*v)[d][CORRIDOR], (*v)[d][RIGHT_WALL]);
		}
	#endif
}

void show_maze(void) {
	#ifdef DEBUG
		pthread_mutex_lock(&maze_mtx);
		for (int r=0; r<rows; r++) {
			fwrite(cell[r], 1, cols, stderr);
			fputc('\n', stderr);
		}

		pthread_mutex_unlock(&maze_mtx);
	#endif
}