#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#define DS_SS_IMPLEMENTATION 
#define DS_SB_IMPLEMENTATION
#define DS_IO_IMPLEMENTATION
#define DS_AP_IMPLEMENTATION
#define DS_DA_IMPLEMENTATION
#include "ds.h"

#define MAX_LEN    1024
#define MAX_LISTEN 10
#define NOT_FOUND_STR "Not found"
#define BAD_REQUEST_STR "Bad request"

typedef enum request_kind {
    GET
} request_kind;

typedef enum protocol_kind {
    HTTP_1_1 
} protocol_kind;

const char *protocol_kind_serialize(protocol_kind kind) {
    switch(kind) {
        case HTTP_1_1:
            return "HTTP/1.1";
    }
}

typedef struct request {
    request_kind kind;
    char *path;
    char *protocol;
} request_t;

typedef struct header {
    char *key;
    char *val;
} header_t;

typedef struct response {
    protocol_kind protocol;
    int status_code;
    ds_dynamic_array headers;
    char *content;
} response_t;

const char *status_code_serialize(int status_code) {
    if (status_code == 200) {
        return "200 OK";
    }
    if (status_code == 400) {
        return "400 BAD_REQUEST";
    }
    if (status_code == 404) {
        return "404 NOT_FOUND";
    }
    if (status_code == 200) {
        return "500";
    }

    return "";
}

const char *headers_serialize(ds_dynamic_array *headers) {
    char *buffer = NULL;
    ds_string_builder buffer_builder;
    ds_string_builder_init(&buffer_builder);

    for (int i = 0; i < headers->count; i++) {
        header_t header;
        ds_dynamic_array_get(headers, i, &header);
        ds_string_builder_append(&buffer_builder, "%s: %s\n", header.key,header.val);
    }

    ds_string_builder_build(&buffer_builder, &buffer);
    return buffer;
}

int response_serialize(response_t *response, char **buffer) {
    int result = 0;

    ds_string_builder response_builder;
    ds_string_builder_init(&response_builder);

    ds_string_builder_append(&response_builder, "%s %s\n%s\n%s",
            protocol_kind_serialize(response->protocol),
            status_code_serialize(response->status_code),
            headers_serialize(&response->headers),
            response->content);

    ds_string_builder_build(&response_builder, buffer);
    result = strlen(*buffer);
defer:
    return result;
}

int response_write(int cfd, response_t *response) {
    int result = 0;
    char *buffer = NULL;
    int buffer_len = response_serialize(response, &buffer);

    result = write(cfd, buffer, buffer_len);

defer:
    return result;
}

int request_parse(char *buffer, unsigned int buffer_len, request_t *request) {
    ds_string_slice buffer_slice, token;
    char *verb = NULL;
    char *path = NULL;
    char *protocol = NULL;
    int result = 0;

    ds_string_slice_init(&buffer_slice, buffer, buffer_len);
    if (ds_string_slice_tokenize(&buffer_slice, ' ', &token) != 0) {
        DS_LOG_ERROR("expected HTTP verb");
        return_defer(-1);
    }
    if (ds_string_slice_to_owned(&token, &verb) != 0) {
        DS_LOG_ERROR("buy more ram");
        return_defer(-1);
    }

    if (strcmp(verb, "GET") == 0) {
        request->kind = GET;
    } else {
        DS_LOG_ERROR("not a GET request");
        return_defer(-1);
    }

    if (ds_string_slice_tokenize(&buffer_slice, ' ', &token) != 0) {
        DS_LOG_ERROR("expected HTTP path");
        return_defer(-1);
    }
    if (ds_string_slice_to_owned(&token, &path) != 0) {
        DS_LOG_ERROR("buy more ram");
        return_defer(-1);
    }

    request->path = path;

    if (ds_string_slice_tokenize(&buffer_slice, '\n', &token) != 0) {
        DS_LOG_ERROR("expected HTTP protocol");
        return_defer(-1);
    }
    if (ds_string_slice_to_owned(&token, &protocol) != 0) {
        DS_LOG_ERROR("buy more ram");
        return_defer(-1);
    }

    request->protocol = protocol;

defer:
    return result;
}

int read_path(char *prefix, char *path, char **content) {
    int result = 0;

    ds_string_builder path_builder;
    ds_string_builder_init(&path_builder);
    if (ds_string_builder_append(&path_builder, "%s%s", prefix, path) != 0) {
        DS_LOG_ERROR("could not create path string");
        return_defer(-1);
    }

    char *full_path = NULL;

    if (ds_string_builder_build(&path_builder, &full_path) != 0) {
        DS_LOG_ERROR("buy more ram");
        return_defer(-1);
    }

    struct stat path_stat;
    if (stat(full_path, &path_stat) != 0) {
        DS_LOG_ERROR("stat: %s", strerror(errno));
        return_defer(-1);
    }

    int content_len = 0;

    if (S_ISREG(path_stat.st_mode)) {
        content_len = ds_io_read_file(full_path, content);
    } else if (S_ISDIR(path_stat.st_mode)) {
        ds_string_builder directory_builder;
        ds_string_builder_init(&directory_builder);
        ds_string_builder_append(
                &directory_builder,
                "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta "
                "charset=\"UTF-8\">\n<title=\"Directory listing for "
                "%s</title>\n</head>\n"
                "<body>\n<h1>Directory listing for %s</h1>\n<hr>\n<ul>\n",
                path, path);

        DIR *directory = opendir(full_path);
        struct dirent *dir;
        if (directory == NULL) {
            DS_LOG_ERROR("opendir: %s", strerror(errno));
            return_defer(-1);
        }

        if (directory) {
            while((dir = readdir(directory)) != NULL) {
                if (ds_string_builder_append(&directory_builder, "<li><a href=\"%s/%s\">%s</a></li>\n", path + 1, dir->d_name, dir->d_name) != 0) {
                    DS_LOG_ERROR("could not append to response string");
                    continue;
                }
            }
        }

        if (closedir(directory) != 0) {
            DS_LOG_ERROR("closedir: %s", strerror(errno));
            return_defer(-1);
        }

        ds_string_builder_append(&directory_builder, "</ul>\n<hr>\n</body>\n</html>\n");
        if (ds_string_builder_build(&directory_builder, content) != 0) {
            DS_LOG_ERROR("buy more ram");
            return_defer(-1);
        }
        content_len = strlen(*content);

    } else {
        DS_LOG_ERROR("mode not supported yest");
        return_defer(-1);
    }

    result = content_len;

defer:
    return result;
}

int handle_request(int cfd, char *prefix_directory) {
    int result = 0;
    unsigned int buffer_len = 0;
    char buffer[MAX_LEN] = {0};
    request_t request = {0};
    int content_len;
    char *content = NULL;
    response_t response = {0};
    ds_dynamic_array_init(&response.headers, sizeof(header_t));

    response.protocol = HTTP_1_1;

    result = read(cfd, buffer, MAX_LEN);
    if (result == -1) {
        response.status_code = 500;
        DS_LOG_ERROR("read %s", strerror(errno));
        return_defer(-1);
    }
    buffer_len = result;

    if (request_parse(buffer, buffer_len, &request) == -1) {
        DS_LOG_ERROR("request parse");
        response.status_code = 400;
        {
            header_t header = {.key = "Content-Type", .val = "text/html"};
            ds_dynamic_array_append(&response.headers, &header);
        }
        {
            ds_string_builder content_len_builder;
            ds_string_builder_init(&content_len_builder);
            ds_string_builder_append(&content_len_builder, "%d", strlen(BAD_REQUEST_STR));
            char *content_len;
            ds_string_builder_build(&content_len_builder, &content_len);
            header_t header = {.key = "Content-Length", .val = content_len};
            ds_dynamic_array_append(&response.headers, &header);
        }
        response.content = BAD_REQUEST_STR;
        return_defer(-1);
    }

    result = read_path(prefix_directory, request.path, &content);
    if (result == -1) {
        DS_LOG_ERROR("read path");
        response.status_code = 404;
        {
            header_t header = {.key = "Content-Type", .val = "text/html"};
            ds_dynamic_array_append(&response.headers, &header);
        }
        {
            ds_string_builder content_len_builder;
            ds_string_builder_init(&content_len_builder);
            ds_string_builder_append(&content_len_builder, "%d", strlen(NOT_FOUND_STR));
            char *content_len;
            ds_string_builder_build(&content_len_builder, &content_len);
            header_t header = {.key = "Content-Length", .val = content_len};
            ds_dynamic_array_append(&response.headers, &header);
        }
        response.content = NOT_FOUND_STR;
        return_defer(-1);
    }

    content_len = result;

    response.status_code = 200;
    {
        header_t header = {.key = "Content-Type", .val = "text/html"};
        ds_dynamic_array_append(&response.headers, &header);
    }
    {
        ds_string_builder content_len_builder;
        ds_string_builder_init(&content_len_builder);
        ds_string_builder_append(&content_len_builder, "%d", content_len);
        char *content_len;
        ds_string_builder_build(&content_len_builder, &content_len);
        header_t header = {.key = "Content-Length", .val = content_len};
        ds_dynamic_array_append(&response.headers, &header);
    }
    response.content = content;

defer:
    response_write(cfd, &response);
    return result;
}

int main(int argc, char *argv[]) {
    int sfd, cfd, result, port;
    char *prefix_directory = NULL;
    struct sockaddr_in server_addr;
    ds_argparse_parser parser;

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        DS_PANIC("getcwd %s", strerror(errno));
    }

    ds_string_builder directory_builder;
    ds_string_builder_init(&directory_builder);
    ds_string_builder_append(&directory_builder, "%s", cwd);

    ds_argparse_parser_init(&parser, "http server", "a clone of http server in C", "1.0");
    ds_argparse_add_argument(
            &parser, (ds_argparse_options){
            .short_name = 'p',
            .long_name = "port",
            .description = 
                "bind to this port(default: 8000)",
            .type = ARGUMENT_TYPE_POSITIONAL, 
            .required = 0});


    ds_argparse_add_argument(
            &parser, (ds_argparse_options){
            .short_name = 'd',
            .long_name = "directory",
            .description = 
                "serve this directory(default: current directory)",
            .type = ARGUMENT_TYPE_VALUE,
            .required = 0});

    ds_argparse_parse(&parser, argc, argv);
    char *port_value = ds_argparse_get_value(&parser, "port");

    char *directory_value = ds_argparse_get_value(&parser, "directoey");
    if (directory_value != NULL) {
        ds_string_builder_append(&directory_builder, "/%s", directory_value);
    }
    ds_string_builder_build(&directory_builder, &prefix_directory);

    port = (port_value == NULL) ? 8000 : atoi(port_value);

    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) {
        DS_PANIC("socket: %s", strerror(errno));
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, "0.0.0.0", &server_addr.sin_addr);

    result = bind(sfd, (struct sockaddr*) &server_addr, sizeof(server_addr));
    if (result == -1) {
        DS_PANIC("bind: %s", strerror(errno));
    }

    result = listen(sfd, MAX_LISTEN);
    if (result == -1) {
        DS_PANIC("listen: %s", strerror(errno));
    }

    DS_LOG_INFO("listening on port: %d serving from %s", port, prefix_directory);

    while(1) {
        int result;
        char buffer[MAX_LEN] = {0};
        struct sockaddr_in client_addr;
        socklen_t client_addr_size = sizeof(client_addr);

        cfd = accept(sfd, (struct sockaddr*) &client_addr, &client_addr_size);
        if (cfd == -1) {
            DS_LOG_ERROR("accept: %s", strerror(errno));
            continue;
        }

        handle_request(cfd, prefix_directory);
    }

    result = close(sfd);
    if (result == -1) {
        DS_PANIC("close");
    }

    return 0;
}
