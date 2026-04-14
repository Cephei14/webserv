#include "ml5.hpp"
static volatile sig_atomic_t signaled = 0;

HTTPRequest::HTTPRequest() : _isParsed(false), _headersParsed(false)
{}

void *get_addr_type(struct sockaddr *the_addr)
{
	if (the_addr->sa_family == AF_INET)
	{
		return &((reinterpret_cast<struct sockaddr_in*>(the_addr))->sin_addr);
	}
	return &((reinterpret_cast<struct sockaddr_in6*>(the_addr))->sin6_addr);
}

void server::start_listening(Config& servers)
{
	bool has_listener = false;
	for (size_t idx = 0; idx < servers._servers.size(); ++idx)
	{
		ServerConfig& srv = servers._servers[idx];
		struct addrinfo hints, *res, *p;
		int ra;
		int yes = 1;
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
			_port_socket[srv._port] = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
			if(_port_socket[srv._port]== -1)
			{
				std::cerr << "srv [socket] " << std::endl;
				continue;
			}
			if (setsockopt(_port_socket[srv._port], SOL_SOCKET, SO_REUSEADDR, &yes ,sizeof(int)) == -1)
			{
				std::cerr << "srv [setsockopt]" << std::endl;
				close(_port_socket[srv._port]);
				continue;
			}
			if (bind(_port_socket[srv._port], p->ai_addr, p->ai_addrlen) == -1)
			{
				close(_port_socket[srv._port]);
				std::cerr << "srv [Bind]" << std::endl;
				continue;
			}
			break;
		}
		freeaddrinfo(res);

		if (p == NULL)
		{
			std::cerr << "server failed to bind" << std::endl;
			continue;
		}
		if (listen(_port_socket[srv._port], 10) == -1)
		{
			std::cerr << "Listen" << std::endl;
			close(_port_socket[srv._port]);
			continue;
		}
		if (fcntl(_port_socket[srv._port], F_SETFL, O_NONBLOCK) == -1)
		{
			close(_port_socket[srv._port]);
			continue;
		}
		poll_setup(_port_socket[srv._port]);
		_listener_to_server[_port_socket[srv._port]] = static_cast<int>(idx);
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

void HTTPRequest::Validate(const std::string& buf)
{
	size_t end_of_headers = buf.find("\r\n\r\n");
	if (!_headersParsed)
	{
		size_t n = buf.find("\r\n");
		if(n == std::string::npos)
			throw std::out_of_range("400 Bad Request");
		std::string requestLine = buf.substr(0, n);
		size_t pos = requestLine.find(' ');
		if(pos == std::string::npos)
			throw std::out_of_range("400 Bad Request");
		_method = requestLine.substr(0, pos);
		if (_method != "POST" && _method != "GET" && _method != "DELETE")
			throw std::runtime_error("501 Not Implemented");
		size_t m = requestLine.find(' ', pos + 1);
		if(m == std::string::npos)
			throw std::runtime_error("400 Bad Request");
		_uri = requestLine.substr(pos + 1, m - pos - 1);
		if (!_uri.empty() && _uri[0] != '/')
			throw std::runtime_error("400 Bad Request");
		_version = requestLine.substr(m + 1, n - m - 1);
		if (_version != "HTTP/1.1")
			throw std::runtime_error("505 HTTP Version Not Supported");

		std::string allowed_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";	
		if (end_of_headers == std::string::npos)
			return;
		std::string Header = buf.substr(n + 2, end_of_headers - (n + 2) + 2); 
		while (Header != "\r\n" && !Header.empty()) 
		{
			size_t line_end = Header.find("\r\n");
			std::string Line = Header.substr(0, line_end);
			size_t colon = Line.find(':');
			if (colon == std::string::npos)
				throw std::runtime_error("400 Bad Request");
			std::string Key = Line.substr(0, colon);
			if (Key.find_first_not_of(allowed_chars) != std::string::npos)
				throw std::runtime_error("400 Bad Request");
			for (size_t i = 0; i < Key.length(); i++)
				Key[i] = (char)tolower(Key[i]);
			std::string Value = Line.substr(colon + 1);
			size_t start = Value.find_first_not_of(" \t");
			if (start != std::string::npos)
				Value = Value.substr(start);
			size_t end = Value.find_last_not_of(" \t");
			if(end != std::string::npos)
				Value = Value.substr(0, end + 1);
			_headers[Key] = Value;
			Header.erase(0, line_end + 2);
		}
		if (_headers.find("host") == _headers.end())
			throw std::runtime_error("400 Bad Request");
		_headersParsed = true;
	}
	if (_headers.find("content-length") != _headers.end())
	{
	    long content_length = std::atol(_headers["content-length"].c_str());
	    size_t body_received = _raw_buf.size() - (end_of_headers + 4);
	    if (body_received >= static_cast<size_t>(content_length))
	        _isParsed = true;
	}
	else if (_method == "POST" && _headers.find("transfer-encoding") == _headers.end())
	    throw std::runtime_error("411 Length Required");
	else if (_method == "POST" && _headers.find("transfer-encoding") != _headers.end())
	{
		if (_raw_buf.find("0\r\n\r\n") != std::string::npos)
			_isParsed = true;
	}
	else _isParsed = true;
}

void HTTPRequest::AddRawP(const char* line, int nbytes)
{
	_raw_buf.append(line, nbytes);
	if ((_raw_buf.find("\r\n\r\n") != std::string::npos) && !_isParsed)
		Validate(_raw_buf);
}

bool HTTPRequest::IsParsed()
{
	return(_isParsed);
}

void HTTPResponse::prepare_GET(const HTTPRequest& req, ServerConfig& srv)
{
	std::string& root = srv._root;
	std::string full_path = root + req.getUri();
	if (req.getUri() == "/" && !srv._index.empty())
		full_path += srv._index[0];
	_contentType = get_content_type(full_path);
	if (req.getUri().find("..") != std::string::npos)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Invalid path</h1>";
		_contentType = "text/html";
		construct_response(req);
		return;
	}
	body_GET(full_path);

	if (_statusCode == 404)
	{
		_body = "<html><body><h1>404 Not Found</h1></body></html>";
		_contentType = "text/html";
	}
	construct_response(req);
}

std::string HTTPRequest::getBody() const
{
	size_t n = _raw_buf.find("\r\n\r\n");
	if (n == std::string::npos)
		return ("");
	return _raw_buf.substr(n + 4);
}

const std::string& HTTPRequest::getUri() const
{
	return(_uri);
}

const std::string& HTTPRequest::getMethod() const
{
	return(_method);
}

const std::string& HTTPRequest::getVersion() const
{
	return(_version);
}


const std::map<std::string, std::string>& HTTPRequest::getMap() const
{
	return(_headers);
}

HTTPResponse::HTTPResponse() 
	: _version("HTTP/1.1"), 
	  _data_size(0),
	  _statusCode(200), 
	  _reason(""), 
	  _body(""), 
	  _contentType("text/plain"),
	  _post_body("") ,
	  final_response("") 
{}

std::string HTTPResponse::get_raw_response() const
{
	return final_response;
}

std::string HTTPResponse::get_content_type(const std::string& uri)
{
	size_t dot_pos = uri.find_last_of('.');

	if (dot_pos == std::string::npos)
        return "application/octet-stream";
	
	std::string ext = uri.substr(dot_pos);

    if (ext == ".html" || ext == ".htm")
		return "text/html";
    if (ext == ".css")
		return "text/css";
    if (ext == ".js")
		return "application/javascript";
    if (ext == ".png")
		return "image/png";
    if (ext == ".jpg" || ext == ".jpeg")
		return "image/jpeg";
    if (ext == ".txt")
		return "text/plain";
    return "application/octet-stream";
}

void HTTPResponse::construct_response(const HTTPRequest& req)
{
	std::stringstream response_stream;
	const std::map<std::string, std::string>& header_req = req.getMap();

	if (_reason.empty()) 
	{
		if (_statusCode == 200)
			_reason = "OK";
		else if (_statusCode == 201)
			_reason = "Created";
		else if (_statusCode == 204)
			_reason = "No Content";
		else if (_statusCode == 404)
			_reason = "Not Found";
		else if (_statusCode == 400)
			_reason = "Bad Request";
		else if (_statusCode == 403)
			_reason = "Forbidden";
		else if (_statusCode == 405)
			_reason = "Method Not Allowed";
		else if (_statusCode == 411)
			_reason = "Length Required";
		else if (_statusCode == 413)
			_reason = "Payload Too Large";
		else if (_statusCode == 501)
			_reason = "Not Implemented";
		else if (_statusCode == 505)
			_reason = "HTTP Version Not Supported";
		else
			_reason = "Error";
	}
	response_stream << req.getVersion() << " " << _statusCode << " " << _reason << "\r\n";
	
	if (_contentType.empty() || _contentType == "text/plain")
		_contentType = "text/html"; // Default for error pages or fallbacks

	response_stream << "Content-Type: " << _contentType << "\r\n";
    response_stream << "Content-Length: " << _body.length() << "\r\n";

	std::map<std::string, std::string>::const_iterator it = header_req.find("connection");

	if (it != header_req.end() && it->second == "close")
	    response_stream << "Connection: close\r\n";
	else
		response_stream << "Connection: keep-alive\r\n";
	response_stream << "\r\n";
	response_stream << _body;
	final_response = response_stream.str();
}

void HTTPResponse::body_GET(const std::string& path)
{
	struct stat st;
	if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
	{
		_statusCode = 404;
		return;
	}

	std::ifstream file(path.c_str(), std::ios::binary);
	if (file.is_open())
	{
		_statusCode = 200;
		_body.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		file.close();
	}
	else
		_statusCode = 404;
}

void HTTPResponse::handle_chunks(const HTTPRequest& req)
{
    std::string raw_body = req.getBody();
    std::string decoded_body = "";
    size_t pos = 0;

    while (pos < raw_body.length())
    {
        size_t hex_end = raw_body.find("\r\n", pos);
        if (hex_end == std::string::npos) 
            break;
        std::string hex_str = raw_body.substr(pos, hex_end - pos);
        long chunk_size = std::strtol(hex_str.c_str(), NULL, 16);
        if (chunk_size == 0) 
            break;
        pos = hex_end + 2;
        decoded_body.append(raw_body, pos, chunk_size);
        pos += chunk_size + 2; 
    }
	_data_size = decoded_body.size();
    _post_body = decoded_body; 
}


void HTTPResponse::body_POST(const std::string& path)
{
	std::ofstream outfile(path.c_str(), std::ios::out | std::ios::binary);
	if(outfile.is_open())
	{
		outfile.write(_post_body.c_str(), _data_size);
		outfile.close();
		_statusCode = 201;
		_body = "<h1>201 Created: File Uploaded Successfully</h1>";
		_contentType = "text/html";
	}
	else
	{
		_statusCode = 403; 
		_body = "<h1>403 Forbidden: Cannot write file</h1>";
		_contentType = "text/html";
	}	
}

void HTTPResponse::prepare_POST(const HTTPRequest& req, ServerConfig& srv)
{
	std::string& root = srv._root;
	std::string full_path = root + req.getUri();
	if (req.getUri().find("..") != std::string::npos)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Invalid path</h1>";
		_contentType = "text/html";
		construct_response(req);
		return;
	}
	const std::map<std::string, std::string> &header_req = req.getMap();
	std::map<std::string, std::string>::const_iterator it = header_req.find("content-length");
	if (it != header_req.end()) 
	{
		_data_size = std::atoi(it->second.c_str());
		_post_body = req.getBody().substr(0, _data_size);
	}
	else
		handle_chunks(req);
	body_POST(full_path);
	construct_response(req);
}

void HTTPResponse::prepare_DELETE(const HTTPRequest& req, ServerConfig& srv)
{
	std::string& root = srv._root;
	std::string full_path = root + req.getUri();

	if (req.getUri().find("..") != std::string::npos)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Invalid path</h1>";
		_contentType = "text/html";
		construct_response(req);
		return;
	}
	if (req.getUri().find("/uploads/") == std::string::npos)
    {
        _statusCode = 403;
        _body = "<h1>403 Forbidden: Deletion is restricted to the /uploads/ directory</h1>";
        _contentType = "text/html";
        construct_response(req);
        return;
    }

	if (std::remove(full_path.c_str()) == 0)
	{
		_statusCode = 204;
		_body = "";
		_contentType = "";
	}
	else if (errno == EACCES || errno == EPERM)
	{
		_statusCode = 403;
		_body = "<h1>403 Forbidden: Permission denied</h1>";
		_contentType = "text/html";
	}
	else
	{
		_statusCode = 404;
		_body = "<h1>404 Not Found: File does not exist</h1>";
		_contentType = "text/html";
	}
	construct_response(req);
}

void HTTPResponse::build(const HTTPRequest& req, ServerConfig& srv)
{
	LocationConfig* best_loc = NULL;
	size_t best_len = 0;

	for(size_t j = 0; j < srv._locations.size(); j++)
	{
		LocationConfig& loc = srv._locations[j];
		if (loc._path.empty())
			continue;
		if (req.getUri().compare(0, loc._path.size(), loc._path) != 0)
			continue;
		if (req.getUri().size() > loc._path.size() && loc._path[loc._path.size() - 1] != '/' && req.getUri()[loc._path.size()] != '/')
			continue;
		if (loc._path.size() >= best_len)
		{
			best_len = loc._path.size();
			best_loc = &loc;
		}
	}

	if (best_loc == NULL)
	{
		for (size_t j = 0; j < srv._locations.size(); ++j)
		{
			if (srv._locations[j]._path == "/")
			{
				best_loc = &srv._locations[j];
				break;
			}
		}
	}

	if (best_loc == NULL)
	{
		_statusCode = 404;
		build_error_response(_statusCode, srv);
		return;
	}

	bool method_found = false;
	for(size_t k = 0; k < best_loc->_allowed_methods.size(); k++)
	{
		if (req.getMethod() == best_loc->_allowed_methods[k])
		{
			method_found = true;
			break;
		}
	}
	if(!method_found)
	{
		_statusCode = 405;
		build_error_response(_statusCode, srv);
		return;
	}

	if(req.getMethod() == "GET")
		prepare_GET(req, srv);
	else if(req.getMethod() == "POST")
	{
		if(req.getBody().size() <= srv._client_max_body_size)
			prepare_POST(req, srv);
		else
		{
			_statusCode = 413;
			build_error_response(_statusCode, srv);
		}
	}
	else if(req.getMethod() == "DELETE")
		prepare_DELETE(req, srv);
	else
	{
		_statusCode = 405;
		build_error_response(_statusCode, srv);
	}
}

void server::close_connection(size_t& i)
{
	if (i >= _fds.size())
		return;

	int fd = _fds[i].fd;
	if (_listener_to_server.find(fd) != _listener_to_server.end())
		return;

	close(fd);
	_requests.erase(fd);
	_client_to_server.erase(fd);
	_responses.erase(fd);
	_fds.erase(_fds.begin() + i);
	_pending_response.erase(fd);
	i--;
}

void HTTPResponse::build_error_response(int status_code, ServerConfig& srv)
{
	std::stringstream response;
    std::string reason_phrase;
    std::string body_str;
    bool custom_page_loaded = false;

	switch(status_code)
	{
		case 400: reason_phrase = "Bad Request"; break;
        case 403: reason_phrase = "Forbidden"; break;
        case 404: reason_phrase = "Not Found"; break;
        case 405: reason_phrase = "Method Not Allowed"; break;
        case 411: reason_phrase = "Length Required"; break;
        case 413: reason_phrase = "Payload Too Large"; break;
        case 500: reason_phrase = "Internal Server Error"; break;
        case 501: reason_phrase = "Not Implemented"; break;
        case 505: reason_phrase = "HTTP Version Not Supported"; break;
		default: reason_phrase = "Error"; break;
	}
	std::map<int, std::string>::iterator it = srv._error_pages.find(status_code);
	std::string filepath;
	if (it != srv._error_pages.end())
	{
		bool r_slash = (!srv._root.empty() && srv._root[srv._root.size() - 1] == '/');
		bool p_slash = (!it->second.empty() && it->second[0] == '/');
		if (r_slash && p_slash)
			filepath = srv._root + it->second.substr(1);
		else if (!r_slash && !p_slash)
			filepath = srv._root + "/" +it->second.substr(1);
		else	
			filepath = srv._root + it->second.substr(1);
        
        std::ifstream file(filepath.c_str());
        if (file.is_open()) 
        {
            std::stringstream buffer;
            buffer << file.rdbuf();
            body_str = buffer.str();
            custom_page_loaded = true;
            file.close();
        }
    }
	if (!custom_page_loaded)
	{
		std::stringstream ss;
		ss << "<html>\r\n";
        ss << "<head><title>" << status_code << " " << reason_phrase << "</title></head>\r\n";
        ss << "<body>\r\n";
        ss << "<center><h1>" << status_code << " " << reason_phrase << "</h1></center>\r\n";
        ss << "<hr><center>webserv/1.0</center>\r\n";
        ss << "</body>\r\n";
        ss << "</html>\r\n";
        
        body_str = ss.str();
	}
	response << "HTTP/1.1 " << status_code << " " << reason_phrase << "\r\n";
    response << "Content-Type: text/html\r\n";
    response << "Content-Length: " << body_str.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << body_str;
    this->final_response = response.str();
}

void server::srv_manage(Config& servers)
{
	while(!signaled)
	{
		if (_fds.empty())
			continue;

		if (poll(&_fds[0], _fds.size(), TOUT) == -1)
		{
			if (errno == EINTR)
				continue;
			std::cerr << "Poll" << std::endl;
			continue;
		}

		size_t i = 0;
		for(i = 0; i < _fds.size(); i++)
		{
			if (_fds[i].revents & POLLIN)
			{
				struct sockaddr_storage the_addr;
				socklen_t len = sizeof the_addr;
				char s[INET6_ADDRSTRLEN];
				int client_fd = _fds[i].fd;
				std::map<int, int>::iterator listener_it = _listener_to_server.find(client_fd);
				bool is_listener = (listener_it != _listener_to_server.end());

				if (is_listener)
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
				if (!is_listener)
				{
					char buf[BUFF_SIZE];
					int nbytes = recv(client_fd, buf, BUFF_SIZE - 1, 0);

					if (nbytes <= 0)
					{
						if(nbytes == 0)
							std::cout << "srv: socket " <<  client_fd << " hung up." << std::endl;
						else
							std::cerr << "srv: recv error on fd " <<  client_fd << std::endl;
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
						}
						catch(const std::exception& e)
						{
						    int status_code = std::atoi(e.what()); 
						
						    if (status_code < 400 || status_code > 599)
						        status_code = 500;
						    std::cerr << "Request Failed: " << e.what() << std::endl;
						
						    HTTPResponse error_res;
							int s_idx = 0;
							std::map<int, int>::iterator s_it = _client_to_server.find(client_fd);
							if (s_it != _client_to_server.end())
								s_idx = s_it->second;
							if (s_idx < 0 || static_cast<size_t>(s_idx) >= servers._servers.size())
								s_idx = 0;
						    error_res.build_error_response(status_code, servers._servers[s_idx]);
						    std::string raw = error_res.get_raw_response();
							_pending_response[client_fd] = raw;
							_fds[i].events = POLLIN | POLLOUT;
						}
						if (current_req.IsParsed())
						{
							int s_idx = 0;
							std::map<int, int>::iterator s_it = _client_to_server.find(client_fd);
							if (s_it != _client_to_server.end())
								s_idx = s_it->second;
							if (s_idx < 0 || static_cast<size_t>(s_idx) >= servers._servers.size())
								s_idx = 0;
							HTTPResponse new_response;
							new_response.build(current_req, servers._servers[s_idx]);
							std::string raw = new_response.get_raw_response();
							_pending_response[client_fd] = raw;
							_fds[i].events = POLLIN | POLLOUT;
						}
					}
				}
			}
			else if(_fds[i].revents & POLLOUT)
			{
				int client_fd = _fds[i].fd;
				std::map<int, std::string>::iterator p_it = _pending_response.find(client_fd);
				if (p_it == _pending_response.end())
				{
					_fds[i].events = POLLIN;
					continue;
				}
				if (p_it->second.empty())
				{
					close_connection(i);
					continue;
				}
				int bytes_sent = send(client_fd, p_it->second.c_str(), p_it->second.size(), 0);
				if (bytes_sent > 0)
					p_it->second.erase(0, static_cast<size_t>(bytes_sent));
				else if (bytes_sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
					continue;
				else
				{
					close_connection(i);
					continue;
				}
				if (p_it->second.empty())
				{
					close_connection(i);
					std::cout << "srv: response sent and connection closed." << std::endl;
				}
			}
		}
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
	return;
}

void AppManager::signal_handler(int s)
{
	(void)s;
	signaled = 1;
	int saved_errno = errno;
	errno = saved_errno;
}

void AppManager::run(Config& servers)
{
	struct sigaction sa;
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags=0;
	if (sigaction(SIGTERM, &sa, NULL) == -1)
		throw std::runtime_error("SIGACTION FAILED");
	if (sigaction(SIGINT, &sa, NULL) == -1)
		throw std::runtime_error("SIGACTION FAILED");
	server serv;
	serv.start_listening(servers);
	serv.srv_manage(servers);
}

std::vector<std::string> tokenize(std::string& config_data)
{
	std::vector<std::string> tokens;
	std::string symbols = "{};";

	for (size_t i = 0; i < config_data.length(); ++i)
	{
		if (symbols.find(config_data[i]) != std::string::npos)
		{
			config_data.insert(i, " ");
			config_data.insert(i + 2, " ");
			i += 2;
		}
	}
	std::stringstream ss(config_data);
	std::string temp;
	while (ss >> temp)
		tokens.push_back(temp);
	return tokens;
}

ServerConfig::ServerConfig() : _port(8080), _client_max_body_size(1048576)
{}

LocationConfig::LocationConfig() : _autoindex(false), _return_code(0)
{}

size_t parse_size(std::string s)
{
	if (s.empty())
		return 0;
	char unit = toupper(s[s.length() - 1]);
	size_t multiplier = 1;

	if (unit == 'K')
		multiplier = 1024;
	else if (unit == 'M')
		multiplier = 1024 * 1024;
	else if (unit == 'G')
		multiplier = 1024 * 1024 * 1024;
	if (unit == 'K' || unit == 'M' || unit == 'G')
		s.erase(s.length() - 1);
	return (static_cast<size_t>(std::atoll(s.c_str()) * multiplier));
}

void parse_config(std::vector<std::string> tokens, Config& final_config)
{
	for (size_t i = 0; i < tokens.size(); ++i)
	{
		if (tokens[i] == "server")
		{
			i++;
			if (i < tokens.size() && tokens[i] == "{")
				i++;
			ServerConfig serv;
			while (i < tokens.size() && tokens[i] != "}")
			{
				if (tokens[i] == "listen")
				{
					if (++i < tokens.size())
						serv._port = std::atoi(tokens[i].c_str());
				}
				else if (tokens[i] == "host")
				{
					if (++i < tokens.size())
						serv._host = tokens[i];
				}
				else if (tokens[i] == "server_name")
				{
					while (++i < tokens.size() && tokens[i] != ";")
						serv._server_names.push_back(tokens[i]);
				}
				else if (tokens[i] == "client_max_body_size")
				{
					if (++i < tokens.size())
						serv._client_max_body_size = parse_size(tokens[i]); 
				}
				else if (tokens[i] == "error_page")
				{
					std::vector<std::string> tmp;
					while (++i < tokens.size() && tokens[i] != ";")
						tmp.push_back(tokens[i]);
					if (tmp.size() >= 2) 
					{
						std::string path = tmp.back();
						for (size_t j = 0; j < tmp.size() - 1; ++j)
							serv._error_pages[std::atoi(tmp[j].c_str())] = path;
					}
				}
				else if (tokens[i] == "root")
				{
					if (++i < tokens.size())
						serv._root = tokens[i];
				}
				else if (tokens[i] == "index")
				{
					while (++i < tokens.size() && tokens[i] != ";")
						serv._index.push_back(tokens[i]);
				}
				else if (tokens[i] == "location")
				{
					LocationConfig loc;
					if (++i < tokens.size())
						loc._path = tokens[i];
					if (++i < tokens.size() && tokens[i] == "{")
						i++;
					while (i < tokens.size() && tokens[i] != "}")
					{
						if (tokens[i] == "root")
						{
							if (++i < tokens.size())
								loc._root = tokens[i];
						}
						else if (tokens[i] == "allow_methods")
						{
							while (++i < tokens.size() && tokens[i] != ";")
								loc._allowed_methods.push_back(tokens[i]);
						}
						else if (tokens[i] == "index")
						{
							while (++i < tokens.size() && tokens[i] != ";")
								loc._index.push_back(tokens[i]);
						}
						else if (tokens[i] == "cgi_path")
						{
							if (++i < tokens.size())
								loc._cgi_path = tokens[i];
						}
						else if (tokens[i] == "cgi_extension")
						{
							while (++i < tokens.size() && tokens[i] != ";")
								loc._cgi_ext.push_back(tokens[i]);
						}
						else if (tokens[i] == "autoindex")
						{
							if(i < tokens.size())
							{
								if(++i < tokens.size() && tokens[i] == "on")
									loc._autoindex = true;
								else
									loc._autoindex = false;
							}
						}
						else if (tokens[i] == "return")
						{
							if (++i < tokens.size())
								loc._return_code = std::atoi(tokens[i].c_str());
							if (++i < tokens.size())
								loc._return_url = tokens[i];
						}
						else
							throw std::runtime_error("Unknown directive in location: " + tokens[i]);

						if (i < tokens.size() && tokens[i] == ";")
							i++;
						else if (i + 1 < tokens.size() && tokens[i + 1] == ";")
							i += 2;
						else
							i++;
					}
					serv._locations.push_back(loc);
					if (i < tokens.size() && tokens[i] == "}")
						i++;
					continue;
				}
				else
					throw std::runtime_error("Unknown directive in config: " + tokens[i]);

				if (i < tokens.size() && tokens[i] == ";")
					i++;
				else if (i + 1 < tokens.size() && tokens[i + 1] == ";")
					i += 2;
				else
					i++;
			}
			for (size_t j = 0; j < serv._locations.size(); ++j)
			{
				if (serv._locations[j]._root.empty())
					serv._locations[j]._root = serv._root;
				if (serv._locations[j]._index.empty())
					serv._locations[j]._index = serv._index;
			}
			if (serv._index.empty())
			{
				for (size_t j = 0; j < serv._locations.size(); ++j)
				{
					if (serv._locations[j]._path == "/" && !serv._locations[j]._index.empty())
					{
						serv._index = serv._locations[j]._index;
						break;
					}
				}
			}
			if (serv._host.empty())
				serv._host = "127.0.0.1";
			final_config._servers.push_back(serv);
		}
	}
}

void ConfigManager(char **argv, Config& servers)
{
    std::string filepath = argv[1];
    std::ifstream cnf(filepath.c_str());
    
    if (cnf.is_open())
    {
        std::ostringstream ss;
        ss << cnf.rdbuf();
        std::string config_data = ss.str();
        cnf.close();

        size_t h = config_data.find("#");
        while (h != std::string::npos)
        {
            size_t n = config_data.find("\n", h);
            
            if (n == std::string::npos) 
			{
                config_data.erase(h); 
                break;
			}
            else 
                config_data.erase(h, n - h); 
            h = config_data.find("#", h);
        }
		std::vector<std::string> tokens = tokenize(config_data);
		parse_config(tokens, servers);
    }
    else
        throw std::runtime_error("Configuration file Error");
}

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		std::cerr << "Configuration file Error" << std::endl;
		return 1;
	}
	try
	{
		Config servers;
		ConfigManager(argv, servers);
		AppManager start;
		start.run(servers);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << std::endl;
		return 1;
	}
    return 0;
}
