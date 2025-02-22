// SPDX-License-Identifier: GPL-2.0
/*
 * ION Memory Allocator
 *
 * Copyright (C) 2011 Google, Inc.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/genalloc.h>
#include <trace/events/kmem.h>
#ifdef CONFIG_COMPAT
#include "compat_ion.h"
#endif

#include "ion.h"
#ifdef CONFIG_ION_CVITEK
extern void cvi_ion_create_debug_info(struct ion_heap *heap);
#endif
static struct ion_device *internal_dev;
static int heap_id;

/* this function should only be called while dev->lock is held */
static void ion_buffer_add(struct ion_device *dev, struct ion_buffer *buffer)
{
	struct rb_node **p = &dev->buffers.rb_node;
	struct rb_node *parent = NULL;
	struct ion_buffer *entry;

	while (*p) {
		parent = *p;
		entry = rb_entry(parent, struct ion_buffer, node);

		if (buffer < entry) {
			p = &(*p)->rb_left;
		} else if (buffer > entry) {
			p = &(*p)->rb_right;
		} else {
			pr_err("%s: buffer already found.", __func__);
			BUG();
		}
	}

	rb_link_node(&buffer->node, parent, p);
	rb_insert_color(&buffer->node, &dev->buffers);
}

extern void cvi_ion_dump(struct ion_heap *heap);
/* this function should only be called while dev->lock is held */
static struct ion_buffer *ion_buffer_create(struct ion_heap *heap,
					    struct ion_device *dev,
					    unsigned long len,
					    unsigned long flags)
{
	struct ion_buffer *buffer;
	u64 pre_num_of_alloc_bytes;
	int ret;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	buffer->heap = heap;
	buffer->flags = flags;
	buffer->dev = dev;
	buffer->size = len;

	ret = heap->ops->allocate(heap, buffer, len, flags);

	if (ret) {
		if (!(heap->flags & ION_HEAP_FLAG_DEFER_FREE))
			goto err2;

		ion_heap_freelist_drain(heap, 0);
		ret = heap->ops->allocate(heap, buffer, len, flags);
		if (ret)
			goto err2;
	}

	if (!buffer->sg_table) {
		WARN_ONCE(1, "This heap needs to set the sgtable");
		ret = -EINVAL;
		goto err1;
	}

	spin_lock(&heap->stat_lock);
	pre_num_of_alloc_bytes = heap->num_of_alloc_bytes;
	spin_unlock(&heap->stat_lock);
	trace_ion_heap_grow(heap->name, len, pre_num_of_alloc_bytes);

	spin_lock(&heap->stat_lock);
	heap->num_of_buffers++;
	heap->num_of_alloc_bytes += len;
	if (heap->num_of_alloc_bytes > heap->alloc_bytes_wm)
		heap->alloc_bytes_wm = heap->num_of_alloc_bytes;
	spin_unlock(&heap->stat_lock);

	INIT_LIST_HEAD(&buffer->attachments);
	mutex_init(&buffer->lock);
	mutex_lock(&dev->buffer_lock);
	ion_buffer_add(dev, buffer);
	mutex_unlock(&dev->buffer_lock);
	return buffer;

err1:
	heap->ops->free(buffer);
err2:
	cvi_ion_dump(heap);
	kfree(buffer);
	return ERR_PTR(ret);
}

void ion_buffer_destroy(struct ion_buffer *buffer)
{
	u64 pre_num_of_alloc_bytes;

	if (buffer->kmap_cnt > 0) {
		pr_warn_once("%s: buffer still mapped in the kernel\n",
			     __func__);
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
	}
	buffer->heap->ops->free(buffer);

	spin_lock(&buffer->heap->stat_lock);
	pre_num_of_alloc_bytes = buffer->heap->num_of_alloc_bytes;
	spin_unlock(&buffer->heap->stat_lock);
	trace_ion_heap_shrink(buffer->heap->name, buffer->size,
			      pre_num_of_alloc_bytes);

	spin_lock(&buffer->heap->stat_lock);
	buffer->heap->num_of_buffers--;
	buffer->heap->num_of_alloc_bytes -= buffer->size;
	spin_unlock(&buffer->heap->stat_lock);

	kfree(buffer);
}

static void _ion_buffer_destroy(struct ion_buffer *buffer)
{
	struct ion_heap *heap = buffer->heap;
	struct ion_device *dev = buffer->dev;

	mutex_lock(&dev->buffer_lock);
	rb_erase(&buffer->node, &dev->buffers);
	mutex_unlock(&dev->buffer_lock);

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ion_heap_freelist_add(heap, buffer);
	else
		ion_buffer_destroy(buffer);
}

static void *ion_buffer_kmap_get(struct ion_buffer *buffer)
{
	void *vaddr;

	if (buffer->kmap_cnt) {
		buffer->kmap_cnt++;
		return buffer->vaddr;
	}
	vaddr = buffer->heap->ops->map_kernel(buffer->heap, buffer);
	if (WARN_ONCE(!vaddr,
		      "heap->ops->map_kernel should return ERR_PTR on error"))
		return ERR_PTR(-EINVAL);
	if (IS_ERR(vaddr))
		return vaddr;
	buffer->vaddr = vaddr;
	buffer->kmap_cnt++;
	return vaddr;
}

static void ion_buffer_kmap_put(struct ion_buffer *buffer)
{
	buffer->kmap_cnt--;
	if (!buffer->kmap_cnt) {
		buffer->heap->ops->unmap_kernel(buffer->heap, buffer);
		buffer->vaddr = NULL;
	}
}

static struct sg_table *dup_sg_table(struct sg_table *table)
{
	struct sg_table *new_table;
	int ret, i;
	struct scatterlist *sg, *new_sg;

	new_table = kzalloc(sizeof(*new_table), GFP_KERNEL);
	if (!new_table)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(new_table, table->orig_nents, GFP_KERNEL);
	if (ret) {
		kfree(new_table);
		return ERR_PTR(-ENOMEM);
	}

	new_sg = new_table->sgl;
	for_each_sgtable_sg(table, sg, i) {
		memcpy(new_sg, sg, sizeof(*sg));
		new_sg->dma_address = 0;
		new_sg = sg_next(new_sg);
	}

	return new_table;
}

static void free_duped_table(struct sg_table *table)
{
	sg_free_table(table);
	kfree(table);
}

struct ion_dma_buf_attachment {
	struct device *dev;
	struct sg_table *table;
	struct list_head list;
};

static int ion_dma_buf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct ion_dma_buf_attachment *a;
	struct sg_table *table;
	struct ion_buffer *buffer = dmabuf->priv;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = dup_sg_table(buffer->sg_table);
	if (IS_ERR(table)) {
		kfree(a);
		return -ENOMEM;
	}

	a->table = table;
	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->list);

	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void ion_dma_buf_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attachment)
{
	struct ion_dma_buf_attachment *a = attachment->priv;
	struct ion_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);
	free_duped_table(a->table);

	kfree(a);
}

static struct sg_table *ion_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct ion_dma_buf_attachment *a = attachment->priv;
	struct sg_table *table;
	int ret;

	table = a->table;

	ret = dma_map_sgtable(attachment->dev, table, direction, 0);
	if (ret)
		return ERR_PTR(ret);

	return table;
}

static void ion_unmap_dma_buf(struct dma_buf_attachment *attachment,
			      struct sg_table *table,
			      enum dma_data_direction direction)
{
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
}

static int ion_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct ion_buffer *buffer = dmabuf->priv;
	int ret = 0;

	if (!buffer->heap->ops->map_user) {
		pr_err("%s: this heap does not define a method for mapping to userspace\n",
		       __func__);
		return -EINVAL;
	}

	if (!(buffer->flags & ION_FLAG_CACHED))
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	mutex_lock(&buffer->lock);
	/* now map it to userspace */
	ret = buffer->heap->ops->map_user(buffer->heap, buffer, vma);
	mutex_unlock(&buffer->lock);

	if (ret)
		pr_err("%s: failure mapping buffer to userspace\n",
		       __func__);

	return ret;
}

static void ion_dma_buf_release(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;

	_ion_buffer_destroy(buffer);
}

static void *ion_dma_buf_vmap(struct dma_buf *dmabuf)
{
	struct ion_buffer *buffer = dmabuf->priv;

	return buffer->vaddr;
}

static void ion_dma_buf_vunmap(struct dma_buf *dmabuf, void *ptr)
{
}

static int ion_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;
	void *vaddr;
	struct ion_dma_buf_attachment *a;
	int ret = 0;

	/*
	 * TODO: Move this elsewhere because we don't always need a vaddr
	 */
	if (buffer->heap->ops->map_kernel) {
		mutex_lock(&buffer->lock);
		vaddr = ion_buffer_kmap_get(buffer);
		if (IS_ERR(vaddr)) {
			ret = PTR_ERR(vaddr);
			goto unlock;
		}
		mutex_unlock(&buffer->lock);
	}

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list)
		dma_sync_sgtable_for_cpu(a->dev, a->table, direction);

unlock:
	mutex_unlock(&buffer->lock);
	return ret;
}

static int ion_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
				      enum dma_data_direction direction)
{
	struct ion_buffer *buffer = dmabuf->priv;
	struct ion_dma_buf_attachment *a;

	if (buffer->heap->ops->map_kernel) {
		mutex_lock(&buffer->lock);
		ion_buffer_kmap_put(buffer);
		mutex_unlock(&buffer->lock);
	}

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list)
		dma_sync_sgtable_for_device(a->dev, a->table, direction);
	mutex_unlock(&buffer->lock);

	return 0;
}

#ifdef CONFIG_ION_CVITEK
int ion_buf_begin_cpu_access(struct ion_buffer *buffer)
{
	void *vaddr;
	int ret = 0;

	if (!buffer) {
		pr_err("%s, buffer is NULL\n", __func__);
		return -EINVAL;
	}

	/*
	 * TODO: Move this elsewhere because we don't always need a vaddr
	 */
	if (buffer->heap->ops->map_kernel) {
		mutex_lock(&buffer->lock);
		vaddr = ion_buffer_kmap_get(buffer);
		if (IS_ERR(vaddr)) {
			ret = PTR_ERR(vaddr);
			goto unlock;
		}
		mutex_unlock(&buffer->lock);
	}

	mutex_lock(&buffer->lock);

unlock:
	mutex_unlock(&buffer->lock);
	return ret;
}
EXPORT_SYMBOL(ion_buf_begin_cpu_access);

int ion_buf_end_cpu_access(struct ion_buffer *buffer)
{
	if (!buffer) {
		pr_err("%s, buffer is NULL\n", __func__);
		return -EINVAL;
	}

	if (buffer->heap->ops->map_kernel) {
		mutex_lock(&buffer->lock);
		ion_buffer_kmap_put(buffer);
		mutex_unlock(&buffer->lock);
	}

	return 0;
}
EXPORT_SYMBOL(ion_buf_end_cpu_access);
#endif

static const struct dma_buf_ops dma_buf_ops = {
	.map_dma_buf = ion_map_dma_buf,
	.unmap_dma_buf = ion_unmap_dma_buf,
	.mmap = ion_mmap,
	.release = ion_dma_buf_release,
	.attach = ion_dma_buf_attach,
	.detach = ion_dma_buf_detach,
	.begin_cpu_access = ion_dma_buf_begin_cpu_access,
	.end_cpu_access = ion_dma_buf_end_cpu_access,
	.vmap = ion_dma_buf_vmap,
	.vunmap = ion_dma_buf_vunmap,
};

#ifdef CONFIG_ION_CVITEK
static void *_ion_alloc(size_t len, unsigned int heap_id_mask, unsigned int flags)
{
	struct ion_device *dev = internal_dev;
	struct ion_buffer *buffer;
	struct ion_heap *heap;

	pr_debug("%s: len %zu heap_id_mask %u flags %x\n", __func__,
		 len, heap_id_mask, flags);
	/*
	 * traverse the list of heaps available in this system in priority
	 * order.  If the heap type is supported by the client, and matches the
	 * request of the caller allocate from it.  Repeat until allocate has
	 * succeeded or all heaps have been tried
	 */
	len = PAGE_ALIGN(len);

	if (!len)
		return ERR_PTR(-EINVAL);

	down_read(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		/* if the caller didn't specify this heap id */
		if (!((1 << heap->id) & heap_id_mask))
			continue;
		buffer = ion_buffer_create(heap, dev, len, flags);
		if (!IS_ERR(buffer))
			break;
	}
	up_read(&dev->lock);

	if (!buffer)
		return ERR_PTR(-ENODEV);

	return buffer;
}

struct ion_buffer *
ion_alloc_nofd(size_t len, unsigned int heap_id_mask, unsigned int flags)
{
	return _ion_alloc(len, heap_id_mask, flags);
}

void ion_free_nofd(struct ion_buffer *buffer)
{
	if (buffer)
		_ion_buffer_destroy(buffer);
}

int ion_alloc(size_t len, unsigned int heap_id_mask, unsigned int flags, struct ion_buffer **buf)
{
	struct ion_buffer *buffer = NULL;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int fd;
	struct dma_buf *dmabuf;

	buffer = _ion_alloc(len, heap_id_mask, flags);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	exp_info.ops = &dma_buf_ops;
	exp_info.size = buffer->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		_ion_buffer_destroy(buffer);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);

	if (buffer && buf)
		*buf = buffer;

	return fd;
}

static int __update_max_avail_size(struct gen_pool_chunk *chunk,
	u32 size_in_bit, int order, uint64_t *max_avail_size)
{
	unsigned long *map = chunk->bits;
	uint64_t region_len;
	u32 free_index_start;
	u32 free_index_end = 0;

	while (free_index_end < size_in_bit) {
		free_index_start = find_next_zero_bit(map, size_in_bit, free_index_end);
		if (free_index_start >= size_in_bit) {
			break;
		}
		free_index_end = find_next_bit(map, size_in_bit, free_index_start);

		region_len = (free_index_end - free_index_start) << order;
		if (region_len > *max_avail_size) {
			*max_avail_size = region_len;
		}
	}
	return 0;
}

static int _ion_update_memory_statics(struct ion_heap *heap,
	uint64_t *total_size, uint64_t *free_size, uint64_t *max_avail_size)
{
	int end_bit;
	struct gen_pool *pool;
	struct gen_pool_chunk *chunk;
	int order;
	unsigned long chunk_size;
	int ret = 0;

	if (heap->type != ION_HEAP_TYPE_CARVEOUT)
		return -1;

	pool = ion_carveout_get_pool(heap);
	order = pool->min_alloc_order;

	#ifndef CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG
	if (in_nmi())
		return -1;
	#endif

	rcu_read_lock();
	list_for_each_entry_rcu(chunk, &pool->chunks, next_chunk) {
		chunk_size = chunk->end_addr - chunk->start_addr + 1;
		*total_size += chunk_size;
		*free_size += atomic_long_read(&chunk->avail);
		end_bit = chunk_size >> order;
		ret = __update_max_avail_size(chunk, end_bit, order, max_avail_size);
		if (ret) {
			pr_err("%s: update max avail size fail, chunk addr:%llu ret:%d.\n",
				__func__, chunk->start_addr, ret);
		}
	}
	rcu_read_unlock();

	return 0;
}

int ion_get_memory_statics(uint64_t *total_size, uint64_t *free_size, uint64_t *max_avail_size)
{
	struct ion_device *dev = internal_dev;
	struct ion_heap *heap;

	*total_size = 0;
	*free_size = 0;
	*max_avail_size = 0;

	down_read(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		_ion_update_memory_statics(heap, total_size, free_size, max_avail_size);
	}
	up_read(&dev->lock);

	return 0;
}
#else
int ion_alloc(size_t len, unsigned int heap_id_mask, unsigned int flags)
{
	struct ion_device *dev = internal_dev;
	struct ion_buffer *buffer = NULL;
	struct ion_heap *heap;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int fd;
	struct dma_buf *dmabuf;

	pr_debug("%s: len %zu heap_id_mask %u flags %x\n", __func__,
		 len, heap_id_mask, flags);
	/*
	 * traverse the list of heaps available in this system in priority
	 * order.  If the heap type is supported by the client, and matches the
	 * request of the caller allocate from it.  Repeat until allocate has
	 * succeeded or all heaps have been tried
	 */
	len = PAGE_ALIGN(len);

	if (!len)
		return -EINVAL;

	down_read(&dev->lock);
	plist_for_each_entry(heap, &dev->heaps, node) {
		/* if the caller didn't specify this heap id */
		if (!((1 << heap->id) & heap_id_mask))
			continue;
		buffer = ion_buffer_create(heap, dev, len, flags);
		if (!IS_ERR(buffer))
			break;
	}
	up_read(&dev->lock);

	if (!buffer)
		return -ENODEV;

	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	exp_info.ops = &dma_buf_ops;
	exp_info.size = buffer->size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		_ion_buffer_destroy(buffer);
		return PTR_ERR(dmabuf);
	}

	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(dmabuf);

	return fd;
}
#endif

int ion_query_heaps(struct ion_heap_query *query, int is_kernel)
{
	struct ion_device *dev = internal_dev;
	struct ion_heap_data __user *buffer = u64_to_user_ptr(query->heaps);
	int ret = -EINVAL, cnt = 0, max_cnt;
	struct ion_heap *heap;
	struct ion_heap_data hdata;

	memset(&hdata, 0, sizeof(hdata));

	down_read(&dev->lock);
	if (!buffer) {
		query->cnt = dev->heap_cnt;
		ret = 0;
		goto out;
	}

	if (query->cnt <= 0)
		goto out;

	max_cnt = query->cnt;

	plist_for_each_entry(heap, &dev->heaps, node) {
		if(heap->name)
			strncpy(hdata.name, heap->name, MAX_HEAP_NAME);
		hdata.name[sizeof(hdata.name) - 1] = '\0';
		hdata.type = heap->type;
		hdata.heap_id = heap->id;

		if (is_kernel) {
			buffer[cnt] = hdata;
		} else {
			if (copy_to_user(&buffer[cnt], &hdata, sizeof(hdata))) {
				ret = -EFAULT;
				goto out;
			}
		}

		cnt++;
		if (cnt >= max_cnt)
			break;
	}

	query->cnt = cnt;
	ret = 0;
out:
	up_read(&dev->lock);
	return ret;
}

union ion_ioctl_arg {
	struct ion_allocation_data allocation;
	struct ion_heap_query query;
	struct ion_custom_data custom;
};

static int validate_ioctl_arg(unsigned int cmd, union ion_ioctl_arg *arg)
{
	switch (cmd) {
	case ION_IOC_HEAP_QUERY:
		if (arg->query.reserved0 ||
		    arg->query.reserved1 ||
		    arg->query.reserved2)
			return -EINVAL;
		break;
	default:
		break;
	}

	return 0;
}

static long ion_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	union ion_ioctl_arg data;
	struct ion_device *dev = container_of(filp->private_data, struct ion_device, dev);
#ifdef CONFIG_ION_CVITEK
	struct ion_buffer *buffer;
#endif

	if (_IOC_SIZE(cmd) > sizeof(data))
		return -EINVAL;

	/*
	 * The copy_from_user is unconditional here for both read and write
	 * to do the validate. If there is no write for the ioctl, the
	 * buffer is cleared
	 */
	if (copy_from_user(&data, (void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	ret = validate_ioctl_arg(cmd, &data);
	if (ret) {
		pr_warn_once("%s: ioctl validate failed\n", __func__);
		return ret;
	}

	if (!(_IOC_DIR(cmd) & _IOC_WRITE))
		memset(&data, 0, sizeof(data));

	switch (cmd) {
	case ION_IOC_ALLOC:
	{
		int fd;
#ifdef CONFIG_ION_CVITEK
		char *name = vmalloc(MAX_ION_BUFFER_NAME);

		fd = ion_alloc(data.allocation.len,
			       data.allocation.heap_id_mask,
			       data.allocation.flags, &buffer);
#else
		fd = ion_alloc(data.allocation.len,
			       data.allocation.heap_id_mask,
			       data.allocation.flags);
#endif
		if (fd < 0)
			return fd;

		data.allocation.fd = fd;
#ifdef CONFIG_ION_CVITEK
		data.allocation.paddr = buffer->paddr;
		strncpy(name, data.allocation.name, MAX_ION_BUFFER_NAME);
		buffer->name = name;
#endif
		break;
	}
	case ION_IOC_ALLOC_LEGACY:
	{
		int fd;
#ifdef CONFIG_ION_CVITEK
		fd = ion_alloc(data.allocation.len,
			       data.allocation.heap_id_mask,
			       data.allocation.flags, &buffer);
#else
		fd = ion_alloc(data.allocation.len,
			       data.allocation.heap_id_mask,
			       data.allocation.flags);
#endif
		if (fd < 0)
			return fd;

		data.allocation.fd = fd;
		data.allocation.paddr = buffer->paddr;
		break;
	}
	case ION_IOC_HEAP_QUERY:
		ret = ion_query_heaps(&data.query, false);
		break;
	case ION_IOC_CUSTOM:
		if (!dev->custom_ioctl)
			return -ENOTTY;
		ret = dev->custom_ioctl(dev, data.custom.cmd,
					data.custom.arg);
		break;
	default:
		return -ENOTTY;
	}

	if (_IOC_DIR(cmd) & _IOC_READ) {
		if (copy_to_user((void __user *)arg, &data, _IOC_SIZE(cmd)))
			return -EFAULT;
	}
	return ret;
}

static const struct file_operations ion_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = ion_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ion_ioctl,
#endif
};

static int debug_shrink_set(void *data, u64 val)
{
	struct ion_heap *heap = data;
	struct shrink_control sc;
	int objs;

	sc.gfp_mask = GFP_HIGHUSER;
	sc.nr_to_scan = val;

	if (!val) {
		objs = heap->shrinker.count_objects(&heap->shrinker, &sc);
		sc.nr_to_scan = objs;
	}

	heap->shrinker.scan_objects(&heap->shrinker, &sc);
	return 0;
}

static int debug_shrink_get(void *data, u64 *val)
{
	struct ion_heap *heap = data;
	struct shrink_control sc;
	int objs;

	sc.gfp_mask = GFP_HIGHUSER;
	sc.nr_to_scan = 0;

	objs = heap->shrinker.count_objects(&heap->shrinker, &sc);
	*val = objs;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_shrink_fops, debug_shrink_get,
			debug_shrink_set, "%llu\n");

void ion_device_add_heap(struct ion_heap *heap)
{
	struct ion_device *dev = internal_dev;
	int ret;
	struct dentry *heap_root;
	char debug_name[64];

	if (!heap->ops->allocate || !heap->ops->free)
		pr_err("%s: can not add heap with invalid ops struct.\n",
		       __func__);

	spin_lock_init(&heap->free_lock);
	spin_lock_init(&heap->stat_lock);
	heap->free_list_size = 0;

	if (heap->flags & ION_HEAP_FLAG_DEFER_FREE)
		ion_heap_init_deferred_free(heap);

	if ((heap->flags & ION_HEAP_FLAG_DEFER_FREE) || heap->ops->shrink) {
		ret = ion_heap_init_shrinker(heap);
		if (ret)
			pr_err("%s: Failed to register shrinker\n", __func__);
	}

	heap->dev = dev;
	heap->num_of_buffers = 0;
	heap->num_of_alloc_bytes = 0;
	heap->alloc_bytes_wm = 0;

	heap_root = debugfs_create_dir(heap->name, dev->debug_root);
	debugfs_create_u64("num_of_buffers",
			   0444, heap_root,
			   &heap->num_of_buffers);
	debugfs_create_u64("num_of_alloc_bytes",
			   0444,
			   heap_root,
			   &heap->num_of_alloc_bytes);
	debugfs_create_u64("alloc_bytes_wm",
			   0444,
			   heap_root,
			   &heap->alloc_bytes_wm);

	if (heap->shrinker.count_objects &&
	    heap->shrinker.scan_objects) {
		snprintf(debug_name, 64, "%s_shrink", heap->name);
		debugfs_create_file(debug_name,
				    0644,
				    heap_root,
				    heap,
				    &debug_shrink_fops);
	}

	down_write(&dev->lock);
	heap->id = heap_id++;
	/*
	 * use negative heap->id to reverse the priority -- when traversing
	 * the list later attempt higher id numbers first
	 */
	plist_node_init(&heap->node, -heap->id);
	plist_add(&heap->node, &dev->heaps);
#ifdef CONFIG_ION_CVITEK
	cvi_ion_create_debug_info(heap);
#endif

	dev->heap_cnt++;
	up_write(&dev->lock);
}
EXPORT_SYMBOL(ion_device_add_heap);

static int ion_device_create(void)
{
	struct ion_device *idev;
	int ret;

	idev = kzalloc(sizeof(*idev), GFP_KERNEL);
	if (!idev)
		return -ENOMEM;

	idev->dev.minor = MISC_DYNAMIC_MINOR;
	idev->dev.name = "ion";
	idev->dev.fops = &ion_fops;
	idev->dev.parent = NULL;
	ret = misc_register(&idev->dev);
	if (ret) {
		pr_err("ion: failed to register misc device.\n");
		kfree(idev);
		return ret;
	}

	idev->debug_root = debugfs_create_dir("ion", NULL);
	idev->buffers = RB_ROOT;
	mutex_init(&idev->buffer_lock);
	init_rwsem(&idev->lock);
	plist_head_init(&idev->heaps);
	internal_dev = idev;
	return 0;
}
subsys_initcall(ion_device_create);
#ifdef CONFIG_ION_CVITEK
#include <linux/syscalls.h>
void ion_free(pid_t pid, int fd)
{
	ksys_close(fd);
}
EXPORT_SYMBOL(ion_free);
#endif
