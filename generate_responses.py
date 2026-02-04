#!/usr/bin/env python3
"""Generate 16 dummy HTTP 1.1 responses with varied status codes, headers, and sizes."""

import os

RESPONSES = [
    # 1. Simple 200 OK
    {
        "name": "01_simple_200.txt",
        "content": "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!"
    },
    # 2. 200 with HTML body
    {
        "name": "02_html_response.txt",
        "content": "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: 95\r\nServer: Apache/2.4.41\r\n\r\n<!DOCTYPE html>\n<html>\n<head><title>Welcome</title></head>\n<body><h1>Hello!</h1></body>\n</html>"
    },
    # 3. 200 JSON response
    {
        "name": "03_json_response.txt",
        "content": 'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 89\r\nX-Request-ID: req-12345\r\n\r\n{"status": "success", "data": {"id": 123, "name": "John Doe", "email": "john@example.com"}}'
    },
    # 4. 201 Created
    {
        "name": "04_201_created.txt",
        "content": 'HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nLocation: /api/users/456\r\nContent-Length: 52\r\n\r\n{"id": 456, "message": "Resource created successfully"}'
    },
    # 5. 204 No Content
    {
        "name": "05_204_no_content.txt",
        "content": "HTTP/1.1 204 No Content\r\nX-Request-ID: abc-789\r\n\r\n"
    },
    # 6. 301 Moved Permanently
    {
        "name": "06_301_redirect.txt",
        "content": "HTTP/1.1 301 Moved Permanently\r\nLocation: https://www.example.com/new-page\r\nContent-Type: text/html\r\nContent-Length: 56\r\n\r\n<html><body>Moved to /new-page</body></html>"
    },
    # 7. 302 Found (temporary redirect)
    {
        "name": "07_302_redirect.txt",
        "content": "HTTP/1.1 302 Found\r\nLocation: /login\r\nSet-Cookie: session=expired; Max-Age=0\r\nContent-Length: 0\r\n\r\n"
    },
    # 8. 304 Not Modified
    {
        "name": "08_304_not_modified.txt",
        "content": "HTTP/1.1 304 Not Modified\r\nETag: \"abc123def456\"\r\nCache-Control: max-age=3600\r\n\r\n"
    },
    # 9. 400 Bad Request
    {
        "name": "09_400_bad_request.txt",
        "content": 'HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 68\r\n\r\n{"error": "Bad Request", "message": "Missing required field: email"}'
    },
    # 10. 401 Unauthorized
    {
        "name": "10_401_unauthorized.txt",
        "content": 'HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer realm="api"\r\nContent-Type: application/json\r\nContent-Length: 52\r\n\r\n{"error": "Unauthorized", "message": "Invalid token"}'
    },
    # 11. 403 Forbidden
    {
        "name": "11_403_forbidden.txt",
        "content": 'HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: 65\r\n\r\n{"error": "Forbidden", "message": "Insufficient permissions"}'
    },
    # 12. 404 Not Found
    {
        "name": "12_404_not_found.txt",
        "content": "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 127\r\n\r\n<!DOCTYPE html>\n<html>\n<head><title>404 Not Found</title></head>\n<body><h1>Not Found</h1><p>Resource not found.</p></body>\n</html>"
    },
    # 13. 500 Internal Server Error
    {
        "name": "13_500_server_error.txt",
        "content": 'HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 74\r\nRetry-After: 30\r\n\r\n{"error": "Internal Server Error", "message": "An unexpected error occurred"}'
    },
    # 14. 503 Service Unavailable
    {
        "name": "14_503_unavailable.txt",
        "content": 'HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nRetry-After: 60\r\nContent-Length: 62\r\n\r\n{"error": "Service Unavailable", "message": "Server overloaded"}'
    },
    # 15. Large response body
    {
        "name": "15_large_response.txt",
        "content": "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 1000\r\nCache-Control: public, max-age=86400\r\n\r\n" + "X" * 1000
    },
    # 16. Response with many headers
    {
        "name": "16_many_headers.txt",
        "content": 'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 27\r\nServer: nginx/1.18.0\r\nDate: Thu, 23 Jan 2026 12:00:00 GMT\r\nCache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nX-XSS-Protection: 1; mode=block\r\nStrict-Transport-Security: max-age=31536000; includeSubDomains\r\nAccess-Control-Allow-Origin: *\r\nX-Request-ID: req-abc-123-xyz\r\nX-Response-Time: 42ms\r\n\r\n{"status": "ok", "data": []}'
    },
]

def main():
    os.makedirs("sample_responses", exist_ok=True)

    for resp in RESPONSES:
        filepath = os.path.join("sample_responses", resp["name"])
        with open(filepath, "wb") as f:
            f.write(resp["content"].encode("utf-8"))
        print(f"Created: {filepath}")

    print(f"\nGenerated {len(RESPONSES)} HTTP 1.1 response files in sample_responses/")

if __name__ == "__main__":
    main()
