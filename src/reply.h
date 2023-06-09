//
// Created by yukino on 2023/5/3.
//

#ifndef RESP_SERVER_REPLY_H
#define RESP_SERVER_REPLY_H

#include "server.h"

int clientHasPendingReplies(client *c);
void addReplyError(client *c, const char *err);
void addReply(client *c, robj *obj);
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyBulk(client *c, robj *obj);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReplyArrayLen(client *c, long length);
#endif //RESP_SERVER_REPLY_H
