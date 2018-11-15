/*
 * Copyright © 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including
 * the next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "fmm.h"
#include "linux/kfd_ioctl.h"
#include "libhsakmt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <errno.h>
#include <pci/pci.h>
#include <numaif.h>
#ifndef MPOL_F_STATIC_NODES
/* Bug in numaif.h, this should be defined in there. Definition copied
 * from linux/mempolicy.h.
 */
#define MPOL_F_STATIC_NODES     (1 << 15)
#endif

#define NON_VALID_GPU_ID 0

#ifdef __FreeBSD__
#ifndef MAP_NORESERVE
#define MAP_NORESERVE MAP_RESERVED0040
#endif
#endif

#define INIT_MANAGEABLE_APERTURE(base_value, limit_value) {	\
	.base = (void *) base_value,				\
	.limit = (void *) limit_value,				\
	.align = 0,						\
	.guard_pages = 1,					\
	.vm_ranges = NULL,					\
	.vm_objects = NULL,					\
	.fmm_mutex = PTHREAD_MUTEX_INITIALIZER,			\
	.is_coherent = false					\
	}

struct vm_object {
	void *start;
	void *userptr;
	uint64_t userptr_size;
	uint64_t size; /* size allocated on GPU. When the user requests a random
			* size, Thunk aligns it to page size and allocates this
			* aligned size on GPU
			*/
	uint64_t handle; /* opaque */
	uint32_t node_id;
	struct vm_object *next;
	struct vm_object *prev;
	uint32_t flags; /* memory allocation flags */
	/* Registered nodes to map on SVM mGPU */
	uint32_t *registered_device_id_array;
	uint32_t registered_device_id_array_size;
	uint32_t *registered_node_id_array;
	uint32_t registration_count; /* the same memory region can be registered multiple times */
	/* Nodes that mapped already */
	uint32_t *mapped_device_id_array;
	uint32_t mapped_device_id_array_size;
	uint32_t *mapped_node_id_array;
	uint32_t mapping_count;
	/* Metadata of imported graphics buffers */
	void *metadata;
	/* User data associated with the memory */
	void *user_data;
	/* Flag to indicate imported KFD buffer */
	bool is_imported_kfd_bo;
};
typedef struct vm_object vm_object_t;

struct vm_area {
	void *start;
	void *end;
	struct vm_area *next;
	struct vm_area *prev;
};
typedef struct vm_area vm_area_t;

/* Memory manager for an aperture */
typedef struct {
	void *base;
	void *limit;
	uint64_t align;
	uint32_t guard_pages;
	vm_area_t *vm_ranges;
	vm_object_t *vm_objects;
	pthread_mutex_t fmm_mutex;
	bool is_coherent;
} manageable_aperture_t;

typedef struct {
	void *base;
	void *limit;
} aperture_t;

typedef struct {
	uint32_t gpu_id;
	uint32_t device_id;
	uint32_t node_id;
	uint64_t local_mem_size;
	aperture_t lds_aperture;
	manageable_aperture_t scratch_aperture;
	manageable_aperture_t scratch_physical; /* For dGPU, scratch physical is allocated from
						 * dgpu_aperture. When requested by RT, each
						 * GPU will get a differnt range
						 */
	manageable_aperture_t gpuvm_aperture;   /* used for GPUVM on APU, outsidethe canonical address range */
	int drm_render_fd;
} gpu_mem_t;

/* The main structure for dGPU Shared Virtual Memory Management */
typedef struct {
	/* used for non-coherent system and invisible device mem on dGPU.
	 * This aperture is shared by all dGPUs
	 */
	manageable_aperture_t dgpu_aperture;

	/* used for coherent (fine-grain) system memory on dGPU,
	 * This aperture is shared by all dGPUs
	 */
	manageable_aperture_t dgpu_alt_aperture;

	/* whether to use userptr for paged memory */
	bool userptr_for_paged_mem;

	/* whether to check userptrs on registration */
	bool check_userptr;
} svm_t;

/* The other apertures are specific to each GPU. gpu_mem_t manages GPU
 * specific memory apertures.
 */
static gpu_mem_t *gpu_mem;
static unsigned int gpu_mem_count;
static bool hsa_debug;
static void *dgpu_shared_aperture_base;
static void *dgpu_shared_aperture_limit;

static svm_t svm = {
	INIT_MANAGEABLE_APERTURE(0, 0),
	INIT_MANAGEABLE_APERTURE(0, 0),
	true,
	false
};

/* On APU, for memory allocated on the system memory that GPU doesn't access
 * via GPU driver, they are not managed by GPUVM. cpuvm_aperture keeps track
 * of this part of memory.
 */
static manageable_aperture_t cpuvm_aperture = INIT_MANAGEABLE_APERTURE(0, 0);

/* GPU node array for default mappings */
static uint32_t all_gpu_id_array_size;
static uint32_t *all_gpu_id_array;

/* IPC structures and helper functions */
typedef enum _HSA_APERTURE {
	HSA_APERTURE_UNSUPPORTED = 0,
	HSA_APERTURE_DGPU,
	HSA_APERTURE_DGPU_ALT,
	HSA_APERTURE_GPUVM,
	HSA_APERTURE_CPUVM
} HSA_APERTURE;

typedef struct _HsaApertureInfo {
	HSA_APERTURE	type;		// Aperture type
	HSAuint32	idx;		// Aperture index
} HsaApertureInfo;

typedef struct _HsaSharedMemoryStruct {
	HSAuint32	ShareHandle[4];
	HsaApertureInfo	ApeInfo;
	HSAuint32	SizeInPages;
	HSAuint32	ExportGpuId;
} HsaSharedMemoryStruct;

static inline const HsaSharedMemoryStruct *to_const_hsa_shared_memory_struct(
			const HsaSharedMemoryHandle *SharedMemoryHandle)
{
	return (const HsaSharedMemoryStruct *)SharedMemoryHandle;
}

static inline HsaSharedMemoryStruct *to_hsa_shared_memory_struct(
			HsaSharedMemoryHandle *SharedMemoryHandle)
{
	return (HsaSharedMemoryStruct *)SharedMemoryHandle;
}

static inline HsaSharedMemoryHandle *to_hsa_shared_memory_handle(
			HsaSharedMemoryStruct *SharedMemoryStruct)
{
	return (HsaSharedMemoryHandle *)SharedMemoryStruct;
}

extern int debug_get_reg_status(uint32_t node_id, bool *is_debugged);
static void __fmm_release(void *address, manageable_aperture_t *aperture);
static int _fmm_unmap_from_gpu_scratch(uint32_t gpu_id,
				       manageable_aperture_t *aperture,
				       void *address);
static void print_device_id_array(uint32_t *device_id_array, uint32_t device_id_array_size);

static int32_t find_first_dgpu(HSAuint32 *gpu_id)
{
	int32_t i;

	*gpu_id = NON_VALID_GPU_ID;

	for (i = 0; i < NUM_OF_SUPPORTED_GPUS; i++) {
		if (gpu_mem[i].gpu_id == NON_VALID_GPU_ID)
			continue;
		if (!topology_is_dgpu(gpu_mem[i].device_id))
			continue;
		*gpu_id = gpu_mem[i].gpu_id;
		return i;
	}

	return -1;
}

static vm_area_t *vm_create_and_init_area(void *start, void *end)
{
	vm_area_t *area = (vm_area_t *) malloc(sizeof(vm_area_t));

	if (area) {
		area->start = start;
		area->end = end;
		area->next = area->prev = NULL;
	}

	return area;
}

static vm_object_t *vm_create_and_init_object(void *start, uint64_t size,
					      uint64_t handle, uint32_t flags)
{
	vm_object_t *object = (vm_object_t *) malloc(sizeof(vm_object_t));

	if (object) {
		object->start = start;
		object->userptr = NULL;
		object->userptr_size = 0;
		object->size = size;
		object->handle = handle;
		object->next = object->prev = NULL;
		object->registered_device_id_array_size = 0;
		object->mapped_device_id_array_size = 0;
		object->registered_device_id_array = NULL;
		object->mapped_device_id_array = NULL;
		object->registered_node_id_array = NULL;
		object->mapped_node_id_array = NULL;
		object->registration_count = 0;
		object->mapping_count = 0;
		object->flags = flags;
		object->metadata = NULL;
		object->user_data = NULL;
		object->is_imported_kfd_bo = false;
	}

	return object;
}


static void vm_remove_area(manageable_aperture_t *app, vm_area_t *area)
{
	vm_area_t *next;
	vm_area_t *prev;

	next = area->next;
	prev = area->prev;

	if (!prev) /* The first element */
		app->vm_ranges = next;
	else
		prev->next = next;

	if (next) /* If not the last element */
		next->prev = prev;

	free(area);
}

static void vm_remove_object(manageable_aperture_t *app, vm_object_t *object)
{
	vm_object_t *next;
	vm_object_t *prev;

	/* Free allocations inside the object */
	if (object->registered_device_id_array)
		free(object->registered_device_id_array);

	if (object->mapped_device_id_array)
		free(object->mapped_device_id_array);

	if (object->metadata)
		free(object->metadata);

	if (object->registered_node_id_array)
		free(object->registered_node_id_array);
	if (object->mapped_node_id_array)
		free(object->mapped_node_id_array);

	next = object->next;
	prev = object->prev;

	if (!prev) /* The first element */
		app->vm_objects = next;
	else
		prev->next = next;

	if (next) /* If not the last element */
		next->prev = prev;

	free(object);
}

static void vm_add_area_after(vm_area_t *after_this, vm_area_t *new_area)
{
	vm_area_t *next = after_this->next;

	after_this->next = new_area;
	new_area->next = next;

	new_area->prev = after_this;
	if (next)
		next->prev = new_area;
}

static void vm_add_object_before(vm_object_t *before_this,
				vm_object_t *new_object)
{
	vm_object_t *prev = before_this->prev;

	before_this->prev = new_object;
	new_object->next = before_this;

	new_object->prev = prev;
	if (prev)
		prev->next = new_object;
}

static void vm_split_area(manageable_aperture_t *app, vm_area_t *area,
				void *address, uint64_t MemorySizeInBytes)
{
	/*
	 * The existing area is split to: [area->start, address - 1]
	 * and [address + MemorySizeInBytes, area->end]
	 */
	vm_area_t *new_area = vm_create_and_init_area(
				VOID_PTR_ADD(address, MemorySizeInBytes),
				area->end);

	/* Shrink the existing area */
	area->end = VOID_PTR_SUB(address, 1);

	vm_add_area_after(area, new_area);
}

static vm_object_t *vm_find_object_by_address(manageable_aperture_t *app,
					const void *address, uint64_t size)
{
	vm_object_t *cur = app->vm_objects;

	size = ALIGN_UP(size, app->align);

	/* Look up the appropriate address range containing the given address */
	while (cur) {
		if (cur->start == address && (cur->size == size || size == 0))
			break;
		cur = cur->next;
	}

	return cur; /* NULL if not found */
}

static vm_object_t *vm_find_object_by_address_range(manageable_aperture_t *app,
						    const void *address)
{
	vm_object_t *cur = app->vm_objects;

	while (cur) {
		if (address >= cur->start &&
			(uint64_t)address < ((uint64_t)cur->start + cur->size))
			break;
		cur = cur->next;
	}

	return cur; /* NULL if not found */
}

static vm_object_t *vm_find_object_by_userptr(manageable_aperture_t *app,
					const void *address, HSAuint64 size)
{
	vm_object_t *cur = app->vm_objects, *obj;
	uint32_t found = 0;

	/* Look up the userptr that matches the address. If size is specified,
	 * the size needs to match too.
	 */
	while (cur) {
		if ((cur->userptr == address) &&
				((cur->userptr_size == size) || !size)) {
			found = 1;
			break;
		}
		cur = cur->next;
	}

	/* If size is not specified, we need to ensure the vm_obj found is the
	 * only obj having this address.
	 */
	if (found && !size) {
		obj = cur->next;
		while (obj) {
			if (obj->userptr == address) {
				cur = NULL;
				break;
			}
			obj = obj->next;
		}
	}

	return cur; /* NULL if any look-up failure */
}

static vm_object_t *vm_find_object_by_userptr_range(manageable_aperture_t *app,
						const void *address)
{
	vm_object_t *cur = app->vm_objects;

	/* Look up the appropriate address range containing the given address */
	while (cur) {
		if (address >= cur->userptr &&
		(uint64_t)address < (uint64_t)cur->userptr + cur->userptr_size)
			break;
		cur = cur->next;
	}

	return cur; /* NULL if not found */
}

static vm_area_t *vm_find(manageable_aperture_t *app, void *address)
{
	vm_area_t *cur = app->vm_ranges;

	/* Look up the appropriate address range containing the given address */
	while (cur) {
		if (cur->start <= address && cur->end >= address)
			break;
		cur = cur->next;
	};

	return cur; /* NULL if not found */
}

static bool aperture_is_valid(void *app_base, void *app_limit)
{
	if (app_base && app_limit && app_base < app_limit)
		return true;
	return false;
}

/* Align size of a VM area
 *
 * Leave at least one guard page after every object to catch
 * out-of-bounds accesses with VM faults.
 */
static uint64_t vm_align_area_size(manageable_aperture_t *app, uint64_t size)
{
	return ALIGN_UP(size + (uint64_t)app->guard_pages * PAGE_SIZE,
			app->align);
}

/*
 * Assumes that fmm_mutex is locked on entry.
 */
static void aperture_release_area(manageable_aperture_t *app, void *address,
					uint64_t MemorySizeInBytes)
{
	vm_area_t *area;
	uint64_t SizeOfRegion;

	MemorySizeInBytes = vm_align_area_size(app, MemorySizeInBytes);

	area = vm_find(app, address);
	if (!area)
		return;

	SizeOfRegion = VOID_PTRS_SUB(area->end, area->start) + 1;

	/* check if block is whole region or part of it */
	if (SizeOfRegion == MemorySizeInBytes) {
		vm_remove_area(app, area);
	} else if (SizeOfRegion > MemorySizeInBytes) {
		/* shrink from the start */
		if (area->start == address)
			area->start =
				VOID_PTR_ADD(area->start, MemorySizeInBytes);
		/* shrink from the end */
		else if (VOID_PTRS_SUB(area->end, address) + 1 ==
				MemorySizeInBytes)
			area->end = VOID_PTR_SUB(area->end, MemorySizeInBytes);
		/* split the area */
		else
			vm_split_area(app, area, address, MemorySizeInBytes);
	}
}

/*
 * returns allocated address or NULL. Assumes, that fmm_mutex is locked
 * on entry.
 */
static void *aperture_allocate_area_aligned(manageable_aperture_t *app,
					    uint64_t MemorySizeInBytes,
					    uint64_t offset,
					    uint64_t align)
{
	vm_area_t *cur, *next;
	void *start;

	if (align < app->align)
		align = app->align;

	/* Align big buffers to the next power-of-2 up to huge page
	 * size for flexible fragment size TLB optimizations
	 */
	while (align < GPU_HUGE_PAGE_SIZE && MemorySizeInBytes >= (align << 1))
		align <<= 1;

	MemorySizeInBytes = vm_align_area_size(app, MemorySizeInBytes);

	/* Find a big enough "hole" in the address space */
	cur = NULL;
	next = app->vm_ranges;
	start = (void *)ALIGN_UP((uint64_t)VOID_PTR_ADD(app->base, offset),
				 align);
	while (next) {
		if (next->start > start &&
		    VOID_PTRS_SUB(next->start, start) >= MemorySizeInBytes)
			break;

		cur = next;
		next = next->next;
		start = (void *)ALIGN_UP((uint64_t)cur->end + 1, align);
	}
	if (!next && VOID_PTRS_SUB(app->limit, start) + 1 < MemorySizeInBytes)
		/* No hole found and not enough space after the last area */
		return NULL;

	if (cur && VOID_PTR_ADD(cur->end, 1) == start) {
		/* extend existing area */
		cur->end = VOID_PTR_ADD(start, MemorySizeInBytes-1);
	} else {
		vm_area_t *new_area;
		/* create a new area between cur and next */
		new_area = vm_create_and_init_area(start,
				VOID_PTR_ADD(start, (MemorySizeInBytes - 1)));
		if (!new_area)
			return NULL;
		new_area->next = next;
		new_area->prev = cur;
		if (cur)
			cur->next = new_area;
		else
			app->vm_ranges = new_area;
		if (next)
			next->prev = new_area;
	}

	return start;
}
static void *aperture_allocate_area(manageable_aperture_t *app,
				    uint64_t MemorySizeInBytes,
				    uint64_t offset)
{
	return aperture_allocate_area_aligned(app, MemorySizeInBytes, offset, app->align);
}

/* returns 0 on success. Assumes, that fmm_mutex is locked on entry */
static vm_object_t *aperture_allocate_object(manageable_aperture_t *app,
					     void *new_address,
					     uint64_t handle,
					     uint64_t MemorySizeInBytes,
					     uint32_t flags)
{
	vm_object_t *new_object;

	MemorySizeInBytes = ALIGN_UP(MemorySizeInBytes, app->align);

	/* Allocate new object */
	new_object = vm_create_and_init_object(new_address,
					       MemorySizeInBytes,
					       handle, flags);
	if (!new_object)
		return NULL;

	/* check for non-empty list */
	if (app->vm_objects)
		/* Add it before the first element */
		vm_add_object_before(app->vm_objects, new_object);

	app->vm_objects = new_object; /* Update head */

	return new_object;
}

static int32_t gpu_mem_find_by_gpu_id(uint32_t gpu_id)
{
	uint32_t i;

	for (i = 0 ; i < gpu_mem_count ; i++)
		if (gpu_mem[i].gpu_id == gpu_id)
			return i;

	return -1;
}

static manageable_aperture_t *fmm_get_aperture(HsaApertureInfo info)
{
	switch (info.type) {
	case HSA_APERTURE_DGPU:
		return &svm.dgpu_aperture;
	case HSA_APERTURE_DGPU_ALT:
		return &svm.dgpu_alt_aperture;
	case HSA_APERTURE_GPUVM:
		return &gpu_mem[info.idx].gpuvm_aperture;
	case HSA_APERTURE_CPUVM:
		return &cpuvm_aperture;
	default:
		return NULL;
	}
}

static manageable_aperture_t *fmm_is_scratch_aperture(const void *address)
{
	uint32_t i;

	for (i = 0; i < gpu_mem_count; i++) {
		if (gpu_mem[i].gpu_id == NON_VALID_GPU_ID)
			continue;

		if ((address >= gpu_mem[i].scratch_physical.base) &&
			(address <= gpu_mem[i].scratch_physical.limit))
			return &gpu_mem[i].scratch_physical;

	}
	return NULL;
}

static manageable_aperture_t *fmm_find_aperture(const void *address,
						HsaApertureInfo *info)
{
	manageable_aperture_t *aperture = NULL;
	uint32_t i;
	HsaApertureInfo _info = { .type = HSA_APERTURE_UNSUPPORTED, .idx = 0};

	if (is_dgpu) {
		if (address >= svm.dgpu_aperture.base &&
			address <= svm.dgpu_aperture.limit) {

			aperture = fmm_is_scratch_aperture(address);
			if (!aperture) {
				aperture = &svm.dgpu_aperture;
				_info.type = HSA_APERTURE_DGPU;
			}
		} else if (address >= svm.dgpu_alt_aperture.base &&
			address <= svm.dgpu_alt_aperture.limit) {
			aperture = &svm.dgpu_alt_aperture;
			_info.type = HSA_APERTURE_DGPU_ALT;
		} else {
			/* Not in SVM, it can be system memory registered by userptr */
			aperture = &svm.dgpu_aperture;
			_info.type = HSA_APERTURE_DGPU;
		}
	} else { /* APU */
		if (address >= svm.dgpu_aperture.base && address <= svm.dgpu_aperture.limit) {
			aperture = &svm.dgpu_aperture;
			_info.type = HSA_APERTURE_DGPU;
		} else {
			/* gpuvm_aperture */
			for (i = 0; i < gpu_mem_count; i++) {
				if ((address >= gpu_mem[i].gpuvm_aperture.base) &&
					(address <= gpu_mem[i].gpuvm_aperture.limit)) {
					aperture = &gpu_mem[i].gpuvm_aperture;
					_info.type = HSA_APERTURE_GPUVM;
					_info.idx = i;
				}
			}
		}
		if (!aperture) {
			/* Not in GPUVM */
			aperture = &cpuvm_aperture;
			_info.type = HSA_APERTURE_CPUVM;
		}
	}

	if (info)
		*info = _info;

	return aperture;
}

/* After allocating the memory, return the vm_object created for this memory.
 * Return NULL if any failure.
 */
static vm_object_t *fmm_allocate_memory_object(uint32_t gpu_id, void *mem,
						uint64_t MemorySizeInBytes,
						manageable_aperture_t *aperture,
						uint64_t *mmap_offset,
						uint32_t flags)
{
	struct kfd_ioctl_alloc_memory_of_gpu_args args = {0};
	struct kfd_ioctl_free_memory_of_gpu_args free_args = {0};
	vm_object_t *vm_obj = NULL;

	if (!mem)
		return NULL;

	/* Allocate memory from amdkfd */
	args.gpu_id = gpu_id;
	args.size = ALIGN_UP(MemorySizeInBytes, aperture->align);

	args.flags = flags |
		KFD_IOC_ALLOC_MEM_FLAGS_NO_SUBSTITUTE;
	args.va_addr = (uint64_t)mem;
	if (!topology_is_dgpu(get_device_id_by_gpu_id(gpu_id)) &&
	    (flags & KFD_IOC_ALLOC_MEM_FLAGS_VRAM))
		args.va_addr = VOID_PTRS_SUB(mem, aperture->base);
	if (flags & KFD_IOC_ALLOC_MEM_FLAGS_USERPTR)
		args.mmap_offset = *mmap_offset;

	if (kmtIoctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &args))
		return NULL;

	/* Allocate object */
	pthread_mutex_lock(&aperture->fmm_mutex);
	vm_obj = aperture_allocate_object(aperture, mem, args.handle,
				      MemorySizeInBytes, flags);
	if (!vm_obj)
		goto err_object_allocation_failed;
	pthread_mutex_unlock(&aperture->fmm_mutex);

	if (mmap_offset)
		*mmap_offset = args.mmap_offset;

	return vm_obj;

err_object_allocation_failed:
	pthread_mutex_unlock(&aperture->fmm_mutex);
	free_args.handle = args.handle;
	kmtIoctl(kfd_fd, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &free_args);

	return NULL;
}

#ifdef DEBUG_PRINT_APERTURE
static void aperture_print(aperture_t *app)
{
	pr_info("\t Base: %p\n", app->base);
	pr_info("\t Limit: %p\n", app->limit);
}

static void manageable_aperture_print(manageable_aperture_t *app)
{
	vm_area_t *cur = app->vm_ranges;
	vm_object_t *object = app->vm_objects;

	pr_info("\t Base: %p\n", app->base);
	pr_info("\t Limit: %p\n", app->limit);
	pr_info("\t Ranges:\n");
	while (cur) {
		pr_info("\t\t Range [%p - %p]\n", cur->start, cur->end);
		cur = cur->next;
	};
	pr_info("\t Objects:\n");
	while (object) {
		pr_info("\t\t Object [%p - %" PRIu64 "]\n",
				object->start, object->size);
		object = object->next;
	};
}

void fmm_print(uint32_t gpu_id)
{
	int32_t gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);

	if (gpu_mem_id >= 0) { /* Found */
		pr_info("LDS aperture:\n");
		aperture_print(&gpu_mem[gpu_mem_id].lds_aperture);
		pr_info("GPUVM aperture:\n");
		manageable_aperture_print(&gpu_mem[gpu_mem_id].gpuvm_aperture);
		pr_info("Scratch aperture:\n");
		manageable_aperture_print(&gpu_mem[gpu_mem_id].scratch_aperture);
		pr_info("Scratch backing memory:\n");
		manageable_aperture_print(&gpu_mem[gpu_mem_id].scratch_physical);
	}

	pr_info("dGPU aperture:\n");
	manageable_aperture_print(&svm.dgpu_aperture);
	pr_info("dGPU alt aperture:\n");
	manageable_aperture_print(&svm.dgpu_alt_aperture);

}
#else
void fmm_print(uint32_t gpu_id)
{
}
#endif

static void fmm_release_scratch(uint32_t gpu_id)
{
	int32_t gpu_mem_id;
	uint64_t size;
	vm_object_t *obj;
	manageable_aperture_t *aperture;

	gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);
	if (gpu_mem_id < 0)
		return;

	aperture = &gpu_mem[gpu_mem_id].scratch_physical;

	size = VOID_PTRS_SUB(aperture->limit, aperture->base) + 1;

	if (topology_is_dgpu(gpu_mem[gpu_mem_id].device_id)) {
		/* unmap and remove all remaining objects */
		pthread_mutex_lock(&aperture->fmm_mutex);
		while ((obj = aperture->vm_objects)) {
			void *obj_addr = obj->start;

			pthread_mutex_unlock(&aperture->fmm_mutex);

			_fmm_unmap_from_gpu_scratch(gpu_id, aperture, obj_addr);

			pthread_mutex_lock(&aperture->fmm_mutex);
		}
		pthread_mutex_unlock(&aperture->fmm_mutex);

		/* release address space */
		pthread_mutex_lock(&svm.dgpu_aperture.fmm_mutex);
		aperture_release_area(&svm.dgpu_aperture,
				      gpu_mem[gpu_mem_id].scratch_physical.base,
				      size);
		pthread_mutex_unlock(&svm.dgpu_aperture.fmm_mutex);
	} else
		/* release address space */
		munmap(gpu_mem[gpu_mem_id].scratch_physical.base, size);

	/* invalidate scratch backing aperture */
	gpu_mem[gpu_mem_id].scratch_physical.base = NULL;
	gpu_mem[gpu_mem_id].scratch_physical.limit = NULL;
}

static uint32_t fmm_translate_hsa_to_ioc_flags(HsaMemFlags flags)
{
	uint32_t ioc_flags = 0;

	if (flags.ui32.AQLQueueMemory)
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_AQL_QUEUE_MEM;
	if (!flags.ui32.ReadOnly)
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE;
	/* TODO: Since, ROCr interfaces doesn't allow caller to set page
	 * permissions, mark all user allocations with exec permission.
	 * Check for flags.ui32.ExecuteAccess once ROCr is ready.
	 */
	ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE;
	return ioc_flags;
}

#define SCRATCH_ALIGN 0x10000
void *fmm_allocate_scratch(uint32_t gpu_id, uint64_t MemorySizeInBytes)
{
	manageable_aperture_t *aperture_phy;
	struct kfd_ioctl_set_scratch_backing_va_args args = {0};
	int32_t gpu_mem_id;
	void *mem = NULL;
	uint64_t aligned_size = ALIGN_UP(MemorySizeInBytes, SCRATCH_ALIGN);

	/* Retrieve gpu_mem id according to gpu_id */
	gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);
	if (gpu_mem_id < 0)
		return NULL;

	aperture_phy = &gpu_mem[gpu_mem_id].scratch_physical;
	if (aperture_phy->base || aperture_phy->limit)
		/* Scratch was already allocated for this GPU */
		return NULL;

	/* Allocate address space for scratch backing, 64KB aligned */
	if (topology_is_dgpu(gpu_mem[gpu_mem_id].device_id)) {
		pthread_mutex_lock(&svm.dgpu_aperture.fmm_mutex);
		mem = aperture_allocate_area_aligned(
			&svm.dgpu_aperture,
			aligned_size, 0, SCRATCH_ALIGN);
		pthread_mutex_unlock(&svm.dgpu_aperture.fmm_mutex);
	} else {
		uint64_t aligned_padded_size = aligned_size +
			SCRATCH_ALIGN - PAGE_SIZE;
		void *padded_end, *aligned_start, *aligned_end;

		mem = mmap(0, aligned_padded_size,
			   PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS,
			   -1, 0);
		if (!mem)
			return NULL;
		/* align start and unmap padding */
		padded_end = VOID_PTR_ADD(mem, aligned_padded_size);
		aligned_start = (void *)ALIGN_UP((uint64_t)mem, SCRATCH_ALIGN);
		aligned_end = VOID_PTR_ADD(aligned_start, aligned_size);
		if (aligned_start > mem)
			munmap(mem, VOID_PTRS_SUB(aligned_start, mem));
		if (aligned_end < padded_end)
			munmap(aligned_end,
			       VOID_PTRS_SUB(padded_end, aligned_end));
		mem = aligned_start;
	}

	/* Remember scratch backing aperture for later */
	aperture_phy->base = mem;
	aperture_phy->limit = VOID_PTR_ADD(mem, aligned_size-1);

	/* Program SH_HIDDEN_PRIVATE_BASE */
	args.gpu_id = gpu_id;
	args.va_addr = ((uint64_t)mem) >> 16;

	if (kmtIoctl(kfd_fd, AMDKFD_IOC_SET_SCRATCH_BACKING_VA, &args)) {
		fmm_release_scratch(gpu_id);
		return NULL;
	}

	return mem;
}

static void *__fmm_allocate_device(uint32_t gpu_id, uint64_t MemorySizeInBytes,
		manageable_aperture_t *aperture, uint64_t offset, uint64_t *mmap_offset,
		uint32_t flags, vm_object_t **vm_obj)
{
	void *mem = NULL;
	vm_object_t *obj;

	/* Check that aperture is properly initialized/supported */
	if (!aperture_is_valid(aperture->base, aperture->limit))
		return NULL;

	/* Allocate address space */
	pthread_mutex_lock(&aperture->fmm_mutex);
	mem = aperture_allocate_area(aperture,
					MemorySizeInBytes, offset);
	pthread_mutex_unlock(&aperture->fmm_mutex);

	/*
	 * Now that we have the area reserved, allocate memory in the device
	 * itself
	 */
	obj = fmm_allocate_memory_object(gpu_id, mem,
			MemorySizeInBytes, aperture, mmap_offset, flags);
	if (!obj) {
		/*
		 * allocation of memory in device failed.
		 * Release region in aperture
		 */
		pthread_mutex_lock(&aperture->fmm_mutex);
		aperture_release_area(aperture, mem, MemorySizeInBytes);
		pthread_mutex_unlock(&aperture->fmm_mutex);

		/* Assign NULL to mem to indicate failure to calling function */
		mem = NULL;
	}
	if (vm_obj)
		*vm_obj = obj;

	return mem;
}

/*
 * The offset from GPUVM aperture base address to ensure that address 0
 * (after base subtraction) won't be used
 */
#define GPUVM_APP_OFFSET 0x10000
void *fmm_allocate_device(uint32_t gpu_id, uint64_t MemorySizeInBytes, HsaMemFlags flags)
{
	manageable_aperture_t *aperture;
	int32_t gpu_mem_id;
	uint32_t ioc_flags = KFD_IOC_ALLOC_MEM_FLAGS_VRAM, offset;
	uint64_t size, mmap_offset;
	void *mem;
	vm_object_t *vm_obj = NULL;

	/* Retrieve gpu_mem id according to gpu_id */
	gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);
	if (gpu_mem_id < 0)
		return NULL;

	size = MemorySizeInBytes;

	if (flags.ui32.HostAccess)
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC;

	ioc_flags |= fmm_translate_hsa_to_ioc_flags(flags);

	if (topology_is_svm_needed(get_device_id_by_gpu_id(gpu_id))) {
		aperture = &svm.dgpu_aperture;
		offset = 0;
		if (flags.ui32.AQLQueueMemory)
			size = MemorySizeInBytes * 2;
	} else {
		aperture = &gpu_mem[gpu_mem_id].gpuvm_aperture;
		offset = GPUVM_APP_OFFSET;
	}

	if (aperture->is_coherent)
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;

	mem = __fmm_allocate_device(gpu_id, size,
			aperture, offset, &mmap_offset,
			ioc_flags, &vm_obj);

	if (mem && vm_obj) {
		pthread_mutex_lock(&aperture->fmm_mutex);
		/* Store memory allocation flags, not ioc flags */
		vm_obj->flags = flags.Value;
		gpuid_to_nodeid(gpu_id, &vm_obj->node_id);
		pthread_mutex_unlock(&aperture->fmm_mutex);
	}

	if (mem && (flags.ui32.HostAccess || hsa_debug)) {
		int map_fd = mmap_offset >= (1ULL<<40) ? kfd_fd :
					gpu_mem[gpu_mem_id].drm_render_fd;
		int prot = flags.ui32.HostAccess ? PROT_READ | PROT_WRITE :
					PROT_NONE;
		int flag = flags.ui32.HostAccess ? MAP_SHARED | MAP_FIXED :
					MAP_PRIVATE|MAP_FIXED;
		void *ret = mmap(mem, MemorySizeInBytes, prot, flag,
					map_fd, mmap_offset);

		if (ret == MAP_FAILED) {
			__fmm_release(mem, aperture);
			return NULL;
		}
	}

	return mem;
}

void *fmm_allocate_doorbell(uint32_t gpu_id, uint64_t MemorySizeInBytes,
			    uint64_t doorbell_offset)
{
	manageable_aperture_t *aperture;
	int32_t gpu_mem_id;
	uint32_t ioc_flags;
	void *mem;
	vm_object_t *vm_obj = NULL;

	/* Retrieve gpu_mem id according to gpu_id */
	gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);
	if (gpu_mem_id < 0)
		return NULL;

	/* Use fine-grained aperture */
	aperture = &svm.dgpu_alt_aperture;
	ioc_flags = KFD_IOC_ALLOC_MEM_FLAGS_DOORBELL |
		    KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
		    KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;

	mem = __fmm_allocate_device(gpu_id, MemorySizeInBytes,
			aperture, 0, NULL,
			ioc_flags, &vm_obj);

	if (mem && vm_obj) {
		HsaMemFlags flags;

		/* Cook up some flags for storing in the VM object */
		flags.Value = 0;
		flags.ui32.NonPaged = 1;
		flags.ui32.HostAccess = 1;
		flags.ui32.Reserved = 0xBe11;

		pthread_mutex_lock(&aperture->fmm_mutex);
		vm_obj->flags = flags.Value;
		gpuid_to_nodeid(gpu_id, &vm_obj->node_id);
		pthread_mutex_unlock(&aperture->fmm_mutex);
	}

	if (mem) {
		void *ret = mmap(mem, MemorySizeInBytes,
				 PROT_READ | PROT_WRITE,
				 MAP_SHARED | MAP_FIXED, kfd_fd,
				 doorbell_offset);
		if (ret == MAP_FAILED) {
			__fmm_release(mem, aperture);
			return NULL;
		}
	}

	return mem;
}

static void *fmm_allocate_host_cpu(uint64_t MemorySizeInBytes,
				HsaMemFlags flags)
{
	void *mem = NULL;
	vm_object_t *vm_obj;
	int mmap_prot = PROT_READ | PROT_WRITE;

	if (flags.ui32.ExecuteAccess)
		mmap_prot |= PROT_EXEC;

	/* mmap will return a pointer with alignment equal to
	 * sysconf(_SC_PAGESIZE).
	 */
	mem = mmap(NULL, MemorySizeInBytes, mmap_prot,
			MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	if (mem == MAP_FAILED)
		return NULL;

	pthread_mutex_lock(&cpuvm_aperture.fmm_mutex);
	vm_obj = aperture_allocate_object(&cpuvm_aperture, mem, 0,
				      MemorySizeInBytes, flags.Value);
	if (vm_obj)
		vm_obj->node_id = 0; /* APU systems only have one CPU node */
	pthread_mutex_unlock(&cpuvm_aperture.fmm_mutex);

	return mem;
}

/* Remove any CPU mapping, but keep the address range reserved */
static void munmap_and_reserve_address(void *address, uint64_t size)
{
	void *mmap_ret;

	mmap_ret = mmap(address, size, PROT_NONE,
			MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_FIXED,
			-1, 0);
	if (mmap_ret == MAP_FAILED && errno == ENOMEM) {
		/* When mmap count reaches max_map_count, any mmap will
		 * fail. Reduce the count with munmap then map it as
		 * NORESERVE immediately.
		 */
		munmap(address, size);
		mmap(address, size, PROT_NONE,
			MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_FIXED,
			-1, 0);
	}
}

static void *fmm_allocate_host_gpu(uint32_t node_id, uint64_t MemorySizeInBytes,
				   HsaMemFlags flags)
{
	void *mem;
	manageable_aperture_t *aperture;
	uint64_t mmap_offset;
	uint32_t ioc_flags;
	uint64_t size;
	int32_t gpu_mem_id;
	uint32_t gpu_id;
	vm_object_t *vm_obj = NULL;

	gpu_mem_id = find_first_dgpu(&gpu_id);
	if (gpu_mem_id < 0)
		return NULL;

	size = MemorySizeInBytes;
	ioc_flags = 0;
	if (flags.ui32.CoarseGrain)
		aperture = &svm.dgpu_aperture;
	else
		aperture = &svm.dgpu_alt_aperture; /* always coherent */

	if (aperture->is_coherent)
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_COHERENT;
	ioc_flags |= fmm_translate_hsa_to_ioc_flags(flags);

	if (flags.ui32.AQLQueueMemory)
		size = MemorySizeInBytes * 2;

	/* Paged memory is allocated as a userptr mapping, non-paged
	 * memory is allocated from KFD
	 */
	if (!flags.ui32.NonPaged && svm.userptr_for_paged_mem) {
		const unsigned int bits_per_long = sizeof(unsigned long) * 8;
		unsigned long node_mask[node_id / bits_per_long + 1];
		int mode = MPOL_F_STATIC_NODES;

		/* Allocate address space */
		pthread_mutex_lock(&aperture->fmm_mutex);
		mem = aperture_allocate_area(aperture, size, 0);
		pthread_mutex_unlock(&aperture->fmm_mutex);
		if (!mem)
			return NULL;

		/* Bind to NUMA node */
		memset(node_mask, 0, sizeof(node_mask));
		node_mask[node_id / bits_per_long] = 1UL << (node_id % bits_per_long);
		mode |= flags.ui32.NoSubstitute ? MPOL_BIND : MPOL_PREFERRED;
		if (mbind(mem, MemorySizeInBytes, mode, node_mask, node_id+1, 0))
			pr_warn("Failed to set NUMA policy for %lu pages at %p\n",
				MemorySizeInBytes >> 12, mem);

		/* Map anonymous pages */
		if (mmap(mem, MemorySizeInBytes, PROT_READ | PROT_WRITE,
			 MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0)
		    == MAP_FAILED) {
			/* Release address space */
			pthread_mutex_lock(&aperture->fmm_mutex);
			aperture_release_area(aperture, mem, size);
			pthread_mutex_unlock(&aperture->fmm_mutex);
			return NULL;
		}

		/* Mappings in the DGPU aperture don't need to be copied on
		 * fork. This avoids MMU notifiers and evictions due to user
		 * memory mappings on fork.
		 */
#ifdef __FreeBSD__
		minherit(mem, MemorySizeInBytes, INHERIT_NONE);
#else
		madvise(mem, MemorySizeInBytes, MADV_DONTFORK);
#endif

		/* Create userptr BO */
		mmap_offset = (uint64_t)mem;
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_USERPTR;
		vm_obj = fmm_allocate_memory_object(gpu_id, mem, size,
						       aperture, &mmap_offset,
						       ioc_flags);
		if (!vm_obj) {
			/* Release address space */
			pthread_mutex_lock(&aperture->fmm_mutex);
			aperture_release_area(aperture, mem, size);
			pthread_mutex_unlock(&aperture->fmm_mutex);
			munmap_and_reserve_address(mem, MemorySizeInBytes);
			return NULL;
		}
	} else {
		ioc_flags |= KFD_IOC_ALLOC_MEM_FLAGS_GTT;
		mem =  __fmm_allocate_device(gpu_id, size,
					     aperture, 0, &mmap_offset,
					     ioc_flags, &vm_obj);

		if (mem && flags.ui32.HostAccess) {
			int map_fd = mmap_offset >= (1ULL<<40) ? kfd_fd :
						gpu_mem[gpu_mem_id].drm_render_fd;
			void *ret = mmap(mem, MemorySizeInBytes,
					 PROT_READ | PROT_WRITE,
					 MAP_SHARED | MAP_FIXED, map_fd, mmap_offset);
			if (ret == MAP_FAILED) {
				__fmm_release(mem, aperture);
				return NULL;
			}

			if (flags.ui32.AQLQueueMemory) {
				uint64_t my_buf_size = ALIGN_UP(size, aperture->align) / 2;

				memset(ret, 0, MemorySizeInBytes);
				mmap(VOID_PTR_ADD(mem, my_buf_size), MemorySizeInBytes,
				     PROT_READ | PROT_WRITE,
				     MAP_SHARED | MAP_FIXED, map_fd, mmap_offset);
			}
		}
	}

	if (mem && vm_obj) {
		/* Store memory allocation flags, not ioc flags */
		pthread_mutex_lock(&aperture->fmm_mutex);
		vm_obj->flags = flags.Value;
		vm_obj->node_id = node_id;
		pthread_mutex_unlock(&aperture->fmm_mutex);
	}

	return mem;
}

void *fmm_allocate_host(uint32_t node_id, uint64_t MemorySizeInBytes,
			HsaMemFlags flags)
{
	if (is_dgpu)
		return fmm_allocate_host_gpu(node_id, MemorySizeInBytes, flags);
	return fmm_allocate_host_cpu(MemorySizeInBytes, flags);
}

static void __fmm_release(void *address, manageable_aperture_t *aperture)
{
	struct kfd_ioctl_free_memory_of_gpu_args args = {0};
	vm_object_t *object;

	if (!address)
		return;

	pthread_mutex_lock(&aperture->fmm_mutex);

	/* Find the object to retrieve the handle */
	object = vm_find_object_by_address(aperture, address, 0);
	if (!object) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return;
	}

	/* If memory is user memory and it's still GPU mapped, munmap
	 * would cause an eviction. If the restore happens quickly
	 * enough, restore would also fail with an error message. So
	 * free the BO before unmapping the pages.
	 */
	args.handle = object->handle;
	kmtIoctl(kfd_fd, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &args);

	if (address >= dgpu_shared_aperture_base &&
	    address <= dgpu_shared_aperture_limit) {
		/* Reset NUMA policy */
		mbind(address, object->size, MPOL_DEFAULT, NULL, 0, 0);
		munmap_and_reserve_address(address, object->size);
	}

	aperture_release_area(aperture, address, object->size);
	vm_remove_object(aperture, object);

	pthread_mutex_unlock(&aperture->fmm_mutex);
}

void fmm_release(void *address)
{
	uint32_t i;
	bool found = false;
	vm_object_t *object;

	for (i = 0; i < gpu_mem_count && !found; i++) {
		if (gpu_mem[i].gpu_id == NON_VALID_GPU_ID)
			continue;
		if (address >= gpu_mem[i].scratch_physical.base &&
			address <= gpu_mem[i].scratch_physical.limit) {
			fmm_release_scratch(gpu_mem[i].gpu_id);
			return;
		}

		if (address >= gpu_mem[i].gpuvm_aperture.base &&
			address <= gpu_mem[i].gpuvm_aperture.limit) {
			found = true;
			__fmm_release(address, &gpu_mem[i].gpuvm_aperture);
			fmm_print(gpu_mem[i].gpu_id);
		}
	}

	if (!found) {
		if (address >= svm.dgpu_aperture.base &&
			address <= svm.dgpu_aperture.limit) {
			found = true;
			__fmm_release(address, &svm.dgpu_aperture);
			fmm_print(gpu_mem[i].gpu_id);
		} else if (address >= svm.dgpu_alt_aperture.base &&
			address <= svm.dgpu_alt_aperture.limit) {
			found = true;
			__fmm_release(address, &svm.dgpu_alt_aperture);
			fmm_print(gpu_mem[i].gpu_id);
		}
	}

	/*
	 * If memory address isn't inside of any defined GPU aperture - it
	 * refers to the system memory
	 */
	if (!found) {
		uint64_t size = 0;
		/* Release the vm object in CPUVM */
		pthread_mutex_lock(&cpuvm_aperture.fmm_mutex);
		object = vm_find_object_by_address(&cpuvm_aperture, address, 0);
		if (object) {
			size = object->size;
			vm_remove_object(&cpuvm_aperture, object);
		}
		pthread_mutex_unlock(&cpuvm_aperture.fmm_mutex);
		/* Free the memory from the system */
		if (size)
			munmap(address, size);
	}
}

static int fmm_set_memory_policy(uint32_t gpu_id, int default_policy, int alt_policy,
				 uintptr_t alt_base, uint64_t alt_size)
{
	struct kfd_ioctl_set_memory_policy_args args = {0};

	args.gpu_id = gpu_id;
	args.default_policy = default_policy;
	args.alternate_policy = alt_policy;
	args.alternate_aperture_base = alt_base;
	args.alternate_aperture_size = alt_size;

	return kmtIoctl(kfd_fd, AMDKFD_IOC_SET_MEMORY_POLICY, &args);
}

static uint32_t get_vm_alignment(uint32_t device_id)
{
	int page_size = 0;

	if (device_id >= 0x6920 && device_id <= 0x6939) /* Tonga */
		page_size = TONGA_PAGE_SIZE;
	else if (device_id >= 0x9870 && device_id <= 0x9877) /* Carrizo */
		page_size = TONGA_PAGE_SIZE;

	return MAX(PAGE_SIZE, page_size);
}

static HSAKMT_STATUS get_process_apertures(
	struct kfd_process_device_apertures *process_apertures,
	uint32_t *num_of_nodes)
{
	struct kfd_ioctl_get_process_apertures_new_args args_new = {0};
	struct kfd_ioctl_get_process_apertures_args args_old;

	args_new.kfd_process_device_apertures_ptr = (uintptr_t)process_apertures;
	args_new.num_of_nodes = *num_of_nodes;
	if (!kmtIoctl(kfd_fd, AMDKFD_IOC_GET_PROCESS_APERTURES_NEW,
		      (void *)&args_new)) {
		*num_of_nodes = args_new.num_of_nodes;
		return HSAKMT_STATUS_SUCCESS;
	}

	/* New IOCTL failed, try the old one in case we're running on
	 * a really old kernel */
	memset(&args_old, 0, sizeof(args_old));

	if (kmtIoctl(kfd_fd, AMDKFD_IOC_GET_PROCESS_APERTURES,
		     (void *)&args_old))
		return HSAKMT_STATUS_ERROR;

	if (args_old.num_of_nodes < *num_of_nodes)
		*num_of_nodes = args_old.num_of_nodes;

	memcpy(process_apertures, args_old.process_apertures,
	       sizeof(*process_apertures) * *num_of_nodes);

	return HSAKMT_STATUS_SUCCESS;
}

/* The VMs from DRM render nodes are used by KFD for the lifetime of
 * the process. Therefore we have to keep using the same FDs for the
 * lifetime of the process, even when we close and reopen KFD. There
 * are up to 128 render nodes that we cache in this array.
 */
#define DRM_FIRST_RENDER_NODE 128
#define DRM_LAST_RENDER_NODE 255
static int drm_render_fds[DRM_LAST_RENDER_NODE + 1 - DRM_FIRST_RENDER_NODE];

static int open_drm_render_device(int minor)
{
	char path[128];
	int index, fd;

	if (minor < DRM_FIRST_RENDER_NODE || minor > DRM_LAST_RENDER_NODE) {
		pr_err("DRM render minor %d out of range [%d, %d]\n", minor,
		       DRM_FIRST_RENDER_NODE, DRM_LAST_RENDER_NODE);
		return -EINVAL;
	}
	index = minor - DRM_FIRST_RENDER_NODE;

	/* If the render node was already opened, keep using the same FD */
	if (drm_render_fds[index])
		return drm_render_fds[index];

	sprintf(path, "/dev/dri/renderD%d", minor);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		pr_err("Failed to open %s: %s\n", path, strerror(errno));
		return -errno;
	}
	drm_render_fds[index] = fd;

	return fd;
}

static HSAKMT_STATUS acquire_vm(uint32_t gpu_id, int fd)
{
	struct kfd_ioctl_acquire_vm_args args;

	args.gpu_id = gpu_id;
	args.drm_fd = fd;
	pr_info("acquiring VM for %x using %d\n", gpu_id, fd);
	if (kmtIoctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, (void *)&args)) {
		pr_err("AMDKFD_IOC_ACQUIRE_VM failed\n");
		return HSAKMT_STATUS_ERROR;
	}

	return HSAKMT_STATUS_SUCCESS;
}

static void *reserve_address(void *addr, unsigned long long int len)
{
	void *ret_addr;

	if (len <= 0)
		return NULL;

	ret_addr = mmap(addr, len, PROT_NONE,
				 MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE, -1, 0);
	if (ret_addr == MAP_FAILED)
		return NULL;

	return ret_addr;
}

/* Managed SVM aperture limits: only reserve up to 40 bits (1TB, what
 * GFX8 supports). Need to find at least 4GB of usable address space.
 */
#define SVM_RESERVATION_LIMIT ((1ULL << 40) - 1)
#define SVM_MIN_VM_SIZE (4ULL << 30)
#define IS_CANONICAL_ADDR(a) ((a) < (1ULL << 47))

static HSAKMT_STATUS init_svm_apertures(HSAuint64 base, HSAuint64 limit,
					HSAuint32 align, HSAuint32 guard_pages,
					bool disable_cache)
{
	const HSAuint64 ADDR_INC = GPU_HUGE_PAGE_SIZE;
	HSAuint64 len, map_size, alt_base, alt_size;
	bool found = false;
	void *addr, *ret_addr;

	/* If we already have an SVM aperture initialized (from a
	 * parent process), keep using it
	 */
	if (dgpu_shared_aperture_limit)
		return HSAKMT_STATUS_SUCCESS;

	base = ALIGN_UP(base, GPU_HUGE_PAGE_SIZE);
	limit = ((limit + 1) & ~(HSAuint64)(GPU_HUGE_PAGE_SIZE - 1)) - 1;
	if (limit > SVM_RESERVATION_LIMIT)
		limit = SVM_RESERVATION_LIMIT;
	if (base >= limit) {
		pr_err("No SVM range compatible with all GPU and software constraints\n");
		return HSAKMT_STATUS_ERROR;
	}

	/* Try to reserve address space for SVM.
	 *
	 * Inner loop: try start addresses in huge-page increments up
	 * to half the VM size we're trying to reserve
	 *
	 * Outer loop: reduce size of the allocation by factor 2 at a
	 * time and print a warning for every reduction
	 */
	for (len = limit - base + 1; !found && len >= SVM_MIN_VM_SIZE;
	     len = (len + 1) >> 1) {
		for (addr = (void *)base, ret_addr = NULL;
		     (HSAuint64)addr + ((len + 1) >> 1) - 1 <= limit;
		     addr = (void *)((HSAuint64)addr + ADDR_INC)) {
			HSAuint64 top = MIN((HSAuint64)addr + len, limit+1);

			map_size = (top - (HSAuint64)addr) &
				~(HSAuint64)(PAGE_SIZE - 1);
			if (map_size < SVM_MIN_VM_SIZE)
				break;

			ret_addr = reserve_address(addr, map_size);
			if (!ret_addr)
				break;
			if ((HSAuint64)ret_addr + ((len + 1) >> 1) - 1 <= limit)
				/* At least half the returned address
				 * space is GPU addressable, we'll
				 * take it
				 */
				break;
			munmap(ret_addr, map_size);
			ret_addr = NULL;
		}
		if (!ret_addr) {
			pr_warn("Failed to reserve %uGB for SVM ...\n",
				(unsigned int)(len >> 30));
			continue;
		}
		if ((HSAuint64)ret_addr + SVM_MIN_VM_SIZE - 1 > limit) {
			/* addressable size is less than the minimum */
			pr_warn("Got %uGB for SVM at %p with only %dGB usable ...\n",
				(unsigned int)(map_size >> 30), ret_addr,
				(int)((limit - (HSAint64)ret_addr) >> 30));
			munmap(ret_addr, map_size);
			ret_addr = NULL;
			continue;
		} else {
			found = true;
			break;
		}
	}

	if (!found) {
		pr_err("Failed to reserve SVM address range. Giving up.\n");
		return HSAKMT_STATUS_ERROR;
	}

	base = (HSAuint64)ret_addr;
	if (base + map_size - 1 > limit)
		/* trim the tail that's not GPU-addressable */
		munmap((void *)(limit + 1), base + map_size - 1 - limit);
	else
		limit = base + map_size - 1;

	/* init aperture */
	svm.dgpu_aperture.base  = dgpu_shared_aperture_base  = ret_addr;
	svm.dgpu_aperture.limit = dgpu_shared_aperture_limit = (void *)limit;
	svm.dgpu_aperture.align = align;
	svm.dgpu_aperture.guard_pages = guard_pages;
	svm.dgpu_aperture.is_coherent = disable_cache;

	/* Use the first 1/4 of the dGPU aperture as
	 * alternate aperture for coherent access.
	 * Base and size must be 64KB aligned.
	 */
	alt_base = (HSAuint64)svm.dgpu_aperture.base;
	alt_size = (VOID_PTRS_SUB(svm.dgpu_aperture.limit,
				  svm.dgpu_aperture.base) + 1) >> 2;
	alt_base = (alt_base + 0xffff) & ~0xffffULL;
	alt_size = (alt_size + 0xffff) & ~0xffffULL;
	svm.dgpu_alt_aperture.base = (void *)alt_base;
	svm.dgpu_alt_aperture.limit = (void *)(alt_base + alt_size - 1);
	svm.dgpu_alt_aperture.align = align;
	svm.dgpu_alt_aperture.guard_pages = guard_pages;
	svm.dgpu_alt_aperture.is_coherent = true;

	svm.dgpu_aperture.base = VOID_PTR_ADD(svm.dgpu_alt_aperture.limit, 1);

	pr_info("SVM alt (coherent): %12p - %12p\n",
		svm.dgpu_alt_aperture.base, svm.dgpu_alt_aperture.limit);
	pr_info("SVM (non-coherent): %12p - %12p\n",
		svm.dgpu_aperture.base, svm.dgpu_aperture.limit);

	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS fmm_init_process_apertures(unsigned int NumNodes)
{
	uint32_t i = 0;
	int32_t gpu_mem_id = 0;
	uint32_t gpu_id;
	HsaNodeProperties props;
	struct kfd_process_device_apertures *process_apertures;
	uint32_t num_of_nodes;
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;
	char *disableCache, *pagedUserptr, *checkUserptr, *guardPagesStr;
	char *hsaDebug;
	unsigned int guardPages = 1;
	struct pci_access *pacc;
	uint64_t svm_base = 0, svm_limit = 0;
	uint32_t svm_alignment = 0;

	hsaDebug = getenv("HSA_DEBUG");
	hsa_debug = hsaDebug && strcmp(hsaDebug, "0");

	/* If HSA_DISABLE_CACHE is set to a non-0 value, disable caching */
	disableCache = getenv("HSA_DISABLE_CACHE");
	if (disableCache && strcmp(disableCache, "0") == 0)
		disableCache = NULL;

	/* If HSA_USERPTR_FOR_PAGED_MEM is set to a non-0 value,
	 * enable userptr for all paged memory allocations
	 */
	pagedUserptr = getenv("HSA_USERPTR_FOR_PAGED_MEM");
	svm.userptr_for_paged_mem = (pagedUserptr && strcmp(pagedUserptr, "0"));

	/* If HSA_CHECK_USERPTR is set to a non-0 value, check all userptrs
	 * when they are registered
	 */
	checkUserptr = getenv("HSA_CHECK_USERPTR");
	svm.check_userptr = (checkUserptr && strcmp(checkUserptr, "0"));

	/* Specify number of guard pages for SVM apertures, default is 1 */
	guardPagesStr = getenv("HSA_SVM_GUARD_PAGES");
	if (!guardPagesStr || sscanf(guardPagesStr, "%u", &guardPages) != 1)
		guardPages = 1;

	/* Trade off - NumNodes includes GPU nodes + CPU Node. So in
	 * systems with CPU node, slightly more memory is allocated than
	 * necessary
	 */
	gpu_mem = (gpu_mem_t *)calloc(NumNodes, sizeof(gpu_mem_t));
	if (!gpu_mem)
		return HSAKMT_STATUS_NO_MEMORY;

	/* Initialize gpu_mem[] from sysfs topology. Rest of the members are
	 * set to 0 by calloc. This is necessary because this function
	 * gets called before hsaKmtAcquireSystemProperties() is called.
	 */
	gpu_mem_count = 0;
	pacc = pci_alloc();
	pci_init(pacc);
	while (i < NumNodes) {
		memset(&props, 0, sizeof(props));
		ret = topology_sysfs_get_node_props(i, &props, &gpu_id, pacc);
		if (ret != HSAKMT_STATUS_SUCCESS)
			goto sysfs_parse_failed;

		/* Skip non-GPU nodes */
		if (gpu_id != 0) {
			int fd = open_drm_render_device(props.DrmRenderMinor);
			if (fd <= 0) {
				ret = HSAKMT_STATUS_ERROR;
				goto sysfs_parse_failed;
			}

			gpu_mem[gpu_mem_count].drm_render_fd = fd;
			gpu_mem[gpu_mem_count].gpu_id = gpu_id;
			gpu_mem[gpu_mem_count].local_mem_size = props.LocalMemSize;
			gpu_mem[gpu_mem_count].device_id = props.DeviceId;
			gpu_mem[gpu_mem_count].node_id = i;
			gpu_mem[gpu_mem_count].scratch_physical.align = PAGE_SIZE;
			pthread_mutex_init(&gpu_mem[gpu_mem_count].scratch_physical.fmm_mutex, NULL);
			gpu_mem[gpu_mem_count].scratch_aperture.align = PAGE_SIZE;
			pthread_mutex_init(&gpu_mem[gpu_mem_count].scratch_aperture.fmm_mutex, NULL);
			gpu_mem[gpu_mem_count].gpuvm_aperture.align =
				get_vm_alignment(props.DeviceId);
			gpu_mem[gpu_mem_count].gpuvm_aperture.guard_pages = guardPages;
			pthread_mutex_init(&gpu_mem[gpu_mem_count].gpuvm_aperture.fmm_mutex, NULL);
			gpu_mem_count++;
		}
		i++;
	}
	pci_cleanup(pacc);

	/* The ioctl will also return Number of Nodes if
	 * args.kfd_process_device_apertures_ptr is set to NULL. This is not
	 * required since Number of nodes is already known. Kernel will fill in
	 * the apertures in kfd_process_device_apertures_ptr
	 */
	num_of_nodes = gpu_mem_count;
	process_apertures = calloc(num_of_nodes, sizeof(struct kfd_process_device_apertures));
	if (!process_apertures) {
		ret = HSAKMT_STATUS_NO_MEMORY;
		goto sysfs_parse_failed;
	}

	ret = get_process_apertures(process_apertures, &num_of_nodes);
	if (ret != HSAKMT_STATUS_SUCCESS)
		goto get_aperture_ioctl_failed;

	all_gpu_id_array_size = 0;
	all_gpu_id_array = NULL;
	if (num_of_nodes > 0) {
		all_gpu_id_array = malloc(sizeof(uint32_t) * num_of_nodes);
		if (!all_gpu_id_array) {
			ret = HSAKMT_STATUS_NO_MEMORY;
			goto get_aperture_ioctl_failed;
		}
	}

	for (i = 0 ; i < num_of_nodes ; i++) {
		/* Map Kernel process device data node i <--> gpu_mem_id which
		 * indexes into gpu_mem[] based on gpu_id
		 */
		gpu_mem_id = gpu_mem_find_by_gpu_id(process_apertures[i].gpu_id);
		if (gpu_mem_id < 0) {
			ret = HSAKMT_STATUS_ERROR;
			goto invalid_gpu_id;
		}

		all_gpu_id_array[i] = process_apertures[i].gpu_id;
		all_gpu_id_array_size += sizeof(uint32_t);

		gpu_mem[gpu_mem_id].lds_aperture.base =
			PORT_UINT64_TO_VPTR(process_apertures[i].lds_base);
		gpu_mem[gpu_mem_id].lds_aperture.limit =
			PORT_UINT64_TO_VPTR(process_apertures[i].lds_limit);

		gpu_mem[gpu_mem_id].scratch_aperture.base =
			PORT_UINT64_TO_VPTR(process_apertures[i].scratch_base);
		gpu_mem[gpu_mem_id].scratch_aperture.limit =
			PORT_UINT64_TO_VPTR(process_apertures[i].scratch_limit);

		if (IS_CANONICAL_ADDR(process_apertures[i].gpuvm_limit)) {
			uint64_t vm_alignment = get_vm_alignment(
				gpu_mem[gpu_mem_id].device_id);

			/* Set proper alignment for scratch backing aperture */
			gpu_mem[gpu_mem_id].scratch_physical.align = vm_alignment;

			/* Non-canonical per-ASIC GPUVM aperture does
			 * not exist on dGPUs in GPUVM64 address mode
			 */
			gpu_mem[gpu_mem_id].gpuvm_aperture.base = NULL;
			gpu_mem[gpu_mem_id].gpuvm_aperture.limit = NULL;

			/* Update SVM aperture limits and alignment */
			if (process_apertures[i].gpuvm_base > svm_base)
				svm_base = process_apertures[i].gpuvm_base;
			if (process_apertures[i].gpuvm_limit < svm_limit ||
			    svm_limit == 0)
				svm_limit = process_apertures[i].gpuvm_limit;
			if (vm_alignment > svm_alignment)
				svm_alignment = vm_alignment;
		} else {
			gpu_mem[gpu_mem_id].gpuvm_aperture.base =
				PORT_UINT64_TO_VPTR(process_apertures[i].gpuvm_base);
			gpu_mem[gpu_mem_id].gpuvm_aperture.limit =
				PORT_UINT64_TO_VPTR(process_apertures[i].gpuvm_limit);
		}

		/* Acquire the VM from the DRM render node for KFD use */
		ret = acquire_vm(gpu_mem[gpu_mem_id].gpu_id,
				 gpu_mem[gpu_mem_id].drm_render_fd);
		if (ret != HSAKMT_STATUS_SUCCESS)
			goto acquire_vm_failed;
	}

	if (svm_limit) {
		/* At least one GPU uses GPUVM in canonical address
		 * space. Set up SVM apertures shared by all such GPUs
		 */
		ret = init_svm_apertures(svm_base, svm_limit, svm_alignment,
					 guardPages, disableCache);
		if (ret != HSAKMT_STATUS_SUCCESS)
			goto init_svm_failed;

		for (i = 0 ; i < num_of_nodes ; i++) {
			uintptr_t alt_base;
			uint64_t alt_size;
			int err;

			if (!IS_CANONICAL_ADDR(process_apertures[i].gpuvm_limit))
				continue;

			/* Set memory policy to match the SVM apertures */
			alt_base = (uintptr_t)svm.dgpu_alt_aperture.base;
			alt_size = VOID_PTRS_SUB(svm.dgpu_alt_aperture.limit,
				svm.dgpu_alt_aperture.base) + 1;
			err = fmm_set_memory_policy(process_apertures[i].gpu_id,
						    svm.dgpu_aperture.is_coherent ?
						    KFD_IOC_CACHE_POLICY_COHERENT :
						    KFD_IOC_CACHE_POLICY_NONCOHERENT,
						    KFD_IOC_CACHE_POLICY_COHERENT,
						    alt_base, alt_size);
			if (err) {
				pr_err("Failed to set mem policy for GPU [0x%x]\n",
				       process_apertures[i].gpu_id);
				ret = HSAKMT_STATUS_ERROR;
			}
		}
	}

	cpuvm_aperture.align = PAGE_SIZE;
	cpuvm_aperture.limit = (void *)0x7FFFFFFFFFFF; /* 2^47 - 1 */

	free(process_apertures);
	return ret;

init_svm_failed:
acquire_vm_failed:
get_aperture_ioctl_failed:
invalid_gpu_id:
	free(process_apertures);
sysfs_parse_failed:
	fmm_destroy_process_apertures();
	return ret;
}

void fmm_destroy_process_apertures(void)
{
	if (gpu_mem) {
		free(gpu_mem);
		gpu_mem = NULL;
	}
	gpu_mem_count = 0;
}

HSAKMT_STATUS fmm_get_aperture_base_and_limit(aperture_type_e aperture_type, HSAuint32 gpu_id,
			HSAuint64 *aperture_base, HSAuint64 *aperture_limit)
{
	HSAKMT_STATUS err = HSAKMT_STATUS_SUCCESS;
	int32_t slot = gpu_mem_find_by_gpu_id(gpu_id);

	if (slot < 0)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	switch (aperture_type) {
	case FMM_GPUVM:
		if (aperture_is_valid(gpu_mem[slot].gpuvm_aperture.base,
			gpu_mem[slot].gpuvm_aperture.limit)) {
			*aperture_base = PORT_VPTR_TO_UINT64(gpu_mem[slot].gpuvm_aperture.base);
			*aperture_limit = PORT_VPTR_TO_UINT64(gpu_mem[slot].gpuvm_aperture.limit);
		}
		break;

	case FMM_SCRATCH:
		if (aperture_is_valid(gpu_mem[slot].scratch_aperture.base,
			gpu_mem[slot].scratch_aperture.limit)) {
			*aperture_base = PORT_VPTR_TO_UINT64(gpu_mem[slot].scratch_aperture.base);
			*aperture_limit = PORT_VPTR_TO_UINT64(gpu_mem[slot].scratch_aperture.limit);
		}
		break;

	case FMM_LDS:
		if (aperture_is_valid(gpu_mem[slot].lds_aperture.base,
			gpu_mem[slot].lds_aperture.limit)) {
			*aperture_base = PORT_VPTR_TO_UINT64(gpu_mem[slot].lds_aperture.base);
			*aperture_limit = PORT_VPTR_TO_UINT64(gpu_mem[slot].lds_aperture.limit);
		}
		break;

	case FMM_SVM:
		/* Report single SVM aperture, starting at base of
		 * fine-grained, ending at limit of coarse-grained
		 */
		if (aperture_is_valid(svm.dgpu_alt_aperture.base,
				      svm.dgpu_aperture.limit)) {
			*aperture_base = PORT_VPTR_TO_UINT64(svm.dgpu_alt_aperture.base);
			*aperture_limit = PORT_VPTR_TO_UINT64(svm.dgpu_aperture.limit);
		}
		break;

	default:
		err = HSAKMT_STATUS_ERROR;
	}

	return err;
}

static bool id_in_array(uint32_t id, uint32_t *ids_array,
		uint32_t ids_array_size)
{
	uint32_t i;

	for (i = 0; i < ids_array_size/sizeof(uint32_t); i++) {
		if (id == ids_array[i])
			return true;
	}
	return false;
}

/* Helper function to remove ids_array from
 * obj->mapped_device_id_array
 */
static void remove_device_ids_from_mapped_array(vm_object_t *obj,
		uint32_t *ids_array, uint32_t ids_array_size)
{
	uint32_t i = 0, j = 0;

	if (obj->mapped_device_id_array == ids_array)
		goto set_size_and_free;

	for (i = 0; i < obj->mapped_device_id_array_size/
			sizeof(uint32_t); i++) {
		if (!id_in_array(obj->mapped_device_id_array[i],
					ids_array, ids_array_size))
			obj->mapped_device_id_array[j++] =
				obj->mapped_device_id_array[i];
	}

set_size_and_free:
	obj->mapped_device_id_array_size = j*sizeof(uint32_t);
	if (!j) {
		if (obj->mapped_device_id_array)
			free(obj->mapped_device_id_array);

		obj->mapped_device_id_array = NULL;
	}
}

/* Helper function to add ids_array to
 * obj->mapped_device_id_array
 */
static void add_device_ids_to_mapped_array(vm_object_t *obj,
		uint32_t *ids_array, uint32_t ids_array_size)
{
	uint32_t new_array_size;

	/* Remove any potential duplicated ids */
	remove_device_ids_from_mapped_array(obj, ids_array, ids_array_size);
	new_array_size = obj->mapped_device_id_array_size
		+ ids_array_size;

	obj->mapped_device_id_array = (uint32_t *)realloc(
			obj->mapped_device_id_array, new_array_size);

	memcpy(&obj->mapped_device_id_array
			[obj->mapped_device_id_array_size/sizeof(uint32_t)],
			ids_array, ids_array_size);

	obj->mapped_device_id_array_size = new_array_size;
}


/* If nodes_to_map is not NULL, map the nodes specified; otherwise map all. */
static int _fmm_map_to_gpu(manageable_aperture_t *aperture,
			void *address, uint64_t size, vm_object_t *obj,
			uint32_t *nodes_to_map, uint32_t nodes_array_size)
{
	struct kfd_ioctl_map_memory_to_gpu_args args = {0};
	vm_object_t *object;
	int ret = 0;

	if (!obj)
		pthread_mutex_lock(&aperture->fmm_mutex);

	object = obj;
	if (!object) {
		/* Find the object to retrieve the handle */
		object = vm_find_object_by_address(aperture, address, 0);
		if (!object) {
			ret = -EINVAL;
			goto err_object_not_found;
		}
	}

	/* For a memory region that is registered by user pointer, changing
	 * mapping nodes is not allowed, so we don't need to check the mapping
	 * nodes or map if it's already mapped. Just increase the reference.
	 */
	if (object->userptr && object->mapping_count) {
		++object->mapping_count;
		goto exit_ok;
	}

	args.handle = object->handle;
	if (nodes_to_map) {
	/* If specified, map the requested */
		args.device_ids_array_ptr = (uint64_t)nodes_to_map;
		args.n_devices = nodes_array_size / sizeof(uint32_t);
	} else if (object->registered_device_id_array_size > 0) {
	/* otherwise map all registered */
		args.device_ids_array_ptr =
			(uint64_t)object->registered_device_id_array;
		args.n_devices = object->registered_device_id_array_size /
			sizeof(uint32_t);
	} else {
	/* not specified, not registered: map all GPUs */
		args.device_ids_array_ptr = (uint64_t)all_gpu_id_array;
		args.n_devices = all_gpu_id_array_size / sizeof(uint32_t);
	}
	args.n_success = 0;

	ret = kmtIoctl(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &args);

	add_device_ids_to_mapped_array(object,
				(uint32_t *)args.device_ids_array_ptr,
				args.n_success * sizeof(uint32_t));
	print_device_id_array((uint32_t *)object->mapped_device_id_array,
			      object->mapped_device_id_array_size);

	object->mapping_count = 1;
	/* Mapping changed and lifecycle of object->mapped_node_id_array
	 * terminates here. Free it and allocate on next query
	 */
	if (object->mapped_node_id_array) {
		free(object->mapped_node_id_array);
		object->mapped_node_id_array = NULL;
	}

exit_ok:
err_object_not_found:
	if (!obj)
		pthread_mutex_unlock(&aperture->fmm_mutex);

	return ret;
}

static int _fmm_map_to_gpu_scratch(uint32_t gpu_id, manageable_aperture_t *aperture,
				   void *address, uint64_t size)
{
	int32_t gpu_mem_id;
	void *mem = NULL;
	int ret;
	bool is_debugger = 0;
	void *mmap_ret = NULL;
	uint64_t mmap_offset = 0;

	/* Retrieve gpu_mem id according to gpu_id */
	gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);
	if (gpu_mem_id < 0)
		return -1;

	if (!topology_is_dgpu(gpu_mem[gpu_mem_id].device_id))
		return 0; /* Nothing to do on APU */

	/* sanity check the address */
	if (address < aperture->base ||
	    VOID_PTR_ADD(address, size - 1) > aperture->limit)
		return -1;

	ret = debug_get_reg_status(gpu_mem[gpu_mem_id].node_id, &is_debugger);
	/* allocate object within the scratch backing aperture */
	if (!ret && !is_debugger) {
		vm_object_t *obj = fmm_allocate_memory_object(
			gpu_id, address, size, aperture, NULL,
			KFD_IOC_ALLOC_MEM_FLAGS_VRAM |
			KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE);
		if (!obj)
			return -1;
	} else {
		int map_fd = mmap_offset >= (1ULL<<40) ? kfd_fd :
					gpu_mem[gpu_mem_id].drm_render_fd;
		fmm_allocate_memory_object(
			gpu_id, address, size, aperture, &mmap_offset,
			KFD_IOC_ALLOC_MEM_FLAGS_GTT |
			KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE);
		mmap_ret = mmap(address, size,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_FIXED, map_fd, mmap_offset);
		if (mmap_ret == MAP_FAILED) {
			__fmm_release(mem, aperture);
			return -1;
		}
	}


	/* map to GPU */
	ret = _fmm_map_to_gpu(aperture, address, size, NULL, NULL, 0);
	if (ret != 0)
		__fmm_release(mem, aperture);

	return ret;
}

static int _fmm_map_to_apu_local(uint32_t gpu_id,
				manageable_aperture_t *aperture,
				void *address, uint64_t size,
				uint64_t *gpuvm_address)
{
	vm_object_t *object;

	if (gpuvm_address)
		*gpuvm_address = 0;
	/* Check that address space was previously reserved */
	if (!vm_find(aperture, address))
		return -1;

	pthread_mutex_lock(&aperture->fmm_mutex);

	/* Find the object to retrieve the handle */
	object = vm_find_object_by_address(aperture, address, 0);
	if (!object) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return -1;
	}
	pthread_mutex_unlock(&aperture->fmm_mutex);

	if (_fmm_map_to_gpu(aperture, address, size, object, NULL, 0))
		return -1;

	if (gpuvm_address) {
		*gpuvm_address = (uint64_t)object->start;
		if (!topology_is_dgpu(get_device_id_by_gpu_id(gpu_id)))
			*gpuvm_address = VOID_PTRS_SUB(object->start, aperture->base);
	}

	return 0;
}

static int _fmm_map_to_gpu_userptr(void *addr, uint64_t size,
				   uint64_t *gpuvm_addr, vm_object_t *object)
{
	manageable_aperture_t *aperture;
	vm_object_t *obj;
	void *svm_addr;
	HSAuint64 svm_size;
	HSAuint32 page_offset = (HSAuint64)addr & (PAGE_SIZE-1);
	int ret;

	aperture = &svm.dgpu_aperture;

	/* Find the start address in SVM space for GPU mapping */
	if (!object)
		pthread_mutex_lock(&aperture->fmm_mutex);

	obj = object;
	if (!obj) {
		obj = vm_find_object_by_userptr(aperture, addr, size);
		if (!obj) {
			pthread_mutex_unlock(&aperture->fmm_mutex);
			return HSAKMT_STATUS_ERROR;
		}
	}
	svm_addr = obj->start;
	svm_size = obj->size;

	/* Map and return the GPUVM address adjusted by the offset
	 * from the start of the page
	 */
	ret = _fmm_map_to_gpu(aperture, svm_addr, svm_size, obj, NULL, 0);
	if (ret == 0 && gpuvm_addr)
		*gpuvm_addr = (uint64_t)svm_addr + page_offset;

	if (!object)
		pthread_mutex_unlock(&aperture->fmm_mutex);

	return ret;
}

int fmm_map_to_gpu(void *address, uint64_t size, uint64_t *gpuvm_address)
{
	uint32_t i;
	uint64_t pi;

	/* Find an aperture the requested address belongs to */
	for (i = 0; i < gpu_mem_count; i++) {
		if (gpu_mem[i].gpu_id == NON_VALID_GPU_ID)
			continue;

		if ((address >= gpu_mem[i].scratch_physical.base) &&
			(address <= gpu_mem[i].scratch_physical.limit))
			return _fmm_map_to_gpu_scratch(gpu_mem[i].gpu_id,
							&gpu_mem[i].scratch_physical,
							address, size);

		if ((address >= gpu_mem[i].gpuvm_aperture.base) &&
			(address <= gpu_mem[i].gpuvm_aperture.limit))
			/* map it */
			return _fmm_map_to_apu_local(gpu_mem[i].gpu_id,
						&gpu_mem[i].gpuvm_aperture,
						address, size, gpuvm_address);
	}

	if ((address >= svm.dgpu_aperture.base) &&
		(address <= svm.dgpu_aperture.limit))
		/* map it */
		return _fmm_map_to_gpu(&svm.dgpu_aperture,
						address, size, NULL, NULL, 0);
	else if ((address >= svm.dgpu_alt_aperture.base) &&
		(address <= svm.dgpu_alt_aperture.limit))
		/* map it */
		return _fmm_map_to_gpu(&svm.dgpu_alt_aperture,
						address, size, NULL, NULL, 0);

	/*
	 * If address isn't an SVM memory address, we assume that this
	 * is system memory address. On dGPU we need to map it,
	 * assuming it was previously registered.
	 */
	if (is_dgpu)
		/* TODO: support mixed APU and dGPU configurations */
		return _fmm_map_to_gpu_userptr(address, size, gpuvm_address, NULL);

	/*
	 * On an APU a system memory address is accessed through
	 * IOMMU. Thus we "prefetch" it.
	 */
	for (pi = 0; pi < size / PAGE_SIZE; pi++)
		((char *) address)[pi * PAGE_SIZE] = 0;

	return 0;
}

static void print_device_id_array(uint32_t *device_id_array, uint32_t device_id_array_size)
{
#ifdef DEBUG_PRINT_APERTURE
	device_id_array_size /= sizeof(uint32_t);

	pr_info("device id array size %d\n", device_id_array_size);

	for (uint32_t i = 0 ; i < device_id_array_size; i++)
		pr_info("%d . 0x%x\n", (i+1), device_id_array[i]);
#endif
}

static int _fmm_unmap_from_gpu(manageable_aperture_t *aperture, void *address,
		uint32_t *device_ids_array, uint32_t device_ids_array_size,
		vm_object_t *obj)
{
	vm_object_t *object;
	int ret = 0;
	struct kfd_ioctl_unmap_memory_from_gpu_args args = {0};
	HSAuint32 page_offset = (HSAint64)address & (PAGE_SIZE - 1);

	if (!obj)
		pthread_mutex_lock(&aperture->fmm_mutex);

	/* Find the object to retrieve the handle */
	object = obj;
	if (!object) {
		object = vm_find_object_by_address(aperture,
					VOID_PTR_SUB(address, page_offset), 0);
		if (!object) {
			ret = -1;
			goto out;
		}
	}

	if (object->userptr && object->mapping_count > 1) {
		--object->mapping_count;
		goto out;
	}

	args.handle = object->handle;
	if (device_ids_array && device_ids_array_size > 0) {
		args.device_ids_array_ptr = (uint64_t)device_ids_array;
		args.n_devices = device_ids_array_size / sizeof(uint32_t);
	} else if (object->mapped_device_id_array_size > 0) {
		args.device_ids_array_ptr = (uint64_t)object->mapped_device_id_array;
		args.n_devices = object->mapped_device_id_array_size /
			sizeof(uint32_t);
	} else {
		/*
		 * When unmap exits here it should return failing error code as the user tried to
		 * unmap already unmapped buffer. Currently we returns success as KFDTEST and RT
		 * need to deploy the change on there side before thunk fails on this case.
		 */
		ret = 0;
		goto out;
	}
	args.n_success = 0;

	print_device_id_array((void *)args.device_ids_array_ptr,
			      args.n_devices * sizeof(uint32_t));

	ret = kmtIoctl(kfd_fd, AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &args);

	remove_device_ids_from_mapped_array(object,
			(uint32_t *)args.device_ids_array_ptr,
			args.n_success * sizeof(uint32_t));

	if (object->mapped_node_id_array)
		free(object->mapped_node_id_array);
	object->mapped_node_id_array = NULL;
	object->mapping_count = 0;

out:
	if (!obj)
		pthread_mutex_unlock(&aperture->fmm_mutex);
	return ret;
}

static int _fmm_unmap_from_gpu_scratch(uint32_t gpu_id,
				       manageable_aperture_t *aperture,
				       void *address)
{
	int32_t gpu_mem_id;
	vm_object_t *object;
	struct kfd_ioctl_unmap_memory_from_gpu_args args = {0};
	int ret;

	/* Retrieve gpu_mem id according to gpu_id */
	gpu_mem_id = gpu_mem_find_by_gpu_id(gpu_id);
	if (gpu_mem_id < 0)
		return -1;

	if (!topology_is_dgpu(gpu_mem[gpu_mem_id].device_id))
		return 0; /* Nothing to do on APU */

	pthread_mutex_lock(&aperture->fmm_mutex);

	/* Find the object to retrieve the handle and size */
	object = vm_find_object_by_address(aperture, address, 0);
	if (!object) {
		ret = -EINVAL;
		goto err;
	}

	if (!object->mapped_device_id_array ||
			object->mapped_device_id_array_size == 0) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return 0;
	}

	/* unmap from GPU */
	args.handle = object->handle;
	args.device_ids_array_ptr = (uint64_t)object->mapped_device_id_array;
	args.n_devices = object->mapped_device_id_array_size / sizeof(uint32_t);
	args.n_success = 0;
	ret = kmtIoctl(kfd_fd, AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &args);

	remove_device_ids_from_mapped_array(object,
			(uint32_t *)args.device_ids_array_ptr,
			args.n_success * sizeof(uint32_t));

	if (object->mapped_node_id_array)
		free(object->mapped_node_id_array);
	object->mapped_node_id_array = NULL;

	if (ret)
		goto err;

	pthread_mutex_unlock(&aperture->fmm_mutex);

	/* free object in scratch backing aperture */
	__fmm_release(address, aperture);

	return 0;

err:
	pthread_mutex_unlock(&aperture->fmm_mutex);
	return ret;
}

static int _fmm_unmap_from_gpu_userptr(void *addr)
{
	manageable_aperture_t *aperture;
	vm_object_t *obj;
	void *svm_addr;

	aperture = &svm.dgpu_aperture;

	/* Find the start address in SVM space for GPU unmapping */
	pthread_mutex_lock(&aperture->fmm_mutex);
	obj = vm_find_object_by_userptr(aperture, addr, 0);
	if (!obj) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_ERROR;
	}
	svm_addr = obj->start;
	pthread_mutex_unlock(&aperture->fmm_mutex);

	/* Unmap */
	return _fmm_unmap_from_gpu(aperture, svm_addr, NULL, 0, NULL);
}

int fmm_unmap_from_gpu(void *address)
{
	uint32_t i;

	/* Find the aperture the requested address belongs to */
	for (i = 0; i < gpu_mem_count; i++) {
		if (gpu_mem[i].gpu_id == NON_VALID_GPU_ID)
			continue;

		if ((address >= gpu_mem[i].scratch_physical.base) &&
			(address <= gpu_mem[i].scratch_physical.limit))
			return _fmm_unmap_from_gpu_scratch(gpu_mem[i].gpu_id,
							&gpu_mem[i].scratch_physical,
							address);

		if ((address >= gpu_mem[i].gpuvm_aperture.base) &&
			(address <= gpu_mem[i].gpuvm_aperture.limit))
			/* unmap it */
			return _fmm_unmap_from_gpu(&gpu_mem[i].gpuvm_aperture,
							address, NULL, 0, NULL);
	}

	if ((address >= svm.dgpu_aperture.base) &&
		(address <= svm.dgpu_aperture.limit))
		/* unmap it */
		return _fmm_unmap_from_gpu(&svm.dgpu_aperture,
						address, NULL, 0, NULL);
	else if ((address >= svm.dgpu_alt_aperture.base) &&
		(address <= svm.dgpu_alt_aperture.limit))
		/* unmap it */
		return _fmm_unmap_from_gpu(&svm.dgpu_alt_aperture,
						address, NULL, 0, NULL);

	/*
	 * If address isn't an SVM address, we assume that this is
	 * system memory address.
	 */
	if (is_dgpu)
		/* TODO: support mixed APU and dGPU configurations */
		return _fmm_unmap_from_gpu_userptr(address);

	return 0;
}

bool fmm_get_handle(void *address, uint64_t *handle)
{
	uint32_t i;
	manageable_aperture_t *aperture;
	vm_object_t *object;
	bool found;

	found = false;
	aperture = NULL;

	/* Find the aperture the requested address belongs to */
	for (i = 0; i < gpu_mem_count; i++) {
		if (gpu_mem[i].gpu_id == NON_VALID_GPU_ID)
			continue;

		if ((address >= gpu_mem[i].gpuvm_aperture.base) &&
			(address <= gpu_mem[i].gpuvm_aperture.limit)) {
			aperture = &gpu_mem[i].gpuvm_aperture;
			break;
		}
	}

	if (!aperture) {
		if ((address >= svm.dgpu_aperture.base) &&
			(address <= svm.dgpu_aperture.limit)) {
			aperture = &svm.dgpu_aperture;
		} else if ((address >= svm.dgpu_alt_aperture.base) &&
			(address <= svm.dgpu_alt_aperture.limit)) {
			aperture = &svm.dgpu_alt_aperture;
		}
	}

	if (!aperture)
		return false;

	pthread_mutex_lock(&aperture->fmm_mutex);
	/* Find the object to retrieve the handle */
	object = vm_find_object_by_address(aperture, address, 0);
	if (object && handle) {
		*handle = object->handle;
		found = true;
	}
	pthread_mutex_unlock(&aperture->fmm_mutex);


	return found;
}

static HSAuint8 fmm_check_user_memory(const void *addr, HSAuint64 size)
{
	volatile const HSAuint8 *ptr = addr;
	volatile const HSAuint8 *end = ptr + size;
	HSAuint8 sum = 0;

	/* Access every page in the buffer to make sure the mapping is
	 * valid. If it's not, it will die with a segfault that's easy
	 * to debug.
	 */
	for (; ptr < end; ptr = (void *)PAGE_ALIGN_UP(ptr + 1))
		sum += *ptr;

	return sum;
}

static HSAKMT_STATUS fmm_register_user_memory(void *addr, HSAuint64 size, vm_object_t **obj_ret)
{
	int32_t i;
	HSAuint32 gpu_id;
	manageable_aperture_t *aperture;
	void *svm_addr = NULL;
	vm_object_t *obj;
	HSAuint32 page_offset = (HSAuint64)addr & (PAGE_SIZE-1);
	HSAuint64 aligned_addr = (HSAuint64)addr - page_offset;
	HSAuint64 aligned_size = PAGE_ALIGN_UP(page_offset + size);

	/* Find first dGPU for creating the userptr BO */
	i = find_first_dgpu(&gpu_id);
	if (i < 0)
		return HSAKMT_STATUS_ERROR;
	aperture = &svm.dgpu_aperture;

	/* Check if this address was already registered */
	pthread_mutex_lock(&aperture->fmm_mutex);
	obj = vm_find_object_by_userptr(aperture, addr, size);
	if (obj) {
		++obj->registration_count;
		pthread_mutex_unlock(&aperture->fmm_mutex);
		*obj_ret = obj;
		return HSAKMT_STATUS_SUCCESS;
	}
	pthread_mutex_unlock(&aperture->fmm_mutex);

	/* Optionally check that the CPU mapping is valid */
	if (svm.check_userptr)
		fmm_check_user_memory(addr, size);

	/* Allocate BO, userptr address is passed in mmap_offset */
	svm_addr = __fmm_allocate_device(gpu_id, aligned_size, aperture, 0,
			 &aligned_addr, KFD_IOC_ALLOC_MEM_FLAGS_USERPTR |
			 KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
			 KFD_IOC_ALLOC_MEM_FLAGS_EXECUTABLE, &obj);
	if (!svm_addr)
		return HSAKMT_STATUS_ERROR;

	if (obj) {
		pthread_mutex_lock(&aperture->fmm_mutex);
		obj->userptr = addr;
		gpuid_to_nodeid(gpu_id, &obj->node_id);
		obj->userptr_size = size;
		obj->registration_count = 1;
		pthread_mutex_unlock(&aperture->fmm_mutex);
	} else
		return HSAKMT_STATUS_ERROR;

	if (obj_ret)
		*obj_ret = obj;
	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS fmm_register_memory(void *address, uint64_t size_in_bytes,
				  uint32_t *gpu_id_array,
				  uint32_t gpu_id_array_size)
{
	manageable_aperture_t *aperture;
	vm_object_t *object = NULL;
	HSAKMT_STATUS ret;

	if (gpu_id_array_size > 0 && !gpu_id_array)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	if ((address >= svm.dgpu_aperture.base) &&
	    (address <= svm.dgpu_aperture.limit))
		aperture = &svm.dgpu_aperture;
	else if ((address >= svm.dgpu_alt_aperture.base) &&
		 (address <= svm.dgpu_alt_aperture.limit))
		aperture = &svm.dgpu_alt_aperture;
	else {
		/*
		 * If address isn't SVM address, we assume that this
		 * is system memory address.
		 */
		ret = fmm_register_user_memory(address, size_in_bytes, &object);
		if (ret != HSAKMT_STATUS_SUCCESS)
			return ret;
		if (gpu_id_array_size == 0)
			return HSAKMT_STATUS_SUCCESS;
		aperture = &svm.dgpu_aperture;
		/* fall through */
	}

	pthread_mutex_lock(&aperture->fmm_mutex);
	if (!object)
		object = vm_find_object_by_address(aperture, address, 0);

	if (!object) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_NOT_SUPPORTED;
	}

	if (object->registered_device_id_array_size > 0) {
		/* Multiple registration is allowed, but not changing nodes */
		if ((gpu_id_array_size != object->registered_device_id_array_size)
			|| memcmp(object->registered_device_id_array,
					gpu_id_array, gpu_id_array_size)) {
			pr_err("Cannot change nodes in a registered addr.\n");
			pthread_mutex_unlock(&aperture->fmm_mutex);
			return HSAKMT_STATUS_MEMORY_ALREADY_REGISTERED;
		} else {
			/* Delete the new array, keep the existing one. */
			if (gpu_id_array)
				free(gpu_id_array);

			pthread_mutex_unlock(&aperture->fmm_mutex);
			return HSAKMT_STATUS_SUCCESS;
		}
	}

	if (gpu_id_array_size > 0) {
		object->registered_device_id_array = gpu_id_array;
		object->registered_device_id_array_size = gpu_id_array_size;
		/* Registration of object changed. Lifecycle of object->
		 * registered_node_id_array terminates here. Free old one
		 * and re-allocate on next query
		 */
		if (object->registered_node_id_array) {
			free(object->registered_node_id_array);
			object->registered_node_id_array = NULL;
		}
	}

	pthread_mutex_unlock(&aperture->fmm_mutex);
	return HSAKMT_STATUS_SUCCESS;
}

#define GRAPHICS_METADATA_DEFAULT_SIZE 64
HSAKMT_STATUS fmm_register_graphics_handle(HSAuint64 GraphicsResourceHandle,
					   HsaGraphicsResourceInfo *GraphicsResourceInfo,
					   uint32_t *gpu_id_array,
					   uint32_t gpu_id_array_size)
{
	struct kfd_ioctl_get_dmabuf_info_args infoArgs = {0};
	struct kfd_ioctl_import_dmabuf_args importArgs = {0};
	struct kfd_ioctl_free_memory_of_gpu_args freeArgs = {0};
	manageable_aperture_t *aperture;
	vm_object_t *obj;
	void *metadata;
	void *mem, *aperture_base;
	int32_t gpu_mem_id;
	uint64_t offset;
	int r;
	HSAKMT_STATUS status = HSAKMT_STATUS_ERROR;
	static const uint64_t IMAGE_ALIGN = 256*1024;

	if (gpu_id_array_size > 0 && !gpu_id_array)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	infoArgs.dmabuf_fd = GraphicsResourceHandle;
	infoArgs.metadata_size = GRAPHICS_METADATA_DEFAULT_SIZE;
	metadata = calloc(infoArgs.metadata_size, 1);
	if (!metadata)
		return HSAKMT_STATUS_NO_MEMORY;
	infoArgs.metadata_ptr = (uint64_t)metadata;
	r = kmtIoctl(kfd_fd, AMDKFD_IOC_GET_DMABUF_INFO, (void *)&infoArgs);
	if (r && infoArgs.metadata_size > GRAPHICS_METADATA_DEFAULT_SIZE) {
		/* Try again with bigger metadata */
		free(metadata);
		metadata = calloc(infoArgs.metadata_size, 1);
		if (!metadata)
			return HSAKMT_STATUS_NO_MEMORY;
		infoArgs.metadata_ptr = (uint64_t)metadata;
		r = kmtIoctl(kfd_fd, AMDKFD_IOC_GET_DMABUF_INFO, (void *)&infoArgs);
	}

	if (r)
		goto error_free_metadata;

	/* Choose aperture based on GPU and allocate virtual address */
	gpu_mem_id = gpu_mem_find_by_gpu_id(infoArgs.gpu_id);
	if (gpu_mem_id < 0)
		goto error_free_metadata;
	if (topology_is_svm_needed(gpu_mem[gpu_mem_id].device_id)) {
		aperture = &svm.dgpu_aperture;
		aperture_base = NULL;
		offset = 0;
	} else {
		aperture = &gpu_mem[gpu_mem_id].gpuvm_aperture;
		aperture_base = aperture->base;
		offset = GPUVM_APP_OFFSET;
	}
	if (!aperture_is_valid(aperture->base, aperture->limit))
		goto error_free_metadata;
	pthread_mutex_lock(&aperture->fmm_mutex);
	mem = aperture_allocate_area_aligned(aperture, infoArgs.size, offset,
					     MAX(aperture->align, IMAGE_ALIGN));
	pthread_mutex_unlock(&aperture->fmm_mutex);
	if (!mem)
		goto error_free_metadata;

	/* Import DMA buffer */
	importArgs.va_addr = VOID_PTRS_SUB(mem, aperture_base);
	importArgs.gpu_id = infoArgs.gpu_id;
	importArgs.dmabuf_fd = GraphicsResourceHandle;
	r = kmtIoctl(kfd_fd, AMDKFD_IOC_IMPORT_DMABUF, (void *)&importArgs);
	if (r)
		goto error_release_aperture;

	pthread_mutex_lock(&aperture->fmm_mutex);
	obj = aperture_allocate_object(aperture, mem, importArgs.handle,
				       infoArgs.size, infoArgs.flags);
	if (obj) {
		obj->metadata = metadata;
		obj->registered_device_id_array = gpu_id_array;
		obj->registered_device_id_array_size = gpu_id_array_size;
		gpuid_to_nodeid(infoArgs.gpu_id, &obj->node_id);
	}
	pthread_mutex_unlock(&aperture->fmm_mutex);
	if (!obj)
		goto error_release_buffer;

	GraphicsResourceInfo->MemoryAddress = mem;
	GraphicsResourceInfo->SizeInBytes = infoArgs.size;
	GraphicsResourceInfo->Metadata = (void *)(unsigned long)infoArgs.metadata_ptr;
	GraphicsResourceInfo->MetadataSizeInBytes = infoArgs.metadata_size;
	GraphicsResourceInfo->Reserved = 0;

	return HSAKMT_STATUS_SUCCESS;

error_release_buffer:
	freeArgs.handle = importArgs.handle;
	kmtIoctl(kfd_fd, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &freeArgs);
error_release_aperture:
	aperture_release_area(aperture, mem, infoArgs.size);
error_free_metadata:
	free(metadata);

	return status;
}

HSAKMT_STATUS fmm_share_memory(void *MemoryAddress,
				HSAuint64 SizeInBytes,
				HsaSharedMemoryHandle *SharedMemoryHandle)
{
	int r = 0;
	HSAuint32 gpu_id = 0;
	vm_object_t *obj = NULL;
	manageable_aperture_t *aperture = NULL;
	struct kfd_ioctl_ipc_export_handle_args exportArgs = {0};
	HsaApertureInfo ApeInfo;
	HsaSharedMemoryStruct *SharedMemoryStruct =
		to_hsa_shared_memory_struct(SharedMemoryHandle);

	if (SizeInBytes >= (1ULL << ((sizeof(HSAuint32) * 8) + PAGE_SHIFT)))
		return HSAKMT_STATUS_INVALID_PARAMETER;

	aperture = fmm_find_aperture(MemoryAddress, &ApeInfo);
	if (!aperture)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	pthread_mutex_lock(&aperture->fmm_mutex);
	obj = vm_find_object_by_address(aperture, MemoryAddress, 0);
	pthread_mutex_unlock(&aperture->fmm_mutex);
	if (!obj)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	r = validate_nodeid(obj->node_id, &gpu_id);
	if (r != HSAKMT_STATUS_SUCCESS)
		return r;
	if (!gpu_id && is_dgpu) {
		/* Sharing non paged system memory. Use first dgpu which was
		 * used during allocation. See fmm_allocate_host_gpu()
		 */
		r = find_first_dgpu(&gpu_id);
		if (r != HSAKMT_STATUS_SUCCESS)
			return r;
	}
	exportArgs.handle = obj->handle;
	exportArgs.gpu_id = gpu_id;


	r = kmtIoctl(kfd_fd, AMDKFD_IOC_IPC_EXPORT_HANDLE, (void *)&exportArgs);
	if (r)
		return HSAKMT_STATUS_ERROR;

	memcpy(SharedMemoryStruct->ShareHandle, exportArgs.share_handle,
			sizeof(SharedMemoryStruct->ShareHandle));
	SharedMemoryStruct->ApeInfo = ApeInfo;
	SharedMemoryStruct->SizeInPages = (HSAuint32) (SizeInBytes >> PAGE_SHIFT);
	SharedMemoryStruct->ExportGpuId = gpu_id;

	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS fmm_register_shared_memory(const HsaSharedMemoryHandle *SharedMemoryHandle,
						HSAuint64 *SizeInBytes,
						void **MemoryAddress,
						uint32_t *gpu_id_array,
						uint32_t gpu_id_array_size)
{
	int r = 0;
	HSAKMT_STATUS err = HSAKMT_STATUS_ERROR;
	vm_object_t *obj = NULL;
	void *reservedMem = NULL;
	manageable_aperture_t *aperture;
	struct kfd_ioctl_ipc_import_handle_args importArgs = {0};
	struct kfd_ioctl_free_memory_of_gpu_args freeArgs = {0};
	const HsaSharedMemoryStruct *SharedMemoryStruct =
		to_const_hsa_shared_memory_struct(SharedMemoryHandle);

	if (gpu_id_array_size > 0 && !gpu_id_array)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	memcpy(importArgs.share_handle, SharedMemoryStruct->ShareHandle,
			sizeof(importArgs.share_handle));
	importArgs.gpu_id = SharedMemoryStruct->ExportGpuId;

	aperture = fmm_get_aperture(SharedMemoryStruct->ApeInfo);

	pthread_mutex_lock(&aperture->fmm_mutex);
	reservedMem = aperture_allocate_area(aperture,
					     (SharedMemoryStruct->SizeInPages << PAGE_SHIFT),
					     0);
	pthread_mutex_unlock(&aperture->fmm_mutex);
	if (!reservedMem) {
		err = HSAKMT_STATUS_NO_MEMORY;
		goto err_free_buffer;
	}

	importArgs.va_addr = (uint64_t)reservedMem;
	r = kmtIoctl(kfd_fd, AMDKFD_IOC_IPC_IMPORT_HANDLE, (void *)&importArgs);
	if (r) {
		err = HSAKMT_STATUS_ERROR;
		goto err_import;
	}

	pthread_mutex_lock(&aperture->fmm_mutex);
	obj = aperture_allocate_object(aperture, reservedMem, importArgs.handle,
				       (SharedMemoryStruct->SizeInPages << PAGE_SHIFT),
				       0);
	if (!obj) {
		err = HSAKMT_STATUS_NO_MEMORY;
		goto err_free_mem;
	}
	pthread_mutex_unlock(&aperture->fmm_mutex);

	if (importArgs.mmap_offset) {
		int32_t gpu_mem_id = gpu_mem_find_by_gpu_id(importArgs.gpu_id);
		int map_fd;
		void *ret;

		if (gpu_mem_id < 0) {
			err = HSAKMT_STATUS_ERROR;
			goto err_free_obj;
		}
		map_fd = importArgs.mmap_offset >= (1ULL<<40) ? kfd_fd :
					gpu_mem[gpu_mem_id].drm_render_fd;
		ret = mmap(reservedMem, (SharedMemoryStruct->SizeInPages << PAGE_SHIFT),
			   PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_FIXED, map_fd, importArgs.mmap_offset);
		if (ret == MAP_FAILED) {
			err = HSAKMT_STATUS_ERROR;
			goto err_free_obj;
		}
	}

	*MemoryAddress = reservedMem;
	*SizeInBytes = (SharedMemoryStruct->SizeInPages << PAGE_SHIFT);

	if (gpu_id_array_size > 0) {
		obj->registered_device_id_array = gpu_id_array;
		obj->registered_device_id_array_size = gpu_id_array_size;
	}
	obj->is_imported_kfd_bo = true;

	return HSAKMT_STATUS_SUCCESS;
err_free_obj:
	pthread_mutex_lock(&aperture->fmm_mutex);
	vm_remove_object(aperture, obj);
err_free_mem:
	aperture_release_area(aperture, reservedMem, (SharedMemoryStruct->SizeInPages << PAGE_SHIFT));
	pthread_mutex_unlock(&aperture->fmm_mutex);
err_free_buffer:
	freeArgs.handle = importArgs.handle;
	kmtIoctl(kfd_fd, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &freeArgs);
err_import:
	return err;
}

static HSAKMT_STATUS fmm_deregister_user_memory(void *addr)
{
	manageable_aperture_t *aperture;
	vm_object_t *obj;
	void *svm_addr;

	aperture = &svm.dgpu_aperture;

	/* Find the size and start address in SVM space */
	pthread_mutex_lock(&aperture->fmm_mutex);
	obj = vm_find_object_by_userptr(aperture, addr, 0);
	if (!obj || obj->registration_count > 1) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_ERROR;
	}
	svm_addr = obj->start;
	pthread_mutex_unlock(&aperture->fmm_mutex);

	/* Destroy BO */
	__fmm_release(svm_addr, aperture);

	return HSAKMT_STATUS_SUCCESS;
}

HSAKMT_STATUS fmm_deregister_memory(void *address)
{
	manageable_aperture_t *aperture = NULL;
	vm_object_t *object = NULL;
	unsigned int i;
	HSAuint32 page_offset = (HSAint64)address & (PAGE_SIZE - 1);

	if ((address >= svm.dgpu_aperture.base) &&
	    (address <= svm.dgpu_aperture.limit))
		aperture = &svm.dgpu_aperture;
	else if ((address >= svm.dgpu_alt_aperture.base) &&
		 (address <= svm.dgpu_alt_aperture.limit))
		aperture = &svm.dgpu_alt_aperture;
	else
		for (i = 0; i < gpu_mem_count; i++) {
			if (gpu_mem[i].gpu_id != NON_VALID_GPU_ID &&
			    address >= gpu_mem[i].gpuvm_aperture.base &&
			    address <= gpu_mem[i].gpuvm_aperture.limit) {
				aperture = &gpu_mem[i].gpuvm_aperture;
				break;
			}
		}

	if (!aperture) {
		/* If address isn't found in any aperture, we assume
		 * that this is system memory address. On APUs, there
		 * is nothing to do (for now).
		 */
		if (!is_dgpu)
			return HSAKMT_STATUS_SUCCESS;
		/* If the userptr object had a
		 * registered_device_id_array, it will be freed by
		 * __fmm_release. Also the object will be
		 * removed. Therefore we can short-circuit the rest of
		 * the function below.
		 */
		return fmm_deregister_user_memory(address);
	}

	pthread_mutex_lock(&aperture->fmm_mutex);

	object = vm_find_object_by_address(aperture,
				VOID_PTR_SUB(address, page_offset), 0);
	if (!object) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_MEMORY_NOT_REGISTERED;
	}

	if (object->registration_count > 1) {
		--object->registration_count;
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_SUCCESS;
	}

	if (object->metadata || object->userptr || object->is_imported_kfd_bo) {
		/* An object with metadata is an imported graphics
		 * buffer. Deregistering imported graphics buffers or
		 * userptrs means releasing the BO.
		 */
		pthread_mutex_unlock(&aperture->fmm_mutex);
		__fmm_release(address, aperture);
		return HSAKMT_STATUS_SUCCESS;
	}

	if (!object->registered_device_id_array ||
		object->registered_device_id_array_size <= 0) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_MEMORY_NOT_REGISTERED;
	}

	if (object->registered_device_id_array) {
		free(object->registered_device_id_array);
		object->registered_device_id_array = NULL;
		object->registered_device_id_array_size = 0;
	}
	if (object->registered_node_id_array)
		free(object->registered_node_id_array);
	object->registered_node_id_array = NULL;
	object->registration_count = 0;

	pthread_mutex_unlock(&aperture->fmm_mutex);

	return HSAKMT_STATUS_SUCCESS;
}

/*
 * This function unmaps all nodes on current mapped nodes list that are not included on nodes_to_map
 * and maps nodes_to_map
 */

HSAKMT_STATUS fmm_map_to_gpu_nodes(void *address, uint64_t size,
		uint32_t *nodes_to_map, uint64_t num_of_nodes,
		uint64_t *gpuvm_address)
{
	manageable_aperture_t *aperture;
	vm_object_t *object = NULL;
	uint32_t i;
	bool userptr = false;
	uint32_t *registered_node_id_array, registered_node_id_array_size;
	HSAKMT_STATUS ret = HSAKMT_STATUS_ERROR;
	int retcode = 0;

	if (!num_of_nodes || !nodes_to_map || !address)
		return HSAKMT_STATUS_INVALID_PARAMETER;

	/* Find object by address */
	if ((address >= svm.dgpu_aperture.base) &&
	    (address <= svm.dgpu_aperture.limit))
		aperture = &svm.dgpu_aperture;
	else if ((address >= svm.dgpu_alt_aperture.base) &&
		 (address <= svm.dgpu_alt_aperture.limit))
		aperture = &svm.dgpu_alt_aperture;
	else {
		aperture = &svm.dgpu_aperture;
		userptr = true;
	}

	pthread_mutex_lock(&aperture->fmm_mutex);
	if (userptr && is_dgpu)
		object = vm_find_object_by_userptr(aperture, address, size);
	else
		object = vm_find_object_by_address(aperture, address, 0);

	if (!object) {
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return HSAKMT_STATUS_ERROR;
	}

	/* For userptr, we ignore the nodes array and map all registered nodes.
	 * This is to simply the implementation of allowing the same memory
	 * region to be registered multiple times.
	 */
	if (userptr && is_dgpu) {
		retcode = _fmm_map_to_gpu_userptr(address, size,
					gpuvm_address, object);
		pthread_mutex_unlock(&aperture->fmm_mutex);
		return retcode;
	}

	/* Verify that all nodes to map are registered already */
	registered_node_id_array = all_gpu_id_array;
	registered_node_id_array_size = all_gpu_id_array_size;
	if (object->registered_device_id_array_size > 0 &&
			object->registered_device_id_array) {
		registered_node_id_array = object->registered_device_id_array;
		registered_node_id_array_size = object->registered_device_id_array_size;
	}
	for (i = 0 ; i < num_of_nodes; i++) {
		if (!id_in_array(nodes_to_map[i], registered_node_id_array,
					registered_node_id_array_size)) {
			pthread_mutex_unlock(&aperture->fmm_mutex);
			return HSAKMT_STATUS_ERROR;
		}
	}

	/* Unmap buffer from all nodes that have this buffer mapped that are not included on nodes_to_map array */
	if (object->mapped_device_id_array_size > 0) {
		uint32_t temp_node_id_array[object->mapped_device_id_array_size];
		uint32_t temp_node_id_array_size = 0;

		for (i = 0 ; i < object->mapped_device_id_array_size / sizeof(uint32_t); i++) {
			if (!id_in_array(object->mapped_device_id_array[i],
					nodes_to_map,
					num_of_nodes*sizeof(uint32_t)))
				temp_node_id_array[temp_node_id_array_size++] =
					object->mapped_device_id_array[i];
		}
		temp_node_id_array_size *= sizeof(uint32_t);

		if (temp_node_id_array_size) {
			ret = _fmm_unmap_from_gpu(aperture, address,
					temp_node_id_array,
					temp_node_id_array_size,
					object);
			if (ret != HSAKMT_STATUS_SUCCESS) {
				pthread_mutex_unlock(&aperture->fmm_mutex);
				return ret;
			}
		}
	}

	/* Remove already mapped nodes from nodes_to_map
	 * to generate the final map list
	 */
	uint32_t map_node_id_array[num_of_nodes];
	uint32_t map_node_id_array_size = 0;

	for (i = 0; i < num_of_nodes; i++) {
		if (!id_in_array(nodes_to_map[i],
				object->mapped_device_id_array,
				object->mapped_device_id_array_size))
			map_node_id_array[map_node_id_array_size++] =
				nodes_to_map[i];
	}

	if (map_node_id_array_size)
		retcode = _fmm_map_to_gpu(aperture, address, size, object,
				map_node_id_array,
				map_node_id_array_size * sizeof(uint32_t));

	pthread_mutex_unlock(&aperture->fmm_mutex);

	if (retcode != 0)
		return HSAKMT_STATUS_ERROR;

	return 0;
}

HSAKMT_STATUS fmm_get_mem_info(const void *address, HsaPointerInfo *info)
{
	HSAKMT_STATUS ret = HSAKMT_STATUS_SUCCESS;
	uint32_t i;
	manageable_aperture_t *aperture;
	vm_object_t *vm_obj;

	memset(info, 0, sizeof(HsaPointerInfo));

	aperture = fmm_find_aperture(address, NULL);

	pthread_mutex_lock(&aperture->fmm_mutex);
	vm_obj = vm_find_object_by_address_range(aperture, address);
	if (!vm_obj)
		vm_obj = vm_find_object_by_userptr_range(aperture, address);

	if (!vm_obj) {
		info->Type = HSA_POINTER_UNKNOWN;
		ret = HSAKMT_STATUS_ERROR;
		goto exit;
	}

	if (vm_obj->metadata)
		info->Type = HSA_POINTER_REGISTERED_GRAPHICS;
	else if (vm_obj->userptr)
		info->Type = HSA_POINTER_REGISTERED_USER;
	else
		info->Type = HSA_POINTER_ALLOCATED;

	info->Node = vm_obj->node_id;
	info->GPUAddress = (HSAuint64)vm_obj->start;
	info->SizeInBytes = vm_obj->size;
	/* registered nodes */
	info->NRegisteredNodes =
		vm_obj->registered_device_id_array_size / sizeof(uint32_t);
	if (info->NRegisteredNodes && !vm_obj->registered_node_id_array) {
		vm_obj->registered_node_id_array = (uint32_t *)
			(uint32_t *)malloc(vm_obj->registered_device_id_array_size);
		/* vm_obj->registered_node_id_array allocated here will be
		 * freed whenever the registration is changed (deregistration or
		 * register to new nodes) or the memory being freed
		 */
		for (i = 0; i < info->NRegisteredNodes; i++)
			gpuid_to_nodeid(vm_obj->registered_device_id_array[i],
				&vm_obj->registered_node_id_array[i]);
	}
	info->RegisteredNodes = vm_obj->registered_node_id_array;
	/* mapped nodes */
	info->NMappedNodes =
		vm_obj->mapped_device_id_array_size / sizeof(uint32_t);
	if (info->NMappedNodes && !vm_obj->mapped_node_id_array) {
		vm_obj->mapped_node_id_array =
			(uint32_t *)malloc(vm_obj->mapped_device_id_array_size);
		/* vm_obj->mapped_node_id_array allocated here will be
		 * freed whenever the mapping is changed (unmapped or map
		 * to new nodes) or memory being freed
		 */
		for (i = 0; i < info->NMappedNodes; i++)
			gpuid_to_nodeid(vm_obj->mapped_device_id_array[i],
				&vm_obj->mapped_node_id_array[i]);
	}
	info->MappedNodes = vm_obj->mapped_node_id_array;
	info->UserData = vm_obj->user_data;

	if (info->Type == HSA_POINTER_REGISTERED_USER) {
		info->CPUAddress = vm_obj->userptr;
		info->SizeInBytes = vm_obj->userptr_size;
		info->GPUAddress += ((HSAuint64)info->CPUAddress & (PAGE_SIZE - 1));
	} else if (info->Type == HSA_POINTER_ALLOCATED) {
		info->MemFlags.Value = vm_obj->flags;
		info->CPUAddress = vm_obj->start;
	}

exit:
	pthread_mutex_unlock(&aperture->fmm_mutex);
	return ret;
}

HSAKMT_STATUS fmm_set_mem_user_data(const void *mem, void *usr_data)
{
	manageable_aperture_t *aperture;
	vm_object_t *vm_obj;

	aperture = fmm_find_aperture(mem, NULL);

	vm_obj = vm_find_object_by_address(aperture, mem, 0);
	if (!vm_obj)
		vm_obj = vm_find_object_by_userptr(aperture, mem, 0);
	if (!vm_obj)
		return HSAKMT_STATUS_ERROR;

	vm_obj->user_data = usr_data;
	return HSAKMT_STATUS_SUCCESS;
}

static void fmm_clear_aperture(manageable_aperture_t *app)
{
	while (app->vm_objects)
		vm_remove_object(app, app->vm_objects);

	while (app->vm_ranges)
		vm_remove_area(app, app->vm_ranges);

}

/* This is a special funcion that should be called only from the child process
 * after a fork(). This will clear all vm_objects and mmaps duplicated from
 * the parent.
 */
void fmm_clear_all_mem(void)
{
	uint32_t i;
	void *map_addr;

	/* Close render node FDs. The child process needs to open new ones */
	for (i = 0; i <= DRM_LAST_RENDER_NODE - DRM_FIRST_RENDER_NODE; i++)
		if (drm_render_fds[i]) {
			close(drm_render_fds[i]);
			drm_render_fds[i] = 0;
		}

	/* Nothing is initialized. */
	if (!gpu_mem)
		return;

	fmm_clear_aperture(&cpuvm_aperture);

	for (i = 0; i < gpu_mem_count; i++) {
		fmm_clear_aperture(&gpu_mem[i].gpuvm_aperture);
		fmm_clear_aperture(&gpu_mem[i].scratch_aperture);
		fmm_clear_aperture(&gpu_mem[i].scratch_physical);
	}

	if (dgpu_shared_aperture_limit) {
		fmm_clear_aperture(&svm.dgpu_aperture);
		fmm_clear_aperture(&svm.dgpu_alt_aperture);

		/* Use the same dgpu range as the parent. If failed, then set
		 * is_dgpu_mem_init to false. Later on dgpu_mem_init will try
		 * to get a new range
		 */
		map_addr = mmap(dgpu_shared_aperture_base, (HSAuint64)(dgpu_shared_aperture_limit)-
			(HSAuint64)(dgpu_shared_aperture_base) + 1, PROT_NONE,
			MAP_ANONYMOUS | MAP_NORESERVE | MAP_PRIVATE | MAP_FIXED, -1, 0);

		if (map_addr == MAP_FAILED) {
			munmap(dgpu_shared_aperture_base,
				   (HSAuint64)(dgpu_shared_aperture_limit) -
				   (HSAuint64)(dgpu_shared_aperture_base) + 1);

			dgpu_shared_aperture_base = NULL;
			dgpu_shared_aperture_limit = NULL;
		}
	}

	if (all_gpu_id_array)
		free(all_gpu_id_array);

	all_gpu_id_array_size = 0;
	all_gpu_id_array = NULL;

	gpu_mem_count = 0;
	free(gpu_mem);
}
