#include "webserv.hpp"

std::vector<std::string> tokenize(const std::string& config_data)
{
	std::vector<std::string> tokens;
	std::string current;
	for (size_t i = 0; i < config_data.length(); ++i)
	{
		char c = config_data[i];
		if (c == '{' || c == '}' || c == ';')
		{
			if (!current.empty())
			{
				tokens.push_back(current);
				current.clear();
			}
			tokens.push_back(std::string(1, c));
		}
		else if (std::isspace(static_cast<unsigned char>(c)))
		{
			if (!current.empty())
			{
				tokens.push_back(current);
				current.clear();
			}
		}
		else
			current += c;
	}
	if (!current.empty())
		tokens.push_back(current);
	return tokens;
}
size_t parse_size(const std::string& s)
{
	if (s.empty())
		return 0;
	char unit = static_cast<char>(toupper(static_cast<unsigned char>(s[s.length() - 1])));
	size_t multiplier = 1;

	if (unit == 'K')
		multiplier = 1024;
	else if (unit == 'M')
		multiplier = 1024 * 1024;
	else if (unit == 'G')
		multiplier = 1024 * 1024 * 1024;
	return (static_cast<size_t>(std::atoll(s.c_str()) * multiplier));
}

void parse_config(const std::vector<std::string>& tokens, Config& final_config)
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
						else if (tokens[i] == "client_max_body_size")
						{
							if (++i < tokens.size())
								loc._client_max_body_size = parse_size(tokens[i]);
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
		parse_config(tokenize(config_data), servers);
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
