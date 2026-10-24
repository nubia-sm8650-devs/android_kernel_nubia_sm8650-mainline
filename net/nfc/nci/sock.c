// SPDX-License-Identifier: GPL-2.0-only
/*
 * NCI user channel
 *
 * Gives one process exclusive raw NCI access to a controller. The kernel
 * powers the transport and passes NCI packets through unchanged, while the
 * process runs the NCI state machine itself.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/capability.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/nfc.h>
#include <net/sock.h>
#include <net/nfc/nci.h>
#include <net/nfc/nci_core.h>

#include "../nfc.h"

struct nci_user_sock {
	struct sock sk;
	struct nci_dev *ndev;
	bool bound;
};

/* Protects nci_user_devs and every nci_user_sock::ndev */
static DEFINE_MUTEX(nci_user_lock);
static LIST_HEAD(nci_user_devs);

static struct nci_user_sock *nci_user_sk(struct sock *sk)
{
	return container_of(sk, struct nci_user_sock, sk);
}

void nci_user_add_device(struct nci_dev *ndev)
{
	mutex_lock(&nci_user_lock);
	list_add_tail(&ndev->user_list, &nci_user_devs);
	mutex_unlock(&nci_user_lock);
}

void nci_user_remove_device(struct nci_dev *ndev)
{
	struct sock *sk;

	mutex_lock(&nci_user_lock);

	list_del(&ndev->user_list);

	sk = ndev->user_sk;
	if (sk) {
		nci_user_close(ndev);
		nci_user_sk(sk)->ndev = NULL;

		sk->sk_err = ENODEV;
		sk->sk_shutdown = SHUTDOWN_MASK;
		WRITE_ONCE(sk->sk_state, TCP_CLOSE);
		sk_error_report(sk);
	}

	mutex_unlock(&nci_user_lock);
}

static struct nci_dev *nci_user_find_device(u32 idx)
{
	struct nci_dev *ndev;

	lockdep_assert_held(&nci_user_lock);

	list_for_each_entry(ndev, &nci_user_devs, user_list)
		if (ndev->nfc_dev->idx == idx)
			return ndev;

	return NULL;
}

static int nci_user_sock_bind(struct socket *sock,
			      struct sockaddr_unsized *_addr, int len)
{
	struct sockaddr_nfc *addr = (struct sockaddr_nfc *)_addr;
	struct nci_user_sock *nsk = nci_user_sk(sock->sk);
	struct nci_dev *ndev;
	int rc;

	if (!addr || len < sizeof(*addr) || addr->sa_family != AF_NFC)
		return -EINVAL;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	mutex_lock(&nci_user_lock);

	if (nsk->bound) {
		rc = -EALREADY;
		goto unlock;
	}

	ndev = nci_user_find_device(addr->dev_idx);
	if (!ndev) {
		rc = -ENODEV;
		goto unlock;
	}

	rc = nci_user_open(ndev, sock->sk);
	if (rc)
		goto unlock;

	nsk->ndev = ndev;
	nsk->bound = true;
	sock->state = SS_CONNECTED;
	WRITE_ONCE(sock->sk->sk_state, TCP_ESTABLISHED);

unlock:
	mutex_unlock(&nci_user_lock);
	return rc;
}

static int nci_user_sock_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct nci_user_sock *nsk;

	if (!sk)
		return 0;

	nsk = nci_user_sk(sk);

	mutex_lock(&nci_user_lock);
	if (nsk->ndev) {
		nci_user_close(nsk->ndev);
		nsk->ndev = NULL;
	}
	mutex_unlock(&nci_user_lock);

	sock_orphan(sk);
	sock_put(sk);

	return 0;
}

static int nci_user_sock_sendmsg(struct socket *sock, struct msghdr *msg,
				 size_t len)
{
	struct nci_user_sock *nsk = nci_user_sk(sock->sk);
	struct nci_dev *ndev;
	struct sk_buff *skb;
	int rc;

	if (msg->msg_flags & MSG_OOB)
		return -EOPNOTSUPP;

	if (len < NCI_CTRL_HDR_SIZE || len > NCI_MAX_PACKET_SIZE)
		return -EINVAL;

	mutex_lock(&nci_user_lock);

	ndev = nsk->ndev;
	if (!ndev) {
		rc = nsk->bound ? -ENODEV : -ENOTCONN;
		goto unlock;
	}

	skb = nci_skb_alloc(ndev, len, GFP_KERNEL);
	if (!skb) {
		rc = -ENOMEM;
		goto unlock;
	}

	rc = memcpy_from_msg(skb_put(skb, len), msg, len);
	if (rc) {
		kfree_skb(skb);
		goto unlock;
	}

	if ((nci_mt(skb->data) != NCI_MT_CMD_PKT &&
	     nci_mt(skb->data) != NCI_MT_DATA_PKT) ||
	    skb->data[2] != len - NCI_CTRL_HDR_SIZE) {
		kfree_skb(skb);
		rc = -EINVAL;
		goto unlock;
	}

	rc = nci_send_frame(ndev, skb);

unlock:
	mutex_unlock(&nci_user_lock);
	return rc ? rc : len;
}

static int nci_user_sock_recvmsg(struct socket *sock, struct msghdr *msg,
				 size_t len, int flags)
{
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int copied;
	int rc;

	skb = skb_recv_datagram(sk, flags, &rc);
	if (!skb)
		return rc;

	copied = skb->len;
	if (len < copied) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	rc = skb_copy_datagram_msg(skb, 0, msg, copied);

	skb_free_datagram(sk, skb);

	return rc ? : copied;
}

static int nci_user_sock_ioctl(struct socket *sock, unsigned int cmd,
			       unsigned long arg)
{
	struct nci_user_sock *nsk = nci_user_sk(sock->sk);
	int rc;

	switch (cmd) {
	case SIOCNCIHWRESET:
		mutex_lock(&nci_user_lock);
		if (nsk->ndev)
			rc = nci_user_reset(nsk->ndev);
		else
			rc = nsk->bound ? -ENODEV : -ENOTCONN;
		mutex_unlock(&nci_user_lock);
		return rc;
	default:
		return -ENOIOCTLCMD;
	}
}

static const struct proto_ops nci_user_sock_ops = {
	.family		= PF_NFC,
	.owner		= THIS_MODULE,
	.release	= nci_user_sock_release,
	.bind		= nci_user_sock_bind,
	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.getname	= sock_no_getname,
	.poll		= datagram_poll,
	.ioctl		= nci_user_sock_ioctl,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.sendmsg	= nci_user_sock_sendmsg,
	.recvmsg	= nci_user_sock_recvmsg,
	.mmap		= sock_no_mmap,
};

static void nci_user_sock_destruct(struct sock *sk)
{
	skb_queue_purge(&sk->sk_receive_queue);
}

static int nci_user_sock_create(struct net *net, struct socket *sock,
				const struct nfc_protocol *nfc_proto, int kern)
{
	struct sock *sk;

	if (sock->type != SOCK_SEQPACKET)
		return -ESOCKTNOSUPPORT;

	sk = sk_alloc(net, PF_NFC, GFP_KERNEL, nfc_proto->proto, kern);
	if (!sk)
		return -ENOMEM;

	sock->ops = &nci_user_sock_ops;
	sock_init_data(sock, sk);
	sk->sk_protocol = nfc_proto->id;
	sk->sk_destruct = nci_user_sock_destruct;
	sock->state = SS_UNCONNECTED;

	return 0;
}

static struct proto nci_user_proto = {
	.name		= "NFC_NCI",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct nci_user_sock),
};

static const struct nfc_protocol nci_user_nfc_proto = {
	.id		= NFC_SOCKPROTO_NCI,
	.proto		= &nci_user_proto,
	.owner		= THIS_MODULE,
	.create		= nci_user_sock_create,
};

int __init nci_user_init(void)
{
	return nfc_proto_register(&nci_user_nfc_proto);
}

void nci_user_exit(void)
{
	nfc_proto_unregister(&nci_user_nfc_proto);
}
