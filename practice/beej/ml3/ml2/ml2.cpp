#include "ml2.hpp"

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
				if(_fds[i].fd == sock) //new connection
				{
					int cnxfd = accept(sock, reinterpret_cast<sockaddr*>(&the_addr), &len);
					if (cnxfd == -1)
					{
						std::cerr << "Failed to connect " << std::endl;
						continue;
					}
					fcntl(cnxfd, F_SETFL, O_NONBLOCK);
					poll_setup(cnxfd);
					inet_ntop(the_addr.ss_family, get_addr_type(reinterpret_cast<sockaddr*>(&the_addr)), s, sizeof s);
					std::cout << "srv: new connection from " << s << " on fd " << cnxfd << " (Total clients: " << _fds.size() - 1 << ")" << std::endl;
				}
				else  //existing connection
				{
					char buf[BUFF_SIZE];
					int nbytes = recv(_fds[i].fd, buf, BUFF_SIZE - 1, 0);
					
					if (nbytes <= 0)
					{
						if(nbytes == 0)
							std::cout << "srv: socket " << _fds[i].fd << " hung up." << std::endl;
						else
							std::cerr << "srv: recv error on fd " << _fds[i].fd << std::endl;
						close(_fds[i].fd);
						_fds.erase(_fds.begin() + i);
						i--;
						std::cout << "srv: client disconnected. (Total clients: " << _fds.size() - 1 << ")" << std::endl;
						continue;
					}
					buf[nbytes] = '\0';
					std::cout << "srv [MSG from fd " << _fds[i].fd << "]: " << buf << std::endl;
					int sender_fd = _fds[i].fd;

					std::ostringstream oss;
					oss << "srv [client " << sender_fd << "] : " << buf;
					std::string broadcast_msg = oss.str();

					for(size_t j = 0; j < _fds.size(); j++)
					{
						int dest_fd = _fds[j].fd;
						if (dest_fd != sock && dest_fd != sender_fd)
						{
							if (send(dest_fd, broadcast_msg.c_str(), broadcast_msg.size(), 0) == -1)
								std::cerr << "Failed to send" << std::endl;
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
		
		sleep(60); // Give yourself time to open browser tabs!

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
