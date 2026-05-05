#ifndef SERVER_HPP
# define SERVER_HPP

# include "Config.hpp"
# include "HTTPRequest.hpp"
# include "HTTPResponse.hpp"

class server
{
public:
	server();
	void start_listening(Config& servers);
	void srv_manage(Config& servers);
	void poll_setup(int newfd);
	void close_connection(size_t& index);
private:
	bool can_read_upload_body(int client_fd, const HTTPRequest& req);
	void release_upload_slot(int client_fd);
	void resume_one_waiting_upload();
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
	bool start_cgi_job(int client_fd, HTTPRequest& req, ServerConfig& srv, const LocationConfig& script_loc, const LocationConfig& cgi_loc, const std::string& uri_path, int server_index);
	void process_cgi_pipe_event(size_t& i, Config& servers);
	bool process_parsed_request(int client_fd, Config& servers);
	void finalize_cgi_job(int client_fd, Config& servers, bool success, int status_code);
	void cleanup_cgi_job(int client_fd, bool kill_child);
	void reap_deferred_children();
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
	std::map<int, bool> _upload_in_progress;
	std::map<int, bool> _upload_waiting;
	std::deque<int> _upload_waiting_order;
	std::vector<pid_t> _children_to_reap;
	size_t _active_upload_count;
};

#endif
