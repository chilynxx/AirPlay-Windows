#include "airplay/server.h"
#include "airplay/client_session.h"
#include "airplay/hls_session.h"
#include "video/hls_player.h"
#include "airplay/reverse_channel.h"
#include "log.h"

namespace ap::airplay {

bool Server::start(const DeviceContext& ctx, uint16_t port) {
    ctx_ = ctx;
    return tcp_.start(port, [this](ap::net::ClientSocket c) {
        handle_client(std::move(c));
    });
}

void Server::stop() {
    tcp_.stop();
}

void Server::disconnect_clients() {
    tcp_.close_all_clients([this] {
        if (ctx_.hls_player) ctx_.hls_player->stop();
        HlsSessionRegistry::instance().set_active_session("");
        if (ctx_.renderer) {
            ctx_.renderer->clear_session();
            ctx_.renderer->clear_active_device();
        }
    });
}

bool Server::probe_and_recover_clients() {
    bool probed = false;
    std::vector<int> rev_fds;
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        for (auto& [fd, sess] : active_sessions_) {
            if (!sess) continue;
            if (sess->streams) {
                sess->streams->request_keyframe_resync();
                probed = true;
            }
            if (!sess->reverse_session_id.empty()) {
                const int rfd = ReverseChannelRegistry::instance().socket_for(
                    sess->reverse_session_id);
                if (rfd >= 0) rev_fds.push_back(rfd);
            }
        }
    }
    static constexpr const char kKeepAliveProbe[] =
        "GET_PARAMETER rtsp://localhost/ RTSP/1.0\r\n"
        "CSeq: 999\r\n"
        "User-Agent: WinAirPlay/0.5\r\n"
        "Content-Length: 0\r\n\r\n";
    for (int rfd : rev_fds) {
        if (ap::net::send_all(static_cast<socket_t>(rfd),
                              kKeepAliveProbe,
                              static_cast<int>(sizeof(kKeepAliveProbe) - 1)) > 0) {
            probed = true;
        }
    }
    if (probed) {
        LOG_INFO << "Smart Auto-Reconnect: sent RTSP keep-alive probe & requested IDR keyframe flush";
    }
    return probed;
}

bool Server::audio_control_available(const std::string& session_id) {
    if (session_id.empty()) return false;
    std::lock_guard<std::mutex> lock(remotes_mutex_);
    for (const auto& entry : remotes_) if (entry.second->available(session_id)) return true;
    return false;
}

bool Server::control_audio(MediaCommand command, uint32_t position_ms, const std::string& session_id) {
    if (session_id.empty()) return false;
    std::shared_ptr<DacpRemote> selected;
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        for (const auto& entry : remotes_) {
            if (entry.second->available(session_id)) { selected = entry.second; break; }
        }
    }
    return selected && selected->send(command, position_ms, session_id);
}

void Server::handle_client(ap::net::ClientSocket client) {
    if (!ap::net::enable_tcp_keepalive(client.fd, 10, 3, 8)) {
        LOG_WARN << "Could not configure control TCP keepalive: "
                 << ap::net::last_error_string();
    }
    RequestReader reader(client.cancelled);
    Request req;

    std::string peer_ip = client.peer;
    if (!peer_ip.empty() && peer_ip.front() == '[') {
        const auto close_br = peer_ip.find(']');
        peer_ip = (close_br == std::string::npos)
                    ? peer_ip.substr(1)
                    : peer_ip.substr(1, close_br - 1);
    } else {
        const auto colon = peer_ip.rfind(':');
        if (colon != std::string::npos) peer_ip.resize(colon);
    }

    auto remote = std::make_shared<DacpRemote>(peer_ip);
    ClientSession session(*ctx_.identity, peer_ip,
                          static_cast<int>(client.fd), ctx_.renderer,
                          ctx_.mirror_hwaccel);
    session.live = ctx_.live;
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        remotes_[client.fd] = remote;
        active_sessions_[client.fd] = &session;
    }

    while (reader.read(client.fd, req)) {
        if (req.method == "SETUP" || req.method == "ANNOUNCE" || req.method == "RECORD") {
            LOG_INFO << "control " << client.peer << " method=" << req.method
                     << " content-type=" << req.header("content-type")
                     << " DACP-ID-present=" << !req.header("dacp-id").empty()
                     << " Active-Remote-present=" << !req.header("active-remote").empty();
        }
        Response res = dispatch(ctx_, session, req);
        remote->update(req.header("dacp-id"), req.header("active-remote"),
                       session.streams && session.streams->audio_only(),
                       session.streams ? session.streams->session_id() : std::string{});
        if (client.cancelled->load()) break;
        if (res.silent) continue;
        std::string wire = res.serialize();
        if (ap::net::send_all(client.fd, wire.data(),
                              static_cast<int>(wire.size())) < 0) {
            LOG_WARN << client.peer << ": send failed";
            break;
        }
    }
    if (!session.reverse_session_id.empty()) {
        ReverseChannelRegistry::instance().unregister_socket(
            session.reverse_session_id);
    }
    LOG_NOTICE << "control read ended peer=" << client.peer
             << " receiver_cancelled=" << client.cancelled->load()
             << " streams=" << (session.streams ? "yes" : "no");
    const std::string ended_session = session.streams
        ? session.streams->session_id() : std::string{};
    {
        std::lock_guard<std::mutex> lock(remotes_mutex_);
        active_sessions_.erase(client.fd);
        remotes_.erase(client.fd);
    }
    session.streams.reset();
    if (ctx_.renderer && !ended_session.empty()) {
        ctx_.renderer->clear_session_if_active(ended_session);
    }
    LOG_INFO << "closing " << client.peer;
}

} // namespace ap::airplay
