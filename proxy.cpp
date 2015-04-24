#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <netdb.h>
#include <string>
#include <iostream>
#include <map>

#define MAX_BACK_LOG (5)
#define RCV_BUF_SIZE (1500)
#define DEBUG (0)

using namespace std;

typedef struct {
	string* op;
	string* page;
	string* version;
	string* host;
} get_request;

typedef struct {
	int conn_num;
	int client_fd;
	map<string, int> *host_fds;
	pthread_mutex_t hmap_lock;
	pthread_mutex_t *cmap_lock;
	int thread_count; // number of threads using the connection
} client_conn;

typedef struct {
	int req_num;
	string* data;
	client_conn* c_conn;
} client_request;

typedef struct {
	string *host_addr;
	client_conn* c_conn;
} host_downstream;

int server(char* port);
int create_tcp_conn(char* port, const char* addr);
void* handle_client_conn(void* conn_fd);
void* handle_host_downstream(void* conn_fd);
void* handle_client_request(client_request* c_req);
get_request* parse_get_request(string* request);
void* forward_data(int source_fd, int dest_fd);
int parse_get_response(string *response);

//TODO
// - when to close downstream connection

int main(int argc, char** argv)
{
	/* ignore sigpipe */
	signal(SIGPIPE, SIG_IGN);

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
	int ccount = 0;
	map<int, client_conn*> *conn_map = new map<int, client_conn*>;
	pthread_mutex_t cmap_lock;
	pthread_mutex_init(&cmap_lock, NULL);

	/* listen for incoming connections */
	listen(server_fd, MAX_BACK_LOG);

	// loop for client connections
	while(1){
		int client_fd;
		if ((client_fd = accept(server_fd, (struct sockaddr*) NULL, NULL)) < 0){
			perror("Accept error");
			return -1;
		}

		/* retreive or construct host structure */
		client_conn* c_conn;
		if(conn_map->find(client_fd) == conn_map->end() ||
				conn_map->operator[](client_fd)->thread_count <= 0) { // new connection
			cout << "CREATING NEW CONNECTION : " << ccount << endl;

			/* construct new client connection struct */
			c_conn = new client_conn;
			c_conn->conn_num = ccount;
			c_conn->client_fd = client_fd;
			c_conn->host_fds = new map<string, int>;
			c_conn->cmap_lock = &cmap_lock;
			c_conn->thread_count = 1;
			pthread_mutex_init(&c_conn->hmap_lock, NULL);
			ccount++;

			/* add connection to map */
			pthread_mutex_lock(&cmap_lock);
			conn_map->operator[](client_fd) = c_conn;
			pthread_mutex_unlock(&cmap_lock);

		} else { // existing connection
			cout << "EXISTING CONNECTION" << endl;
			c_conn = conn_map->operator[](client_fd);
			c_conn->thread_count++;
		}

		// dispatch worker thread to handle connection
		pthread_t worker;
		pthread_create(&worker, NULL, handle_client_conn, (void*) c_conn);
	}

	/* close server socket */
	close(server_fd);

	return 0;
}

void* handle_client_conn(void* conn_ptr) {
	client_conn* conn = (client_conn*) conn_ptr;
	int client_fd = conn->client_fd;
	int rcount = 0;

	/* debug */
	if(1) {
		cout << "=============================" << endl;
		cout << "NEW CONNECTION" << endl;
		cout << "-----------------------------" << endl;
		cout << "Connection No. " << conn->conn_num << endl;
		cout << "Socket No. " << conn->client_fd << endl;
		cout << "=============================" << endl;
	}

	/* loop for client messages */
	while (1) {
		char rcv_buf[RCV_BUF_SIZE]; // receive data buffer
		ssize_t rcv_len; // receive data length

		/* receive message from client */
		rcv_len = recv(client_fd, (void*) rcv_buf, sizeof(rcv_buf), 0);

		if (rcv_len == 0) { // client has closed the connection
			cout << "Closing Connection No. " << conn->conn_num << endl;
			break;
		} else if (rcv_len == -1) { // receive error
			char err[100];
			sprintf(err, "Connection No. %d receive error", conn->conn_num);
			perror(err);
			break;
		} else { // handle HTTP request
			string* rcv_str = new string(rcv_buf, rcv_len);
			client_request* c_req = new client_request;
			c_req->req_num = rcount;
			c_req->data = rcv_str;
			c_req->c_conn = conn;
			rcount++;

			handle_client_request(c_req);
		}
	}

	/* set connection closed flag */
	pthread_mutex_lock(conn->cmap_lock);
	conn->thread_count--;

	/* delete connection if no other threads are using it */
	if(conn->thread_count == 0) {

		/* close all sockets */
		pthread_mutex_lock(&conn->hmap_lock);
		map<string, int>::iterator it;
		for(it = conn->host_fds->begin(); it != conn->host_fds->end(); it++) {
			close(it->second);
			//TODO kill downstream threads
		}
		pthread_mutex_unlock(&conn->hmap_lock);
		close(client_fd);

		/* free connection struct memory */
		//delete conn->host_fds;
		delete conn;
	}
	pthread_mutex_unlock(conn->cmap_lock);

	pthread_exit(NULL);
}

void* handle_client_request(client_request* req) {
	client_conn* conn = req->c_conn;
	string* request = req->data;

	/* debug */
	if(1) {
		cout << "=============================" << endl;
		cout << "NEW REQUEST : " << req->req_num << endl;
		cout << "-----------------------------" << endl;
		cout << "Connection No. " << conn->conn_num << endl;
		cout << "Socket No. " << conn->client_fd << endl;
		cout << "-----------------------------" << endl;
		cout << *request << endl;
		cout << "=============================" << endl;
	}

	/* return if not GET request */
	//if(request->find("GET") != 0) {
		//cout << "Ignoring non-GET request." << endl;
		//return NULL;
	//}

	/* parse get request */
	get_request* greq = parse_get_request(request);

	/* check if requested page is cached */
	// TODO

	/* get host socket */
	int host_fd;
	string host_addr = *greq->host;
	map<string, int> *host_map = conn->host_fds;

	if(host_map->find(host_addr) == host_map->end()) { // no connection to host exists
		pthread_mutex_lock(&conn->hmap_lock);
		host_fd = create_tcp_conn((char*) "http", host_addr.c_str()); // create connection
		host_map->operator[](host_addr) = host_fd; // add connection to host map
		pthread_mutex_unlock(&conn->hmap_lock);

		host_downstream *hds = new host_downstream;
		hds->host_addr = greq->host;
		hds->c_conn = conn;

		pthread_t worker;
		pthread_create(&worker, NULL, handle_host_downstream, (void*) hds);
	} else { // connection to host exists
		cout << "Host in map." << endl;
		host_fd = host_map->operator[](host_addr);
	}

	/* forward request to host */
	int send_len;
	cout << "Sending to : " << host_fd << endl;
	if((send_len = send(host_fd, request->c_str(), request->length(), 0)) < 0) {
		perror("Request forwarding error");
	}
}

void* handle_host_downstream(void *host_dstream) {
	host_downstream* dstream = (host_downstream*) host_dstream;
	string host_addr = *dstream->host_addr;
	int client_fd = dstream->c_conn->client_fd;
	map<string, int> *host_map = dstream->c_conn->host_fds;
	int host_fd = host_map->operator[](host_addr);

	/* forward data from host to client */
	forward_data(host_fd, client_fd);

	/* remove host from map and close socket */
	pthread_mutex_lock(&dstream->c_conn->hmap_lock);
	host_map->erase(host_addr);
	close(host_fd);
	pthread_mutex_unlock(&dstream->c_conn->hmap_lock);

	pthread_exit(NULL);
}

void* forward_data(int source_fd, int dest_fd) {
	char fwd_buf[RCV_BUF_SIZE];
	ssize_t fwd_len;
	struct timeval tv;
	fd_set read_fds;
	int parse_code;

	tv.tv_sec = 100;
	tv.tv_usec = 500000;

	/* setup read fd set */
	FD_ZERO(&read_fds);
	FD_SET(source_fd, &read_fds);

	while(1) {

		/* wait for something to read on source socket */
		select(source_fd + 1, &read_fds, NULL, NULL, &tv);

		if(FD_ISSET(source_fd, &read_fds)) {

			/* recieve from source */
			fwd_len = recv(source_fd, fwd_buf, sizeof(fwd_buf), 0);

			if(fwd_len == -1) {
				perror("Host receive error");
				break;
			} else if(fwd_len == 0) {
				cout << "Host closed connection." << endl;
				break;
			}

			/* parse response and add to cache if appropriate */
			string *response = new string(fwd_buf, fwd_len);
			parse_code = parse_get_response(response);

			/* forward data to destination */
			if(send(dest_fd, fwd_buf, fwd_len, 0) <= 0) {
				perror("Downstream send error");
				break;
			}
		}
	}
}

get_request* parse_get_request(string* request) {
	/* parse operation type */
	size_t first_space = request->find(" ");
	string op = request->substr(0, first_space);

	/* parse page */
	size_t second_space = request->find(" ", first_space + 1);
	string page = request->substr(first_space + 1, second_space - first_space - 1);

	/* parse version */
	size_t first_crlf = request->find("\n");
	string version = request->substr(second_space + 1, first_crlf - second_space - 1);

	/* parse host */
	string host_tag = "Host: ";
	size_t host_start = request->find(host_tag) + host_tag.length();
	size_t host_end = request->find("\n", host_start);
	string host = request->substr(host_start, host_end - host_start - 1);

	/* debug printing */
	if(DEBUG) {
		cout << "Op: " << op << endl;
		cout << "Page: " << page << endl;
		cout << "Version: " << version << endl;
		cout << "Host: " << host << endl;
	}

	/* generate get_request struct */
	get_request* greq = new get_request;
	greq->op = new string(op);
	greq->page = new string(page);
	greq->version = new string(version);
	greq->host = new string(host);

	return greq;
}

// Parse codes:
// -1 => don't buffer
// 0  => start new buffer
// >0 => append to current buffer
int parse_get_response(string *response) {
	cout << "*************************" << endl;
	cout << "RESPONSE" << endl;
	cout << "*************************" << endl;
	cout << response << endl;
	cout << "*************************" << endl;

	/* check that response has http header*/
	size_t http_loc = response->find("HTTP/1.1");
	if(http_loc != 0) {
		cout << "Response doesn't have HTTP header." << endl;
		return -1;
	}

	/* check that response has success code */
	size_t two_loc = response->find("2");
	if(two_loc != 9) {
		cout << "Response no successful." << endl;
		return -1;
	}

	/* parse content length */
	string cl_tag = "Content-Length: ";
	size_t cl_start = response->find(cl_tag) + cl_tag.length();
	size_t cl_end = response->find("\n", cl_start);
	string cl = response->substr(cl_start, cl_start - cl_start - 1);
	cout << "Content-Length: " << cl << endl;
	return atoi(cl.c_str());
}

int create_tcp_conn(char* port, const char* addr) {
	int sockfd_ls; // listen socket descriptor
	long sockfd_ac; // accept socket descriptor
	struct addrinfo ai_hints; // hints address info
	struct addrinfo *ai_list; // linked list of address info structs returned by get address info
	struct addrinfo *ai; // valid address info

	/* fill hints address info */
	memset(&ai_hints, 0, sizeof(ai_hints));
	ai_hints.ai_family = AF_UNSPEC;
	ai_hints.ai_socktype = SOCK_STREAM;
	if (addr == NULL) ai_hints.ai_flags = AI_PASSIVE;

	/* get address info */
	int code;
	if ((code = getaddrinfo(addr, port, &ai_hints, &ai_list)) != 0) {
		cout << "Get address info error: " << gai_strerror(code) << endl;
		return -1;
	}

	/* loop through ai linked list and open socket on first valid addrinfo struct */
	for (ai = ai_list; ai != NULL; ai = ai->ai_next) {

		/* try to open a socket */
		if ((sockfd_ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) != -1) {

			if(addr == NULL) {
				/* try to bink socket to port */
				if (bind(sockfd_ls, ai->ai_addr, ai->ai_addrlen) == 0) {
					break;
				} else {
					perror("Socket binding error");
				}
			} else {
				/* try to connect to address */
				if (connect(sockfd_ls, ai->ai_addr, ai->ai_addrlen) == 0) {
					break;
				} else {
					perror("Socket connect error");
				}
			}

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
