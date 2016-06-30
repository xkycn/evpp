#include "conn.h"

#include <evpp/event_loop.h>
#include <evpp/tcp_client.h>
#include <evpp/tcp_conn.h>
#include <evpp/httpc/request.h>
#include <evpp/httpc/response.h>
#include <evpp/httpc/conn.h>

#include <rapidjson/document.h>

#include "command.h"
#include "option.h"

namespace evnsq {
    static const std::string kNSQMagic = "  V2";
    static const std::string kOK = "OK";

    Conn::Conn(evpp::EventLoop* loop, const Option& ops)
        : loop_(loop), option_(ops), status_(kDisconnected) {}

    Conn::~Conn() {}

    void Conn::ConnectToNSQD(const std::string& addr) {
        tcp_client_ = evpp::TCPClientPtr(new evpp::TCPClient(loop_, addr, std::string("NSQClient-") + addr));
        status_ = kConnecting;
        tcp_client_->SetConnectionCallback(std::bind(&Conn::OnConnection, this, std::placeholders::_1));
        tcp_client_->SetMessageCallback(std::bind(&Conn::OnRecv, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        tcp_client_->Connect();
    }


    void Conn::OnConnection(const evpp::TCPConnPtr& conn) {
        if (conn->IsConnected()) {
            assert(tcp_client_->conn() == conn);
            assert(status_ == kConnecting);
            Identify();
        } else {
            if (conn->IsDisconnecting()) {
                LOG_ERROR << "Connection to " << conn->remote_addr() << " was closed by remote server.";
            } else {
                LOG_ERROR << "Connect to " << conn->remote_addr() << " failed.";
            }
        }
    }

    void Conn::OnRecv(const evpp::TCPConnPtr& conn, evpp::Buffer* buf, evpp::Timestamp ts) {
        if (buf->size() < 4) {
            return;
        }

        size_t size = buf->PeekInt32();
        if (buf->size() < size) {
            // need to read more data
            return;
        }
        buf->Skip(4); // 4 bytes of size
                      //LOG_INFO << "Recv a data from NSQD msg body len=" << size - 4 << " body=[" << std::string(buf->data(), size - 4) << "]";
        int32_t frame_type = buf->ReadInt32();
        switch (status_) {
        case evnsq::Conn::kDisconnected:
            break;
        case evnsq::Conn::kConnecting:
            break;
        case evnsq::Conn::kIdentifying:
            if (buf->NextString(size - sizeof(frame_type)) == kOK) {
                status_ = kConnected;
            } else {
                LOG_ERROR << "Identify ERROR"; // TODO close the connetion?
            }
            break;
        case evnsq::Conn::kConnected:
            OnMessage(size - sizeof(frame_type), frame_type, buf);
            break;
        default:
            break;
        }
    }

    void Conn::OnMessage(size_t message_len, int32_t frame_type, evpp::Buffer* buf) {
        if (frame_type == kFrameTypeResponse) {
            if (strncmp(buf->data(), "_heartbeat_", 11) == 0) {
                LOG_TRACE << "recv heartbeat from nsqd " << tcp_client_->remote_addr();
                Command c;
                c.Nop();
                WriteCommand(c);
            } else {
                LOG_ERROR << "frame_type=" << frame_type << " kFrameTypeResponse. [" << std::string(buf->data(), message_len) << "]";
            }
            buf->Skip(message_len);
            return;
        }

        if (frame_type == kFrameTypeMessage) {
            Message msg;
            msg.Decode(message_len, buf);
            if (msg_fn_) {
                //TODO dispatch msg to a working thread pool
                if (msg_fn_(&msg) == 0) {
                    Finish(msg.id);
                } else {
                    Requeue(msg.id);
                }
            }
            return;
        }

        if (frame_type == kFrameTypeError) {
            //TODO add error processing logic
        }
    }

    void Conn::WriteCommand(const Command& c) {
        evpp::Buffer buf;
        c.WriteTo(&buf);
        tcp_client_->conn()->Send(&buf);
    }

    void Conn::Identify() {
        tcp_client_->conn()->Send(kNSQMagic);
        Command c;
        c.Identify(option_.ToJSON());
        WriteCommand(c);
        status_ = kIdentifying;
    }

    void Conn::Finish(const std::string& id) {
        Command c;
        c.Finish(id);
        WriteCommand(c);
    }

    void Conn::Requeue(const std::string& id) {
        Command c;
        c.Requeue(id, evpp::Duration(0));
        WriteCommand(c);
    }
}
