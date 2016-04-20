#include "Log/Logger.h"

#include <algorithm>
#include "QStore.h"
#include "QCommand.h"
#include "QSlowLog.h"
#include "QClient.h"

// *3  CR LF
// $4  CR LF
// sadd CR LF
// $5  CR LF
// myset CR LF
// $3   CR LF
// 345 CR LF

namespace qedis
{

QClient*  QClient::s_pCurrentClient = 0;

std::set<std::weak_ptr<QClient>, std::owner_less<std::weak_ptr<QClient> > >
          QClient::s_monitors;

BODY_LENGTH_T QClient::_ProcessInlineCmd(const char* buf, size_t bytes)
{
    if (bytes < 2)
        return 0;
    
    BODY_LENGTH_T  len = 0;
    size_t cursor = 0;
    for (cursor = 0; cursor + 1 < bytes; ++ cursor)
    {
        if (buf[cursor] == '\r' && buf[cursor+1] == '\n')
        {
            len = static_cast<BODY_LENGTH_T>(cursor + 2);
            state_ = ParseCmdState::Ready;
            break;
        }
    }
    
    if (state_ == ParseCmdState::Ready)
    {
        QString   param;
        for (size_t i = 0; i < cursor; ++ i)
        {
            if (isblank(buf[i]))
            {
                if (!param.empty())
                {
                    INF << "inline cmd param " << param.c_str();
                    params_.emplace_back(std::move(param));
                    param.clear();
                }
            }
            else
            {
                param.push_back(buf[i]);
            }
        }
        
        DBG << "inline cmd param " << param.c_str();
        params_.emplace_back(std::move(param));
    }
    
    return len;
}

BODY_LENGTH_T QClient::_HandlePacket(AttachedBuffer& buf)
{
    const size_t   bytes = buf.ReadableSize();
    const char* const start = buf.ReadAddr();
    const char* const end   = start + bytes;

    const char* ptr  = start;
    
    {
        auto state = QREPL.GetMasterState();

        // discard all data before request sync;
        // or support service use old data? TODO
        if (state == QReplState_connected)
            return static_cast<BODY_LENGTH_T>(bytes);
            
        if (state == QReplState_wait_rdb)
        {
            //RECV RDB FILE
            if (QREPL.GetRdbSize() == std::size_t(-1))
            {
                ++ ptr; // skip $
                int s;
                if (QParseInt::ok == GetIntUntilCRLF(ptr, end - ptr, s))
                {
                    QREPL.SetRdbSize(s);
                    USR << "recv rdb size " << s;
                    ptr += 2; // skip CRLF
                }
            }
            else
            {
                auto rdb = bytes;
                if (rdb > QREPL.GetRdbSize())
                    rdb = QREPL.GetRdbSize();
                
                QREPL.SaveTmpRdb(start, rdb);
                ptr += rdb;
            }

            return static_cast<BODY_LENGTH_T>(ptr - start);
        }
    }
    
    QParseInt parseIntRet = QParseInt::ok;
    
    switch (state_)
    {
        case ParseCmdState::Init:
            assert (multibulk_ == 0);
            if (*ptr == '*')
            {
                ++ ptr;
                
                parseIntRet = GetIntUntilCRLF(ptr, end - ptr, multibulk_);
                if (parseIntRet == QParseInt::ok)
                {
                    state_ = ParseCmdState::Arglen;
                    ptr += 2; // skip CRLF
                }
                else if (parseIntRet == QParseInt::error)
                {
                    OnError();
                    return 0;
                }
            }
            else
            {
                ptr += _ProcessInlineCmd(ptr, bytes);
            }
                
            break;
                
        case ParseCmdState::Arglen:
            assert(*ptr == '$');
            ++ ptr;
                
            parseIntRet = GetIntUntilCRLF(ptr, end - ptr, paramLen_);
            if (parseIntRet == QParseInt::ok)
            {
                state_ = ParseCmdState::Arg;
                ptr += 2; // skip CRLF
            }
            else if (parseIntRet == QParseInt::error)
            {
                OnError();
                return 0;
            }
                
            break;
                
        case ParseCmdState::Arg:
        {
            const char* crlf = SearchCRLF(ptr, end - ptr);
                
            if (!crlf)
            {
                USR << "wait crlf for arg";
                break;
            }
                
            if (crlf - ptr != paramLen_)
            {
                ERR << "param len said " << paramLen_ << ", but actual get " << (crlf - ptr);
                OnError();
                return 0;
            }
                
            params_.emplace_back(QString(ptr, crlf - ptr));
            if (params_.size() == static_cast<size_t>(multibulk_))
            {
                state_ =  ParseCmdState::Ready;
            }
            else
            {
                state_ =  ParseCmdState::Arglen;
            }
                
            ptr = crlf + 2; // skip CRLF
            break;
        }
                
        default:
            break;
    }
    
    if (state_ != ParseCmdState::Ready)
        return static_cast<BODY_LENGTH_T>(ptr - start);

    /// handle packet
    s_pCurrentClient = this;
    
    QEDIS_DEFER {
        _Reset();
    };

    if (!auth_ && params_[0] != "auth")
    {
        ReplyError(QError_needAuth, &reply_);
        SendPacket(reply_);
        return static_cast<BODY_LENGTH_T>(ptr - start);
    }
    
    QString& cmd = params_[0];
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    
    QSTORE.SelectDB(db_);
    
    DBG << "client " << GetID() << ", cmd " << params_[0].c_str();
    
    FeedMonitors(params_);
    
    const QCommandInfo* info = QCommandTable::GetCommandInfo(params_[0]);
    
    if (!info)
    {
        ReplyError(QError_unknowCmd, &reply_);
        SendPacket(reply_);
        return static_cast<BODY_LENGTH_T>(ptr - start);
    }

    if (IsFlagOn(ClientFlag_multi))
    {
        if (cmd != "multi" &&
            cmd != "exec" &&
            cmd != "watch" &&
            cmd != "unwatch" &&
            cmd != "discard")
        {
            if (!info->CheckParamsCount(static_cast<int>(params_.size())))
            {
                ERR << "queue failed: cmd " << cmd.c_str() << " has params " << params_.size();
                ReplyError(info ? QError_param : QError_unknowCmd, &reply_);
                SendPacket(reply_);
                FlagExecWrong();
            }
            else
            {
                if (!IsFlagOn(ClientFlag_wrongExec))
                    queueCmds_.push_back(params_);
                
                SendPacket("+QUEUED\r\n", 9);
                INF << "queue cmd " << cmd.c_str();
            }
            
            return static_cast<BODY_LENGTH_T>(ptr - start);
        }
    }
    
    QError err = QError_ok;
    if (QREPL.GetMasterState() != QReplState_none &&
        !IsFlagOn(ClientFlag_master) &&
        (info->attr & QCommandAttr::QAttr_write))
    {
        ReplyError(err = QError_readonlySlave, &reply_);
    }
    else
    {
        QSlowLog::Instance().Begin();
        err = QCommandTable::ExecuteCmd(params_,
                                        info,
                                        IsFlagOn(ClientFlag_master) ? nullptr : &reply_);
        QSlowLog::Instance().EndAndStat(params_);
    }
    
    if (!reply_.IsEmpty())
    {
        SendPacket(reply_);
    }
    
    if (err == QError_ok && (info->attr & QAttr_write))
    {
        Propogate(params_);
    }
    
    return static_cast<BODY_LENGTH_T>(ptr - start);
}

QClient*  QClient::Current()
{
    return s_pCurrentClient;
}

QClient::QClient() : db_(0), flag_(0), name_("clientxxx")
{
    auth_ = false;
    SelectDB(0);
    _Reset();
}

void QClient::OnConnect()
{
    if (QSTORE.password_.empty())
        SetAuth();
}

bool QClient::SelectDB(int db)
{ 
    if (QSTORE.SelectDB(db) >= 0)
    {
        db_ = db;
        return true;
    }

    return false;
}

void QClient::_Reset()
{
    s_pCurrentClient = 0;

    state_     = ParseCmdState::Init;
    multibulk_ = 0;
    paramLen_  = 0;
    params_.clear();
    reply_.Clear();
}

// multi
bool QClient::Watch(int dbno, const QString& key)
{
    INF << "Watch " << key.c_str() << ", db no = " << dbno;
    return watchKeys_[dbno].insert(key).second;
}

bool QClient::NotifyDirty(int dbno, const QString& key)
{
    if (IsFlagOn(ClientFlag_dirty))
    {
        INF << "client is already dirty " << GetID();
        return true;
    }
    
    if (watchKeys_[dbno].count(key))
    {
        INF << GetID() << " client become dirty because key " << key << ", dbno " << dbno;
        SetFlag(ClientFlag_dirty);
        return true;
    }
    else
    {
        INF << "Dirty key is not exist: " << key << ", because client unwatch before dirty";
    }
    
    return false;
}

bool QClient::Exec()
{
    QEDIS_DEFER  {
        this->ClearMulti();
        this->ClearWatch();
    };
    
    if (IsFlagOn(ClientFlag_wrongExec))
    {
        return false;
    }
    
    if (IsFlagOn(ClientFlag_dirty))
    {
        FormatNullArray(&reply_);
        return true;
    }
    
    PreFormatMultiBulk(queueCmds_.size(), &reply_);
    for (const auto& cmd : queueCmds_)
    {
        INF << "EXEC " << cmd[0] << ", for " << GetID();
        const QCommandInfo* info = QCommandTable::GetCommandInfo(cmd[0]);
        QError err = QCommandTable::ExecuteCmd(cmd, info, &reply_);
        SendPacket(reply_);
        _Reset();
        
        // may dirty clients;
        if (err == QError_ok && (info->attr & QAttr_write))
        {
            Propogate(cmd);
        }
    }
    
    return true;
}

void QClient::ClearMulti()
{
    queueCmds_.clear();
    ClearFlag(ClientFlag_multi);
    ClearFlag(ClientFlag_wrongExec);
}
    
void QClient::ClearWatch()
{
    watchKeys_.clear();
    ClearFlag(ClientFlag_dirty);
}


bool  QClient::WaitFor(const QString& key, const QString* target)
{
    bool  succ = waitingKeys_.insert(key).second;
    
    if (succ && target)
    {
        if (!target_.empty())
        {
            ERR << "Wait failed for key " << key << ", because old target " << target_;
            waitingKeys_.erase(key);
            return false;
        }
        
        target_ = *target;
    }
    
    return succ;
}


void   QClient::SetSlaveInfo()
{
    slaveInfo_.reset(new QSlaveInfo());
}

void  QClient::AddCurrentToMonitor()
{
    s_monitors.insert(std::static_pointer_cast<QClient>(s_pCurrentClient->shared_from_this()));
}

void  QClient::FeedMonitors(const std::vector<QString>& params)
{
    assert(!params.empty());

    if (s_monitors.empty())
        return;

    char buf[512];
    int n = snprintf(buf, sizeof buf, "+[db%d %s:%hu]: \"",
             QSTORE.GetDB(),
             s_pCurrentClient->peerAddr_.GetIP(),
             s_pCurrentClient->peerAddr_.GetPort());

    assert(n > 0);
    if (n > static_cast<int>(sizeof buf))
    {
        ERR << "why snprintf return " << n << " bigger than buf size " << sizeof buf;
        n = sizeof buf;
    }
    
    for (const auto& e : params)
    {
        if (static_cast<int>(sizeof buf) < n)
            n += snprintf(buf + n, sizeof buf - n, "%s ", e.data());
        else
            break;
    }
    
    -- n; // no space follow last param
    
    for (auto it(s_monitors.begin()); it != s_monitors.end(); )
    {
        auto  m = it->lock();
        if (m)
        {
            m->SendPacket(buf, n);
            m->SendPacket("\"" CRLF, 3);
            
            ++ it;
        }
        else
        {
            s_monitors.erase(it ++);
        }
    }
}
    
}
