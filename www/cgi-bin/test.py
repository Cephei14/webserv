#!/usr/bin/env python3
import os

print("Status: 200")
print("Content-Type: text/plain")
print()
print("method=" + os.environ.get("REQUEST_METHOD", ""))
print("CGI ended!")
