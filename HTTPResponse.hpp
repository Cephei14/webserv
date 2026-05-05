#ifndef HTTPRESPONSE_HPP
# define HTTPRESPONSE_HPP

# include "HTTPRequest.hpp"
# include "ServerConfig.hpp"

class HTTPResponse
{
public:
	HTTPResponse();
	void construct_response(const HTTPRequest& req);
	void build(const HTTPRequest& req, ServerConfig& srv);
	void prepare_GET(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc);
	void prepare_POST(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc);
	void prepare_DELETE(const HTTPRequest& req, ServerConfig& srv, const LocationConfig& loc);
	std::string get_content_type(const std::string& uri);
	const std::string& get_raw_response() const;
	void body_GET(const std::string& path);
	void body_POST(const std::string& path, const std::string& body, size_t size);
	std::string handle_chunks(const HTTPRequest& req);
	void build_error_response(int status_code, ServerConfig& config);

private:
	std::string _version;
	size_t _data_size;
	int _statusCode;
	std::string _reason;
	std::string _body;
	std::string _contentType;
	std::string _post_body;
	std::string _final_response;
};

#endif
