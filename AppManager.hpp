#ifndef APPMANAGER_HPP
# define APPMANAGER_HPP

# include "Config.hpp"

class AppManager
{
public:
	static void signal_handler(int s);
	void run(Config& servers);
};

#endif
