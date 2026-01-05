/**
 * Copyright 2023 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/time.h>
#ifdef LINUX
#include <sys/sendfile.h>
#endif
#include <errno.h>
#include <pthread.h>
#include <sys/sendfile.h>
#include "web.h"
#include "wifi_util.h"

typedef struct {
    web_t *web;
    int sock;
} web_args_t;

web_t* web_t::instance = nullptr;
web_t* web_t::get_instance(const char *path)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    wifi_util_info_print(WIFI_APPS," %s:%d\n",__func__,__LINE__);
    pthread_mutex_lock(&lock);
 
    if (instance == nullptr) {
        instance = new web_t(path);
    }
 
    pthread_mutex_unlock(&lock);
    return instance;
}

char* web_t::parse(char line[], const char symbol[])
{
#if 0
    char *copy = (char *)malloc(strlen(line) + 1);
    strcpy(copy, line);

    char *message;
    char * token = strtok(copy, symbol);
    int current = 0;

    while (token != NULL) {

        token = strtok(NULL, " ");
        if (current == 0) {
            message = token;
            free(token);
            free(copy);
            return message;
        }
        current = current + 1;
    }
    free(token);
    free(copy);
    return NULL;
 #endif
    if (!line) return NULL;
    char *copy = strdup(line);
    if (!copy) return NULL;

    char *token = strtok(copy, symbol);
    token = strtok(NULL, symbol);  // second token

    char *result = token ? strdup(token) : NULL;

    free(copy);
    return result;
}

char* web_t::parse_method(char line[], const char symbol[])
{
#if 0
    char *copy = (char *)malloc(strlen(line) + 1);
    strcpy(copy, line);

    char *message = NULL;
    char * token = strtok(copy, symbol);
    int current = 0;

    while (token != NULL) {

      //token = strtok(NULL, " ");
        if (current == 0) {
            message = token;
            free(token);
            free(copy);
            return message;
        }
        current = current + 1;
    }
    free(copy);
    free(token);
    return NULL;
 #endif
  if (!line) return NULL;

    char *copy = strdup(line);
    if (!copy) return NULL;

    char *token = strtok(copy, symbol);  // first token
    char *result = token ? strdup(token) : NULL;

    free(copy);
    return result;
}

char* web_t::find_token(char line[], const char symbol[], const char match[])
{
#if 0
    char *copy = (char *)malloc(strlen(line) + 1);
    strcpy(copy, line);

    char *message;
    char *token = strtok(copy, symbol);

    while (token != NULL) {
        
        if (strlen(match) <= strlen(token)) {
            int match_char = 0;
            for (int i = 0; i < strlen(match); i++) {
                if (token[i] == match[i]) {
                    match_char++;
                }
            }
            if (match_char == strlen(match)) {
                message = token;
                free(token);
                free(copy);
                return message;
            }
        }
        token = strtok(NULL, symbol);
    }
    free(copy);
    free(token);
    return NULL;
 #endif
    if (!line || !match) return NULL;
    char *copy = strdup(line);
    if (!copy) return NULL;

    char *token = strtok(copy, symbol);
    while (token) {
        if (strncmp(token, match, strlen(match)) == 0) {
            char *result = strdup(token);
            free(copy);
            return result;
        }
        token = strtok(NULL, symbol);
    }

    free(copy);
    return NULL;
}

int web_t::send_message(int fd, char image_path[], char head[])
{
    int fdimg;
    struct stat stat_buf;  /* hold information about input file */
    off_t total_size, offset = 0;
    off_t len = 0;

    write(fd, head, strlen(head));

    if((fdimg = open(image_path, O_RDONLY)) < 0){
        wifi_util_dbg_print(WIFI_APPS,"%s:%d: Cannot Open file in path : %s with error %d\n", __func__, __LINE__, image_path, fdimg);
        return -1;
    }

    fstat(fdimg, &stat_buf);
    total_size = stat_buf.st_size;
    
    while (offset < total_size) {
        len = total_size - offset;
        if (sendfile(fd, fdimg, &offset, len) != 0) {
            close(fdimg);
            return -1;
        }
        offset += len;
    }
 
    close(fdimg);
    return 0;
}

int web_t::server(int sock)
{
    char path_head[PATH_NAME_SZ] = {0};
    char buffer[30000] = {0};
    long valread;
    char *parse_string_method = NULL;
    char *parse_string = NULL;
    char *copy = NULL;
    char *parse_ext = NULL;
    char *copy_head = NULL; 
    snprintf(path_head, sizeof(path_head), "%s", m_head);
    do {    
        valread = read(sock , buffer, 30000);
        if (valread <= 0) {
            wifi_util_dbg_print(WIFI_CTRL,
                "%s:%d: read failed or empty\n", __func__, __LINE__);
            break;
        }
	buffer[valread] = '\0';
	parse_string_method = parse_method(buffer, " ");
        if (!parse_string_method ) {  //Try to get the path which the client ask for
            wifi_util_error_print(WIFI_CTRL,"%s:%d:\n", __func__, __LINE__);
            break;
        }
        parse_string = parse(buffer, " ");
        if (!parse_string) {
            wifi_util_error_print(WIFI_CTRL,"%s:%d:\n", __func__, __LINE__);
            break;
        }

        copy = (char *)malloc(strlen(parse_string) + 1);
        if (!copy) break;
        strcpy(copy, parse_string);

        parse_ext = parse(copy, ".");  // get the file extension such as JPG, jpg
        copy_head = (char *)malloc(strlen(m_http_header) +200);
        if (!copy_head) break;
	strcpy(copy_head, m_http_header);
        /* Handle URLs without extension (e.g. /api/status) */
        if (!parse_ext || strlen(parse_ext) < 2) {
            strcat(path_head, "/index.html");
            wifi_util_error_print(WIFI_CTRL,"%s:%d:\n", __func__, __LINE__);
            strcat(copy_head, "Content-Type: text/html\r\n\r\n");
            send_message(sock, path_head, copy_head);
            break;
        }

        if(parse_string_method[0] == 'G' && parse_string_method[1] == 'E' && parse_string_method[2] == 'T'){
        //https://developer.mozilla.org/en-US/docs/Web/HTTP/Basics_of_HTTP/MIME_types/Common_types
            if (strlen(parse_string) <= 1) {
                //case that the parse_string = "/"  --> Send index.html file
               //write(new_socket , httpHeader , strlen(httpHeader));
            
                strcat(path_head, "/index.html");
                strcat(copy_head, "Content-Type: text/html\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }
            else if ((parse_ext[0] == 'j' && parse_ext[1] == 'p' && parse_ext[2] == 'g') || (parse_ext[0] == 'J' && parse_ext[1] == 'P' && parse_ext[2] == 'G'))
            {
                //send image to client
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: image/jpeg\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }
            else if (parse_ext[0] == 'i' && parse_ext[1] == 'c' && parse_ext[2] == 'o')
            {
                //https://www.cisco.com/c/en/us/support/docs/security/web-security-appliance/117995-qna-wsa-00.html
                strcat(path_head, "/img/favicon.png");
                strcat(copy_head, "Content-Type: image/vnd.microsoft.icon\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }
            else if (parse_ext[0] == 't' && parse_ext[1] == 't' && parse_ext[2] == 'f')
            {
                //font type, to display icon from FontAwesome
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: font/ttf\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }
            else if (parse_ext[strlen(parse_ext)-2] == 'j' && parse_ext[strlen(parse_ext)-1] == 's')
            {
                //javascript
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: text/javascript\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }
            else if (parse_ext[strlen(parse_ext)-3] == 'c' && parse_ext[strlen(parse_ext)-2] == 's' && parse_ext[strlen(parse_ext)-1] == 's')
            {
                 //css
                 strcat(path_head, parse_string);
                 strcat(copy_head, "Content-Type: text/css\r\n\r\n");
                 send_message(sock, path_head, copy_head);
            }
            else if (parse_ext[0] == 'w' && parse_ext[1] == 'o' && parse_ext[2] == 'f')
            {
                //Web Open Font Format woff and woff2
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: font/woff\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }
            else if (parse_ext[0] == 'm' && parse_ext[1] == '3' && parse_ext[2] == 'u' && parse_ext[3] == '8')
            {
                //Web Open m3u8
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: application/vnd.apple.mpegurl\r\n\r\n");
                send_message(sock, path_head, copy_head);
            }   
            else if (parse_ext[0] == 't' && parse_ext[1] == 's')
            {
                //Web Open ts
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: video/mp2t\r\n\r\n");
                wifi_util_dbg_print(WIFI_CTRL,": %s:%d \n",__func__,__LINE__);
                send_message(sock, path_head, copy_head);
            }
            else {
                //send other file
                strcat(path_head, parse_string);
                strcat(copy_head, "Content-Type: text/plain\r\n\r\n");
               send_message(sock, path_head, copy_head);
            
            }
        
        }
        else if (parse_string_method[0] == 'P' && parse_string_method[1] == 'O' && parse_string_method[2] == 'S' && parse_string_method[3] == 'T')    {
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) body += 4; // skip the delimiter
            else body = buffer;

            char downlink[256] = {0};
            char uplink[256] = {0};
            char aggregate[256] = {0};

            // Simple parsing for urlencoded body
            char *d = strstr(body, "downlink=");
            char *u = strstr(body, "uplink=");
            char *a = strstr(body, "aggregate=");
            if (d) {
                sscanf(d, "downlink=%255[^&]", downlink);
            }
            if (u) {
                sscanf(u, "uplink=%255[^&]", uplink);
            }
            if (a) {
                sscanf(a, "aggregate=%255[^&]", aggregate);
            }
            // Placeholder: print received values
            wifi_util_dbg_print(WIFI_CTRL,"Received Link Parameters:\n");
            wifi_util_dbg_print(WIFI_CTRL,"  Downlink: %s\n", downlink);
            wifi_util_dbg_print(WIFI_CTRL,"  Uplink: %s\n", uplink);
            wifi_util_dbg_print(WIFI_CTRL,"  Aggregate: %s\n", aggregate);
 
            // Switch/case placeholder logic for combinations
            // (In real code, parse and split values as needed)
            if (strlen(downlink) && strlen(uplink) && strlen(aggregate)) {
             // All selected
                wifi_util_dbg_print(WIFI_CTRL,"[SWITCH] All parameters selected\n");
            } else if (strlen(downlink) && strlen(uplink)) {
                wifi_util_dbg_print(WIFI_CTRL,"[SWITCH] Downlink and Uplink selected\n");
            } else if (strlen(downlink)) {
                wifi_util_dbg_print(WIFI_CTRL,"[SWITCH] Only Downlink selected\n");
            } else if (strlen(uplink)) {
                wifi_util_dbg_print(WIFI_CTRL,"[SWITCH] Only Uplink selected\n");
            } else if (strlen(aggregate)) {
                wifi_util_dbg_print(WIFI_CTRL,"[SWITCH] Only Aggregate selected\n");
            } else {
                wifi_util_dbg_print(WIFI_CTRL,"[SWITCH] No parameters selected\n");
            }
            strcat(copy_head, "Content-Type: text/plain \r\n\r\n");
            strcat(copy_head, "Received Link Parameters\n");
            strcat(copy_head, "Downlink: "); strcat(copy_head, downlink); strcat(copy_head, "\n");
            strcat(copy_head, "Uplink: "); strcat(copy_head, uplink); strcat(copy_head, "\n");
            strcat(copy_head, "Aggregate: "); strcat(copy_head, aggregate); strcat(copy_head, "\n");
            write(sock, copy_head, strlen(copy_head));
        }
            
    } while(0);

    if (copy) free(copy);
    if (copy_head) free(copy_head);
    if (parse_string_method) free(parse_string_method);
    if (parse_string) free(parse_string);
    
    return 0;
}

void *web_t::server(void *arg)
{
    web_args_t *args = (web_args_t *)arg;
    web_t *web = args->web;
    
    web->server(args->sock);
    close(args->sock);   // close socket
    free(args);
    return NULL;
}



int web_t::run()
{
    pthread_attr_t attr;
   size_t stack_size = 1024 * 1024; // Example: 1 MB stack size
    int new_sock;
    pthread_t client_tid;
    int addrlen = sizeof(m_address);
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    
    while (m_exit == false) {
        if ((new_sock = accept(m_server_fd, (struct sockaddr *)&m_address, (socklen_t*)&addrlen))<0) {
            if (m_exit) {
                wifi_util_error_print(WIFI_APPS,
                "%s:%d: m_exit : %d\n",
                __func__, __LINE__,m_exit);
               break;
            }
            wifi_util_error_print(WIFI_APPS,
                "%s:%d: Error in accept: %d\n",
                __func__, __LINE__, errno);
            continue;
        }
        web_args_t *args = (web_args_t *)malloc(sizeof(web_args_t));
        if (!args) {
            close(new_sock);
           continue;
        } 
        args->web = this;
        args->sock = new_sock;
        
        if (pthread_create(&client_tid, NULL, server, args) != 0) {
            wifi_util_error_print(WIFI_APPS,"%s:%d: Failed to start child thread\n", __func__, __LINE__);
            close(new_sock);
            free(args);
            return -1;
        }
        pthread_detach(client_tid); 
    }
    pthread_attr_destroy(&attr);
    return 0;
}

void *web_t::run(void *arg)
{
    
    web_t *web = (web_t *)arg;
    
    web->run();
    
    return NULL;
}

void web_t::stop()
{
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    m_exit = true;

    if (m_server_fd >= 0) {
        shutdown(m_server_fd, SHUT_RDWR);  // wake accept()
        close(m_server_fd);
        m_server_fd = -1;
    }
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
    if (m_run_tid != 0) {
        pthread_join(m_run_tid, nullptr);
        m_run_tid = 0;
    }
    wifi_util_info_print(WIFI_APPS,"%s:%d\n",__func__,__LINE__);
}

int web_t::start()
{
    
    pthread_t tid;
    pthread_attr_t attr;
    size_t stack_size = 1024 * 1024; // Example: 1 MB stack size
    m_run_tid = 0; 
    if (init() != 0) {
        wifi_util_error_print(WIFI_APPS,"%s:%d: Error in initializing server\n", __func__, __LINE__);
        return -1;
    }
    
    // Creating socket file descriptor
    if ((m_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        wifi_util_error_print(WIFI_APPS,"%s:%d: Error in cfreating socket: %d\n", __func__, __LINE__, errno);
        return -1;
    }
    int opt = 1;
    if (setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR,&opt, sizeof(opt)) < 0) {
        wifi_util_error_print(WIFI_APPS, "%s:%d: setsockopt(SO_REUSEADDR) failed: %d\n",
            __func__, __LINE__, errno);
        close(m_server_fd);
        return -1;
    }
    if (bind(m_server_fd, (struct sockaddr *)&m_address, sizeof(m_address))<0) {
        wifi_util_error_print(WIFI_APPS,"%s:%d: Error in binding: %d\n", __func__, __LINE__, errno);
        close(m_server_fd);
        return -1;
    }
    
    if (listen(m_server_fd, 10) < 0) {
        wifi_util_error_print(WIFI_APPS,"%s:%d: Error in listen: %d\n", __func__, __LINE__, errno);
        return -1;
    }
    
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    
    if (pthread_create(&tid, &attr, run, this) != 0) {
        pthread_attr_destroy(&attr);
        wifi_util_error_print(WIFI_APPS,"%s:%d: Failed to start child thread\n", __func__, __LINE__);
        return -1;
    }
    m_run_tid = tid;
    return 0;
}

void web_t::deinit()
{
    
}

int web_t::init()
{
    
    snprintf(m_http_header, sizeof(m_http_header), "HTTP/1.1 200 Ok\r\n");
    m_exit = false;
    
    m_address.sin_family = AF_INET;
    m_address.sin_addr.s_addr = INADDR_ANY;
    m_address.sin_port = htons(PORT);

    memset(m_address.sin_zero, '\0', sizeof m_address.sin_zero);
    
    return 0;
}

web_t::web_t(const char *path)
{
    wifi_util_info_print(WIFI_CTRL, "%s:%d path=%s\n", __func__, __LINE__, path);
    strncpy(m_head, path, strlen(path) + 1);
}

web_t::~web_t()
{
    
}

