#include "ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <stdatomic.h>

#include "log.h"
#include "platform.h"

#define MCAST_GROUP "239.0.0.1"
#define MCAST_PORT 12345

static int sockfd = -1;
static struct sockaddr_in mcast_addr;

static CompletionMessage g_incoming_buf[MESSAGE_QUEUE_SIZE];
static _Atomic uint64_t g_incoming_seq[MESSAGE_QUEUE_SIZE];
static CompletionMessage g_outgoing_buf[MESSAGE_QUEUE_SIZE];
static _Atomic uint64_t g_outgoing_seq[MESSAGE_QUEUE_SIZE];

void ipc_thread_init() {
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("socket() failed");
    exit(EXIT_FAILURE);
  }

  int reuse = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

#ifdef SO_REUSEPORT
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt(SO_REUSEPORT) failed (warning)");
  }
#endif

  struct sockaddr_in local_addr;
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons(MCAST_PORT);
  local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
    perror("bind() failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  struct ip_mreq mreq;
  mreq.imr_multiaddr.s_addr = inet_addr(MCAST_GROUP);
  mreq.imr_interface.s_addr = inet_addr("127.0.0.1");
  if (setsockopt(sockfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    perror("setsockopt(IP_ADD_MEMBERSHIP) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  int flags = fcntl(sockfd, F_GETFL, 0);
  if (flags < 0) {
    perror("fcntl(F_GETFL) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }
  if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
    perror("fcntl(F_SETFL, O_NONBLOCK) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  memset(&mcast_addr, 0, sizeof(mcast_addr));
  mcast_addr.sin_family = AF_INET;
  mcast_addr.sin_port = htons(MCAST_PORT);
  mcast_addr.sin_addr.s_addr = inet_addr(MCAST_GROUP);

  struct in_addr local_if;
  local_if.s_addr = inet_addr("127.0.0.1");
  if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_IF, &local_if,
                 sizeof(local_if)) < 0) {
    perror("setsockopt(IP_MULTICAST_IF) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  unsigned char ttl = 1;
  if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
    perror("setsockopt(IP_MULTICAST_TTL) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  unsigned char loop = 1;
  if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) <
      0) {
    perror("setsockopt(IP_MULTICAST_LOOP) failed");
    close(sockfd);
    exit(EXIT_FAILURE);
  }

  ring_buffer_init(&processor_state.incoming_completion_msg_queue,
                   MESSAGE_QUEUE_SIZE, g_incoming_buf, g_incoming_seq,
                   sizeof(CompletionMessage));
  ring_buffer_init(&processor_state.outgoing_completion_msg_queue,
                   MESSAGE_QUEUE_SIZE, g_outgoing_buf, g_outgoing_seq,
                   sizeof(CompletionMessage));

  LOG(LOG_LEVEL_INFO,
      "IPC thread initialized. Multicasting to %s:%d (loopback-only)",
      MCAST_GROUP, MCAST_PORT);
}

void ipc_receive_completion_messages() {
  LOG(LOG_LEVEL_DEBUG, "Checking for incoming completion messages...");
  char packet_buf[1 + (MESSAGE_QUEUE_SIZE * sizeof(CompletionMessage))];
  ssize_t len;
  struct sockaddr_in sender_addr;
  socklen_t sender_len = sizeof(sender_addr);

  while (1) {
    len = recvfrom(sockfd, packet_buf, sizeof(packet_buf), 0,
                   (struct sockaddr *)&sender_addr, &sender_len);

    if (len < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        break;
      }
      perror("recvfrom() error");
      break;
    }

    PacketType pkt_type = (PacketType)packet_buf[0];
    char *payload = packet_buf + 1;
    ssize_t payload_len = len - 1;

    if (pkt_type == PACKET_TYPE_CRITICALITY_CHANGE &&
        payload_len == sizeof(CriticalityChangeMessage)) {
      CriticalityChangeMessage msg;
      memcpy(&msg, payload, sizeof(CriticalityChangeMessage));

      if (msg.new_level >
              atomic_load(&processor_state.system_criticality_level) &&
          msg.new_level < MAX_CRITICALITY_LEVELS) {
        LOG(LOG_LEVEL_WARN,
            "Received criticality change to level %d from %s:%d", msg.new_level,
            inet_ntoa(sender_addr.sin_addr), ntohs(sender_addr.sin_port));
        atomic_store(&processor_state.system_criticality_level, msg.new_level);
      }
    } else if (pkt_type == PACKET_TYPE_COMPLETION) {
      int num_msgs = payload_len / sizeof(CompletionMessage);

      for (int i = 0; i < num_msgs; i++) {
        CompletionMessage msg;
        memcpy(&msg, payload + (i * sizeof(CompletionMessage)),
               sizeof(CompletionMessage));

        LOG(LOG_LEVEL_DEBUG,
            "Received completion message for task ID %d from %s:%d",
            msg.completed_task_id, inet_ntoa(sender_addr.sin_addr),
            ntohs(sender_addr.sin_port));
        ring_buffer_enqueue(&processor_state.incoming_completion_msg_queue,
                            &msg);
      }
    } else {
      LOG(LOG_LEVEL_WARN, "Received unknown packet type %d from %s:%d",
          pkt_type, inet_ntoa(sender_addr.sin_addr),
          ntohs(sender_addr.sin_port));
      continue;
    }
  }
}

void ipc_broadcast_criticality_change(CriticalityLevel new_level) {
  char packet[1 + sizeof(CriticalityChangeMessage)];
  packet[0] = PACKET_TYPE_CRITICALITY_CHANGE;
  CriticalityChangeMessage msg = {.new_level = new_level};
  memcpy(packet + 1, &msg, sizeof(CriticalityChangeMessage));
  sendto(sockfd, packet, sizeof(packet), 0, (struct sockaddr *)&mcast_addr,
         sizeof(mcast_addr));
}

void ipc_send_completion_messages() {
  char packet_buf[1 + (MESSAGE_QUEUE_SIZE * sizeof(CompletionMessage))];

  int num_msgs = 0;

  packet_buf[0] = PACKET_TYPE_COMPLETION;

  while (num_msgs < MESSAGE_QUEUE_SIZE) {
    CompletionMessage msg;
    if (ring_buffer_try_dequeue(&processor_state.outgoing_completion_msg_queue,
                                &msg) == 0) {
      memcpy(packet_buf + 1 + (num_msgs * sizeof(CompletionMessage)), &msg,
             sizeof(CompletionMessage));
      LOG(LOG_LEVEL_DEBUG,
          "Queued completion message for task ID %d for sending",
          msg.completed_task_id);
      num_msgs++;
    } else {
      break;
    }
  }

  if (num_msgs > 0) {
    ssize_t len = 1 + (num_msgs * sizeof(CompletionMessage));

    ssize_t sent_len =
        sendto(sockfd, packet_buf, len, 0, (struct sockaddr *)&mcast_addr,
               sizeof(mcast_addr));

    if (sent_len < 0) {
      perror("sendto() failed");
    } else if (sent_len != len) {
      fprintf(stderr, "Warning: sendto() sent partial packet!\n");
    }
  }
}

void ipc_cleanup() {
  if (sockfd >= 0) {
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MCAST_GROUP);
    mreq.imr_interface.s_addr = inet_addr("127.0.0.1");
    setsockopt(sockfd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

    close(sockfd);
    sockfd = -1;
  }
}
