/*
 * Implementation of the TCP relay client part of Tox.
 */

/*
 * Copyright © 2016-2017 The TokTok team.
 * Copyright © 2014 Tox project.
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Copyright (c) 2019 ioeXNetwork
 *
 * This file is part of Tox, the free peer to peer instant messenger.
 *
 * Tox is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Tox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "TCP_client.h"

#if defined(ELASTOS_BUILD)
#include "network.h"
#endif

#include "util.h"

#if !defined(_WIN32) && !defined(__WIN32__) && !defined (WIN32)
#include <sys/ioctl.h>
#endif

/* return 1 on success
 * return 0 on failure
 */
static int connect_sock_to(Socket sock, IP_Port ip_port, TCP_Proxy_Info *proxy_info)
{
    if (proxy_info->proxy_type != TCP_PROXY_NONE) {
        ip_port = proxy_info->ip_port;
    }

    /* nonblocking socket, connect will never return success */
    net_connect(sock, ip_port);
    return 1;
}

/* return 1 on success.
 * return 0 on failure.
 */
static int proxy_http_generate_connection_request(TCP_Client_Connection *TCP_conn)
{
    char one[] = "CONNECT ";
    char two[] = " HTTP/1.1\nHost: ";
    char three[] = "\r\n\r\n";

    char ip[INET6_ADDRSTRLEN];

    if (!ip_parse_addr(&TCP_conn->ip_port.ip, ip, sizeof(ip))) {
        return 0;
    }

    const uint16_t port = net_ntohs(TCP_conn->ip_port.port);
    const int written = snprintf((char *)TCP_conn->last_packet, MAX_PACKET_SIZE, "%s%s:%hu%s%s:%hu%s", one, ip, port, two,
                                 ip, port, three);

    if (written < 0 || MAX_PACKET_SIZE < written) {
        return 0;
    }

    TCP_conn->last_packet_length = written;
    TCP_conn->last_packet_sent = 0;

    return 1;
}

/* return 1 on success.
 * return 0 if no data received.
 * return -1 on failure (connection refused).
 */
static int proxy_http_read_connection_response(TCP_Client_Connection *TCP_conn)
{
    char success[] = "200";
    uint8_t data[16]; // draining works the best if the length is a power of 2

    int ret = read_TCP_packet(TCP_conn->sock, data, sizeof(data) - 1);

    if (ret == -1) {
        return 0;
    }

    data[sizeof(data) - 1] = 0;

    if (strstr((char *)data, success)) {
        // drain all data
        unsigned int data_left = TCP_socket_data_recv_buffer(TCP_conn->sock);

        if (data_left) {
            VLA(uint8_t, temp_data, data_left);
            read_TCP_packet(TCP_conn->sock, temp_data, data_left);
        }

        return 1;
    }

    return -1;
}

static void proxy_socks5_generate_handshake(TCP_Client_Connection *TCP_conn)
{
    TCP_conn->last_packet[0] = 5; /* SOCKSv5 */
    TCP_conn->last_packet[1] = 1; /* number of authentication methods supported */
    TCP_conn->last_packet[2] = 0; /* No authentication */

    TCP_conn->last_packet_length = 3;
    TCP_conn->last_packet_sent = 0;
}

/* return 1 on success.
 * return 0 if no data received.
 * return -1 on failure (connection refused).
 */
static int socks5_read_handshake_response(TCP_Client_Connection *TCP_conn)
{
    uint8_t data[2];
    int ret = read_TCP_packet(TCP_conn->sock, data, sizeof(data));

    if (ret == -1) {
        return 0;
    }

    if (data[0] == 5 && data[1] == 0) { // TODO(irungentoo): magic numbers
        return 1;
    }

    return -1;
}

static void proxy_socks5_generate_connection_request(TCP_Client_Connection *TCP_conn)
{
    TCP_conn->last_packet[0] = 5; /* SOCKSv5 */
    TCP_conn->last_packet[1] = 1; /* command code: establish a TCP/IP stream connection */
    TCP_conn->last_packet[2] = 0; /* reserved, must be 0 */
    uint16_t length = 3;

    if (TCP_conn->ip_port.ip.family == AF_INET) {
        TCP_conn->last_packet[3] = 1; /* IPv4 address */
        ++length;
        memcpy(TCP_conn->last_packet + length, TCP_conn->ip_port.ip.ip4.uint8, sizeof(IP4));
        length += sizeof(IP4);
    } else {
        TCP_conn->last_packet[3] = 4; /* IPv6 address */
        ++length;
        memcpy(TCP_conn->last_packet + length, TCP_conn->ip_port.ip.ip6.uint8, sizeof(IP6));
        length += sizeof(IP6);
    }

    memcpy(TCP_conn->last_packet + length, &TCP_conn->ip_port.port, sizeof(uint16_t));
    length += sizeof(uint16_t);

    TCP_conn->last_packet_length = length;
    TCP_conn->last_packet_sent = 0;
}

/* return 1 on success.
 * return 0 if no data received.
 * return -1 on failure (connection refused).
 */
static int proxy_socks5_read_connection_response(TCP_Client_Connection *TCP_conn)
{
    if (TCP_conn->ip_port.ip.family == AF_INET) {
        uint8_t data[4 + sizeof(IP4) + sizeof(uint16_t)];
        int ret = read_TCP_packet(TCP_conn->sock, data, sizeof(data));

        if (ret == -1) {
            return 0;
        }

        if (data[0] == 5 && data[1] == 0) {
            return 1;
        }
    } else {
        uint8_t data[4 + sizeof(IP6) + sizeof(uint16_t)];
        int ret = read_TCP_packet(TCP_conn->sock, data, sizeof(data));

        if (ret == -1) {
            return 0;
        }

        if (data[0] == 5 && data[1] == 0) {
            return 1;
        }
    }

    return -1;
}

/* return 0 on success.
 * return -1 on failure.
 */
static int generate_handshake(TCP_Client_Connection *TCP_conn)
{
    uint8_t plain[CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_NONCE_SIZE];
    crypto_new_keypair(plain, TCP_conn->temp_secret_key);
    random_nonce(TCP_conn->sent_nonce);
    memcpy(plain + CRYPTO_PUBLIC_KEY_SIZE, TCP_conn->sent_nonce, CRYPTO_NONCE_SIZE);
    memcpy(TCP_conn->last_packet, TCP_conn->self_public_key, CRYPTO_PUBLIC_KEY_SIZE);
    random_nonce(TCP_conn->last_packet + CRYPTO_PUBLIC_KEY_SIZE);
    int len = encrypt_data_symmetric(TCP_conn->shared_key, TCP_conn->last_packet + CRYPTO_PUBLIC_KEY_SIZE, plain,
                                     sizeof(plain), TCP_conn->last_packet + CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_NONCE_SIZE);

    if (len != sizeof(plain) + CRYPTO_MAC_SIZE) {
        return -1;
    }

    TCP_conn->last_packet_length = CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_NONCE_SIZE + sizeof(plain) + CRYPTO_MAC_SIZE;
    TCP_conn->last_packet_sent = 0;
    return 0;
}

/* data must be of length TCP_SERVER_HANDSHAKE_SIZE
 *
 * return 0 on success.
 * return -1 on failure.
 */
static int handle_handshake(TCP_Client_Connection *TCP_conn, const uint8_t *data)
{
    uint8_t plain[CRYPTO_PUBLIC_KEY_SIZE + CRYPTO_NONCE_SIZE];
    int len = decrypt_data_symmetric(TCP_conn->shared_key, data, data + CRYPTO_NONCE_SIZE,
                                     TCP_SERVER_HANDSHAKE_SIZE - CRYPTO_NONCE_SIZE, plain);

    if (len != sizeof(plain)) {
        return -1;
    }

    memcpy(TCP_conn->recv_nonce, plain + CRYPTO_PUBLIC_KEY_SIZE, CRYPTO_NONCE_SIZE);
    encrypt_precompute(plain, TCP_conn->temp_secret_key, TCP_conn->shared_key);
    crypto_memzero(TCP_conn->temp_secret_key, CRYPTO_SECRET_KEY_SIZE);
    return 0;
}

/* return 0 if pending data was sent completely
 * return -1 if it wasn't
 */
static int client_send_pending_data_nonpriority(TCP_Client_Connection *con)
{
    if (con->last_packet_length == 0) {
        return 0;
    }

    uint16_t left = con->last_packet_length - con->last_packet_sent;
    int len = send(con->sock, (const char *)(con->last_packet + con->last_packet_sent), left, MSG_NOSIGNAL);

    if (len <= 0) {
        return -1;
    }

    if (len == left) {
        con->last_packet_length = 0;
        con->last_packet_sent = 0;
        return 0;
    }

    con->last_packet_sent += len;
    return -1;
}

/* return 0 if pending data was sent completely
 * return -1 if it wasn't
 */
static int client_send_pending_data(TCP_Client_Connection *con)
{
    /* finish sending current non-priority packet */
    if (client_send_pending_data_nonpriority(con) == -1) {
        return -1;
    }

    TCP_Priority_List *p = con->priority_queue_start;

    while (p) {
        uint16_t left = p->size - p->sent;
        int len = send(con->sock, (const char *)(p->data + p->sent), left, MSG_NOSIGNAL);

        if (len != left) {
            if (len > 0) {
                p->sent += len;
            }

            break;
        }

        TCP_Priority_List *pp = p;
        p = p->next;
        free(pp);
    }

    con->priority_queue_start = p;

    if (!p) {
        con->priority_queue_end = NULL;
        return 0;
    }

    return -1;
}

/* return 0 on failure (only if malloc fails)
 * return 1 on success
 */
static bool client_add_priority(TCP_Client_Connection *con, const uint8_t *packet, uint16_t size, uint16_t sent)
{
    TCP_Priority_List *p = con->priority_queue_end;
    TCP_Priority_List *new_list = (TCP_Priority_List *)malloc(sizeof(TCP_Priority_List) + size);

    if (!new_list) {
        return 0;
    }

    new_list->next = NULL;
    new_list->size = size;
    new_list->sent = sent;
    memcpy(new_list->data, packet, size);

    if (p) {
        p->next = new_list;
    } else {
        con->priority_queue_start = new_list;
    }

    con->priority_queue_end = new_list;
    return 1;
}

static void wipe_priority_list(TCP_Client_Connection *con)
{
    TCP_Priority_List *p = con->priority_queue_start;

    while (p) {
        TCP_Priority_List *pp = p;
        p = p->next;
        free(pp);
    }
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int write_packet_TCP_client_secure_connection(TCP_Client_Connection *con, const uint8_t *data, uint16_t length,
        bool priority)
{
    if (length + CRYPTO_MAC_SIZE > MAX_PACKET_SIZE) {
        return -1;
    }

    bool sendpriority = 1;

    if (client_send_pending_data(con) == -1) {
        if (priority) {
            sendpriority = 0;
        } else {
            return 0;
        }
    }

    ELASTOS_VLA(uint8_t, packet, sizeof(uint16_t) + length + CRYPTO_MAC_SIZE);

    uint16_t c_length = net_htons(length + CRYPTO_MAC_SIZE);
    memcpy(packet, &c_length, sizeof(uint16_t));
    int len = encrypt_data_symmetric(con->shared_key, con->sent_nonce, data, length, packet + sizeof(uint16_t));

    if ((unsigned int)len != (ELASTOS_SIZEOF_VLA(packet) - sizeof(uint16_t))) {
        return -1;
    }

    ela_magic_set(packet);
    if (priority) {
        len = sendpriority ? send(con->sock, (const char *)ela_rewind(packet), ela_rewind_sizeof(packet), MSG_NOSIGNAL) : 0;

        if (len <= 0) {
            len = 0;
        }

        increment_nonce(con->sent_nonce);

        if ((unsigned int)len == ela_rewind_sizeof(packet)) {
            return 1;
        }

        return client_add_priority(con, ela_rewind(packet), ela_rewind_sizeof(packet), len);
    }

    len = send(con->sock, (const char *)ela_rewind(packet), ela_rewind_sizeof(packet), MSG_NOSIGNAL);

    if (len <= 0) {
        return 0;
    }

    increment_nonce(con->sent_nonce);

    if ((unsigned int)len == ela_rewind_sizeof(packet)) {
        return 1;
    }

    memcpy(con->last_packet, ela_rewind(packet), ela_rewind_sizeof(packet));
    con->last_packet_length = ela_rewind_sizeof(packet);
    con->last_packet_sent = len;
    return 1;
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
int send_routing_request(TCP_Client_Connection *con, uint8_t *public_key)
{
    uint8_t packet[1 + CRYPTO_PUBLIC_KEY_SIZE];
    packet[0] = TCP_PACKET_ROUTING_REQUEST;
    memcpy(packet + 1, public_key, CRYPTO_PUBLIC_KEY_SIZE);
    return write_packet_TCP_client_secure_connection(con, packet, sizeof(packet), 1);
}

void routing_response_handler(TCP_Client_Connection *con, int (*response_callback)(void *object, uint8_t connection_id,
                              const uint8_t *public_key), void *object)
{
    con->response_callback = response_callback;
    con->response_callback_object = object;
}

void routing_status_handler(TCP_Client_Connection *con, int (*status_callback)(void *object, uint32_t number,
                            uint8_t connection_id, uint8_t status), void *object)
{
    con->status_callback = status_callback;
    con->status_callback_object = object;
}

static int tcp_send_ping_response(TCP_Client_Connection *con);
static int tcp_send_ping_request(TCP_Client_Connection *con);

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure.
 */
int send_data(TCP_Client_Connection *con, uint8_t con_id, const uint8_t *data, uint16_t length)
{
    if (con_id >= NUM_CLIENT_CONNECTIONS) {
        return -1;
    }

    if (con->connections[con_id].status != 2) {
        return -1;
    }

    if (tcp_send_ping_response(con) == 0 || tcp_send_ping_request(con) == 0) {
        return 0;
    }

    VLA(uint8_t, packet, 1 + length);
    packet[0] = con_id + NUM_RESERVED_PORTS;
    memcpy(packet + 1, data, length);
    return write_packet_TCP_client_secure_connection(con, packet, SIZEOF_VLA(packet), 0);
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure.
 */
int send_oob_packet(TCP_Client_Connection *con, const uint8_t *public_key, const uint8_t *data, uint16_t length)
{
    if (length == 0 || length > TCP_MAX_OOB_DATA_LENGTH) {
        return -1;
    }

    VLA(uint8_t, packet, 1 + CRYPTO_PUBLIC_KEY_SIZE + length);
    packet[0] = TCP_PACKET_OOB_SEND;
    memcpy(packet + 1, public_key, CRYPTO_PUBLIC_KEY_SIZE);
    memcpy(packet + 1 + CRYPTO_PUBLIC_KEY_SIZE, data, length);
    return write_packet_TCP_client_secure_connection(con, packet, SIZEOF_VLA(packet), 0);
}


/* Set the number that will be used as an argument in the callbacks related to con_id.
 *
 * When not set by this function, the number is ~0.
 *
 * return 0 on success.
 * return -1 on failure.
 */
int set_tcp_connection_number(TCP_Client_Connection *con, uint8_t con_id, uint32_t number)
{
    if (con_id >= NUM_CLIENT_CONNECTIONS) {
        return -1;
    }

    if (con->connections[con_id].status == 0) {
        return -1;
    }

    con->connections[con_id].number = number;
    return 0;
}

void routing_data_handler(TCP_Client_Connection *con, int (*data_callback)(void *object, uint32_t number,
                          uint8_t connection_id, const uint8_t *data, uint16_t length, void *userdata), void *object)
{
    con->data_callback = data_callback;
    con->data_callback_object = object;
}

void oob_data_handler(TCP_Client_Connection *con, int (*oob_data_callback)(void *object, const uint8_t *public_key,
                      const uint8_t *data, uint16_t length, void *userdata), void *object)
{
    con->oob_data_callback = oob_data_callback;
    con->oob_data_callback_object = object;
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int client_send_disconnect_notification(TCP_Client_Connection *con, uint8_t id)
{
    uint8_t packet[1 + 1];
    packet[0] = TCP_PACKET_DISCONNECT_NOTIFICATION;
    packet[1] = id;
    return write_packet_TCP_client_secure_connection(con, packet, sizeof(packet), 1);
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int tcp_send_ping_request(TCP_Client_Connection *con)
{
    if (!con->ping_request_id) {
        return 1;
    }

    uint8_t packet[1 + sizeof(uint64_t)];
    packet[0] = TCP_PACKET_PING;
    memcpy(packet + 1, &con->ping_request_id, sizeof(uint64_t));
    int ret;

    if ((ret = write_packet_TCP_client_secure_connection(con, packet, sizeof(packet), 1)) == 1) {
        con->ping_request_id = 0;
    }

    return ret;
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
static int tcp_send_ping_response(TCP_Client_Connection *con)
{
    if (!con->ping_response_id) {
        return 1;
    }

    uint8_t packet[1 + sizeof(uint64_t)];
    packet[0] = TCP_PACKET_PONG;
    memcpy(packet + 1, &con->ping_response_id, sizeof(uint64_t));
    int ret;

    if ((ret = write_packet_TCP_client_secure_connection(con, packet, sizeof(packet), 1)) == 1) {
        con->ping_response_id = 0;
    }

    return ret;
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
int send_disconnect_request(TCP_Client_Connection *con, uint8_t con_id)
{
    if (con_id >= NUM_CLIENT_CONNECTIONS) {
        return -1;
    }

    con->connections[con_id].status = 0;
    con->connections[con_id].number = 0;
    return client_send_disconnect_notification(con, con_id + NUM_RESERVED_PORTS);
}

/* return 1 on success.
 * return 0 if could not send packet.
 * return -1 on failure (connection must be killed).
 */
int send_onion_request(TCP_Client_Connection *con, const uint8_t *data, uint16_t length)
{
    VLA(uint8_t, packet, 1 + length);
    packet[0] = TCP_PACKET_ONION_REQUEST;
    memcpy(packet + 1, data, length);
    return write_packet_TCP_client_secure_connection(con, packet, SIZEOF_VLA(packet), 0);
}

void onion_response_handler(TCP_Client_Connection *con, int (*onion_callback)(void *object, const uint8_t *data,
                            uint16_t length, void *userdata), void *object)
{
    con->onion_callback = onion_callback;
    con->onion_callback_object = object;
}

/* Create new TCP connection to ip_port/public_key
 */
TCP_Client_Connection *new_TCP_connection(IP_Port ip_port, const uint8_t *public_key, const uint8_t *self_public_key,
        const uint8_t *self_secret_key, TCP_Proxy_Info *proxy_info)
{
    if (networking_at_startup() != 0) {
        return NULL;
    }

    if (ip_port.ip.family != AF_INET && ip_port.ip.family != AF_INET6) {
        return NULL;
    }

    uint8_t family = ip_port.ip.family;

    TCP_Proxy_Info default_proxyinfo;

    if (proxy_info == NULL) {
        default_proxyinfo.proxy_type = TCP_PROXY_NONE;
        proxy_info = &default_proxyinfo;
    }

    if (proxy_info->proxy_type != TCP_PROXY_NONE) {
        family = proxy_info->ip_port.ip.family;
    }

    Socket sock = net_socket(family, TOX_SOCK_STREAM, TOX_PROTO_TCP);

    if (!sock_valid(sock)) {
        return NULL;
    }

    if (!set_socket_nosigpipe(sock)) {
        kill_sock(sock);
        return 0;
    }

    if (!(set_socket_nonblock(sock) && connect_sock_to(sock, ip_port, proxy_info))) {
        kill_sock(sock);
        return NULL;
    }

    TCP_Client_Connection *temp = (TCP_Client_Connection *)calloc(sizeof(TCP_Client_Connection), 1);

    if (temp == NULL) {
        kill_sock(sock);
        return NULL;
    }

    temp->sock = sock;
    memcpy(temp->public_key, public_key, CRYPTO_PUBLIC_KEY_SIZE);
    memcpy(temp->self_public_key, self_public_key, CRYPTO_PUBLIC_KEY_SIZE);
    encrypt_precompute(temp->public_key, self_secret_key, temp->shared_key);
    temp->ip_port = ip_port;
    temp->proxy_info = *proxy_info;

    switch (proxy_info->proxy_type) {
        case TCP_PROXY_HTTP:
            temp->status = TCP_CLIENT_PROXY_HTTP_CONNECTING;
            proxy_http_generate_connection_request(temp);
            break;

        case TCP_PROXY_SOCKS5:
            temp->status = TCP_CLIENT_PROXY_SOCKS5_CONNECTING;
            proxy_socks5_generate_handshake(temp);
            break;

        case TCP_PROXY_NONE:
            temp->status = TCP_CLIENT_CONNECTING;

            if (generate_handshake(temp) == -1) {
                kill_sock(sock);
                free(temp);
                return NULL;
            }

            break;
    }

    temp->kill_at = unix_time() + TCP_CONNECTION_TIMEOUT;

    return temp;
}

/* return 0 on success
 * return -1 on failure
 */
static int handle_TCP_client_packet(TCP_Client_Connection *conn, const uint8_t *data, uint16_t length, void *userdata)
{
    if (length <= 1) {
        return -1;
    }

    switch (data[0]) {
        case TCP_PACKET_ROUTING_RESPONSE: {
            if (length != 1 + 1 + CRYPTO_PUBLIC_KEY_SIZE) {
                return -1;
            }

            if (data[1] < NUM_RESERVED_PORTS) {
                return 0;
            }

            uint8_t con_id = data[1] - NUM_RESERVED_PORTS;

            if (conn->connections[con_id].status != 0) {
                return 0;
            }

            conn->connections[con_id].status = 1;
            conn->connections[con_id].number = ~0;
            memcpy(conn->connections[con_id].public_key, data + 2, CRYPTO_PUBLIC_KEY_SIZE);

            if (conn->response_callback) {
                conn->response_callback(conn->response_callback_object, con_id, conn->connections[con_id].public_key);
            }

            return 0;
        }

        case TCP_PACKET_CONNECTION_NOTIFICATION: {
            if (length != 1 + 1) {
                return -1;
            }

            if (data[1] < NUM_RESERVED_PORTS) {
                return -1;
            }

            uint8_t con_id = data[1] - NUM_RESERVED_PORTS;

            if (conn->connections[con_id].status != 1) {
                return 0;
            }

            conn->connections[con_id].status = 2;

            if (conn->status_callback) {
                conn->status_callback(conn->status_callback_object, conn->connections[con_id].number, con_id,
                                      conn->connections[con_id].status);
            }

            return 0;
        }

        case TCP_PACKET_DISCONNECT_NOTIFICATION: {
            if (length != 1 + 1) {
                return -1;
            }

            if (data[1] < NUM_RESERVED_PORTS) {
                return -1;
            }

            uint8_t con_id = data[1] - NUM_RESERVED_PORTS;

            if (conn->connections[con_id].status == 0) {
                return 0;
            }

            if (conn->connections[con_id].status != 2) {
                return 0;
            }

            conn->connections[con_id].status = 1;

            if (conn->status_callback) {
                conn->status_callback(conn->status_callback_object, conn->connections[con_id].number, con_id,
                                      conn->connections[con_id].status);
            }

            return 0;
        }

        case TCP_PACKET_PING: {
            if (length != 1 + sizeof(uint64_t)) {
                return -1;
            }

            uint64_t ping_id;
            memcpy(&ping_id, data + 1, sizeof(uint64_t));
            conn->ping_response_id = ping_id;
            tcp_send_ping_response(conn);
            return 0;
        }

        case TCP_PACKET_PONG: {
            if (length != 1 + sizeof(uint64_t)) {
                return -1;
            }

            uint64_t ping_id;
            memcpy(&ping_id, data + 1, sizeof(uint64_t));

            if (ping_id) {
                if (ping_id == conn->ping_id) {
                    conn->ping_id = 0;
                }

                return 0;
            }

            return -1;
        }

        case TCP_PACKET_OOB_RECV: {
            if (length <= 1 + CRYPTO_PUBLIC_KEY_SIZE) {
                return -1;
            }

            if (conn->oob_data_callback) {
                conn->oob_data_callback(conn->oob_data_callback_object, data + 1, data + 1 + CRYPTO_PUBLIC_KEY_SIZE,
                                        length - (1 + CRYPTO_PUBLIC_KEY_SIZE), userdata);
            }

            return 0;
        }

        case TCP_PACKET_ONION_RESPONSE: {
            conn->onion_callback(conn->onion_callback_object, data + 1, length - 1, userdata);
            return 0;
        }

        default: {
            if (data[0] < NUM_RESERVED_PORTS) {
                return -1;
            }

            uint8_t con_id = data[0] - NUM_RESERVED_PORTS;

            if (conn->data_callback) {
                conn->data_callback(conn->data_callback_object, conn->connections[con_id].number, con_id, data + 1, length - 1,
                                    userdata);
            }
        }
    }

    return 0;
}

static int do_confirmed_TCP(TCP_Client_Connection *conn, void *userdata)
{
    client_send_pending_data(conn);
    tcp_send_ping_response(conn);
    tcp_send_ping_request(conn);

    uint8_t packet[MAX_PACKET_SIZE];
    int len;

    if (is_timeout(conn->last_pinged, TCP_PING_FREQUENCY)) {
        uint64_t ping_id = random_64b();

        if (!ping_id) {
            ++ping_id;
        }

        conn->ping_request_id = conn->ping_id = ping_id;
        tcp_send_ping_request(conn);
        conn->last_pinged = unix_time();
    }

    if (conn->ping_id && is_timeout(conn->last_pinged, TCP_PING_TIMEOUT)) {
        conn->status = TCP_CLIENT_DISCONNECTED;
        return 0;
    }

    while ((len = read_packet_TCP_secure_connection(conn->sock, &conn->next_packet_length, conn->shared_key,
                  conn->recv_nonce, packet, sizeof(packet)))) {
        if (len == -1) {
            conn->status = TCP_CLIENT_DISCONNECTED;
            break;
        }

        if (handle_TCP_client_packet(conn, packet, len, userdata) == -1) {
            conn->status = TCP_CLIENT_DISCONNECTED;
            break;
        }
    }

    return 0;
}

/* Run the TCP connection
 */
void do_TCP_connection(TCP_Client_Connection *TCP_connection, void *userdata)
{
    unix_time_update();

    if (TCP_connection->status == TCP_CLIENT_DISCONNECTED) {
        return;
    }

    if (TCP_connection->status == TCP_CLIENT_PROXY_HTTP_CONNECTING) {
        if (client_send_pending_data(TCP_connection) == 0) {
            int ret = proxy_http_read_connection_response(TCP_connection);

            if (ret == -1) {
                TCP_connection->kill_at = 0;
                TCP_connection->status = TCP_CLIENT_DISCONNECTED;
            }

            if (ret == 1) {
                generate_handshake(TCP_connection);
                TCP_connection->status = TCP_CLIENT_CONNECTING;
            }
        }
    }

    if (TCP_connection->status == TCP_CLIENT_PROXY_SOCKS5_CONNECTING) {
        if (client_send_pending_data(TCP_connection) == 0) {
            int ret = socks5_read_handshake_response(TCP_connection);

            if (ret == -1) {
                TCP_connection->kill_at = 0;
                TCP_connection->status = TCP_CLIENT_DISCONNECTED;
            }

            if (ret == 1) {
                proxy_socks5_generate_connection_request(TCP_connection);
                TCP_connection->status = TCP_CLIENT_PROXY_SOCKS5_UNCONFIRMED;
            }
        }
    }

    if (TCP_connection->status == TCP_CLIENT_PROXY_SOCKS5_UNCONFIRMED) {
        if (client_send_pending_data(TCP_connection) == 0) {
            int ret = proxy_socks5_read_connection_response(TCP_connection);

            if (ret == -1) {
                TCP_connection->kill_at = 0;
                TCP_connection->status = TCP_CLIENT_DISCONNECTED;
            }

            if (ret == 1) {
                generate_handshake(TCP_connection);
                TCP_connection->status = TCP_CLIENT_CONNECTING;
            }
        }
    }

    if (TCP_connection->status == TCP_CLIENT_CONNECTING) {
        if (client_send_pending_data(TCP_connection) == 0) {
            TCP_connection->status = TCP_CLIENT_UNCONFIRMED;
        }
    }

    if (TCP_connection->status == TCP_CLIENT_UNCONFIRMED) {
        uint8_t data[TCP_SERVER_HANDSHAKE_SIZE];
        int len = read_TCP_packet(TCP_connection->sock, data, sizeof(data));

        if (sizeof(data) == len) {
            if (handle_handshake(TCP_connection, data) == 0) {
                TCP_connection->kill_at = ~0;
                TCP_connection->status = TCP_CLIENT_CONFIRMED;
            } else {
                TCP_connection->kill_at = 0;
                TCP_connection->status = TCP_CLIENT_DISCONNECTED;
            }
        }
    }

    if (TCP_connection->status == TCP_CLIENT_CONFIRMED) {
        do_confirmed_TCP(TCP_connection, userdata);
    }

    if (TCP_connection->kill_at <= unix_time()) {
        TCP_connection->status = TCP_CLIENT_DISCONNECTED;
    }
}

/* Kill the TCP connection
 */
void kill_TCP_connection(TCP_Client_Connection *TCP_connection)
{
    if (TCP_connection == NULL) {
        return;
    }

    wipe_priority_list(TCP_connection);
    kill_sock(TCP_connection->sock);
    crypto_memzero(TCP_connection, sizeof(TCP_Client_Connection));
    free(TCP_connection);
}
