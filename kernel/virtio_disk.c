// Virtio磁盘驱动程序 ----
// 用于QEMU的virtio磁盘设备的驱动程序
// 使用QEMU的MMIO接口与virtio通信
//
// 使用方法：qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

// 头文件包含 ----
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// 宏定义 ----
// virtio MMIO寄存器r的地址
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

// 磁盘结构体定义 ----
static struct disk
{
  // DMA描述符集合（不是环形），驱动程序通过它告诉设备
  // 在哪里读写单个磁盘操作。共有NUM个描述符。
  // 大多数命令由这些描述符的"链"（链表）组成。
  struct virtq_desc *desc;

  // 驱动程序写入描述符编号的环形缓冲区，
  // 这些描述符是驱动程序希望设备处理的。
  // 它只包含每个链的头描述符。环形缓冲区有NUM个元素。
  struct virtq_avail *avail;

  // 设备写入已处理完成的描述符编号的环形缓冲区
  // （只是每个链的头）。共有NUM个已使用环形缓冲区条目。
  struct virtq_used *used;

  // 我们自己的记账信息
  char free[NUM];  // 描述符是否空闲？
  uint16 used_idx; // 我们在used[2..NUM]中已经查看了多远。

  // 跟踪正在进行的操作的信息，
  // 用于完成中断到达时使用。
  // 通过链的第一个描述符索引。
  struct
  {
    struct buf *b; // 缓冲区指针
    char status;   // 状态
  } info[NUM];

  // 磁盘命令头
  // 与描述符一一对应，为了方便
  struct virtio_blk_req ops[NUM];

  // 自旋锁，用于保护磁盘结构
  struct spinlock vdisk_lock;

} disk;

// Virtio磁盘初始化 ----
void virtio_disk_init(void)
{
  uint32 status = 0;

  // 初始化磁盘自旋锁
  initlock(&disk.vdisk_lock, "virtio_disk");

  // 检查设备是否存在并匹配预期的值
  // VIRTIO_MMIO_MAGIC_VALUE: 魔数 0x74726976 ("virt"的ASCII码)，用于确认这是一个Virtio设备
  // VIRTIO_MMIO_VERSION: 版本号，应为2（Legacy接口为1，Modern接口为2）
  // VIRTIO_MMIO_DEVICE_ID: 设备类型ID，2表示块设备（磁盘），1表示网络设备
  // VIRTIO_MMIO_VENDOR_ID: 供应商ID，0x554d4551是QEMU的供应商ID（"QEMU"的ASCII码）
  if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 || // 检查魔数是否正确
      *R(VIRTIO_MMIO_VERSION) != 2 ||              // 检查版本是否为2
      *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||            // 检查是否为块设备
      *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551)     // 检查供应商是否为QEMU
  {
    panic("could not find virtio disk");
  }

  // 重置设备
  // 通过将状态寄存器写入0来重置设备
  // 根据Virtio规范，将状态寄存器设置为0会触发设备重置
  // 重置后设备将回到初始状态，所有配置都会被清除
  *R(VIRTIO_MMIO_STATUS) = status; // status此时为0，所以这是重置操作

  // 设置ACKNOWLEDGE状态位，表示驱动程序已识别设备
  // 这是初始化序列的第一步，告诉设备驱动程序已经发现了它，这个的结果是1
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE; // 设置第0位
  *R(VIRTIO_MMIO_STATUS) = status;       // 将新状态写入设备

  // 设置DRIVER状态位，表示驱动程序可以驱动设备
  // 这是初始化序列的第二步，告诉设备驱动程序准备好接管设备
  status |= VIRTIO_CONFIG_S_DRIVER; // 设置第1位
  *R(VIRTIO_MMIO_STATUS) = status;  // 将新状态写入设备

  // 协商功能特性
  // 读取设备支持的功能特性列表
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  // 清除我们不需要的功能特性位
  // 这是一个位掩码操作，通过将相应位设置为0来禁用功能
  features &= ~(1 << VIRTIO_BLK_F_RO);             // 不使用只读模式
  features &= ~(1 << VIRTIO_BLK_F_SCSI);           // 不使用SCSI命令
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);     // 不使用写缓存使能
  features &= ~(1 << VIRTIO_BLK_F_MQ);             // 不使用多队列
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);         // 不使用任意布局
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);     // 不使用事件索引
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC); // 不使用间接描述符
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;      // 写入我们选择的功能特性

  // 告诉设备功能特性协商已完成
  // 这是初始化序列的第三步，表示功能特性协商完成
  status |= VIRTIO_CONFIG_S_FEATURES_OK; // 设置第3位
  *R(VIRTIO_MMIO_STATUS) = status;       // 将新状态写入设备

  // 重新读取状态以确保FEATURES_OK已设置
  // 设备可能会拒绝我们选择的功能特性，所以需要检查
  status = *R(VIRTIO_MMIO_STATUS);
  if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // 初始化队列0
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // 确保队列0未在使用
  if (*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // 检查最大队列大小
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (max == 0)
    panic("virtio disk has no queue 0");
  if (max < NUM)
    panic("virtio disk max queue too short");

  // 分配并清零队列内存
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if (!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // 设置队列大小
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // 写入物理地址
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // 队列准备就绪
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // 所有NUM个描述符开始时都未使用
  for (int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // 告诉设备我们已完全准备就绪
  // 这是初始化序列的最后一步，表示驱动程序已完成所有初始化
  // 设备现在可以开始处理请求
  status |= VIRTIO_CONFIG_S_DRIVER_OK; // 设置第2位
  *R(VIRTIO_MMIO_STATUS) = status;     // 将新状态写入设备

  // plic.c和trap.c安排来自VIRTIO0_IRQ的中断
}

// 描述符管理函数 ----

// 查找一个空闲描述符，将其标记为非空闲，并返回其索引 ----
static int
alloc_desc()
{
  for (int i = 0; i < NUM; i++)
  {
    if (disk.free[i])
    {
      disk.free[i] = 0;
      return i;
    }
  }
  return -1;
}

// 将描述符标记为空闲 ----
static void
free_desc(int i)
{
  if (i >= NUM)
    panic("free_desc 1");
  if (disk.free[i])
    panic("free_desc 2");
  // 清零描述符字段
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  // 唤醒等待空闲描述符的进程
  wakeup(&disk.free[0]);
}

// 释放一个描述符链 ----
static void
free_chain(int i)
{
  while (1)
  {
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    // 如果有下一个描述符，继续释放
    if (flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// 分配三个描述符（它们不需要是连续的） ----
// 磁盘传输总是使用三个描述符
static int
alloc3_desc(int *idx)
{
  for (int i = 0; i < 3; i++)
  {
    idx[i] = alloc_desc();
    if (idx[i] < 0)
    {
      // 如果分配失败，释放已分配的描述符
      for (int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

// 磁盘读写操作 ----
void virtio_disk_rw(struct buf *b, int write)
{
  // 计算扇区号
  uint64 sector = b->blockno * (BSIZE / 512);

  // 获取磁盘锁
  acquire(&disk.vdisk_lock);

  // 规范的第5.2节说，传统块操作使用三个描述符：
  // 一个用于类型/保留/扇区，一个用于数据，一个用于1字节状态结果。

  // 分配三个描述符
  int idx[3];
  while (1)
  {
    if (alloc3_desc(idx) == 0)
    {
      break;
    }
    // 如果没有可用的描述符，等待
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // 格式化三个描述符
  // qemu的virtio-blk.c会读取它们

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  // 设置请求类型
  if (write)
    buf0->type = VIRTIO_BLK_T_OUT; // 写磁盘
  else
    buf0->type = VIRTIO_BLK_T_IN; // 读磁盘
  buf0->reserved = 0;
  buf0->sector = sector;

  // 第一个描述符：请求头
  disk.desc[idx[0]].addr = (uint64)buf0;
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  // 第二个描述符：数据
  disk.desc[idx[1]].addr = (uint64)b->data;
  disk.desc[idx[1]].len = BSIZE;
  if (write)
    disk.desc[idx[1]].flags = 0; // 设备读取b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // 设备写入b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  // 第三个描述符：状态
  disk.info[idx[0]].status = 0xff; // 设备成功时写入0
  disk.desc[idx[2]].addr = (uint64)&disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // 设备写入状态
  disk.desc[idx[2]].next = 0;

  // 记录struct buf供virtio_disk_intr()使用
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // 告诉设备我们描述符链中的第一个索引
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  // 内存屏障，确保之前的写入完成
  __sync_synchronize();

  // 告诉设备另一个可用环形缓冲区条目可用
  disk.avail->idx += 1; // 不是 % NUM ...

  // 内存屏障，确保之前的写入完成
  __sync_synchronize();

  // 通知设备有新的请求
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // 值是队列号

  // 等待virtio_disk_intr()表示请求已完成
  while (b->disk == 1)
  {
    sleep(b, &disk.vdisk_lock);
  }

  // 清理
  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  // 释放磁盘锁
  release(&disk.vdisk_lock);
}

// 磁盘中断处理程序 ----
void virtio_disk_intr()
{
  // 获取磁盘锁
  acquire(&disk.vdisk_lock);

  // 设备在我们告诉它已看到此中断之前不会引发另一个中断，
  // 以下行执行此操作。
  // 这可能与设备向"已使用"环形缓冲区写入新条目竞争，
  // 在这种情况下，我们可能在此中断中处理新的完成条目，
  // 而在下一次中断中无事可做，这是无害的。
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  // 内存屏障，确保之前的读取完成
  __sync_synchronize();

  // 设备在向已使用环形缓冲区添加条目时
  // 会增加disk.used->idx

  // 处理所有已完成的请求
  while (disk.used_idx != disk.used->idx)
  {
    // 内存屏障，确保读取最新值
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    // 检查状态
    if (disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    // 获取缓冲区并唤醒等待的进程
    struct buf *b = disk.info[id].b;
    b->disk = 0; // 磁盘已完成对缓冲区的操作
    wakeup(b);

    // 更新已使用索引
    disk.used_idx += 1;
  }

  // 释放磁盘锁
  release(&disk.vdisk_lock);
}
