#include "ml4.hpp"

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

void server::start_listening()
{
	struct addrinfo hints, *res, *p;
	int ra;
	int yes = 1;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if ((ra = getaddrinfo(NULL, PORT, &hints, &res)) != 0)
	{
		std::cerr << "srv[ERROR]: getaddrinfo " << gai_strerror(ra) <<std::endl;
		return;
	}
	for(p = res; p != NULL; p = p->ai_next)
	{
		if((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			std::cerr << "srv [socket] " << std::endl;
			continue;
		}
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes ,sizeof(int)) == -1)
		{
			std::cerr << "srv [setsockopt]" << std::endl;
			exit(1);
		}
		if (bind(sock, p->ai_addr, p->ai_addrlen) == -1)
		{
			close(sock);
			std::cerr << "srv [Bind]" << std::endl;
			continue;
		}
		break;
	}
	freeaddrinfo(res);

	if (p == NULL)
	{
		std::cerr << "server failed to bind" << std::endl;
		exit (1);
	}
	if (listen(sock, 10) == -1)
	{
		std::cerr << "Listen" << std::endl;
		exit(1);
	}

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
			throw std::out_of_range("Invalid Method");
		std::string requestLine = buf.substr(0, n);
		size_t pos = requestLine.find(' ');
		if(pos == std::string::npos)
			throw std::out_of_range("Invalid Method");
		_method = requestLine.substr(0, pos);
		if (_method != "POST" && _method != "GET" && _method != "DELETE")
			throw std::runtime_error("Invalid Method");
		size_t m = requestLine.find(' ', pos + 1);
		if(m == std::string::npos)
			throw std::out_of_range("Invalid Method");
		_uri = requestLine.substr(pos + 1, m - pos - 1);
		if (_uri[0] != '/')
			throw std::runtime_error("Invalid Method");
		_version = requestLine.substr(m + 1, n - m - 1);
		if (_version != "HTTP/1.1")
			throw std::runtime_error("Invalid Method");

		std::string allowed_chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";	
		if (end_of_headers == std::string::npos)
			throw std::runtime_error("400 Bad Request");
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

void HTTPResponse::prepare_GET(const HTTPRequest& req)
{
	std::string root = "./www";
	std::string full_path = root + req.getUri();
	if (req.getUri() == "/")
		full_path += "index.html";
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
	size_t dot_pos = uri.find_last_of('.'); //to avoid downloading

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

	if (header_req.find("host") == header_req.end())
	{
		_statusCode = 400;
        _body = "<h1>400 Bad Request</h1>";
	}
	if (_reason.empty()) 
	{
		if (_statusCode == 200)
			_reason = "OK";
		else if (_statusCode == 404)
			_reason = "Not Found";
		else if (_statusCode == 400)
			_reason = "Bad Request";
		else if (_statusCode == 501)
			_reason = "Not Implemented";
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

void HTTPResponse::prepare_POST(const HTTPRequest& req)
{
	std::string root = "./www";
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

void HTTPResponse::prepare_DELETE(const HTTPRequest& req)
{
	std::string root = "./www";
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

void HTTPResponse::prepare_else(const HTTPRequest& req)
{
	_statusCode = 501;
	_body = "<h1>501 Not Implemented</h1>";
	_contentType = "text/html";
	construct_response(req);
}

void HTTPResponse::build(const HTTPRequest& req)
{
	if(req.getMethod() == "GET")
		prepare_GET(req);
	else if(req.getMethod() == "POST")
		prepare_POST(req);
	else if(req.getMethod() == "DELETE")
		prepare_DELETE(req);
	else
		prepare_else(req);
}

void server::close_connection(size_t& i)
{
	int fd = _fds[i].fd;
	close(fd);
	_requests.erase(fd);
	_responses.erase(fd);
	_fds.erase(_fds.begin() + i);
	i--;
}

void server::srv_manage()
{
	fcntl(sock, F_SETFL, O_NONBLOCK);
	poll_setup(sock);
	while(1)
	{
		if (poll(&_fds[0], _fds.size(), TOUT) == -1)
		{
			if (errno == EINTR)
				continue;
			std::cerr << "Poll" << std::endl;
			exit(1);
		}
		for(size_t i = 0; i < _fds.size(); i++)
		{
			if (_fds[i].revents & POLLIN)
			{
				struct sockaddr_storage the_addr;
				socklen_t len = sizeof the_addr;
				char s[INET6_ADDRSTRLEN];
				int client_fd = _fds[i].fd;

				if(client_fd == sock) //new connection
				{
					int cnxfd = accept(sock, reinterpret_cast<sockaddr*>(&the_addr), &len);
					if (cnxfd == -1)
					{
						std::cerr << "Failed to connect " << std::endl;
						continue;
					}
					fcntl(cnxfd, F_SETFL, O_NONBLOCK);
					poll_setup(cnxfd);
					_requests[cnxfd] = HTTPRequest();
					inet_ntop(the_addr.ss_family, get_addr_type(reinterpret_cast<sockaddr*>(&the_addr)), s, sizeof s);
					std::cout << "srv: new connection from " << s << " on fd " << cnxfd << " (Total clients: " << _fds.size() - 1 << ")" << std::endl;
				}
				else  //existing connection
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
							std::cerr << e.what() << std::endl;
							close_connection(i);
						}
						if (current_req.IsParsed())
						{
							HTTPResponse new_response;
							new_response.build(current_req);
							std::string raw = new_response.get_raw_response();
                            send(client_fd, raw.c_str(), raw.size(), 0);
							close_connection(i);
							std::cout << "srv: response sent and connection closed." << std::endl;
						}
						
					}
				}
			}
		}
	}
}

void client::start_cnx()
{
	struct addrinfo *p, hints, *res;
	int ra, sz;
	char buf[BUFF_SIZE];
	char s[INET6_ADDRSTRLEN];

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if ((ra = getaddrinfo(IP , PORT , &hints, &res)) != 0)
	{
		std::cerr << "cli[ERROR]: getaddrinfo " << gai_strerror(ra) <<std::endl;
		return;
	}
	for (p = res; p != NULL; p = p->ai_next)
	{
		if ((sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1)
		{
			std::cerr << "cli [socket] " << std::endl;
			continue;
		}
		if (connect(sock, p->ai_addr, p->ai_addrlen) == -1)
		{
			std::cerr << "cli [connect] " << std::endl;
			close(sock);
			continue;
		}
		break;
	}

	if (p == NULL)
	{
		std::cerr << "Failed to connect to a server " << std::endl;
		freeaddrinfo(res);
		return; 
	}

	inet_ntop(p->ai_family, get_addr_type(p->ai_addr), s, sizeof s);
	std::cout << "Connected to " << s << std::endl;
	freeaddrinfo(res);

	if ((sz = send(sock, "Hello from client", 17, 0)) == -1)
		std::cerr << "cli [send] " << std::endl;

	if ((sz = recv(sock, buf, BUFF_SIZE - 1, 0)) > 0)
	{
		buf[sz] = '\0';
		std::cout << "cli [MSG] : " << buf << std::endl;
	}
	else if (sz == -1)
		std::cerr << "cli [recv] " << std::endl;

	close(sock);
}

void AppManager::signal_handler(int s)
{
	(void)s;
	int saved_errno = errno;
	while(waitpid(-1, NULL, WNOHANG) > 0);
	errno = saved_errno;
}

void AppManager::run()
{
	struct sigaction sa;

	sa.sa_handler = AppManager::signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if(sigaction(SIGCHLD, &sa, NULL) == -1)
	{
		std::cerr << "Sigaction" <<std::endl;
		exit (1);
	}

	pid_t pid = fork();

	if(pid == -1)
	{
		std::cerr << "Fork" << std::endl;
		exit (1);
	}
	else if (pid == 0)
	{
		server serv;
		serv.start_listening();
		serv.srv_manage();
	}
	else
	{
		std::cout << "[Test Mode] Server is running on port " << PORT << std::endl;
		std::cout << "[Test Mode] You can now connect via Browser or 'nc' in other terminals." << std::endl;
		std::cout << "[Test Mode] Waiting 600 seconds before auto-shutdown..." << std::endl;
		
		sleep(600);

		std::cout << "[Test Mode] Shutting down server..." << std::endl;
		kill(pid, SIGINT);
		waitpid(pid, NULL, 0);
	}
}

int main(void)
{
	AppManager start;
	start.run();
    return 0;
}
