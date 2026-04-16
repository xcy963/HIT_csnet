# 计算机网络实验

## fuckwindows还是不支持utf-8
- 把代码保存为gbk就能解决了

## 第一次转发失败,但是终端里有收到请求
```bash
PS G:\0cs_net> make run
.\build/proxy_server.exe
Proxy is listening on port 10240
接收到连接 method: CONNECT host:cn.bing.com:443 url:cn.bing.com:443
接收到连接 method: CONNECT host:functional.events.data.microsoft.com:443 url:functional.events.data.microsoft.com:443
接收到连接 method: CONNECT host:cn.bing.com:443 url:cn.bing.com:443
接收到连接 method: CONNECT host:functional.events.data.microsoft.com:443 url:functional.events.data.microsoft.com:443
接收到连接 method: CONNECT host:cn.bing.com:443 url:cn.bing.com:443
```


## 查到的http协议内容 
```c

// HTTP 报文（逻辑结构，不是内存定长结构）
struct HttpMessage {
    StartLine start_line;     // 请求行 或 状态行
    Header headers[];         // 0..N 个首部行
    char CRLF[2];             // 空行: "\r\n"，表示首部结束
    byte body[];              // 可选，长度由规则决定
};

struct RequestLine {
    string method;            // "GET", "POST", ...
    string request_target;    // "/index.html?x=1"
    string http_version;      // "HTTP/1.1"
    // 格式: METHOD SP request-target SP HTTP-version CRLF
};

struct StatusLine {
    string http_version;      // "HTTP/1.1"
    int status_code;          // 200, 404, ...
    string reason_phrase;     // "OK", "Not Found"（可为空）
    // 格式: HTTP-version SP status-code SP reason-phrase CRLF
};

struct Header {
    string field_name;        // "Host", "Content-Length", ...
    string field_value;       // 对应值
    // 格式: field-name ":" OWS field-value OWS CRLF
};
```

```txt
[文本行区域]
Request-Line\r\n
Header1: ...\r\n
Header2: ...\r\n
...
\r\n
[原始字节Body，可文本可二进制]

```

- 一个简单的例子
```txt
CONNECT ws.chatgpt.com:443 HTTP/1.1
Host: ws.chatgpt.com:443
Proxy-Connection: keep-alive
User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36 Edg/147.0.0.0


```

## get和post的区别

- get从url传递参数
```http
GET /search?q=chatgpt HTTP/1.1
Host: example.com
```

- post创建一个json行
```http
POST /submit HTTP/1.1
Host: example.com
Content-Type: application/json

{"name": "Alice"}
```