*This project has been created as part of the 42 curriculum by rdhaibi.*

## Description
`webserv` is a small HTTP/1.1 server written in C++98.

It supports:
- Multiple server blocks and listening ports from a configuration file
- Route-based configuration (`root`, `index`, `allow_methods`, `autoindex`, `return`)
- Static file serving
- `GET`, `POST`, and `DELETE`
- File upload and delete in configured locations
- CGI execution by extension (Python example)
- Custom and default error pages
- Keep-alive and request pipelining handling in a non-blocking poll-driven loop

## Instructions
Build:

```bash
make
```

Run:

```bash
../webserv webserv.conf
```

Quick checks:

```bash
# Static page
curl --http1.1 -i http://127.0.0.1:8080/

# Redirect
curl --http1.1 -i http://127.0.0.1:8080/old_site

# Upload + read + delete
curl --http1.1 -i -X POST http://127.0.0.1:8080/uploads/newfile.txt --data-binary 'hello'
curl --http1.1 -i http://127.0.0.1:8080/uploads/newfile.txt
curl --http1.1 -i -X DELETE http://127.0.0.1:8080/uploads/newfile.txt

# CGI
curl --http1.1 -i 'http://127.0.0.1:8080/cgi-bin/test.py?x=42'
```

## Resources
- RFC 7230 (HTTP/1.1 Message Syntax and Routing)
- RFC 7231 (HTTP/1.1 Semantics and Content)
- NGINX documentation for behavior comparison
- BEEJ's guide for socket coding
- `man poll`, `man socket`, `man recv`, `man send`, `man execve`, `man waitpid`

AI usage:
- AI was used for incremental code review, debugging support, and explaining CGI/poll architecture.
- All generated changes were manually reviewed, compiled, and tested before keeping them.
