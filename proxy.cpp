#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>
#include <string>
#include <iostream>

//TODO try changing to local host
#define SERVER_IP "127.0.0.1"
#define MAX_BACK_LOG (5)
#define MAX_PKT_SIZE (1500)

using namespace std;

int server(uint16_t port, uint16_t cache);
void* handle_connection(void* conn_fd);
void* handle_request(void* request_ptr);

int main(int argc, char** argv)
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


int server(uint16_t port, uint16_t cache)
{
	int server_fd;
	struct sockaddr_in listenaddr;
	socklen_t listenlen = sizeof(struct sockaddr_in);
	int val;

	/* create socket */
	if ((server_fd = socket(AF_INET,SOCK_STREAM,0)) < 0){
		perror("Create server error.");
		return -1;
	}

	bzero((char*) &listenaddr,sizeof(listenaddr));
	listenaddr.sin_family = AF_INET;
	listenaddr.sin_addr.s_addr = inet_addr(SERVER_IP);
	listenaddr.sin_port = htons(port);

	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&val, sizeof(int)) < 0){
		perror("Cannot reuse address.");
		return -1;
	}

	if (bind(server_fd, (struct sockaddr*)&listenaddr, sizeof(listenaddr)) < 0){
		perror("Cannot bind.");
		return -1;
	}

	/* listen for incoming connections */
	listen(server_fd, MAX_BACK_LOG);

	// loop for client connections
	while(1){
		int* conn_fd = new int;
		if ((*conn_fd = accept(server_fd, (struct sockaddr*)&listenaddr, &listenlen)) < 0){
			perror("Accept error");
			return -1;
		}
		pthread_t worker;
		pthread_create(&worker, NULL, handle_connection, (void*) conn_fd);
	}

	/* close server socket */
	close(server_fd);

	/* exit thread without killing children */
	pthread_exit(NULL);

	return 0;
}

void* handle_connection(void* sockfd_ptr) {
	int sockfd = *((int*) sockfd_ptr);

	/* loop for client messages */
	while (1) {
		//TODO how long is HTTP requst?
		char rcv_buf[MAX_PKT_SIZE]; // receive data buffer
		ssize_t rcv_len; // receive data length

		/* receive message from client */
		rcv_len = recv(sockfd, (void*) rcv_buf, sizeof(rcv_buf), 0);

		if (rcv_len == 0) { // client has closed the connection
			break;
		} else if (rcv_len == -1) { // receive error
			perror("Receive error:");
			continue;
		} else { // handle HTTP request
			string* rcv_str = new string(rcv_buf);
			pthread_t worker;
			pthread_create(&worker, NULL, handle_request, (void*) rcv_str);
		}
	}

	/* close socket and free memory */
	close(sockfd);
	delete (int*) sockfd_ptr;

	pthread_exit(NULL);
}

void* handle_request(void* request_ptr) {
	string request = *((string*) request_ptr);

	/* return if not GET request */
	if(request.find("GET") != 0) {
		cout << "Ignoring non-GET request." << endl;
		return NULL;
	}

}
