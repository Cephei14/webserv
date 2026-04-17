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
#include <sys/stat.h>

#define BUFF_SIZE 1024
#define TOUT 1000

class LocationConfig
{
public:
	LocationConfig();
	std::string _root;
	std::vector<std::string> _index;
	std::vector<std::string> _cgi_ext;
	std::string _path;
	std::string _cgi_path;
	std::vector<std::string> _allowed_methods;
	bool _autoindex;
	int _return_code;
	std::string _return_url;
};

class ServerConfig
{
public:
	ServerConfig();
	int _port;
	size_t _client_max_body_size;
	std::string _root;
	std::vector<std::string> _index;
	std::string _host;
	std::vector<std::string> _server_names;
	std::map<int, std::string> _error_pages;
	std::vector<LocationConfig> _locations;
};

class Config
{
public:
	std::vector<ServerConfig> _servers;
};

class HTTPRequest;

class HTTPResponse
{
public:
	HTTPResponse();
	void construct_response(const HTTPRequest& req);
	void build(const HTTPRequest& req, ServerConfig& srv);
	void prepare_GET(const HTTPRequest& req, ServerConfig& srv);
	void prepare_POST(const HTTPRequest& req, ServerConfig& srv);
	void prepare_DELETE(const HTTPRequest& req, ServerConfig& srv);
	void prepare_CGI(const HTTPRequest& req, ServerConfig& srv);
	std::string get_content_type(const std::string& uri);
	std::string get_raw_response() const;
	void body_GET(const std::string& path);
	void body_POST(const std::string& path);
	void handle_chunks(const HTTPRequest& req);
	void build_error_response(int status_code, ServerConfig& config);
	std::pair<std::string, std::string> split_uri_path_query(const std::string& uri);

private:
	std::string _version;
	size_t _data_size;
	int _statusCode;
	std::string _reason;
	std::string _body;
	std::string _contentType;
	std::string _post_body;
	std::string final_response;
};

class HTTPRequest
{
public:
	HTTPRequest();
	bool IsParsed();
	void AddRawP(const char* line, int nbytes);
	void Validate(const std::string& line);
	void consume_parsed_request();
	void reset_parse_state();
	const std::string& getMethod() const;
	const std::string& getUri() const;
	const std::map<std::string, std::string>& getMap() const;
	const std::string& getVersion() const;
	std::string getBody() const;

private:
	std::string _method;
	std::string _uri;
	std::string _version;
	std::string _raw_buf;
	std::map<std::string, std::string> _headers;
	bool _isParsed;
	bool _headersParsed;
	size_t _parsed_request_end;
};

class server
{
public:
	void start_listening(Config& servers);
	void srv_manage(Config& servers);
	void poll_setup(int newfd);
	void close_connection(size_t& index);
private:
	std::map<int, int> _port_socket;
	std::map<int, int> _listener_to_server;
	std::vector<struct pollfd> _fds;
	std::map<int, HTTPRequest> _requests;
	std::map<int, HTTPResponse> _responses;
	std::map<int, std::string> _pending_response;
	std::map<int, int> _client_to_server;
	std::map<int, bool> _fd_keep_alive;
};

// getaddrinfo() ; freeaddrinfo() ; socket() ; bind() ; listen() ; accept() ; recv() ; close(); setsockopt

class AppManager
{
public:
	static void signal_handler(int s);
	void run(Config& servers);
};
