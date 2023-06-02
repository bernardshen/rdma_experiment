#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#include "debug.h"
#include "ib.h"

uint32_t udp_sock_;
struct ibv_context* ib_ctx_;
struct ibv_pd* ib_pd_;
struct ibv_port_attr ib_port_attr_;
struct ibv_device_attr ib_device_attr_;
uint32_t ib_gid_idx_;
union ibv_gid ib_gid_;
struct sockaddr_in* server_addr_list_;
uint16_t udp_port_ = 2333;
volatile int need_stop_ = 0;
struct ibv_ah* remote_ah_;
uint32_t remote_qpn_;

int recv_udp_msg(__OUT UDPMsg* msg,
                 __OUT struct sockaddr_in* src_addr,
                 __OUT socklen_t* src_addr_len) {
  int rc = recvfrom(udp_sock_, msg, sizeof(UDPMsg), 0,
                    (struct sockaddr*)src_addr, src_addr_len);
  if (rc != sizeof(UDPMsg)) {
    return -1;
  }
  return 0;
}

int send_udp_msg(UDPMsg* msg,
                 struct sockaddr_in* dest_addr,
                 socklen_t dest_addr_len) {
  int rc = sendto(udp_sock_, msg, sizeof(UDPMsg), 0,
                  (struct sockaddr*)dest_addr, dest_addr_len);
  if (rc != sizeof(UDPMsg)) {
    return -1;
  }
  return 0;
}

int get_qp_info(struct ibv_qp* qp, __OUT QPInfo* qp_info) {
  qp_info->qp_num = qp->qp_num;
  qp_info->lid = ib_port_attr_.lid;
  qp_info->port_num = 1;
  memset(qp_info->gid, 0, 16);
  return 0;
}

void get_mr_info(struct ibv_mr* mr, __OUT MrInfo* mr_info) {
  mr_info->addr = (uint64_t)mr->addr;
  mr_info->rkey = mr->rkey;
}

struct ibv_qp* create_ud_qp(struct ibv_cq* send_cq, struct ibv_cq* recv_cq) {
  struct ibv_qp* qp;
  struct ibv_qp_init_attr qp_init_attr;
  memset(&qp_init_attr, 0, sizeof(struct ibv_qp_init_attr));
  qp_init_attr.qp_type = IBV_QPT_UD;
  qp_init_attr.sq_sig_all = 0;
  qp_init_attr.send_cq = send_cq;
  qp_init_attr.recv_cq = recv_cq;
  qp_init_attr.cap.max_send_wr = 1024;
  qp_init_attr.cap.max_recv_wr = 1024;
  qp_init_attr.cap.max_send_sge = 16;
  qp_init_attr.cap.max_recv_sge = 16;
  qp_init_attr.cap.max_inline_data = 256;

  qp = ib_create_qp(ib_pd_, &qp_init_attr);
  assert(qp != NULL);

  QPInfo local_qp_info;
  get_qp_info(qp, &local_qp_info);
  int ret = ib_connect_ud_qp(qp, &local_qp_info);

  return qp;
}

int connect_qp(struct ibv_qp* ud_qp, const QPInfo* remote_ud_qp_info) {
  int rc = 0;

  // record AH for UD message
  QPInfo local_ud_qp_info;
  get_qp_info(ud_qp, &local_ud_qp_info);
  struct ibv_ah* ah = ib_create_ah(ib_pd_, remote_ud_qp_info);
  if (ah == NULL) {
    printd(L_ERROR, "ib_create_ah failed: %s", strerror(errno));
  }
  printd(L_INFO, "AH recorded, client: %d, ah: %lx, qpn: %x", client_id,
         (uint64_t)ah, remote_ud_qp_info->qp_num);
  remote_ah_ = ah;
  remote_qpn_ = remote_ud_qp_info->qp_num;
  return 0;
}

int main() {
  int ret = 0;
  udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
  assert(udp_sock_ >= 0);

  ib_ctx_ = ib_get_ctx(0, 1);
  assert(ib_ctx_ != 0);

  ib_pd_ = ibv_alloc_pd(ib_ctx_);
  assert(ib_pd_ != NULL);

  ret = ibv_query_port(ib_ctx_, 1, &ib_port_attr_);
  assert(ret == 0);

  ret = ibv_query_device(ib_ctx_, &ib_device_attr_);
  assert(ret == 0);

  server_addr_list_ = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
  assert(server_addr_list_ != NULL);
  memset(server_addr_list_, 0, sizeof(struct sockaddr_in));

  server_addr_list_[0].sin_family = AF_INET;
  server_addr_list_[0].sin_port = htons(udp_port_);
  server_addr_list_[0].sin_addr.s_addr = htonl(INADDR_ANY);

  char buf[1024];
  struct ibv_mr* mr =
      ibv_reg_mr(ib_pd_, buf, 1024,
                 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                     IBV_ACCESS_REMOTE_WRITE);

  // create multiple ud qps
  struct ibv_cq* ud_send_cq = ibv_create_cq(ib_ctx_, 128, NULL, NULL, 0);
  struct ibv_cq* ud_recv_cq = ibv_create_cq(ib_ctx_, 128, NULL, NULL, 0);
  assert(ud_send_cq != NULL && ud_recv_cq != NULL);
  struct ibv_qp* ud_qp = create_ud_qp(ud_send_cq, ud_recv_cq);

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(udp_port_);
  server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

  UDPMsg request;
  request.type = UDPMSG_REQ_CONNECT;
  get_qp_info(ud_qp, &request.body.conn_info.ud_qp_info);
  printf("[local ud_qp] lid 0x%04x, qpn %d\n",
         request.body.conn_info.ud_qp_info.lid,
         request.body.conn_info.ud_qp_info.qp_num);
  get_mr_info(mr, &request.body.conn_info.mr_info);
  serialize_udpmsg(&request);
  ret = sendto(udp_sock_, &request, sizeof(UDPMsg), 0,
               (struct sockaddr*)&server_addr, sizeof(struct sockaddr_in));
  assert(ret == sizeof(UDPMsg));

  UDPMsg reply;
  ret = recvfrom(udp_sock_, &reply, sizeof(UDPMsg), 0, NULL, NULL);
  deserialize_udpmsg(&reply);
  deserialize_udpmsg(&request);
  assert(reply.type == UDPMSG_REP_CONNECT);
  printf("[remote ud_qp] lid 0x%04x, qpn %d\n",
         reply.body.conn_info.ud_qp_info.lid,
         reply.body.conn_info.ud_qp_info.qp_num);
  remote_ah_ = ib_create_ah(ib_pd_, &reply.body.conn_info.ud_qp_info);
  remote_qpn_ = reply.body.conn_info.ud_qp_info.qp_num;

  // sync
  ret = recvfrom(udp_sock_, &reply, sizeof(UDPMsg), 0, NULL, NULL);

  // test ud send recv
  *(uint64_t*)buf = 1233;
  struct ibv_send_wr sr, *bad_sr;
  struct ibv_sge sge;
  memset(&sr, 0, sizeof(sr));
  memset(&sge, 0, sizeof(sge));
  ib_create_sge((uint64_t)buf, mr->lkey, 256, &sge);
  sr.wr_id = 1023;
  sr.next = NULL;
  sr.sg_list = &sge;
  sr.num_sge = 1;
  sr.opcode = IBV_WR_SEND;
  sr.send_flags = IBV_SEND_SIGNALED;
  sr.wr.ud.ah = remote_ah_;
  sr.wr.ud.remote_qpn = remote_qpn_;
  sr.wr.ud.remote_qkey = IB_UD_QKEY;
  ret = ibv_post_send(ud_qp, &sr, &bad_sr);
  assert(ret == 0);

  struct ibv_wc wc;
  int num_polled = 0;
  do {
    num_polled = ibv_poll_cq(ud_send_cq, 1, &wc);
  } while (num_polled == 0);
  if (wc.status != IBV_WC_SUCCESS) {
    printf("wc %d\n", wc.status);
  }
  assert(wc.status == IBV_WC_SUCCESS);
  assert(wc.opcode == IBV_WC_SEND);
  assert(wc.wr_id == 1023);

  return 0;
}