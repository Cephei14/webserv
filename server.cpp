#include "server.hpp"
#include "WebservUtils.hpp"

server::server() : _active_upload_count(0)
{}

bool server::can_read_upload_body(int client_fd, const HTTPRequest& req)
{
	if (req.getMethod() != "POST")
		return true;
	const std::map<std::string, std::string>& hdrs = req.getMap();
	if (hdrs.empty())
		return true;
	std::map<int, bool>::iterator in_prog_it = _upload_in_progress.find(client_fd);
	if (in_prog_it != _upload_in_progress.end() && in_prog_it->second)
		return true;
	if (_upload_waiting.find(client_fd) != _upload_waiting.end())
	{
		set_client_events(client_fd, 0);
		return false;
	}
	if (!_upload_waiting.empty())
	{
		_upload_waiting[client_fd] = true;
		_upload_waiting_order.push_back(client_fd);
		set_client_events(client_fd, 0);
		resume_one_waiting_upload();
		return false;
	}
	if (_active_upload_count < MAX_CONCURRENT_UPLOADS)
	{
		_upload_in_progress[client_fd] = true;
		_upload_waiting.erase(client_fd);
		++_active_upload_count;
		return true;
	}
	_upload_waiting[client_fd] = true;
	_upload_waiting_order.push_back(client_fd);
	set_client_events(client_fd, 0);
	return false;
}

void server::release_upload_slot(int client_fd)
{
	std::map<int, bool>::iterator in_prog_it = _upload_in_progress.find(client_fd);
	if (in_prog_it != _upload_in_progress.end() && in_prog_it->second)
	{
		_upload_in_progress.erase(in_prog_it);
		if (_active_upload_count > 0)
			--_active_upload_count;
	}
	_upload_waiting.erase(client_fd);
	resume_one_waiting_upload();
}

void server::resume_one_waiting_upload()
{
	if (_active_upload_count >= MAX_CONCURRENT_UPLOADS)
		return;
	while (!_upload_waiting_order.empty())
	{
		int fd = _upload_waiting_order.front();
		_upload_waiting_order.pop_front();
		std::map<int, bool>::iterator it = _upload_waiting.find(fd);
		if (it == _upload_waiting.end())
			continue;
		_upload_in_progress[fd] = true;
		_upload_waiting.erase(it);
		++_active_upload_count;
		set_client_events(fd, POLLIN | POLLOUT);
		break;
	}
}

void server::start_listening(Config& servers)
{
	bool has_listener = false;
	for (size_t idx = 0; idx < servers._servers.size(); ++idx)
	{
		ServerConfig& srv = servers._servers[idx];
		if (_port_socket.find(srv._port) != _port_socket.end())
		{
			has_listener = true;
			continue;
		}
		struct addrinfo hints, *res, *p;
		int ra;
		int yes = 1;
		int listen_fd = -1;
		std::stringstream ss;
		ss << srv._port;
		std::string port = ss.str();
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		if ((ra = getaddrinfo(srv._host.c_str(), port.c_str(), &hints, &res)) != 0)
		{
			std::cerr << "srv[ERROR]: getaddrinfo " << gai_strerror(ra) <<std::endl;
			continue;
		}
		for(p = res; p != NULL; p = p->ai_next)
		{
			listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if(listen_fd == -1)
			{
				std::cerr << "srv [socket] " << std::endl;
				continue;
			}
			if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes ,sizeof(int)) == -1)
			{
				std::cerr << "srv [setsockopt]" << std::endl;
				close(listen_fd);
				listen_fd = -1;
				continue;
			}
			if (bind(listen_fd, p->ai_addr, p->ai_addrlen) == -1)
			{
				close(listen_fd);
				listen_fd = -1;
				std::cerr << "srv [Bind]" << std::endl;
				continue;
			}
			break;
		}
		freeaddrinfo(res);

		if (p == NULL || listen_fd == -1)
		{
			std::cerr << "server failed to bind" << std::endl;
			continue;
		}
		if (listen(listen_fd, 256) == -1)
		{
			std::cerr << "Listen" << std::endl;
			close(listen_fd);
			continue;
		}
		if (fcntl(listen_fd, F_SETFL, O_NONBLOCK) == -1)
		{
			close(listen_fd);
			continue;
		}
		_port_socket[srv._port] = listen_fd;
		poll_setup(listen_fd);
		_listener_to_server[listen_fd] = static_cast<int>(idx);
		has_listener = true;
	}
	if (!has_listener)
		throw std::runtime_error("No valid listening sockets");
}

void server::poll_setup(int newfd)
{
	struct pollfd pfd;
	pfd.fd = newfd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	_fds.push_back(pfd);
}
bool server::get_keep_alive(const HTTPRequest& req) const
{
	bool keep_alive = true;
	const std::map<std::string, std::string>& headers = req.getMap();
	std::map<std::string, std::string>::const_iterator conn_it = headers.find("connection");
	if (conn_it != headers.end())
	{
		std::string conn_value = to_lower_copy(conn_it->second);
		if (conn_value == "close")
			keep_alive = false;
	}
	return keep_alive;
}

int server::resolve_server_index(int client_fd, const HTTPRequest& req, const Config& servers) const
{
	int default_idx = 0;
	std::map<int, int>::const_iterator c_it = _client_to_server.find(client_fd);
	if (c_it != _client_to_server.end() && c_it->second >= 0 && static_cast<size_t>(c_it->second) < servers._servers.size())
		default_idx = c_it->second;
	int target_port = -1;
	if (default_idx >= 0 && static_cast<size_t>(default_idx) < servers._servers.size())
		target_port = servers._servers[default_idx]._port;
	if (target_port < 0)
	{
		struct sockaddr_in local_addr;
		socklen_t local_len = sizeof(local_addr);
		if (getsockname(client_fd, reinterpret_cast<struct sockaddr*>(&local_addr), &local_len) == 0)
			target_port = ntohs(local_addr.sin_port);
	}
	if (target_port < 0)
		return 0;
	std::string host_name;
	const std::map<std::string, std::string>& headers = req.getMap();
	std::map<std::string, std::string>::const_iterator h_it = headers.find("host");
	if (h_it != headers.end())
		host_name = normalize_host_header_value(h_it->second);
	int first_on_port = -1;
	for (size_t i = 0; i < servers._servers.size(); ++i)
	{
		const ServerConfig& candidate = servers._servers[i];
		if (candidate._port != target_port)
			continue;
		if (first_on_port == -1)
			first_on_port = static_cast<int>(i);
		for (size_t j = 0; j < candidate._server_names.size(); ++j)
		{
			if (to_lower_copy(candidate._server_names[j]) == host_name)
				return static_cast<int>(i);
		}
	}
	if (default_idx >= 0 && static_cast<size_t>(default_idx) < servers._servers.size() && servers._servers[default_idx]._port == target_port)
		return default_idx;
	if (first_on_port != -1)
		return first_on_port;
	return 0;
}

void server::set_client_events(int client_fd, short events)
{
	for (size_t k = 0; k < _fds.size(); ++k)
	{
		if (_fds[k].fd == client_fd)
		{
			_fds[k].events = events;
			_fds[k].revents = 0;
			return;
		}
	}
}

bool server::is_cgi_request(const HTTPRequest& req, ServerConfig& srv, LocationConfig*& script_loc, LocationConfig*& cgi_loc, std::string& uri_path)
{
	script_loc = NULL;
	cgi_loc = NULL;
	uri_path = req.getUri();
	size_t q = uri_path.find('?');
	if (q != std::string::npos)
		uri_path = uri_path.substr(0, q);

	size_t best_len = 0;
	for (size_t j = 0; j < srv._locations.size(); ++j)
	{
		LocationConfig& loc = srv._locations[j];
		size_t match_len = location_match_prefix_length(uri_path, loc._path);
		if (match_len == 0)
			continue;
		if (match_len >= best_len)
		{
			best_len = match_len;
			script_loc = &loc;
		}
	}
	if (script_loc == NULL)
	{
		for (size_t j = 0; j < srv._locations.size(); ++j)
		{
			if (srv._locations[j]._path == "/")
			{
				script_loc = &srv._locations[j];
				break;
			}
		}
	}
	if (script_loc == NULL)
		return false;
	if (script_loc->_return_code > 0 && !script_loc->_return_url.empty())
		return false;//manage redirect somewhere else
	if (req.getMethod() != "GET" && req.getMethod() != "POST")
		return false;
	size_t dot = uri_path.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = uri_path.substr(dot);
	for (size_t j = 0; j < srv._locations.size(); ++j)
	{
		LocationConfig& loc = srv._locations[j];
		if (loc._cgi_path.empty() || loc._cgi_ext.empty())
			continue;
		for (size_t k = 0; k < loc._cgi_ext.size(); ++k)
		{
			if (loc._cgi_ext[k] == ext)
			{
				cgi_loc = &loc;
				return true;
			}
		}
	}
	return false;
}

bool server::start_cgi_job(int client_fd, HTTPRequest& req, ServerConfig& srv, const LocationConfig& script_loc, const LocationConfig& cgi_loc, const std::string& uri_path, int server_index)
{
	if (_cgi_jobs.find(client_fd) != _cgi_jobs.end())
		return true;

	if (uri_path.find("..") != std::string::npos)
	{
		HTTPResponse err;
		err.build_error_response(403, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}

	std::string root = script_loc._root.empty() ? srv._root : script_loc._root;
	std::string rel = uri_path;
	size_t prefix_len = location_match_prefix_length(rel, script_loc._path);
	if (prefix_len > 0 && script_loc._path != "/")
		rel = rel.substr(prefix_len);
	if (rel.empty() || rel[0] != '/')
		rel = "/" + rel;
	std::string script_path = root + rel;
	if (!script_path.empty() && script_path[0] != '/')
	{
		char cwd[4096];
		if (getcwd(cwd, sizeof(cwd)) != NULL)
		{
			std::string base = cwd;
			if (!base.empty() && base[base.size() - 1] != '/')
				base += "/";
			if (script_path.size() > 2 && script_path.substr(0, 2) == "./")
				script_path = base + script_path.substr(2);
			else
				script_path = base + script_path; //absolute path
		}
	}
	bool is_post = (req.getMethod() == "POST");
	if (!is_post)
	{
		struct stat script_stat;
		if (stat(script_path.c_str(), &script_stat) != 0 || S_ISDIR(script_stat.st_mode))
		{
			HTTPResponse err;
			err.build_error_response(404, srv);
			_pending_response[client_fd] = err.get_raw_response();
			_fd_keep_alive[client_fd] = get_keep_alive(req);
			set_client_events(client_fd, POLLIN | POLLOUT);
			return false;
		}
		if (access(script_path.c_str(), R_OK) != 0)
		{
			HTTPResponse err;
			err.build_error_response(403, srv);
			_pending_response[client_fd] = err.get_raw_response();
			_fd_keep_alive[client_fd] = get_keep_alive(req);
			set_client_events(client_fd, POLLIN | POLLOUT);
			return false;
		}
	}
	bool use_decoded_body = false;
	size_t request_body_size = 0;
	std::string decoded_request_body;
	if (is_post)
	{
		const std::map<std::string, std::string>& headers = req.getMap();
		bool is_chunked = false;
		std::map<std::string, std::string>::const_iterator te_it = headers.find("transfer-encoding");
		if (te_it != headers.end())
		{
			std::string te = to_lower_copy(te_it->second);
			if (te.find("chunked") != std::string::npos)
				is_chunked = true;
		}
		if (is_chunked)
		{
			if (!decode_chunked_body_for_cgi(req.getBody(), decoded_request_body))
			{
				HTTPResponse err;
				err.build_error_response(400, srv);
				_pending_response[client_fd] = err.get_raw_response();
				_fd_keep_alive[client_fd] = get_keep_alive(req);
				set_client_events(client_fd, POLLIN | POLLOUT);
				return false;
			}
			use_decoded_body = true;
			request_body_size = decoded_request_body.size();
		}
		else
			request_body_size = req.getBody().size();
	}
	int out_fd[2];
	int in_fd[2];
	in_fd[0] = -1;
	in_fd[1] = -1;
	out_fd[0] = -1;
	out_fd[1] = -1;
	if (pipe(out_fd) == -1)
	{
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	if (is_post && pipe(in_fd) == -1)//we never create a in_pipe if it is not a post method
	{
		close(out_fd[0]);
		close(out_fd[1]);
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	std::string env_method = "REQUEST_METHOD=" + req.getMethod();
	std::string env_gateway = "GATEWAY_INTERFACE=CGI/1.1";
	std::string env_server_protocol = "SERVER_PROTOCOL=" + req.getVersion();
	std::string env_query = "QUERY_STRING=";
	size_t q = req.getUri().find('?');
	if (q != std::string::npos)
		env_query += req.getUri().substr(q + 1);
	std::string env_request_uri = "REQUEST_URI=" + req.getUri();
	std::string env_script_name = "SCRIPT_NAME=" + uri_path;
	std::string env_path_info = "PATH_INFO=" + uri_path;
	std::string env_script = "SCRIPT_FILENAME=" + script_path;
	std::string env_content_length = "CONTENT_LENGTH=0";
	std::string env_content_type = "CONTENT_TYPE=";
	if (is_post)
	{
		std::stringstream ss;
		ss << request_body_size;
		env_content_length = "CONTENT_LENGTH=" + ss.str();
		const std::map<std::string, std::string>& headers = req.getMap();
		std::map<std::string, std::string>::const_iterator it = headers.find("content-type");
		if (it != headers.end())
			env_content_type = "CONTENT_TYPE=" + it->second;
	}

	std::vector<std::string> extra_http_env;
	{
		const std::map<std::string, std::string>& headers = req.getMap();
		for (std::map<std::string, std::string>::const_iterator it = headers.begin(); it != headers.end(); ++it)
		{
			if (it->first == "content-type" || it->first == "content-length")
				continue;
			extra_http_env.push_back(to_cgi_http_header_env_key(it->first) + "=" + it->second);
		}
	}
	std::vector<char*> envp;
	envp.push_back(const_cast<char*>(env_method.c_str()));
	envp.push_back(const_cast<char*>(env_gateway.c_str()));
	envp.push_back(const_cast<char*>(env_server_protocol.c_str()));
	envp.push_back(const_cast<char*>(env_query.c_str()));
	envp.push_back(const_cast<char*>(env_request_uri.c_str()));
	envp.push_back(const_cast<char*>(env_script_name.c_str()));
	envp.push_back(const_cast<char*>(env_path_info.c_str()));
	envp.push_back(const_cast<char*>(env_script.c_str()));
	envp.push_back(const_cast<char*>(env_content_length.c_str()));
	envp.push_back(const_cast<char*>(env_content_type.c_str()));
	for (size_t i = 0; i < extra_http_env.size(); ++i)
		envp.push_back(const_cast<char*>(extra_http_env[i].c_str()));
	envp.push_back(NULL);
	char* args[3];
	std::string abs_cgi_path = cgi_loc._cgi_path;
	if (!abs_cgi_path.empty() && abs_cgi_path[0] != '/')
	{
		char cwd[4096];
		if (getcwd(cwd, sizeof(cwd)) != NULL)
		{
			std::string base = cwd;
			if (!base.empty() && base[base.size() - 1] != '/')
				base += "/";
			if (abs_cgi_path.size() > 2 && abs_cgi_path.substr(0, 2) == "./")
				abs_cgi_path = base + abs_cgi_path.substr(2);
			else
				abs_cgi_path = base + abs_cgi_path;
		}
	}
	args[0] = const_cast<char*>(abs_cgi_path.c_str());//e.g python
	args[1] = const_cast<char*>(script_path.c_str());//script
	args[2] = NULL;
	pid_t pid = fork();
	if (pid == -1)
	{
		close(out_fd[0]);
		close(out_fd[1]);
		if (is_post)
		{
			close(in_fd[0]);
			close(in_fd[1]);
		}
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	//Child -> POST (read & write <=> in_fd[0] & out_fd[1])
	//Child -> GET (write <=> out_fd[1])
	//Parent -> POST (read & write <=> out_fd[0] & in_fd[1])
	//Parent -> GET (read <=> out_fd[0])
	if (pid == 0)
	{
		if (is_post)
		{
			close(in_fd[1]);//child want to read data from parent, save result to the HD and write data to parent using out_fd[1]
			if (dup2(in_fd[0], STDIN_FILENO) == -1)
				_exit(1);
			close(in_fd[0]);
		}
		else
		{
			int null_fd = open("/dev/null", O_RDONLY);
			if (null_fd == -1)
				_exit(1);
			if (dup2(null_fd, STDIN_FILENO) == -1)
				_exit(1);
			close(null_fd);
		}
		close(out_fd[0]);//child will not read anything to the parent using out_fd
		if (dup2(out_fd[1], STDOUT_FILENO) == -1)
			_exit(1);
		close(out_fd[1]);
		size_t slash = script_path.find_last_of('/');
		std::string script_dir = ".";
		if (slash != std::string::npos)
		{
			script_dir = script_path.substr(0, slash);
			if (script_dir.empty())
				script_dir = "/";
		}
		if (chdir(script_dir.c_str()) == -1)
			_exit(1);
		execve(args[0], args, &envp[0]);
		_exit(1);
	} //child ended
	close(out_fd[1]); //closing child ends
	if (is_post)
		close(in_fd[0]);
	if (fcntl(out_fd[0], F_SETFL, O_NONBLOCK) == -1)
	{
		close(out_fd[0]);//closing parent ends
		if (is_post)
			close(in_fd[1]);
		kill(pid, SIGKILL);
		if (!reap_child_nonblocking(pid))
			_children_to_reap.push_back(pid);
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	if (is_post && fcntl(in_fd[1], F_SETFL, O_NONBLOCK) == -1)
	{
		close(out_fd[0]);
		close(in_fd[1]);
		kill(pid, SIGKILL);
		if (!reap_child_nonblocking(pid))
			_children_to_reap.push_back(pid);
		HTTPResponse err;
		err.build_error_response(500, srv);
		_pending_response[client_fd] = err.get_raw_response();
		_fd_keep_alive[client_fd] = get_keep_alive(req);
		set_client_events(client_fd, POLLIN | POLLOUT);
		return false;
	}
	CgiJob job;
	job.client_fd = client_fd;
	job.server_index = server_index;
	job.pid = pid;
	job.in_fd = is_post ? in_fd[1] : -1;
	job.out_fd = out_fd[0];
	job.write_offset = 0;
	job.request_body_size = request_body_size;
	job.use_decoded_body = use_decoded_body;
	if (job.use_decoded_body)
		job.request_body.swap(decoded_request_body);
	else if (is_post && job.request_body_size > 0)
		req.takeBody(job.request_body);
	job.output.clear();
	job.start_ms = now_ms();
	job.last_activity_ms = job.start_ms;
	job.child_done = false;
	job.child_status = 0;
	if (job.in_fd != -1 && job.request_body_size == 0) //POST without a body
	{
		close(job.in_fd);
		job.in_fd = -1;
	}
	_cgi_jobs[client_fd] = job;
	if (job.in_fd != -1)
	{
		struct pollfd in_pfd;
		in_pfd.fd = job.in_fd;
		in_pfd.events = POLLOUT;
		in_pfd.revents = 0;
		_fds.push_back(in_pfd);
		_cgi_in_to_client[job.in_fd] = client_fd;
	}
	struct pollfd out_pfd;
	out_pfd.fd = job.out_fd;
	out_pfd.events = POLLIN;
	out_pfd.revents = 0;
	_fds.push_back(out_pfd);
	_cgi_out_to_client[job.out_fd] = client_fd;
	set_client_events(client_fd, 0);
	return true;
}

void server::cleanup_cgi_job(int client_fd, bool kill_child)
{
	std::map<int, CgiJob>::iterator it = _cgi_jobs.find(client_fd);
	if (it == _cgi_jobs.end())
		return;

	CgiJob& job = it->second;
	if (kill_child && job.pid > 0 && !job.child_done)
	{
		kill(job.pid, SIGKILL);
		job.child_done = reap_child_nonblocking(job.pid);
		if (!job.child_done)
			_children_to_reap.push_back(job.pid);
	}

	int fds_to_disable[2];
	fds_to_disable[0] = job.in_fd;
	fds_to_disable[1] = job.out_fd;
	for (int n = 0; n < 2; ++n)
	{
		int fd = fds_to_disable[n];
		if (fd == -1)
			continue;
		close(fd);
		_cgi_in_to_client.erase(fd);
		_cgi_out_to_client.erase(fd);
		for (size_t k = 0; k < _fds.size(); ++k)
		{
			if (_fds[k].fd == fd)
			{
				_fds[k].fd = -1;
				_fds[k].events = 0;
				_fds[k].revents = 0;
				break;
			}
		}
	}
	job.in_fd = -1;
	job.out_fd = -1;
}

void server::reap_deferred_children()
{
	for (size_t i = 0; i < _children_to_reap.size();)
	{
		pid_t w = waitpid(_children_to_reap[i], NULL, WNOHANG);
		if (w == _children_to_reap[i] || w == -1)
			_children_to_reap.erase(_children_to_reap.begin() + i);
		else
			++i;
	}
}

void server::finalize_cgi_job(int client_fd, Config& servers, bool success, int status_code)
{
	std::map<int, CgiJob>::iterator it = _cgi_jobs.find(client_fd);
	if (it == _cgi_jobs.end())
		return;
	CgiJob& job = it->second;

	std::map<int, HTTPRequest>::iterator req_it = _requests.find(client_fd);
	if (req_it == _requests.end())
	{
		cleanup_cgi_job(client_fd, !job.child_done);
		_cgi_jobs.erase(client_fd);
		return;
	}

	bool keep_alive = get_keep_alive(req_it->second);
	std::string raw;
	if (success)
	{
		int final_status = 200;
		std::string final_content_type = "text/html";
		std::string final_body;
		parse_cgi_output_block(job.output, final_status, final_content_type, final_body);
		build_raw_http_response(req_it->second, final_status, final_content_type, final_body, keep_alive, raw);
		log_post_upload_success(client_fd, req_it->second, raw);
	}
	else
	{
		HTTPResponse err;
		err.build_error_response(status_code, servers._servers[job.server_index]);
		raw = err.get_raw_response();
	}

	_pending_response[client_fd] = raw;
	_fd_keep_alive[client_fd] = keep_alive;
	set_client_events(client_fd, POLLIN | POLLOUT);

	cleanup_cgi_job(client_fd, !job.child_done);
	_cgi_jobs.erase(client_fd);
	release_upload_slot(client_fd);
}

bool server::process_parsed_request(int client_fd, Config& servers)
{
	std::map<int, HTTPRequest>::iterator req_it = _requests.find(client_fd);
	if (req_it == _requests.end() || !req_it->second.IsParsed())
		return false;
	if (_pending_response.find(client_fd) != _pending_response.end() || _cgi_jobs.find(client_fd) != _cgi_jobs.end())
		return false;
	if (!can_read_upload_body(client_fd, req_it->second))
		return true;

	int s_idx = resolve_server_index(client_fd, req_it->second, servers);
	LocationConfig* script_loc = NULL;
	LocationConfig* cgi_loc = NULL;
	std::string uri_path;
	if (is_cgi_request(req_it->second, servers._servers[s_idx], script_loc, cgi_loc, uri_path))
	{
		size_t body_size = 0;
		if (!get_request_body_size_for_limit(req_it->second, body_size))
		{
			release_upload_slot(client_fd);
			_responses[client_fd].build_error_response(400, servers._servers[s_idx]);
			_pending_response[client_fd] = _responses[client_fd].get_raw_response();
			set_client_events(client_fd, POLLIN | POLLOUT);
			return true;
		}
		if (cgi_loc != NULL && cgi_loc->_client_max_body_size > 0 && body_size > cgi_loc->_client_max_body_size)
		{
			release_upload_slot(client_fd);
			_responses[client_fd].build_error_response(413, servers._servers[s_idx]);
			_pending_response[client_fd] = _responses[client_fd].get_raw_response();
			set_client_events(client_fd, POLLIN | POLLOUT);
			return true;
		}
		if (!start_cgi_job(client_fd, req_it->second, servers._servers[s_idx], *script_loc, *cgi_loc, uri_path, s_idx))
			release_upload_slot(client_fd);
		req_it->second.clearBody();
		return true;
	}

	release_upload_slot(client_fd);
	HTTPResponse new_response;
	new_response.build(req_it->second, servers._servers[s_idx]);
	_pending_response[client_fd] = new_response.get_raw_response();
	log_post_upload_success(client_fd, req_it->second, _pending_response[client_fd]);
	_fd_keep_alive[client_fd] = get_keep_alive(req_it->second);
	set_client_events(client_fd, POLLIN | POLLOUT);
	return true;
}

void server::process_cgi_pipe_event(size_t& i, Config& servers)
{
	if (i >= _fds.size())
		return;
	int fd = _fds[i].fd;
	bool is_in_pipe = (_cgi_in_to_client.find(fd) != _cgi_in_to_client.end());
	bool is_out_pipe = (_cgi_out_to_client.find(fd) != _cgi_out_to_client.end());
	if (!is_in_pipe && !is_out_pipe)
		return;
	int client_fd = -1;
	if (is_in_pipe)
		client_fd = _cgi_in_to_client[fd];
	else
		client_fd = _cgi_out_to_client[fd];
	std::map<int, CgiJob>::iterator it = _cgi_jobs.find(client_fd);
	if (it == _cgi_jobs.end())
	{
		close(fd);
		_cgi_in_to_client.erase(fd);
		_cgi_out_to_client.erase(fd);
		_fds[i].fd = -1;
		_fds[i].events = 0;
		_fds[i].revents = 0;
		return;
	}
	CgiJob& job = it->second;
	short revents = _fds[i].revents;
	if (is_in_pipe)
	{
		if (revents & (POLLERR | POLLHUP | POLLNVAL))
		{
			finalize_cgi_job(client_fd, servers, false, 500);
			return;
		}
		if (revents & POLLOUT)
		{
			const char* write_buf = job.request_body.c_str();
			size_t write_size = job.request_body_size;

			if (job.write_offset < write_size)
			{
				ssize_t w = write(fd, write_buf + job.write_offset, write_size - job.write_offset);
				if (w <= 0)
					return;
				job.write_offset += static_cast<size_t>(w);
				job.last_activity_ms = now_ms(); 
			}
			if (job.write_offset >= write_size)
			{
				close(fd);
				_cgi_in_to_client.erase(fd);
				job.in_fd = -1;
				_fds[i].fd = -1;
				_fds[i].events = 0;
				_fds[i].revents = 0;
			}
		}
	}
	else
	{
		if (revents & POLLIN)
		{
			static char buffer[BUFF_SIZE];
			ssize_t bytes_read = read(fd, buffer, sizeof(buffer));
			if (bytes_read > 0)
			{
				job.output.append(buffer, static_cast<size_t>(bytes_read));
				job.last_activity_ms = now_ms();
			}
			else if (bytes_read == 0)
			{
				close(fd);
				_cgi_out_to_client.erase(fd);
				job.out_fd = -1;
				_fds[i].fd = -1;
				_fds[i].events = 0;
				_fds[i].revents = 0;
			}
			else
			{
				finalize_cgi_job(client_fd, servers, false, 500);
				return;
			}
		}
		if (revents & (POLLERR | POLLNVAL))
		{
			finalize_cgi_job(client_fd, servers, false, 500);
			return;
		}
		if ((revents & POLLHUP) && !(revents & POLLIN))
		{
			close(fd);
			_cgi_out_to_client.erase(fd);
			job.out_fd = -1;
			_fds[i].fd = -1;
			_fds[i].events = 0;
			_fds[i].revents = 0;
		}
	}

	if (!job.child_done)
	{
		pid_t w = waitpid(job.pid, &job.child_status, WNOHANG);
		if (w == -1)
		{
			finalize_cgi_job(client_fd, servers, false, 500);
			return;
		}
		if (w == job.pid)
			job.child_done = true;
	}

	if (now_ms() - job.last_activity_ms > 30000) 
	{
		finalize_cgi_job(client_fd, servers, false, 500);
		return;
	}

	if (job.child_done && job.in_fd == -1 && job.out_fd == -1)
	{
		bool success = (WIFEXITED(job.child_status) && WEXITSTATUS(job.child_status) == 0);
		finalize_cgi_job(client_fd, servers, success, 500);
	}
}

void server::close_connection(size_t& i)
{
	if (i >= _fds.size())
		return;

	int fd = _fds[i].fd;
	if (_listener_to_server.find(fd) != _listener_to_server.end())
		return;

	if (_cgi_jobs.find(fd) != _cgi_jobs.end())
	{
		cleanup_cgi_job(fd, true);
		_cgi_jobs.erase(fd);
	}
	release_upload_slot(fd);

	close(fd);
	_requests.erase(fd);
	_client_to_server.erase(fd);
	_responses.erase(fd);
	_fds.erase(_fds.begin() + i);
	_pending_response.erase(fd);
	_fd_keep_alive.erase(fd);
	i--;
}
void server::srv_manage(Config& servers)
{
	while(!signaled)
	{
		if (_fds.empty())
			break;
		if (poll(&_fds[0], _fds.size(), TOUT) == -1)
			continue;
		reap_deferred_children();
		std::vector<int> timed_out_clients;
		long now = now_ms();
		for (std::map<int, CgiJob>::iterator jt = _cgi_jobs.begin(); jt != _cgi_jobs.end(); ++jt)
		{
			if (now - jt->second.last_activity_ms > 30000)
				timed_out_clients.push_back(jt->first);
		}
		for (size_t t = 0; t < timed_out_clients.size(); ++t)
			finalize_cgi_job(timed_out_clients[t], servers, false, 500);

		std::vector<int> orphaned;
		for (std::map<int, CgiJob>::iterator jt = _cgi_jobs.begin(); jt != _cgi_jobs.end(); ++jt)
		{
			CgiJob& job = jt->second;
			if (job.in_fd == -1 && job.out_fd == -1 && !job.child_done)
			{
				pid_t w = waitpid(job.pid, &job.child_status, WNOHANG);
				if (w == job.pid)
				{
					job.child_done = true;
					orphaned.push_back(jt->first);
				}
			}
		}
		for (size_t t = 0; t < orphaned.size(); ++t)
		{
			int cfd = orphaned[t];
			bool ok = (WIFEXITED(_cgi_jobs[cfd].child_status) && WEXITSTATUS(_cgi_jobs[cfd].child_status) == 0);
			finalize_cgi_job(cfd, servers, ok, 500);
		}

		size_t i = 0;
		for(i = 0; i < _fds.size(); i++)
		{
			if (_fds[i].fd < 0) //connection closed
				continue;
			if (_cgi_in_to_client.find(_fds[i].fd) != _cgi_in_to_client.end() || _cgi_out_to_client.find(_fds[i].fd) != _cgi_out_to_client.end()) //pipe
			{
				if (_fds[i].revents != 0)
					process_cgi_pipe_event(i, servers);
				continue;
			}
			if (_fds[i].revents & POLLIN) //client
			{
				struct sockaddr_storage the_addr;
				socklen_t len = sizeof the_addr;
				char s[INET6_ADDRSTRLEN];
				int client_fd = _fds[i].fd;
				std::map<int, int>::iterator listener_it = _listener_to_server.find(client_fd); //socket-server map
				bool is_listener = (listener_it != _listener_to_server.end());

				if (is_listener) //new client
				{
					int cnxfd = accept(client_fd, reinterpret_cast<sockaddr*>(&the_addr), &len);
					if (cnxfd == -1)
					{
						std::cerr << "Failed to connect " << std::endl;
						continue;
					}
					if (fcntl(cnxfd, F_SETFL, O_NONBLOCK) == -1)
					{
						close(cnxfd);
						continue;
					}
					poll_setup(cnxfd);
					_requests[cnxfd] = HTTPRequest();
					_client_to_server[cnxfd] = listener_it->second;
					inet_ntop(the_addr.ss_family, get_addr_type(reinterpret_cast<sockaddr*>(&the_addr)), s, sizeof s);
					std::cout << "srv: new connection from " << s << " on fd " << cnxfd << " (Total clients: " << _fds.size() - 1 << ")" << std::endl;
				}
				if (!is_listener) //exiting client
				{
					if (_cgi_jobs.find(client_fd) != _cgi_jobs.end())
						continue;
					static char buf[BUFF_SIZE];
					int nbytes = recv(client_fd, buf, BUFF_SIZE - 1, 0);
					if (nbytes < 0)
					{
						std::cerr << "srv: recv error (hard failure) on fd " <<  client_fd << std::endl;
						close_connection(i);
						std::cout << "srv: client disconnected. (Total clients: " << _fds.size() - 1 << ")" << std::endl;
						continue;
					}
					if (nbytes == 0)
					{
						std::cout << "srv: socket " <<  client_fd << " hung up." << std::endl;
						close_connection(i);
						std::cout << "srv: client disconnected. (Total clients: " << _fds.size() - 1 << ")" << std::endl;
						continue;
					}
					else
					{
						HTTPRequest& current_req = _requests[client_fd];
						try
						{
							current_req.AddRawP(buf, nbytes);
							if (!can_read_upload_body(client_fd, current_req))
								continue;
							if (current_req.getMethod() == "POST" && !current_req.IsParsed())
							{
								const std::map<std::string, std::string>& hdrs = current_req.getMap();
								std::map<std::string, std::string>::const_iterator cl_it = hdrs.find("content-length");
								if (cl_it != hdrs.end())
								{
									unsigned long declared_len = std::strtoul(cl_it->second.c_str(), NULL, 10);
									int s_idx = resolve_server_index(client_fd, current_req, servers);
									ServerConfig& selected_srv = servers._servers[s_idx];
									size_t limit = selected_srv._client_max_body_size;
									LocationConfig* matched_loc = find_best_location_for_path(selected_srv, current_req.getUri());
									LocationConfig* script_loc = NULL;
									LocationConfig* cgi_loc = NULL;
									std::string cgi_uri_path;
									bool is_cgi = is_cgi_request(current_req, selected_srv, script_loc, cgi_loc, cgi_uri_path);
									if (!is_cgi)
									{
										if (matched_loc != NULL && matched_loc->_client_max_body_size > 0)
											limit = matched_loc->_client_max_body_size;
										if (declared_len > limit)
										{
											release_upload_slot(client_fd);
											HTTPResponse too_large_res;
											too_large_res.build_error_response(413, selected_srv);
											_pending_response[client_fd] = too_large_res.get_raw_response();
											_fd_keep_alive[client_fd] = false;
											_fds[i].events = POLLOUT;
											continue;
										}
									}
								}
							}
						}
						catch(const std::exception& e)
						{
							release_upload_slot(client_fd);
						    int status_code = std::atoi(e.what()); 
						
						    if (status_code < 400 || status_code > 599)
						        status_code = 500;
						    std::cerr << "Request Failed: " << e.what() << std::endl;
						
						    HTTPResponse error_res;
							int s_idx = resolve_server_index(client_fd, current_req, servers);
						    error_res.build_error_response(status_code, servers._servers[s_idx]);
							_pending_response[client_fd] = error_res.get_raw_response();
							_fd_keep_alive[client_fd] = false;
							_fds[i].events = POLLOUT;
						}
							if (process_parsed_request(client_fd, servers))
								continue;
					}
				}
			}
			else if(_fds[i].revents & POLLOUT)
			{
				int client_fd = _fds[i].fd;
				std::map<int, std::string>::iterator p_it = _pending_response.find(client_fd);
				std::map<int, bool>::iterator k_it = _fd_keep_alive.find(client_fd);
					bool keep_alive = (k_it != _fd_keep_alive.end() && k_it->second);
					if (p_it == _pending_response.end())
					{
						if (!process_parsed_request(client_fd, servers))
							_fds[i].events = POLLIN;
						continue;
					}
				if (p_it->second.empty())
				{
					if (keep_alive)
					{
							_pending_response.erase(client_fd);
							_fd_keep_alive.erase(client_fd);
							_requests[client_fd].consume_parsed_request();
							if (!process_parsed_request(client_fd, servers))
								_fds[i].events = POLLIN;
						}
					else
					{
						close_connection(i);
						std::cout << "srv: response sent and connection closed." << std::endl;
					}
					continue;
				}
				int bytes_sent = send(client_fd, p_it->second.c_str(), p_it->second.size(), 0);
				if (bytes_sent > 0)
					p_it->second.erase(0, static_cast<size_t>(bytes_sent));
				else if (bytes_sent < 0)
				{
					close_connection(i);
					continue;
				}
				if (p_it->second.empty())
				{
					if (keep_alive)
					{
							_pending_response.erase(client_fd);
							_fd_keep_alive.erase(client_fd);
							_requests[client_fd].consume_parsed_request();
							if (!process_parsed_request(client_fd, servers))
								_fds[i].events = POLLIN;
						}
					else
					{
						close_connection(i);
						std::cout << "srv: response sent and connection closed." << std::endl;
					}
				}
			}
		}
		for (size_t k = 0; k < _fds.size();)
		{
			if (_fds[k].fd < 0)
				_fds.erase(_fds.begin() + k);
			else
				++k;
		}
	}
	for (std::map<int, CgiJob>::iterator it = _cgi_jobs.begin(); it != _cgi_jobs.end(); ++it)
	{
		kill(it->second.pid, SIGKILL);
		reap_child_nonblocking(it->second.pid);
	}
	for(size_t i = 0; i < _fds.size(); i++)
	{
		int fd = _fds[i].fd;
		if (fd >= 0)
			close(fd);
	}
	_fds.clear();
	_requests.clear();
	_responses.clear();
	_client_to_server.clear();
	_listener_to_server.clear();
	_port_socket.clear();
	_pending_response.clear();
	_fd_keep_alive.clear();
		_cgi_jobs.clear();
	_cgi_in_to_client.clear();
	_cgi_out_to_client.clear();
	_children_to_reap.clear();
	_upload_in_progress.clear();
		_upload_waiting.clear();
		_upload_waiting_order.clear();
		_active_upload_count = 0;
		return;
	}
