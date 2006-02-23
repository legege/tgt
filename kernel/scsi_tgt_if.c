/*
 * SCSI target kernel/user interface functions
 *
 * Copyright (C) 2005 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2005 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#include <linux/blkdev.h>
#include <linux/file.h>
#include <linux/netlink.h>
#include <net/tcp.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tgt.h>
#include <scsi/scsi_tgt_if.h>

#include "scsi_tgt_priv.h"

static int tgtd_pid;
static struct sock *nl_sk;

static int send_event_res(uint16_t type, struct tgt_event *p,
			  void *data, int dlen, gfp_t flags, pid_t pid)
{
	struct tgt_event *ev;
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	uint32_t len;

	len = NLMSG_SPACE(sizeof(*ev) + dlen);
	skb = alloc_skb(len, flags);
	if (!skb)
		return -ENOMEM;

	nlh = __nlmsg_put(skb, pid, 0, type, len - sizeof(*nlh), 0);

	ev = NLMSG_DATA(nlh);
	memcpy(ev, p, sizeof(*ev));
	if (dlen)
		memcpy(ev->data, data, dlen);

	return netlink_unicast(nl_sk, skb, pid, 0);
}

int scsi_tgt_uspace_send(struct scsi_cmnd *cmd, struct scsi_lun *lun, gfp_t gfp_mask)
{
	struct Scsi_Host *shost = scsi_tgt_cmd_to_host(cmd);
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	struct tgt_event *ev;
	struct tgt_cmd *tcmd;
	int err, len;

	len = NLMSG_SPACE(sizeof(*ev) + sizeof(struct tgt_cmd));
	/*
	 * TODO: add MAX_COMMAND_SIZE to ev and add mempool
	 */
	skb = alloc_skb(NLMSG_SPACE(len), gfp_mask);
	if (!skb)
		return -ENOMEM;

	nlh = __nlmsg_put(skb, tgtd_pid, 0, TGT_KEVENT_CMD_REQ,
			  len - sizeof(*nlh), 0);

	ev = NLMSG_DATA(nlh);
	ev->k.cmd_req.host_no = shost->host_no;
	ev->k.cmd_req.cid = cmd->request->tag;
	ev->k.cmd_req.data_len = cmd->request_bufflen;

	dprintk("%p %d %u %u\n", cmd, ev->k.cmd_req.host_no, ev->k.cmd_req.cid,
		ev->k.cmd_req.data_len);

	/* FIXME: we need scsi core to do that. */
	memcpy(cmd->cmnd, cmd->data_cmnd, MAX_COMMAND_SIZE);

	tcmd = (struct tgt_cmd *) ev->data;
	memcpy(tcmd->scb, cmd->cmnd, sizeof(tcmd->scb));
	memcpy(tcmd->lun, lun, sizeof(struct scsi_lun));

	err = netlink_unicast(nl_sk, skb, tgtd_pid, 0);
	if (err < 0)
		printk(KERN_ERR "scsi_tgt_uspace_send: could not send skb %d\n",
		       err);
	return err;
}

int scsi_tgt_uspace_send_status(struct scsi_cmnd *cmd, gfp_t gfp_mask)
{
	struct Scsi_Host *shost = scsi_tgt_cmd_to_host(cmd);
	struct tgt_event ev;
	char dummy[sizeof(struct tgt_cmd)];

	memset(&ev, 0, sizeof(ev));
	ev.k.cmd_done.host_no = shost->host_no;
	ev.k.cmd_done.cid = cmd->request->tag;
	ev.k.cmd_done.result = cmd->result;

	return send_event_res(TGT_KEVENT_CMD_DONE, &ev, dummy, sizeof(dummy),
			      gfp_mask, tgtd_pid);
}

static int event_recv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	struct tgt_event *ev = NLMSG_DATA(nlh);
	int err = 0;

	dprintk("%d %d %d\n", nlh->nlmsg_type,
		nlh->nlmsg_pid, current->pid);

	switch (nlh->nlmsg_type) {
	case TGT_UEVENT_TGTD_BIND:
		tgtd_pid = NETLINK_CREDS(skb)->pid;
		break;
	case TGT_UEVENT_CMD_RES:
		/* TODO: handle multiple cmds in one event */
		err = scsi_tgt_kspace_exec(ev->u.cmd_res.host_no,
					   ev->u.cmd_res.cid,
					   ev->u.cmd_res.result,
					   ev->u.cmd_res.len,
					   ev->u.cmd_res.offset,
					   ev->u.cmd_res.uaddr,
					   ev->u.cmd_res.rw,
					   ev->u.cmd_res.try_map);
		break;
	default:
		eprintk("unknown type %d\n", nlh->nlmsg_type);
		err = -EINVAL;
	}

	return err;
}

static int event_recv_skb(struct sk_buff *skb)
{
	int err;
	uint32_t rlen;
	struct nlmsghdr	*nlh;

	while (skb->len >= NLMSG_SPACE(0)) {
		nlh = (struct nlmsghdr *) skb->data;
		if (nlh->nlmsg_len < sizeof(*nlh) || skb->len < nlh->nlmsg_len)
			return 0;
		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		err = event_recv_msg(skb, nlh);

		dprintk("%d %d\n", nlh->nlmsg_type, err);
		/*
		 * TODO for passthru commands the lower level should
		 * probably handle the result or we should modify this
		 */
		if (nlh->nlmsg_type != TGT_UEVENT_CMD_RES) {
			struct tgt_event ev;

			memset(&ev, 0, sizeof(ev));
			ev.k.event_res.err = err;
			send_event_res(TGT_KEVENT_RESPONSE, &ev, NULL, 0,
				       GFP_KERNEL | __GFP_NOFAIL,
					nlh->nlmsg_pid);
		}
		skb_pull(skb, rlen);
	}
	return 0;
}

static void event_recv(struct sock *sk, int length)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&sk->sk_receive_queue))) {
		if (NETLINK_CREDS(skb)->uid) {
			skb_pull(skb, skb->len);
			kfree_skb(skb);
			continue;
		}

		if (event_recv_skb(skb) && skb->len)
			skb_queue_head(&sk->sk_receive_queue, skb);
		else
			kfree_skb(skb);
	}
}

void __exit scsi_tgt_if_exit(void)
{
	sock_release(nl_sk->sk_socket);
}

int __init scsi_tgt_if_init(void)
{
	nl_sk = netlink_kernel_create(NETLINK_TGT, 1, event_recv,
				    THIS_MODULE);
	if (!nl_sk)
		return -ENOMEM;

	return 0;
}