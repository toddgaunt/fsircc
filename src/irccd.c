/*
 * Name: irccd
 * Author: Todd Gaunt
 * Last updated: 2016-03-19
 * License: MIT
 * Description:
 * irccd is a simple irc-bot daemon
 * it will auto-save messages for you
 * and can be called from the command line
 * to send messages to the specified channel.
 * TODO conform to XDG spec, ~/.config/irccd/socket, ~/.config/irccd/log etc..
 */

#include <limits.h>
#include <stdio.h> 
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h> 
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <malloc.h>
//#include <libconfig.h>//Use this to make config file
/* - - - - - - - */
#include "irccd.h"

static void usage()
{
	fprintf(stderr, "irccd - irc client daemon - " VERSION "\n"
	"usage: irccd []\n");
}

int send_msg(int sockfd, char *out)
{/* Send message with debug output to socket */
	int n = send(sockfd, out, strlen(out), 0);
	if (n <= 0) {
		fprintf(stderr, PRGNAME": error: message sending error\n");
	} else {
		fprintf(stderr, PRGNAME": out: %s", out);
	}
	return n;
}

int read_msg(int sockfd, char *recvline)
{/* Read next message from socket, replies to any PING with a PONG */
	char *pos, out[PIPE_BUF];
	int n = read(sockfd, recvline, PIPE_BUF);
	if (n > 0) {
		fprintf(stdout, PRGNAME": in: %s", recvline);
	}
	memset(&out, 0, sizeof(out));
	if (n > 0 && strstr(recvline, "PING") != NULL) {
		pos = strstr(recvline, " ")+1;
		sprintf(out, "PONG %s\r\n", pos);
		send_msg(sockfd, out);
	}
	return n;
}

int sock_conn(char *host, int *host_sockfd)
{/* Construct socket at host_sockfd, then connect */
	struct sockaddr_in servaddr; 
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(IRCCD_PORT);
	
	// Modifies the host_sockfd for the rest of main() to use
	*host_sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (*host_sockfd < 0) {
		fprintf(stdout, PRGNAME": socket creation error\n");
		return 1;
	}
	if (inet_pton(AF_INET, host, &servaddr.sin_addr) <= 0) {
		fprintf(stderr, PRGNAME": error: invalid network address error\n");
		return 1;
	}
	// Points sockaddr pointer to servaddr because connect takes sockaddr structs
	if (connect(*host_sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
		return 1;
	}

	return 0;
}

int host_disc(int *host_sockfd)
{/* Disconnects from irc gracefully, then close the socket. */
	send_msg(*host_sockfd, "QUIT\r\n");
	return shutdown(*host_sockfd, SHUT_RDWR);
}

int bind_sock(char *path, int *local_sockfd)
{/* Contruct a unix socket for inter-process communication, then connect */
	struct sockaddr_un servaddr; 
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sun_family = AF_UNIX;
	strcpy(servaddr.sun_path, path);

	// modifies local socket for unix communication 
	*local_sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (*local_sockfd < 0) {
		fprintf(stderr, PRGNAME": error: socket creation error\n");
		return 1;
	}
	if (bind(*local_sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
		return 1;
	}
	return 0;
}

int spawn_reader(int sockfd)
{/* Fork process to constantly read from irc socket */
	int timer = 0;
	int pid = fork(); 
	char recvline[PIPE_BUF];
	if (pid == 0) {
		while(1) {
			memset(&recvline, 0, PIPE_BUF);
			if (read_msg(sockfd, recvline) <= 0) {
				timer += 1;
				sleep(1);
				fprintf(stderr, PRGNAME": reader error (%d)\n", timer);
			} else { 
				timer = 0;
			}
			if (timer >= PING_TIMEOUT) {
				exit(1); // kill the reader if read attempt fails after 10 tries
			}
		}
	}
	return pid;
}

int add_chan(Channel *head, char *chan_name)
{/* Add a channel to end of a list */
	Channel *tmp = head;
	while (tmp->next != NULL) {
		tmp = tmp->next;
		if (strcmp(tmp->name, chan_name) == 0) {
			return 1;
		}
	}
	tmp->next = (Channel *)malloc(sizeof(Channel)); 
	tmp = tmp->next; 
	if (tmp == 0) {
		fprintf(stderr, PRGNAME": error: out of memory\n");
		return -1;
	}
	tmp->name = strdup(chan_name);
	tmp->next = NULL;

	fprintf(stdout, PRGNAME": channel added: %s\n", tmp->name);
	return 0;
}

int rm_chan(Channel *head, char *chan_name)
{/* Remove an entry with name = chan_name from list */
	Channel *garbage;
	Channel *tmp = head;
	while (tmp->next != NULL) {
		if (strcmp(tmp->next->name, chan_name) != 0) {
			tmp = tmp->next;
		} else {
			garbage = tmp->next; 
			tmp->next = tmp->next->next;

			fprintf(stdout, PRGNAME": channel removed: %s\n", garbage->name);

			free(garbage->name);
			free(garbage);
			return 0;
		}
	}
	fprintf(stdout, PRGNAME": channel %s not found in list\n", chan_name);
	return 1;
}

// Currently unused
int printchan(char *buf, Channel *head)
{/* Pretty Prints all channels client is connected to 
  * Later should write channels to a log file */
	Channel *node = head;
	if (node != NULL) {
		sprintf(buf, "%s->", node->name);
		while (node->next != NULL) {
			node = node->next;
			sprintf(buf, "%s%s->", buf, node->name);
		}
		sprintf(buf, "%s\n", buf);
		return 1;
	}
	return 0;
}

int ping_host(int sockfd, char *msg)
{/* Sends a ping message to a socket */
	char out[PIPE_BUF];
	sprintf(out, "PING %s\r\n", msg);
	return send_msg(sockfd, out);
}

int sendlogin(int sockfd, char *nick, char *realname) 
{/* Sends out login information to the host */
	char out[PIPE_BUF];
	sprintf(out, "NICK %s\r\nUSER %s 8 * :nick\r\n", nick, nick);
	send_msg(sockfd, out);
	return 0;
}

int chan_name_check(char *chan_name) 
{/* Performs checks to make sure the string is a channel name */
	if (chan_name[0] != '#' || sizeof(chan_name) > CHAN_LEN) {
		fprintf(stdout, "PRGRM_NAME: %s is not a valid channel\n", chan_name);
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[]) 
{
	usage();
	// Paths to sockets and fifos
	char *nick = "iwakura_lain";
	char *fifopath = "/tmp/irccd.fifo";
	char *sockpath = "/tmp/irccd.socket";
	// User info
	int host_sockfd = 0; // fd used for internet socket
	char host_serv[IP_LEN] = "162.213.39.42";
	// Channel list pointers
	Channel *channels = (Channel*)malloc(sizeof(Channel)); // List of channels
    char chan_name[CHAN_LEN];

	int local_sockfd = 0; // socket used for client to talk to irccd (also forked processes)
	// Defines the action to be performed
	char actmode = 0;
	int pid = 0;
	// Message buffers for sending and recieving 
	char buf[PIPE_BUF], out[PIPE_BUF], pos[PIPE_BUF];
	// File descriptor and for loop counter
	int fd, i;

	mkfifo(fifopath, 0666);
	while(1) {
		fd = open(fifopath, O_RDONLY); 
		memset(&buf, 0, sizeof(buf));
		read(fd, buf, sizeof(buf));
		actmode = buf[0];

		fprintf(stdout, PRGNAME": mode: %c\n", actmode);

		for (i = 0; i < sizeof(buf)-1; i++) {
			pos[i] = buf[i+1]; // Stores the message seperately from actcode
		}
		close(fd);

		switch (actmode) {
		case JOIN_MOD:
			if (chan_name_check(pos)) {
				break;
			}
			strcpy(chan_name, pos);
			sprintf(out, "JOIN %s\r\n", chan_name);
			if (send_msg(host_sockfd, out) > 0) {
				add_chan(channels, pos);
			}
			break;
		case PART_MOD:
			strcpy(chan_name, pos);
			sprintf(out, "PART %s\r\n", chan_name);

			if (send_msg(host_sockfd, out) > 0) {
				rm_chan(channels, chan_name);
			}
			break;
		case LIST_MOD:
			sprintf(out, "LIST %s\r\n", pos);
			send_msg(host_sockfd, out);
			break;
		case WRITE_MOD:
			sprintf(out, "PRIVMSG %s :%s\r\n", chan_name, pos);
			send_msg(host_sockfd, out);
			break;
		case NICK_MOD:
			if (strcmp(nick, pos) == 0) {
				fprintf(stdout, PRGNAME": nickname %s already in use "
								"by this client\n", nick);
				break;
			} 
			strcpy(nick, pos);
			sprintf(out, "NICK %s\r\n", nick);
			send_msg(host_sockfd, out);
			break;
		case CONN_MOD:
			// Test if we're already connected somewhere
			if (strcmp(host_serv, pos) == 0 && send_msg(host_sockfd, "PING\r\n") != -1) {
				fprintf(stdout, PRGNAME": server %s already connected to.\n", host_serv);
			} else {
				strcpy(host_serv, pos);
				if (sock_conn(host_serv, &host_sockfd) != 0) {
					fprintf(stderr, PRGNAME": error: connection failed\n");
					exit(1);
				}
				sprintf(out, "NICK %s\r\n", nick);
				send_msg(host_sockfd, out);
				sprintf(out, "USER %s 8 * :nick\r\n", nick);
				send_msg(host_sockfd, out);
				// Seperate process reads socket
				pid = spawn_reader(host_sockfd);
			}
			break;
		case PING_MOD:
			ping_host(host_sockfd, pos);
			break;
		case DISC_MOD:
			host_disc(&host_sockfd);
			break;
		case QUIT_MOD:
			kill(pid, SIGTERM);
			exit(0);
		default:
			printf("NO COMMAND\n");
			sleep(1);
		}
	}
	printf("Failed to fork\n");
	kill(pid, SIGTERM);
	exit(1);
}