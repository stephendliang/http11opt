#!/usr/bin/env python3
"""Generate 32 dummy HTTP 1.1 requests with varied methods, headers, and sizes."""

import os

REQUESTS = [
    # 1. Simple GET
    {
        "name": "01_simple_get.txt",
        "content": "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"
    },
    # 2. GET with multiple headers
    {
        "name": "02_get_with_headers.txt",
        "content": "GET /api/users HTTP/1.1\r\nHost: api.example.com\r\nAccept: application/json\r\nAccept-Language: en-US,en;q=0.9\r\nUser-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\nCache-Control: no-cache\r\n\r\n"
    },
    # 3. POST with small body
    {
        "name": "03_post_small.txt",
        "content": "POST /login HTTP/1.1\r\nHost: example.com\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 29\r\n\r\nusername=admin&password=1234"
    },
    # 4. POST with JSON body
    {
        "name": "04_post_json.txt",
        "content": 'POST /api/data HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/json\r\nAccept: application/json\r\nContent-Length: 52\r\n\r\n{"name": "John Doe", "email": "john@example.com"}'
    },
    # 5. PUT request
    {
        "name": "05_put_request.txt",
        "content": 'PUT /api/users/123 HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/json\r\nAuthorization: Bearer token123abc\r\nContent-Length: 67\r\n\r\n{"id": 123, "name": "Jane Doe", "role": "admin", "active": true}'
    },
    # 6. DELETE request
    {
        "name": "06_delete_request.txt",
        "content": "DELETE /api/users/456 HTTP/1.1\r\nHost: api.example.com\r\nAuthorization: Bearer secrettoken\r\nX-Request-ID: abc123\r\n\r\n"
    },
    # 7. PATCH request
    {
        "name": "07_patch_request.txt",
        "content": 'PATCH /api/users/789 HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/json-patch+json\r\nContent-Length: 34\r\n\r\n[{"op": "replace", "path": "/name"}]'
    },
    # 8. HEAD request
    {
        "name": "08_head_request.txt",
        "content": "HEAD /index.html HTTP/1.1\r\nHost: www.example.com\r\nAccept: text/html\r\n\r\n"
    },
    # 9. OPTIONS request
    {
        "name": "09_options_request.txt",
        "content": "OPTIONS /api/users HTTP/1.1\r\nHost: api.example.com\r\nOrigin: https://frontend.example.com\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: Content-Type, Authorization\r\n\r\n"
    },
    # 10. GET with query parameters
    {
        "name": "10_get_query_params.txt",
        "content": "GET /search?q=http+protocol&page=1&limit=20&sort=relevance HTTP/1.1\r\nHost: search.example.com\r\nAccept: application/json\r\nX-API-Key: api_key_12345\r\n\r\n"
    },
    # 11. POST with large body
    {
        "name": "11_post_large.txt",
        "content": "POST /api/documents HTTP/1.1\r\nHost: docs.example.com\r\nContent-Type: text/plain\r\nContent-Length: 1000\r\n\r\n" + "A" * 1000
    },
    # 12. GET with cookies
    {
        "name": "12_get_with_cookies.txt",
        "content": "GET /dashboard HTTP/1.1\r\nHost: app.example.com\r\nCookie: session_id=abc123def456; user_pref=dark_mode; tracking_id=xyz789\r\nAccept: text/html\r\n\r\n"
    },
    # 13. POST multipart (simplified)
    {
        "name": "13_post_multipart.txt",
        "content": "POST /upload HTTP/1.1\r\nHost: files.example.com\r\nContent-Type: multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxk\r\nContent-Length: 200\r\n\r\n------WebKitFormBoundary7MA4YWxk\r\nContent-Disposition: form-data; name=\"file\"; filename=\"test.txt\"\r\nContent-Type: text/plain\r\n\r\nHello World!\r\n------WebKitFormBoundary7MA4YWxk--\r\n"
    },
    # 14. GET with range header
    {
        "name": "14_get_range.txt",
        "content": "GET /video/large.mp4 HTTP/1.1\r\nHost: cdn.example.com\r\nRange: bytes=0-1023\r\nAccept: video/mp4\r\nIf-Range: \"etag123\"\r\n\r\n"
    },
    # 15. POST with compression headers
    {
        "name": "15_post_compressed.txt",
        "content": 'POST /api/batch HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nAccept-Encoding: gzip, deflate, br\r\nContent-Length: 85\r\n\r\n{"items": [{"id": 1}, {"id": 2}, {"id": 3}], "operation": "update", "async": true}'
    },
    # 16. CONNECT request (for proxies)
    {
        "name": "16_connect_request.txt",
        "content": "CONNECT www.example.com:443 HTTP/1.1\r\nHost: www.example.com:443\r\nProxy-Authorization: Basic dXNlcjpwYXNz\r\nProxy-Connection: Keep-Alive\r\n\r\n"
    },
    # 17. TRACE request (rarely used)
    {
        "name": "17_trace_request.txt",
        "content": "TRACE /debug HTTP/1.1\r\nHost: example.com\r\nMax-Forwards: 5\r\n\r\n"
    },
    # 18. Very long URI (2KB path)
    {
        "name": "18_long_uri.txt",
        "content": "GET /" + "a" * 2000 + "?param=value HTTP/1.1\r\nHost: example.com\r\n\r\n"
    },
    # 19. Many query parameters
    {
        "name": "19_many_query_params.txt",
        "content": "GET /api/search?" + "&".join([f"param{i}=value{i}" for i in range(50)]) + " HTTP/1.1\r\nHost: api.example.com\r\nAccept: application/json\r\n\r\n"
    },
    # 20. Duplicate headers (valid per RFC)
    {
        "name": "20_duplicate_headers.txt",
        "content": "GET /resource HTTP/1.1\r\nHost: example.com\r\nAccept: text/html\r\nAccept: application/xhtml+xml\r\nAccept: application/xml;q=0.9\r\nCache-Control: no-cache\r\nCache-Control: no-store\r\n\r\n"
    },
    # 21. Header with very long value
    {
        "name": "21_long_header_value.txt",
        "content": "GET /api/data HTTP/1.1\r\nHost: example.com\r\nX-Custom-Data: " + "x" * 4000 + "\r\nAccept: application/json\r\n\r\n"
    },
    # 22. Many headers (50+)
    {
        "name": "22_many_headers.txt",
        "content": "GET /api/resource HTTP/1.1\r\nHost: example.com\r\n" + "".join([f"X-Custom-Header-{i}: value-{i}\r\n" for i in range(50)]) + "\r\n"
    },
    # 23. Chunked transfer encoding request
    {
        "name": "23_chunked_request.txt",
        "content": "POST /api/stream HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n7\r\n{\"data\":\r\n8\r\n\"hello\"}\r\n0\r\n\r\n"
    },
    # 24. POST with very large body (10KB)
    {
        "name": "24_very_large_body.txt",
        "content": "POST /api/bulk HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/octet-stream\r\nContent-Length: 10000\r\n\r\n" + "B" * 10000
    },
    # 25. Request with UTF-8 in headers
    {
        "name": "25_utf8_headers.txt",
        "content": "GET /api/users HTTP/1.1\r\nHost: example.com\r\nX-User-Name: José García\r\nX-City: 東京\r\nX-Emoji: 🚀🔥\r\nAccept: application/json\r\n\r\n"
    },
    # 26. Request with URL-encoded special chars
    {
        "name": "26_url_encoded.txt",
        "content": "GET /search?q=%E4%B8%AD%E6%96%87&filter=%3Cscript%3E&path=%2Fetc%2Fpasswd HTTP/1.1\r\nHost: example.com\r\nAccept: text/html\r\n\r\n"
    },
    # 27. Request with empty header values
    {
        "name": "27_empty_header_values.txt",
        "content": "GET /resource HTTP/1.1\r\nHost: example.com\r\nX-Empty-Header: \r\nX-Another-Empty: \r\nAccept: */*\r\n\r\n"
    },
    # 28. Request with folded headers (obsolete but valid)
    {
        "name": "28_folded_headers.txt",
        "content": "GET /legacy HTTP/1.1\r\nHost: example.com\r\nX-Long-Header: this is a very long header value\r\n that continues on the next line\r\n and even a third line\r\nAccept: text/html\r\n\r\n"
    },
    # 29. POST with binary-like content
    {
        "name": "29_binary_content.txt",
        "content": "POST /upload/binary HTTP/1.1\r\nHost: files.example.com\r\nContent-Type: application/octet-stream\r\nContent-Length: 256\r\n\r\n" + "".join([chr(i) for i in range(256) if i not in (0, 10, 13)])
    },
    # 30. Absolute URI in request line
    {
        "name": "30_absolute_uri.txt",
        "content": "GET http://proxy.example.com/path/to/resource?query=value HTTP/1.1\r\nHost: proxy.example.com\r\nProxy-Connection: keep-alive\r\n\r\n"
    },
    # 31. Request with Expect: 100-continue
    {
        "name": "31_expect_continue.txt",
        "content": 'POST /api/large-upload HTTP/1.1\r\nHost: api.example.com\r\nContent-Type: application/json\r\nContent-Length: 1048576\r\nExpect: 100-continue\r\n\r\n'
    },
    # 32. Complex multipart with multiple files
    {
        "name": "32_complex_multipart.txt",
        "content": "POST /api/upload-multiple HTTP/1.1\r\nHost: files.example.com\r\nContent-Type: multipart/form-data; boundary=---------------------------9051914041544843365972754266\r\nContent-Length: 554\r\n\r\n-----------------------------9051914041544843365972754266\r\nContent-Disposition: form-data; name=\"text_field\"\r\n\r\nsome text value\r\n-----------------------------9051914041544843365972754266\r\nContent-Disposition: form-data; name=\"file1\"; filename=\"document.txt\"\r\nContent-Type: text/plain\r\n\r\nThis is file 1 content.\r\n-----------------------------9051914041544843365972754266\r\nContent-Disposition: form-data; name=\"file2\"; filename=\"image.png\"\r\nContent-Type: image/png\r\n\r\n" + "PNG_BINARY_DATA_HERE" + "\r\n-----------------------------9051914041544843365972754266--\r\n"
    },
]

def main():
    os.makedirs("sample_requests", exist_ok=True)

    for req in REQUESTS:
        filepath = os.path.join("sample_requests", req["name"])
        with open(filepath, "wb") as f:
            f.write(req["content"].encode("utf-8"))
        print(f"Created: {filepath}")

    print(f"\nGenerated {len(REQUESTS)} HTTP 1.1 request files in sample_requests/")

if __name__ == "__main__":
    main()
