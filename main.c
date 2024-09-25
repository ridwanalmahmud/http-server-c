#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define DS_SS_IMPLEMENTATION 
#define DS_SB_IMPLEMENTATION
#define DS_IO_IMPLEMENTATION
#include "ds.h"

#define MAX_LEN 1024

int main() {
    int sfd, cfd, result;
    socklen_t client_addr_size;
    struct sockaddr_in addr, client_addr;

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        DS_PANIC("socket");
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(8000);
    inet_pton(AF_INET, "0.0.0.0", &addr.sin_addr);

    result = bind(sfd, (struct sockaddr*) &addr, sizeof(addr));
    if (result == -1) {
        DS_PANIC("bind");
    }

    result = listen(sfd, 10);
    if (result == -1) {
        DS_PANIC("listen");
    }

    while(1) {
        client_addr_size = sizeof(client_addr);
        cfd = accept(sfd, (struct sockaddr*) &client_addr, &client_addr_size);
        if (cfd == -1) {
            DS_PANIC("accept");
        }

        char buffer[MAX_LEN] = {0};
        int result = read(cfd, buffer, MAX_LEN);
        if (result == -1) {
            perror("read");
            continue;
        }
        unsigned int buffer_len = result;

        ds_string_slice request, token;
        ds_string_slice_init(&request, buffer, buffer_len);

        ds_string_slice_tokenize(&request, ' ', &token);
        char *verb = NULL;
        ds_string_slice_to_owned(&token, &verb);
        if (strcmp(verb, "GET") != 0) {
            DS_LOG_ERROR("not a get request");
            continue;
        }

        ds_string_slice_tokenize(&request, ' ', &token);
        char *path = NULL;
        ds_string_slice_to_owned(&token, &path);

        char *content = NULL;
        int content_len = ds_io_read_file(path + 1, &content);

        ds_string_builder response_builder;
        ds_string_builder_init(&response_builder);

        ds_string_builder_append(&response_builder, "HTTP/1.1 200 OK\nContent-Type: text/html\nContent-Length: %d\n\n%s", content_len, content);

        char *response = NULL;
        ds_string_builder_build(&response_builder, &response);
        int response_len = strlen(response);

        write(cfd, response, response_len);
    }

    result = close(sfd);
    if (result == -1) {
        DS_PANIC("close");
    }

    return 0;
}
