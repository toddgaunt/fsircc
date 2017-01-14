/* See LICENSE file for copyright and license details */
#include <arpa/inet.h> 
#include <errno.h>
#include <malloc.h>
#include <netdb.h>
#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <unistd.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
//#include <libconfig.h>//Use this to make config file
#include <stdbool.h>
#include <libistr.h>

#ifndef PIPE_BUF /* If no PIPE_BUF defined */
#define PIPE_BUF _POSIX_PIPE_BUF
#endif

#define PATH_DEVNULL "/dev/null"

#define PING_TIMEOUT 300

#define IRC_BUF_MAX 512

#define VERSION "0.2"

struct irccd_args {
	istring *nick;
	istring *realname;
	istring *host;
	unsigned long port;

	istring *path;
	int daemon;
	int verbose;
};

struct irc_chan {
	int fd; //File Descriptor of a channel's FIFO file
	istring *name;
	struct irc_chan *next;
};

// Functions

static void usage()
{
	fprintf(stderr, "irccd - irc client daemon - %s\n"
		"usage: irccd [-h] [-v] [-d]\n", VERSION);
}

static int daemonize(int nochdir, int noclose) 
{/* Fork the process, detach from terminal, and redirect std streams */
	switch (fork()) { 
	case -1:
		return -1;
	case 0: 
		break;
	default: 
		exit(EXIT_SUCCESS);
	}

	if (setsid() < 0) return -1;

	if (!nochdir) {
		if (chdir("/") < 0) return -1;
	}

	if (!noclose) {
		int devnull = open(PATH_DEVNULL, O_RDWR);
		if (devnull < 0) return -1;
		dup2(devnull, STDIN_FILENO);
		dup2(devnull, STDOUT_FILENO);
		dup2(devnull, STDERR_FILENO);
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	return 0;
}

static int host_connect(int sockfd, char *cs_host, unsigned long port)
{
	struct sockaddr_in servaddr; 
	memset(&servaddr, 0, sizeof(struct sockaddr_in));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);

	// Define socket to be TCP
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("irccd: cannot create socket");
	}
	// Convert ip address to usable bytes
	if (inet_pton(AF_INET, cs_host, &servaddr.sin_addr) <= 0) {
		fprintf(stderr, "irccd: cannot transform ip\n");
	}

	// Cast sockaddr pointer to servaddr because connect takes sockaddr structs
	if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
		perror("irccd: unable to connect socket");
	}

	return sockfd;
}

static int send_msg(int sockfd, istring *msg)
{
	msg = istr_insert_bytes(msg, 0, "PRIVMSG ", 8);
	if (istr_len(msg) > IRC_BUF_MAX) {
		istr_trunc(msg, IRC_BUF_MAX - 2);
		msg = istr_append_bytes(msg, "\r\n", 2);
	}

	if (NULL == msg) {
		return -1;
	}

	return send(sockfd, msg, istr_len(msg), 0);
}

static istring* read_line(int sockfd, istring *recvln)
{/* Read chars from socket until a newline or IRC_BUF_MAX chars have been read */
	char c;
	do {
		if (recv(sockfd, &c, sizeof(char), 0) != sizeof(char)) {
			perror("irccd: Unable to read socket");
			return NULL;
		}
		recvln = istr_append_bytes(recvln, &c, 1);
	} while (c != '\n' && istr_len(recvln) < IRC_BUF_MAX - 2);
	return recvln;
}

static void login(int sockfd, istring *nick, const istring *realname)
{
	istring *msg = istr_new_bytes("USER ", 5);
	msg = istr_append(msg, nick);
	msg = istr_append_bytes(msg, " 8 * :", 6);
	msg = istr_append(msg, realname);
	msg = istr_append_bytes(msg, "\r\nNICK ", 7);
	msg = istr_append(msg, nick);
	msg = istr_append_bytes(msg, "\r\n", 2);
	send_msg(sockfd, msg);
	istr_free(msg);
}

// Channel Linked List Functions:
static struct irc_chan* find_chan(struct irc_chan *head, istring *name)
{
	if (NULL == head || NULL == name) {
		errno = EINVAL;
		return NULL;
	}

	struct irc_chan *tmp = head;
	while (tmp) {
		if (0 == istr_eq(tmp->name, name)) {
			return tmp;
		}
		tmp = tmp->next;
	}
	return NULL;
}

static struct irc_chan* prepend_chan(struct irc_chan *head, istring *name)
{
	struct irc_chan *tmp = malloc(sizeof(tmp)); 
	if (!tmp) {
		errno = ENOMEM;
		return NULL;
	}
	tmp->next = head;
	tmp->name = istr_new(name);
	tmp->next = NULL;

	return tmp;
}

static void rm_chan(struct irc_chan *node)
{
	if (NULL == node) {
		return;
	}

	struct irc_chan *garbage = node;
	struct irc_chan **pp = &node;
	*pp = node->next;
	istr_free(garbage->name);
	free(garbage);
}

static void setup_dirtree(istring *path)
{
}

static void handle_server_output(struct irccd_args *args, int sockfd)
{
	// Buffer for formatting messages to send back to the irc server
	istring *ping_buf = istr_new_cstr("PING ");
	ping_buf = istr_append(ping_buf, args->host);

	// Buffer for storing messages from the irc server
	istring *recvln = istr_new(NULL);
	recvln = read_line(sockfd, recvln);
	printf("server in: %s", recvln);

	istr_free(recvln);
	istr_free(ping_buf);
}

static void handle_client_input(struct irccd_args *args, struct irc_chan *chan)
{
}

int main(int argc, char *argv[]) 
{
	struct irccd_args args = {
		.nick = istr_new_cstr("user"),
		.realname = istr_new_cstr("user"),
		.host = istr_new_cstr("185.30.166.38"),
		.port = 6667,
		.path = istr_new_cstr("irccd/"),
		.daemon = 0,
		.verbose = 0
	};

	// Argument parsing
	if (argc > 1) {
		for(int i = 1; (i < argc) && (argv[i][0] == '-'); i++) {
			switch (argv[i][1]) {
			case 'h': 
				usage(); 
				return EXIT_SUCCESS;
				break;
			case 'f':
				i++;
				args.path = istr_assign_cstr(args.path, argv[i]);
				break;
			case 'v': 
				for (int j = 1; argv[i][j] && 'v' == argv[i][j]; j++) {
					args.verbose++; 
				}
				break;
			case 'q': 
				for (int j = 1; argv[i][j] && argv[i][j] == 'q'; j++) {
					if (args.verbose > 0) {
						args.verbose--; 
					}
				}
				break;
			case 'p': 
				i++;
				args.port = strtoul(argv[i], NULL, 10); break; // daemon port
				if (0 == args.port) {
					usage();
					return EXIT_FAILURE;
				}
			case 'n': 
				i++;
				args.nick = istr_assign_cstr(args.nick, argv[i]); 
				break;
			case 'r':
				i++;
				args.realname = istr_assign_cstr(args.realname, argv[i]);
				break;
			case 'd': 
				args.daemon = 1; 
				continue; // flag the daemonize process to start
			default: 
				usage();
				return EXIT_FAILURE;
			}
		}
	}

	// A host must be provided by user, no default is given
	if (NULL == args.host) {
		fprintf(stderr, "irccd: No host given, not starting\n");
		return EXIT_FAILURE;
	}

	if (args.daemon) {
		daemonize(0, 0);
	}

	// Linked list of channels user is connected to
	struct irc_chan *head_chan = NULL;
	// Connection to irc server
	int sockfd = 0;
	sockfd = host_connect(sockfd, args.host, args.port);
	login(sockfd, args.nick, args.realname);
	//setup_dirtree(args.path);

	fd_set readfds;
	int maxfd, s_fd;
	struct timeval tv;
	while(true) {
		FD_ZERO(&readfds);
		maxfd = sockfd;
		FD_SET(sockfd, &readfds);
		for (struct irc_chan *chan = head_chan; chan; chan = chan->next) {
			// Find the largest file-descriptor
			if (chan->fd > maxfd) {
				maxfd = chan->fd;
			}
			// Reset the file descriptor
			FD_SET(chan->fd, &readfds);
		}

		tv.tv_sec = 120;
		tv.tv_usec = 0;
		s_fd = select(maxfd+1, &readfds, 0, 0, &tv);

		// See if the irc server has any new messages
		if (FD_ISSET(sockfd, &readfds)) {
			handle_server_output(&args, sockfd);
		}
		// See if any channel FIFOs have any new messages
		for (struct irc_chan *chan = head_chan; chan; chan = chan->next) {
			if (FD_ISSET(chan->fd, &readfds)) {
				handle_client_input(&args, chan);
			}
		}
	}
	// Free the args object
	istr_free(args.nick);
	istr_free(args.realname);
	istr_free(args.host);
	istr_free(args.path);

	// Free all channel objects
	for (struct irc_chan *tmp = head_chan; tmp;) {
		struct irc_chan *garbage = tmp;
		tmp = tmp->next;
		istr_free(garbage->name);
		free(garbage);
	}

	return EXIT_SUCCESS;
}
