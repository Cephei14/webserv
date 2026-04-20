#!/usr/bin/env python3
import os
import sys

body = sys.stdin.read()

print("Status: 200")
print("Content-Type: text/plain")
print()
print("method=" + os.environ.get("REQUEST_METHOD", ""))
print("query=" + os.environ.get("QUERY_STRING", ""))
print("len=" + os.environ.get("CONTENT_LENGTH", ""))
print("body=" + body)
