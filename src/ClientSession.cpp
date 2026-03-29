#include "ClientSession.h"
#include "BoWWServer.h"

namespace boww {

    void ClientSession::SendStartSignal() {
        if (server_ && is_authenticated_) {
            server_->SendJSON(hdl_, {
                {"type", Protocol::MSG_START}
            });
        }
    }

    void ClientSession::SendStopSignal() {
        if (server_ && is_authenticated_) {
            server_->SendJSON(hdl_, {
                {"type", Protocol::MSG_STOP}
            });
        }
    }

}
