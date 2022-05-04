/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/vmd.h"
#include "spdk/nvme_zns.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr		*ctrlr;
	TAILQ_ENTRY(ctrlr_entry)	link;
	char				name[1024];
};

struct ns_entry {
	struct spdk_nvme_ctrlr	*ctrlr;
	struct spdk_nvme_ns	*ns;
	TAILQ_ENTRY(ns_entry)	link;
	struct spdk_nvme_qpair	*qpair;
};

// 存放所有 ctrlr_entry 的链表头
static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
// 存放所有 namespace 的链表头
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static struct spdk_nvme_transport_id g_trid = {};

static bool g_vmd = false;

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;

	if (!spdk_nvme_ns_is_active(ns)) {
		return;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->ctrlr = ctrlr;
	entry->ns = ns;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);

	printf("  Namespace ID: %d size: %juGB\n", spdk_nvme_ns_get_id(ns),
	       spdk_nvme_ns_get_size(ns) / 1000000000);
}

struct hello_world_sequence {
	struct ns_entry	*ns_entry;
	char		*buf;
	// 是否 using controller memory buffer for IO
	unsigned        using_cmb_io;
	int		is_completed;
};

static void
read_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the sequence as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	printf("%s", sequence->buf);
	spdk_free(sequence->buf);
}

static void
write_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence	*sequence = arg;
	struct ns_entry			*ns_entry = sequence->ns_entry;
	int				rc;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	// 成功的时候没有任何输出
	spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 */
	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(sequence->buf);
	}
	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence->buf,
				   0, /* LBA start */
				   1, /* number of LBAs */
				   read_complete, (void *)sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}

static void
read_zeros_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Read I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}

	/*
	 * The read I/O has completed.  Print the contents of the
	 *  buffer, free the buffer, then mark the sequence as
	 *  completed.  This will trigger the hello_world() function
	 *  to exit its polling loop.
	 */
	uint64_t* buf_64 = (uint64_t*)sequence->buf;
	for(uint64_t i = 0; i < spdk_nvme_ns_get_sector_size(sequence->ns_entry->ns)/sizeof(uint64_t); ++i) {
		if(buf_64[i] != 0) {
			printf("u64 i: %lu 非零!\n", i);
		}
	}
	printf("全零!\n");
	spdk_free(sequence->buf);
}

static void
write_zeros_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence	*sequence = arg;
	struct ns_entry			*ns_entry = sequence->ns_entry;
	int				rc;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		fprintf(stderr, "Unsuppored write zeros\n");
		sequence->is_completed = 2;
		exit(1);
	}
	// 成功的时候没有任何输出
	spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
	/*
	 * The write I/O has completed.  Free the buffer associated with
	 *  the write I/O and allocate a new zeroed buffer for reading
	 *  the data back from the NVMe namespace.
	 */
	if (sequence->using_cmb_io) {
		spdk_nvme_ctrlr_unmap_cmb(ns_entry->ctrlr);
	} else {
		spdk_free(sequence->buf);
	}
	sequence->buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, sequence->buf,
				   0, /* LBA start */
				   1, /* number of LBAs */
				   read_zeros_complete, (void *)sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
}

static void
reset_zone_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence *sequence = arg;

	/* Assume the I/O was successful */
	sequence->is_completed = 1;
	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Reset zone I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
}

static void
reset_zone_and_wait_for_completion(struct hello_world_sequence *sequence)
{
	// 提交IO操作
	if (spdk_nvme_zns_reset_zone(sequence->ns_entry->ns, sequence->ns_entry->qpair,
				     0, /* starting LBA of the zone to reset */
				     false, /* don't reset all zones */
				     reset_zone_complete,
				     sequence)) {
		fprintf(stderr, "starting reset zone I/O failed\n");
		exit(1);
	}
	while (!sequence->is_completed) {
		// 轮询操作完成
		spdk_nvme_qpair_process_completions(sequence->ns_entry->qpair, 0);
	}
	sequence->is_completed = 0;
}

static void
hello_world(void)
{
	struct ns_entry			*ns_entry;
	struct hello_world_sequence	sequence;
	int				rc;
	size_t				sz;

	TAILQ_FOREACH(ns_entry, &g_namespaces, link) {
		/*
		 * Allocate an I/O qpair that we can use to submit read/write requests
		 *  to namespaces on the controller.  NVMe controllers typically support
		 *  many qpairs per controller.  Any I/O qpair allocated for a controller
		 *  can submit I/O to any namespace on that controller.
		 * 为控制器分配的任何 I/O qpair 都可以将 I/O 提交到该控制器上的任何命名空间。
		 *
		 * The SPDK NVMe driver provides no synchronization for qpair accesses -
		 *  the application must ensure only a single thread submits I/O to a
		 *  qpair, and that same thread must also check for completions on that
		 *  qpair.  This enables extremely efficient I/O processing by making all
		 *  I/O operations completely lockless.
		 * 不提供任何qp同步，因此一个qp只能单个线程使用
		 */
		// 3.1 分配qp
		ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
		if (ns_entry->qpair == NULL) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
			return;
		}

		/*
		 * Use spdk_dma_zmalloc to allocate a 4KB zeroed buffer.  This memory
		 * will be pinned, which is required for data buffers used for SPDK NVMe
		 * I/O operations.
		 * 3.2 分配一个4KB的页并pin，用于SPDK的I/O操作
		 */
		sequence.using_cmb_io = 1;
		sequence.buf = spdk_nvme_ctrlr_map_cmb(ns_entry->ctrlr, &sz);
		if (sequence.buf == NULL || sz < 0x1000) {
			sequence.using_cmb_io = 0;
			sequence.buf = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		}
		if (sequence.buf == NULL) {
			printf("ERROR: write buffer allocation failed\n");
			return;
		}
		if (sequence.using_cmb_io) {
			printf("INFO: using controller memory buffer for IO\n");
		} else {
			printf("INFO: using host memory buffer for IO\n");
		}
		sequence.is_completed = 0;
		sequence.ns_entry = ns_entry;

		/*
		 * If the namespace is a Zoned Namespace, rather than a regular
		 * NVM namespace, we need to reset the first zone, before we
		 * write to it. This not needed for regular NVM namespaces.
		 */
		if (spdk_nvme_ns_get_csi(ns_entry->ns) == SPDK_NVME_CSI_ZNS) {
			reset_zone_and_wait_for_completion(&sequence);
		}

		/*
		 * Print "Hello world!" to sequence.buf.  We will write this data to LBA
		 *  0 on the namespace, and then later read it back into a separate buffer
		 *  to demonstrate the full I/O path.
		 */
		snprintf(sequence.buf, 0x1000, "%s", "Hello world!\n");

		/*
		 * Write the data buffer to LBA 0 of this namespace.  "write_complete" and
		 *  "&sequence" are specified as the completion callback function and
		 *  argument respectively.  write_complete() will be called with the
		 *  value of &sequence as a parameter when the write I/O is completed.
		 *  This allows users to potentially specify different completion
		 *  callback routines for each I/O, as well as pass a unique handle
		 *  as an argument so the application knows which I/O has completed.
		 * write_complete（） 将在写入 I/O 完成时以 &sequence 的 * 值作为参数进行调用。
		 * 这允许用户为每个 I/O 指定不同的完成 * 回调例程，以及传递一个唯一的句柄 * 作为参数，
		 * 以便应用程序知道哪个 I/O 已完成。
		 *
		 * Note that the SPDK NVMe driver will only check for completions
		 *  when the application calls spdk_nvme_qpair_process_completions().
		 *  It is the responsibility of the application to trigger the polling
		 *  process.
		 */
		rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, sequence.buf,
					    0, /* LBA start */
					    1, /* number of LBAs */
					    write_complete, &sequence, 0);
		if (rc != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			exit(1);
		}

		/*
		 * Poll for completions.  0 here means process all available completions.
		 *  In certain usage models, the caller may specify a positive integer
		 *  instead of 0 to signify the maximum number of completions it should
		 *  process.  This function will never block - if there are no
		 *  completions pending on the specified qpair, it will return immediately.
		 *
		 * When the write I/O completes, write_complete() will submit a new I/O
		 *  to read LBA 0 into a separate buffer, specifying read_complete() as its
		 *  completion routine.  When the read I/O completes, read_complete() will
		 *  print the buffer contents and set sequence.is_completed = 1.  That will
		 *  break this loop and then exit the program.
		 */
		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		// my test
		sequence.is_completed = 0;
		rc = spdk_nvme_ns_cmd_write_zeroes(ns_entry->ns, ns_entry->qpair,
					    0, /* LBA start */
					    1, /* number of LBAs */
					    write_zeros_complete, &sequence, 0);
		if (rc != 0) {
			fprintf(stderr, "write_zeroes I/O failed\n");
			exit(1);
		}
		while (!sequence.is_completed) {
			spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		}

		/*
		 * Free the I/O qpair.  This typically is done when an application exits.
		 *  But SPDK does support freeing and then reallocating qpairs during
		 *  operation.  It is the responsibility of the caller to ensure all
		 *  pending I/O are completed before trying to free the qpair.
		 */
		spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
	}
}

static void
test_deallocate_common_complete(void *arg, const struct spdk_nvme_cpl *completion)
{
	struct hello_world_sequence	*sequence = arg;

	/* See if an error occurred. If so, display information
	 * about it, and set completion value so that I/O
	 * caller is aware that an error occurred.
	 */
	if (spdk_nvme_cpl_is_error(completion)) {
		spdk_nvme_qpair_print_completion(sequence->ns_entry->qpair, (struct spdk_nvme_cpl *)completion);
		fprintf(stderr, "I/O error status: %s\n", spdk_nvme_cpl_get_status_string(&completion->status));
		fprintf(stderr, "Write I/O failed, aborting run\n");
		sequence->is_completed = 2;
		exit(1);
	}
	sequence->is_completed = 1;
}

// 自添加
static void
test_deallocate(void)
{
	struct ns_entry			*ns_entry;
	struct hello_world_sequence	sequence;
	int				rc;
	ns_entry = TAILQ_FIRST(&g_namespaces);
	ns_entry->qpair = spdk_nvme_ctrlr_alloc_io_qpair(ns_entry->ctrlr, NULL, 0);
	if (ns_entry->qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return;
	}

	const int LBA_NUM = 64;
	const int START_LBA_FOR_DSM = 256;
	const int START_LBA_FOR_RAW = START_LBA_FOR_DSM + LBA_NUM;
	const int RANGE_NUM = 32;
	const int LBA_LEVEL = LBA_NUM / RANGE_NUM;
	int sector_size = spdk_nvme_ns_get_sector_size(ns_entry->ns);
	size_t total_size = LBA_NUM * sector_size;

	void* write_buf = spdk_zmalloc(total_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (write_buf == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}
	void* dsm_read_buf = spdk_zmalloc(total_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (dsm_read_buf == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}
	void* raw_read_buf = spdk_zmalloc(total_size, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (raw_read_buf == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}

	memset(write_buf, 1, total_size);
	memset(dsm_read_buf, 2, total_size);
	memset(raw_read_buf, 3, total_size);

	strcpy((char*)write_buf, "TEST spdk_nvme_ctrlr_cmd_io_raw Deallocate!!!");

	struct spdk_nvme_dsm_range* dsm_ranges = (struct spdk_nvme_dsm_range*)malloc(sizeof(struct spdk_nvme_dsm_range)*RANGE_NUM);
	if (dsm_ranges == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}
	struct spdk_nvme_dsm_range* raw_ranges = spdk_zmalloc(0x1000, 0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (raw_ranges == NULL) {
		printf("ERROR: write buffer allocation failed\n");
		return;
	}

	// 首先写数据
	sequence.is_completed = 0;
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, write_buf,
					    START_LBA_FOR_DSM, /* LBA start */
					    LBA_NUM, /* number of LBAs */
					    test_deallocate_common_complete, &sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}
	sequence.is_completed = 0;
	rc = spdk_nvme_ns_cmd_write(ns_entry->ns, ns_entry->qpair, write_buf,
					    START_LBA_FOR_RAW, /* LBA start */
					    LBA_NUM, /* number of LBAs */
					    test_deallocate_common_complete, &sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		exit(1);
	}
	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	// DSM释放数据
	for(int i = 0; i < RANGE_NUM; ++i) {
		dsm_ranges[i].attributes.raw = 0;
		dsm_ranges[i].starting_lba = START_LBA_FOR_DSM + i * LBA_LEVEL;
		dsm_ranges[i].length = LBA_LEVEL;
	}
	sequence.is_completed = 0;
	rc = spdk_nvme_ns_cmd_dataset_management(ns_entry->ns, ns_entry->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE, dsm_ranges, RANGE_NUM, test_deallocate_common_complete, &sequence);
	if (rc) {
		printf("Error in nvme command completion, values may be inaccurate.\n");
	}
	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}
	printf("spdk_nvme_ns_cmd_dataset_management over\n");

	// RAW 释放数据
	for(int i = 0; i < RANGE_NUM; ++i) {
		raw_ranges[i].attributes.raw = 0;
		raw_ranges[i].starting_lba = START_LBA_FOR_RAW + i * LBA_LEVEL;
		raw_ranges[i].length = LBA_LEVEL;
	}
	sequence.is_completed = 0;
	struct spdk_nvme_cmd cmd = {0};
	cmd.opc = SPDK_NVME_OPC_DATASET_MANAGEMENT;
	cmd.nsid = spdk_nvme_ns_get_id(ns_entry->ns);
	cmd.cdw10_bits.dsm.nr = RANGE_NUM - 1;
	cmd.cdw11 = SPDK_NVME_DSM_ATTR_DEALLOCATE;
	rc = spdk_nvme_ctrlr_cmd_io_raw(ns_entry->ctrlr, ns_entry->qpair, &cmd, raw_ranges,
		LBA_NUM * sizeof(struct spdk_nvme_dsm_range), test_deallocate_common_complete, &sequence);
	if (rc) {
		printf("Error in nvme command completion, values may be inaccurate.\n");
	}
	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}
	printf("spdk_nvme_ctrlr_cmd_io_raw over\n");

	// 读出来 DSM
	sequence.is_completed = 0;
	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, dsm_read_buf,
				   START_LBA_FOR_DSM, /* LBA start */
				   LBA_NUM, /* number of LBAs */
				   test_deallocate_common_complete, &sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}

	// 读出来 RAW
	sequence.is_completed = 0;
	rc = spdk_nvme_ns_cmd_read(ns_entry->ns, ns_entry->qpair, raw_read_buf,
				   START_LBA_FOR_RAW, /* LBA start */
				   LBA_NUM, /* number of LBAs */
				   test_deallocate_common_complete, &sequence, 0);
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		exit(1);
	}
	while (!sequence.is_completed) {
		spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	}
	if(memcmp(dsm_read_buf, raw_read_buf, total_size) == 0) {
		printf("=== spdk_nvme_ctrlr_cmd_io_raw SUCCESS!\n");
	} else {
		printf("=== spdk_nvme_ctrlr_cmd_io_raw FAIL!\n");
	}
	if(memcmp(dsm_read_buf, write_buf, total_size) == 0) {
		printf("=== deallocate read [EQUAL] write buf!\n");
	} else {
		printf("=== deallocate read [NOT EQUAL] write buf!\n");
	}
	printf("=== dsm_read_buf: %s\n", (char*)dsm_read_buf);
	printf("=== raw_read_buf: %s\n", (char*)raw_read_buf);

	spdk_free(raw_ranges);
	spdk_free(write_buf);
	spdk_free(dsm_read_buf);
	spdk_free(raw_read_buf);

	/*
	 * Free the I/O qpair.  This typically is done when an application exits.
	 *  But SPDK does support freeing and then reallocating qpairs during
	 *  operation.  It is the responsibility of the caller to ensure all
	 *  pending I/O are completed before trying to free the qpair.
	 */
	spdk_nvme_ctrlr_free_io_qpair(ns_entry->qpair);
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("Attaching to %s\n", trid->traddr);

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	printf("spdk_nvme_ctrlr_opts default:\n");
	printf("\tnum_io_queues:%u\n", opts->num_io_queues);
	printf("\tuse_cmb_sqs:%u\n", opts->use_cmb_sqs);
	printf("\tio_queue_size:%u\n", opts->io_queue_size);
	printf("\tio_queue_requests:%u\n", opts->io_queue_requests);
	int nsid;
	struct ctrlr_entry *entry;
	struct spdk_nvme_ns *ns;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	printf("Attached to %s\n", trid->traddr);
	printf("\ttrstring: %s\n", trid->trstring);
	printf("\ttrtype: %d\n", trid->trtype);
	printf("\tadrfam: 0x%x\n", trid->adrfam);
	printf("\ttraddr: %s\n", trid->traddr);
	printf("\ttrsvcid: %s\n", trid->trsvcid);
	printf("\tsubnqn: %s\n", trid->subnqn);
	printf("\tpriority: %d\n", trid->priority);

	/*
	 * spdk_nvme_ctrlr is the logical abstraction in SPDK for an NVMe
	 *  controller.  During initialization, the IDENTIFY data for the
	 *  controller is read using an NVMe admin command, and that data
	 *  can be retrieved using spdk_nvme_ctrlr_get_data() to get
	 *  detailed information on the controller.  Refer to the NVMe
	 *  specification for more details on IDENTIFY for NVMe controllers.
	 */
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	// printf("%*.*s\n",m,n,ch); 前边的*定义的是总的宽度，后边的*定义的是输出的个数。分别对应外面的参数m和n
	snprintf(entry->name, sizeof(entry->name), "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);
	printf("entry->name: %s\n", entry->name);

	entry->ctrlr = ctrlr;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	/*
	 * Each controller has one or more namespaces.  An NVMe namespace is basically
	 *  equivalent to a SCSI LUN.  The controller's IDENTIFY data tells us how
	 *  many namespaces exist on the controller.  For Intel(R) P3X00 controllers,
	 *  it will just be one namespace.
	 *
	 * Note that in NVMe, namespace IDs start at 1, not 0.
	 */
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		// 获取命名空间
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
		if (ns == NULL) {
			continue;
		}
		register_ns(ctrlr, ns);
	}
}

static void
cleanup(void)
{
	struct ns_entry *ns_entry, *tmp_ns_entry;
	struct ctrlr_entry *ctrlr_entry, *tmp_ctrlr_entry;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(ns_entry, &g_namespaces, link, tmp_ns_entry) {
		TAILQ_REMOVE(&g_namespaces, ns_entry, link);
		free(ns_entry);
	}

	TAILQ_FOREACH_SAFE(ctrlr_entry, &g_controllers, link, tmp_ctrlr_entry) {
		TAILQ_REMOVE(&g_controllers, ctrlr_entry, link);
		// 分离nvme设备
		spdk_nvme_detach_async(ctrlr_entry->ctrlr, &detach_ctx);
		free(ctrlr_entry);
	}

	if (detach_ctx) {
		// 轮询设备分离完成
		spdk_nvme_detach_poll(detach_ctx);
	}
}

static void
usage(const char *program_name)
{
	printf("%s [options]", program_name);
	printf("\t\n");
	printf("options:\n");
	printf("\t[-d DPDK huge memory size in MB]\n");
	printf("\t[-g use single file descriptor for DPDK memory segments]\n");
	printf("\t[-i shared memory group ID]\n");
	printf("\t[-r remote NVMe over Fabrics target address]\n");
	printf("\t[-V enumerate VMD]\n");
#ifdef DEBUG
	printf("\t[-L enable debug logging]\n");
#else
	printf("\t[-L enable debug logging (flag disabled, must reconfigure with --enable-debug)\n");
#endif
}

static int
parse_args(int argc, char **argv, struct spdk_env_opts *env_opts)
{
	int op, rc;

	// 填充传输类型和字符串
	spdk_nvme_trid_populate_transport(&g_trid, SPDK_NVME_TRANSPORT_PCIE);
	snprintf(g_trid.subnqn, sizeof(g_trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

	while ((op = getopt(argc, argv, "d:gi:r:L:V")) != -1) {
		switch (op) {
		case 'V':
			g_vmd = true;
			break;
		case 'i':
			env_opts->shm_id = spdk_strtol(optarg, 10);
			if (env_opts->shm_id < 0) {
				fprintf(stderr, "Invalid shared memory ID\n");
				return env_opts->shm_id;
			}
			break;
		case 'g':
			env_opts->hugepage_single_segments = true;
			break;
		case 'r':
			if (spdk_nvme_transport_id_parse(&g_trid, optarg) != 0) {
				fprintf(stderr, "Error parsing transport address\n");
				return 1;
			}
			break;
		case 'd':
			env_opts->mem_size = spdk_strtol(optarg, 10);
			if (env_opts->mem_size < 0) {
				fprintf(stderr, "Invalid DPDK memory size\n");
				return env_opts->mem_size;
			}
			break;
		case 'L':
			rc = spdk_log_set_flag(optarg);
			if (rc < 0) {
				fprintf(stderr, "unknown flag\n");
				usage(argv[0]);
				exit(EXIT_FAILURE);
			}
#ifdef DEBUG
			spdk_log_set_print_level(SPDK_LOG_DEBUG);
#endif
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	spdk_log_set_print_level(2);
	spdk_log_set_level(2);
	printf("----g_spdk_log_print_level: [%d], g_spdk_log_level: [%d]\n", spdk_log_get_print_level(), spdk_log_get_level());
	int rc;
	struct spdk_env_opts opts;

	/*
	 * SPDK relies on an abstraction around the local environment
	 * named env that handles memory allocation and PCI device operations.
	 * This library must be initialized first.
	 * 1. 先初始化环境 struct spdk_env_opts opts;
	 *
	 */
	spdk_env_opts_init(&opts);
	rc = parse_args(argc, argv, &opts);
	if (rc != 0) {
		return rc;
	}

	opts.name = "hello_world";
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	printf("Initializing NVMe Controllers\n");

	if (g_vmd && spdk_vmd_init()) {
		fprintf(stderr, "Failed to initialize VMD."
			" Some NVMe devices can be unavailable.\n");
	}

	/*
	 * Start the SPDK NVMe enumeration process.  probe_cb will be called
	 *  for each NVMe controller found, giving our application a choice on
	 *  whether to attach to each controller.  attach_cb will then be
	 *  called for each controller after the SPDK NVMe driver has completed
	 *  initializing the controller we chose to attach.
	 * 2. 探测并连接设备
	 */
	rc = spdk_nvme_probe(&g_trid, NULL, probe_cb, attach_cb, NULL);
	if (rc != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed\n");
		rc = 1;
		goto exit;
	}

	if (TAILQ_EMPTY(&g_controllers)) {
		fprintf(stderr, "no NVMe controllers found\n");
		rc = 1;
		goto exit;
	}

	printf("Initialization complete.\n");
	// 3. 执行与设备相关的读写操作
	test_deallocate();
	hello_world();
	cleanup();
	if (g_vmd) {
		spdk_vmd_fini();
	}

exit:
	// 4. 释放attach设备时的相关资源
	cleanup();
	// 5. 释放环境资源
	// 释放环境库中spdk_env_init（）分配的任何资源。
	// 此调用后，不得进行 SPDK env 函数调用。
	// 预计此函数的常见用法是在终止进程之前或在同一进程中重新初始化环境库之前调用它。
	spdk_env_fini();
	return rc;
}
