// os/net/rndis_lwip.hpp
// ---------------------------------------------------------------------------
// RNDIS как lwIP netif. Включается только если в проекте есть lwIP
// (ESP-IDF/C3, STM32+lwIP): lwIP владеет TCP/IP, RNDIS — только Ethernet.
//
//   Rndis rndis;
//   RndisNetif netif(rndis, hal);
//   netif.init(cfg);                 // static IP
//   // bulk OUT: rndis.receive(data, len);  (sink уже подключён)
//   // цикл: netif.poll();
// ---------------------------------------------------------------------------
#pragma once

#include "socket.hpp"
#include "rndis_factory.hpp"
#include "../usb/rndis.hpp"
#include "../usb/usb.hpp"

#if __has_include("lwip/netif.h")
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#ifdef MCFRAME_RNDIS_LWIP_DHCP
#include "lwip/dhcp.h"
#endif

namespace embed::net {

class RndisNetif {
public:
    RndisNetif(usb::rndis::Rndis& rndis, usb::Hal& hal) noexcept
        : rndis_(&rndis), hal_(&hal) {}

    Status init(const RndisNetConfig& cfg) noexcept {
        cfg_ = cfg;
        ip4_addr_t ip{}, gw{}, mask{};
        IP4_ADDR(&ip, cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
        IP4_ADDR(&gw, cfg.gw[0], cfg.gw[1], cfg.gw[2], cfg.gw[3]);
        IP4_ADDR(&mask, cfg.mask[0], cfg.mask[1], cfg.mask[2], cfg.mask[3]);
        if (netif_add(&netif_, &ip, &mask, &gw, this,
                      &RndisNetif::netif_init_cb, &RndisNetif::netif_output_cb) == nullptr) {
            return Status::NoMemory;
        }
        netif_.name[0] = 'r';
        netif_.name[1] = 'n';
        netif_set_default(&netif_);
        netif_set_link_up(&netif_);
        netif_set_up(&netif_);
        rndis_->set_sink(&RndisNetif::frame_sink, this);
        inited_ = true;
#ifdef MCFRAME_RNDIS_LWIP_DHCP
        (void)dhcp_start(&netif_);
#endif
        return Status::Ok;
    }

    /// Вызывать из задачи/цикла: отправляет накопленные TX-кадры.
    Status poll() noexcept {
        if (!inited_) return Status::Unavailable;
        (void)rndis_->flush(*hal_);
        return Status::Ok;
    }

    netif& raw() noexcept { return netif_; }

private:
    static err_t netif_init_cb(netif* n) noexcept {
        auto* self = static_cast<RndisNetif*>(n->state);
        if (self == nullptr) return ERR_ARG;
        n->flags |= NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
        n->mtu = self->cfg_.mtu;
        n->output = &etharp_output;
        n->linkoutput = &RndisNetif::netif_output_cb;
        return ERR_OK;
    }

    static err_t netif_output_cb(netif* n, pbuf* p) noexcept {
        auto* self = static_cast<RndisNetif*>(n->state);
        if (self == nullptr || p == nullptr || p->tot_len > usb::rndis::Rndis::kMaxFrame) {
            if (p != nullptr) pbuf_free(p);
            return ERR_MEM;
        }
        uint8_t frame[usb::rndis::Rndis::kMaxFrame]{};
        const u16_t len = p->tot_len;
        if (pbuf_copy_partial(p, frame, len, 0u) != len) {
            pbuf_free(p);
            return ERR_BUF;
        }
        pbuf_free(p);
        const Status st = self->rndis_->send_frame(frame, len);
        if (st != Status::Ok) return (st == Status::NoMemory) ? ERR_MEM : ERR_IF;
        (void)self->rndis_->flush(*self->hal_);
        return ERR_OK;
    }

    static void frame_sink(void* ctx, const uint8_t* frame, Size len) noexcept {
        auto* self = static_cast<RndisNetif*>(ctx);
        if (self == nullptr || !self->inited_ || frame == nullptr) return;
        pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(len), PBUF_POOL);
        if (p == nullptr) { ++self->drops_; return; }
        if (pbuf_take(p, frame, static_cast<u16_t>(len)) != ERR_OK) {
            pbuf_free(p); ++self->drops_; return;
        }
        if (self->netif_.input(p, &self->netif_) != ERR_OK) ++self->drops_;
    }

    usb::rndis::Rndis* rndis_ = nullptr;
    usb::Hal* hal_ = nullptr;
    RndisNetConfig cfg_{};
    netif netif_{};
    bool inited_ = false;
    uint32_t drops_ = 0u;
};

} // namespace embed::net
#endif
