#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdbool.h>

#include "hw/cxl/cxl_mctp_message.h"

#define BUFFER_SIZE 4096

void read_qmp_response(int sockfd)
{
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int len = read(sockfd, buffer, BUFFER_SIZE - 1);
    if (len > 0) {
        printf("QMP Response:\n%s\n", buffer);
    }
}

static void send_qmp_command(int sockfd, const char *cmd)
{
    send(sockfd, cmd, strlen(cmd), 0);
}

static void send_qmp_cap_command(int sockfd)
{
    const char *cap_cmd = "{ \"execute\": \"qmp_capabilities\" }\n";
    send_qmp_command(sockfd, cap_cmd);
    read_qmp_response(sockfd);
}

static int connect_to_qmp(const char *server, uint16_t port)
{
    int sockfd;
    struct sockaddr_in serv_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server, &serv_addr.sin_addr) <= 0) {
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        return -1;
    }

    send_qmp_cap_command(sockfd);

    return sockfd;
}

int setup_mctp_qmp_connection(const char *qmp_str)
{
    char host[256];
    uint16_t port;

    memset(host, 0, 256);
    if (sscanf(qmp_str, "%255[^:]:%hu", host, &port) != 2) {
        return -1;
    }

    return connect_to_qmp(host, port);
}

void qmp_cxl_mctp_process_cci_message(const int sockfd, const char *cci_name)
{
    char command[256];

    memset(command, 0, 256);
    sprintf(command, "{ \"execute\": \"cxl-process-mctp-message\", \
                             \"arguments\": { \
                             \"cci-name\": \"%s\" \
}}\n", cci_name);
    send_qmp_command(sockfd, command);
    read_qmp_response(sockfd);
}
