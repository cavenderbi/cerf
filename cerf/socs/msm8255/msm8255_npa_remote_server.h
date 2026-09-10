#pragma once

#include "../../core/service.h"

#include <cstdint>

class StateReader;
class StateWriter;

class Msm8255NpaRemoteServer : public Service {
public:
    using Service::Service;

    bool ShouldRegister() override;
    void OnReady() override;

    uint32_t ServerProg() const;
    uint32_t ServerVers() const;
    uint32_t ServerCid() const;
    uint32_t CallbackClientCid() const;

    uint32_t AnswerCall(uint32_t in_pa, uint32_t size, uint32_t out_pa,
                        uint32_t out_cap, uint32_t self_pid, uint32_t peer_pid,
                        uint32_t peer_cid);
    uint32_t ConsumeCallbackReply(uint32_t in_pa, uint32_t size,
                                  uint32_t out_pa, uint32_t out_cap);

    void SaveState(StateWriter& w);
    void RestoreState(StateReader& r);

private:
    void ReadDefineNodeArgs(uint32_t body, uint32_t size, uint32_t& callback,
                            uint32_t& object);
    void ReadDefineResourceArgs(uint32_t body, uint32_t size,
                                uint32_t& callback, uint32_t& object);
    void ReadCreateClientArgs(uint32_t body, uint32_t size, uint32_t& type,
                              uint32_t& supplied);
    uint32_t AnswerCreateClient(uint32_t in_pa, uint32_t body, uint32_t size,
                                uint32_t out_pa, uint32_t out_cap,
                                uint32_t self_pid, uint32_t peer_pid,
                                uint32_t peer_cid, uint32_t xid);
    uint32_t EmitCallback(uint32_t out_pa, uint32_t self_pid, uint32_t proc,
                          uint32_t cb_index, uint32_t node);

    uint32_t next_xid_            = 1;
    uint32_t cb_xid_              = 0;
    uint32_t cb_proc_             = 0;
    bool     cb_outstanding_      = false;
    uint32_t next_client_handle_  = 0;
};
