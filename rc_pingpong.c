/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <stdlib.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include "pingpong.h"

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};
int random_value = 0;
struct pingpong_context {
	struct ibv_context	*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;
	void			*buf;
	int			 size;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
};

static int page_size;
        int                      port = 18515;
        int                      ib_port = 1;
        int                      size = 4096;
        enum ibv_mtu             mtu = IBV_MTU_1024;
        int                      rx_depth = 500;
        int                      iters = 1000;
        int                      use_event = 0;
        char                    *ib_devname = NULL;
        char                    *servername = NULL;
        int                      sl = 0;
        int                      gidx = -1;
        char                     gid[33];

struct flow_info{

        struct pingpong_context* ctx;
        struct ibv_device **dev_list;
        int iters;
        struct timeval* start;
        struct timeval* end;
	int routs;
	struct pingpong_dest * rem_dest;
};


struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};

static int pp_post_recv(struct pingpong_context *ctx, int n);

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
                                            int rx_depth, int port,
                                            int use_event, int is_server);
void *flow_control (void * arg);

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx)
{
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num		= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	fprintf(stdout, "modifying to RTR - rq psn %d\n", dest->psn);
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	fprintf(stdout, "modifying to RTS - sq psn %d\n", my_psn);
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];


	if (asprintf(&service, "%d", port) < 0)
		return NULL;
	fprintf(stdout, "printed port to %d\n", port);
	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}
	fprintf(stdout, "getting socket\n");
	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}
	fprintf(stdout, "freeing service\n");
	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
	fprintf(stdout, "sending local addr\n");
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}
	fprintf(stdout, "reading remote addr\n");
	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}
	fprintf(stdout, "sending done\n");
	write(sockfd, "done", sizeof "done");

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	fprintf(stdout, "closing fd\n");
	close(sockfd);
	return rem_dest;
}

static void* pp_server_exch_dest(void * arg) {

	
        struct ibv_device      **dev_list;
        struct ibv_device       *ib_dev;
        struct pingpong_context *ctx;
        struct pingpong_dest*    my_dest = calloc(1, sizeof(struct pingpong_dest));
        struct pingpong_dest    *rem_dest = NULL;
        struct timeval           start, end;

        page_size = sysconf(_SC_PAGESIZE);




	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}

	if (listen(sockfd, 1) < 0) {
		fprintf(stderr, "listen failed\n");
	}
	while (1) {
	connfd = accept(sockfd, NULL, 0);
	
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}
	fprintf(stdout, "reading remote addr\n");
	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	struct pingpong_dest * rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);
	fprintf(stdout, "initting conn\n");
        dev_list = ibv_get_device_list(NULL);
        if (!dev_list) {
                perror("Failed to get IB devices list");
                return NULL;
        }

        if (!ib_devname) {
                ib_dev = *dev_list;
                if (!ib_dev) {
                        fprintf(stderr, "No IB devices found\n");
                        return NULL;
                }
        } else {
                int i;
                for (i = 0; dev_list[i]; ++i)
                        if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                                break;
                ib_dev = dev_list[i];
                if (!ib_dev) {
                        fprintf(stderr, "IB device %s not found\n", ib_devname);
                        return NULL;
                }
        }


        ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port, use_event, !servername);
        if (!ctx)
                return NULL;
	ctx->pending = PINGPONG_RECV_WRID;
        int routs = pp_post_recv(ctx, ctx->rx_depth);
        if (routs < ctx->rx_depth) {
                fprintf(stderr, "Couldn't post receive (%d)\n", routs);
                return NULL;
        }

        if (use_event)
                if (ibv_req_notify_cq(ctx->cq, 0)) {
                        fprintf(stderr, "Couldn't request CQ notification\n");
                        return NULL;
                }


        if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
                fprintf(stderr, "Couldn't get port info\n");
                return NULL;
        }

        my_dest->lid = ctx->portinfo.lid;
        if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest->lid) {
                fprintf(stderr, "Couldn't get local LID\n");
                return NULL;
        }

        if (gidx >= 0) {
                if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest->gid)) {
                        fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
                        return NULL;
                }
        } else
                memset(&my_dest->gid, 0, sizeof my_dest->gid);

        my_dest->qpn = ctx->qp->qp_num;
        my_dest->psn = lrand48() & 0xffffff;
	fprintf(stdout, "connecting to qp\n");

        if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, gidx)) {
                fprintf(stderr, "Couldn't connect to remote QP\n");
                free(rem_dest);
                rem_dest = NULL;
                goto out;
        }


        gid_to_wire_gid(&my_dest->gid, gid);
        sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn, my_dest->psn, gid);
	fprintf(stdout, "sending local addr\n");
        if (write(connfd, msg, sizeof msg) != sizeof msg) {
                fprintf(stderr, "Couldn't send local address\n");
                free(rem_dest);
                rem_dest = NULL;
                goto out;
        }
	fprintf(stdout, "reading done\n");
        read(connfd, msg, sizeof msg);

	inet_ntop(AF_INET6, &my_dest->gid, gid, sizeof gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       my_dest->lid, my_dest->qpn, my_dest->psn, gid);

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);
	pthread_t thread_id;
        struct flow_info *fi = calloc(1, sizeof (struct flow_info));
        fi->ctx = ctx;
        fi->dev_list = dev_list;
        fi->iters = iters;
        fi->start = &start;
        fi->end = &end;
	fi->routs = routs;
	fi->rem_dest = rem_dest;
        if (pthread_create(&thread_id, NULL, flow_control,
				    (void *) fi) < 0) {
			fprintf(stderr, "ERA could not create thread to "
					       "handle flow control.\n");
			break;
	}
	
	}//while

out:
	close(connfd);
	
}

#include <sys/param.h>

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port,
					    int use_event, int is_server)
{
	struct pingpong_context *ctx;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size     = size;
	ctx->rx_depth = rx_depth;
	fprintf(stdout, "mallocing ctx buf\n");
	ctx->buf = malloc(roundup(size, page_size));
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		return NULL;
	}

	memset(ctx->buf, 0x7b + is_server, size);
	fprintf(stdout, "opening dev\n");
	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		return NULL;
	}
	fprintf(stdout, "creating cmp channel\n");
	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			return NULL;
		}
	} else
		ctx->channel = NULL;
	fprintf(stdout, "allocating pd\n");
	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		return NULL;
	}
	fprintf(stdout, "reging mr\n");
	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		return NULL;
	}
	fprintf(stdout, "creating cq\n");
	ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
				ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		return NULL;
	}

	{
		struct ibv_qp_init_attr attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};
		fprintf (stdout, "creating qp\n");
		ctx->qp = ibv_create_qp(ctx->pd, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			return NULL;
		}
	}

	{
		struct ibv_qp_attr attr = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qp_access_flags = 0
		};
		fprintf(stdout, "initting qps\n");
		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			return NULL;
		}
	}

	return ctx;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static int pp_post_send(struct pingpong_context *ctx)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
	};
	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>      size of message to exchange (default 4096)\n");
	printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
	printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
	printf("  -l, --sl=<sl>          service level value\n");
	printf("  -e, --events           sleep on CQ events (default poll)\n");
	printf("  -a, --random           use random value for PSN when connecting (default 0)\n");
	printf("  -g, --gid-idx=<gid index> local port gid index\n");
}
void init();
int main(int argc, char *argv[])
{

	srand48(getpid() * time(NULL));

	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",     .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",   .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",  .has_arg = 1, .val = 'i' },
			{ .name = "size",     .has_arg = 1, .val = 's' },
			{ .name = "mtu",      .has_arg = 1, .val = 'm' },
			{ .name = "rx-depth", .has_arg = 1, .val = 'r' },
			{ .name = "iters",    .has_arg = 1, .val = 'n' },
			{ .name = "sl",       .has_arg = 1, .val = 'l' },
			{ .name = "events",   .has_arg = 0, .val = 'e' },
			{ .name = "gid-idx",  .has_arg = 1, .val = 'g' },
			{ .name = "random",   .has_arg = 0, .val = 'a' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:eag:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
                case 'a':

			random_value = lrand48() & 0xffffff;
			break;
		case 'p':
			port = strtol(optarg, NULL, 0);
			if (port < 0 || port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdup(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			size = strtol(optarg, NULL, 0);
			break;

		case 'm':
			mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
			if (mtu < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'r':
			rx_depth = strtol(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtol(optarg, NULL, 0);
			break;

		case 'l':
			sl = strtol(optarg, NULL, 0);
			break;

		case 'e':
			++use_event;
			break;

		case 'g':
			gidx = strtol(optarg, NULL, 0);
			break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1)
		servername = strdup(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	init();

}
void init() {


        if (!servername) {
		pthread_t thread_id;
                if (pthread_create (&thread_id, NULL, pp_server_exch_dest,
                                    NULL) < 0) {
                        fprintf (stderr, "ERA could not create thread to "
                               "handle client connection.\n");
                        exit(0);
                }
		pthread_join(thread_id, NULL);
		return;
        }


        struct ibv_device      **dev_list;
        struct ibv_device       *ib_dev;
        struct pingpong_context *ctx;
        struct pingpong_dest*    my_dest = calloc(1, sizeof(struct pingpong_dest));
        struct pingpong_dest    *rem_dest;
        struct timeval           start, end;
        char                    *ib_devname = NULL;
        int                      routs;
        int                      rcnt, scnt;
        int                      num_cq_events = 0;
        int                      sl = 0;
        int                      gidx = -1;
        char                     gid[33];

	page_size = sysconf(_SC_PAGESIZE);
        fprintf(stdout, "getting dev list\n"); 
	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return ;
	}

	if (!ib_devname) {
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return ;
		}
	} else {
		fprintf(stdout, "finding dev name\n");
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return ;
		}
	}

	fprintf(stdout, "initing ctx\n");
	ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port, use_event, !servername);
	if (!ctx) {
		fprintf(stdout, "ctx NULL\n");
		return ;
        }
	fprintf(stdout, "posting recv\n");
	routs = pp_post_recv(ctx, ctx->rx_depth);
	if (routs < ctx->rx_depth) {
		fprintf(stderr, "Couldn't post receive (%d)\n", routs);
		return ;
	}

	fprintf(stdout, "reqing notify cq\n");
	if (use_event)
		if (ibv_req_notify_cq(ctx->cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return ;
		}

	fprintf(stdout, "getting port\n");
	if (pp_get_port_info(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return ;
	}

	my_dest->lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer == IBV_LINK_LAYER_INFINIBAND && !my_dest->lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return ;
	}

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest->gid)) {
			fprintf(stderr, "Could not get local gid for gid index %d\n", gidx);
			return ;
		}
	} else
		memset(&my_dest->gid, 0, sizeof my_dest->gid);

	my_dest->qpn = ctx->qp->qp_num;
	my_dest->psn = random_value & 0xffffff;


	inet_ntop(AF_INET6, &my_dest->gid, gid, sizeof gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       my_dest->lid, my_dest->qpn, my_dest->psn, gid);
	int thread_id;
	

	rem_dest = pp_client_exch_dest(servername, port, my_dest);
        if (!rem_dest) exit (1);
	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
        printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
		      rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

        if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest, gidx)) return ;
	ctx->pending = PINGPONG_RECV_WRID;
        fprintf(stdout, "posting 1st send\n");
        if (pp_post_send(ctx)) {
                fprintf(stderr, "Couldn't post send\n");
                return ;
        }
        ctx->pending |= PINGPONG_SEND_WRID;
	struct flow_info fi;
	fi.ctx = ctx;
	fi.dev_list = dev_list;
	fi.iters = iters;
	fi.start = &start;
	fi.end = &end;
	fi.routs = routs;
	fi.rem_dest = rem_dest;
	flow_control((void *)&fi);
		
}

void *flow_control (void * arg)
{
	struct flow_info * fi = (struct flow_info *)arg;

	struct pingpong_context* ctx = fi->ctx;
	struct ibv_device **dev_list = fi->dev_list;
	int iters = fi->iters;
	struct timeval* start = fi->start;
	struct timeval* end = fi->end;
	int routs = fi->routs;
	struct pingpong_dest* rem_dest = fi->rem_dest;
	if (gettimeofday(start, NULL)) {
		perror("gettimeofday");
		return NULL;
	}

	int rcnt = 0;
	int scnt = 0;
	int num_cq_events = 0;
	fprintf(stdout, "new enter\n");
	while (rcnt < iters || scnt < iters) {
		fprintf(stderr,"entering with send %d, recv %d\n", scnt, rcnt);
		if (use_event) {
			struct ibv_cq *ev_cq;
			void          *ev_ctx;
			fprintf(stdout, "getting cq event\n");
			if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
				fprintf(stderr, "Failed to get cq_event\n");
				return NULL;
			}

			++num_cq_events;

			if (ev_cq != ctx->cq) {
				fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
				return NULL;
			}

			if (ibv_req_notify_cq(ctx->cq, 0)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return NULL;
			}
		}

		{
			struct ibv_wc wc[2];
			int ne, i;
			int p = 0;
			fprintf(stdout, "polling\n");
			do {
				//fprintf(stderr, "polling %d\n", p++);
				ne = ibv_poll_cq(ctx->cq, 2, wc);
				if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n", ne);
					return NULL;
				}

			} while (!use_event && ne < 1);
			//fprintf(stderr,"polled %d, send %d, recv %d\n", ne, scnt, rcnt);
			for (i = 0; i < ne; ++i) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
						ibv_wc_status_str(wc[i].status),
						wc[i].status, (int) wc[i].wr_id);
					return NULL;
				}

				switch ((int) wc[i].wr_id) {
				case PINGPONG_SEND_WRID:
					++scnt;
					break;

				case PINGPONG_RECV_WRID:
					if (--routs <= 1) {
						routs += pp_post_recv(ctx, ctx->rx_depth - routs);
						if (routs < ctx->rx_depth) {
							fprintf(stderr,
								"Couldn't post receive (%d)\n",
								routs);
							return NULL;
						}
					}

					++rcnt;
					break;

				default:
					fprintf(stderr, "Completion for unknown wr_id %d\n",
						(int) wc[i].wr_id);
					return NULL;
				}

				ctx->pending &= ~(int) wc[i].wr_id;
				if (scnt < iters && !ctx->pending) {
					if (pp_post_send(ctx)) {
						fprintf(stderr, "Couldn't post send\n");
						return NULL;
					}
					ctx->pending = PINGPONG_RECV_WRID |
						       PINGPONG_SEND_WRID;
				}
			}
		}
	}

	if (gettimeofday(end, NULL)) {
		perror("gettimeofday");
		return NULL;
	}

	{
		float usec = (end->tv_sec - start->tv_sec) * 1000000 +
			(end->tv_usec - start->tv_usec);
		long long bytes = (long long) size * iters * 2;

		printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
		       bytes, usec / 1000000., bytes * 8. / usec);
		printf("%d iters in %.2f seconds = %.2f usec/iter\n",
		       iters, usec / 1000000., usec / iters);
	}

	ibv_ack_cq_events(ctx->cq, num_cq_events);

	if (pp_close_ctx(ctx))
		return NULL;

	ibv_free_device_list(dev_list);
	free(rem_dest);

	return NULL;
}
