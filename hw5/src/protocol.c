#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "protocol.h"

static int writen(int fd, const void *buf, size_t n) {
	const uint8_t *p = buf;

	while (n>0) {
		ssize_t k = write(fd, p, n);
		if (k<0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += k;
		n -= (size_t)k;
	}

	return 0;
}

static int readn(int fd, void *buf, size_t n) {
	uint8_t *p = buf;

	while (n>0) {
		ssize_t k = read(fd, p, n);

		if (k == 0) {
			errno = EPIPE;
			return -1;
		}

		if (k<0) {
			return -1;
		}
		p += k;
		n -= (size_t)k;
	}

	return 0;
}

int proto_send_packet(int fd, MZW_PACKET *pkt, void *data) {
	if (!pkt) {
		errno = EINVAL;
		return -1;
	}

	MZW_PACKET net = *pkt;
	net.size = htons(pkt->size);
	net.timestamp_sec = htonl(pkt->timestamp_sec);
	net.timestamp_nsec = htonl(pkt->timestamp_nsec);

	if (writen(fd, &net, sizeof net) < 0) return -1;

	if (pkt->size) {
		if (!data) {
			errno = EINVAL;
			return -1;
		}
		if (writen(fd, data, pkt->size) < 0) return -1;
	}

	return 0;
}

int proto_recv_packet(int fd, MZW_PACKET *pkt, void **datap) {
	if (!pkt || !datap) {
		errno = EINVAL;
		return -1;
	}
	*datap = NULL;

	if (readn(fd, pkt, sizeof *pkt) < 0) return -1;

	pkt->size = ntohs(pkt->size);
	pkt->timestamp_sec = ntohl(pkt->timestamp_sec);
	pkt->timestamp_nsec = ntohl(pkt->timestamp_nsec);

	if (pkt->size) {
		void *payload = malloc(pkt->size);
		if (!payload) return -1;

		if (readn(fd, payload, pkt->size) < 0) {
			free(payload);
			return -1;
		}
		*datap = payload;
	}

	return 0;
}