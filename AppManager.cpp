#include "AppManager.hpp"
#include "server.hpp"
#include "WebservUtils.hpp"

void AppManager::signal_handler(int s)
{
	(void)s;
	signaled = 1;
}

void AppManager::run(Config& servers)
{
	if (signal(SIGTERM, signal_handler) == SIG_ERR)
		throw std::runtime_error("SIGNAL FAILED");
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		throw std::runtime_error("SIGNAL FAILED");
	if (signal(SIGINT, signal_handler) == SIG_ERR)
		throw std::runtime_error("SIGNAL FAILED");
	server serv;
	serv.start_listening(servers);
	serv.srv_manage(servers);
}
