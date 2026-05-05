#ifndef LOCATIONCONFIG_HPP
# define LOCATIONCONFIG_HPP

# include "WebservCommon.hpp"

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

#endif
