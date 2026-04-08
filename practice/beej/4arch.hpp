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
#include <fstream>	

#define PORT "3890"
#define IP "127.0.0.1"
#define BUFF_SIZE 1024
#define TOUT 1000

class HTTPRequest;

class HTTPResponse
{
public:
	HTTPResponse();
	void construct_response(const HTTPRequest& req);
	void build(const HTTPRequest& req);
	void prepare_GET(const HTTPRequest& req);
	void prepare_POST(const HTTPRequest& req);
	void prepare_DELETE(const HTTPRequest& req);
	void prepare_else(const HTTPRequest& req);
	std::string get_content_type(const std::string& uri);
	std::string get_raw_response() const;
	void handle_body(const std::string& path);

private:
	std::string _version;
	int _statusCode;
	std::string _reason;
	std::string _body;
	std::string _contentType;
	std::string final_response;
};

class HTTPRequest
{
public:
	HTTPRequest();
	bool IsParsed();
	void AddRawP(const char* line, int nbytes);
	void Validate(const std::string& line);
	const std::string& getMethod() const;
	const std::string& getUri() const;
	const std::map<std::string, std::string>& getMap() const;
	const std::string& getVersion() const;

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
	void close_connection(size_t& index);
private:
	int sock;
	std::vector<struct pollfd> _fds;
	std::map<int, HTTPRequest> _requests;
	std::map<int, HTTPResponse> _responses;
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
