// virtio_disk.c
//
// 这是 QEMU virtio 磁盘设备的驱动程序。
// 它使用 QEMU 的内存映射 I/O (MMIO) 接口与 virtio 设备进行通信。
//
// virtio 是一种半虚拟化 I/O 接口，旨在提高虚拟机中 I/O 操作的性能。
// 对于块设备（如磁盘），virtio 接口定义了一组标准，允许虚拟机中的驱动程序
// 与宿主机上的虚拟设备进行高效通信。
//
// QEMU 命令行示例:
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//   -drive: 定义一个磁盘镜像文件 (fs.img)。
//   -device virtio-blk-device: 创建一个 virtio 块设备。
//   -bus virtio-mmio-bus.0: 将设备连接到 virtio MMIO 总线。
//
// 该驱动程序负责：
// 1. 初始化 virtio 磁盘设备，包括协商特性、设置虚拟队列。
// 2. 管理描述符（descriptors），用于描述磁盘操作的缓冲区。
// 3. 处理磁盘读写请求，将缓冲区提交给设备。
// 4. 处理来自设备的完成中断，更新缓冲区状态并唤醒等待进程。
//
// disk 结构体：表示 virtio 块设备的驱动程序状态。
// 包含了与 virtio 设备通信所需的所有数据结构和簿记信息。
//


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

// virtio MMIO 寄存器 r 的地址。
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk {
  // 一组（非环形）DMA 描述符，驱动程序通过它们告诉设备在哪里读写单个磁盘操作。\n  // 共有 NUM 个描述符。\n  // 大多数命令由一个“链”（链表）组成，包含几个这样的描述符。
  struct virtq_desc *desc;

  // 一个环形缓冲区，驱动程序在其中写入希望设备处理的描述符编号。\n  // 它只包含每个链的头部描述符。该环形缓冲区有 NUM 个元素。
  struct virtq_avail *avail;

  // a ring in which the device writes descriptor numbers that
  // the device has finished processing (just the head of each chain).
// virtio_disk_init 函数：初始化 virtio 磁盘设备。
// 该函数执行以下步骤：
// 1. 初始化自旋锁 `disk.vdisk_lock`。
// 2. 检查 virtio MMIO 设备的魔数、版本、设备 ID 和供应商 ID，确保找到正确的设备。
// 3. 重置设备状态。
// 4. 设置 ACKNOWLEDGE 和 DRIVER 状态位，表示驱动程序已识别设备并准备好进行配置。
// 5. 协商设备特性，禁用一些 xv6 不支持或不需要的特性。
// 6. 设置 FEATURES_OK 状态位，表示特性协商完成。
// 7. 初始化队列 0：
//    a. 检查队列是否在使用中。
//    b. 检查最大队列大小。
//    c. 分配并清零描述符、可用环和已用环的内存。
//    d. 设置队列大小。
//    e. 将描述符、可用环和已用环的物理地址写入 MMIO 寄存器。
//    f. 设置 QUEUE_READY 位，使队列准备就绪。
// 8. 将所有描述符标记为未使用。
// 9. 设置 DRIVER_OK 状态位，表示驱动程序已完全准备好。
  // there are NUM used ring entries.
  struct virtq_used *used;

  // 驱动程序自身的簿记信息。\n  char free\[NUM\];  // 描述符是否空闲？\n  uint16 used_idx; // 已处理的 used 环中的索引。

  // 跟踪正在进行的磁盘操作信息，\n  // 用于中断完成时使用。\n  // 通过描述符链的第一个描述符索引进行索引。
  struct {
    struct buf *b;
    char status;
  } info[NUM];

  // disk command headers.
  // one-for-one with descriptors, for convenience.
  struct virtio_blk_req ops[NUM];
  
  struct spinlock vdisk_lock;
  
} disk;

void
virtio_disk_init(void)
{
  uint32 status = 0;

  initlock(&disk.vdisk_lock, "virtio_disk");

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 2 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
    panic("could not find virtio disk");
  }
  
  // reset device
  *R(VIRTIO_MMIO_STATUS) = status;

  // set ACKNOWLEDGE status bit
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  // set DRIVER status bit
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  // negotiate features
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  // tell device that feature negotiation is complete.
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  // re-read status to ensure FEATURES_OK is set.
  status = *R(VIRTIO_MMIO_STATUS);
  if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
    panic("virtio disk FEATURES_OK unset");

  // initialize queue 0.
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

  // ensure queue 0 is not in use.
  if(*R(VIRTIO_MMIO_QUEUE_READY))
    panic("virtio disk should not be ready");

  // check maximum queue size.
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
// alloc_desc 函数：查找并分配一个空闲的描述符。
// 返回空闲描述符的索引，如果所有描述符都被占用则返回 -1。
  if(max == 0)
    panic("virtio disk has no queue 0");
  if(max < NUM)
    panic("virtio disk max queue too short");

  // allocate and zero queue memory.
  disk.desc = kalloc();
  disk.avail = kalloc();
  disk.used = kalloc();
  if(!disk.desc || !disk.avail || !disk.used)
    panic("virtio disk kalloc");
  memset(disk.desc, 0, PGSIZE);
// free_desc 函数：将指定的描述符标记为可用。
// 同时清空描述符的地址、长度、标志和下一个描述符的索引。
// 唤醒可能在等待空闲描述符的进程。
  memset(disk.avail, 0, PGSIZE);
  memset(disk.used, 0, PGSIZE);

  // set queue size.
  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

  // write physical addresses.
  *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
  *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
  *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
  *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
  *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
// free_chain 函数：释放一个描述符链。
// 遍历描述符链，并依次调用 `free_desc` 释放每个描述符。
  *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

  // queue is ready.
  *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

  // all NUM descriptors start out unused.
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  // tell device we're completely ready.
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

// alloc3_desc 函数：分配三个描述符。
// 磁盘传输操作通常需要三个描述符（一个用于请求头，一个用于数据，一个用于状态）。
// 如果无法分配所有三个描述符，则会释放已分配的描述符并返回 -1。
  // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
static int
alloc_desc()
{
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
// virtio_disk_rw 函数：执行 virtio 磁盘的读写操作。
// 参数:
//   b: 指向 `buf` 结构体的指针，包含要读写的数据和块信息。
//   write: 如果为 1，表示写入操作；如果为 0，表示读取操作。
//
// 该函数执行以下步骤：
// 1. 计算要操作的扇区号。
// 2. 获取 `disk.vdisk_lock` 自旋锁，保护对 `disk` 结构体的并发访问。
// 3. 分配三个描述符链（一个用于请求头，一个用于数据，一个用于状态）。
//    如果当前没有足够的空闲描述符，则进程会进入睡眠状态，直到有描述符可用。
// 4. 格式化这三个描述符：
//    a. 第一个描述符用于 `virtio_blk_req` 结构体，包含操作类型（读/写）、保留字段和扇区号。
//    b. 第二个描述符用于数据缓冲区 (`b->data`)。根据 `write` 参数设置读写标志。
//    c. 第三个描述符用于存储设备操作的状态结果（1 字节）。
// 5. 记录 `buf` 结构体，以便在中断处理程序 `virtio_disk_intr` 中使用。
// 6. 将第一个描述符的索引添加到设备的可用环中。
// 7. 更新可用环的索引，并通知设备有新的请求。
// 8. 进程进入睡眠状态，等待 `virtio_disk_intr` 通知请求完成。
// 9. 请求完成后，释放描述符链并释放 `disk.vdisk_lock`。
  return -1;
}

// mark a descriptor as free.
static void
free_desc(int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disk.free[i])
    panic("free_desc 2");
  disk.desc[i].addr = 0;
  disk.desc[i].len = 0;
  disk.desc[i].flags = 0;
  disk.desc[i].next = 0;
  disk.free[i] = 1;
  wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void
free_chain(int i)
{
  while(1){
    int flag = disk.desc[i].flags;
    int nxt = disk.desc[i].next;
    free_desc(i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int
alloc3_desc(int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc();
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(idx[j]);
      return -1;
    }
  }
  return 0;
}

void
virtio_disk_rw(struct buf *b, int write)
{
  uint64 sector = b->blockno * (BSIZE / 512);

  acquire(&disk.vdisk_lock);

  // the spec's Section 5.2 says that legacy block operations use
  // three descriptors: one for type/reserved/sector, one for the
  // data, one for a 1-byte status result.
// virtio_disk_intr 函数：virtio 磁盘设备的中断处理程序。
// 当 virtio 磁盘设备完成一个或多个请求时，会触发此中断。
// 该函数执行以下步骤：
// 1. 获取 `disk.vdisk_lock` 自旋锁。
// 2. 通知设备已处理中断，通过写入 `VIRTIO_MMIO_INTERRUPT_ACK` 寄存器。
// 3. 遍历设备的已用环 (used ring)，处理所有已完成的请求：
//    a. 获取已完成请求的描述符 ID。
//    b. 检查操作状态，如果非零表示出错。
//    c. 将 `buf` 结构体的 `disk` 字段设置为 0，表示磁盘操作完成。
//    d. 唤醒等待该 `buf` 的进程。
//    e. 更新 `disk.used_idx`，指向下一个要处理的已用环条目。
// 4. 释放 `disk.vdisk_lock`。

  // allocate the three descriptors.
  int idx[3];
  while(1){
    if(alloc3_desc(idx) == 0) {
      break;
    }
    sleep(&disk.free[0], &disk.vdisk_lock);
  }

  // format the three descriptors.
  // qemu's virtio-blk.c reads them.

  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

  if(write)
    buf0->type = VIRTIO_BLK_T_OUT; // write the disk
  else
    buf0->type = VIRTIO_BLK_T_IN; // read the disk
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr = (uint64) buf0;
  disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next = idx[1];

  disk.desc[idx[1]].addr = (uint64) b->data;
  disk.desc[idx[1]].len = BSIZE;
  if(write)
    disk.desc[idx[1]].flags = 0; // device reads b->data
  else
    disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
  disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next = idx[2];

  disk.info[idx[0]].status = 0xff; // device writes 0 on success
  disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
  disk.desc[idx[2]].len = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
  disk.desc[idx[2]].next = 0;

  // record struct buf for virtio_disk_intr().
  b->disk = 1;
  disk.info[idx[0]].b = b;

  // tell the device the first index in our chain of descriptors.
  disk.avail->ring[disk.avail->idx % NUM] = idx[0];

  __sync_synchronize();

  // tell the device another avail ring entry is available.
  disk.avail->idx += 1; // not % NUM ...

  __sync_synchronize();

  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

  // Wait for virtio_disk_intr() to say request has finished.
  while(b->disk == 1) {
    sleep(b, &disk.vdisk_lock);
  }

  disk.info[idx[0]].b = 0;
  free_chain(idx[0]);

  release(&disk.vdisk_lock);
}

void
virtio_disk_intr()
{
  acquire(&disk.vdisk_lock);

  // the device won't raise another interrupt until we tell it
  // we've seen this interrupt, which the following line does.
  // this may race with the device writing new entries to
  // the "used" ring, in which case we may process the new
  // completion entries in this interrupt, and have nothing to do
  // in the next interrupt, which is harmless.
  *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

  __sync_synchronize();

  // the device increments disk.used->idx when it
  // adds an entry to the used ring.

  while(disk.used_idx != disk.used->idx){
    __sync_synchronize();
    int id = disk.used->ring[disk.used_idx % NUM].id;

    if(disk.info[id].status != 0)
      panic("virtio_disk_intr status");

    struct buf *b = disk.info[id].b;
    b->disk = 0;   // disk is done with buf
    wakeup(b);

    disk.used_idx += 1;
  }

  release(&disk.vdisk_lock);
}
