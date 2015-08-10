/*
 *Copyright (c) 2013-2013, yinqiwen <yinqiwen@gmail.com>
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

#include "slave.hpp"
#include <sstream>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include "comms.hpp"
#include "repl.hpp"

namespace comms
{
    enum SlaveState
    {
        SLAVE_STATE_INVALID = 0,
        SLAVE_STATE_CONNECTING,
        SLAVE_STATE_WAITING_AUTH_REPLY,
        SLAVE_STATE_WAITING_INFO_REPLY,
        SLAVE_STATE_WAITING_REPLCONF_REPLY,
        SLAVE_STATE_WAITING_PSYNC_REPLY,
        SLAVE_STATE_WAITING_SNAPSHOT,
        SLAVE_STATE_LOADING_SNAPSHOT,
        SLAVE_STATE_SYNCED,
    };
    Slave::Slave() :
            m_client(NULL), m_cmd_recved_time(0), m_master_link_down_time(0), m_routine_ts(0), m_lastinteraction(0)
    {
        m_slave_ctx.server_address = MASTER_SERVER_ADDRESS_NAME;
        m_slave_ctx.client = NULL;
    }

    int Slave::Init()
    {
        struct RoutineTask: public Runnable
        {
                Slave* c;
                RoutineTask(Slave* cc) :
                        c(cc)
                {
                }
                void Run()
                {
                    c->Routine();
                }
        };
        g_repl->GetTimer().ScheduleHeapTask(new RoutineTask(this), 1, 1, SECONDS);
        return 0;
    }

    static int slave_replay_wal(const void* log, size_t loglen, void* data)
    {
        g_repl->GetSlave().ReplayWAL(log, loglen);
        return 0;
    }

    void Slave::ReplayWAL()
    {
        if (m_status.state != SLAVE_STATE_SYNCED)
        {
            //can not replay wal in non synced state
            return;
        }
        if (g_repl->DataOffset() == g_repl->WALEndOffset())
        {
            swal_clear_replay_cache(g_repl->GetWAL());
            return;
        }
        if (m_status.replaying_wal)
        {
            return;
        }
        m_status.replaying_wal = true;
        swal_replay(g_repl->GetWAL(), g_repl->DataOffset(), -1, slave_replay_wal, NULL);
        m_status.replaying_wal = false;
    }

    void Slave::ReplayWAL(const void* log, size_t loglen)
    {
        std::string err;
        Buffer logbuf((char*) log, 0, loglen);
        while (logbuf.Readable() && m_status.state == SLAVE_STATE_SYNCED)
        {
            RedisCommandFrame msg;
            size_t rest = logbuf.ReadableBytes();
            if (!RedisCommandDecoder::Decode(logbuf, msg, err))
            {
                break;
            }
            CallFlags flags;
            flags.no_wal = 1;
            g_db->Call(m_slave_ctx, msg, flags);
            g_repl->UpdateDataOffsetCksm(msg.GetRawProtocolData());
            g_repl->GetIOServ().Continue();
        }
    }

    void Slave::ChannelConnected(ChannelHandlerContext& ctx, ChannelStateEvent& e)
    {
        DEBUG_LOG("Master conn connected.");
        m_lastinteraction = m_cmd_recved_time = time(NULL);
        m_master_link_down_time = 0;
        if (!g_db->GetConfig().masterauth.empty())
        {
            Buffer auth;
            auth.Printf("auth %s\r\n", g_db->GetConfig().masterauth.c_str());
            ctx.GetChannel()->Write(auth);
            m_status.state = SLAVE_STATE_WAITING_AUTH_REPLY;
            return;
        }
        InfoMaster();
    }

    void Slave::HandleRedisCommand(Channel* ch, RedisCommandFrame& cmd)
    {
        bool write_wal_only = false;
        if (m_status.state != SLAVE_STATE_SYNCED)
        {
            write_wal_only = true;
        }
        m_cmd_recved_time = time(NULL);
        int len = g_repl->WriteWAL(cmd.GetRawProtocolData());
        DEBUG_LOG("Recv master inline:%d cmd %s with len:%d at %lld %lld at state:%d", cmd.IsInLine(), cmd.ToString().c_str(), len,
                g_repl->DataOffset(), g_repl->WALEndOffset(), m_status.state);
        if (!write_wal_only && g_repl->DataOffset() + len == g_repl->WALEndOffset())
        {
            CallFlags flags;
            flags.no_wal = 1;
            g_db->Call(m_slave_ctx, cmd, flags);
            g_repl->UpdateDataOffsetCksm(cmd.GetRawProtocolData());
            return;
        }
        ReplayWAL();
    }
    void Slave::Routine()
    {
        if (g_db->GetConfig().master_host.empty())
        {
            return;
        }
        ReplayWAL();
        uint32 now = time(NULL);
        if (NULL == m_client)
        {
            ConnectMaster();
            return;
        }
        if (m_status.state == SLAVE_STATE_SYNCED || m_status.state == SLAVE_STATE_LOADING_SNAPSHOT)
        {
            if (m_cmd_recved_time > 0 && now - m_cmd_recved_time >= g_db->GetConfig().repl_timeout)
            {
                if (m_status.state == SLAVE_STATE_SYNCED)
                {
                    WARN_LOG("now = %u, ping_recved_time=%u", now, m_cmd_recved_time);
                    Timeout();
                    return;
                }
            }
            if (m_status.server_support_psync && NULL != m_client)
            {
                Buffer ack;
                ack.Printf("REPLCONF ACK %llu\r\n", g_repl->DataOffset());
                m_client->Write(ack);
            }
        }
        m_routine_ts = now;
    }

    void Slave::InfoMaster()
    {
        Buffer info;
        info.Printf("info Server\r\n");
        m_client->Write(info);
        m_status.state = SLAVE_STATE_WAITING_INFO_REPLY;
    }

    static int LoadRDBRoutine(void* cb)
    {
        g_repl->GetIOServ().Continue();
        Slave* slave = (Slave*) cb;
        if (slave->GetStatus().state != SLAVE_STATE_LOADING_SNAPSHOT)
        {
            return -1;
        }
        slave->Routine();
        return 0;
    }
    void Slave::HandleRedisReply(Channel* ch, RedisReply& reply)
    {
        switch (m_status.state)
        {
            case SLAVE_STATE_WAITING_AUTH_REPLY:
            {
                if (reply.type == REDIS_REPLY_ERROR)
                {
                    ERROR_LOG("Recv auth reply error:%s", reply.str.c_str());
                    Close();
                }
                InfoMaster();
                return;
            }
            case SLAVE_STATE_WAITING_INFO_REPLY:
            {
                if (reply.type == REDIS_REPLY_ERROR)
                {
                    ERROR_LOG("Recv info reply error:%s", reply.str.c_str());
                    Close();
                    return;
                }
                const char* redis_ver_key = "redis_version:";
                const char* comms_ver_key = "comms_version:";
                m_status.server_is_redis = reply.str.find(comms_ver_key) == std::string::npos;
                if (m_status.server_is_redis)
                {
                    size_t start = reply.str.find(redis_ver_key);
                    size_t end = reply.str.find("\n", start);
                    std::string v = reply.str.substr(start + strlen(redis_ver_key),
                            end - start - strlen(redis_ver_key));
                    v = trim_string(v);
                    m_status.server_support_psync = (compare_version<3>(v, "2.7.0") >= 0);
                    INFO_LOG("[Slave]Remote master is a Redis %s instance, support partial sync:%u", v.c_str(),
                            m_status.server_support_psync);

                }
                else
                {
                    INFO_LOG("[Slave]Remote master is an Comms instance.");
                    m_status.server_support_psync = true;
                }
                Buffer replconf;
                replconf.Printf("replconf listening-port %u\r\n", g_db->GetConfig().PrimayPort());
                ch->Write(replconf);
                m_status.state = SLAVE_STATE_WAITING_REPLCONF_REPLY;
                break;
            }
            case SLAVE_STATE_WAITING_REPLCONF_REPLY:
            {
                if (reply.type == REDIS_REPLY_ERROR)
                {
                    ERROR_LOG("Recv replconf reply error:%s", reply.str.c_str());
                    ch->Close();
                    return;
                }
                if (m_status.server_support_psync)
                {
                    Buffer sync;
                    if (!m_status.server_is_redis)
                    {
                        sync.Printf("psync %s %lld cksm %llu\r\n", g_repl->GetServerKey(), g_repl->WALEndOffset(),
                                g_repl->WALCksm());
                    }
                    else
                    {
                        sync.Printf("psync %s %lld\r\n", g_repl->GetServerKey(), g_repl->WALEndOffset());
                    }
                    INFO_LOG("Send %s", trim_string(sync.AsString()).c_str());
                    ch->Write(sync);
                    m_status.state = SLAVE_STATE_WAITING_PSYNC_REPLY;
                }
                else
                {
                    Buffer sync;
                    sync.Printf("sync\r\n");
                    ch->Write(sync);
                    m_status.state = SLAVE_STATE_WAITING_SNAPSHOT;
                    m_decoder.SwitchToDumpFileDecoder();
                }
                break;
            }
            case SLAVE_STATE_WAITING_PSYNC_REPLY:
            {
                if (reply.type != REDIS_REPLY_STATUS)
                {
                    ERROR_LOG("Recv psync reply error:%s", reply.str.c_str());
                    ch->Close();
                    return;
                }
                INFO_LOG("Recv psync reply:%s", reply.str.c_str());
                std::vector<std::string> ss = split_string(reply.str, " ");
                if (!strcasecmp(ss[0].c_str(), "FULLRESYNC"))
                {
                    int64 offset;
                    if (!string_toint64(ss[2], offset))
                    {
                        ERROR_LOG("Invalid psync offset:%s", ss[2].c_str());
                        ch->Close();
                        return;
                    }

                    m_status.cached_master_runid = ss[1];
                    m_status.cached_master_repl_offset = offset;
                    /*
                     * if remote master is comms, there would be a cksm part
                     */
                    if (ss.size() > 3)
                    {
                        uint64 cksm;
                        if (!string_touint64(ss[3], cksm))
                        {
                            ERROR_LOG("Invalid psync cksm:%s", ss[3].c_str());
                            ch->Close();
                            return;
                        }
                        m_status.cached_master_repl_cksm = cksm;
                    }
                    m_status.state = SLAVE_STATE_WAITING_SNAPSHOT;
                    m_decoder.SwitchToDumpFileDecoder();
                    break;
                }
                else if (!strcasecmp(ss[0].c_str(), "CONTINUE"))
                {
                    m_decoder.SwitchToCommandDecoder();
                    m_status.state = SLAVE_STATE_SYNCED;
                    break;
                }
                else
                {
                    ERROR_LOG("Invalid psync status:%s", reply.str.c_str());
                    ch->Close();
                    return;
                }
                break;
            }
            default:
            {
                ERROR_LOG("Slave client is in invalid state:%d", m_status.state);
                Close();
                break;
            }
        }
    }

    void Slave::HandleRedisDumpChunk(Channel* ch, RedisDumpFileChunk& chunk)
    {
        if (m_status.state != SLAVE_STATE_WAITING_SNAPSHOT)
        {
            ERROR_LOG("Invalid state:%u to handler redis dump file chunk.", m_status.state);
            ch->Close();
            return;
        }
        if (chunk.IsFirstChunk())
        {
            m_status.snapshot.Close();
            char tmp[g_db->GetConfig().repl_data_dir.size() + 100];
            uint32 now = time(NULL);
            sprintf(tmp, "%s/temp-%u-%u.snapshot", g_db->GetConfig().repl_data_dir.c_str(), getpid(), now);
            m_status.snapshot.Open(tmp, false);
            INFO_LOG("[Slave]Create dump file:%s, master is redis:%d", tmp, m_status.server_is_redis);
            //m_status.snapshot->SetExpectedDataSize(chunk.len);
        }
        if (!chunk.chunk.empty())
        {
            m_status.snapshot.Write(chunk.chunk.c_str(), chunk.chunk.size());
        }
        if (chunk.IsLastChunk())
        {
            m_status.snapshot.Flush();
            SnapshotType snapshot_type = Snapshot::IsRedisSnapshot(m_status.snapshot.GetPath())?REDIS_SNAPSHOT:MMKV_SNAPSHOT;
            m_status.snapshot.RenameDefault();
            m_decoder.SwitchToCommandDecoder();
            m_status.state = SLAVE_STATE_LOADING_SNAPSHOT;
            /*
             * set server key to a random string first, if server restart when loading data, it would do a full resync again with another server key.
             * set wal offset&cksm to make sure that this slave could accept&save synced commands when loading snapshot file.
             */
            g_repl->SetServerKey(random_hex_string(40));
            g_repl->ResetWALOffsetCksm(m_status.cached_master_repl_offset, m_status.cached_master_repl_cksm);
            if (g_db->GetConfig().slave_cleardb_before_fullresync)
            {
                g_db->GetKVStore().FlushAll();
            }
            INFO_LOG("Start loading snapshot file.");
            m_cmd_recved_time = time(NULL);
            int ret = m_status.snapshot.Reload(LoadRDBRoutine, this);
            if (0 != ret)
            {
                if (NULL != m_client)
                {
                    m_client->Close();
                }
                WARN_LOG("Failed to load snapshot file.");
                return;
            }
            g_repl->SetServerKey(m_status.cached_master_runid);
            g_repl->ResetDataOffsetCksm(m_status.cached_master_repl_offset, m_status.cached_master_repl_cksm);
            m_status.snapshot.UpdateSnapshotOffsetCksm(snapshot_type, m_status.cached_master_repl_offset, m_status.cached_master_repl_cksm);
            m_status.snapshot.Close();
            m_status.state = SLAVE_STATE_SYNCED;
            //Disconnect all slaves when all data resynced
            g_repl->GetMaster().DisconnectAllSlaves();
            ReplayWAL();
        }
    }

    void Slave::MessageReceived(ChannelHandlerContext& ctx, MessageEvent<RedisMessage>& e)
    {
        m_lastinteraction = time(NULL);
        if (e.GetMessage()->IsReply())
        {
            HandleRedisReply(ctx.GetChannel(), e.GetMessage()->reply);
        }
        else if (e.GetMessage()->IsCommand())
        {
            HandleRedisCommand(ctx.GetChannel(), e.GetMessage()->command);
        }
        else
        {
            HandleRedisDumpChunk(ctx.GetChannel(), e.GetMessage()->chunk);
        }
    }

    void Slave::ChannelClosed(ChannelHandlerContext& ctx, ChannelStateEvent& e)
    {
        INFO_LOG("[Slave]Replication connection closed.");
        m_lastinteraction = m_master_link_down_time = time(NULL);
        m_client = NULL;
        m_slave_ctx.Clear();
        m_status.Clear();
    }

    void Slave::Timeout()
    {
        WARN_LOG("Master connection timeout.");
        Close();
    }

    int Slave::ConnectMaster()
    {
        if (g_db->GetConfig().master_host.empty())
        {
            return -1;
        }
        SocketHostAddress addr(g_db->GetConfig().master_host, g_db->GetConfig().master_port);
        if (NULL != m_client)
        {
            return 0;
        }
        m_client = g_repl->GetIOServ().NewClientSocketChannel();

        m_decoder.Clear();
        m_client->GetPipeline().AddLast("decoder", &m_decoder);
        m_client->GetPipeline().AddLast("encoder", &m_encoder);
        m_client->GetPipeline().AddLast("handler", this);
        m_decoder.SwitchToReplyDecoder();
        m_status.state = SLAVE_STATE_CONNECTING;
        m_client->Connect(&addr);
        DEBUG_LOG("[Slave]Connecting master %s:%u", g_db->GetConfig().master_host.c_str(),
                g_db->GetConfig().master_port);
        return 0;
    }

    bool Slave::IsSynced()
    {
        return NULL != m_client && SLAVE_STATE_SYNCED == m_status.state;
    }

    void Slave::Stop()
    {
        m_status.state = SLAVE_STATE_INVALID;
        Close();
    }
    void Slave::Close()
    {
        if (NULL != m_client)
        {
            m_client->Close();
        }
    }
}

