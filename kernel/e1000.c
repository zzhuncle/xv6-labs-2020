#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

// 网卡的发送数据队列
#define TX_RING_SIZE 16
// 发送数据描述符
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
// Read packets to be transmitted from RAM, and to write received packets to RAM
// This technique is called DMA, for direct memory access, referring to the fact that the E1000 hardware directly writes and reads packets to/from RAM.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

// lab11
// https://blog.csdn.net/LostUnravel/article/details/121437373
// 发送以太网数据帧到网卡
// 函数调用关系: write() -> filewrite() -> sockwrite() -> net_tx_udp() -> net_tx_ip() -> net_tx_eth() -> e1000_transmit()
// 最上层是通过文件描述符的 filewrite() 函数调用到写套接字函数 sockwrite(), 在该函数中会通过 mbufalloc() 函数分配一个缓冲区用于写入数据. 接着会嵌套调用 net_tx_udp(), net_tx_ip() 以及 net_tx_eth() 函数, 依次进行 UDP 报文, IP 数据包以及以太网帧的封装, 最终在 net_tx_eth() 中调用 e1000_transmit() 发送数据到网卡的发送队列, 后续再由网卡硬件完成发送
// Your transmit code must place a pointer to the packet data in a descriptor in the TX (transmit) ring.
// 队列的首尾指针 E1000_TDH E1000_TDT
// 发送首指针指向以及载入输出队列的数据, 由网卡硬件维护并更新该指针. 而发送尾指针则指向第一个软件可以写入的描述符的位置, 即由网卡驱动软件维护该指针
int
e1000_transmit(struct mbuf *m)
{
  //
  // Your code here.
  //
  // the mbuf contains an ethernet frame; program it into
  // the TX descriptor ring so that the e1000 sends it. Stash
  // a pointer so that it can be freed after sending.
  //
  uint32 tail;
  struct tx_desc *desc;
  acquire(&e1000_lock);
  
  // 1. 首先通过读取发送尾指针对应的寄存器 regs[E1000_TDT] 获取到软件可以写入的位置, 也就是后续放入下一个数据帧的发送队列的索引
  tail = regs[E1000_TDT];
  desc = &tx_ring[tail];
  // 2. 检查尾指针指向的描述符是否在状态中写入了 E1000_TXD_STAT_DD标志位, 若遇到 DD 位未被设置的描述符, 则可说明队列已满
  if ((desc->status & E1000_TXD_STAT_DD) == 0) {
    release(&e1000_lock);
    return -1;
  }
  // You will need to ensure that each mbuf is eventually freed, but only after the E1000 has finished transmitting the packet (the E1000 sets the E1000_TXD_STAT_DD bit in the descriptor to indicate this).
  // 3. 检查尾指针执行的描述符对应的缓冲区是否被释放, 若未被释放使用 mbuffree() 进行释放
  if (tx_mbufs[tail])
    mbuffree(tx_mbufs[tail]);
  // 4. 更新尾指针指向的描述符的 addr 字段指向数据帧缓冲区的头部 m->head
  // e1000_transmit() 是网络栈最后进行调用, 在上层 sockwrite() 中分配的缓冲区 m, 同时在一层层封装时会更新缓冲区头部(首地址)的位置字段 m->head
  desc->addr = (uint64)m->head;
  // length 字段记录数据帧的长度 m->len
  desc->length = m->len;
  // 5. 更新尾指针指向的描述符的 cmd 字段
  desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
  // 6. 将数据帧缓冲区 m 记录到缓冲区队列 mbuf 中用于之后的释放
  // 数据帧的缓冲区 m 在此时还未被网卡硬件发送因此不能释放, 因此会将其记录到描述符对应的缓冲区队列中, 当后续尾指针又指向该位置时再将其释放
  tx_mbufs[tail] = m;

  // 最后即更新发送尾指针. 而在这之前需要使用 __sync_synchronize() 来设置内存屏障. 这么做的原因是确保描述符的 cmd 字段设置完成后才会更新尾指针, 避免可能的指令重排导致描述符还未更新完毕就移动了尾指针
  __sync_synchronize();
  regs[E1000_TDT] = (tail + 1) % TX_RING_SIZE;
  release(&e1000_lock);
  return 0;
}

// 对驱动接受数据
// 由网卡的中断处理程序 e1000_intr() 调用, 即网卡硬件收到数据(以太网帧)后会触发中断, 由中断处理程序对数据进行处理. 在 e1000_recv() 中会调用 net_rx() 对收到的数据帧进行解封, 之后会根据数据包类型分别调用 net_rx_ip() 或 net_rx_arp() 进行 IP 数据包或 ARP 数据包的解封装. 对于 IP 数据包, 会进一步调用 net_rx_udp() 解封装 UDP 报文, 然后调用 sockrecvudp() 函数, 其中会调用 mbufq_pushtail() 函数将报文放入一个队列, 在使用 sockread() 读取报文时, 实际上就是通过 mbufq_pophead() 函数从该队列中取出一个报文内容, 而若队列为空, 则会将线程休眠直至队列有数据时被唤醒. 而对于 ARP 报文, 其本身不再有传输层报文, 而是会调用 net_tx_arp() 进行一个 ARP 报文的回复
//  Your e1000_recv() code must scan the RX ring and deliver each new packet's mbuf to the network stack (in net.c) by calling net_rx(). 
// You will then need to allocate a new mbuf and place it into the descriptor
// so that when the E1000 reaches that point in the RX ring again it finds a fresh buffer into which to DMA a new packet.
static void
e1000_recv(void)
{
  //
  // Your code here.
  //
  // Check for packets that have arrived from the e1000
  // Create and deliver an mbuf for each packet (using net_rx()).
  //
  // 1. 首先通过读取接收尾指针对应的寄存器并加 1 取余 (regs[E1000_RDT]+1)%RX_RING_SIZE 获取到软件可以读取的位置, 也就是接收且未被软件处理的第一个数据帧在接收队列的索引. 该位置即软件需要解封装的数据帧的描述符
  int tail = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  struct rx_desc *desc = &rx_ring[tail];

  // 实验指导中指出可能之前到达的数据包超过队列大小, 需要进行处理. 这里考虑通过在 e1000_recv() 添加循环, 从而让一次中断触发后网卡软件会一直将可解封装的数据传递到网络栈, 以避免队列中的可处理数据帧的堆积来避免上述情况
  // 2. 需要检查数据帧状态的 E1000_RXD_STAT_DD 标志位, 以确定当前的数据帧已被网卡硬件处理完毕, 可以由内核解封装
  while ((desc->status & E1000_RXD_STAT_DD)) {
    // 3. 接收缓冲区 rx_mbufs[tail] 中为待处理的数据帧, 首先将其长度记录到描述符的 length 字段, 然后调用 net_rx() 传递给网络栈进行解封装
    if (desc->length > MBUF_SIZE)
      panic("e1000 len");

    rx_mbufs[tail]->len = desc->length;
    net_rx(rx_mbufs[tail]);
    // 4. 调用 mbufalloc() 分配一个新的接收缓冲区替代发送给网络栈的缓冲区, 并更新描述符的 addr 字段, 指向新的缓冲区. 这里也能看出与发送缓冲区队列不同的地方, 发送缓冲区队列初始时全为空指针, 而缓冲区实际由 sockwrite() 分配, 在最后时绑定到缓冲区队列中, 主要是为了方便后续释放缓冲区; 而接收缓冲区队列在初始化时全部都已分配, 由内核解封装后释放内存. 而此处由于缓冲区已经交由网络栈去解封装, 因此需要替换成一个新的缓冲区用于下一次硬件接收数据
    rx_mbufs[tail] = mbufalloc(0);
    if (!rx_mbufs[tail])
      panic("e1000 no mbufs");
    desc->addr = (uint64)rx_mbufs[tail]->head;
    desc->status = 0;

    // 用于循环
    tail = (tail + 1) % RX_RING_SIZE;
    desc = &rx_ring[tail];
  }
  // 5. 最后更新接收尾指针 RDT. 需要注意的是, 正如前文所述, 尾指针需要指向最后一个已被软件处理的描述符, 是终止上述循环时的描述符的前一个. 此外, 此处没有使用 __sync_synchronize() 添加内存屏障(实际上可以添加到更新尾指针前), 原因是考虑到 while 循环的存在, 理论上更新尾指针的语句不会发生指令重排
  regs[E1000_RDT] = (tail - 1) % RX_RING_SIZE;
  // 6. 与发送数据不同, 接收时该函数只会被中断处理函数 e1000_intr() 调用, 因此不会出现并发的情况; 此外, 网卡的接收和发送的数据结构是独立的, 没有共享, 因此无需加锁
}

void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
