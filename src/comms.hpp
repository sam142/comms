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

#ifndef DB_HPP_
#define DB_HPP_
#include "common/common.hpp"
#include "thread/thread_local.hpp"
#include "thread/spin_rwlock.hpp"
#include "thread/spin_mutex_lock.hpp"
#include "channel/all_includes.hpp"
#include "statistics.hpp"
#include "context.hpp"
#include "cron.hpp"
#include "config.hpp"
#include "logger.hpp"
#include "util/redis_helper.hpp"
#include "command/lua_scripting.hpp"
#include "replication/repl.hpp"
#include <sparsehash/dense_hash_map>

#define COMMS_OK 0
#define ERR_OVERLOAD -100
#define ERR_SNAPSHOT_SAVING -101

/* Command flags. Please check the command table defined in the redis.c file
 * for more information about the meaning of every flag. */
#define COMMS_CMD_WRITE 1                   /* "w" flag */
#define COMMS_CMD_READONLY 2                /* "r" flag */
//#define COMMS_CMD_DENYOOM 4                 /* "m" flag */
//#define COMMS_CMD_NOT_USED_1 8              /* no longer used flag */
#define COMMS_CMD_ADMIN 16                  /* "a" flag */
#define COMMS_CMD_PUBSUB 32                 /* "p" flag */
#define COMMS_CMD_NOSCRIPT  64              /* "s" flag */
#define COMMS_CMD_RANDOM 128                /* "R" flag */
//#define COMMS_CMD_SORT_FOR_SCRIPT 256       /* "S" flag */
//#define COMMS_CMD_LOADING 512               /* "l" flag */
//#define COMMS_CMD_STALE 1024                /* "t" flag */
//#define COMMS_CMD_SKIP_MONITOR 2048         /* "M" flag */
//#define COMMS_CMD_ASKING 4096               /* "k" flag */

#define SCRIPT_KILL_EVENT 1
#define SCRIPT_FLUSH_EVENT 2

OP_NAMESPACE_BEGIN

    class RedisRequestHandler;
    class ExpireCheck;
    class ConnectionTimeout;
    class BlockListTimeout;
    class WakeBlockListData;
    class Comms
    {
        public:
            typedef int (Comms::*RedisCommandHandler)(Context&, RedisCommandFrame&);
            struct RedisCommandHandlerSetting
            {
                    const char* name;
                    RedisCommandType type;
                    RedisCommandHandler handler;
                    int min_arity;
                    int max_arity;
                    const char* sflags;
                    int flags;
                    volatile uint64 microseconds;
                    volatile uint64 calls;
                    volatile uint64 max_latency;
            };
            struct RedisCommandHash
            {
                    size_t operator()(const std::string& t) const;
            };
            struct RedisCommandEqual
            {
                    bool operator()(const std::string& s1, const std::string& s2) const;
            };
        private:
            mmkv::MMKV* m_kv_store;
            ChannelService* m_service;

            CommsConfig m_cfg;
            Properties m_cfg_props;
            SpinRWLock m_cfg_lock;

            CronManager m_cron;
            Statistics m_stat;

            typedef google::dense_hash_map<std::string, RedisCommandHandlerSetting, RedisCommandHash, RedisCommandEqual> RedisCommandHandlerSettingTable;
            RedisCommandHandlerSettingTable m_settings;

            LUAInterpreter m_lua;

            ThreadLocal<RedisReplyPool> m_reply_pool;
            /*
             * Transction watched keys
             */
            typedef TreeMap<WatchKey, ContextSet>::Type WatchedContextTable;
            WatchedContextTable m_watched_ctx;
            SpinRWLock m_watched_keys_lock;

            typedef TreeMap<std::string, ContextSet>::Type PubsubContextTable;
            PubsubContextTable m_pubsub_channels;
            PubsubContextTable m_pubsub_patterns;
            SpinRWLock m_pubsub_ctx_lock;

            typedef TreeMap<WatchKey, ContextSet>::Type BlockContextTable;
            BlockContextTable m_block_context_table;
            SpinRWLock m_block_ctx_lock;

            ContextTable m_clients;
            SpinMutexLock m_clients_lock;

            typedef TreeMap<std::string, std::string>::Type ScriptTable;
            ScriptTable m_lua_scripts;
            SpinMutexLock m_scripts_lock;

            ReplicationService m_repl;

            static std::string& ReplyResultStringAlloc(void* data);
            void AddBlockKey(Context& ctx, const std::string& key);
            void WakeBlockingListsByKey(Context& ctx, const std::string& key);
            void WakeBlockedList(Context& ctx, const std::string& key);
            void UnblockList(Context& ctx, const std::string& key);
            void ClearBlockKeys(Context& ctx);

            int PublishMessage(Context& ctx, const std::string& channel, const std::string& message);
            int PUnsubscribeAll(Context& ctx, bool notify);
            int PUnsubscribeChannel(Context& ctx, const std::string& pattern, bool notify);
            int PSubscribeChannel(Context& ctx, const std::string& pattern, bool notify);
            int UnsubscribeAll(Context& ctx, bool notify);
            int SubscribeChannel(Context& ctx, const std::string& channel, bool notify);
            int UnsubscribeChannel(Context& ctx, const std::string& channel, bool notify);

            void TryPushSlowCommand(const RedisCommandFrame& cmd, uint64 micros);
            void FillInfoResponse(const std::string& section, std::string& info);
            void GetSlowlog(Context& ctx, uint32 len);

            bool FillErrorReply(Context& ctx, int err);

            bool GetInt64Value(Context& ctx, const std::string& str, int64& v);
            bool GetDoubleValue(Context& ctx, const std::string& str, long double& v);

            int IncrDecrCommand(Context& ctx, const std::string& key, int64 incr);

            time_t m_starttime;

            void RewriteClientCommand(Context& ctx, RedisCommandFrame& cmd);

            int SaveScript(const std::string& sha, const std::string& body);
            int GetScript(const std::string& sha, std::string& body);
            int FlushScripts(Context& ctx);

            int UnwatchKeys(Context& ctx);
            int AbortWatchKey(DBID db, const std::string& key);
            int FireKeyChangedEvent(Context& ctx, const std::string& key);
            int FireKeyChangedEvent(Context& ctx, DBID db, const std::string& key);

            int Time(Context& ctx, RedisCommandFrame& cmd);
            int FlushDB(Context& ctx, RedisCommandFrame& cmd);
            int FlushAll(Context& ctx, RedisCommandFrame& cmd);
            int Save(Context& ctx, RedisCommandFrame& cmd);
            int LastSave(Context& ctx, RedisCommandFrame& cmd);
            int BGSave(Context& ctx, RedisCommandFrame& cmd);
            int Import(Context& ctx, RedisCommandFrame& cmd);
            int Info(Context& ctx, RedisCommandFrame& cmd);
            int DBSize(Context& ctx, RedisCommandFrame& cmd);
            int Config(Context& ctx, RedisCommandFrame& cmd);
            int SlowLog(Context& ctx, RedisCommandFrame& cmd);
            int Client(Context& ctx, RedisCommandFrame& cmd);
            int Keys(Context& ctx, RedisCommandFrame& cmd);
            int Randomkey(Context& ctx, RedisCommandFrame& cmd);
            int Scan(Context& ctx, RedisCommandFrame& cmd);

            int Multi(Context& ctx, RedisCommandFrame& cmd);
            int Discard(Context& ctx, RedisCommandFrame& cmd);
            int Exec(Context& ctx, RedisCommandFrame& cmd);
            int Watch(Context& ctx, RedisCommandFrame& cmd);
            int UnWatch(Context& ctx, RedisCommandFrame& cmd);

            int Subscribe(Context& ctx, RedisCommandFrame& cmd);
            int UnSubscribe(Context& ctx, RedisCommandFrame& cmd);
            int PSubscribe(Context& ctx, RedisCommandFrame& cmd);
            int PUnSubscribe(Context& ctx, RedisCommandFrame& cmd);
            int Publish(Context& ctx, RedisCommandFrame& cmd);

            int Slaveof(Context& ctx, RedisCommandFrame& cmd);
            int Sync(Context& ctx, RedisCommandFrame& cmd);
            int PSync(Context& ctx, RedisCommandFrame& cmd);
            int ReplConf(Context& ctx, RedisCommandFrame& cmd);

            int Ping(Context& ctx, RedisCommandFrame& cmd);
            int Echo(Context& ctx, RedisCommandFrame& cmd);
            int Select(Context& ctx, RedisCommandFrame& cmd);
            int Quit(Context& ctx, RedisCommandFrame& cmd);

            int Shutdown(Context& ctx, RedisCommandFrame& cmd);
            int Type(Context& ctx, RedisCommandFrame& cmd);
            int Move(Context& ctx, RedisCommandFrame& cmd);
            int Rename(Context& ctx, RedisCommandFrame& cmd);
            int RenameNX(Context& ctx, RedisCommandFrame& cmd);
            int Sort(Context& ctx, RedisCommandFrame& cmd);

            int Append(Context& ctx, RedisCommandFrame& cmd);

            int Decr(Context& ctx, RedisCommandFrame& cmd);
            int Decrby(Context& ctx, RedisCommandFrame& cmd);
            int Get(Context& ctx, RedisCommandFrame& cmd);

            int GetRange(Context& ctx, RedisCommandFrame& cmd);
            int GetSet(Context& ctx, RedisCommandFrame& cmd);
            int Incr(Context& ctx, RedisCommandFrame& cmd);
            int Incrby(Context& ctx, RedisCommandFrame& cmd);
            int IncrbyFloat(Context& ctx, RedisCommandFrame& cmd);
            int MGet(Context& ctx, RedisCommandFrame& cmd);
            int MSet(Context& ctx, RedisCommandFrame& cmd);
            int MSetNX(Context& ctx, RedisCommandFrame& cmd);
            int PSetEX(Context& ctx, RedisCommandFrame& cmd);
            int SetEX(Context& ctx, RedisCommandFrame& cmd);
            int SetNX(Context& ctx, RedisCommandFrame& cmd);
            int SetRange(Context& ctx, RedisCommandFrame& cmd);
            int Strlen(Context& ctx, RedisCommandFrame& cmd);
            int Set(Context& ctx, RedisCommandFrame& cmd);
            int Del(Context& ctx, RedisCommandFrame& cmd);
            int Exists(Context& ctx, RedisCommandFrame& cmd);
            int Expire(Context& ctx, RedisCommandFrame& cmd);
            int Expireat(Context& ctx, RedisCommandFrame& cmd);
            int Persist(Context& ctx, RedisCommandFrame& cmd);
            int PExpire(Context& ctx, RedisCommandFrame& cmd);
            int PExpireat(Context& ctx, RedisCommandFrame& cmd);
            int PTTL(Context& ctx, RedisCommandFrame& cmd);
            int TTL(Context& ctx, RedisCommandFrame& cmd);

            int Bitcount(Context& ctx, RedisCommandFrame& cmd);
            int Bitop(Context& ctx, RedisCommandFrame& cmd);
            int SetBit(Context& ctx, RedisCommandFrame& cmd);
            int GetBit(Context& ctx, RedisCommandFrame& cmd);
            int Bitpos(Context& ctx, RedisCommandFrame& cmd);

            int HDel(Context& ctx, RedisCommandFrame& cmd);
            int HExists(Context& ctx, RedisCommandFrame& cmd);
            int HGet(Context& ctx, RedisCommandFrame& cmd);
            int HGetAll(Context& ctx, RedisCommandFrame& cmd);
            int HIncrby(Context& ctx, RedisCommandFrame& cmd);
//            int HMIncrby(Context& ctx, RedisCommandFrame& cmd);
            int HIncrbyFloat(Context& ctx, RedisCommandFrame& cmd);
            int HKeys(Context& ctx, RedisCommandFrame& cmd);
            int HLen(Context& ctx, RedisCommandFrame& cmd);
            int HMGet(Context& ctx, RedisCommandFrame& cmd);
            int HMSet(Context& ctx, RedisCommandFrame& cmd);
            int HSet(Context& ctx, RedisCommandFrame& cmd);
            int HSetNX(Context& ctx, RedisCommandFrame& cmd);
            int HVals(Context& ctx, RedisCommandFrame& cmd);
            int HScan(Context& ctx, RedisCommandFrame& cmd);
            int HStrlen(Context& ctx, RedisCommandFrame& cmd);

            int SAdd(Context& ctx, RedisCommandFrame& cmd);
            int SCard(Context& ctx, RedisCommandFrame& cmd);
            int SDiff(Context& ctx, RedisCommandFrame& cmd);
            int SDiffStore(Context& ctx, RedisCommandFrame& cmd);
            int SInter(Context& ctx, RedisCommandFrame& cmd);
            int SInterStore(Context& ctx, RedisCommandFrame& cmd);
            int SIsMember(Context& ctx, RedisCommandFrame& cmd);
            int SMembers(Context& ctx, RedisCommandFrame& cmd);
            int SMove(Context& ctx, RedisCommandFrame& cmd);
            int SPop(Context& ctx, RedisCommandFrame& cmd);
            int SRandMember(Context& ctx, RedisCommandFrame& cmd);
            int SRem(Context& ctx, RedisCommandFrame& cmd);
            int SUnion(Context& ctx, RedisCommandFrame& cmd);
            int SUnionStore(Context& ctx, RedisCommandFrame& cmd);
//            int SUnionCount(Context& ctx, RedisCommandFrame& cmd);
//            int SInterCount(Context& ctx, RedisCommandFrame& cmd);
//            int SDiffCount(Context& ctx, RedisCommandFrame& cmd);
            int SScan(Context& ctx, RedisCommandFrame& cmd);

            int ZAdd(Context& ctx, RedisCommandFrame& cmd);
            int ZCard(Context& ctx, RedisCommandFrame& cmd);
            int ZCount(Context& ctx, RedisCommandFrame& cmd);
            int ZIncrby(Context& ctx, RedisCommandFrame& cmd);
            int ZRange(Context& ctx, RedisCommandFrame& cmd);
            int ZRangeByScore(Context& ctx, RedisCommandFrame& cmd);
            int ZRank(Context& ctx, RedisCommandFrame& cmd);
            int ZRem(Context& ctx, RedisCommandFrame& cmd);
            int ZRemRangeByRank(Context& ctx, RedisCommandFrame& cmd);
            int ZRemRangeByScore(Context& ctx, RedisCommandFrame& cmd);
            int ZRevRange(Context& ctx, RedisCommandFrame& cmd);
            int ZRevRangeByScore(Context& ctx, RedisCommandFrame& cmd);
            int ZRevRank(Context& ctx, RedisCommandFrame& cmd);
            int ZInterStore(Context& ctx, RedisCommandFrame& cmd);
            int ZUnionStore(Context& ctx, RedisCommandFrame& cmd);
            int ZScore(Context& ctx, RedisCommandFrame& cmd);
            int ZScan(Context& ctx, RedisCommandFrame& cmd);
            int ZLexCount(Context& ctx, RedisCommandFrame& cmd);
            int ZRangeByLex(Context& ctx, RedisCommandFrame& cmd);
            int ZRemRangeByLex(Context& ctx, RedisCommandFrame& cmd);

            int LIndex(Context& ctx, RedisCommandFrame& cmd);
            int LInsert(Context& ctx, RedisCommandFrame& cmd);
            int LLen(Context& ctx, RedisCommandFrame& cmd);
            int LPop(Context& ctx, RedisCommandFrame& cmd);
            int LPush(Context& ctx, RedisCommandFrame& cmd);
            int LPushx(Context& ctx, RedisCommandFrame& cmd);
            int LRange(Context& ctx, RedisCommandFrame& cmd);
            int LRem(Context& ctx, RedisCommandFrame& cmd);
            int LSet(Context& ctx, RedisCommandFrame& cmd);
            int LTrim(Context& ctx, RedisCommandFrame& cmd);
            int RPop(Context& ctx, RedisCommandFrame& cmd);
            int RPopLPush(Context& ctx, RedisCommandFrame& cmd);
            int RPush(Context& ctx, RedisCommandFrame& cmd);
            int RPushx(Context& ctx, RedisCommandFrame& cmd);
            int BLPop(Context& ctx, RedisCommandFrame& cmd);
            int BRPop(Context& ctx, RedisCommandFrame& cmd);
            int BRPopLPush(Context& ctx, RedisCommandFrame& cmd);

            int Eval(Context& ctx, RedisCommandFrame& cmd);
            int EvalSHA(Context& ctx, RedisCommandFrame& cmd);
            int Script(Context& ctx, RedisCommandFrame& cmd);

            int GeoAdd(Context& ctx, RedisCommandFrame& cmd);
            int GeoSearch(Context& ctx, RedisCommandFrame& cmd);

            int Auth(Context& ctx, RedisCommandFrame& cmd);

            int PFAdd(Context& ctx, RedisCommandFrame& cmd);
            int PFCount(Context& ctx, RedisCommandFrame& cmd);
            int PFMerge(Context& ctx, RedisCommandFrame& cmd);

            int DoCall(Context& ctx, RedisCommandHandlerSetting& setting, RedisCommandFrame& cmd);
            RedisCommandHandlerSetting* FindRedisCommandHandlerSetting(RedisCommandFrame& cmd);
            bool ParseConfig(const Properties& props);
            void RenameCommand();
            void FreeClientContext(Context& ctx);
            void AddClientContext(Context& ctx);
            RedisReplyPool& GetRedisReplyPool();

            friend class RedisRequestHandler;
            friend class ExpireCheck;
            friend class ConnectionTimeout;
            friend class LUAInterpreter;
            friend class BlockListTimeout;
            friend class WakeBlockListData;
        public:
            Comms();
            int Init(const CommsConfig& cfg);
            void Start();
            CommsConfig& GetConfig()
            {
                return m_cfg;
            }
            ChannelService& GetChannelService()
            {
                return *m_service;
            }
            Timer& GetTimer()
            {
                return m_service->GetTimer();
            }
            Statistics& GetStatistics()
            {
                return m_stat;
            }
            mmkv::MMKV& GetKVStore()
            {
                return *m_kv_store;
            }
            int Call(Context& ctx, RedisCommandFrame& cmd, CallFlags flags);
            static void WakeBlockedConnCallback(Channel* ch, void * data);
            ~Comms();

    };
    extern Comms* g_db;
OP_NAMESPACE_END

#endif /* DB_HPP_ */
