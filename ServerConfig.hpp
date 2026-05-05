#ifndef SERVERCONFIG_HPP
# define SERVERCONFIG_HPP

# include "LocationConfig.hpp"

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

#endif
