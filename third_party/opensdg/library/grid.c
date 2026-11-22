#include <string.h>
#include "client.h"
#include "control_protocol.h"
#include "mainloop.h"
#include "socket.h"
#include "logging.h"
#include "tunnel_protocol.h"

/* heatbrain addition, see osdg_set_incoming_call_callback() */
static osdg_incoming_call_cb_t incoming_call_cb;

void osdg_set_incoming_call_callback(osdg_incoming_call_cb_t cb)
{
    incoming_call_cb = cb;
}

/*
 * heatbrain addition: tell the grid which peer we are paired with (message 22: uint32 sequence = 1,
 * bytes peerId = 2). The grid routes an incoming call to us only from peers on this list; the
 * official library sends it after every pairing change and the grid confirms with message 21.
 * Runs on the main loop thread.
 */
#define MSG_PAIRINGS 22
osdg_result_t grid_register_pairing(struct _osdg_connection *grid, const unsigned char *peerId)
{
    static unsigned int sequence;
    unsigned char body[1 + 1 + 5 + 2 + sizeof(osdg_key_t)];
    unsigned int seq = sequence++;
    size_t n = 0;
    struct packetMESG *mesg;
    struct mesg_payload *payload;

    if (!grid || grid->state != osdg_connected)
        return osdg_wrong_state;

    body[n++] = MSG_PAIRINGS;
    body[n++] = 0x08; /* field 1, varint */
    do {
        unsigned char b = seq & 0x7F;
        seq >>= 7;
        body[n++] = seq ? (b | 0x80) : b;
    } while (seq);
    body[n++] = 0x12; /* field 2, length-delimited */
    body[n++] = sizeof(osdg_key_t);
    memcpy(&body[n], peerId, sizeof(osdg_key_t));
    n += sizeof(osdg_key_t);

    mesg = get_MESG_packet(grid, n);
    if (!mesg)
        return osdg_buffer_exceeded;
    payload = (struct mesg_payload *)(mesg->mesg_payload - crypto_box_BOXZEROBYTES);
    memcpy(payload->data.data, body, n);
    LOG(PROTOCOL, "Grid[%p] registering pairing", grid);
    return send_MESG_packet(grid, mesg);
}

static osdg_result_t grid_handle_incoming_packet(struct _osdg_connection *conn,
                                                 const void *p, unsigned int length)
{
    const unsigned char *data = p;
    osdg_result_t ret = osdg_no_error;
    unsigned char msgType;
  
    /*
     * Grid messages come in protobuf format, prefixed by one byte, indicating
     * message type.
     */
    if (length == 0)
    {
        LOG(ERRORS, "Grid[%p] Empty packet received", conn);
        return osdg_no_error; /* Ignore this */
    }

    msgType = *data++;
    length--;

    if (msgType == MSG_PROTOCOL_VERSION)
    {
        ProtocolVersion *protocolVer = protocol_version__unpack(NULL, length, data);

        if (!protocolVer)
        {
            LOG(ERRORS, "MSG_PROTOCOL_VERSION protobuf decoding error");
            return osdg_protocol_error;
        }

        if (protocolVer->magic != PROTOCOL_VERSION_MAGIC)
        {
            LOG(ERRORS, "Incorrect protocol version magic 0x%08X", protocolVer->magic);
            ret = osdg_protocol_error;
        }
        else if (protocolVer->major != PROTOCOL_VERSION_MAJOR || protocolVer->minor != PROTOCOL_VERSION_MINOR)
        {
            LOG(ERRORS, "Unsupported grid protocol version %u.%u", protocolVer->major, protocolVer->minor);
            ret = osdg_protocol_error;
        }
        else
        {
            LOG(PROTOCOL, "Using protocol version %u.%u", protocolVer->major, protocolVer->minor);
            /* We're done with the handshake */
        }

        protocol_version__free_unpacked(protocolVer, NULL);

        /* Send the very first ping right after the connection has been established.
           This is what the original library does. */
        if (ret == osdg_no_error)
            ret = connection_ping(conn);
    }
    else if (msgType == MSG_PONG)
    {
        Pong *reply = pong__unpack(NULL, length, data);

        if (!reply)
        {
            DUMP(ERRORS, data, length, "Grid[%p] MSG_PONG protobuf decoding error", conn);
            return osdg_no_error; /* Do not abort grid connection */
        }

        /* Ignore old stray PONGs, this is what the original library does */
        if (reply->seq == conn->pingSequence - 1)
        {
            conn->pingDelay = (int)(timestamp() - conn->lastPing);
            LOG(PROTOCOL, "Grid[%p] PING roundtrip %ld ms", conn, conn->pingDelay);
        }

        pong__free_unpacked(reply, NULL);

        /* This is reply to the very first PING, we are connected now. */
        if (conn->state == osdg_connecting)
        {
            connection_set_status(conn, osdg_connected);
            mainloop_client_event(); // This will recalculate delay before the next PING
        }
    }
    else if ((msgType == MSG_REMOTE_REPLY) || (msgType == MSG_PAIR_REMOTE_REPLY))
    {
        PeerReply *reply = peer_reply__unpack(NULL, length, data);
        struct list_element *req;

        if (!reply)
        {
            DUMP(ERRORS, data, length, "MSG_REMOTE_REPLY protobuf decoding error");
            return osdg_no_error; /* Do not abort grid connection */
        }

        for (req = conn->forwardList.head; req->next; req = req->next)
        {
            struct _osdg_connection *peer = get_connection(req);

            if (peer->uid == reply->id)
            {
                list_remove(req);
                ret = peer_handle_remote_call_reply(peer, reply);
                peer_reply__free_unpacked(reply, NULL);
                return ret;
            }
        }

        LOG(ERRORS, "Received MSG_PEER_REPLY for nonexistent peer %u\n", reply->id);
        peer_reply__free_unpacked(reply, NULL);
        /* Ignore, this is not critical */
    }
    else if (msgType == MSG_INCOMING_CALL)
    {
        IncomingCall *call = incoming_call__unpack(NULL, length, data);
        IncomingCallReply reply = INCOMING_CALL_REPLY__INIT;

        if (!call)
        {
            DUMP(ERRORS, data, length, "MSG_INCOMING_CALL protobuf decoding error");
            return osdg_no_error; /* Do not abort grid connection */
        }

        LOG(PROTOCOL, "Incoming call from %s protocol %s", call->peer->peerid, call->protocol);

        /* Refuse, unless the application accepts (heatbrain addition) */
        reply.id = call->id;
        reply.result = 0;
        if (incoming_call_cb && call->peer && call->peer->server && call->protocol)
            reply.result = incoming_call_cb(conn, call->peer->peerid, call->protocol,
                                            call->peer->server->host, call->peer->server->port,
                                            call->peer->tunnelid.data, (unsigned int)call->peer->tunnelid.len) ? 1 : 0;

        incoming_call__free_unpacked(call, NULL);
        ret = sendMESG(conn, MSG_INCOMING_CALL_REPLY, &reply);
    }
    else
    {
        DUMP(PROTOCOL, data, length, "Unhandled grid message type %u", msgType);
    }

    return ret;
}

/* Well-known grid servers */
osdg_result_t osdg_connect_to_danfoss(osdg_connection_t conn)
{
    static const struct osdg_endpoint danfoss_servers[] =
    {
        {"77.66.11.90" , 443},
        {"77.66.11.92" , 443},
        {"5.179.92.180", 443},
        {"5.179.92.182", 443}
    };

    return osdg_connect_to_grid(conn, danfoss_servers, 4);
}

osdg_result_t osdg_connect_to_grid(osdg_connection_t client,
                                   const struct osdg_endpoint *servers, unsigned int nServers)
{
    unsigned int left, i;
    int res;
    const struct osdg_endpoint **list, **randomized;

    if (connection_in_use(client))
        return osdg_wrong_state;

    if (nServers == 0)
    {
        client->errorKind = osdg_invalid_parameters;
        return osdg_invalid_parameters;
    }

    res = connection_init(client);
    if (res)
        return client->errorKind;

    client->mode        = mode_grid;
    client->receiveData = grid_handle_incoming_packet;

    /* Grid connection is not used often; most of the time it just keeps silence.
       A ping is mandatory, otherwise the server will drop the connection */
    if (!client->pingInterval)
        client->pingInterval = 30 * MILLISECONDS_PER_SECOND;

    /* Permute servers in random order in order to distribute the load */
    list = malloc(nServers * sizeof(void *));
    randomized = malloc(nServers * sizeof(void*));

    if (!(list && randomized))
    {
        free((void *)list);
        free((void *)randomized);

        client->errorKind = osdg_memory_error;
        client->state = osdg_error;
        return osdg_connection_failed;
    }

    for (i = 0; i < nServers; i++)
        list[i] = &servers[i];

    left = nServers;
    for (i = 0; i < nServers; i++)
    {
        unsigned int idx = rand() / ((((unsigned int)RAND_MAX) + 1) / left);

        randomized[i] = list[idx];
        left--;
        list[idx] = list[left];
    }

    free((void *)list);

    for (i = 0; i < nServers; i++)
    {
        res = connect_to_host(client, randomized[i]->host, randomized[i]->port);
        if (res != 0)
            break; /* Success or serious error */
    }

    free((void *)randomized);

    if (res == 0)
    {
        client->errorKind = osdg_connection_failed;
        res = -1;
    }

    if (res < 0)
    {
        client->state = osdg_error;
        return osdg_connection_failed;
    }

    return connection_wait(client);
}

osdg_result_t osdg_set_ping_interval(osdg_connection_t conn, unsigned int seconds)
{
    /* Only grid can be pinged */
    if (connection_in_use(conn) && conn->mode != mode_grid)
        return osdg_wrong_state;

    conn->pingInterval = seconds * MILLISECONDS_PER_SECOND;

    /* This causes main loop timeout update */
    if (conn->state == osdg_connected)
        mainloop_client_event();

    return osdg_no_error;
}

osdg_result_t connection_ping(struct _osdg_connection *grid)
{
    Ping ping = PING__INIT;

    ping.seq = grid->pingSequence++;
    /* The server wants to know ping roundtrip time in milliseconds.
       For the very first ping packet there's no data yet */
    if (grid->pingDelay != -1)
    {
        ping.has_delay = 1;
        ping.delay = grid->pingDelay;
    }

    grid->lastPing = timestamp();
    return sendMESG(grid, MSG_PING, &ping);
}
