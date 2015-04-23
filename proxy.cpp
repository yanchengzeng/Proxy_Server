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
#include <map>

//TODO try changing to local host
#define LOCAL_ADDR "127.0.0.1"
#define MAX_BACK_LOG (5)
#define MAX_PKT_SIZE (1500)

using namespace std;

typedef struct {
	string* op;
	string* page;
	string* version;
	string* host;
} get_request;

typedef struct {
	int client_fd;
	map<string, int>* host_fds;
} client_conn;

typedef struct {
	string* data;
	client_conn* c_conn;
} request;

int server(char* port);
int create_tcp_conn(char* port, char* addr);
void* handle_client_conn(void* conn_fd);
void* handle_request(void* request_ptr);
get_request* parse_get_request(string* request);

int main(int argc, char** argv)
{
	if (argc < 2) {
		printf("Command should be: proxy <port> cache_size\n");
		return 1;
	}

	char* port_str = argv[1];
	int port = atoi(port_str);

	if (port < 1024 || port > 65535) {
		printf("Port number should be equal to or larger than 1024 and smaller than 65535\n");
		return 1;
	}

	int cache_size = atoi(argv[2]);

	server(port_str);

	return 0;
}


int server(char* port)
{
	int server_fd = create_tcp_conn(port, NULL);

	/* listen for incoming connections */
	listen(server_fd, MAX_BACK_LOG);

	// loop for client connections
	while(1){
		int conn_fd;
		if ((conn_fd = accept(server_fd, (struct sockaddr*) NULL, NULL)) < 0){
			perror("Accept error");
			return -1;
		}

		// construct new client connection struct
		client_conn* c_conn = new client_conn;
		c_conn->client_fd = conn_fd;
		c_conn->host_fds = new map<string, int>;

		// dispatch worker thread to handle connection
		pthread_t worker;
		pthread_create(&worker, NULL, handle_client_conn, (void*) c_conn);
	}

	/* close server socket */
	close(server_fd);

	/* exit thread without killing children */
	pthread_exit(NULL);

	return 0;
}

int create_tcp_conn(char* port, char* addr) {
	int sockfd_ls; // listen socket descriptor
	long sockfd_ac; // accept socket descriptor
	struct addrinfo ai_hints; // hints address info
	struct addrinfo *ai_list; // linked list of address info structs returned by get address info
	struct addrinfo *ai; // valid address info

	/* fill hints address info */
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	if(addr == NULL) ai_hints.ai_flags = AI_PASSIVE;

	/* get address info */
	if (getaddrinfo(addr, port, &ai_hints, &ai_list) != 0) {
		perror("Get address info error");
		return -1;
	}

	/* loop through ai linked list and open socket on first valid addrinfo struct */
	for (ai = ai_list; ai != NULL; ai = ai->ai_next) {

		/* try to open a socket */
		if ((sockfd_ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) != -1) {

			/* try to bink socket to port */
			if (bind(sockfd_ls, ai->ai_addr, ai->ai_addrlen) == 0) {

				/* socket opened and bound */
				break;

			} else perror("Socket binding error");

		} else perror("Socket open error");
	}

	/* return error if no valid addrinfos found */
	if (ai == NULL) {
		printf("No valid address info structure found.\n");
		return -1;
	}

	/* free memory */
	freeaddrinfo(ai_list);

	return sockfd_ls;
}


void* handle_client_conn(void* c_conn_ptr) {
	client_conn* c_conn = (client_conn*) c_conn_ptr;
	int sockfd = c_conn->client_fd;

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
			perror("Receive error");
			continue;
		} else { // handle HTTP request
			string* rcv_str = new string(rcv_buf);
			request* req = new request;
			req->data = rcv_str;
			req->c_conn = c_conn;

			//TODO how to handle the case of two requests to same host?
			pthread_t worker;
			pthread_create(&worker, NULL, handle_request, (void*) req);
		}
	}

	/* close socket and free connection struct */
	close(sockfd);
	delete c_conn->host_fds;
	delete c_conn;

	pthread_exit(NULL);
}

void* handle_request(void* req_ptr) {
	request* req = (request*) req_ptr;
	client_conn* c_conn = req->c_conn;
	string* request = req->data;
	cout << *request << endl;

	/* return if not GET request */
	if(request->find("GET") != 0) {
		cout << "Ignoring non-GET request." << endl;
		return NULL;
	}

	/* parse get request */
	get_request* greq = parse_get_request(request);

	/* check if requested page is cached */
	// TODO

	/* forward request to server */
	/* get host socket */
	int host_fd;
	map<string, int> host_map = *c_conn->host_fds;
	if(host_map.find(*greq->host) == host_map.end()) { // no connection to host exists
		// open new connection
		// spawn listener thread
	} else { // connection to host exists
		host_fd = host_map[*greq->host];
	}

	/* forward request to host */
	if(send(host_fd, request->c_str(), request->length(), 0) < 0) {
		perror("Request forwarding error");
	}
}

get_request* parse_get_request(string* request) {
	/* parse operation type */
	size_t first_space = request->find(" ");
	string op = request->substr(0, first_space);
	cout << "Op: " << op << endl;

	/* parse page */
	size_t second_space = request->find(" ", first_space + 1);
	string page = request->substr(first_space + 1, second_space - first_space - 1);
	cout << "Page: " << page << endl;

	/* parse version */
	size_t first_crlf = request->find("\n");
	string version = request->substr(second_space + 1, first_crlf - second_space - 1);
	cout << "Version: " << version << endl;

	/* parse host */
	string host_tag = "Host: ";
	size_t host_start = request->find(host_tag) + host_tag.length();
	size_t host_end = request->find("\n", host_start);
	string host = request->substr(host_start, host_end - host_start - 1);
	cout << "Host: " << host << endl;

	/* generate get_request struct */
	get_request* greq = new get_request;
	greq->op = new string(op);
	greq->page = new string(page);
	greq->version = new string(version);
	greq->host = new string(host);

	return greq;
}






