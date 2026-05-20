#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "ff_api.h"

char html[] =
"HTTP/1.1 200 OK\r\n"
"Server: F-Stack\r\n"
"Date: Sat, 25 Feb 2017 09:26:33 GMT\r\n"
"Content-Type: text/html\r\n"
"Content-Length: 438\r\n"
"Last-Modified: Tue, 21 Feb 2017 09:44:03 GMT\r\n"
"Connection: keep-alive\r\n"
"Accept-Ranges: bytes\r\n"
"\r\n"
"<!DOCTYPE html>\r\n"
"<html>\r\n"
"<head>\r\n"
"<title>Welcome to F-Stack!</title>\r\n"
"<style>\r\n"
"    body {  \r\n"
"        width: 35em;\r\n"
"        margin: 0 auto; \r\n"
"        font-family: Tahoma, Verdana, Arial, sans-serif;\r\n"
"    }\r\n"
"</style>\r\n"
"</head>\r\n"
"<body>\r\n"
"<h1>Welcome to F-Stack!</h1>\r\n"
"\r\n"
"<p>For online documentation and support please refer to\r\n"
"<a href=\"http://F-Stack.org/\">F-Stack.org</a>.<br/>\r\n"
"\r\n"
"<p><em>Thank you for using F-Stack.</em></p>\r\n"
"</body>\r\n"
"</html>";

int main(int argc, char **argv)
{
    int fd;
    char buffer[100];
    ssize_t bytesRead;
    struct stat st;

    ff_init(argc, argv);

    fd = ff_open("/f-stack.html", O_WRONLY | O_CREAT | O_TRUNC | O_FSYNC, 0644);
    if (fd == -1) {
        printf("ff_open failed\n");
        return 1;
    }

    if (ff_write(fd, html, sizeof(html)-1) == -1) {
        printf("ff_write failed\n");
        ff_close(fd);
        return 1;
    }

    printf("ff_write success\n");

    if (ff_fstat(fd, &st) == -1) {
        printf("ff_fstat failed \n");
        ff_close(fd);
        return 1;
    }

    printf("st.st_size = %ld\n",st.st_size);

    ff_close(fd);

    fd = ff_open("/f-stack.html", O_RDONLY,0644);

    if (fd == -1) {
        perror("打开文件失败");
        return 1;
    }

    printf("\n--------------------------\n");

    while ((bytesRead = ff_read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        printf("%s", buffer);
    }

    printf("\n--------------------------\n");

    ff_close(fd);

    return 0;
}
