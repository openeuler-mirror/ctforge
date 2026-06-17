#include "ctforge.h"

int netlink_sock_fd;

#define MAX_MSG_SIZE 1024
#define MAX_PAYLOAD 1024

int create_netlink_socket(int protocol)
{
	struct sockaddr_nl src_addr;
	int sock_fd = socket(PF_NETLINK, SOCK_RAW, protocol);

	if (sock_fd < 0) {
		pr_err("netlink socket connect failed!");
		pthread_exit((void *)(intptr_t)EXIT_FAILURE);
	}
	memset(&src_addr, 0, sizeof(src_addr));
	src_addr.nl_family = AF_NETLINK;
	src_addr.nl_pid = getpid();
	src_addr.nl_groups = 0;
	if (bind(sock_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
		pr_err("netlink bind failed!");
		close(sock_fd);
		exit(EXIT_FAILURE);
	}
	return sock_fd;
}

void send_nl_handshake(int sock_fd)
{
	struct nlmsghdr *nlh = NULL;
	struct sockaddr_nl dest_addr;
	struct iovec iov;
	struct msghdr msg;

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.nl_family = AF_NETLINK;
	dest_addr.nl_pid = 0;
	dest_addr.nl_groups = 0;

	nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
	if (!nlh) {
		pr_err("malloc");
		close(sock_fd);
		exit(EXIT_FAILURE);
	}

	nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
	nlh->nlmsg_pid = getpid();
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_type = CTFORGE_NL_CONNECT;
	nlh->nlmsg_seq = 0;

	strscpy(NLMSG_DATA(nlh), "1000", 5);

	iov.iov_base = (void *)nlh;
	iov.iov_len = nlh->nlmsg_len;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *)&dest_addr;
	msg.msg_namelen = sizeof(dest_addr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	pr_info("Netlink sending handshake to kernel %d", nlh->nlmsg_len);
	if (sendmsg(sock_fd, &msg, 0) < 0) {
		pr_err("sendmsg");
		free(nlh);
		close(sock_fd);
		exit(EXIT_FAILURE);
	}

	if (nlh != NULL) {
		free(nlh);
		nlh = NULL;
	}
}

void do_ctforge_nl_log(struct nlmsghdr *nlh)
{
	char *str;

	str = (char *)NLMSG_DATA(nlh);
	pr_info("msg:[%s], len:%d, len:%d", str, strlen(str), nlh->nlmsg_len);
	ctforged_klog_to_file(str);
}

void do_ctforge_nl_ping(struct nlmsghdr *nlh)
{
	char *str;

	str = (char *)NLMSG_DATA(nlh);
	pr_info("msg:[%s], len:%d, len:%d", str, strlen(str), nlh->nlmsg_len);
	ctforged_klog_to_file(str);
}

/* msg from kernel netlink response */
void tackle_nl_msg(struct nlmsghdr *nlh)
{
	switch (nlh->nlmsg_type) {
//	case CTFORGE_NL_AUDIT_LOG:
//		do_ctforge_nl_log(nlh);
//		break;
//	case CTFORGE_NL_PING:
//		do_ctforge_nl_ping(nlh);
//		break;
//	case CTFORGE_NL_SET_HMAC_TYPE:
//		pr_info("receive kernel msg CTFORGE_NL_SET_HMAC_TYPE");
//		do_ctforge_nl_set_hmac_type(nlh);
//		break;
//	case CTFORGE_NL_GET_HMAC_TYPE:
//		pr_info("receive kernel msg CTFORGE_NL_GET_HMAC_TYPE");
//		do_ctforge_nl_get_hmac_type(nlh);
//		break;
//	case CTFORGE_NL_GET_KFIFO_AVAIL:
//		pr_info("receive kernel msg CTFORGE_NL_GET_KFIFO_AVAIL");
//		do_ctforge_nl_get_kfifo_avail(nlh);
//		break;
//	case CTFORGE_NL_GET_KFIFO_DROP:
//		pr_info("receive kernel msg CTFORGE_NL_GET_KFIFO_DROP");
//		do_ctforge_nl_get_kfifo_drop(nlh);
//		break;
//	case CTFORGE_NL_GET_KFIFO_TOTAL:
//		pr_info("receive kernel msg CTFORGE_NL_GET_KFIFO_TOTAL");
//		do_ctforge_nl_get_kfifo_total(nlh);
//		break;
//	case CTFORGE_NL_AUDIT_TEST:
//		pr_info("receive kernel msg CTFORGE_NL_AUDIT_TEST");
//		do_ctforge_nl_audit_test(nlh);
//		break;
//	case CTFORGE_NL_GET_STATUS:
//		pr_info("receive kernel msg CTFORGE_NL_GET_STATUS");
//		do_ctforge_nl_get_status(nlh);
//		break;
	default:
		pr_info("ctforged receive unknown nlmsg_type from kernel");
		pr_info("dump message %d %d", nlh->nlmsg_len + NLMSG_HDRLEN,
			nlh->nlmsg_type);
		hexdump(nlh, nlh->nlmsg_len + NLMSG_HDRLEN);
		pr_info("kernel return err");
		break;
	}
}

void do_send_nl_ctl(int sock_fd, struct ctforged_params *s)
{
	struct nlmsghdr *nlh = NULL;
	struct sockaddr_nl dest_addr;
	struct iovec iov;
	struct msghdr msg;

	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.nl_family = AF_NETLINK;
	dest_addr.nl_pid = 0;
	dest_addr.nl_groups = 0;

	nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
	if (!nlh) {
		pr_err("malloc failed!");
		close(sock_fd);
		exit(EXIT_FAILURE);
	}

	nlh->nlmsg_len = NLMSG_SPACE(sizeof(struct ctforged_params));
	nlh->nlmsg_pid = getpid();
	nlh->nlmsg_flags = 0;
	nlh->nlmsg_type = s->nlmsg_type;
	nlh->nlmsg_seq = 0;

	memcpy(NLMSG_DATA(nlh), s, sizeof(struct ctforged_params));

	iov.iov_base = (void *)nlh;
	iov.iov_len = nlh->nlmsg_len;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *)&dest_addr;
	msg.msg_namelen = sizeof(dest_addr);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	pr_info("send msg %d", nlh->nlmsg_len + NLMSG_HDRLEN);
	//	hexdump(nlh, nlh->nlmsg_len + NLMSG_HDRLEN);
	if (sendmsg(sock_fd, &msg, 0) < 0) {
		pr_err("sendmsg failed!");
		free(nlh);
		close(sock_fd);
		exit(EXIT_FAILURE);
	}
	free(nlh);
}

void ctforge_nl_ctl(struct ctforged_params *s)
{
	do_send_nl_ctl(netlink_sock_fd, s);
}

void netlink_loop(void)
{
	struct nlmsghdr *nlh = NULL;
	struct msghdr msg;
	struct sockaddr_nl dest_addr;
	struct iovec iov;

	netlink_sock_fd = create_netlink_socket(NETLINK_CTFORGE);
	pr_info("Netlink bind success!");

	send_nl_handshake(netlink_sock_fd);
	pr_info("Netlink waiting for messages from the kernel");

	while (1) {
		nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_MSG_SIZE));
		if (!nlh) {
			pr_err("malloc");
			exit(EXIT_FAILURE);
		}
		memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
		iov.iov_base = (void *)nlh;
		iov.iov_len = NLMSG_SPACE(MAX_MSG_SIZE);

		memset(&msg, 0, sizeof(msg));
		msg.msg_name = (void *)&dest_addr;
		msg.msg_namelen = sizeof(dest_addr);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		if (recvmsg(netlink_sock_fd, &msg, 0) < 0) {
			pr_err("recvmsg failed!");
			free(nlh);
			close(netlink_sock_fd);
			exit(EXIT_FAILURE);
		}
		pr_debug("msg type: %d, msg len: %d [%d,%d]", nlh->nlmsg_type,
			 nlh->nlmsg_len, NLMSG_HDRLEN,
			 NLMSG_SPACE(MAX_PAYLOAD) + 32);

		if (nlh->nlmsg_len <= NLMSG_HDRLEN ||
		    nlh->nlmsg_len >= NLMSG_SPACE(MAX_PAYLOAD) + 32) {
			pr_err("invalid payload length %d (%d,%d)",
			       nlh->nlmsg_len, NLMSG_HDRLEN,
			       NLMSG_SPACE(MAX_PAYLOAD));
			pr_err("type %d", nlh->nlmsg_type);
			pr_err("info %x", nlh->nlmsg_flags);
			continue;
		}

		tackle_nl_msg(nlh);
		if (nlh != NULL) {
			free(nlh);
			nlh = NULL;
		}
	}
}
