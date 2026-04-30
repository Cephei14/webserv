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
#include <dirent.h>
#include <sys/time.h>

#define BUFF_SIZE 524288
#define TOUT 500

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
	size_t _client_max_body_size;
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
	void prepare_GET(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc);
	void prepare_POST(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc);
	void prepare_DELETE(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc);
	std::string get_content_type(const std::string& uri);
	const std::string& get_raw_response() const;
	void body_GET(const std::string& path);
	void body_POST(const std::string& path, const std::string& body, size_t size);
	std::string handle_chunks(const HTTPRequest& req);
	void build_error_response(int status_code, ServerConfig& config);

private:
	std::string _version;
	size_t _data_size;
	int _statusCode;
	std::string _reason;
	std::string _body;
	std::string _contentType;
	std::string _post_body;
	std::string _final_response;
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
	void clearBody();
	const std::string& getMethod() const;
	const std::string& getUri() const;
	const std::map<std::string, std::string>& getMap() const;
	const std::string& getVersion() const;
	const std::string& getBody() const;

private:
	std::string _method;
	std::string _uri;
	std::string _version;
	std::string _raw_buf;
	std::string _body;
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
	struct CgiJob
	{
		CgiJob()
			: client_fd(-1), server_index(0), pid(-1), in_fd(-1), out_fd(-1), write_offset(0), request_body_size(0), use_decoded_body(false), start_ms(0), child_done(false), child_status(0)
		{}
		int client_fd;
		int server_index;
		pid_t pid;
		int in_fd;
		int out_fd;
		std::string request_body;
		size_t write_offset;
		size_t request_body_size;
		bool use_decoded_body;
		std::string output;
		long start_ms;
		long last_activity_ms; 
		bool child_done;
		int child_status;
	};

	bool is_cgi_request(const HTTPRequest& req, ServerConfig& srv, LocationConfig*& script_loc, LocationConfig*& cgi_loc, std::string& uri_path);
	bool start_cgi_job(int client_fd, const HTTPRequest& req, ServerConfig& srv, const LocationConfig& script_loc, const LocationConfig& cgi_loc, const std::string& uri_path, int server_index);
	void process_cgi_pipe_event(size_t& i, Config& servers);
	void finalize_cgi_job(int client_fd, Config& servers, bool success, int status_code);
	void cleanup_cgi_job(int client_fd, bool kill_child);
	void set_client_events(int client_fd, short events);
	bool get_keep_alive(const HTTPRequest& req) const;
	int resolve_server_index(int client_fd, const HTTPRequest& req, const Config& servers) const;

	std::map<int, int> _port_socket;
	std::map<int, int> _listener_to_server;
	std::vector<struct pollfd> _fds;
	std::map<int, HTTPRequest> _requests;
	std::map<int, HTTPResponse> _responses;
	std::map<int, std::string> _pending_response;
	std::map<int, int> _client_to_server;
	std::map<int, bool> _fd_keep_alive;
	std::map<int, CgiJob> _cgi_jobs;
	std::map<int, int> _cgi_in_to_client;
	std::map<int, int> _cgi_out_to_client;
};

class AppManager
{
public:
	static void signal_handler(int s);
	void run(Config& servers);
};
