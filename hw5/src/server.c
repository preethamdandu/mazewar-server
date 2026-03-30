#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>

#include "server.h"
#include "protocol.h"
#include "player.h"
#include "debug.h"

int debug_show_maze = 0;

void *mzw_client_service(void *arg) {
	int fd = *((int *)arg);
	free(arg);

	pthread_detach(pthread_self());
	creg_register(client_registry, fd);

	PLAYER *player = NULL;
	int logged_in = 0;

	while(1) {
		if (logged_in)
			player_check_for_laser_hit(player);

		MZW_PACKET pkt;
		void *payload = NULL;

		if (proto_recv_packet(fd, &pkt, &payload) < 0) {
			if (errno == EINTR) {
				if (payload) free(payload);
				continue;
			}
			if (payload) free(payload);
			break;
		}

		switch(pkt.type) {
		case MZW_LOGIN_PKT:
			if (logged_in) break;
			if (pkt.size == 0 || !payload) break;

			char *uname = malloc(pkt.size + 1);
			memcpy(uname, payload, pkt.size);
			uname[pkt.size] = '\0';

			player = player_login(fd, (unsigned char)pkt.param1, uname);
			free(uname);

			if (player) {
				logged_in = 1;
				MZW_PACKET rep = { .type = MZW_READY_PKT, .size = 0 };
				proto_send_packet(fd, &rep, NULL);
				player_reset(player);
			} else {
				MZW_PACKET rep = { .type = MZW_INUSE_PKT, .size = 0 };
				proto_send_packet(fd, &rep, NULL);
			}
			break;

		case MZW_MOVE_PKT:
			if (logged_in)
				player_move(player, pkt.param1);
			break;

		case MZW_TURN_PKT:
			if (logged_in)
				player_rotate(player, pkt.param1);
			break;

		case MZW_FIRE_PKT:
			if (logged_in)
				player_fire_laser(player);
			break;

		case MZW_REFRESH_PKT:
			if (logged_in) {
				player_invalidate_view(player);
				player_update_view(player);
			}
			break;

		case MZW_SEND_PKT:
			if (logged_in && payload && pkt.size > 0)
				player_send_chat(player, payload, pkt.size);
			break;

		default:
			break;
		}

		if (payload) free(payload);
	}

	if (logged_in) player_logout(player);

	creg_unregister(client_registry, fd);
	close(fd);
	return NULL;
}