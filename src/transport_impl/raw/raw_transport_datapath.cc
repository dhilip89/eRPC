#include "raw_transport.h"

namespace erpc {

void RawTransport::tx_burst(const tx_burst_item_t* tx_burst_arr,
                            size_t num_pkts) {
  for (size_t i = 0; i < num_pkts; i++) {
    const tx_burst_item_t& item = tx_burst_arr[i];
    const MsgBuffer* msg_buffer = item.msg_buffer;
    assert(msg_buffer->is_valid());  // Can be fake for control packets

    // Verify constant fields of work request
    struct ibv_send_wr& wr = send_wr[i];
    struct ibv_sge* sgl = send_sgl[i];

    assert(wr.next == &send_wr[i + 1]);  // +1 is valid
    assert(wr.opcode == IBV_WR_SEND);
    assert(wr.sg_list == sgl);

    // Set signaling flag. The work request is non-inline by default.
    wr.send_flags = get_signaled_flag();

    size_t pkt_size;
    pkthdr_t* pkthdr;
    if (item.pkt_index == 0) {
      // This is the first packet, so we need only 1 SGE. This can be CR/RFR.
      pkthdr = msg_buffer->get_pkthdr_0();
      sgl[0].addr = reinterpret_cast<uint64_t>(pkthdr);
      sgl[0].length = msg_buffer->get_pkt_size<kMaxDataPerPkt>(0);
      sgl[0].lkey = msg_buffer->buffer.lkey;

      if (kMaxInline > 0 &&
          sgl[0].length <= kMaxInline + MLX5_ETH_INLINE_HEADER_SIZE) {
        wr.send_flags |= IBV_SEND_INLINE;
      }

      pkt_size = sgl[0].length;
      wr.num_sge = 1;
    } else {
      // This is not the first packet, so we need 2 SGEs
      pkthdr = msg_buffer->get_pkthdr_n(item.pkt_index);
      sgl[0].addr = reinterpret_cast<uint64_t>(pkthdr);
      sgl[0].length = static_cast<uint32_t>(sizeof(pkthdr_t));
      sgl[0].lkey = msg_buffer->buffer.lkey;

      size_t offset = item.pkt_index * kMaxDataPerPkt;
      sgl[1].addr = reinterpret_cast<uint64_t>(&msg_buffer->buf[offset]);
      sgl[1].length = std::min(kMaxDataPerPkt, msg_buffer->data_size - offset);
      sgl[1].lkey = msg_buffer->buffer.lkey;

      pkt_size = sgl[0].length + sgl[1].length;
      wr.num_sge = 2;
    }

    // We can do an 8-byte aligned memcpy because the two UDP checksum bytes
    // are already zero.
    static constexpr size_t hdr_copy_sz = kInetHdrsTotSize - 2;
    static_assert(hdr_copy_sz == 40, "");

    memcpy(&pkthdr->headroom[0], reinterpret_cast<uint8_t*>(item.routing_info),
           hdr_copy_sz);

    auto* ipv4_hdr =
        reinterpret_cast<ipv4_hdr_t*>(&pkthdr->headroom[sizeof(eth_hdr_t)]);
    assert(ipv4_hdr->check == 0);
    ipv4_hdr->tot_len = htons(pkt_size - sizeof(eth_hdr_t));
    if (kTesting && item.drop) ipv4_hdr->dst_ip = 0;  // Dropped by switch, fast

    auto* udp_hdr = reinterpret_cast<udp_hdr_t*>(&ipv4_hdr[1]);
    assert(udp_hdr->check == 0);
    udp_hdr->len = htons(pkt_size - sizeof(eth_hdr_t) - sizeof(ipv4_hdr_t));

    LOG_TRACE(
        "eRPC RawTransport: Sending packet (drop = %u). SGE #1 = %u bytes, "
        "SGE #2 = %u bytes. pkthdr = %s. Frame header = %s.\n",
        item.drop, sgl[0].length, (wr.num_sge == 2 ? sgl[1].length : 0),
        pkthdr->to_string().c_str(),
        frame_header_to_string(&pkthdr->headroom[0]).c_str());
  }

  send_wr[num_pkts - 1].next = nullptr;  // Breaker of chains

  struct ibv_send_wr* bad_wr;
  int ret = ibv_post_send(qp, &send_wr[0], &bad_wr);
  assert(ret == 0);
  if (unlikely(ret != 0)) {
    fprintf(stderr, "eRPC: Fatal error. ibv_post_send failed. ret = %d\n", ret);
    exit(-1);
  }

  send_wr[num_pkts - 1].next = &send_wr[num_pkts];  // Restore chain; safe
}

void RawTransport::tx_flush() {}

size_t RawTransport::rx_burst() {
  if (kDumb) {
    cqe_snapshot_t cur_snapshot;
    snapshot_cqe(&recv_cqe_arr[cqe_idx], cur_snapshot);
    const size_t delta = get_cqe_cycle_delta(prev_snapshot, cur_snapshot);
    if (delta == 0 || delta >= kNumRxRingEntries) return 0;

    recv_backlog += delta;
    size_t comps_clamped = recv_backlog < kPostlist ? recv_backlog : kPostlist;
    recv_backlog -= comps_clamped;

    for (size_t i = 0; i < comps_clamped; i++) {
      auto* pkthdr =
          reinterpret_cast<pkthdr_t*>(&ring_extent.buf[recv_head * kRecvSize]);
      __builtin_prefetch(pkthdr, 0, 3);

      LOG_TRACE(
          "eRPC RawTransport: Received pkt. pkthdr = %s. Frame header = %s.\n",
          pkthdr->to_string().c_str(),
          frame_header_to_string(&pkthdr->headroom[0]).c_str());

      recv_head = (recv_head + 1) % kNumRxRingEntries;
    }

    cqe_idx = (cqe_idx + 1) % kRecvCQDepth;
    prev_snapshot = cur_snapshot;
    return comps_clamped;
  } else {
    int ret = ibv_poll_cq(recv_cq, kPostlist, recv_wc);
    assert(ret >= 0);
    return static_cast<size_t>(ret);
  }
}

void RawTransport::post_recvs(size_t num_recvs) {
  assert(num_recvs <= kNumRxRingEntries);  // num_recvs can be 0
  recvs_to_post += num_recvs;

  if (kDumb) {
    if (recvs_to_post < kStridesPerWQE) return;

    int ret = wq_family->recv_burst(wq, &mp_recv_sge[mp_sge_idx], 1);
    _unused(ret);
    assert(ret == 0);
    mp_sge_idx = (mp_sge_idx + 1) % kRQDepth;
    recvs_to_post -= kStridesPerWQE;  // Reset slack counter
  } else {
    if (recvs_to_post < kRecvSlack) return;
    bool use_fast_recv = true;

    if (use_fast_recv) {
      // Construct a special RECV wr that the modded driver understands. Encode
      // the number of required RECVs in its num_sge field.
      struct ibv_recv_wr special_wr;
      special_wr.wr_id = kMagicWrIDForFastRecv;
      special_wr.num_sge = recvs_to_post;

      struct ibv_recv_wr* bad_wr = &special_wr;
      int ret = ibv_post_recv(qp, nullptr, &bad_wr);
      if (unlikely(ret != 0)) {
        fprintf(stderr, "eRPC IBTransport: Post RECV (fast) error %d\n", ret);
        exit(-1);
      }

      // Reset slack counter
      recvs_to_post = 0;
      return;
    }

    // The recvs posted are @first_wr through @last_wr, inclusive
    struct ibv_recv_wr *first_wr, *last_wr, *temp_wr, *bad_wr;

    size_t first_wr_i = recv_head;
    size_t last_wr_i = first_wr_i + (recvs_to_post - 1);
    if (last_wr_i >= kRQDepth) last_wr_i -= kRQDepth;

    first_wr = &recv_wr[first_wr_i];
    last_wr = &recv_wr[last_wr_i];
    temp_wr = last_wr->next;

    last_wr->next = nullptr;  // Breaker of chains

    int ret = ibv_post_recv(qp, first_wr, &bad_wr);
    if (unlikely(ret != 0)) {
      fprintf(stderr, "eRPC IBTransport: Post RECV (normal) error %d\n", ret);
      exit(-1);
    }

    last_wr->next = temp_wr;  // Restore circularity

    // Update RECV head: go to the last wr posted and take 1 more step
    recv_head = last_wr_i;
    recv_head = (recv_head + 1) % kRQDepth;
    recvs_to_post = 0;  // Reset slack counter
    return;
  }

  // Nothing should be here
}
}  // End erpc
