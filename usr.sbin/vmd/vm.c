/*	$OpenBSD: vm.c,v 1.106 2024/09/26 01:45:13 jsg Exp $	*/

/*
 * Copyright (c) 2015 Mike Larkin <mlarkin@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>	/* PAGE_SIZE, MAXCOMLEN */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <dev/vmm/vmm.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <poll.h>
#include <pthread.h>
#include <pthread_np.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>

#include "atomicio.h"
#include "pci.h"
#include "virtio.h"
#include "vmd.h"

#define MMIO_NOTYET 0

static int run_vm(struct vmop_create_params *, struct vcpu_reg_state *);
static void vm_dispatch_vmm(int, short, void *);
static void *event_thread(void *);
static void *vcpu_run_loop(void *);
static int vmm_create_vm(struct vmd_vm *);
static int alloc_guest_mem(struct vmd_vm *);
static int send_vm(int, struct vmd_vm *);
static int dump_vmr(int , struct vm_mem_range *);
static int dump_mem(int, struct vmd_vm *);
static void restore_vmr(int, struct vm_mem_range *);
static void restore_mem(int, struct vm_create_params *);
static int restore_vm_params(int, struct vm_create_params *);
static void pause_vm(struct vmd_vm *);
static void unpause_vm(struct vmd_vm *);
static int start_vm(struct vmd_vm *, int);

int con_fd;
struct vmd_vm *current_vm;

extern struct vmd *env;

extern char *__progname;

pthread_mutex_t threadmutex;
pthread_cond_t threadcond;

pthread_cond_t vcpu_run_cond[VMM_MAX_VCPUS_PER_VM];
pthread_mutex_t vcpu_run_mtx[VMM_MAX_VCPUS_PER_VM];
pthread_barrier_t vm_pause_barrier;
pthread_cond_t vcpu_unpause_cond[VMM_MAX_VCPUS_PER_VM];
pthread_mutex_t vcpu_unpause_mtx[VMM_MAX_VCPUS_PER_VM];

pthread_mutex_t vm_mtx;
uint8_t vcpu_hlt[VMM_MAX_VCPUS_PER_VM];
uint8_t vcpu_done[VMM_MAX_VCPUS_PER_VM];

/*
 * vm_main
 *
 * Primary entrypoint for launching a vm. Does not return.
 *
 * fd: file descriptor for communicating with vmm process.
 * fd_vmm: file descriptor for communicating with vmm(4) device
 */
void
vm_main(int fd, int fd_vmm)
{
	struct vm_create_params	*vcp = NULL;
	struct vmd_vm		 vm;
	size_t			 sz = 0;
	int			 ret = 0;

	/*
	 * The vm process relies on global state. Set the fd for /dev/vmm.
	 */
	env->vmd_fd = fd_vmm;

	/*
	 * We aren't root, so we can't chroot(2). Use unveil(2) instead.
	 */
	if (unveil(env->argv0, "x") == -1)
		fatal("unveil %s", env->argv0);
	if (unveil(NULL, NULL) == -1)
		fatal("unveil lock");

	/*
	 * pledge in the vm processes:
	 * stdio - for malloc and basic I/O including events.
	 * vmm - for the vmm ioctls and operations.
	 * proc exec - fork/exec for launching devices.
	 * recvfd - for vm send/recv and sending fd to devices.
	 */
	if (pledge("stdio vmm proc exec recvfd", NULL) == -1)
		fatal("pledge");

	/* Receive our vm configuration. */
	memset(&vm, 0, sizeof(vm));
	sz = atomicio(read, fd, &vm, sizeof(vm));
	if (sz != sizeof(vm)) {
		log_warnx("failed to receive start message");
		_exit(EIO);
	}

	/* Update process with the vm name. */
	vcp = &vm.vm_params.vmc_params;
	setproctitle("%s", vcp->vcp_name);
	log_procinit("vm/%s", vcp->vcp_name);

	/* Receive the local prefix settings. */
	sz = atomicio(read, fd, &env->vmd_cfg.cfg_localprefix,
	    sizeof(env->vmd_cfg.cfg_localprefix));
	if (sz != sizeof(env->vmd_cfg.cfg_localprefix)) {
		log_warnx("failed to receive local prefix");
		_exit(EIO);
	}

	/*
	 * We need, at minimum, a vm_kernel fd to boot a vm. This is either a
	 * kernel or a BIOS image.
	 */
	if (!(vm.vm_state & VM_STATE_RECEIVED)) {
		if (vm.vm_kernel == -1) {
			log_warnx("%s: failed to receive boot fd",
			    vcp->vcp_name);
			_exit(EINVAL);
		}
	}

	if (vcp->vcp_sev && env->vmd_psp_fd < 0) {
		log_warnx("%s not available", PSP_NODE);
		_exit(EINVAL);
	}

	ret = start_vm(&vm, fd);
	_exit(ret);
}

/*
 * start_vm
 *
 * After forking a new VM process, starts the new VM with the creation
 * parameters supplied (in the incoming vm->vm_params field). This
 * function performs a basic sanity check on the incoming parameters
 * and then performs the following steps to complete the creation of the VM:
 *
 * 1. validates and create the new VM
 * 2. opens the imsg control channel to the parent and drops more privilege
 * 3. drops additional privileges by calling pledge(2)
 * 4. loads the kernel from the disk image or file descriptor
 * 5. runs the VM's VCPU loops.
 *
 * Parameters:
 *  vm: The VM data structure that is including the VM create parameters.
 *  fd: The imsg socket that is connected to the parent process.
 *
 * Return values:
 *  0: success
 *  !0 : failure - typically an errno indicating the source of the failure
 */
int
start_vm(struct vmd_vm *vm, int fd)
{
	struct vmop_create_params *vmc = &vm->vm_params;
	struct vm_create_params	*vcp = &vmc->vmc_params;
	struct vcpu_reg_state	 vrs;
	int			 nicfds[VM_MAX_NICS_PER_VM];
	int			 ret;
	size_t			 i;
	struct vm_rwregs_params  vrp;

	/*
	 * We first try to initialize and allocate memory before bothering
	 * vmm(4) with a request to create a new vm.
	 */
	if (!(vm->vm_state & VM_STATE_RECEIVED))
		create_memory_map(vcp);

	ret = alloc_guest_mem(vm);
	if (ret) {
		struct rlimit lim;
		char buf[FMT_SCALED_STRSIZE];
		if (ret == ENOMEM && getrlimit(RLIMIT_DATA, &lim) == 0) {
			if (fmt_scaled(lim.rlim_cur, buf) == 0)
				fatalx("could not allocate guest memory (data "
				    "limit is %s)", buf);
		}
		errno = ret;
		log_warn("could not allocate guest memory");
		return (ret);
	}

	/* We've allocated guest memory, so now create the vm in vmm(4). */
	ret = vmm_create_vm(vm);
	if (ret) {
		/* Let the vmm process know we failed by sending a 0 vm id. */
		vcp->vcp_id = 0;
		atomicio(vwrite, fd, &vcp->vcp_id, sizeof(vcp->vcp_id));
		return (ret);
	}

	/* Setup SEV. */
	ret = sev_init(vm);
	if (ret) {
		log_warnx("could not initialize SEV");
		return (ret);
	}

	/*
	 * Some of vmd currently relies on global state (current_vm, con_fd).
	 */
	current_vm = vm;
	con_fd = vm->vm_tty;
	if (fcntl(con_fd, F_SETFL, O_NONBLOCK) == -1) {
		log_warn("failed to set nonblocking mode on console");
		return (1);
	}

	/*
	 * We now let the vmm process know we were successful by sending it our
	 * vmm(4) assigned vm id.
	 */
	if (atomicio(vwrite, fd, &vcp->vcp_id, sizeof(vcp->vcp_id)) !=
	    sizeof(vcp->vcp_id)) {
		log_warn("failed to send created vm id to vmm process");
		return (1);
	}

	/* Prepare either our boot image or receive an existing vm to launch. */
	if (vm->vm_state & VM_STATE_RECEIVED) {
		ret = atomicio(read, vm->vm_receive_fd, &vrp, sizeof(vrp));
		if (ret != sizeof(vrp))
			fatal("received incomplete vrp - exiting");
		vrs = vrp.vrwp_regs;
	} else if (load_firmware(vm, &vrs))
		fatalx("failed to load kernel or firmware image");

	if (vm->vm_kernel != -1)
		close_fd(vm->vm_kernel);

	/* Initialize our mutexes. */
	ret = pthread_mutex_init(&threadmutex, NULL);
	if (ret) {
		log_warn("%s: could not initialize thread state mutex",
		    __func__);
		return (ret);
	}
	ret = pthread_cond_init(&threadcond, NULL);
	if (ret) {
		log_warn("%s: could not initialize thread state "
		    "condition variable", __func__);
		return (ret);
	}
	ret = pthread_mutex_init(&vm_mtx, NULL);
	if (ret) {
		log_warn("%s: could not initialize vm state mutex",
		    __func__);
		return (ret);
	}

	/* Lock thread mutex now. It's unlocked when waiting on threadcond. */
	mutex_lock(&threadmutex);

	/*
	 * Finalize our communication socket with the vmm process. From here
	 * onwards, communication with the vmm process is event-based.
	 */
	event_init();
	if (vmm_pipe(vm, fd, vm_dispatch_vmm) == -1)
		fatal("setup vm pipe");

	/*
	 * Initialize or restore our emulated hardware.
	 */
	for (i = 0; i < VMM_MAX_NICS_PER_VM; i++)
		nicfds[i] = vm->vm_ifs[i].vif_fd;

	if (vm->vm_state & VM_STATE_RECEIVED) {
		restore_mem(vm->vm_receive_fd, vcp);
		restore_emulated_hw(vcp, vm->vm_receive_fd, nicfds,
		    vm->vm_disks, vm->vm_cdrom);
		if (restore_vm_params(vm->vm_receive_fd, vcp))
			fatal("restore vm params failed");
		unpause_vm(vm);
	} else
		init_emulated_hw(vmc, vm->vm_cdrom, vm->vm_disks, nicfds);

	/* Drop privleges further before starting the vcpu run loop(s). */
	if (pledge("stdio vmm recvfd", NULL) == -1)
		fatal("pledge");

	/*
	 * Execute the vcpu run loop(s) for this VM.
	 */
	ret = run_vm(&vm->vm_params, &vrs);

	/* Shutdown SEV. */
	if (sev_shutdown(vm))
		log_warnx("%s: could not shutdown SEV", __func__);

	/* Ensure that any in-flight data is written back */
	virtio_shutdown(vm);

	return (ret);
}

/*
 * vm_dispatch_vmm
 *
 * imsg callback for messages that are received from the vmm parent process.
 */
void
vm_dispatch_vmm(int fd, short event, void *arg)
{
	struct vmd_vm		*vm = arg;
	struct vmop_result	 vmr;
	struct vmop_addr_result	 var;
	struct imsgev		*iev = &vm->vm_iev;
	struct imsgbuf		*ibuf = &iev->ibuf;
	struct imsg		 imsg;
	ssize_t			 n;
	int			 verbose;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("%s: imsg_read", __func__);
		if (n == 0)
			_exit(0);
	}

	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("%s: msgbuf_write fd %d", __func__, ibuf->fd);
		if (n == 0)
			_exit(0);
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("%s: imsg_get", __func__);
		if (n == 0)
			break;

#if DEBUG > 1
		log_debug("%s: got imsg %d from %s",
		    __func__, imsg.hdr.type,
		    vm->vm_params.vmc_params.vcp_name);
#endif

		switch (imsg.hdr.type) {
		case IMSG_CTL_VERBOSE:
			IMSG_SIZE_CHECK(&imsg, &verbose);
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_setverbose(verbose);
			virtio_broadcast_imsg(vm, IMSG_CTL_VERBOSE, &verbose,
			    sizeof(verbose));
			break;
		case IMSG_VMDOP_VM_SHUTDOWN:
			if (vmmci_ctl(VMMCI_SHUTDOWN) == -1)
				_exit(0);
			break;
		case IMSG_VMDOP_VM_REBOOT:
			if (vmmci_ctl(VMMCI_REBOOT) == -1)
				_exit(0);
			break;
		case IMSG_VMDOP_PAUSE_VM:
			vmr.vmr_result = 0;
			vmr.vmr_id = vm->vm_vmid;
			pause_vm(vm);
			imsg_compose_event(&vm->vm_iev,
			    IMSG_VMDOP_PAUSE_VM_RESPONSE,
			    imsg.hdr.peerid, imsg.hdr.pid, -1, &vmr,
			    sizeof(vmr));
			break;
		case IMSG_VMDOP_UNPAUSE_VM:
			vmr.vmr_result = 0;
			vmr.vmr_id = vm->vm_vmid;
			unpause_vm(vm);
			imsg_compose_event(&vm->vm_iev,
			    IMSG_VMDOP_UNPAUSE_VM_RESPONSE,
			    imsg.hdr.peerid, imsg.hdr.pid, -1, &vmr,
			    sizeof(vmr));
			break;
		case IMSG_VMDOP_SEND_VM_REQUEST:
			vmr.vmr_id = vm->vm_vmid;
			vmr.vmr_result = send_vm(imsg_get_fd(&imsg), vm);
			imsg_compose_event(&vm->vm_iev,
			    IMSG_VMDOP_SEND_VM_RESPONSE,
			    imsg.hdr.peerid, imsg.hdr.pid, -1, &vmr,
			    sizeof(vmr));
			if (!vmr.vmr_result) {
				imsg_flush(&current_vm->vm_iev.ibuf);
				_exit(0);
			}
			break;
		case IMSG_VMDOP_PRIV_GET_ADDR_RESPONSE:
			IMSG_SIZE_CHECK(&imsg, &var);
			memcpy(&var, imsg.data, sizeof(var));

			log_debug("%s: received tap addr %s for nic %d",
			    vm->vm_params.vmc_params.vcp_name,
			    ether_ntoa((void *)var.var_addr), var.var_nic_idx);

			vionet_set_hostmac(vm, var.var_nic_idx, var.var_addr);
			break;
		default:
			fatalx("%s: got invalid imsg %d from %s",
			    __func__, imsg.hdr.type,
			    vm->vm_params.vmc_params.vcp_name);
		}
		imsg_free(&imsg);
	}
	imsg_event_add(iev);
}

/*
 * vm_shutdown
 *
 * Tell the vmm parent process to shutdown or reboot the VM and exit.
 */
__dead void
vm_shutdown(unsigned int cmd)
{
	switch (cmd) {
	case VMMCI_NONE:
	case VMMCI_SHUTDOWN:
		(void)imsg_compose_event(&current_vm->vm_iev,
		    IMSG_VMDOP_VM_SHUTDOWN, 0, 0, -1, NULL, 0);
		break;
	case VMMCI_REBOOT:
		(void)imsg_compose_event(&current_vm->vm_iev,
		    IMSG_VMDOP_VM_REBOOT, 0, 0, -1, NULL, 0);
		break;
	default:
		fatalx("invalid vm ctl command: %d", cmd);
	}
	imsg_flush(&current_vm->vm_iev.ibuf);

	if (sev_shutdown(current_vm))
		log_warnx("%s: could not shutdown SEV", __func__);

	_exit(0);
}

int
send_vm(int fd, struct vmd_vm *vm)
{
	struct vm_rwregs_params	   vrp;
	struct vm_rwvmparams_params vpp;
	struct vmop_create_params *vmc;
	struct vm_terminate_params vtp;
	unsigned int		   flags = 0;
	unsigned int		   i;
	int			   ret = 0;
	size_t			   sz;

	if (dump_send_header(fd)) {
		log_warnx("%s: failed to send vm dump header", __func__);
		goto err;
	}

	pause_vm(vm);

	vmc = calloc(1, sizeof(struct vmop_create_params));
	if (vmc == NULL) {
		log_warn("%s: calloc error getting vmc", __func__);
		ret = -1;
		goto err;
	}

	flags |= VMOP_CREATE_MEMORY;
	memcpy(&vmc->vmc_params, &current_vm->vm_params, sizeof(struct
	    vmop_create_params));
	vmc->vmc_flags = flags;
	vrp.vrwp_vm_id = vm->vm_params.vmc_params.vcp_id;
	vrp.vrwp_mask = VM_RWREGS_ALL;
	vpp.vpp_mask = VM_RWVMPARAMS_ALL;
	vpp.vpp_vm_id = vm->vm_params.vmc_params.vcp_id;

	sz = atomicio(vwrite, fd, vmc, sizeof(struct vmop_create_params));
	if (sz != sizeof(struct vmop_create_params)) {
		ret = -1;
		goto err;
	}

	for (i = 0; i < vm->vm_params.vmc_params.vcp_ncpus; i++) {
		vrp.vrwp_vcpu_id = i;
		if ((ret = ioctl(env->vmd_fd, VMM_IOC_READREGS, &vrp))) {
			log_warn("%s: readregs failed", __func__);
			goto err;
		}

		sz = atomicio(vwrite, fd, &vrp,
		    sizeof(struct vm_rwregs_params));
		if (sz != sizeof(struct vm_rwregs_params)) {
			log_warn("%s: dumping registers failed", __func__);
			ret = -1;
			goto err;
		}
	}

	/* Dump memory before devices to aid in restoration. */
	if ((ret = dump_mem(fd, vm)))
		goto err;
	if ((ret = dump_devs(fd)))
		goto err;
	if ((ret = pci_dump(fd)))
		goto err;
	if ((ret = virtio_dump(fd)))
		goto err;

	for (i = 0; i < vm->vm_params.vmc_params.vcp_ncpus; i++) {
		vpp.vpp_vcpu_id = i;
		if ((ret = ioctl(env->vmd_fd, VMM_IOC_READVMPARAMS, &vpp))) {
			log_warn("%s: readvmparams failed", __func__);
			goto err;
		}

		sz = atomicio(vwrite, fd, &vpp,
		    sizeof(struct vm_rwvmparams_params));
		if (sz != sizeof(struct vm_rwvmparams_params)) {
			log_warn("%s: dumping vm params failed", __func__);
			ret = -1;
			goto err;
		}
	}

	vtp.vtp_vm_id = vm->vm_params.vmc_params.vcp_id;
	if (ioctl(env->vmd_fd, VMM_IOC_TERM, &vtp) == -1) {
		log_warnx("%s: term IOC error: %d, %d", __func__,
		    errno, ENOENT);
	}
err:
	close(fd);
	if (ret)
		unpause_vm(vm);
	return ret;
}

int
dump_mem(int fd, struct vmd_vm *vm)
{
	unsigned int	i;
	int		ret;
	struct		vm_mem_range *vmr;

	for (i = 0; i < vm->vm_params.vmc_params.vcp_nmemranges; i++) {
		vmr = &vm->vm_params.vmc_params.vcp_memranges[i];
		ret = dump_vmr(fd, vmr);
		if (ret)
			return ret;
	}
	return (0);
}

int
restore_vm_params(int fd, struct vm_create_params *vcp) {
	unsigned int			i;
	struct vm_rwvmparams_params    vpp;

	for (i = 0; i < vcp->vcp_ncpus; i++) {
		if (atomicio(read, fd, &vpp, sizeof(vpp)) != sizeof(vpp)) {
			log_warn("%s: error restoring vm params", __func__);
			return (-1);
		}
		vpp.vpp_vm_id = vcp->vcp_id;
		vpp.vpp_vcpu_id = i;
		if (ioctl(env->vmd_fd, VMM_IOC_WRITEVMPARAMS, &vpp) < 0) {
			log_debug("%s: writing vm params failed", __func__);
			return (-1);
		}
	}
	return (0);
}

void
restore_mem(int fd, struct vm_create_params *vcp)
{
	unsigned int	     i;
	struct vm_mem_range *vmr;

	for (i = 0; i < vcp->vcp_nmemranges; i++) {
		vmr = &vcp->vcp_memranges[i];
		restore_vmr(fd, vmr);
	}
}

int
dump_vmr(int fd, struct vm_mem_range *vmr)
{
	size_t	rem = vmr->vmr_size, read=0;
	char	buf[PAGE_SIZE];

	while (rem > 0) {
		if (read_mem(vmr->vmr_gpa + read, buf, PAGE_SIZE)) {
			log_warn("failed to read vmr");
			return (-1);
		}
		if (atomicio(vwrite, fd, buf, sizeof(buf)) != sizeof(buf)) {
			log_warn("failed to dump vmr");
			return (-1);
		}
		rem = rem - PAGE_SIZE;
		read = read + PAGE_SIZE;
	}
	return (0);
}

void
restore_vmr(int fd, struct vm_mem_range *vmr)
{
	size_t	rem = vmr->vmr_size, wrote=0;
	char	buf[PAGE_SIZE];

	while (rem > 0) {
		if (atomicio(read, fd, buf, sizeof(buf)) != sizeof(buf))
			fatal("failed to restore vmr");
		if (write_mem(vmr->vmr_gpa + wrote, buf, PAGE_SIZE))
			fatal("failed to write vmr");
		rem = rem - PAGE_SIZE;
		wrote = wrote + PAGE_SIZE;
	}
}

static void
pause_vm(struct vmd_vm *vm)
{
	unsigned int n;
	int ret;

	mutex_lock(&vm_mtx);
	if (vm->vm_state & VM_STATE_PAUSED) {
		mutex_unlock(&vm_mtx);
		return;
	}
	current_vm->vm_state |= VM_STATE_PAUSED;
	mutex_unlock(&vm_mtx);

	ret = pthread_barrier_init(&vm_pause_barrier, NULL,
	    vm->vm_params.vmc_params.vcp_ncpus + 1);
	if (ret) {
		log_warnx("%s: cannot initialize pause barrier (%d)",
		    __progname, ret);
		return;
	}

	for (n = 0; n < vm->vm_params.vmc_params.vcp_ncpus; n++) {
		ret = pthread_cond_broadcast(&vcpu_run_cond[n]);
		if (ret) {
			log_warnx("%s: can't broadcast vcpu run cond (%d)",
			    __func__, (int)ret);
			return;
		}
	}
	ret = pthread_barrier_wait(&vm_pause_barrier);
	if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD) {
		log_warnx("%s: could not wait on pause barrier (%d)",
		    __func__, (int)ret);
		return;
	}

	ret = pthread_barrier_destroy(&vm_pause_barrier);
	if (ret) {
		log_warnx("%s: could not destroy pause barrier (%d)",
		    __progname, ret);
		return;
	}

	pause_vm_md(vm);
}

static void
unpause_vm(struct vmd_vm *vm)
{
	unsigned int n;
	int ret;

	mutex_lock(&vm_mtx);
	if (!(vm->vm_state & VM_STATE_PAUSED)) {
		mutex_unlock(&vm_mtx);
		return;
	}
	current_vm->vm_state &= ~VM_STATE_PAUSED;
	mutex_unlock(&vm_mtx);

	for (n = 0; n < vm->vm_params.vmc_params.vcp_ncpus; n++) {
		ret = pthread_cond_broadcast(&vcpu_unpause_cond[n]);
		if (ret) {
			log_warnx("%s: can't broadcast vcpu unpause cond (%d)",
			    __func__, (int)ret);
			return;
		}
	}

	unpause_vm_md(vm);
}

/*
 * vcpu_reset
 *
 * Requests vmm(4) to reset the VCPUs in the indicated VM to
 * the register state provided
 *
 * Parameters
 *  vmid: VM ID to reset
 *  vcpu_id: VCPU ID to reset
 *  vrs: the register state to initialize
 *
 * Return values:
 *  0: success
 *  !0 : ioctl to vmm(4) failed (eg, ENOENT if the supplied VM ID is not
 *      valid)
 */
int
vcpu_reset(uint32_t vmid, uint32_t vcpu_id, struct vcpu_reg_state *vrs)
{
	struct vm_resetcpu_params vrp;

	memset(&vrp, 0, sizeof(vrp));
	vrp.vrp_vm_id = vmid;
	vrp.vrp_vcpu_id = vcpu_id;
	memcpy(&vrp.vrp_init_state, vrs, sizeof(struct vcpu_reg_state));

	log_debug("%s: resetting vcpu %d for vm %d", __func__, vcpu_id, vmid);

	if (ioctl(env->vmd_fd, VMM_IOC_RESETCPU, &vrp) == -1)
		return (errno);

	return (0);
}

/*
 * alloc_guest_mem
 *
 * Allocates memory for the guest.
 * Instead of doing a single allocation with one mmap(), we allocate memory
 * separately for every range for the following reasons:
 * - ASLR for the individual ranges
 * - to reduce memory consumption in the UVM subsystem: if vmm(4) had to
 *   map the single mmap'd userspace memory to the individual guest physical
 *   memory ranges, the underlying amap of the single mmap'd range would have
 *   to allocate per-page reference counters. The reason is that the
 *   individual guest physical ranges would reference the single mmap'd region
 *   only partially. However, if every guest physical range has its own
 *   corresponding mmap'd userspace allocation, there are no partial
 *   references: every guest physical range fully references an mmap'd
 *   range => no per-page reference counters have to be allocated.
 *
 * Return values:
 *  0: success
 *  !0: failure - errno indicating the source of the failure
 */
int
alloc_guest_mem(struct vmd_vm *vm)
{
	void *p;
	int ret = 0;
	size_t i, j;
	struct vm_create_params *vcp = &vm->vm_params.vmc_params;
	struct vm_mem_range *vmr;

	for (i = 0; i < vcp->vcp_nmemranges; i++) {
		vmr = &vcp->vcp_memranges[i];

		/*
		 * We only need R/W as userland. vmm(4) will use R/W/X in its
		 * mapping.
		 *
		 * We must use MAP_SHARED so emulated devices will be able
		 * to generate shared mappings.
		 */
		p = mmap(NULL, vmr->vmr_size, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_CONCEAL | MAP_SHARED, -1, 0);
		if (p == MAP_FAILED) {
			ret = errno;
			for (j = 0; j < i; j++) {
				vmr = &vcp->vcp_memranges[j];
				munmap((void *)vmr->vmr_va, vmr->vmr_size);
			}
			return (ret);
		}
		vmr->vmr_va = (vaddr_t)p;
	}

	return (ret);
}

/*
 * vmm_create_vm
 *
 * Requests vmm(4) to create a new VM using the supplied creation
 * parameters. This operation results in the creation of the in-kernel
 * structures for the VM, but does not start the VM's vcpu(s).
 *
 * Parameters:
 *  vm: pointer to the vm object
 *
 * Return values:
 *  0: success
 *  !0 : ioctl to vmm(4) failed
 */
static int
vmm_create_vm(struct vmd_vm *vm)
{
	struct vm_create_params *vcp = &vm->vm_params.vmc_params;
	size_t i;

	/* Sanity check arguments */
	if (vcp->vcp_ncpus > VMM_MAX_VCPUS_PER_VM)
		return (EINVAL);

	if (vcp->vcp_nmemranges == 0 ||
	    vcp->vcp_nmemranges > VMM_MAX_MEM_RANGES)
		return (EINVAL);

	if (vm->vm_params.vmc_ndisks > VM_MAX_DISKS_PER_VM)
		return (EINVAL);

	if (vm->vm_params.vmc_nnics > VM_MAX_NICS_PER_VM)
		return (EINVAL);

	if (ioctl(env->vmd_fd, VMM_IOC_CREATE, vcp) == -1)
		return (errno);

	for (i = 0; i < vcp->vcp_ncpus; i++)
		vm->vm_sev_asid[i] = vcp->vcp_asid[i];

	return (0);
}


	/*
 * run_vm
 *
 * Runs the VM whose creation parameters are specified in vcp
 *
 * Parameters:
 *  child_cdrom: previously-opened child ISO disk file descriptor
 *  child_disks: previously-opened child VM disk file file descriptors
 *  child_taps: previously-opened child tap file descriptors
 *  vmc: vmop_create_params struct containing the VM's desired creation
 *      configuration
 *  vrs: VCPU register state to initialize
 *
 * Return values:
 *  0: the VM exited normally
 *  !0 : the VM exited abnormally or failed to start
 */
static int
run_vm(struct vmop_create_params *vmc, struct vcpu_reg_state *vrs)
{
	struct vm_create_params *vcp = &vmc->vmc_params;
	struct vm_rwregs_params vregsp;
	uint8_t evdone = 0;
	size_t i;
	int ret;
	pthread_t *tid, evtid;
	char tname[MAXCOMLEN + 1];
	struct vm_run_params **vrp;
	void *exit_status;

	if (vcp == NULL)
		return (EINVAL);

	if (vcp->vcp_nmemranges == 0 ||
	    vcp->vcp_nmemranges > VMM_MAX_MEM_RANGES)
		return (EINVAL);

	tid = calloc(vcp->vcp_ncpus, sizeof(pthread_t));
	vrp = calloc(vcp->vcp_ncpus, sizeof(struct vm_run_params *));
	if (tid == NULL || vrp == NULL) {
		log_warn("%s: memory allocation error - exiting.",
		    __progname);
		return (ENOMEM);
	}

	log_debug("%s: starting %zu vcpu thread(s) for vm %s", __func__,
	    vcp->vcp_ncpus, vcp->vcp_name);

	/*
	 * Create and launch one thread for each VCPU. These threads may
	 * migrate between PCPUs over time; the need to reload CPU state
	 * in such situations is detected and performed by vmm(4) in the
	 * kernel.
	 */
	for (i = 0 ; i < vcp->vcp_ncpus; i++) {
		vrp[i] = malloc(sizeof(struct vm_run_params));
		if (vrp[i] == NULL) {
			log_warn("%s: memory allocation error - "
			    "exiting.", __progname);
			/* caller will exit, so skip freeing */
			return (ENOMEM);
		}
		vrp[i]->vrp_exit = malloc(sizeof(struct vm_exit));
		if (vrp[i]->vrp_exit == NULL) {
			log_warn("%s: memory allocation error - "
			    "exiting.", __progname);
			/* caller will exit, so skip freeing */
			return (ENOMEM);
		}
		vrp[i]->vrp_vm_id = vcp->vcp_id;
		vrp[i]->vrp_vcpu_id = i;

		if (vcpu_reset(vcp->vcp_id, i, vrs)) {
			log_warnx("%s: cannot reset VCPU %zu - exiting.",
			    __progname, i);
			return (EIO);
		}

		if (sev_activate(current_vm, i)) {
			log_warnx("%s: SEV activatation failed for VCPU "
			    "%zu failed - exiting.", __progname, i);
			return (EIO);
		}

		if (sev_encrypt_memory(current_vm)) {
			log_warnx("%s: memory encryption failed for VCPU "
			    "%zu failed - exiting.", __progname, i);
			return (EIO);
		}

		/* once more because reset_cpu changes regs */
		if (current_vm->vm_state & VM_STATE_RECEIVED) {
			vregsp.vrwp_vm_id = vcp->vcp_id;
			vregsp.vrwp_vcpu_id = i;
			vregsp.vrwp_regs = *vrs;
			vregsp.vrwp_mask = VM_RWREGS_ALL;
			if ((ret = ioctl(env->vmd_fd, VMM_IOC_WRITEREGS,
			    &vregsp)) == -1) {
				log_warn("%s: writeregs failed", __func__);
				return (ret);
			}
		}

		ret = pthread_cond_init(&vcpu_run_cond[i], NULL);
		if (ret) {
			log_warnx("%s: cannot initialize cond var (%d)",
			    __progname, ret);
			return (ret);
		}

		ret = pthread_mutex_init(&vcpu_run_mtx[i], NULL);
		if (ret) {
			log_warnx("%s: cannot initialize mtx (%d)",
			    __progname, ret);
			return (ret);
		}

		ret = pthread_cond_init(&vcpu_unpause_cond[i], NULL);
		if (ret) {
			log_warnx("%s: cannot initialize unpause var (%d)",
			    __progname, ret);
			return (ret);
		}

		ret = pthread_mutex_init(&vcpu_unpause_mtx[i], NULL);
		if (ret) {
			log_warnx("%s: cannot initialize unpause mtx (%d)",
			    __progname, ret);
			return (ret);
		}

		vcpu_hlt[i] = 0;

		/* Start each VCPU run thread at vcpu_run_loop */
		ret = pthread_create(&tid[i], NULL, vcpu_run_loop, vrp[i]);
		if (ret) {
			/* caller will _exit after this return */
			ret = errno;
			log_warn("%s: could not create vcpu thread %zu",
			    __func__, i);
			return (ret);
		}

		snprintf(tname, sizeof(tname), "vcpu-%zu", i);
		pthread_set_name_np(tid[i], tname);
	}

	log_debug("%s: waiting on events for VM %s", __func__, vcp->vcp_name);
	ret = pthread_create(&evtid, NULL, event_thread, &evdone);
	if (ret) {
		errno = ret;
		log_warn("%s: could not create event thread", __func__);
		return (ret);
	}
	pthread_set_name_np(evtid, "event");

	for (;;) {
		ret = pthread_cond_wait(&threadcond, &threadmutex);
		if (ret) {
			log_warn("%s: waiting on thread state condition "
			    "variable failed", __func__);
			return (ret);
		}

		/*
		 * Did a VCPU thread exit with an error? => return the first one
		 */
		mutex_lock(&vm_mtx);
		for (i = 0; i < vcp->vcp_ncpus; i++) {
			if (vcpu_done[i] == 0)
				continue;

			if (pthread_join(tid[i], &exit_status)) {
				log_warn("%s: failed to join thread %zd - "
				    "exiting", __progname, i);
				mutex_unlock(&vm_mtx);
				return (EIO);
			}

			ret = (intptr_t)exit_status;
		}
		mutex_unlock(&vm_mtx);

		/* Did the event thread exit? => return with an error */
		if (evdone) {
			if (pthread_join(evtid, &exit_status)) {
				log_warn("%s: failed to join event thread - "
				    "exiting", __progname);
				return (EIO);
			}

			log_warnx("%s: vm %d event thread exited "
			    "unexpectedly", __progname, vcp->vcp_id);
			return (EIO);
		}

		/* Did all VCPU threads exit successfully? => return */
		mutex_lock(&vm_mtx);
		for (i = 0; i < vcp->vcp_ncpus; i++) {
			if (vcpu_done[i] == 0)
				break;
		}
		mutex_unlock(&vm_mtx);
		if (i == vcp->vcp_ncpus)
			return (ret);

		/* Some more threads to wait for, start over */
	}

	return (ret);
}

static void *
event_thread(void *arg)
{
	uint8_t *donep = arg;
	intptr_t ret;

	ret = event_dispatch();

	*donep = 1;

	mutex_lock(&threadmutex);
	pthread_cond_signal(&threadcond);
	mutex_unlock(&threadmutex);

	return (void *)ret;
 }

/*
 * vcpu_run_loop
 *
 * Runs a single VCPU until vmm(4) requires help handling an exit,
 * or the VM terminates.
 *
 * Parameters:
 *  arg: vcpu_run_params for the VCPU being run by this thread
 *
 * Return values:
 *  NULL: the VCPU shutdown properly
 *  !NULL: error processing VCPU run, or the VCPU shutdown abnormally
 */
static void *
vcpu_run_loop(void *arg)
{
	struct vm_run_params *vrp = (struct vm_run_params *)arg;
	intptr_t ret = 0;
	uint32_t n = vrp->vrp_vcpu_id;
	int paused = 0, halted = 0;

	for (;;) {
		ret = pthread_mutex_lock(&vcpu_run_mtx[n]);

		if (ret) {
			log_warnx("%s: can't lock vcpu run mtx (%d)",
			    __func__, (int)ret);
			return ((void *)ret);
		}

		mutex_lock(&vm_mtx);
		paused = (current_vm->vm_state & VM_STATE_PAUSED) != 0;
		halted = vcpu_hlt[n];
		mutex_unlock(&vm_mtx);

		/* If we are halted and need to pause, pause */
		if (halted && paused) {
			ret = pthread_barrier_wait(&vm_pause_barrier);
			if (ret != 0 && ret != PTHREAD_BARRIER_SERIAL_THREAD) {
				log_warnx("%s: could not wait on pause barrier (%d)",
				    __func__, (int)ret);
				return ((void *)ret);
			}

			ret = pthread_mutex_lock(&vcpu_unpause_mtx[n]);
			if (ret) {
				log_warnx("%s: can't lock vcpu unpause mtx (%d)",
				    __func__, (int)ret);
				return ((void *)ret);
			}

			/* Interrupt may be firing, release run mtx. */
			mutex_unlock(&vcpu_run_mtx[n]);
			ret = pthread_cond_wait(&vcpu_unpause_cond[n],
			    &vcpu_unpause_mtx[n]);
			if (ret) {
				log_warnx(
				    "%s: can't wait on unpause cond (%d)",
				    __func__, (int)ret);
				break;
			}
			mutex_lock(&vcpu_run_mtx[n]);

			ret = pthread_mutex_unlock(&vcpu_unpause_mtx[n]);
			if (ret) {
				log_warnx("%s: can't unlock unpause mtx (%d)",
				    __func__, (int)ret);
				break;
			}
		}

		/* If we are halted and not paused, wait */
		if (halted) {
			ret = pthread_cond_wait(&vcpu_run_cond[n],
			    &vcpu_run_mtx[n]);

			if (ret) {
				log_warnx(
				    "%s: can't wait on cond (%d)",
				    __func__, (int)ret);
				(void)pthread_mutex_unlock(
				    &vcpu_run_mtx[n]);
				break;
			}
		}

		ret = pthread_mutex_unlock(&vcpu_run_mtx[n]);

		if (ret) {
			log_warnx("%s: can't unlock mutex on cond (%d)",
			    __func__, (int)ret);
			break;
		}

		if (vrp->vrp_irqready && intr_pending(current_vm)) {
			vrp->vrp_inject.vie_vector = intr_ack(current_vm);
			vrp->vrp_inject.vie_type = VCPU_INJECT_INTR;
		} else
			vrp->vrp_inject.vie_type = VCPU_INJECT_NONE;

		/* Still more interrupts pending? */
		vrp->vrp_intr_pending = intr_pending(current_vm);

		if (ioctl(env->vmd_fd, VMM_IOC_RUN, vrp) == -1) {
			/* If run ioctl failed, exit */
			ret = errno;
			log_warn("%s: vm %d / vcpu %d run ioctl failed",
			    __func__, current_vm->vm_vmid, n);
			break;
		}

		/* If the VM is terminating, exit normally */
		if (vrp->vrp_exit_reason == VM_EXIT_TERMINATED) {
			ret = (intptr_t)NULL;
			break;
		}

		if (vrp->vrp_exit_reason != VM_EXIT_NONE) {
			/*
			 * vmm(4) needs help handling an exit, handle in
			 * vcpu_exit.
			 */
			ret = vcpu_exit(vrp);
			if (ret)
				break;
		}
	}

	mutex_lock(&vm_mtx);
	vcpu_done[n] = 1;
	mutex_unlock(&vm_mtx);

	mutex_lock(&threadmutex);
	pthread_cond_signal(&threadcond);
	mutex_unlock(&threadmutex);

	return ((void *)ret);
}

int
vcpu_intr(uint32_t vm_id, uint32_t vcpu_id, uint8_t intr)
{
	struct vm_intr_params vip;

	memset(&vip, 0, sizeof(vip));

	vip.vip_vm_id = vm_id;
	vip.vip_vcpu_id = vcpu_id; /* XXX always 0? */
	vip.vip_intr = intr;

	if (ioctl(env->vmd_fd, VMM_IOC_INTR, &vip) == -1)
		return (errno);

	return (0);
}

/*
 * fd_hasdata
 *
 * Determines if data can be read from a file descriptor.
 *
 * Parameters:
 *  fd: the fd to check
 *
 * Return values:
 *  1 if data can be read from an fd, or 0 otherwise.
 */
int
fd_hasdata(int fd)
{
	struct pollfd pfd[1];
	int nready, hasdata = 0;

	pfd[0].fd = fd;
	pfd[0].events = POLLIN;
	nready = poll(pfd, 1, 0);
	if (nready == -1)
		log_warn("checking file descriptor for data failed");
	else if (nready == 1 && pfd[0].revents & POLLIN)
		hasdata = 1;
	return (hasdata);
}

/*
 * mutex_lock
 *
 * Wrapper function for pthread_mutex_lock that does error checking and that
 * exits on failure
 */
void
mutex_lock(pthread_mutex_t *m)
{
	int ret;

	ret = pthread_mutex_lock(m);
	if (ret) {
		errno = ret;
		fatal("could not acquire mutex");
	}
}

/*
 * mutex_unlock
 *
 * Wrapper function for pthread_mutex_unlock that does error checking and that
 * exits on failure
 */
void
mutex_unlock(pthread_mutex_t *m)
{
	int ret;

	ret = pthread_mutex_unlock(m);
	if (ret) {
		errno = ret;
		fatal("could not release mutex");
	}
}


void
vm_pipe_init(struct vm_dev_pipe *p, void (*cb)(int, short, void *))
{
	vm_pipe_init2(p, cb, NULL);
}

/*
 * vm_pipe_init2
 *
 * Initialize a vm_dev_pipe, setting up its file descriptors and its
 * event structure with the given callback and argument.
 *
 * Parameters:
 *  p: pointer to vm_dev_pipe struct to initizlize
 *  cb: callback to use for READ events on the read end of the pipe
 *  arg: pointer to pass to the callback on event trigger
 */
void
vm_pipe_init2(struct vm_dev_pipe *p, void (*cb)(int, short, void *), void *arg)
{
	int ret;
	int fds[2];

	memset(p, 0, sizeof(struct vm_dev_pipe));

	ret = pipe2(fds, O_CLOEXEC);
	if (ret)
		fatal("failed to create vm_dev_pipe pipe");

	p->read = fds[0];
	p->write = fds[1];

	event_set(&p->read_ev, p->read, EV_READ | EV_PERSIST, cb, arg);
}

/*
 * vm_pipe_send
 *
 * Send a message to an emulated device vie the provided vm_dev_pipe. This
 * relies on the fact sizeof(msg) < PIPE_BUF to ensure atomic writes.
 *
 * Parameters:
 *  p: pointer to initialized vm_dev_pipe
 *  msg: message to send in the channel
 */
void
vm_pipe_send(struct vm_dev_pipe *p, enum pipe_msg_type msg)
{
	size_t n;
	n = write(p->write, &msg, sizeof(msg));
	if (n != sizeof(msg))
		fatal("failed to write to device pipe");
}

/*
 * vm_pipe_recv
 *
 * Receive a message for an emulated device via the provided vm_dev_pipe.
 * Returns the message value, otherwise will exit on failure. This relies on
 * the fact sizeof(enum pipe_msg_type) < PIPE_BUF for atomic reads.
 *
 * Parameters:
 *  p: pointer to initialized vm_dev_pipe
 *
 * Return values:
 *  a value of enum pipe_msg_type or fatal exit on read(2) error
 */
enum pipe_msg_type
vm_pipe_recv(struct vm_dev_pipe *p)
{
	size_t n;
	enum pipe_msg_type msg;
	n = read(p->read, &msg, sizeof(msg));
	if (n != sizeof(msg))
		fatal("failed to read from device pipe");

	return msg;
}

/*
 * Re-map the guest address space using vmm(4)'s VMM_IOC_SHARE
 *
 * Returns 0 on success, non-zero in event of failure.
 */
int
remap_guest_mem(struct vmd_vm *vm, int vmm_fd)
{
	struct vm_create_params	*vcp;
	struct vm_mem_range	*vmr;
	struct vm_sharemem_params vsp;
	size_t			 i, j;
	void			*p = NULL;
	int			 ret;

	if (vm == NULL)
		return (1);

	vcp = &vm->vm_params.vmc_params;

	/*
	 * Initialize our VM shared memory request using our original
	 * creation parameters. We'll overwrite the va's after mmap(2).
	 */
	memset(&vsp, 0, sizeof(vsp));
	vsp.vsp_nmemranges = vcp->vcp_nmemranges;
	vsp.vsp_vm_id = vcp->vcp_id;
	memcpy(&vsp.vsp_memranges, &vcp->vcp_memranges,
	    sizeof(vsp.vsp_memranges));

	/*
	 * Use mmap(2) to identify virtual address space for our mappings.
	 */
	for (i = 0; i < VMM_MAX_MEM_RANGES; i++) {
		if (i < vsp.vsp_nmemranges) {
			vmr = &vsp.vsp_memranges[i];

			/* Ignore any MMIO ranges. */
			if (vmr->vmr_type == VM_MEM_MMIO) {
				vmr->vmr_va = 0;
				vcp->vcp_memranges[i].vmr_va = 0;
				continue;
			}

			/* Make initial mappings for the memrange. */
			p = mmap(NULL, vmr->vmr_size, PROT_READ, MAP_ANON, -1,
			    0);
			if (p == MAP_FAILED) {
				ret = errno;
				log_warn("%s: mmap", __func__);
				for (j = 0; j < i; j++) {
					vmr = &vcp->vcp_memranges[j];
					munmap((void *)vmr->vmr_va,
					    vmr->vmr_size);
				}
				return (ret);
			}
			vmr->vmr_va = (vaddr_t)p;
			vcp->vcp_memranges[i].vmr_va = vmr->vmr_va;
		}
	}

	/*
	 * munmap(2) now that we have va's and ranges that don't overlap. vmm
	 * will use the va's and sizes to recreate the mappings for us.
	 */
	for (i = 0; i < vsp.vsp_nmemranges; i++) {
		vmr = &vsp.vsp_memranges[i];
		if (vmr->vmr_type == VM_MEM_MMIO)
			continue;
		if (munmap((void*)vmr->vmr_va, vmr->vmr_size) == -1)
			fatal("%s: munmap", __func__);
	}

	/*
	 * Ask vmm to enter the shared mappings for us. They'll point
	 * to the same host physical memory, but will have a randomized
	 * virtual address for the calling process.
	 */
	if (ioctl(vmm_fd, VMM_IOC_SHAREMEM, &vsp) == -1)
		return (errno);

	return (0);
}

void
vcpu_halt(uint32_t vcpu_id)
{
	mutex_lock(&vm_mtx);
	vcpu_hlt[vcpu_id] = 1;
	mutex_unlock(&vm_mtx);
}

void
vcpu_unhalt(uint32_t vcpu_id)
	{
	mutex_lock(&vm_mtx);
	vcpu_hlt[vcpu_id] = 0;
	mutex_unlock(&vm_mtx);
}

void
vcpu_signal_run(uint32_t vcpu_id)
{
	int ret;

	mutex_lock(&vcpu_run_mtx[vcpu_id]);
	ret = pthread_cond_signal(&vcpu_run_cond[vcpu_id]);
	if (ret)
		fatalx("%s: can't signal (%d)", __func__, ret);
	mutex_unlock(&vcpu_run_mtx[vcpu_id]);
}
