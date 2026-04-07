#include "ml3.hpp"

HTTPRequest::HTTPRequest() : _isParsed(false), _headersParsed(false) {}

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
	else
	    _isParsed = true;
}

void HTTPRequest::AddRawP(const char* line, int nbytes)
{
	_raw_buf.append(line, nbytes);
	if ((_raw_buf.find("\r\n\r\n") != std::string::npos) && !_isParsed)
		Validate(_raw_buf);
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
						close(client_fd);
						_requests.erase(client_fd);
						_fds.erase(_fds.begin() + i);
						i--;
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
							close(client_fd);
							_requests.erase(client_fd);
							_fds.erase(_fds.begin() + i);
							i--;
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
		std::cout << "[Test Mode] Waiting 60 seconds before auto-shutdown..." << std::endl;
		
		sleep(60);

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
