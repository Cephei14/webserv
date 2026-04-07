#include <iostream>
#include <unistd.h>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>
#include <sstream>
#include <cstdlib>
#include <vector>
#include <poll.h>
#include <map>
#include <fcntl.h>
#include <cctype>
#include <stdexcept>	

#define PORT "3890"
#define IP "127.0.0.1"
#define BUFF_SIZE 1024
#define TOUT 1000

class HTTPRequest
{
public:
	HTTPRequest();
	void AddRawP(const char* line, int nbytes);
	void Validate(const std::string& line);

private:
	std::string _method;
	std::string _uri;
	std::string _version;
	std::string _raw_buf;
	std::map<std::string, std::string> _headers;
	bool _isParsed;
	bool _headersParsed;
};

class server
{
public:
	void start_listening();
	void srv_manage();
	void poll_setup(int newfd);
private:
	int sock;
	std::vector<struct pollfd> _fds;
	std::map<int, HTTPRequest> _requests;
};

// getaddrinfo() ; freeaddrinfo() ; socket() ; bind() ; listen() ; accept() ; recv() ; close(); setsockopt

class client
{
public:
	void start_cnx();
	
private:
	int sock;
	int port;
	std::string ip;
};

// getaddrinfo() ; socket() ; connect() ; send();

class AppManager
{
public:
	static void signal_handler(int s);
	void run();
};
