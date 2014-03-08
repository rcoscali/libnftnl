/*
 * (C) 2012 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software has been sponsored by Sophos Astaro <http://www.sophos.com>
 */

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stddef.h>	/* for offsetof */
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>

#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>

#include <libmnl/libmnl.h>
#include <libnftnl/rule.h>
#include <libnftnl/expr.h>

static void add_payload(struct nft_rule *r, uint32_t base, uint32_t dreg,
			uint32_t offset, uint32_t len)
{
	struct nft_rule_expr *e;

	e = nft_rule_expr_alloc("payload");
	if (e == NULL) {
		perror("expr payload oom");
		exit(EXIT_FAILURE);
	}

	nft_rule_expr_set_u32(e, NFT_EXPR_PAYLOAD_BASE, base);
	nft_rule_expr_set_u32(e, NFT_EXPR_PAYLOAD_DREG, dreg);
	nft_rule_expr_set_u32(e, NFT_EXPR_PAYLOAD_OFFSET, offset);
	nft_rule_expr_set_u32(e, NFT_EXPR_PAYLOAD_LEN, len);

	nft_rule_add_expr(r, e);
}

static void add_cmp(struct nft_rule *r, uint32_t sreg, uint32_t op,
		    const void *data, uint32_t data_len)
{
	struct nft_rule_expr *e;

	e = nft_rule_expr_alloc("cmp");
	if (e == NULL) {
		perror("expr cmp oom");
		exit(EXIT_FAILURE);
	}

	nft_rule_expr_set_u32(e, NFT_EXPR_CMP_SREG, sreg);
	nft_rule_expr_set_u32(e, NFT_EXPR_CMP_OP, op);
	nft_rule_expr_set(e, NFT_EXPR_CMP_DATA, data, data_len);

	nft_rule_add_expr(r, e);
}

static void add_counter(struct nft_rule *r)
{
	struct nft_rule_expr *e;

	e = nft_rule_expr_alloc("counter");
	if (e == NULL) {
		perror("expr counter oom");
		exit(EXIT_FAILURE);
	}

	nft_rule_add_expr(r, e);
}

static struct nft_rule *setup_rule(uint8_t family, const char *table,
				   const char *chain, const char *handle)
{
	struct nft_rule *r = NULL;
	uint8_t proto;
	uint16_t dport;
	uint64_t handle_num;

	r = nft_rule_alloc();
	if (r == NULL) {
		perror("OOM");
		exit(EXIT_FAILURE);
	}

	nft_rule_attr_set(r, NFT_RULE_ATTR_TABLE, table);
	nft_rule_attr_set(r, NFT_RULE_ATTR_CHAIN, chain);
	nft_rule_attr_set_u32(r, NFT_RULE_ATTR_FAMILY, family);

	if (handle != NULL) {
		handle_num = atoll(handle);
		nft_rule_attr_set_u64(r, NFT_RULE_ATTR_POSITION, handle_num);
	}

	proto = IPPROTO_TCP;
	add_payload(r, NFT_PAYLOAD_NETWORK_HEADER, NFT_REG_1,
		    offsetof(struct iphdr, protocol), sizeof(uint8_t));
	add_cmp(r, NFT_REG_1, NFT_CMP_EQ, &proto, sizeof(uint8_t));

	dport = htons(22);
	add_payload(r, NFT_PAYLOAD_TRANSPORT_HEADER, NFT_REG_1,
		    offsetof(struct tcphdr, dest), sizeof(uint16_t));
	add_cmp(r, NFT_REG_1, NFT_CMP_EQ, &dport, sizeof(uint16_t));

	add_counter(r);

	return r;
}

static void nft_mnl_batch_put(char *buf, uint16_t type, uint32_t seq)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfg;

	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = type;
	nlh->nlmsg_flags = NLM_F_REQUEST;
	nlh->nlmsg_seq = seq;

	nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(*nfg));
	nfg->nfgen_family = AF_INET;
	nfg->version = NFNETLINK_V0;
	nfg->res_id = NFNL_SUBSYS_NFTABLES;
}

int main(int argc, char *argv[])
{
	struct mnl_socket *nl;
	struct nft_rule *r;
	struct nlmsghdr *nlh;
	struct mnl_nlmsg_batch *batch;
	uint8_t family;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	uint32_t seq = time(NULL);
	int ret;

	if (argc < 4 || argc > 5) {
		fprintf(stderr, "Usage: %s <family> <table> <chain>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if (strcmp(argv[1], "ip") == 0)
		family = NFPROTO_IPV4;
	else if (strcmp(argv[1], "ip6") == 0)
		family = NFPROTO_IPV6;
	else {
		fprintf(stderr, "Unknown family: ip, ip6\n");
		exit(EXIT_FAILURE);
	}

	if (argc != 5)
		r = setup_rule(family, argv[2], argv[3], NULL);
	else
		r = setup_rule(family, argv[2], argv[3], argv[4]);

	nl = mnl_socket_open(NETLINK_NETFILTER);
	if (nl == NULL) {
		perror("mnl_socket_open");
		exit(EXIT_FAILURE);
	}

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0) {
		perror("mnl_socket_bind");
		exit(EXIT_FAILURE);
	}

	batch = mnl_nlmsg_batch_start(buf, sizeof(buf));

	nft_mnl_batch_put(mnl_nlmsg_batch_current(batch),
			  NFNL_MSG_BATCH_BEGIN, seq++);
	mnl_nlmsg_batch_next(batch);

	nlh = nft_rule_nlmsg_build_hdr(mnl_nlmsg_batch_current(batch),
			NFT_MSG_NEWRULE,
			nft_rule_attr_get_u32(r, NFT_RULE_ATTR_FAMILY),
			NLM_F_APPEND|NLM_F_CREATE|NLM_F_ACK, seq++);

	nft_rule_nlmsg_build_payload(nlh, r);
	nft_rule_free(r);
	mnl_nlmsg_batch_next(batch);

	nft_mnl_batch_put(mnl_nlmsg_batch_current(batch), NFNL_MSG_BATCH_END,
			 seq++);
	mnl_nlmsg_batch_next(batch);

	ret = mnl_socket_sendto(nl, mnl_nlmsg_batch_head(batch),
				mnl_nlmsg_batch_size(batch));
	if (ret == -1) {
		perror("mnl_socket_sendto");
		exit(EXIT_FAILURE);
	}

	mnl_nlmsg_batch_stop(batch);

	ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
	if (ret == -1) {
		perror("mnl_socket_recvfrom");
		exit(EXIT_FAILURE);
	}

	ret = mnl_cb_run(buf, ret, 0, mnl_socket_get_portid(nl), NULL, NULL);
	if (ret < 0) {
		perror("mnl_cb_run");
		exit(EXIT_FAILURE);
	}

	mnl_socket_close(nl);

	return EXIT_SUCCESS;
}
