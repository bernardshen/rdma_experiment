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
  qp_init_attr.cap.max_send_wr = 1;
  qp_init_attr.cap.max_recv_wr = 128;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;

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
  struct ibv_ah* ah = ib_create_ah(ib_pd_, remote_ud_qp_info);
  if (ah == NULL) {
    printd(L_ERROR, "ib_create_ah failed: %s", strerror(errno));
  }
  printf("AH recorded ah: %lx, qpn: %d\n", (uint64_t)ah,
         remote_ud_qp_info->qp_num);
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

  ret = bind(udp_sock_, (struct sockaddr*)&server_addr_list_[0],
             sizeof(struct sockaddr_in));
  assert(ret >= 0);

  UDPMsg request;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(struct sockaddr_in);
  ret = recv_udp_msg(&request, &client_addr, &client_addr_len);
  deserialize_udpmsg(&request);
  assert(request.type == UDPMSG_REQ_CONNECT);

  UDPMsg reply;
  reply.type = UDPMSG_REP_CONNECT;
  get_qp_info(ud_qp, &reply.body.conn_info.ud_qp_info);
  get_mr_info(mr, &reply.body.conn_info.mr_info);
  serialize_udpmsg(&reply);
  send_udp_msg(&reply, &client_addr, client_addr_len);

  deserialize_udpmsg(&reply);
  printf("[local ud_qp] lid 0x%04x, qpn %d\n",
         reply.body.conn_info.ud_qp_info.lid,
         reply.body.conn_info.ud_qp_info.qp_num);
  printf("[remote ud_qp] lid 0x%04x, qpn %d\n",
         request.body.conn_info.ud_qp_info.lid,
         request.body.conn_info.ud_qp_info.qp_num);
  connect_qp(ud_qp, &request.body.conn_info.ud_qp_info);

  // test ud send recv
  *(uint64_t*)buf = 2333;
  struct ibv_recv_wr rr, *bad_rr;
  struct ibv_sge sge;
  memset(&rr, 0, sizeof(rr));
  memset(&sge, 0, sizeof(sge));
  ib_create_sge((uint64_t)buf, mr->lkey, 296, &sge);
  rr.wr_id = 1023;
  rr.next = NULL;
  rr.sg_list = &sge;
  rr.num_sge = 1;
  ret = ibv_post_recv(ud_qp, &rr, &bad_rr);
  assert(ret == 0);

  // sync
  send_udp_msg(&reply, &client_addr, client_addr_len);

  struct ibv_wc wc;
  int num_polled = 0;
  do {
    num_polled = ibv_poll_cq(ud_recv_cq, 1, &wc);
  } while (num_polled == 0);
  if (wc.status != IBV_WC_SUCCESS) {
    printf("wc status: %d\n", wc.status);
  }
  assert(wc.status == IBV_WC_SUCCESS);
  assert(wc.opcode == IBV_WC_RECV);
  assert(wc.wr_id == 1023);
  assert(*(uint64_t*)((uint64_t)buf + 40) == 1233);
  printf("ud success\n");

  return 0;
}