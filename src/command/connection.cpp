/*
 *Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "comms.hpp"

OP_NAMESPACE_BEGIN
    int Comms::Quit(Context& ctx, RedisCommandFrame& cmd)
    {
        fill_ok_reply(ctx.reply);
        return -1;
    }
    int Comms::Ping(Context& ctx, RedisCommandFrame& cmd)
    {
        fill_pong_reply(ctx.reply);
        return 0;
    }
    int Comms::Echo(Context& ctx, RedisCommandFrame& cmd)
    {
        fill_str_reply(ctx.reply, cmd.GetArguments()[0]);
        return 0;
    }
    int Comms::Select(Context& ctx, RedisCommandFrame& cmd)
    {
        uint32 newdb = 0;
        if (!string_touint32(cmd.GetArguments()[0], newdb) || newdb >= m_cfg.maxdb)
        {
            fill_error_reply(ctx.reply, "invalid DB index");
            return 0;
        }
        ctx.currentDB = newdb;
        fill_ok_reply(ctx.reply);
        return 0;
    }

    int Comms::Auth(Context& ctx, RedisCommandFrame& cmd)
    {
        if (m_cfg.requirepass.empty())
        {
            fill_error_reply(ctx.reply, "Client sent AUTH, but no password is set");
        }
        else if (m_cfg.requirepass != cmd.GetArguments()[0])
        {
            ctx.authenticated = false;
            fill_error_reply(ctx.reply, "invalid password");
        }
        else
        {
            ctx.authenticated = true;
            fill_ok_reply(ctx.reply);
        }
        return 0;
    }

OP_NAMESPACE_END

