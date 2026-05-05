#ifndef HTTPREQUEST_HPP
# define HTTPREQUEST_HPP

# include "WebservCommon.hpp"

class HTTPRequest
{
public:
	HTTPRequest();
	bool IsParsed() const;
	void AddRawP(const char* line, int nbytes);
	void Validate(const std::string& line);
	void consume_parsed_request();
	void reset_parse_state();
	void clearBody();
	void takeBody(std::string& out);
	const std::string& getMethod() const;
	const std::string& getUri() const;
	const std::map<std::string, std::string>& getMap() const;
	const std::string& getVersion() const;
	const std::string& getBody() const;

private:
	std::string _method;
	std::string _uri;
	std::string _version;
	std::string _raw_buf;
	std::string _body;
	std::map<std::string, std::string> _headers;
	bool _isParsed;
	bool _headersParsed;
	size_t _parsed_request_end;
};

#endif
