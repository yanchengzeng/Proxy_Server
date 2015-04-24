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
#include <queue>
#include <list>

#define MAX_BACK_LOG (5)
#define RCV_BUF_SIZE (1500)
#define DEBUG (0)
#define MEGABYTE (1048576)

using namespace std;
struct host_conn;

map<int, queue<string*>*> *cache_name_map;
map<string, string*> *cache;
list<string> *evict_list;
pthread_mutex_t cache_map_lock;
pthread_mutex_t cache_lock;
long max_cache_size;
long cur_cache_size;

typedef struct {
	string* op;
	string* page;
	string* version;
	string* host;
} get_request;

typedef struct {
	int conn_num;
	int client_fd;
	map<string, host_conn> *host_fds;
	pthread_mutex_t hmap_lock;
	pthread_mutex_t *cmap_lock;
	int thread_count; // number of threads using the connection
} client_conn;

typedef struct {
	int req_num;
	string* data;
	client_conn* c_conn;
} client_request;

struct host_conn{
	string *host_addr;
	int host_fd;
	queue<string*> *req_queue;
	client_conn* c_conn;
};
typedef struct host_conn host_conn;

int server(char* port);
int create_tcp_conn(char* port, const char* addr);
void* handle_client_conn(void* conn_fd);
void* handle_host_downstream(void* conn_fd);
void* handle_client_request(client_request* c_req);
get_request* parse_get_request(string* request);
void* forward_data(int source_fd, int dest_fd);
int parse_get_response(string *response);

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

	max_cache_size = atoi(argv[2]) * MEGABYTE;
	cur_cache_size = 0;

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

	/* construct cache name map */
	cache_name_map = new map<int, queue<string*>*>;
	cache = new map<string, string*>;
	evict_list = new list<string>;
	pthread_mutex_init(&cache_map_lock, NULL);
	pthread_mutex_init(&cache_lock, NULL);

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
			c_conn->host_fds = new map<string, host_conn>;
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
	time_t start_time = time(NULL);

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

		/* check for timeout */
		//if(time(NULL) - start_time >= 3) {
			//cout << "TIMEOUT" << endl;
			//break;
		//}

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
		map<string, host_conn>::iterator it;
		for(it = conn->host_fds->begin(); it != conn->host_fds->end(); it++) {
			close(it->second.host_fd);
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

	/* define page entry depending on request type */
	string *page;
	cout << "greq->op : " << *greq->op << endl;
	if(*greq->op == ("GET")) {
		page = new string(*greq->page);
	} else {
		page = new string("NO_CACHE");
	}

	/* check if requested page is cached */
	cout << "CACHE: checking for " << *page << endl;
	if(cache->find(*page) != cache->end()) {
		cout << "CACHE: found data for " << *page << endl;
		string *cache_entry = cache->operator[](*page);

		/* update evict list */
		evict_list->remove(*page);
		evict_list->push_back(*page);

		/* cache hit -- send data to client */
		if(send(conn->client_fd, cache_entry->c_str(), cache_entry->length(), 0) <= 0) {
			perror("Cache send error");
		}
		return NULL;
	}

	/* get host socket */
	int host_fd;
	string host_addr = *greq->host;
	map<string, host_conn> *host_map = conn->host_fds;

	if(host_map->find(host_addr) == host_map->end()) { // no connection to host exists
		pthread_mutex_lock(&conn->hmap_lock);
		host_fd = create_tcp_conn((char*) "http", host_addr.c_str()); // create connection

		host_conn *h_conn = new host_conn;
		h_conn->host_addr = greq->host;
		h_conn->host_fd = host_fd;
		h_conn->req_queue = new queue<string*>;
		h_conn->c_conn = conn;

		host_map->operator[](host_addr) = *h_conn; // add connection to host map
		pthread_mutex_unlock(&conn->hmap_lock);

		pthread_t worker;
		pthread_create(&worker, NULL, handle_host_downstream, (void*) h_conn);
	} else { // connection to host exists
		cout << "Host in map." << endl;
		host_fd = host_map->operator[](host_addr).host_fd;
	}

	/* populate host name map */
	pthread_mutex_lock(&cache_map_lock);
	if(cache_name_map->find(host_fd) == cache_name_map->end()) { // no entry exists
		/* initialize new page queue and push current page */
		queue<string*> *req_queue = new queue<string*>;
		req_queue->push(new string("FIRST"));
		req_queue->push(page);

		/* add queue to cache map */
		cache_name_map->operator[](host_fd) = req_queue;
	} else { // entry exists
		cache_name_map->operator[](host_fd)->push(page);
	}
	pthread_mutex_unlock(&cache_map_lock);


	/* forward request to host */
	int send_len;
	cout << "Sending to : " << host_fd << endl;
	if((send_len = send(host_fd, request->c_str(), request->length(), 0)) < 0) {
		perror("Request forwarding error");
	}
}

void* handle_host_downstream(void *h_conn_ptr) {
	host_conn* h_conn = (host_conn*) h_conn_ptr;
	string host_addr = *h_conn->host_addr;
	int client_fd = h_conn->c_conn->client_fd;
	int host_fd = h_conn->host_fd;
	map<string, host_conn> *host_map = h_conn->c_conn->host_fds;

	/* forward data from host to client */
	forward_data(host_fd, client_fd);

	/* remove host from map and close socket */
	pthread_mutex_lock(&h_conn->c_conn->hmap_lock);
	host_map->erase(host_addr);
	close(host_fd);
	pthread_mutex_unlock(&h_conn->c_conn->hmap_lock);

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
			string *page_name;
			string *response = new string(fwd_buf, fwd_len);
			parse_code = parse_get_response(response);

				if(cache_name_map->find(source_fd) == cache_name_map->end()) {
					cout << source_fd << " CACHE: host fd not in cache name map" << endl;
				} else {
					/* get page name */
					pthread_mutex_lock(&cache_map_lock);
					queue<string*> *req_queue = cache_name_map->operator[](source_fd);
					if(parse_code <= 0) req_queue->pop();
					page_name = req_queue->front();
					pthread_mutex_unlock(&cache_map_lock);

					/* do appropriate caching */
					if(*page_name == "NO_CACHE") {
						cout << source_fd << " CACHE: skipping non-GET data" << endl;
					} else if(*page_name == "FIRST") {
						cout << source_fd << " CACHE: huge error, got FIRST" << endl;
					} else {

						/* evict cache block if not enough space */
						pthread_mutex_lock(&cache_lock);
						cur_cache_size += response->length();
						while(cur_cache_size > max_cache_size) {
							cout << source_fd << " CACHE: not enough space to store response. evicting block by LRU." << endl;

							/* get least recently used block */
							string lru_block = evict_list->front();
							evict_list->pop_front();

							/* remove block from cache */
							cur_cache_size -= cache->operator[](lru_block)->length();
							cache->erase(lru_block);
						}

						if(parse_code == 0) {
							cout << source_fd << " CACHE: adding new entry " << *page_name << endl;
							cache->operator[](*page_name) = response;
							evict_list->push_back(*page_name);
						} else if(parse_code == 1) {
							cout << source_fd << " CACHE: appending to existing entry " << *page_name << endl;
							cache->operator[](*page_name)->operator+=(*response);
						}
						pthread_mutex_unlock(&cache_lock);
					}
				}

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
//  0 => start new buffer
// >0 => append to current buffer
int parse_get_response(string *response) {
	size_t http_loc = response->find("HTTP/1.1");
	if(http_loc == 0) {
		size_t ok_loc = response->find("200");
		if(ok_loc == 9) {
			cout << "Response is new data and reads success." << endl;
			cout << "*****************************" << endl;
			cout << *response << endl;
			cout << "*****************************" << endl;
			return 0;
		} else {
			cout << "Response is new data and reads non-success." << endl;
			return -1;
		}
	} else {
		cout << "Response is continued data." << endl;
		return 1;
	}
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
