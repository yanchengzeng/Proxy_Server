#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>

int server(uint16_t port, uint16_t cache);
void *connection(void *sock);
int parse_initial_request_header(const char* request_msg, char* request_header[]);
void print_string(const char* print);
int access_web(char* webpage_request[], char* content_buf, char* original_request);
int handle_request(int connect, char* website, char* webpage_buf, int* initial, char* request_header[]);
char* delete_end_newline(char* string);
void reply_request(char* webpage, int connect);
char* strip_http(char* string);
int retrieve_packet_length(char* data);

#define MAX_MSG_LENGTH (1024)
#define MAX_BACK_LOG (5)
#define DEFAULT_PAGE_SIZE (2000000)
#define HEADER_BUF_SIZE 1024

const char* default_address = "127.0.0.1";
const char* accept_type = "GET";
const char* http_1_0 = "HTTP/1.0";
const char* http_1_1 = "HTTP/1.1";
const char* port_80 = "80";
const char* cnn = "www.cnn.com";
const char* bbc = "www.bbc.com";
const char* content_length = "Content-Length:";

int main(int argc, char ** argv)
{
	if (argc < 2) {
		printf("Command should be: proxy <port> cache_size\n");
		return 1;
	}

	int port = atoi(argv[1]);

	if (port < 1024 || port > 65535) {
		printf("Port number should be equal to or larger than 1024 and smaller than 65535\n");
		return 1;
	}

	uint16_t cache_size = atoi(argv[2]);

	server(port, cache_size);

	return 0;
}

//process every new connection
int server(uint16_t port, uint16_t cache)
{
	int serverfd;
	struct sockaddr_in listenaddr;
	socklen_t listenlen = sizeof(struct sockaddr_in);
	int val;
	int *confd;
	pthread_t tid;

	if ((serverfd = socket(AF_INET,SOCK_STREAM,0)) < 0){
		perror("Create Server Error.");
		return 1;
	}

	bzero((char *) &listenaddr,sizeof(listenaddr));
	listenaddr.sin_family = AF_INET;
	listenaddr.sin_addr.s_addr = inet_addr(default_address);
	listenaddr.sin_port = htons(port);

	printf("The serverfd is %d\n", serverfd);

	if (setsockopt(serverfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&val, sizeof(int)) < 0){
		perror("Cannot resue address");
		return 1;
	}

	if (bind(serverfd, (struct sockaddr *)&listenaddr, sizeof(listenaddr)) < 0){
		perror("Cannot Bind.");
		return 1;
	}

	listen(serverfd, MAX_BACK_LOG);

	while(1){
		confd = malloc(sizeof(int));
		if ((*confd = accept(serverfd, (struct sockaddr *)&listenaddr, &listenlen)) < 0){
			perror("Accept error");
		}
		pthread_create(&tid, NULL, connection, confd);
	}

	printf("Server is closed");
	close(serverfd);
	return serverfd;
}

//Each thread handles one persistent connection
void *connection(void *sock){
	int connect = *((int *)sock);
	char received_msg[MAX_MSG_LENGTH], buf[DEFAULT_PAGE_SIZE];
	pthread_detach(pthread_self());
	free(sock);
	int msg_len;
	int initial_flag = 0;
	char* request_header[HEADER_BUF_SIZE];
	while(1){
		if ((msg_len = recv(connect, received_msg, MAX_MSG_LENGTH, 0)) < 0){
			perror("Receive fails");
		} else if(msg_len == 0){
			sleep(5);
		} else {
			if(handle_request(connect, received_msg, buf, &initial_flag, request_header) == 0){
				memset(received_msg, 0, sizeof(received_msg));
				memset(buf, 0, sizeof(buf));
			} else {
				perror("Cannot handle webpage request");
			}
		}

	}

	close(connect);
	return NULL;

}

//handle the HTTP request from browser and deliver the webpage
int handle_request(int connect, char* received_msg, char* webpage_buf, int* initial, char* request_header[]){
	printf("The Browser Request Message is:\n\n%sThe length is %zu.\n", received_msg, strlen(received_msg));
//	if(*initial == 0){
//		*initial = 1;
		if(parse_initial_request_header(received_msg, request_header) == 0){
			access_web(request_header, webpage_buf, received_msg);
			reply_request(webpage_buf, connect);
			return 0;
		} else {
			printf("Invalid Non-GET request");
			return -1;
		}
//	} else {
		//add error handling later for different error signals
//		access_web(request_header, webpage_buf, received_msg);
//		reply_request(webpage_buf, connect);
//		return 0;
//	}
}


void reply_request(char* webpage_buf, int connect){
		char* render_page;
		render_page = (char* )malloc(strlen(webpage_buf));
		strcpy(render_page, webpage_buf);
		printf("Requested page content from the remote server is:\n\n%s", render_page);
		if(send(connect, webpage_buf, DEFAULT_PAGE_SIZE, 0) < 0){
			perror("Cannot deliever web page");
		}
		printf("Sent Response\n\n");
}

//parse the HTTP request header
int parse_initial_request_header(const char* request_msg, char* request_header[]){
	char *header[1024];
	const char s[2] = " ";
	int index = 0;
	char* token;
	char* copy_msg;
	copy_msg = (char* )malloc(strlen(request_msg));
	strcpy(copy_msg, request_msg);
	token = strtok(copy_msg, s);
	while (token != NULL && strcmp(token, "\n") != 0){
//		printf("%s\n", token);
		header[index] = token;
		request_header[index] = token;
//		printf("debug header %d is %s\n", index, request_header[index]);
		index++;
		token = strtok(NULL, s);
	}
//	printf("Initial request type is %s and version is %s \n", request_header[0], request_header[2]);
	header[2] = delete_end_newline(header[2]);
	if(strcmp(accept_type, header[0]) == 0){ // && (strcmp(http_1_0, header[2]) == 0 || strcmp(http_1_1, header[2]) == 0)){
		printf("Request Type is correct!\n");
		return 0;
	} else {
		printf("The accepted header 0 is %s\n", header[0]);
		return 1;
	}
}

//delete the new line character at the end of the string
char* delete_end_newline(char* string){
	char* ss;
//	printf("string len is %zu\n", strlen(string));
	ss = (char* )malloc(strlen(string) - 1);
	int i;
	for(i = 0; i < strlen(string) - 2; i++){
		ss[i] = string[i];
	}
	ss[i] = '\0';
//	printf("ss is %s, string is %s\n", ss, string);
	return ss;
}


void print_string(const char *print){
	int i;
	printf("The string to be printed is \n");
	for(i = 0; i < strlen(print); i++){
		printf("%c", print[i]);
	}
}

char* strip_http(char* string){
	char* ss;
	ss = (char* )malloc(strlen(string));
	strcpy(ss, string);
	char* token;
	token = strtok(ss, ":");
	if(strlen(string) == strlen(token)){
		return ss;
	} else {
		token = strtok(NULL, ":");
		char* temp;
		temp = strtok(token, "//");
		return temp;
	}

}

//connect remote web server to fetch the page content
int access_web(char* webpage_request[], char *content_buf, char* original_request){
	//remove http head later
	struct addrinfo hints, *serverinfo, *p;
	int sockfd;
	int rv;
	printf("The website is %s\n\n", webpage_request[1]);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	char* website = strip_http(webpage_request[1]);
	printf("The stripped website is %s\n", website);
	if(rv = getaddrinfo(website, "http", &hints, &serverinfo) != 0){
		perror("Requesting Remote Server Failed");
   	}

	for(p = serverinfo; p != NULL; p = p->ai_next){
		if((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1){
			perror("Socket failed in acccess_web");
			continue;
		}

		if(connect(sockfd, p->ai_addr, p->ai_addrlen) == -1){
			close(sockfd);
			perror("Connect failed in access_web");
			continue;
		}

		break;
	}

	if(p == NULL){
		perror("Socket Not Avaiable in access_web");
		return -1;
	}

	char pin_page[500];
	int sendfd;
	if((sendfd = send(sockfd, original_request, strlen(original_request), 0)) < 0){
		perror("Cannot retransmit web request");
	}

	bzero(content_buf, DEFAULT_PAGE_SIZE);
	int recv_size = 0;
	if((recv_size = recv(sockfd, content_buf, DEFAULT_PAGE_SIZE, 0)) == -1){
		perror("Proxy cannot receive web content");
		return -1;
	}
//	retrieve_packet_length(content_buf);
	printf("Received %d bytes of data from web server\n", recv_size);
	return 0;
}

int retrieve_packet_length(char* data){
	char* token;
	char** header_buf;
	int i;
	int j;
	header_buf = (char** )malloc(sizeof(char* ) * 64);
	printf("debug1\n");
	for(i = 0; i < 64; i++){
		header_buf[i] = (char* )malloc(sizeof(char) * 1400);
//		for(j = 0; j < 2048; j++){
//			header_buf[i][j] = '0';
//		}
	}
	printf("debug2\n");
	int index = 0;
	token = strtok(data, " ");
	long result = 0;

	if(strcmp(token, http_1_1) == 0){
		printf("This is indeed a response header\n");
		while(token != NULL){
			printf("Token is %s ", token);
			header_buf[index] = token;
			index++;
			token = strtok(NULL, "  \n");
		}
		printf("The third spot is %s\n", header_buf[2]);
		int i;
		for(i = 0; i < index; i++){
			if(strcmp(content_length, header_buf[i]) == 0){
				char* ptr;
				result = strtol(header_buf[i+1], &ptr, 5);
				printf("The search len result is %ld\n", result);
			}
		}
		free(header_buf);
		return (int) result;
	} else {
		printf("This packet ain't a response header\n");
		free(header_buf);
		return -1;
	}
}
