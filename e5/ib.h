#ifndef _DMC_IB_H_
#define _DMC_IB_H_

#include <infiniband/verbs.h>
#include <stdlib.h>

#define __OUT
#define IB_UD_QKEY (0x11111111)

typedef struct _MrInfo {
  uint64_t addr;
  uint32_t rkey;
} MrInfo;

typedef struct _QPInfo {
  uint32_t qp_num;
  uint16_t lid;
  uint8_t port_num;
  uint8_t gid[16];
  uint8_t gid_idx;
} QPInfo;

typedef struct _ConnInfo {
  QPInfo ud_qp_info;
  QPInfo rc_qp_info;
  MrInfo mr_info;
} ConnInfo;

typedef struct _UDPMsg {
  uint16_t type;
  uint16_t id;
  union {
    ConnInfo conn_info;
    MrInfo mr_info;
    uint64_t sys_start_ts;
  } body;
} UDPMsg;

enum ConnType {
  IB,
  ROCE,
};

enum UDPMsgType {
  UDPMSG_REQ_CONNECT,
  UDPMSG_REP_CONNECT,
  UDPMSG_REQ_ALLOC,
  UDPMSG_REP_ALLOC,
};

struct ibv_context* ib_get_ctx(uint32_t dev_id, uint32_t port_id);
struct ibv_qp* ib_create_qp(struct ibv_pd* ib_pd,
                            struct ibv_qp_init_attr* qp_init_attr);
int ib_connect_ud_qp(struct ibv_qp* local_qp, const QPInfo* local_qp_info);
int ib_connect_rc_qp(struct ibv_qp* local_qp,
                     const QPInfo* local_qp_info,
                     const QPInfo* remote_qp_info,
                     uint8_t conn_type);

void ib_print_gid(const uint8_t* gid);

struct ibv_ah* ib_create_ah(struct ibv_pd* ib_pd, const QPInfo* remote_qp_info);

void ib_print_wr(struct ibv_send_wr* wr_list);

void ib_create_sge(uint64_t local_addr,
                   uint32_t lkey,
                   uint32_t len,
                   __OUT struct ibv_sge* sge);

enum IB_RES_TYPE {
  IB_SR,
  IB_RR,
  IB_SGE,
};

static inline void* ib_zalloc(uint8_t ib_res_type, uint32_t num) {
  void* ret_ptr = NULL;
  switch (ib_res_type) {
    case IB_SR:
      ret_ptr = malloc(sizeof(struct ibv_send_wr) * num);
      memset(ret_ptr, 0, sizeof(struct ibv_send_wr) * num);
      break;
    case IB_RR:
      ret_ptr = malloc(sizeof(struct ibv_recv_wr) * num);
      memset(ret_ptr, 0, sizeof(struct ibv_recv_wr));
    case IB_SGE:
      ret_ptr = malloc(sizeof(struct ibv_sge) * num);
      memset(ret_ptr, 0, sizeof(struct ibv_sge) * num);
      break;
    default:
      ret_ptr = NULL;
  }
  return ret_ptr;
}

void serialize_qp_info(__OUT QPInfo* qp_info);
void deserialize_qp_info(__OUT QPInfo* qp_info);
void serialize_mr_info(__OUT MrInfo* mr_info);
void deserialize_mr_info(__OUT MrInfo* mr_info);
void serialize_conn_info(__OUT ConnInfo* conn_info);
void deserialize_conn_info(__OUT ConnInfo* conn_info);
void serialize_udpmsg(__OUT UDPMsg* msg);
void deserialize_udpmsg(__OUT UDPMsg* msg);

#endif